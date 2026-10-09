// engine.h - Mirrors whatever one playback device is playing onto other devices.
#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sas {

struct EngineConfig {
  bool enabled = true;
  std::wstring sourceId;                // "" = follow the Windows default output device
  std::vector<std::wstring> targetIds;  // devices that should also play the sound
  double marginMs = 5.0;                // safety buffer beyond the structural minimum
};

enum class TargetState {
  Idle,         // mirroring is off, or there is no source to copy from
  Starting,     // opening / buffering
  Playing,
  IsSource,     // this device is the source: it already plays the sound itself
  Unavailable,  // not connected or disabled
  Error,        // could not be opened; retried periodically
};

struct TargetStatus {
  std::wstring id;
  TargetState state = TargetState::Idle;
  long error = 0;           // HRESULT for Unavailable/Error
  double latencyMs = 0;     // how far behind the source this device plays
  double driftPpm = 0;      // learned clock difference to the source
  double adjustPpm = 0;     // current resampling correction
  double marginMs = 0;      // current safety buffer (grows after dropouts)
  uint32_t underruns = 0;
  uint32_t resyncs = 0;
  std::wstring format;
};

struct EngineStatus {
  bool enabled = false;
  bool capturing = false;
  std::wstring sourceId;     // the device being copied (resolved if following the default)
  long sourceError = 0;      // HRESULT when the source could not be opened
  std::wstring sourceFormat;
  bool captureEvents = false;
  uint64_t capturedFrames = 0;
  std::vector<TargetStatus> targets;  // in configuration order
};

class Engine {
 public:
  Engine();
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  // Starts the audio thread. When the set of devices changes (plugged in, removed,
  // renamed, new default), `notifyMsg` is posted to `notifyWnd`.
  void start(HWND notifyWnd = nullptr, UINT notifyMsg = 0);
  void stop();

  void setConfig(const EngineConfig& config);
  EngineStatus status() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace sas
