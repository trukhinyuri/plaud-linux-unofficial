// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)

#include "engine/capture_engine.h"

#include <poll.h>
#include <pthread.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/result.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <random>

namespace plaud_rec {

namespace {

constexpr size_t kRingCapacity = 1u << 17;          // 131072 samples ≈ 2.7 s at 48 kHz
constexpr int64_t kStallNs = 60'000'000;            // official worker stall threshold (60 ms)
constexpr size_t kMaxSkewSamples = 48000 / 10;      // tolerated backlog difference between sources
constexpr int kApmStreamDelayMs = 15;
constexpr int kConnectTimeoutMs = 2000;
constexpr int kSyncTimeoutMs = 1500;
constexpr int kRestartBudgetMs = 3000;

int64_t MonotonicNs() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

float Rms(const float* x, size_t n) {
  if (n == 0) return 0.f;
  double acc = 0;
  for (size_t i = 0; i < n; ++i) acc += static_cast<double>(x[i]) * x[i];
  return static_cast<float>(std::sqrt(acc / static_cast<double>(n)));
}

std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

bool Contains(const std::string& haystack, const char* needle) { return haystack.find(needle) != std::string::npos; }

bool StartsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

const char* SideLabel(Source s) { return s == Source::Microphone ? "Microphone" : "System audio"; }

const char* StateName(pw_stream_state s) { return pw_stream_state_as_string(s); }

struct AecDecision {
  bool enable;
  std::string reason;
};

// Official rule: AEC only when the default output is the built-in speaker
// ("Default output transport is BuiltIn (enabling AEC if mixing)", otherwise
// "Non-BuiltIn (disabling AEC)"). On Linux the equivalent signal is whether
// the captured sink can leak into the microphone acoustically: headphone-type
// form factors, headphone/headset ports and Bluetooth outputs cannot. The
// form factor and bus are read from the node (virtual and filter sinks may
// carry them) and from its device.
bool IsHeadphoneFormFactor(const std::string& formFactor) {
  const std::string ff = Lower(formFactor);
  return ff == "headphone" || ff == "headset" || ff == "handset" || ff == "hands-free";
}

AecDecision DecideAec(const RegistrySnapshot& snap, const PwAudioNode* sink) {
  if (sink == nullptr) return {true, "no output device information"};
  if (StartsWith(sink->name, "bluez_") || sink->bus == "bluetooth") return {false, "Bluetooth output"};
  if (IsHeadphoneFormFactor(sink->formFactor)) {
    return {false, "headphone-type output (form factor " + Lower(sink->formFactor) + ")"};
  }
  if (const PwAudioDevice* dev = snap.DeviceOf(*sink)) {
    if (dev->bus == "bluetooth" || dev->api == "bluez5") return {false, "Bluetooth output"};
    if (IsHeadphoneFormFactor(dev->formFactor)) {
      return {false, "headphone-type output (form factor " + Lower(dev->formFactor) + ")"};
    }
    const PwRoute* route = nullptr;
    auto it = dev->outputRoutes.find(sink->profileDevice);
    if (it != dev->outputRoutes.end()) {
      route = &it->second;
    } else if (dev->outputRoutes.size() == 1) {
      route = &dev->outputRoutes.begin()->second;
    }
    if (route != nullptr) {
      const std::string pt = Lower(route->portType);
      const std::string name = Lower(route->name);
      if (pt == "headphones" || pt == "headset" || pt == "handset" || pt == "earpiece" || Contains(name, "headphone") ||
          Contains(name, "headset")) {
        return {false, "headphone port active (" + route->name + ")"};
      }
    }
  }
  return {true, "loudspeaker-type output"};
}

const pw_stream_events& StreamEvents(void (*process)(void*),
                                     void (*state)(void*, pw_stream_state, pw_stream_state, const char*)) {
  static pw_stream_events ev = [&] {
    pw_stream_events e{};
    e.version = PW_VERSION_STREAM_EVENTS;
    e.process = process;
    e.state_changed = state;
    return e;
  }();
  return ev;
}

std::string DumpPath(const char* kind, const std::string& stamp) {
  const char* tmp = std::getenv("TMPDIR");
  std::string dir = (tmp != nullptr && *tmp != '\0') ? tmp : "/tmp";
  return dir + "/linux_recorder_" + kind + "_" + stamp + ".raw";
}

std::atomic<uint32_t> gGroupCounter{0};

}  // namespace

std::string DeviceSelection::Describe() const {
  switch (kind) {
    case Kind::Default:
      return "system default";
    case Kind::Id:
      return "id " + std::to_string(id);
    case Kind::Uid:
      return "\"" + uid + "\"";
  }
  return "system default";
}

const PwAudioNode* ResolveDeviceSelection(const RegistrySnapshot& snap, const DeviceSelection& sel, bool sink) {
  const PwAudioNode* n = nullptr;
  if (sel.kind == DeviceSelection::Kind::Uid) {
    n = snap.ByName(sel.uid, sink);
    if (n == nullptr && !sel.uid.empty() &&
        std::all_of(sel.uid.begin(), sel.uid.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
      n = snap.ById(static_cast<uint32_t>(std::strtoul(sel.uid.c_str(), nullptr, 10)));
    }
  } else if (sel.kind == DeviceSelection::Kind::Id) {
    n = snap.ById(sel.id);
  }
  if (n == nullptr || !(sink ? n->isSink : n->isSource)) return nullptr;
  return n;
}

namespace {

bool AllDigits(const std::string& s) {
  return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

// The node.name a stream is pinned to for `sel` (empty = follow the default).
// A uid that names no current node is still pinned: WirePlumber links the
// stream to the default meanwhile and moves it to the device once it
// appears (e.g. after a PipeWire restart or a re-plug). Numeric ids are not
// stable across restarts, and WirePlumber reads an all-digit target as an
// object serial, so those only pin while resolved.
std::string PinTarget(const RegistrySnapshot& snap, const DeviceSelection& sel, bool sink,
                      const PwAudioNode** resolved) {
  const PwAudioNode* n = ResolveDeviceSelection(snap, sel, sink);
  if (resolved != nullptr) *resolved = n;
  if (n != nullptr) return n->name;
  if (sel.kind == DeviceSelection::Kind::Uid && !AllDigits(sel.uid)) return sel.uid;
  return {};
}

}  // namespace

struct CaptureEngine::Side {
  CaptureEngine* engine = nullptr;
  Source source = Source::Microphone;
  pw_stream* stream = nullptr;
  spa_hook listener{};
  bool listenerAdded = false;
  SpscFloatRing ring{kRingCapacity};
  std::atomic<int64_t> lastDataNs{0};
  std::atomic<uint32_t> lastWriteSamples{0};
  std::atomic<uint64_t> overflowSamples{0};
  std::atomic<bool> inPipeline{false};
  std::atomic<bool> enabled{false};
  std::atomic<uint32_t> nodeId{SPA_ID_INVALID};
  // Drop requests from other threads (begin(), restart()). The worker is the
  // ring's only consumer and applies them: discardMark is a ring
  // WriteIndex() snapshot, published by bumping discardSeq.
  std::atomic<size_t> discardMark{0};
  std::atomic<uint64_t> discardSeq{0};
  DeviceSelection selection;
  std::string targetName;  // pinned node.name; empty = follow the default device
  // Worker-only bookkeeping.
  bool stallReported = false;
  uint64_t reportedOverflow = 0;
  uint64_t discardApplied = 0;
  uint64_t reportedConsumerViolations = 0;
};

struct CaptureEngine::Dumps {
  FILE* sys = nullptr;
  FILE* mic = nullptr;
  FILE* mix = nullptr;
  FILE* micRaw = nullptr;
  FILE* micPost = nullptr;
  FILE* sysRaw = nullptr;
  FILE* outMix = nullptr;
  ~Dumps() {
    for (FILE* f : {sys, mic, mix, micRaw, micPost, sysRaw, outMix})
      if (f != nullptr) std::fclose(f);
  }
  static void Write(FILE* f, const float* x, size_t n) {
    if (f != nullptr) std::fwrite(x, sizeof(float), n, f);
  }
};

CaptureEngine::CaptureEngine(std::shared_ptr<EventPump> pump)
    : pump_(std::move(pump)), conn_("plaud-recorder-pw") {
  wakeFd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
}

CaptureEngine::~CaptureEngine() {
  Stop();
  if (wakeFd_ >= 0) close(wakeFd_);
}

void CaptureEngine::Wake() {
  if (wakeFd_ < 0) return;
  const uint64_t one = 1;
  const ssize_t rc = write(wakeFd_, &one, sizeof(one));
  (void)rc;
}

bool CaptureEngine::Start(const EngineConfig& config, std::string* tag, std::string* detail) {
  if (running_) {
    *tag = config.enableMicrophone ? "mic:init_failed" : "sys:init_failed";
    *detail = "Recorder is already running.";
    return false;
  }
  config_ = config;
  connectionLostReported_ = false;
  pump_->Log(LogLevel::Info, "Start()");
  pump_->Log(LogLevel::Info, std::string("Audio processing backend: ") +
                                 (config.backend == ApmBackend::WebRtc ? "webrtc" : "none"));
  if (config.emitApmSignals) {
    pump_->Log(LogLevel::Info, "emitApmSignals enabled: JS will receive per-chunk APM (lpb/mic/enh) taps.");
  }
  if (!config.enableMicrophone && config.microphone.kind != DeviceSelection::Kind::Default) {
    pump_->Log(LogLevel::Warning,
               "microphoneDeviceId provided but enableMicrophone is false. Ignoring device selection.");
  }

  mic_ = std::make_unique<Side>();
  mic_->engine = this;
  mic_->source = Source::Microphone;
  mic_->selection = config.microphone;
  mic_->inPipeline = config.enableMicrophone;
  mic_->enabled = config.enableMicrophone;
  sys_ = std::make_unique<Side>();
  sys_->engine = this;
  sys_->source = Source::SystemAudio;
  sys_->selection = config.speaker;
  sys_->inPipeline = config.enableSystemAudio;
  sys_->enabled = config.enableSystemAudio;
  groupName_ = "plaud-recorder-" + std::to_string(getpid()) + "-" + std::to_string(gGroupCounter.fetch_add(1));
  lastSinkName_.clear();
  lastMicName_.clear();
  lastAecState_ = -1;
  lastMuteState_ = -1;
  sysOutputMuted_ = false;
  aecWanted_ = false;
  aecApplied_ = false;
  apmEnabled_ = true;
  encodingPaused_ = false;
  stallLogCount_ = 0;
  encodeErrorLogged_ = false;
  mixer_.Reset();
  micBuf_.assign(kChunk, 0.f);
  sysBuf_.assign(kChunk, 0.f);
  enhBuf_.assign(kChunk, 0.f);
  outBuf_.assign(kChunk, 0.f);

  if (config.backend == ApmBackend::WebRtc) {
    apm_ = std::make_unique<ApmProcessor>();
    std::string err;
    if (apm_->Init(kSampleRate, false, &err)) {
      pump_->Log(LogLevel::Info, "APM initialized. Input: 48000 Hz, Output: 48000 Hz, Channels: 1");
    } else {
      pump_->Log(LogLevel::Error, err + " Continuing without audio processing.");
      apm_.reset();
    }
  } else {
    pump_->Log(LogLevel::Info, "Audio processing disabled (backend=none).");
  }

  if (!SetupPipeWire(detail)) {
    TeardownPipeWire();
    apm_.reset();
    *tag = config.enableMicrophone ? "mic:init_failed" : "sys:init_failed";
    pump_->Log(LogLevel::Error, "Failed to start recorder: " + *detail);
    return false;
  }

  if (config.encoding) {
    auto encoder = std::make_unique<PlaudOggOpusEncoder>();
    std::random_device rd;
    const uint32_t serial = static_cast<uint32_t>(rd()) & 0x7ffffffeu;
    std::string err;
    auto pump = pump_;
    const bool ok = encoder->Init(static_cast<int>(config.encodingBitRate), serial,
                                  [pump](const uint8_t* data, size_t size, double durationMs) {
                                    RecorderEvent ev;
                                    ev.kind = RecorderEvent::Kind::Encoded;
                                    ev.bytes.assign(data, data + size);
                                    ev.duration = durationMs;
                                    pump->Push(std::move(ev));
                                  },
                                  &err);
    if (ok) {
      std::lock_guard<std::mutex> lock(encMu_);
      encoder_ = std::move(encoder);
    } else {
      pump_->Log(LogLevel::Warning, "Warning: Failed to create encoder, falling back to PCM (" + err + ")");
    }
  }
  encoderReady_ = encoder_ != nullptr;

  OpenDumps();
  StartWorker();
  running_ = true;
  pump_->Log(LogLevel::Info, "Recorder started successfully.");
  return true;
}

void CaptureEngine::Stop() {
  if (!running_) return;
  pump_->Log(LogLevel::Info, "Stopping recorder...");
  TeardownPipeWire();
  StopWorker();
  {
    std::lock_guard<std::mutex> lock(encMu_);
    encoder_.reset();
  }
  encoderReady_ = false;
  CloseDumps();
  apm_.reset();
  mic_.reset();
  sys_.reset();
  running_ = false;
  pump_->Log(LogLevel::Info, "Recorder fully stopped and resources released.");
}

bool CaptureEngine::Restart(std::string* detail) {
  if (!running_) {
    *detail = "recorder is not running";
    return false;
  }
  pump_->Log(LogLevel::Info, "LinuxRecorder::Restart - restarting recorder.");
  TeardownPipeWire();
  // No producer exists any more; the worker, the rings' only consumer,
  // drops what the old streams left behind.
  for (Side* s : {mic_.get(), sys_.get()}) {
    s->lastDataNs = MonotonicNs();
    RequestDiscard(*s);
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kRestartBudgetMs);
  std::string err;
  for (int attempt = 1;; ++attempt) {
    err.clear();
    if (SetupPipeWire(&err)) {
      connectionLostReported_ = false;
      pump_->Log(LogLevel::Info, "LinuxRecorder::Restart - recorder restarted successfully.");
      return true;
    }
    TeardownPipeWire();
    if (std::chrono::steady_clock::now() >= deadline) break;
    pump_->Log(LogLevel::Warning,
               "LinuxRecorder::Restart - attempt " + std::to_string(attempt) + " failed: " + err + "; retrying.");
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
  *detail = err;
  pump_->Log(LogLevel::Error, "LinuxRecorder::Restart - restart failed: " + err);
  return false;
}

bool CaptureEngine::SetupPipeWire(std::string* detail) {
  conn_.SetErrorHandler(
      [this](uint32_t id, int res, const std::string& message) { OnCoreError(id, res, message); });
  std::string err;
  if (!conn_.Connect(kConnectTimeoutMs, &err)) {
    *detail = "PipeWire: " + err;
    return false;
  }
  pump_->Log(LogLevel::Info, "Connected to PipeWire " + conn_.serverVersion() + " (" + conn_.serverName() + ")");
  PwLock lock(conn_);
  registry_ = std::make_unique<PwRegistry>(&conn_);
  if (!registry_->Start([this](uint32_t mask) { OnGraphChangedLocked(mask); })) {
    *detail = "PipeWire: cannot get the registry";
    return false;
  }
  // First round trip delivers the globals, the second the bound objects' info.
  if (!conn_.SyncLocked(kSyncTimeoutMs) || !conn_.SyncLocked(kSyncTimeoutMs)) {
    *detail = "PipeWire: daemon did not answer the registry round trip";
    return false;
  }
  const RegistrySnapshot snap = registry_->Snapshot();
  if (mic_->inPipeline) {
    if (snap.Sources().empty()) {
      pump_->Log(LogLevel::Warning,
                 "No microphone is available yet; recording continues and picks one up when it appears.");
    }
    if (!CreateStreamLocked(*mic_, snap, detail)) return false;
  }
  if (sys_->inPipeline) {
    if (snap.Sinks().empty()) {
      pump_->Log(LogLevel::Warning,
                 "No audio output is available yet; system audio is captured once an output appears.");
    }
    if (!CreateStreamLocked(*sys_, snap, detail)) return false;
  }
  if (!conn_.SyncLocked(kSyncTimeoutMs)) {
    *detail = "PipeWire: daemon did not acknowledge the capture streams";
    return false;
  }
  graphReady_ = true;
  EvaluateGraphLocked(true);
  return true;
}

void CaptureEngine::TeardownPipeWire() {
  if (conn_.threadLoop() != nullptr) {
    PwLock lock(conn_);
    graphReady_ = false;
    if (mic_) DestroyStreamLocked(*mic_);
    if (sys_) DestroyStreamLocked(*sys_);
    if (registry_) registry_->Stop();
  }
  registry_.reset();
  conn_.Disconnect();
}

bool CaptureEngine::CreateStreamLocked(Side& side, const RegistrySnapshot& snap, std::string* detail) {
  const bool isSys = side.source == Source::SystemAudio;
  const PwAudioNode* n = nullptr;
  side.targetName = PinTarget(snap, side.selection, isSys, &n);
  if (n != nullptr) {
    pump_->Log(LogLevel::Info, std::string("Using user-selected ") + (isSys ? "speaker" : "microphone") +
                                   " device: " + n->DisplayName() + " (" + n->name + ")");
  } else if (side.selection.kind != DeviceSelection::Kind::Default) {
    pump_->Log(LogLevel::Warning, std::string(isSys ? "Speaker" : "Microphone") + " device " +
                                      side.selection.Describe() +
                                      " is not available. Falling back to system default.");
  }

  pw_properties* props = pw_properties_new(
      PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_APP_NAME, "Plaud", PW_KEY_NODE_NAME,
      isSys ? "plaud-recorder-system-audio" : "plaud-recorder-microphone", PW_KEY_NODE_DESCRIPTION,
      isSys ? "Plaud system audio capture" : "Plaud microphone capture", PW_KEY_MEDIA_NAME,
      isSys ? "Plaud system audio" : "Plaud microphone", PW_KEY_NODE_GROUP, groupName_.c_str(), nullptr);
  if (props == nullptr) {
    *detail = std::string("pw_properties_new failed: ") + std::strerror(errno);
    return false;
  }
  if (isSys) pw_properties_set(props, PW_KEY_STREAM_CAPTURE_SINK, "true");
  if (!side.targetName.empty()) {
    pw_properties_set(props, "target.object", side.targetName.c_str());
    // Older session managers only know the numeric node.target.
    if (n != nullptr) pw_properties_setf(props, "node.target", "%u", n->id);
    // An explicit selection stays on its device like the official recorder:
    // without this, WirePlumber turns a stream linked to the current default
    // into a default-follower (PulseAudio semantics) via stream metadata.
    // Fallback to the default while the device is missing and the move back
    // when it (re)appears still happen, since they follow target.object.
    pw_properties_set(props, "node.dont-move", "true");
  }
  side.stream = pw_stream_new(conn_.core(), isSys ? "plaud-system-audio" : "plaud-microphone", props);
  if (side.stream == nullptr) {
    *detail = std::string("pw_stream_new failed: ") + std::strerror(errno);
    return false;
  }
  pw_stream_add_listener(side.stream, &side.listener,
                         &StreamEvents(&CaptureEngine::OnStreamProcess, &CaptureEngine::OnStreamStateChanged), &side);
  side.listenerAdded = true;

  uint8_t buffer[1024];
  spa_pod_builder b{};
  spa_pod_builder_init(&b, buffer, sizeof(buffer));
  spa_audio_info_raw info{};
  info.format = SPA_AUDIO_FORMAT_F32;
  info.rate = kSampleRate;
  info.channels = kChannels;
  info.position[0] = SPA_AUDIO_CHANNEL_MONO;
  const spa_pod* params[1] = {spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info)};

  side.lastDataNs = MonotonicNs();
  side.lastWriteSamples = 0;
  const int rc = pw_stream_connect(
      side.stream, PW_DIRECTION_INPUT, PW_ID_ANY,
      static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS),
      params, 1);
  if (rc < 0) {
    *detail = std::string("pw_stream_connect failed: ") + spa_strerror(rc);
    DestroyStreamLocked(side);
    return false;
  }
  return true;
}

void CaptureEngine::DestroyStreamLocked(Side& side) {
  if (side.stream == nullptr) return;
  if (side.listenerAdded) {
    spa_hook_remove(&side.listener);
    side.listenerAdded = false;
  }
  pw_stream_destroy(side.stream);
  side.stream = nullptr;
  side.nodeId = SPA_ID_INVALID;
}

void CaptureEngine::OnStreamProcess(void* data) {
  auto* side = static_cast<Side*>(data);
  pw_buffer* pwb = pw_stream_dequeue_buffer(side->stream);
  if (pwb == nullptr) return;
  spa_buffer* buf = pwb->buffer;
  if (buf->n_datas > 0 && buf->datas[0].chunk != nullptr) {
    const spa_data& d = buf->datas[0];
    const uint32_t offset = SPA_MIN(d.chunk->offset, d.maxsize);
    const uint32_t size = SPA_MIN(d.chunk->size, d.maxsize - offset);
    const uint32_t n = size / sizeof(float);
    bool empty = d.data == nullptr;
#ifdef SPA_CHUNK_FLAG_EMPTY
    empty = empty || (d.chunk->flags & SPA_CHUNK_FLAG_EMPTY) != 0;
#endif
    const size_t written =
        empty ? side->ring.WriteZeros(n) : side->ring.Write(SPA_PTROFF(d.data, offset, const float), n);
    if (written < n) side->overflowSamples.fetch_add(n - written, std::memory_order_relaxed);
    if (n > 0) {
      side->lastWriteSamples.store(n, std::memory_order_relaxed);
      side->lastDataNs.store(MonotonicNs(), std::memory_order_release);
    }
  }
  pw_stream_queue_buffer(side->stream, pwb);
  side->engine->Wake();
}

void CaptureEngine::OnStreamStateChanged(void* data, pw_stream_state oldState, pw_stream_state state,
                                         const char* error) {
  auto* side = static_cast<Side*>(data);
  CaptureEngine* self = side->engine;
  if (state == PW_STREAM_STATE_PAUSED || state == PW_STREAM_STATE_STREAMING) {
    side->nodeId = pw_stream_get_node_id(side->stream);
  }
  self->pump_->Log(LogLevel::Debug, std::string(SideLabel(side->source)) + " stream: " + StateName(oldState) +
                                        " -> " + StateName(state));
  if (state == PW_STREAM_STATE_ERROR) {
    const std::string why = error != nullptr ? error : "unknown error";
    self->pump_->Log(LogLevel::Error, std::string(SideLabel(side->source)) + " stream error: " + why);
    if (!self->connectionLostReported_.exchange(true)) {
      const char* tag = side->source == Source::Microphone ? "[mic:env_device_invalidated]" : "[sys:env_device_invalidated]";
      self->pump_->Fatal("device_invalidated", std::string(tag) + " " + SideLabel(side->source) +
                                                   " capture stream failed: " + why);
    }
  }
  self->EvaluateGraphLocked(false);
}

void CaptureEngine::OnCoreError(uint32_t id, int res, const std::string& message) {
  if (id == PW_ID_CORE && res == -EPIPE) {
    if (!connectionLostReported_.exchange(true)) {
      pump_->Log(LogLevel::Error, "PipeWire connection lost: " + message);
      pump_->Fatal("device_invalidated", "[sys:env_device_invalidated] PipeWire connection lost: " + message);
    }
    return;
  }
  pump_->Log(LogLevel::Warning, "PipeWire error (object " + std::to_string(id) + ", " + spa_strerror(res) +
                                    "): " + message);
}

void CaptureEngine::OnGraphChangedLocked(uint32_t mask) {
  (void)mask;
  EvaluateGraphLocked(false);
}

void CaptureEngine::EvaluateGraphLocked(bool force) {
  if (!graphReady_ || !registry_ || !mic_ || !sys_) return;
  const RegistrySnapshot snap = registry_->Snapshot();

  if (mic_->inPipeline) {
    const uint32_t streamNode = mic_->nodeId.load();
    const PwAudioNode* src = nullptr;
    if (streamNode != SPA_ID_INVALID) {
      if (const uint32_t linked = snap.LinkedOutputNode(streamNode)) src = snap.ById(linked);
    }
    const std::string name = src != nullptr ? src->name : std::string();
    if (src != nullptr && (force || name != lastMicName_)) {
      pump_->Log(LogLevel::Info, "Microphone source: " + src->DisplayName() + " (" + src->name + ")");
      lastMicName_ = name;
    }
  }

  const PwAudioNode* sink = nullptr;
  if (sys_->inPipeline) {
    const uint32_t streamNode = sys_->nodeId.load();
    if (streamNode != SPA_ID_INVALID) {
      if (const uint32_t linked = snap.LinkedOutputNode(streamNode)) sink = snap.ById(linked);
    }
    if (sink == nullptr && !sys_->targetName.empty()) sink = snap.ByName(sys_->targetName, true);
    if (sink == nullptr) sink = snap.DefaultSink();
    const std::string name = sink != nullptr ? sink->name : std::string();
    if (sink != nullptr && (force || name != lastSinkName_)) {
      pump_->Log(LogLevel::Info, "System audio source: monitor of " + sink->DisplayName() + " (" + sink->name + ")");
      lastSinkName_ = name;
    }
    const bool muted = sink != nullptr && sink->propsKnown && (sink->mute || sink->maxVolume <= 0.f);
    if (static_cast<int>(muted) != lastMuteState_) {
      if (muted) {
        pump_->Log(LogLevel::Info,
                   "WorkerLoop: hardware output muted/volume=0, zero-filling system audio buffer before processing");
      } else if (lastMuteState_ == 1) {
        pump_->Log(LogLevel::Info, "WorkerLoop: hardware output unmuted, resuming normal system audio processing");
      }
      lastMuteState_ = muted ? 1 : 0;
      sysOutputMuted_ = muted;
    }
  }

  const bool mixing = mic_->inPipeline && sys_->inPipeline;
  const AecDecision decision = DecideAec(snap, sink);
  const bool want = apm_ != nullptr && mixing && decision.enable;
  if (static_cast<int>(want) != lastAecState_) {
    if (apm_ != nullptr && mixing) {
      pump_->Log(LogLevel::Info, "Default output transport is " + decision.reason +
                                     (decision.enable ? " (enabling AEC if mixing)" : " (disabling AEC)"));
    }
    lastAecState_ = want ? 1 : 0;
    aecWanted_ = want;
    Wake();
  }
}

void CaptureEngine::Pause(Source source) {
  Side* side = source == Source::Microphone ? mic_.get() : sys_.get();
  if (!running_ || side == nullptr) return;
  const char* fn = source == Source::Microphone ? "StopMicrophone()" : "StopSystemAudio()";
  const char* what = source == Source::Microphone ? "microphone" : "system audio";
  if (!side->enabled.exchange(false)) {
    pump_->Log(LogLevel::Info, std::string(fn) + ": " + what + " already disabled.");
    return;
  }
  pump_->Log(LogLevel::Info, std::string(fn) + ": " + what + " capture paused (flag-only).");
  Wake();
}

bool CaptureEngine::Begin(Source source, std::string* detail) {
  Side* side = source == Source::Microphone ? mic_.get() : sys_.get();
  if (!running_ || side == nullptr) return true;
  const char* fn = source == Source::Microphone ? "BeginMicrophone()" : "BeginSystemAudio()";
  const char* what = source == Source::Microphone ? "microphone" : "system audio";
  // The drop request is published before `enabled`/`inPipeline` are set, so
  // the worker applies it before it consumes the resumed side.
  if (side->inPipeline) {
    if (side->enabled.load()) {
      pump_->Log(LogLevel::Info, std::string(fn) + ": " + what + " already running.");
    } else {
      side->lastDataNs = MonotonicNs();
      RequestDiscard(*side);
      side->enabled = true;
      pump_->Log(LogLevel::Info, std::string(fn) + ": " + what + " capture resumed.");
    }
    Wake();
    return true;
  }
  // The source was not part of the pipeline at start: add its stream now.
  // It is marked part of the pipeline first, so a restart after a failure
  // here recreates it.
  pump_->Log(LogLevel::Info, std::string(fn) + ": adding the " + what + " stream to the running pipeline.");
  side->lastDataNs = MonotonicNs();
  RequestDiscard(*side);
  side->inPipeline = true;
  side->enabled = true;
  if (conn_.threadLoop() == nullptr || !conn_.connected() || !registry_) {
    *detail = "PipeWire connection is not available";
    return false;
  }
  PwLock lock(conn_);
  const RegistrySnapshot snap = registry_->Snapshot();
  if (!CreateStreamLocked(*side, snap, detail)) return false;
  EvaluateGraphLocked(true);
  Wake();
  return true;
}

void CaptureEngine::SetPreferredDevices(const DeviceSelection& microphone, const DeviceSelection& speaker) {
  if (!running_) return;
  pump_->Log(LogLevel::Info, "SetPreferredDevices(microphone=" + microphone.Describe() +
                                 ", speaker=" + speaker.Describe() + ")");
  if (conn_.threadLoop() == nullptr || !conn_.connected() || !registry_) {
    mic_->selection = microphone;
    sys_->selection = speaker;
    pump_->Log(LogLevel::Info, "SetPreferredDevices: PipeWire is reconnecting; the selection applies on restart.");
    return;
  }
  bool exhausted = false;
  {
    PwLock lock(conn_);
    for (Side* side : {mic_.get(), sys_.get()}) {
      const DeviceSelection& wanted = side == mic_.get() ? microphone : speaker;
      const DeviceSelection previous = side->selection;
      side->selection = wanted;
      if (!side->inPipeline || side->stream == nullptr) continue;
      if (previous == wanted) continue;
      const bool isSys = side == sys_.get();
      const RegistrySnapshot snap = registry_->Snapshot();
      // What the new selection pins (empty = follow the default).
      const std::string newTarget = PinTarget(snap, wanted, isSys, nullptr);
      if (newTarget == side->targetName) continue;
      pump_->Log(LogLevel::Info, std::string(isSys ? "Switching system audio capture to " : "Switching microphone to ") +
                                     (newTarget.empty() ? std::string("the system default") : newTarget) + ".");
      DestroyStreamLocked(*side);
      std::string err;
      if (!CreateStreamLocked(*side, snap, &err)) {
        pump_->Log(LogLevel::Warning,
                   "SetPreferredDevices: restart with requested device failed: " + err + ". Falling back to system default.");
        side->selection = DeviceSelection{};
        DestroyStreamLocked(*side);
        if (!CreateStreamLocked(*side, snap, &err)) {
          pump_->Log(LogLevel::Error, "SetPreferredDevices: fallback to system default also failed: " + err);
          exhausted = true;
        } else {
          pump_->Log(LogLevel::Info, "Recorder restored using system default devices.");
        }
      }
    }
    EvaluateGraphLocked(true);
  }
  if (exhausted) {
    pump_->Fatal("recorder_restart_exhausted", "SetPreferredDevices: all restart attempts failed");
  }
}

void CaptureEngine::SetApmEnabled(bool enabled) {
  if (apmEnabled_.exchange(enabled) != enabled) {
    pump_->Log(LogLevel::Info, std::string("Audio processing ") + (enabled ? "enabled." : "disabled (raw microphone)."));
  }
}

void CaptureEngine::SetEncodingPaused(bool paused) { encodingPaused_ = paused; }

void CaptureEngine::FillSilence(int durationMs) {
  if (!running_ || durationMs < 1) return;
  const int chunks = durationMs / 10;
  if (chunks <= 0) return;
  static const std::vector<float> zeros(kChunk, 0.f);
  std::lock_guard<std::mutex> lock(encMu_);
  if (!encoder_) return;
  for (int i = 0; i < chunks; ++i) encoder_->Encode(zeros.data(), zeros.size(), nullptr);
}

void CaptureEngine::StartWorker() {
  workerStop_ = false;
  worker_ = std::thread([this] { WorkerMain(); });
}

void CaptureEngine::StopWorker() {
  workerStop_ = true;
  Wake();
  if (worker_.joinable()) worker_.join();
}

void CaptureEngine::WorkerMain() {
  pthread_setname_np(pthread_self(), "plaud-rec-work");
  while (!workerStop_.load(std::memory_order_acquire)) {
    pollfd p{wakeFd_, POLLIN, 0};
    poll(&p, 1, 10);
    uint64_t v = 0;
    while (read(wakeFd_, &v, sizeof(v)) > 0) {
    }
    ProcessAvailable(false);
  }
  ProcessAvailable(true);
  std::lock_guard<std::mutex> lock(encMu_);
  if (encoder_) encoder_->Finish();
}

bool CaptureEngine::Stalled(const Side& side, int64_t nowNs) const {
  // A source is stalled when it delivered nothing for longer than the
  // official 60 ms threshold, stretched for graphs running large quanta.
  const int64_t quantumNs = static_cast<int64_t>(side.lastWriteSamples.load(std::memory_order_relaxed)) *
                            1000000000LL / kSampleRate;
  const int64_t threshold = std::max<int64_t>(kStallNs, 3 * quantumNs);
  return nowNs - side.lastDataNs.load(std::memory_order_acquire) > threshold;
}

void CaptureEngine::UpdateStall(Side& side, bool haveData, int64_t nowNs) {
  const char* what = side.source == Source::Microphone ? "microphone" : "system audio";
  if (!haveData && Stalled(side, nowNs)) {
    if (!side.stallReported) {
      side.stallReported = true;
      ++stallLogCount_;
      const int64_t ms = (nowNs - side.lastDataNs.load()) / 1000000;
      pump_->Log(LogLevel::Warning, std::string("WorkerLoop: ") + what + " stalled (no new data for " +
                                        std::to_string(ms) + " ms, buffered " +
                                        std::to_string(side.ring.Available() * sizeof(float)) + " bytes).");
    }
  } else if (side.stallReported && !Stalled(side, nowNs)) {
    side.stallReported = false;
    pump_->Log(LogLevel::Info, std::string("WorkerLoop: ") + what + " recovered; data flowing again.");
  }
}

void CaptureEngine::ReportOverflow(Side& side) {
  const uint64_t total = side.overflowSamples.load(std::memory_order_relaxed);
  if (total != side.reportedOverflow) {
    const uint64_t dropped = total - side.reportedOverflow;
    side.reportedOverflow = total;
    pump_->Log(LogLevel::Warning, std::string(side.source == Source::Microphone ? "Mic" : "System audio") +
                                      " ring buffer overflow, dropped " + std::to_string(dropped) + " samples.");
  }
}

void CaptureEngine::RequestDiscard(Side& side) {
  side.discardMark.store(side.ring.WriteIndex(), std::memory_order_relaxed);
  side.discardSeq.fetch_add(1, std::memory_order_release);
  Wake();
}

void CaptureEngine::ApplyConsumerRequests(Side& side) {
  const uint64_t seq = side.discardSeq.load(std::memory_order_acquire);
  if (seq != side.discardApplied) {
    side.discardApplied = seq;
    side.ring.DiscardTo(side.discardMark.load(std::memory_order_relaxed));
  }
  const uint64_t violations = side.ring.consumerViolations();
  if (violations != side.reportedConsumerViolations) {
    side.reportedConsumerViolations = violations;
    pump_->Log(LogLevel::Error, std::string("Internal error: the ") +
                                    (side.source == Source::Microphone ? "microphone" : "system audio") +
                                    " ring buffer was consumed from more than one thread (" +
                                    std::to_string(violations) + " times).");
  }
}

void CaptureEngine::ProcessAvailable(bool final) {
  if (apm_) {
    const bool want = aecWanted_.load();
    if (want != aecApplied_) {
      apm_->SetEchoCancellation(want);
      aecApplied_ = want;
      pump_->Log(LogLevel::Info, std::string("Echo cancellation ") + (want ? "enabled." : "disabled."));
    }
  }
  if (!mic_ || !sys_) return;
  for (;;) {
    const int64_t now = MonotonicNs();
    // Read the flags before the requests: begin() publishes its drop request
    // before it sets `enabled`.
    const bool micOn = mic_->inPipeline && mic_->enabled;
    const bool sysOn = sys_->inPipeline && sys_->enabled;
    ApplyConsumerRequests(*mic_);
    ApplyConsumerRequests(*sys_);
    if (!micOn) mic_->ring.DiscardAll();
    if (!sysOn) sys_->ring.DiscardAll();
    ReportOverflow(*mic_);
    ReportOverflow(*sys_);
    size_t micAvail = micOn ? mic_->ring.Available() : 0;
    size_t sysAvail = sysOn ? sys_->ring.Available() : 0;
    const bool haveMic = micAvail >= kChunk;
    const bool haveSys = sysAvail >= kChunk;
    if (micOn && sysOn) {
      UpdateStall(*mic_, haveMic, now);
      UpdateStall(*sys_, haveSys, now);
      if (haveMic && haveSys) {
        // Both streams share a driver (node.group), so their backlogs stay
        // equal; a large difference means one side lagged (e.g. relink) and
        // the excess is dropped to keep microphone and loopback aligned.
        if (micAvail > sysAvail + kMaxSkewSamples) {
          const size_t drop = micAvail - sysAvail;
          mic_->ring.Discard(drop);
          pump_->Log(LogLevel::Warning, "Microphone ahead of system audio by " +
                                            std::to_string(drop * 1000 / kSampleRate) + " ms; realigned.");
          continue;
        }
        if (sysAvail > micAvail + kMaxSkewSamples) {
          const size_t drop = sysAvail - micAvail;
          sys_->ring.Discard(drop);
          pump_->Log(LogLevel::Warning, "System audio ahead of microphone by " +
                                            std::to_string(drop * 1000 / kSampleRate) + " ms; realigned.");
          continue;
        }
        ProcessChunk(true, true, true);
        continue;
      }
      if (haveMic && (final || Stalled(*sys_, now))) {
        ProcessChunk(true, false, false);
        continue;
      }
      if (haveSys && (final || Stalled(*mic_, now))) {
        ProcessChunk(false, true, false);
        continue;
      }
      return;
    }
    if (micOn && haveMic) {
      ProcessChunk(true, false, false);
      continue;
    }
    if (sysOn && haveSys) {
      ProcessChunk(false, true, false);
      continue;
    }
    return;
  }
}

void CaptureEngine::ProcessChunk(bool useMic, bool useSys, bool mixed) {
  float* mic = micBuf_.data();
  float* sys = sysBuf_.data();
  if (useMic) {
    mic_->ring.Read(mic, kChunk);
  } else {
    std::fill(micBuf_.begin(), micBuf_.end(), 0.f);
  }
  if (useSys) {
    sys_->ring.Read(sys, kChunk);
    if (sysOutputMuted_.load(std::memory_order_relaxed)) std::fill(sysBuf_.begin(), sysBuf_.end(), 0.f);
  } else {
    std::fill(sysBuf_.begin(), sysBuf_.end(), 0.f);
  }
  const float micRms = useMic ? Rms(mic, kChunk) : 0.f;
  const float sysRms = useSys ? Rms(sys, kChunk) : 0.f;
  if (dumps_) {
    Dumps::Write(dumps_->micRaw, mic, kChunk);
    Dumps::Write(dumps_->sysRaw, sys, kChunk);
    Dumps::Write(dumps_->sys, sys, kChunk);
  }

  const float* micProcessed = mic;
  bool apmRan = false;
  if (useMic && apm_ && apmEnabled_.load(std::memory_order_relaxed)) {
    if (mixed) apm_->ProcessReverse(sys, kChunk);
    if (apm_->ProcessCapture(mic, enhBuf_.data(), kChunk, kApmStreamDelayMs) == 0) {
      micProcessed = enhBuf_.data();
      apmRan = true;
    }
  }
  if (dumps_) {
    Dumps::Write(dumps_->micPost, micProcessed, kChunk);
    Dumps::Write(dumps_->mic, micProcessed, kChunk);
  }

  float* out = outBuf_.data();
  if (mixed) {
    const AdaptiveMixer::Gains gains = mixer_.Update(sysRms, Rms(micProcessed, kChunk), config_.systemVolume,
                                                     config_.microphoneVolume);
    AdaptiveMixer::Mix(sys, micProcessed, kChunk, gains, out);
  } else if (useMic) {
    const float v = static_cast<float>(config_.microphoneVolume);
    for (size_t i = 0; i < kChunk; ++i) out[i] = std::clamp(micProcessed[i] * v, -1.f, 1.f);
  } else {
    const float v = static_cast<float>(config_.systemVolume);
    for (size_t i = 0; i < kChunk; ++i) out[i] = std::clamp(sys[i] * v, -1.f, 1.f);
  }
  if (dumps_) {
    Dumps::Write(dumps_->mix, out, kChunk);
    Dumps::Write(dumps_->outMix, out, kChunk);
  }

  if (config_.emitApmSignals && apmRan) {
    RecorderEvent ev;
    ev.kind = RecorderEvent::Kind::Apm;
    ev.lpb.assign(sys, sys + kChunk);
    ev.mic.assign(mic, mic + kChunk);
    ev.enh.assign(micProcessed, micProcessed + kChunk);
    ev.sampleRate = kSampleRate;
    ev.channels = kChannels;
    pump_->Push(std::move(ev));
  }

  RecorderEvent audio;
  audio.kind = RecorderEvent::Kind::Audio;
  audio.pcm.assign(out, out + kChunk);
  audio.systemVolume = sysRms;
  audio.microphoneVolume = micRms;
  pump_->Push(std::move(audio));

  if (!encodingPaused_.load(std::memory_order_relaxed)) Encode(out, kChunk);
}

void CaptureEngine::Encode(const float* pcm, size_t n) {
  std::lock_guard<std::mutex> lock(encMu_);
  if (!encoder_) return;
  std::string err;
  if (!encoder_->Encode(pcm, n, &err) && !encodeErrorLogged_) {
    encodeErrorLogged_ = true;
    pump_->Log(LogLevel::Error, "LinuxOpusEncoder: " + err);
  }
}

void CaptureEngine::OpenDumps() {
  const char* preApm = std::getenv("PLAUD_PRE_APM_DUMP");
  const bool preApmDump = preApm != nullptr && *preApm != '\0' && std::strcmp(preApm, "0") != 0;
  if (!config_.debugDumpRawAudio && !preApmDump) return;
  char stamp[32];
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
  localtime_r(&t, &tm);
  std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tm);
  dumps_ = std::make_unique<Dumps>();
  if (config_.debugDumpRawAudio) {
    const std::string sysPath = DumpPath("sys", stamp), micPath = DumpPath("mic", stamp),
                      mixPath = DumpPath("mix", stamp);
    dumps_->sys = std::fopen(sysPath.c_str(), "wb");
    dumps_->mic = std::fopen(micPath.c_str(), "wb");
    dumps_->mix = std::fopen(mixPath.c_str(), "wb");
    if (dumps_->sys == nullptr || dumps_->mic == nullptr || dumps_->mix == nullptr) {
      pump_->Log(LogLevel::Warning, "Failed to open debug audio dump files; disabling debug dump.");
      for (FILE** f : {&dumps_->sys, &dumps_->mic, &dumps_->mix}) {
        if (*f != nullptr) std::fclose(*f);
        *f = nullptr;
      }
    } else {
      pump_->Log(LogLevel::Info, "Debug audio dumps enabled: sys=" + sysPath + ", mic=" + micPath + ", mix=" +
                                     mixPath + " (Float32 PCM, 48000 Hz, mono)");
    }
  }
  if (preApmDump) {
    pump_->Log(LogLevel::Info, "Pre-APM debug dumping enabled via PLAUD_PRE_APM_DUMP.");
    struct Item {
      const char* kind;
      FILE** file;
      const char* label;
    };
    const Item items[] = {{"mic_raw", &dumps_->micRaw, "Raw mic dump enabled (pre-processing): "},
                          {"mic_post3a", &dumps_->micPost, "Processed mic dump enabled (post-3A, pre-mix): "},
                          {"sys_raw", &dumps_->sysRaw, "Raw system dump enabled (pre-processing): "},
                          {"out_mix", &dumps_->outMix, "Output dump enabled (post-processing/mix): "}};
    for (const Item& item : items) {
      const std::string path = DumpPath(item.kind, stamp);
      *item.file = std::fopen(path.c_str(), "wb");
      if (*item.file == nullptr) {
        pump_->Log(LogLevel::Warning, std::string("Failed to open ") + item.kind + " dump file: " + path);
      } else {
        pump_->Log(LogLevel::Info, std::string(item.label) + path + " (Float32 PCM, interleaved)");
      }
    }
  }
}

void CaptureEngine::CloseDumps() { dumps_.reset(); }

}  // namespace plaud_rec
