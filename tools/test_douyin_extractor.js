// Self-contained test for the Douyin extractor's C++ pipeline.
// Spins up a local mock of the aweme/iteminfo endpoint plus a media endpoint,
// then runs audio_extractor.exe against a Douyin URL with DOUYIN_API_BASE
// pointed at the mock. Validates: aweme id parse -> POST iteminfo -> JSON
// parse -> audio URL selection (music.play_url) -> download -> ffmpeg transcode.
//
// Usage: node tools/test_douyin_extractor.js
const http = require('http');
const fs = require('fs');
const path = require('path');
const { spawn } = require('child_process');

const PORT = 8732;
const BASE = `http://127.0.0.1:${PORT}/iteminfo`;

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
const AID = '7300000000000000000';

const fixture = JSON.stringify({
  status_code: 0,
  aweme_list: [
    {
      aweme_id: AID,
      desc: 'Mock Douyin 音乐 Test',
      music: { play_url: { uri: 'mock', url_list: [`http://127.0.0.1:${PORT}/media`] } },
      video: { play_addr: { uri: 'mockv', url_list: [`http://127.0.0.1:${PORT}/media`] } },
    },
  ],
});

const server = http.createServer((req, res) => {
  console.log('MOCK received', req.method, req.url);
  if (req.method === 'POST' && req.url.startsWith('/iteminfo')) {
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
  const out = path.resolve(__dirname, 'douyin_extract_test.mp3');
  if (fs.existsSync(out)) fs.unlinkSync(out);
  const exe = path.resolve(__dirname, '..', 'build', 'audio_extractor.exe');
  const env = { ...process.env, DOUYIN_API_BASE: BASE };
  for (const k of ['HTTP_PROXY', 'HTTPS_PROXY', 'http_proxy', 'https_proxy',
                   'ALL_PROXY', 'all_proxy', 'NO_PROXY', 'no_proxy']) {
    delete env[k];
  }
  const child = spawn(
    exe,
    ['https://www.douyin.com/video/' + AID, '-o', out],
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
    console.log(ok ? 'DOUYIN_TEST: PASS' : 'DOUYIN_TEST: FAIL');
    server.close();
    process.exit(ok ? 0 : 1);
  });
});
