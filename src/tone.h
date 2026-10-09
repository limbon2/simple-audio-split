// tone.h - A short chime played on one device, to tell identical headsets apart.
#pragma once

#include <windows.h>

#include <string>

namespace sas {

// Plays the chime (about 0.8 s) on the given device. Blocks; call it from a worker thread.
HRESULT playTestSound(const std::wstring& deviceId);

}  // namespace sas
