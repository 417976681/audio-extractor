// Self-contained test for the YouTube extractor's C++ pipeline.
// It spins up a local mock of the innertube /player endpoint plus a media
// endpoint serving a decodable WAV, then runs audio_extractor.exe against a
// YouTube URL with YT_API_BASE pointed at the mock. This validates the real
// code path (videoId parse -> POST -> JSON parse -> audio selection ->
// download -> ffmpeg transcode) without needing live YouTube access.
//
// Usage: node tools/test_youtube_extractor.js
const http = require('http');
const fs = require('fs');
const path = require('path');
const { spawn } = require('child_process');

const PORT = 8731;
const BASE = `http://127.0.0.1:${PORT}/player`;

function makeWav() {
  const sr = 44100, ch = 2, dur = 1;
  const n = sr * dur;
  const dataSize = n * ch * 2;
  const buf = Buffer.alloc(44 + dataSize);
  buf.write('RIFF', 0); buf.writeUInt32LE(36 + dataSize, 4); buf.write('WAVE', 8);
  buf.write('fmt ', 12); buf.writeUInt32LE(16, 16); buf.writeUInt16LE(1, 20);
  buf.writeUInt16LE(ch, 22); buf.writeUInt32LE(sr, 24); buf.writeUInt32LE(sr * ch * 2, 28);
  buf.writeUInt16LE(ch * 2, 32); buf.writeUInt16LE(16, 34);
  buf.write('data', 36); buf.writeUInt32LE(dataSize, 40);
  return buf;
}
const WAV = makeWav();

const fixture = JSON.stringify({
  playabilityStatus: { status: 'OK' },
  videoDetails: { videoId: 'dQw4w9WgXcQ', title: 'Mock YouTube Song — Test' },
  streamingData: {
    adaptiveFormats: [
      { itag: 140, mimeType: 'audio/mp4; codecs="mp4a.40.2"', bitrate: 128000,
        url: `http://127.0.0.1:${PORT}/media` },
      { itag: 251, mimeType: 'audio/webm; codecs="opus"', bitrate: 160000,
        url: `http://127.0.0.1:${PORT}/media` },
      { itag: 22, mimeType: 'video/mp4; codecs="avc1.64001F, mp4a.40.2"', bitrate: 200000,
        url: `http://127.0.0.1:${PORT}/media` },
    ],
  },
});

const server = http.createServer((req, res) => {
  console.log('MOCK received', req.method, req.url);
  if (req.method === 'POST' && req.url.startsWith('/player')) {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(fixture);
    return;
  }
  if (req.method === 'GET' && req.url.startsWith('/media')) {
    res.writeHead(200, { 'Content-Type': 'audio/wav', 'Content-Length': WAV.length });
    res.end(WAV);
    return;
  }
  res.writeHead(404);
  res.end('not found');
});

server.listen(PORT, '127.0.0.1', () => {
  const out = path.resolve(__dirname, 'yt_extract_test.mp3');
  if (fs.existsSync(out)) fs.unlinkSync(out);
  const exe = path.resolve(__dirname, '..', 'build', 'audio_extractor.exe');
  const env = { ...process.env, YT_API_BASE: BASE };
  // Unset (not empty-string) proxy vars so libcurl does not fall back to the
  // Windows system proxy. An empty string is treated differently and would
  // still route localhost through the sandbox proxy.
  for (const k of ['HTTP_PROXY', 'HTTPS_PROXY', 'http_proxy', 'https_proxy',
                   'ALL_PROXY', 'all_proxy', 'NO_PROXY', 'no_proxy']) {
    delete env[k];
  }
  console.log('spawn env HTTP_PROXY =', JSON.stringify(env.HTTP_PROXY),
              'HTTPS_PROXY =', JSON.stringify(env.HTTPS_PROXY));
  // Use async spawn (NOT spawnSync): spawnSync blocks Node's event loop, which
  // would prevent this same-process mock server from ever accepting the exe's
  // connection.
  const child = spawn(
    exe,
    ['https://www.youtube.com/watch?v=dQw4w9WgXcQ', '-o', out],
    { env, timeout: 90000 }
  );
  let stdout = '', stderr = '';
  child.stdout.on('data', (d) => { stdout += d; });
  child.stderr.on('data', (d) => { stderr += d; });
  child.on('error', (e) => { stderr += '\n' + String(e); });
  child.on('close', (code) => {
    console.log('--- audio_extractor output ---');
    console.log(stdout + '\n' + stderr);
    let ok = code === 0 && fs.existsSync(out) && fs.statSync(out).size > 1000;
    console.log('exit code:', code);
    console.log('output exists:', fs.existsSync(out),
                'size:', fs.existsSync(out) ? fs.statSync(out).size : 0);
    console.log(ok ? 'YT_TEST: PASS' : 'YT_TEST: FAIL');
    server.close();
    process.exit(ok ? 0 : 1);
  });
});
