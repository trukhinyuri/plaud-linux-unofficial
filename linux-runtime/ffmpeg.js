'use strict';

// The official upload service spawns "<resources>/ffmpeg" to encode raw PCM
// (s16le, 48 kHz, mono) into Ogg/Opus at 16 kHz and to read media durations.
// This build ships a wrapper that executes the system ffmpeg (security
// updates come from the distribution; no codec redistribution). This module
// runs the app's exact encoding pipeline once at start-up and tells the user
// when it fails, instead of letting uploads fail silently later.

const path = require('node:path');
const { spawn } = require('node:child_process');
const { noticeOnce, clearNotice } = require('./notify');
const { createLogger } = require('./log');

const log = createLogger('ffmpeg');

// Arguments used by the official uploadService (uploadService-*.js).
const PIPELINE = ['-hide_banner', '-loglevel', 'error', '-f', 's16le', '-ar', '48000', '-ac', '1', '-i', 'pipe:0', '-f', 'ogg', '-c:a', 'libopus', '-ar', '16000', '-ac', '1', '-vbr', 'on', '-compression_level', '10', 'pipe:1'];

function probe(ffmpegPath = path.join(process.resourcesPath, 'ffmpeg')) {
  return new Promise((resolve) => {
    let stderr = '';
    let bytes = 0;
    let child;
    try {
      child = spawn(ffmpegPath, PIPELINE, { stdio: ['pipe', 'pipe', 'pipe'] });
    } catch (error) {
      resolve({ ok: false, reason: error.message });
      return;
    }
    const timer = setTimeout(() => child.kill('SIGKILL'), 15_000);
    child.stdout.on('data', (d) => (bytes += d.length));
    child.stderr.on('data', (d) => (stderr += d));
    child.on('error', (error) => {
      clearTimeout(timer);
      resolve({ ok: false, reason: error.message });
    });
    child.on('close', (code, signal) => {
      clearTimeout(timer);
      const header = bytes > 0;
      resolve(code === 0 && header ? { ok: true } : { ok: false, reason: (stderr.trim() || `exit ${code ?? signal}`).slice(-500) });
    });
    child.stdin.on('error', () => {});
    // 100 ms of silence.
    child.stdin.end(Buffer.alloc(9600));
  });
}

function checkInBackground() {
  probe().then((result) => {
    if (result.ok) {
      log.info('ffmpeg Ogg/Opus pipeline works');
      clearNotice('ffmpeg-missing');
      return;
    }
    log.error('ffmpeg cannot encode Ogg/Opus; recordings cannot be uploaded', { reason: result.reason });
    noticeOnce('ffmpeg-missing', {
      title: 'Plaud: ffmpeg with Opus support is required',
      body: 'Recordings cannot be uploaded until ffmpeg with the libopus encoder is installed (or PLAUD_FFMPEG points to one).',
    });
  });
}

module.exports = { probe, checkInBackground, PIPELINE };
