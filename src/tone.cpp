#include "tone.h"

#include <cmath>
#include <vector>

#include "dsp.h"
#include "wasapi_util.h"

namespace sas {
namespace {

// Two soft bell notes (E5 then B5), interleaved for the given format. Only the front
// pair carries sound when the device has one, so virtual-surround headsets keep it
// centred.
std::vector<float> makeChime(const StreamFormat& fmt) {
  const double rate = fmt.rate;
  const size_t frames = static_cast<size_t>(0.8 * rate);
  const auto pos = channelPositions(fmt.channels, fmt.mask);
  bool hasFront = false;
  for (uint32_t p : pos) hasFront = hasFront || p == kFL || p == kFR;

  std::vector<float> pcm(frames * fmt.channels, 0.0f);
  struct Note { double start, freq; };
  const Note notes[] = {{0.0, 659.25}, {0.16, 987.77}};
  for (size_t i = 0; i < frames; ++i) {
    const double t = i / rate;
    double v = 0.0;
    for (const Note& n : notes) {
      const double u = t - n.start;
      if (u < 0) continue;
      const double env = std::min(1.0, u / 0.004) * std::exp(-u / 0.18);
      v += env * (std::sin(2 * kPi * n.freq * u) + 0.25 * std::sin(4 * kPi * n.freq * u));
    }
    const double tail = std::min(1.0, (0.8 - t) / 0.05);  // fade the last 50 ms to zero
    const float s = static_cast<float>(0.16 * v * tail);
    for (int c = 0; c < fmt.channels; ++c)
      if (!hasFront || pos[c] == kFL || pos[c] == kFR) pcm[i * fmt.channels + c] = s;
  }
  return pcm;
}

HRESULT play(const std::wstring& deviceId) {
  ComPtr<IMMDeviceEnumerator> en;
  HRESULT hr = createEnumerator(en);
  if (FAILED(hr)) return hr;
  ComPtr<IMMDevice> dev;
  hr = getActiveDevice(en.Get(), deviceId, dev);
  if (FAILED(hr)) return hr;
  ComPtr<IAudioClient> client;
  hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
  if (FAILED(hr)) return hr;
  MixFormat mix;
  hr = client->GetMixFormat(mix.put());
  if (FAILED(hr)) return hr;
  StreamFormat fmt;
  if (!parseFormat(mix.get(), fmt)) return AUDCLNT_E_UNSUPPORTED_FORMAT;
  hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 2000000, 0, mix.get(), nullptr);  // 200 ms buffer
  if (FAILED(hr)) return hr;
  UINT32 bufferFrames = 0;
  hr = client->GetBufferSize(&bufferFrames);
  if (FAILED(hr)) return hr;
  ComPtr<IAudioRenderClient> render;
  hr = client->GetService(IID_PPV_ARGS(&render));
  if (FAILED(hr)) return hr;

  const std::vector<float> pcm = makeChime(fmt);
  const size_t ch = static_cast<size_t>(fmt.channels);
  const size_t total = pcm.size() / ch;
  size_t pos = 0;
  bool started = false;
  while (pos < total) {
    UINT32 padding = 0;
    hr = client->GetCurrentPadding(&padding);
    if (FAILED(hr)) break;
    const UINT32 n = static_cast<UINT32>(std::min<size_t>(bufferFrames - padding, total - pos));
    if (n > 0) {
      BYTE* data = nullptr;
      hr = render->GetBuffer(n, &data);
      if (FAILED(hr)) break;
      fromFloat(fmt.type, &pcm[pos * ch], data, n * ch);
      hr = render->ReleaseBuffer(n, 0);
      if (FAILED(hr)) break;
      pos += n;
    }
    if (!started) {
      hr = client->Start();
      if (FAILED(hr)) break;
      started = true;
    }
    Sleep(20);
  }
  for (int i = 0; SUCCEEDED(hr) && i < 50; ++i) {  // let the tail play out
    UINT32 padding = 0;
    if (FAILED(client->GetCurrentPadding(&padding)) || padding == 0) break;
    Sleep(10);
  }
  client->Stop();
  return hr;
}

}  // namespace

HRESULT playTestSound(const std::wstring& deviceId) {
  const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const HRESULT hr = play(deviceId);
  if (SUCCEEDED(coHr)) CoUninitialize();
  return hr;
}

}  // namespace sas
