// cli.cpp - Console tool for trying the engine on real devices.
//
//   sas-cli list
//   sas-cli run    --target D [--target D ...] [--source D] [--seconds S] [--margin MS]
//   sas-cli verify --source D --target D [--seconds S] [--level DBFS]
//   sas-cli tone   D
//
// D is a device index from `list`, "default", or a full device ID.
// `verify` plays a quiet noise signal on the source, mirrors it with the engine, records
// both devices in loopback and checks that the copy matches (delay, fidelity, dropouts).

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "dsp.h"
#include "engine.h"
#include "tone.h"
#include "wasapi_util.h"

using namespace sas;

namespace {

std::string u8(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

std::vector<DeviceInfo> devices() {
  std::vector<DeviceInfo> list;
  ComPtr<IMMDeviceEnumerator> en;
  if (SUCCEEDED(createEnumerator(en))) listOutputDevices(en.Get(), list);
  return list;
}

// Resolves an index, "default" or an ID to a device ID ("" if unknown).
std::wstring pick(const std::vector<DeviceInfo>& list, const std::string& sel) {
  if (sel == "default") {
    for (const DeviceInfo& d : list)
      if (d.isDefault) return d.id;
    return L"";
  }
  if (!sel.empty() && sel.find_first_not_of("0123456789") == std::string::npos) {
    const size_t i = std::strtoul(sel.c_str(), nullptr, 10);
    return i < list.size() ? list[i].id : L"";
  }
  return widen(sel);
}

std::string nameOf(const std::vector<DeviceInfo>& list, const std::wstring& id) {
  for (const DeviceInfo& d : list)
    if (d.id == id) return u8(d.name);
  return u8(id);
}

const char* stateName(TargetState s) {
  switch (s) {
    case TargetState::Idle: return "Idle";
    case TargetState::Starting: return "Starting";
    case TargetState::Playing: return "Playing";
    case TargetState::IsSource: return "IsSource";
    case TargetState::Unavailable: return "Unavailable";
    case TargetState::Error: return "Error";
  }
  return "?";
}

int cmdList() {
  ComPtr<IMMDeviceEnumerator> en;
  if (FAILED(createEnumerator(en))) return 1;
  std::vector<DeviceInfo> list;
  listOutputDevices(en.Get(), list);
  for (size_t i = 0; i < list.size(); ++i) {
    const DeviceInfo& d = list[i];
    std::string fmt = "?";
    REFERENCE_TIME period = 0, minPeriod = 0;
    ComPtr<IMMDevice> dev;
    ComPtr<IAudioClient> client;
    if (SUCCEEDED(getActiveDevice(en.Get(), d.id, dev)) &&
        SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client))) {
      MixFormat mix;
      StreamFormat f;
      if (SUCCEEDED(client->GetMixFormat(mix.put())) && parseFormat(mix.get(), f)) fmt = u8(describeFormat(f));
      client->GetDevicePeriod(&period, &minPeriod);
    }
    std::printf("[%zu]%s %s\n     %s, period %.2f ms (min %.2f)\n     %s\n", i, d.isDefault ? " (default)" : "",
                u8(d.name).c_str(), fmt.c_str(), period / 1e4, minPeriod / 1e4, u8(d.id).c_str());
  }
  return 0;
}

void printStatus(const EngineStatus& st, const std::vector<DeviceInfo>& list, double t) {
  std::printf("[%5.1fs] source: %s | %s | loopback %s | captured %llu frames%s\n", t,
              nameOf(list, st.sourceId).c_str(), st.capturing ? u8(st.sourceFormat).c_str() : "not capturing",
              st.captureEvents ? "event-driven" : "polled", static_cast<unsigned long long>(st.capturedFrames),
              st.sourceError ? (" | " + u8(describeError(st.sourceError))).c_str() : "");
  for (const TargetStatus& ts : st.targets) {
    std::printf("          -> %-40.40s %-11s latency %6.1f ms  drift %+7.1f ppm  adj %+7.1f ppm  margin %4.1f ms  "
                "underruns %u  resyncs %u%s\n",
                nameOf(list, ts.id).c_str(), stateName(ts.state), ts.latencyMs, ts.driftPpm, ts.adjustPpm, ts.marginMs,
                ts.underruns, ts.resyncs, ts.error ? (" | " + u8(describeError(ts.error))).c_str() : "");
  }
  std::fflush(stdout);
}

struct Options {
  std::wstring source;  // "" = default
  std::vector<std::wstring> targets;
  double seconds = 10;
  double marginMs = 5;
  double levelDb = -60;
};

bool parseOptions(int argc, char** argv, int first, const std::vector<DeviceInfo>& list, Options& o) {
  for (int i = first; i < argc; ++i) {
    const std::string a = argv[i];
    const bool hasValue = i + 1 < argc;
    if (a == "--source" && hasValue) {
      o.source = pick(list, argv[++i]);
    } else if (a == "--target" && hasValue) {
      const std::wstring id = pick(list, argv[++i]);
      if (id.empty()) {
        std::fprintf(stderr, "unknown device: %s\n", argv[i]);
        return false;
      }
      o.targets.push_back(id);
    } else if (a == "--seconds" && hasValue) {
      o.seconds = std::atof(argv[++i]);
    } else if (a == "--margin" && hasValue) {
      o.marginMs = std::atof(argv[++i]);
    } else if (a == "--level" && hasValue) {
      o.levelDb = std::atof(argv[++i]);
    } else {
      std::fprintf(stderr, "unexpected argument: %s\n", a.c_str());
      return false;
    }
  }
  return true;
}

int cmdRun(const Options& o, const std::vector<DeviceInfo>& list) {
  Engine engine;
  engine.start();
  EngineConfig cfg;
  cfg.sourceId = o.source;
  cfg.targetIds = o.targets;
  cfg.marginMs = o.marginMs;
  engine.setConfig(cfg);
  const double t0 = nowSeconds();
  for (int s = 1; s <= static_cast<int>(o.seconds); ++s) {
    Sleep(static_cast<DWORD>(std::max(0.0, (t0 + s - nowSeconds()) * 1000.0)));
    printStatus(engine.status(), list, nowSeconds() - t0);
  }
  engine.stop();
  return 0;
}

// Keeps the first target playing while the second is added and removed every second;
// the first must not drop out. With two --source devices, alternates the source
// instead (the path a change of Windows default takes) and checks the target recovers.
int cmdChurn(const Options& o, const std::vector<std::wstring>& sources, const std::vector<DeviceInfo>& list) {
  const bool swapSources = sources.size() == 2;
  if ((swapSources && o.targets.size() != 1) || (!swapSources && o.targets.size() != 2)) {
    std::fprintf(stderr, "churn needs --target A --target B, or --source X --source Y --target A\n");
    return 2;
  }
  Engine engine;
  engine.start();
  EngineConfig cfg;
  cfg.marginMs = o.marginMs;
  cfg.sourceId = swapSources ? sources[0] : o.source;
  cfg.targetIds = swapSources ? o.targets : std::vector<std::wstring>{o.targets[0]};
  engine.setConfig(cfg);
  Sleep(1500);

  int cycles = 0, recovered = 0;
  const double t0 = nowSeconds();
  while (nowSeconds() - t0 < o.seconds) {
    ++cycles;
    if (swapSources) {
      cfg.sourceId = sources[cycles % 2];
    } else {
      cfg.targetIds = cycles % 2 ? o.targets : std::vector<std::wstring>{o.targets[0]};
    }
    engine.setConfig(cfg);
    Sleep(swapSources ? 2000 : 1000);
    const EngineStatus st = engine.status();
    if (swapSources && !st.targets.empty() && st.targets[0].state == TargetState::Playing) ++recovered;
  }
  const EngineStatus st = engine.status();
  engine.stop();
  printStatus(st, list, nowSeconds() - t0);
  if (swapSources) {
    std::printf("RESULT: %s - target playing after %d of %d source switches\n",
                recovered == cycles ? "PASS" : "FAIL", recovered, cycles);
    return recovered == cycles ? 0 : 1;
  }
  const bool ok = !st.targets.empty() && st.targets[0].state == TargetState::Playing && st.targets[0].underruns == 0 &&
                  st.targets[0].resyncs == 0;
  std::printf("RESULT: %s - %d add/remove cycles of the second device, first device underruns %u\n",
              ok ? "PASS" : "FAIL", cycles, st.targets.empty() ? 0u : st.targets[0].underruns);
  return ok ? 0 : 1;
}

// --- verify -------------------------------------------------------------------

// Opens a device in shared mode on the calling thread.
HRESULT openClient(const std::wstring& id, DWORD flags, ComPtr<IAudioClient>& client, StreamFormat& fmt) {
  ComPtr<IMMDeviceEnumerator> en;
  HRESULT hr = createEnumerator(en);
  if (FAILED(hr)) return hr;
  ComPtr<IMMDevice> dev;
  hr = getActiveDevice(en.Get(), id, dev);
  if (FAILED(hr)) return hr;
  hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
  if (FAILED(hr)) return hr;
  MixFormat mix;
  hr = client->GetMixFormat(mix.put());
  if (FAILED(hr)) return hr;
  if (!parseFormat(mix.get(), fmt)) return AUDCLNT_E_UNSUPPORTED_FORMAT;
  return client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 2000000, 0, mix.get(), nullptr);
}

// Indices of the front-left and front-right channels (both 0 for mono).
void frontPair(const StreamFormat& f, int& l, int& r) {
  const auto pos = channelPositions(f.channels, f.mask);
  l = 0;
  r = f.channels > 1 ? 1 : 0;
  for (int c = 0; c < f.channels; ++c) {
    if (pos[c] == kFL) l = c;
    if (pos[c] == kFR) r = c;
  }
}

// Low-passed noise (two one-pole stages at ~2 kHz), so small fractional delays
// barely affect the correlation.
struct NoiseGen {
  std::mt19937 rng;
  std::normal_distribution<double> n{0.0, 1.0};
  double a = 0, z1 = 0, z2 = 0, gain = 1;
  NoiseGen(unsigned seed, double rate, double levelDb) : rng(seed) {
    a = 1.0 - std::exp(-2 * kPi * 2000.0 / rate);
    gain = std::pow(10.0, levelDb / 20.0) * 2.4;  // compensates the filter's RMS loss
  }
  float next() {
    z1 += a * (n(rng) - z1);
    z2 += a * (z1 - z2);
    return static_cast<float>(z2 * gain);
  }
};

struct Player {
  std::atomic<bool> stop{false};
  HRESULT result = S_OK;
  void run(std::wstring id, double levelDb) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IAudioClient> client;
    StreamFormat f;
    ComPtr<IAudioRenderClient> render;
    UINT32 bufferFrames = 0;
    result = openClient(id, 0, client, f);
    if (SUCCEEDED(result)) result = client->GetBufferSize(&bufferFrames);
    if (SUCCEEDED(result)) result = client->GetService(IID_PPV_ARGS(&render));
    if (SUCCEEDED(result)) {
      int l, r;
      frontPair(f, l, r);
      NoiseGen left(1, f.rate, levelDb), right(2, f.rate, levelDb);
      std::vector<float> buf;
      bool started = false;
      while (!stop && SUCCEEDED(result)) {
        UINT32 padding = 0;
        if (FAILED(result = client->GetCurrentPadding(&padding))) break;
        const UINT32 n = bufferFrames - padding;
        if (n > 0) {
          buf.assign(static_cast<size_t>(n) * f.channels, 0.0f);
          for (UINT32 i = 0; i < n; ++i) {
            buf[i * f.channels + l] = left.next();
            if (r != l) buf[i * f.channels + r] = right.next();
          }
          BYTE* data = nullptr;
          if (FAILED(result = render->GetBuffer(n, &data))) break;
          fromFloat(f.type, buf.data(), data, buf.size());
          if (FAILED(result = render->ReleaseBuffer(n, 0))) break;
        }
        if (!started) {
          result = client->Start();
          started = true;
        }
        Sleep(5);
      }
      client->Stop();
    }
    CoUninitialize();
  }
};

struct Recorder {
  std::atomic<bool> stop{false};
  HRESULT result = S_OK;
  double rate = 0;
  std::vector<float> left, right;
  void run(std::wstring id) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    StreamFormat f;
    result = openClient(id, AUDCLNT_STREAMFLAGS_LOOPBACK, client, f);
    if (SUCCEEDED(result)) result = client->GetService(IID_PPV_ARGS(&capture));
    if (SUCCEEDED(result)) result = client->Start();
    if (SUCCEEDED(result)) {
      rate = f.rate;
      int l, r;
      frontPair(f, l, r);
      std::vector<float> tmp;
      while (!stop && SUCCEEDED(result)) {
        UINT32 packet = 0;
        while (SUCCEEDED(result = capture->GetNextPacketSize(&packet)) && packet > 0) {
          BYTE* data = nullptr;
          UINT32 frames = 0;
          DWORD flags = 0;
          if (FAILED(result = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
          tmp.resize(static_cast<size_t>(frames) * f.channels);
          if (flags & AUDCLNT_BUFFERFLAGS_SILENT) std::fill(tmp.begin(), tmp.end(), 0.0f);
          else toFloat(f.type, data, tmp.data(), tmp.size());
          capture->ReleaseBuffer(frames);
          for (UINT32 i = 0; i < frames; ++i) {
            left.push_back(tmp[i * f.channels + l]);
            right.push_back(tmp[i * f.channels + r]);
          }
        }
        Sleep(5);
      }
      client->Stop();
    }
    CoUninitialize();
  }
};

// Normalized correlation of copy[i..i+n) with ref[i-lag..i-lag+n).
double corrAt(const std::vector<float>& ref, const std::vector<float>& copy, size_t start, size_t n, double lag) {
  const long long l = static_cast<long long>(std::llround(lag));
  if (static_cast<long long>(start) - l < 0 || start + n > copy.size() || start - l + n > ref.size()) return 0;
  double xy = 0, xx = 0, yy = 0;
  for (size_t i = 0; i < n; ++i) {
    const double x = ref[start - l + i], y = copy[start + i];
    xy += x * y;
    xx += x * x;
    yy += y * y;
  }
  return xx > 0 && yy > 0 ? xy / std::sqrt(xx * yy) : 0;
}

// Best lag (in samples, fractional) of copy relative to ref within [lo, hi].
double bestLag(const std::vector<float>& ref, const std::vector<float>& copy, size_t start, size_t n, long long lo,
               long long hi, double& corr) {
  long long best = lo;
  double bestC = -2;
  std::vector<double> c(static_cast<size_t>(hi - lo + 1));
  for (long long lag = lo; lag <= hi; ++lag) {
    c[static_cast<size_t>(lag - lo)] = corrAt(ref, copy, start, n, static_cast<double>(lag));
    if (c[static_cast<size_t>(lag - lo)] > bestC) {
      bestC = c[static_cast<size_t>(lag - lo)];
      best = lag;
    }
  }
  corr = bestC;
  double frac = 0;
  if (best > lo && best < hi) {  // parabolic peak interpolation
    const double y0 = c[static_cast<size_t>(best - lo - 1)], y1 = bestC, y2 = c[static_cast<size_t>(best - lo + 1)];
    const double d = y0 - 2 * y1 + y2;
    if (d < 0) frac = 0.5 * (y0 - y2) / d;
  }
  return best + frac;
}

int cmdVerify(const Options& o, const std::vector<DeviceInfo>& list) {
  if (o.source.empty() || o.targets.size() != 1) {
    std::fprintf(stderr, "verify needs --source and exactly one --target\n");
    return 2;
  }
  const std::wstring target = o.targets[0];
  std::printf("verify: %s -> %s, %.0f s, test signal %.0f dBFS\n", nameOf(list, o.source).c_str(),
              nameOf(list, target).c_str(), o.seconds, o.levelDb);

  Recorder refRec, copyRec;
  Player player;
  std::thread tRef([&] { refRec.run(o.source); });
  std::thread tCopy([&] { copyRec.run(target); });
  std::thread tPlay([&] { player.run(o.source, o.levelDb); });

  Engine engine;
  engine.start();
  EngineConfig cfg;
  cfg.sourceId = o.source;
  cfg.targetIds = {target};
  cfg.marginMs = o.marginMs;
  engine.setConfig(cfg);
  const double t0 = nowSeconds();
  for (int s = 1; s <= static_cast<int>(o.seconds); ++s) {
    Sleep(static_cast<DWORD>(std::max(0.0, (t0 + s - nowSeconds()) * 1000.0)));
    if (s % 5 == 0 || s == static_cast<int>(o.seconds)) printStatus(engine.status(), list, nowSeconds() - t0);
  }
  const EngineStatus final = engine.status();
  engine.stop();
  player.stop = refRec.stop = copyRec.stop = true;
  tPlay.join();
  tRef.join();
  tCopy.join();
  if (FAILED(player.result) || FAILED(refRec.result) || FAILED(copyRec.result)) {
    std::printf("device error: player %s, source recorder %s, target recorder %s\n",
                u8(describeError(player.result)).c_str(), u8(describeError(refRec.result)).c_str(),
                u8(describeError(copyRec.result)).c_str());
    return 1;
  }

  // Bring the reference to the copy's sample rate if they differ.
  std::vector<float> refL = refRec.left, refR = refRec.right;
  if (refRec.rate != copyRec.rate) {
    for (std::vector<float>* v : {&refL, &refR}) {
      ResamplingFifo fifo;
      fifo.configure(1, refRec.rate, copyRec.rate, v->size() / refRec.rate + 1);
      fifo.write(v->data(), v->size());
      fifo.seekToFill(static_cast<double>(v->size()));
      std::vector<float> out(static_cast<size_t>(v->size() * copyRec.rate / refRec.rate) - 64);
      fifo.read(out.data(), out.size(), 0.0);
      *v = std::move(out);
    }
  }
  const std::vector<float>& copyL = copyRec.left;
  const std::vector<float>& copyR = copyRec.right;
  const double rate = copyRec.rate;
  std::printf("recorded: source %.1f s @ %.0f Hz, target %.1f s @ %.0f Hz\n", refRec.left.size() / refRec.rate,
              refRec.rate, copyL.size() / rate, rate);
  auto rmsDb = [](const std::vector<float>& v) {
    double s = 0;
    for (float x : v) s += static_cast<double>(x) * x;
    return 10.0 * std::log10(std::max(s / std::max<size_t>(v.size(), 1), 1e-30));
  };
  std::printf("levels: source L %.1f dBFS R %.1f dBFS, target L %.1f dBFS R %.1f dBFS\n", rmsDb(refL), rmsDb(refR),
              rmsDb(copyL), rmsDb(copyR));

  // The two recordings started at slightly different moments; the engine's delay is
  // measured from their alignment. Find the coarse offset first, then track it.
  const size_t win = static_cast<size_t>(0.25 * rate);
  const size_t skip = static_cast<size_t>(3.0 * rate);  // let the engine start up
  const long long maxLag = static_cast<long long>(0.5 * rate);
  if (copyL.size() < skip + 8 * win) {
    std::printf("not enough audio recorded\n");
    return 1;
  }
  double corr = 0;
  double lag = bestLag(refL, copyL, skip + static_cast<size_t>(maxLag), win, -maxLag, maxLag, corr);
  std::printf("coarse alignment: offset %.2f ms, correlation %.4f\n", lag / rate * 1000.0, corr);
  if (corr < 0.5) {
    std::printf("RESULT: FAIL - the target does not carry the source signal\n");
    return 1;
  }

  int windows = 0, bad = 0;
  double minL = 1, minR = 1, sumL = 0, sumR = 0, maxCross = 0, lagMin = lag, lagMax = lag, firstLag = lag;
  for (size_t start = skip + static_cast<size_t>(maxLag); start + win + 64 < copyL.size(); start += win) {
    const long long center = std::llround(lag);
    double cl = 0;
    lag = bestLag(refL, copyL, start, win, center - 48, center + 48, cl);
    const double cr = corrAt(refR, copyR, start, win, lag);
    const double cross = std::fabs(corrAt(refR, copyL, start, win, lag));  // left must not carry the right signal
    ++windows;
    if (cl < 0.98 || cr < 0.98) {
      ++bad;
      if (bad <= 10) std::printf("  low correlation at %.2f s: L %.4f R %.4f\n", start / rate, cl, cr);
    }
    minL = std::min(minL, cl);
    minR = std::min(minR, cr);
    sumL += cl;
    sumR += cr;
    maxCross = std::max(maxCross, cross);
    lagMin = std::min(lagMin, lag);
    lagMax = std::max(lagMax, lag);
  }
  std::printf("windows: %d x 250 ms, correlation L min %.4f mean %.4f, R min %.4f mean %.4f, L/R crosstalk %.3f\n",
              windows, minL, sumL / windows, minR, sumR / windows, maxCross);
  std::printf("offset: first %.2f ms, last %.2f ms, range %.2f ms\n", firstLag / rate * 1000, lag / rate * 1000,
              (lagMax - lagMin) / rate * 1000);
  const bool pass = bad == 0 && maxCross < 0.2 && !final.targets.empty() && final.targets[0].underruns == 0;
  std::printf("RESULT: %s\n", pass ? "PASS" : "FAIL");
  return pass ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  SetConsoleOutputCP(CP_UTF8);
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const std::string cmd = argc > 1 ? argv[1] : "";
  const std::vector<DeviceInfo> list = devices();
  int rc = 2;
  if (cmd == "list") {
    rc = cmdList();
  } else if (cmd == "tone" && argc > 2) {
    const HRESULT hr = playTestSound(pick(list, argv[2]));
    std::printf("%s\n", SUCCEEDED(hr) ? "played" : u8(describeError(hr)).c_str());
    rc = SUCCEEDED(hr) ? 0 : 1;
  } else if (cmd == "run" || cmd == "verify") {
    Options o;
    if (parseOptions(argc, argv, 2, list, o)) rc = cmd == "run" ? cmdRun(o, list) : cmdVerify(o, list);
  } else if (cmd == "churn") {
    // Collect every --source (churn accepts two); the rest parses as usual.
    std::vector<std::wstring> sources;
    std::vector<char*> rest = {argv[0], argv[1]};
    for (int i = 2; i < argc; ++i) {
      if (std::string(argv[i]) == "--source" && i + 1 < argc) sources.push_back(pick(list, argv[++i]));
      else rest.push_back(argv[i]);
    }
    Options o;
    if (sources.size() == 1) o.source = sources[0];
    if (parseOptions(static_cast<int>(rest.size()), rest.data(), 2, list, o)) rc = cmdChurn(o, sources, list);
  } else {
    std::fprintf(stderr,
                 "usage: sas-cli list | run --target D [--source D] [--seconds S] [--margin MS] |\n"
                 "       verify --source D --target D [--seconds S] [--level DBFS] | tone D\n");
  }
  CoUninitialize();
  return rc;
}
