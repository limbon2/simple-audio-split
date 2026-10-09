// engine.cpp - Mirrors whatever one playback device is playing onto other devices.
//
// Two threads:
//   * The audio thread (real-time priority) only moves audio. It drains the source's
//     loopback capture (exactly what the source is playing) into one MirrorPipeline
//     per target and tops up each target's buffer once per device period. It never
//     opens, closes or enumerates devices: that can take tens of milliseconds and
//     would starve the devices that are already playing.
//   * The control thread follows the configuration and device notifications. It opens
//     streams (fully initialised and already running on silence) and hands them to the
//     audio thread through a command queue. Streams the audio thread drops come back to
//     it for release, together with a report when a device failed, so it can retry.
//
// A silent stream is kept open on the source so its loopback keeps delivering steady
// packets during silence instead of stopping and restarting.

#include "engine.h"

#include <avrt.h>
#include <propkeydef.h>  // must precede functiondiscoverykeys_devpkey.h
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <mutex>
#include <thread>

#include "dsp.h"
#include "wasapi_util.h"

namespace sas {
namespace {

constexpr REFERENCE_TIME kCaptureBufferHns = 2000000;  // 200 ms loopback buffer
constexpr REFERENCE_TIME kSilenceBufferHns = 1000000;  // 100 ms
constexpr REFERENCE_TIME kRenderBufferHns = 500000;    // 50 ms; about 2 periods are kept queued
constexpr double kStatusIntervalSec = 0.25;

// Set SAS_TRACE=1 to log device opens and each target's first refills to stderr.
const bool kTrace = [] {
  wchar_t v[8];
  return GetEnvironmentVariableW(L"SAS_TRACE", v, 8) > 0;
}();

// How long to wait before trying a device again after a failure.
double retryDelay(HRESULT hr) {
  if (hr == kErrDeviceNotFound || hr == kErrDeviceNotActive) return 10.0;  // device notifications usually come first
  if (hr == AUDCLNT_E_DEVICE_INVALIDATED) return 0.3;                     // e.g. its sample rate was changed
  return 2.0;
}

// Forwards device notifications. Called on a system thread, so it only sets flags.
class DeviceWatcher final : public IMMNotificationClient {
 public:
  explicit DeviceWatcher(std::function<void(bool audioRelevant)> onChange) : onChange_(std::move(onChange)) {}

  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG r = InterlockedDecrement(&refs_);
    if (r == 0) delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
      *out = static_cast<IMMNotificationClient*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return notify(true); }
  HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return notify(true); }
  HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return notify(true); }
  HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
    return flow == eRender && role == eConsole ? notify(true) : S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY key) override {
    const bool renamed = IsEqualGUID(key.fmtid, PKEY_Device_FriendlyName.fmtid) && key.pid == PKEY_Device_FriendlyName.pid;
    return renamed ? notify(false) : S_OK;  // a rename only matters to the UI
  }

 private:
  HRESULT notify(bool audioRelevant) {
    onChange_(audioRelevant);
    return S_OK;
  }
  LONG refs_ = 1;
  std::function<void(bool)> onChange_;
};

struct Source {
  std::wstring id;
  uint64_t gen = 0;
  StreamFormat fmt;
  ComPtr<IAudioClient> client;
  ComPtr<IAudioCaptureClient> capture;
  HANDLE event = nullptr;
  bool eventDriven = false;
  ComPtr<IAudioClient> silentClient;
  ComPtr<IAudioRenderClient> silentRender;
  UINT32 silentFrames = 0;
  std::vector<float> samples;
  uint64_t frames = 0;

  ~Source() {
    if (client) client->Stop();
    if (silentClient) silentClient->Stop();
    capture.Reset();
    client.Reset();  // release before closing the event the client signals
    silentRender.Reset();
    silentClient.Reset();
    if (event) CloseHandle(event);
  }
};

struct Target {
  std::wstring id;
  uint64_t gen = 0;
  uint64_t sourceGen = 0;  // the source this target's pipeline was configured for
  StreamFormat fmt;
  ComPtr<IAudioClient> client;
  ComPtr<IAudioRenderClient> render;
  HANDLE event = nullptr;
  UINT32 bufferFrames = 0;
  UINT32 periodFrames = 0;
  UINT32 queueFrames = 0;  // how full the device buffer is kept
  MirrorPipeline pipe;
  std::vector<float> out;
  int traced = 0;  // refills logged so far (SAS_TRACE)

  ~Target() {
    if (client) client->Stop();
    render.Reset();
    client.Reset();
    if (event) CloseHandle(event);
  }
};

enum class SlotState { Idle, Open, IsSource, Unavailable, Error, Reopening };

// The control thread's view of one configured target.
struct Slot {
  std::wstring id;
  SlotState state = SlotState::Idle;
  HRESULT error = S_OK;
  double retryAt = 0;
  uint64_t gen = 0;  // generation of the stream handed to the audio thread
};

struct Command {
  enum class Kind { SetSource, ClearSource, AddTarget, RemoveTarget } kind;
  std::unique_ptr<Source> source;
  std::unique_ptr<Target> target;
  std::wstring id;
};

struct Failure {
  bool isSource = false;
  std::wstring id;
  uint64_t gen = 0;
  HRESULT hr = S_OK;
};

struct StreamStats {
  std::wstring id;
  bool priming = true;
  double latencyMs = 0, driftPpm = 0, adjustPpm = 0, marginMs = 0;
  uint32_t underruns = 0, resyncs = 0;
  std::wstring format;
};

struct AudioSnapshot {
  bool capturing = false;
  std::wstring sourceFormat;
  bool captureEvents = false;
  uint64_t capturedFrames = 0;
  std::vector<StreamStats> targets;
};

struct ControlSnapshot {
  bool enabled = false;
  std::wstring sourceId;
  HRESULT sourceError = S_OK;
  std::vector<Slot> slots;
};

}  // namespace

struct Engine::Impl {
  // Shared between threads (guarded by mu).
  mutable std::mutex mu;
  EngineConfig pendingConfig;
  bool configChanged = false;
  std::vector<Command> commands;                     // control -> audio
  std::vector<Failure> failures;                     // audio -> control
  std::vector<std::unique_ptr<Source>> deadSources;  // audio -> control, released there
  std::vector<std::unique_ptr<Target>> deadTargets;
  AudioSnapshot audioSnap;
  ControlSnapshot controlSnap;

  std::atomic<bool> quit{false};
  std::atomic<bool> devicesChanged{false};
  HANDLE controlWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  HANDLE audioWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  HWND notifyWnd = nullptr;
  UINT notifyMsg = 0;
  std::thread controlThread;
  std::thread audioThread;

  // Control thread only.
  ComPtr<IMMDeviceEnumerator> enumerator;
  DeviceWatcher* watcher = nullptr;
  EngineConfig cfg;
  std::vector<Slot> slots;
  bool sourceOpen = false;
  std::wstring sourceId;
  StreamFormat sourceFormat;
  uint64_t sourceGen = 0;
  HRESULT sourceError = S_OK;
  double sourceRetryAt = 0;
  uint64_t nextGen = 1;

  // Audio thread only.
  std::unique_ptr<Source> source;
  std::vector<std::unique_ptr<Target>> targets;

  ~Impl() {
    CloseHandle(controlWake);
    CloseHandle(audioWake);
  }

  // Control thread.
  void controlMain();
  void reconcile(double now);
  void handleFailures(const std::vector<Failure>& list, double now);
  double nextRetry() const;
  std::unique_ptr<Source> openSource(const std::wstring& id, HRESULT& hr);
  std::unique_ptr<Target> openTarget(const std::wstring& id, HRESULT& hr);
  void send(Command cmd);
  void publishControl();

  // Audio thread.
  void audioMain();
  void runCommands();
  void dropSource(HRESULT hr);
  void dropTarget(size_t index, HRESULT hr);
  void pumpCapture(double now);
  HRESULT serviceTarget(Target& t, double now);
  void feedSilence();
  void publishAudio();
};

// ---------------------------------------------------------------------------
// Control thread

void Engine::Impl::controlMain() {
  const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  sourceError = createEnumerator(enumerator);
  if (enumerator) {
    watcher = new DeviceWatcher([this](bool audioRelevant) {
      if (audioRelevant) {
        devicesChanged = true;
        SetEvent(controlWake);
      }
      if (notifyWnd) PostMessageW(notifyWnd, notifyMsg, 0, 0);
    });
    if (FAILED(enumerator->RegisterEndpointNotificationCallback(watcher))) {
      watcher->Release();
      watcher = nullptr;
    }
  }

  bool dirty = true;
  while (!quit) {
    std::vector<Failure> failed;
    std::vector<std::unique_ptr<Source>> oldSources;
    std::vector<std::unique_ptr<Target>> oldTargets;
    {
      std::lock_guard<std::mutex> lock(mu);
      if (configChanged) {
        cfg = pendingConfig;
        configChanged = false;
        dirty = true;
      }
      failed.swap(failures);
      oldSources.swap(deadSources);
      oldTargets.swap(deadTargets);
    }
    oldTargets.clear();  // stop and release streams here, off the audio thread
    oldSources.clear();

    const double now = nowSeconds();
    if (!failed.empty()) {
      handleFailures(failed, now);
      dirty = true;
    }
    if (devicesChanged.exchange(false)) {
      // Something was plugged in, removed or made default: retry everything now.
      sourceRetryAt = 0;
      for (Slot& s : slots) s.retryAt = 0;
      dirty = true;
    }
    if (now >= nextRetry()) dirty = true;
    if (dirty && enumerator) {
      reconcile(now);
      dirty = false;
    }
    publishControl();

    const double wait = nextRetry() - nowSeconds();
    const DWORD timeout = std::isinf(wait) ? INFINITE : static_cast<DWORD>(std::clamp(wait * 1000.0 + 1.0, 1.0, 60000.0));
    WaitForSingleObject(controlWake, timeout);
  }

  if (watcher) {
    enumerator->UnregisterEndpointNotificationCallback(watcher);
    watcher->Release();
    watcher = nullptr;
  }
  enumerator.Reset();
  if (SUCCEEDED(coHr)) CoUninitialize();
}

// Brings the streams in line with the configuration and the devices present.
void Engine::Impl::reconcile(double now) {
  // One slot per configured target, in configuration order.
  std::vector<Slot> next;
  for (const std::wstring& id : cfg.targetIds) {
    if (id.empty() || std::any_of(next.begin(), next.end(), [&](const Slot& s) { return s.id == id; })) continue;
    auto it = std::find_if(slots.begin(), slots.end(), [&](const Slot& s) { return s.id == id; });
    if (it != slots.end()) {
      next.push_back(std::move(*it));
      it->id.clear();  // moved
    } else {
      Slot s;
      s.id = id;
      next.push_back(std::move(s));
    }
  }
  for (const Slot& old : slots)
    if (!old.id.empty() && old.state == SlotState::Open) send({Command::Kind::RemoveTarget, nullptr, nullptr, old.id});
  slots = std::move(next);

  auto closeSource = [&] {
    if (sourceOpen) send({Command::Kind::ClearSource});
    sourceOpen = false;
    for (Slot& s : slots) {
      if (s.state == SlotState::Open) s.state = SlotState::Idle;
      if (s.state == SlotState::Idle) s.retryAt = 0;
    }
  };

  if (!cfg.enabled) {
    closeSource();
    sourceId.clear();
    sourceError = S_OK;
    for (Slot& s : slots) s.state = SlotState::Idle;
    return;
  }

  const std::wstring wanted = cfg.sourceId.empty() ? defaultOutputId(enumerator.Get()) : cfg.sourceId;
  if (sourceOpen && wanted != sourceId) closeSource();
  sourceId = wanted;
  if (!sourceOpen) {
    if (wanted.empty()) {
      sourceError = kErrDeviceNotFound;
      sourceRetryAt = now + retryDelay(sourceError);
    } else if (now >= sourceRetryAt) {
      HRESULT hr = S_OK;
      std::unique_ptr<Source> src = openSource(wanted, hr);
      if (src) {
        sourceFormat = src->fmt;
        sourceGen = src->gen = nextGen++;
        send({Command::Kind::SetSource, std::move(src)});
        sourceOpen = true;
        sourceError = S_OK;
      } else {
        sourceError = hr;
        sourceRetryAt = now + retryDelay(hr);
      }
    }
  }

  for (Slot& s : slots) {
    if (s.id == sourceId) {  // it already plays the sound itself
      if (s.state == SlotState::Open) send({Command::Kind::RemoveTarget, nullptr, nullptr, s.id});
      s.state = SlotState::IsSource;
      s.error = S_OK;
      s.retryAt = 0;
      continue;
    }
    if (!sourceOpen) {
      s.state = SlotState::Idle;
      s.retryAt = 0;
      continue;
    }
    if (s.state == SlotState::Open || now < s.retryAt) continue;
    HRESULT hr = S_OK;
    std::unique_ptr<Target> t = openTarget(s.id, hr);
    if (t) {
      s.gen = t->gen = nextGen++;
      t->sourceGen = sourceGen;
      send({Command::Kind::AddTarget, nullptr, std::move(t)});
      s.state = SlotState::Open;
      s.error = S_OK;
    } else {
      s.error = hr;
      s.state = hr == AUDCLNT_E_DEVICE_INVALIDATED ? SlotState::Reopening
                : isDeviceMissing(hr)              ? SlotState::Unavailable
                                                   : SlotState::Error;
      s.retryAt = now + retryDelay(hr);
    }
  }
}

void Engine::Impl::handleFailures(const std::vector<Failure>& list, double now) {
  for (const Failure& f : list) {
    if (f.isSource) {
      if (!sourceOpen || f.gen != sourceGen) continue;  // about a source that was already replaced
      sourceOpen = false;
      sourceError = f.hr;
      sourceRetryAt = now + retryDelay(f.hr);
      for (Slot& s : slots) {
        if (s.state == SlotState::Open) {  // dropped together with the source
          s.state = SlotState::Reopening;
          s.retryAt = 0;
        }
      }
    } else {
      auto it = std::find_if(slots.begin(), slots.end(), [&](const Slot& s) { return s.id == f.id; });
      if (it == slots.end() || it->state != SlotState::Open || it->gen != f.gen) continue;
      it->error = f.hr;
      it->state = f.hr == AUDCLNT_E_DEVICE_INVALIDATED ? SlotState::Reopening
                  : isDeviceMissing(f.hr)              ? SlotState::Unavailable
                                                       : SlotState::Error;
      it->retryAt = now + retryDelay(f.hr);
    }
  }
}

// When the next retry of a failed device is due (infinity if nothing is pending).
double Engine::Impl::nextRetry() const {
  double at = std::numeric_limits<double>::infinity();
  if (!cfg.enabled || !enumerator) return at;
  if (!sourceOpen) return sourceRetryAt;
  for (const Slot& s : slots)
    if (s.state != SlotState::Open && s.state != SlotState::IsSource) at = std::min(at, s.retryAt);
  return at;
}

std::unique_ptr<Source> Engine::Impl::openSource(const std::wstring& id, HRESULT& hr) {
  const double started = nowSeconds();
  auto src = std::make_unique<Source>();
  src->id = id;
  ComPtr<IMMDevice> dev;
  hr = getActiveDevice(enumerator.Get(), id, dev);
  if (FAILED(hr)) return nullptr;

  MixFormat mix;
  for (int attempt = 0; attempt < 2; ++attempt) {
    // Event-driven loopback is not supported on every Windows build; fall back to polling.
    const bool events = attempt == 0;
    src->client.Reset();
    hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &src->client);
    if (FAILED(hr)) return nullptr;
    hr = src->client->GetMixFormat(mix.put());
    if (FAILED(hr)) return nullptr;
    if (!parseFormat(mix.get(), src->fmt)) {
      hr = AUDCLNT_E_UNSUPPORTED_FORMAT;
      return nullptr;
    }
    const DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK | (events ? AUDCLNT_STREAMFLAGS_EVENTCALLBACK : 0);
    hr = src->client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kCaptureBufferHns, 0, mix.get(), nullptr);
    if (SUCCEEDED(hr) && events) {
      if (!src->event) src->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      hr = src->client->SetEventHandle(src->event);
    }
    if (SUCCEEDED(hr)) {
      src->eventDriven = events;
      break;
    }
    if (!events) return nullptr;
  }
  hr = src->client->GetService(IID_PPV_ARGS(&src->capture));
  if (FAILED(hr)) return nullptr;

  // The silent keep-alive stream is optional: without it the loopback just pauses
  // during silence.
  bool silentOk = SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &src->silentClient)) &&
                  SUCCEEDED(src->silentClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, kSilenceBufferHns, 0,
                                                          mix.get(), nullptr)) &&
                  SUCCEEDED(src->silentClient->GetBufferSize(&src->silentFrames)) &&
                  SUCCEEDED(src->silentClient->GetService(IID_PPV_ARGS(&src->silentRender)));
  if (silentOk) {
    BYTE* data = nullptr;
    silentOk = SUCCEEDED(src->silentRender->GetBuffer(src->silentFrames, &data)) &&
               SUCCEEDED(src->silentRender->ReleaseBuffer(src->silentFrames, AUDCLNT_BUFFERFLAGS_SILENT)) &&
               SUCCEEDED(src->silentClient->Start());
  }
  if (!silentOk) {
    src->silentRender.Reset();
    src->silentClient.Reset();
  }

  hr = src->client->Start();
  if (FAILED(hr)) return nullptr;
  if (kTrace) std::fprintf(stderr, "opened source in %.1f ms\n", (nowSeconds() - started) * 1000.0);
  return src;
}

std::unique_ptr<Target> Engine::Impl::openTarget(const std::wstring& id, HRESULT& hr) {
  const double started = nowSeconds();
  auto t = std::make_unique<Target>();
  t->id = id;
  ComPtr<IMMDevice> dev;
  hr = getActiveDevice(enumerator.Get(), id, dev);
  if (FAILED(hr)) return nullptr;
  hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &t->client);
  if (FAILED(hr)) return nullptr;
  MixFormat mix;
  hr = t->client->GetMixFormat(mix.put());
  if (FAILED(hr)) return nullptr;
  if (!parseFormat(mix.get(), t->fmt)) {
    hr = AUDCLNT_E_UNSUPPORTED_FORMAT;
    return nullptr;
  }
  hr = t->client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, kRenderBufferHns, 0,
                             mix.get(), nullptr);
  if (FAILED(hr)) return nullptr;
  t->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  hr = t->client->SetEventHandle(t->event);
  if (FAILED(hr)) return nullptr;
  hr = t->client->GetBufferSize(&t->bufferFrames);
  if (FAILED(hr)) return nullptr;
  hr = t->client->GetService(IID_PPV_ARGS(&t->render));
  if (FAILED(hr)) return nullptr;

  REFERENCE_TIME period = 0;
  if (FAILED(t->client->GetDevicePeriod(&period, nullptr)) || period <= 0) period = 100000;  // 10 ms
  t->periodFrames = std::max<UINT32>(1, static_cast<UINT32>(t->fmt.rate * period / 1e7 + 0.5));
  t->queueFrames = std::min(t->bufferFrames, 2 * t->periodFrames);

  const StreamFormat& s = sourceFormat;
  t->pipe.configure({s.channels, s.mask, s.rate}, {t->fmt.channels, t->fmt.mask, t->fmt.rate},
                    t->periodFrames / t->fmt.rate, cfg.marginMs / 1000.0);

  // Start on a full queue of silence; real audio follows once the pipeline has buffered.
  BYTE* data = nullptr;
  hr = t->render->GetBuffer(t->queueFrames, &data);
  if (FAILED(hr)) return nullptr;
  hr = t->render->ReleaseBuffer(t->queueFrames, AUDCLNT_BUFFERFLAGS_SILENT);
  if (FAILED(hr)) return nullptr;
  hr = t->client->Start();
  if (FAILED(hr)) return nullptr;
  if (kTrace) std::fprintf(stderr, "opened target in %.1f ms\n", (nowSeconds() - started) * 1000.0);
  return t;
}

void Engine::Impl::send(Command cmd) {
  {
    std::lock_guard<std::mutex> lock(mu);
    commands.push_back(std::move(cmd));
  }
  SetEvent(audioWake);
}

void Engine::Impl::publishControl() {
  ControlSnapshot snap;
  snap.enabled = cfg.enabled;
  snap.sourceId = sourceId;
  snap.sourceError = sourceOpen ? S_OK : sourceError;
  snap.slots = slots;
  std::lock_guard<std::mutex> lock(mu);
  controlSnap = std::move(snap);
}

// ---------------------------------------------------------------------------
// Audio thread

void Engine::Impl::audioMain() {
  const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  DWORD taskIndex = 0;
  HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

  double lastPublish = 0;
  while (!quit) {
    runCommands();
    HANDLE handles[MAXIMUM_WAIT_OBJECTS];
    DWORD count = 0;
    handles[count++] = audioWake;
    if (source && source->event) handles[count++] = source->event;
    for (const auto& t : targets)
      if (count < MAXIMUM_WAIT_OBJECTS) handles[count++] = t->event;
    WaitForMultipleObjects(count, handles, FALSE, source ? 10 : 250);
    if (quit) break;

    const double now = nowSeconds();
    if (source) pumpCapture(now);
    for (size_t i = 0; i < targets.size();) {
      const HRESULT hr = serviceTarget(*targets[i], now);
      if (FAILED(hr)) dropTarget(i, hr);
      else ++i;
    }
    if (source) feedSilence();
    if (now - lastPublish >= kStatusIntervalSec) {
      publishAudio();
      lastPublish = now;
    }
  }

  dropSource(S_OK);  // hands everything to the control side for release
  publishAudio();
  if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
  if (SUCCEEDED(coHr)) CoUninitialize();
}

void Engine::Impl::runCommands() {
  std::vector<Command> cmds;
  {
    std::lock_guard<std::mutex> lock(mu);
    if (commands.empty()) return;
    cmds.swap(commands);
  }
  for (Command& c : cmds) {
    switch (c.kind) {
      case Command::Kind::SetSource:
        dropSource(S_OK);
        source = std::move(c.source);
        break;
      case Command::Kind::ClearSource:
        dropSource(S_OK);
        break;
      case Command::Kind::AddTarget:
        targets.push_back(std::move(c.target));
        if (!source || targets.back()->sourceGen != source->gen) {
          // Built for a source that has gone meanwhile: have it reopened.
          dropTarget(targets.size() - 1, AUDCLNT_E_DEVICE_INVALIDATED);
        }
        break;
      case Command::Kind::RemoveTarget:
        for (size_t i = 0; i < targets.size(); ++i) {
          if (targets[i]->id == c.id) {
            dropTarget(i, S_OK);
            break;
          }
        }
        break;
    }
  }
  publishAudio();
}

// Drops the source and every target (they were built for its format). A failure
// HRESULT is reported to the control thread.
void Engine::Impl::dropSource(HRESULT hr) {
  if (!source && targets.empty()) return;
  {
    std::lock_guard<std::mutex> lock(mu);
    if (source) {
      if (FAILED(hr)) failures.push_back({true, source->id, source->gen, hr});
      deadSources.push_back(std::move(source));
    }
    for (auto& t : targets) deadTargets.push_back(std::move(t));
  }
  source.reset();
  targets.clear();
  SetEvent(controlWake);
}

void Engine::Impl::dropTarget(size_t index, HRESULT hr) {
  std::unique_ptr<Target> t = std::move(targets[index]);
  targets.erase(targets.begin() + static_cast<ptrdiff_t>(index));
  {
    std::lock_guard<std::mutex> lock(mu);
    if (FAILED(hr)) failures.push_back({false, t->id, t->gen, hr});
    deadTargets.push_back(std::move(t));
  }
  SetEvent(controlWake);
}

void Engine::Impl::pumpCapture(double now) {
  Source& src = *source;
  for (;;) {
    UINT32 packet = 0;
    HRESULT hr = src.capture->GetNextPacketSize(&packet);
    if (FAILED(hr)) return dropSource(hr);
    if (packet == 0) return;
    BYTE* data = nullptr;
    UINT32 frames = 0;
    DWORD flags = 0;
    hr = src.capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
    if (hr == AUDCLNT_S_BUFFER_EMPTY) return;
    if (FAILED(hr)) return dropSource(hr);
    const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
    const size_t count = static_cast<size_t>(frames) * src.fmt.channels;
    if (!silent) {
      src.samples.resize(count);
      toFloat(src.fmt.type, data, src.samples.data(), count);
    }
    hr = src.capture->ReleaseBuffer(frames);
    if (FAILED(hr)) return dropSource(hr);
    src.frames += frames;
    for (const auto& t : targets) t->pipe.push(silent ? nullptr : src.samples.data(), frames, now);
  }
}

HRESULT Engine::Impl::serviceTarget(Target& t, double now) {
  UINT32 padding = 0;
  HRESULT hr = t.client->GetCurrentPadding(&padding);
  if (FAILED(hr)) return hr;
  if (padding >= t.queueFrames) return S_OK;  // the device has not taken the next period yet

  const UINT32 frames = t.queueFrames - padding;
  const size_t count = static_cast<size_t>(frames) * t.fmt.channels;
  t.out.resize(count);
  const double bufferedMs = t.pipe.bufferedSec() * 1000.0;
  const bool audible = t.pipe.pull(t.out.data(), frames, now);
  if (kTrace && t.traced < 80) {
    std::fprintf(stderr, "[%.4f] target %llu: padding %u, wrote %u, fifo %.2f ms before, %s, underruns %u\n", now,
                 static_cast<unsigned long long>(t.gen), padding, frames, bufferedMs, audible ? "audio" : "silence",
                 t.pipe.underruns());
    ++t.traced;
  }

  BYTE* data = nullptr;
  hr = t.render->GetBuffer(frames, &data);
  if (FAILED(hr)) return hr;
  if (audible) fromFloat(t.fmt.type, t.out.data(), data, count);
  return t.render->ReleaseBuffer(frames, audible ? 0 : AUDCLNT_BUFFERFLAGS_SILENT);
}

void Engine::Impl::feedSilence() {
  Source& src = *source;
  if (!src.silentRender) return;
  UINT32 padding = 0;
  if (FAILED(src.silentClient->GetCurrentPadding(&padding))) return;  // the capture side reports real failures
  const UINT32 space = src.silentFrames - padding;
  if (space < src.silentFrames / 2) return;
  BYTE* data = nullptr;
  if (SUCCEEDED(src.silentRender->GetBuffer(space, &data)))
    src.silentRender->ReleaseBuffer(space, AUDCLNT_BUFFERFLAGS_SILENT);
}

void Engine::Impl::publishAudio() {
  AudioSnapshot snap;
  snap.capturing = source != nullptr;
  if (source) {
    snap.sourceFormat = describeFormat(source->fmt);
    snap.captureEvents = source->eventDriven;
    snap.capturedFrames = source->frames;
  }
  for (const auto& t : targets) {
    StreamStats s;
    s.id = t->id;
    s.priming = t->pipe.priming();
    const double queueSec = (t->queueFrames - t->periodFrames / 2.0) / t->fmt.rate;  // average device queue
    s.latencyMs = (t->pipe.latencySec() + queueSec) * 1000.0;
    s.driftPpm = t->pipe.driftPpm();
    s.adjustPpm = t->pipe.adjustPpm();
    s.marginMs = t->pipe.marginSec() * 1000.0;
    s.underruns = t->pipe.underruns();
    s.resyncs = t->pipe.resyncs();
    s.format = describeFormat(t->fmt);
    snap.targets.push_back(std::move(s));
  }
  std::lock_guard<std::mutex> lock(mu);
  audioSnap = std::move(snap);
}

// ---------------------------------------------------------------------------
// Public interface

Engine::Engine() : impl_(std::make_unique<Impl>()) {}

Engine::~Engine() { stop(); }

void Engine::start(HWND notifyWnd, UINT notifyMsg) {
  if (impl_->audioThread.joinable()) return;
  impl_->quit = false;
  impl_->notifyWnd = notifyWnd;
  impl_->notifyMsg = notifyMsg;
  impl_->audioThread = std::thread([this] { impl_->audioMain(); });
  impl_->controlThread = std::thread([this] { impl_->controlMain(); });
}

void Engine::stop() {
  if (!impl_->audioThread.joinable()) return;
  impl_->quit = true;
  SetEvent(impl_->audioWake);
  impl_->audioThread.join();
  SetEvent(impl_->controlWake);
  impl_->controlThread.join();
  // Streams still queued between the threads.
  impl_->commands.clear();
  impl_->deadTargets.clear();
  impl_->deadSources.clear();
}

void Engine::setConfig(const EngineConfig& config) {
  {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->pendingConfig = config;
    impl_->configChanged = true;
  }
  SetEvent(impl_->controlWake);
}

EngineStatus Engine::status() const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  const ControlSnapshot& c = impl_->controlSnap;
  const AudioSnapshot& a = impl_->audioSnap;
  EngineStatus st;
  st.enabled = c.enabled;
  st.capturing = a.capturing;
  st.sourceId = c.sourceId;
  st.sourceError = a.capturing ? S_OK : c.sourceError;
  st.sourceFormat = a.sourceFormat;
  st.captureEvents = a.captureEvents;
  st.capturedFrames = a.capturedFrames;
  for (const Slot& slot : c.slots) {
    TargetStatus ts;
    ts.id = slot.id;
    ts.error = slot.error;
    switch (slot.state) {
      case SlotState::Idle: ts.state = TargetState::Idle; break;
      case SlotState::IsSource: ts.state = TargetState::IsSource; break;
      case SlotState::Unavailable: ts.state = TargetState::Unavailable; break;
      case SlotState::Error: ts.state = TargetState::Error; break;
      case SlotState::Reopening: ts.state = TargetState::Starting; break;
      case SlotState::Open: {
        ts.state = TargetState::Starting;
        const auto it = std::find_if(a.targets.begin(), a.targets.end(),
                                     [&](const StreamStats& s) { return s.id == slot.id; });
        if (it != a.targets.end()) {
          ts.state = it->priming ? TargetState::Starting : TargetState::Playing;
          ts.latencyMs = it->latencyMs;
          ts.driftPpm = it->driftPpm;
          ts.adjustPpm = it->adjustPpm;
          ts.marginMs = it->marginMs;
          ts.underruns = it->underruns;
          ts.resyncs = it->resyncs;
          ts.format = it->format;
        }
        break;
      }
    }
    st.targets.push_back(std::move(ts));
  }
  return st;
}

}  // namespace sas
