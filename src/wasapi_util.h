// wasapi_util.h - Small helpers around the Windows Core Audio (MMDevice/WASAPI) APIs.
#pragma once

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <string>
#include <vector>

#include "dsp.h"

namespace sas {

using Microsoft::WRL::ComPtr;

struct DeviceInfo {
  std::wstring id;
  std::wstring name;
  bool isDefault = false;
};

struct StreamFormat {
  double rate = 0;
  int channels = 0;
  uint32_t mask = 0;
  SampleType type = SampleType::Unsupported;
  int blockAlign = 0;
};

// Owns a WAVEFORMATEX allocated by WASAPI (e.g. from GetMixFormat).
class MixFormat {
 public:
  MixFormat() = default;
  MixFormat(const MixFormat&) = delete;
  MixFormat& operator=(const MixFormat&) = delete;
  ~MixFormat() { CoTaskMemFree(p_); }
  WAVEFORMATEX** put() {
    CoTaskMemFree(p_);
    p_ = nullptr;
    return &p_;
  }
  WAVEFORMATEX* get() const { return p_; }

 private:
  WAVEFORMATEX* p_ = nullptr;
};

// Error codes used when a device is missing or unplugged.
constexpr HRESULT kErrDeviceNotFound = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
constexpr HRESULT kErrDeviceNotActive = HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED);

HRESULT createEnumerator(ComPtr<IMMDeviceEnumerator>& out);

// Active playback devices, in Windows' order, with the default one flagged.
HRESULT listOutputDevices(IMMDeviceEnumerator* en, std::vector<DeviceInfo>& out);

// ID of the default playback device ("" if there is none).
std::wstring defaultOutputId(IMMDeviceEnumerator* en);

std::wstring deviceName(IMMDevice* dev);

// Looks a device up by ID; fails unless it is present and enabled.
HRESULT getActiveDevice(IMMDeviceEnumerator* en, const std::wstring& id, ComPtr<IMMDevice>& out);

bool parseFormat(const WAVEFORMATEX* wf, StreamFormat& out);
std::wstring describeFormat(const StreamFormat& f);

// A short explanation of an audio error, suitable for showing to the user.
std::wstring describeError(HRESULT hr);
bool isDeviceMissing(HRESULT hr);

// Seconds on the performance counter clock.
double nowSeconds();

}  // namespace sas
