// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
//
// PipeWire capture engine behind Recorder#start: one capture stream for the
// microphone (an Audio/Source) and one for system audio (the monitor of an
// Audio/Sink), both 48 kHz mono float. Real-time process callbacks only copy
// into lock-free rings; a worker thread cuts 10 ms chunks, runs
// webrtc-audio-processing (AEC/NS/AGC), mixes adaptively, reports volume
// and feeds the Plaud Ogg/Opus encoder. Events go to an EventPump.

#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio/adaptive_mix.h"
#include "audio/apm.h"
#include "audio/plaud_ogg_opus.h"
#include "engine/event_pump.h"
#include "pw/pw_connection.h"
#include "pw/pw_registry.h"
#include "util/spsc_ring.h"

struct pw_stream;

namespace plaud_rec {

struct DeviceSelection {
  enum class Kind { Default, Id, Uid };
  Kind kind = Kind::Default;
  uint32_t id = 0;
  std::string uid;

  bool operator==(const DeviceSelection& o) const { return kind == o.kind && id == o.id && uid == o.uid; }
  bool operator!=(const DeviceSelection& o) const { return !(*this == o); }
  std::string Describe() const;
};

// Resolves a selection against the graph: uid = node.name (a digits-only uid
// is also tried as a PipeWire object id), id = PipeWire object id. Returns
// nullptr for the default selection or when no matching source/sink exists.
const PwAudioNode* ResolveDeviceSelection(const RegistrySnapshot& snap, const DeviceSelection& sel, bool sink);

enum class ApmBackend { None, WebRtc };

struct EngineConfig {
  bool enableMicrophone = false;
  bool enableSystemAudio = false;
  double microphoneVolume = 1.0;
  double systemVolume = 1.0;
  bool encoding = false;
  uint32_t encodingBitRate = 32000;
  bool emitApmSignals = false;
  bool debugDumpRawAudio = false;
  ApmBackend backend = ApmBackend::WebRtc;
  DeviceSelection microphone;
  DeviceSelection speaker;
};

enum class Source { Microphone = 0, SystemAudio = 1 };

class CaptureEngine {
 public:
  static constexpr int kSampleRate = 48000;
  static constexpr int kChannels = 1;
  static constexpr size_t kChunk = kSampleRate / 100;  // 10 ms

  explicit CaptureEngine(std::shared_ptr<EventPump> pump);
  ~CaptureEngine();
  CaptureEngine(const CaptureEngine&) = delete;
  CaptureEngine& operator=(const CaptureEngine&) = delete;

  // On failure returns false; *tag is the "<mic|sys>:<category>" used by the
  // JS retry logic and *detail a human readable reason.
  bool Start(const EngineConfig& config, std::string* tag, std::string* detail);
  // Stops capture, processes buffered audio, flushes the encoder. Idempotent.
  void Stop();
  // Re-creates the PipeWire connection and streams; encoder and file continue.
  bool Restart(std::string* detail);

  void Pause(Source source);
  bool Begin(Source source, std::string* detail);
  void SetPreferredDevices(const DeviceSelection& microphone, const DeviceSelection& speaker);
  void SetApmEnabled(bool enabled);
  void SetEncodingPaused(bool paused);
  void FillSilence(int durationMs);

  bool running() const { return running_; }
  bool encoderActive() const { return encoderReady_; }

 private:
  struct Side;
  struct Dumps;

  bool SetupPipeWire(std::string* detail);
  void TeardownPipeWire();
  bool CreateStreamLocked(Side& side, const RegistrySnapshot& snap, std::string* detail);
  void DestroyStreamLocked(Side& side);
  void OnGraphChangedLocked(uint32_t mask);
  void EvaluateGraphLocked(bool force);
  void OnCoreError(uint32_t id, int res, const std::string& message);

  void StartWorker();
  void StopWorker();
  void WorkerMain();
  void ProcessAvailable(bool final);
  void ProcessChunk(bool useMic, bool useSys, bool mixed);
  void UpdateStall(Side& side, bool haveData, int64_t nowNs);
  bool Stalled(const Side& side, int64_t nowNs) const;
  void ReportOverflow(Side& side);
  // Any thread: asks the worker to drop what `side` captured so far.
  void RequestDiscard(Side& side);
  // Worker: applies pending drop requests; reports ring misuse.
  void ApplyConsumerRequests(Side& side);
  void Encode(const float* pcm, size_t n);
  void OpenDumps();
  void CloseDumps();
  void Wake();

  static void OnStreamProcess(void* data);
  static void OnStreamStateChanged(void* data, enum pw_stream_state oldState, enum pw_stream_state state,
                                   const char* error);

  std::shared_ptr<EventPump> pump_;
  EngineConfig config_;
  bool running_ = false;

  PwConnection conn_;
  std::unique_ptr<PwRegistry> registry_;
  std::unique_ptr<Side> mic_;
  std::unique_ptr<Side> sys_;
  std::string groupName_;
  std::atomic<bool> connectionLostReported_{false};
  bool graphReady_ = false;  // loop lock; set once the initial graph is known

  // Graph-derived state used by the worker.
  std::atomic<bool> sysOutputMuted_{false};
  std::atomic<bool> aecWanted_{false};
  std::string lastSinkName_;
  std::string lastMicName_;
  int lastAecState_ = -1;
  int lastMuteState_ = -1;

  // Worker.
  std::thread worker_;
  std::atomic<bool> workerStop_{false};
  int wakeFd_ = -1;
  std::atomic<bool> apmEnabled_{true};
  std::atomic<bool> encodingPaused_{false};
  std::unique_ptr<ApmProcessor> apm_;
  bool aecApplied_ = false;
  AdaptiveMixer mixer_;
  std::vector<float> micBuf_, sysBuf_, enhBuf_, outBuf_;
  std::unique_ptr<Dumps> dumps_;
  uint64_t stallLogCount_ = 0;

  // Encoder (worker thread and fillSilence on the JS thread).
  std::mutex encMu_;
  std::unique_ptr<PlaudOggOpusEncoder> encoder_;
  std::atomic<bool> encoderReady_{false};
  bool encodeErrorLogged_ = false;
};

}  // namespace plaud_rec
