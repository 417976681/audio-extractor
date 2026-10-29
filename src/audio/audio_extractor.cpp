#define __STDC_CONSTANT_MACROS
#define __STDC_FORMAT_MACROS

#include "audio_extractor.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
}

#include "common/platform.h"

#include <memory>
#include <string>
#include <stdexcept>
#include <vector>
#include <cstring>

// --- RAII wrappers for FFmpeg objects -------------------------------------
static void fmtInDeleter(AVFormatContext* p)  { if (p) avformat_close_input(&p); }
static void fmtOutDeleter(AVFormatContext* p) { if (p) avformat_free_context(p); }
static void codecDeleter(AVCodecContext* p)   { if (p) avcodec_free_context(&p); }
static void frameDeleter(AVFrame* p)          { if (p) av_frame_free(&p); }
static void pktDeleter(AVPacket* p)           { if (p) av_packet_free(&p); }
static void swrDeleter(SwrContext* p)         { if (p) swr_free(&p); }

using FmtInPtr  = std::unique_ptr<AVFormatContext, decltype(&fmtInDeleter)>;
using FmtOutPtr = std::unique_ptr<AVFormatContext, decltype(&fmtOutDeleter)>;
using CodecPtr  = std::unique_ptr<AVCodecContext, decltype(&codecDeleter)>;
using FramePtr  = std::unique_ptr<AVFrame, decltype(&frameDeleter)>;
using PktPtr    = std::unique_ptr<AVPacket, decltype(&pktDeleter)>;
using SwrPtr    = std::unique_ptr<SwrContext, decltype(&swrDeleter)>;

static std::string ffErr(int err) {
    char buf[256] = {0};
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}

// --- Custom AVIO over a UTF-16-capable FILE* ---------------------------------
// Lets FFmpeg read/write the actual destination file (which may contain
// non-ASCII / Chinese characters in its path on Windows) regardless of how the
// bundled FFmpeg build handles filename encoding.
static int fileWriteCb(void* opaque, const uint8_t* buf, int buf_size) {
    auto* f = static_cast<FILE*>(opaque);
    size_t w = fwrite(buf, 1, (size_t)buf_size, f);
    if (w != (size_t)buf_size) return -1;
    return buf_size;
}

static int64_t fileSeekCb(void* opaque, int64_t offset, int whence) {
    auto* f = static_cast<FILE*>(opaque);
    if (whence & AVSEEK_SIZE) {
        // Query total file size without changing the current position.
        int64_t cur = plat::tell64(f);
        if (plat::seek64(f, 0, SEEK_END) != 0) return -1;
        int64_t end = plat::tell64(f);
        plat::seek64(f, cur, SEEK_SET);
        return end;
    }
    int w = (whence == SEEK_CUR) ? SEEK_CUR : (whence == SEEK_END) ? SEEK_END : SEEK_SET;
    if (plat::seek64(f, offset, w) != 0) return -1;
    return plat::tell64(f);
}

int extractAudio(const MediaSource& src, const AudioOptions& opt) {
    avformat_network_init();

    FmtInPtr ifmt(nullptr, fmtInDeleter);
    FmtOutPtr ofmt(nullptr, fmtOutDeleter);
    CodecPtr decCtx(nullptr, codecDeleter);
    CodecPtr encCtx(nullptr, codecDeleter);
    FramePtr frame(av_frame_alloc(), frameDeleter);
    PktPtr pkt(av_packet_alloc(), pktDeleter);
    PktPtr encPkt(av_packet_alloc(), pktDeleter);

    // Custom output I/O: we own the FILE* and AVIO buffer so the destination
    // path can contain non-ASCII (e.g. Chinese) characters on Windows.
    FILE* outF = nullptr;
    unsigned char* outIoBuf = nullptr;

    auto fail = [&](const std::string& msg) {
        LOG_ERROR(msg);
        return -1;
    };

    // ---- Open input -------------------------------------------------------
    AVFormatContext* pIfmt = nullptr;
    int ret = 0;
    if (src.mode == MediaSource::Mode::CONCAT) {
        AVDictionary* idict = nullptr;
        av_dict_set(&idict, "safe", "0", 0);
        ret = avformat_open_input(&pIfmt, src.concatListPath.c_str(),
                                  av_find_input_format("concat"), &idict);
        av_dict_free(&idict);
    } else {
        ret = avformat_open_input(&pIfmt, src.filePath.c_str(), nullptr, nullptr);
    }
    if (ret < 0) return fail("extractAudio: cannot open input (" + ffErr(ret) + ")");
    ifmt.reset(pIfmt);

    if ((ret = avformat_find_stream_info(ifmt.get(), nullptr)) < 0)
        return fail("extractAudio: find stream info (" + ffErr(ret) + ")");

    int aindex = av_find_best_stream(ifmt.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (aindex < 0) return fail("extractAudio: no audio stream found");

    AVCodecParameters* par = ifmt.get()->streams[aindex]->codecpar;
    const AVCodec* dec = avcodec_find_decoder(par->codec_id);
    if (!dec) return fail("extractAudio: audio decoder not found (codec id " +
                          std::to_string(par->codec_id) + ")");

    CodecPtr dctx(avcodec_alloc_context3(dec), codecDeleter);
    if ((ret = avcodec_parameters_to_context(dctx.get(), par)) < 0)
        return fail("extractAudio: parameters_to_context (" + ffErr(ret) + ")");
    if ((ret = avcodec_open2(dctx.get(), dec, nullptr)) < 0)
        return fail("extractAudio: open decoder (" + ffErr(ret) + ")");
    decCtx = std::move(dctx);

    // ---- Output format / muxer -------------------------------------------
    const AVOutputFormat* oformat = av_guess_format(nullptr, opt.outputPath.c_str(), nullptr);
    if (!oformat) return fail("extractAudio: cannot guess output format from '" +
                              opt.outputPath + "'");

    AVFormatContext* pOfmt = nullptr;
    if ((ret = avformat_alloc_output_context2(&pOfmt, oformat, nullptr,
                                              opt.outputPath.c_str())) < 0)
        return fail("extractAudio: alloc output context (" + ffErr(ret) + ")");
    ofmt.reset(pOfmt);

    // ---- Encoder ----------------------------------------------------------
    const AVCodec* enc = nullptr;
    if (opt.format == AudioFormat::WAV) {
        enc = avcodec_find_encoder(AV_CODEC_ID_PCM_S16LE);
    } else {
        enc = avcodec_find_encoder_by_name("libmp3lame");
        if (!enc) enc = avcodec_find_encoder(AV_CODEC_ID_MP3);
    }
    if (!enc) return fail("extractAudio: audio encoder not found (need libmp3lame "
                          "for MP3, or PCM for WAV)");

    AVStream* outStream = avformat_new_stream(ofmt.get(), nullptr);
    if (!outStream) return fail("extractAudio: cannot create output stream");

    CodecPtr ectx(avcodec_alloc_context3(enc), codecDeleter);
    ectx->sample_rate = opt.sampleRate;
    av_channel_layout_default(&ectx->ch_layout, opt.channels);
    if (opt.format == AudioFormat::WAV) {
        ectx->sample_fmt = AV_SAMPLE_FMT_S16;
        ectx->bit_rate = 0;
    } else {
        ectx->sample_fmt = AV_SAMPLE_FMT_FLTP;  // libmp3lame preferred format
        ectx->bit_rate = opt.bitrateKbps * 1000;
    }
    ectx->time_base = (AVRational){1, opt.sampleRate};
    if ((ret = avcodec_open2(ectx.get(), enc, nullptr)) < 0)
        return fail("extractAudio: open encoder (" + ffErr(ret) + ")");
    encCtx = std::move(ectx);

    if ((ret = avcodec_parameters_from_context(outStream->codecpar, encCtx.get())) < 0)
        return fail("extractAudio: parameters_from_context (" + ffErr(ret) + ")");
    outStream->time_base = encCtx.get()->time_base;

    if (!(ofmt.get()->oformat->flags & AVFMT_NOFILE)) {
        outF = plat::openFileWrite(opt.outputPath);
        if (!outF)
            return fail("extractAudio: cannot open output file '" + opt.outputPath + "'");
        const size_t AVIO_BUFSZ = 1 << 16;  // 64 KB
        outIoBuf = (unsigned char*)av_malloc(AVIO_BUFSZ);
        if (!outIoBuf) {
            fclose(outF); outF = nullptr;
            return fail("extractAudio: av_malloc for I/O buffer failed");
        }
        AVIOContext* pb = avio_alloc_context(outIoBuf, (int)AVIO_BUFSZ, 1, outF,
                                             nullptr, fileWriteCb, fileSeekCb);
        if (!pb) {
            av_free(outIoBuf); outIoBuf = nullptr;
            fclose(outF); outF = nullptr;
            return fail("extractAudio: avio_alloc_context failed");
        }
        ofmt.get()->pb = pb;
        ofmt.get()->flags |= AVFMT_FLAG_CUSTOM_IO;
    }

    if ((ret = avformat_write_header(ofmt.get(), nullptr)) < 0)
        return fail("extractAudio: write header (" + ffErr(ret) + ")");

    // ---- Resampler --------------------------------------------------------
    SwrPtr swr(swr_alloc(), swrDeleter);
    av_opt_set_chlayout(swr.get(), "in_chlayout", &decCtx.get()->ch_layout, 0);
    av_opt_set_int(swr.get(), "in_sample_rate", decCtx.get()->sample_rate, 0);
    av_opt_set_sample_fmt(swr.get(), "in_sample_fmt", decCtx.get()->sample_fmt, 0);
    av_opt_set_chlayout(swr.get(), "out_chlayout", &encCtx.get()->ch_layout, 0);
    av_opt_set_int(swr.get(), "out_sample_rate", encCtx.get()->sample_rate, 0);
    av_opt_set_sample_fmt(swr.get(), "out_sample_fmt", encCtx.get()->sample_fmt, 0);
    if ((ret = swr_init(swr.get())) < 0)
        return fail("extractAudio: swr_init (" + ffErr(ret) + ")");

    // ---- Resample + encode pipeline ---------------------------------------
    int64_t outSamplePos = 0;
    const int fs = encCtx.get()->frame_size;            // 0 for WAV, 1152 for MP3
    const bool planar = av_sample_fmt_is_planar(encCtx.get()->sample_fmt);
    const int ch = encCtx.get()->ch_layout.nb_channels;
    std::vector<std::vector<float>> pbuf;               // planar sample buffer (MP3)
    if (planar) pbuf.resize(ch);

    // Drain up to `fs` samples (or all remaining when final) into the encoder.
    auto drain = [&](bool final) -> int {
        while (true) {
            size_t avail = pbuf.empty() ? 0 : pbuf[0].size();
            if (avail == 0) return 0;
            if (fs > 0 && !final && (int)avail < fs) return 0;
            int n = (fs > 0) ? ((int)avail < fs ? (int)avail : fs) : (int)avail;
            FramePtr f(av_frame_alloc(), frameDeleter);
            f->format = encCtx.get()->sample_fmt;
            f->sample_rate = encCtx.get()->sample_rate;
            av_channel_layout_copy(&f->ch_layout, &encCtx.get()->ch_layout);
            f->nb_samples = n;
            if ((ret = av_frame_get_buffer(f.get(), 0)) < 0) return ret;
            for (int c = 0; c < ch; ++c) {
                memcpy(f->extended_data[c], pbuf[c].data(),
                       (size_t)n * av_get_bytes_per_sample(encCtx.get()->sample_fmt));
                pbuf[c].erase(pbuf[c].begin(), pbuf[c].begin() + n);
            }
            if ((ret = avcodec_send_frame(encCtx.get(), f.get())) < 0) return ret;
            while ((ret = avcodec_receive_packet(encCtx.get(), encPkt.get())) >= 0) {
                encPkt->stream_index = outStream->index;
                encPkt->pts = encPkt->dts = av_rescale_q(
                    outSamplePos, (AVRational){1, encCtx.get()->sample_rate},
                    outStream->time_base);
                if (encPkt->duration) outSamplePos += encPkt->duration;
                if ((ret = av_interleaved_write_frame(ofmt.get(), encPkt.get())) < 0)
                    return ret;
                av_packet_unref(encPkt.get());
            }
            if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF && ret < 0) return ret;
            if (fs == 0) return 0;  // WAV: whole buffer encoded as one frame
        }
    };

    // Resample one decoded frame, then push to encoder (buffered for MP3).
    auto processFrame = [&](AVFrame* decFrame) -> int {
        if (!decFrame) return 0;
        int outSamples = swr_get_out_samples(swr.get(), decFrame->nb_samples);
        FramePtr outFrame(av_frame_alloc(), frameDeleter);
        outFrame->format = encCtx.get()->sample_fmt;
        outFrame->sample_rate = encCtx.get()->sample_rate;
        av_channel_layout_copy(&outFrame->ch_layout, &encCtx.get()->ch_layout);
        outFrame->nb_samples = outSamples;
        if ((ret = av_frame_get_buffer(outFrame.get(), 0)) < 0) return ret;
        int got = swr_convert(swr.get(), outFrame->extended_data, outSamples,
                              (const uint8_t**)decFrame->extended_data,
                              decFrame->nb_samples);
        if (got < 0) return got;
        outFrame->nb_samples = got;

        if (planar) {
            for (int c = 0; c < ch; ++c)
                pbuf[c].insert(pbuf[c].end(),
                               (const float*)outFrame->extended_data[c],
                               (const float*)outFrame->extended_data[c] + got);
            if ((ret = drain(false)) < 0) return ret;
        } else {
            // Interleaved (e.g. WAV): feed the whole frame directly.
            if ((ret = avcodec_send_frame(encCtx.get(), outFrame.get())) < 0) return ret;
            while ((ret = avcodec_receive_packet(encCtx.get(), encPkt.get())) >= 0) {
                encPkt->stream_index = outStream->index;
                encPkt->pts = encPkt->dts = av_rescale_q(
                    outSamplePos, (AVRational){1, encCtx.get()->sample_rate},
                    outStream->time_base);
                if (encPkt->duration) outSamplePos += encPkt->duration;
                if ((ret = av_interleaved_write_frame(ofmt.get(), encPkt.get())) < 0)
                    return ret;
                av_packet_unref(encPkt.get());
            }
            if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF && ret < 0) return ret;
        }
        return 0;
    };

    double durSec = 0.0;
    if (ifmt.get()->duration != AV_NOPTS_VALUE)
        durSec = (double)ifmt.get()->duration / AV_TIME_BASE;
    if (durSec <= 0 && src.durationHint > 0) durSec = src.durationHint;
    AVRational aTb = ifmt.get()->streams[aindex]->time_base;

    auto reportProgress = [&](AVFrame* f) {
        if (!opt.progress || !f || f->pts == AV_NOPTS_VALUE || durSec <= 0) return;
        double t = (double)f->pts * av_q2d(aTb);
        opt.progress(t / durSec);
    };

    // ---- Decode loop ------------------------------------------------------
    while (av_read_frame(ifmt.get(), pkt.get()) >= 0) {
        if (pkt.get()->stream_index == aindex) {
            if ((ret = avcodec_send_packet(decCtx.get(), pkt.get())) < 0)
                return fail("extractAudio: send_packet (" + ffErr(ret) + ")");
            while ((ret = avcodec_receive_frame(decCtx.get(), frame.get())) >= 0) {
                if ((ret = processFrame(frame.get())) < 0)
                    return fail("extractAudio: processFrame (" + ffErr(ret) + ")");
                reportProgress(frame.get());
                av_frame_unref(frame.get());
            }
            if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
                return fail("extractAudio: receive_frame (" + ffErr(ret) + ")");
        }
        av_packet_unref(pkt.get());
    }

    // Flush decoder.
    avcodec_send_packet(decCtx.get(), nullptr);
    while ((ret = avcodec_receive_frame(decCtx.get(), frame.get())) >= 0) {
        if ((ret = processFrame(frame.get())) < 0)
            return fail("extractAudio: flush process (" + ffErr(ret) + ")");
        reportProgress(frame.get());
        av_frame_unref(frame.get());
    }

    // Flush any buffered samples (MP3) then signal end-of-stream to encoder.
    if (planar) {
        if ((ret = drain(true)) < 0)
            return fail("extractAudio: drain final (" + ffErr(ret) + ")");
    }
    // Flush encoder.
    avcodec_send_frame(encCtx.get(), nullptr);
    while ((ret = avcodec_receive_packet(encCtx.get(), encPkt.get())) >= 0) {
        encPkt->stream_index = outStream->index;
        encPkt->pts = encPkt->dts =
            av_rescale_q(outSamplePos, (AVRational){1, encCtx.get()->sample_rate},
                         outStream->time_base);
        if (encPkt->duration) outSamplePos += encPkt->duration;
        if ((ret = av_interleaved_write_frame(ofmt.get(), encPkt.get())) < 0)
            return fail("extractAudio: final write (" + ffErr(ret) + ")");
        av_packet_unref(encPkt.get());
    }

    // ---- Finalize ---------------------------------------------------------
    if ((ret = av_write_trailer(ofmt.get())) < 0)
        return fail("extractAudio: write trailer (" + ffErr(ret) + ")");
    if (ofmt.get()->pb) {
        avio_flush(ofmt.get()->pb);
        avio_context_free(&ofmt.get()->pb);
    }
    if (outIoBuf) { av_free(outIoBuf); outIoBuf = nullptr; }
    if (outF) { fclose(outF); outF = nullptr; }

    if (opt.progress) opt.progress(1.0);
    LOG_INFO("extractAudio: wrote " + opt.outputPath);
    return 0;
}
