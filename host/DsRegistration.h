#pragma once
//
// Windows 10 virtual camera registration. Windows 10 has no IMFVirtualCamera, so
// a slot's camera is a DirectShow source filter (in PSCam4Win.dll) listed in the
// "Video Input Device" category for as long as a physical camera occupies the
// slot -- the same lifecycle the MF path gives MFCreateVirtualCamera.
//
// Needs elevation (HKLM) and COM initialised on the calling thread.
//
namespace dsregistration {

// True when this OS has the Media Foundation virtual camera API, so the DirectShow
// path is not needed.
bool HasMediaFoundationVirtualCamera();

// Lists slot `slot` as a capture device named `friendlyName`. Replaces any earlier
// listing of the same slot. Returns an HRESULT.
long Register(int slot, const wchar_t* friendlyName);

// Removes slot `slot` from the device list. Safe when not listed.
void Unregister(int slot);

} // namespace dsregistration
