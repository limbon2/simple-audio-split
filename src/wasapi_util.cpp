#include <initguid.h>  // defines the PKEY_* and KSDATAFORMAT_* GUIDs used below

#include "wasapi_util.h"

#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <mmreg.h>

#include <cwchar>

namespace sas {

HRESULT createEnumerator(ComPtr<IMMDeviceEnumerator>& out) {
  return CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&out));
}

std::wstring deviceName(IMMDevice* dev) {
  std::wstring name;
  ComPtr<IPropertyStore> props;
  if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
    PROPVARIANT v;
    PropVariantInit(&v);
    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal) name = v.pwszVal;
    PropVariantClear(&v);
  }
  return name.empty() ? L"Unknown device" : name;
}

static std::wstring deviceId(IMMDevice* dev) {
  std::wstring id;
  LPWSTR raw = nullptr;
  if (SUCCEEDED(dev->GetId(&raw)) && raw) {
    id = raw;
    CoTaskMemFree(raw);
  }
  return id;
}

std::wstring defaultOutputId(IMMDeviceEnumerator* en) {
  ComPtr<IMMDevice> dev;
  if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) return L"";
  return deviceId(dev.Get());
}

HRESULT listOutputDevices(IMMDeviceEnumerator* en, std::vector<DeviceInfo>& out) {
  out.clear();
  ComPtr<IMMDeviceCollection> devices;
  HRESULT hr = en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices);
  if (FAILED(hr)) return hr;
  UINT count = 0;
  devices->GetCount(&count);
  const std::wstring def = defaultOutputId(en);
  for (UINT i = 0; i < count; ++i) {
    ComPtr<IMMDevice> dev;
    if (FAILED(devices->Item(i, &dev))) continue;
    DeviceInfo info;
    info.id = deviceId(dev.Get());
    info.name = deviceName(dev.Get());
    info.isDefault = !def.empty() && info.id == def;
    out.push_back(std::move(info));
  }
  return S_OK;
}

HRESULT getActiveDevice(IMMDeviceEnumerator* en, const std::wstring& id, ComPtr<IMMDevice>& out) {
  out.Reset();
  ComPtr<IMMDevice> dev;
  if (FAILED(en->GetDevice(id.c_str(), &dev))) return kErrDeviceNotFound;
  DWORD state = 0;
  if (FAILED(dev->GetState(&state)) || state != DEVICE_STATE_ACTIVE) return kErrDeviceNotActive;
  out = dev;
  return S_OK;
}

bool parseFormat(const WAVEFORMATEX* wf, StreamFormat& out) {
  out = StreamFormat{};
  if (!wf) return false;
  out.rate = wf->nSamplesPerSec;
  out.channels = wf->nChannels;
  out.blockAlign = wf->nBlockAlign;
  WORD tag = wf->wFormatTag;
  if (tag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
    const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
    out.mask = ext->dwChannelMask;
    if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) tag = WAVE_FORMAT_IEEE_FLOAT;
    else if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) tag = WAVE_FORMAT_PCM;
    else tag = 0;
  }
  if (tag == WAVE_FORMAT_IEEE_FLOAT && wf->wBitsPerSample == 32) {
    out.type = SampleType::Float32;
  } else if (tag == WAVE_FORMAT_PCM) {
    // 24-in-32 formats are left-justified, so they read correctly as 32-bit.
    if (wf->wBitsPerSample == 16) out.type = SampleType::Int16;
    else if (wf->wBitsPerSample == 24) out.type = SampleType::Int24;
    else if (wf->wBitsPerSample == 32) out.type = SampleType::Int32;
  }
  return out.type != SampleType::Unsupported && out.channels > 0 && out.rate > 0 &&
         out.blockAlign == out.channels * bytesPerSample(out.type);
}

std::wstring describeFormat(const StreamFormat& f) {
  const wchar_t* type = L"?";
  switch (f.type) {
    case SampleType::Float32: type = L"float32"; break;
    case SampleType::Int16: type = L"int16"; break;
    case SampleType::Int24: type = L"int24"; break;
    case SampleType::Int32: type = L"int32"; break;
    default: break;
  }
  wchar_t buf[96];
  swprintf_s(buf, L"%.0f Hz, %d ch (mask 0x%X), %s", f.rate, f.channels, f.mask, type);
  return buf;
}

bool isDeviceMissing(HRESULT hr) {
  return hr == kErrDeviceNotFound || hr == kErrDeviceNotActive || hr == AUDCLNT_E_DEVICE_INVALIDATED;
}

std::wstring describeError(HRESULT hr) {
  switch (hr) {
    case S_OK: return L"";
    case kErrDeviceNotFound:
    case kErrDeviceNotActive: return L"Not connected";
    case AUDCLNT_E_DEVICE_INVALIDATED: return L"Device was disconnected or reconfigured";
    case AUDCLNT_E_DEVICE_IN_USE: return L"Taken over by another app (exclusive mode)";
    case AUDCLNT_E_SERVICE_NOT_RUNNING: return L"Windows Audio service is not running";
    case AUDCLNT_E_UNSUPPORTED_FORMAT: return L"Unsupported audio format";
    case AUDCLNT_E_ENDPOINT_CREATE_FAILED: return L"Could not open the device";
    case AUDCLNT_E_CPUUSAGE_EXCEEDED: return L"Audio engine overloaded";
    case E_ACCESSDENIED: return L"Access denied";
    case E_OUTOFMEMORY: return L"Out of memory";
    default: {
      wchar_t buf[48];
      swprintf_s(buf, L"Error 0x%08X", static_cast<unsigned>(hr));
      return buf;
    }
  }
}

double nowSeconds() {
  static const double freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return static_cast<double>(f.QuadPart);
  }();
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return static_cast<double>(c.QuadPart) / freq;
}

}  // namespace sas
