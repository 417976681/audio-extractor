#pragma once

#include <string>
#include "common/common.h"
#include "extract/extractor.h"

// Output format selection.
enum class AudioFormat { MP3, WAV };

struct AudioOptions {
    AudioFormat format = AudioFormat::MP3;
    int sampleRate = 44100;   // output sample rate (Hz)
    int channels = 2;         // output channels
    int bitrateKbps = 192;    // MP3 bitrate (ignored for WAV)
    std::string outputPath;   // destination file
    ProgressCb progress;      // reported during decoding (0..1, or <0)
};

// Extract the audio track from a local MediaSource (produced by an extractor)
// and write it to AudioOptions::outputPath as MP3 or WAV.
// Returns 0 on success, or a negative FFmpeg error code / -1 on failure.
int extractAudio(const MediaSource& src, const AudioOptions& opt);
