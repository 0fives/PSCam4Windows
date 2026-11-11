#pragma once
//
// DirectShow flavour of the PSCam4Win virtual camera -- the Windows 10 path.
//
// Windows 10 has no Media Foundation virtual camera (IMFVirtualCamera is 22000+),
// so the camera is presented as a classic DirectShow video capture source filter
// instead: one COM class per slot, listed in the "Video Input Device" category by
// the tray while a physical camera occupies the slot.
//
// Differences from the MF path that matter:
//   * The filter is loaded INTO the consuming application (OBS, Zoom, a browser's
//     capture process...), not into the Frame Server. It reads the same FrameBus
//     and keeps the same ControlBus keepalive, but does so as the interactive
//     user -- see controlbus::kSddlInteractiveUser.
//   * DirectShow only: Media Foundation / UWP / Store apps will not list it, and
//     it must match the application's bitness (this DLL is x64).
//   * Output is YUY2 (zero-copy from the bus) and RGB24.
//
// The filter is implemented directly against the COM interfaces rather than the
// DirectShow base-class library, which no longer ships with the Windows SDK.
//
#include <windows.h>

// DLL plumbing (dllmain.cpp) calls these.
HRESULT DsGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv, bool* handled);
