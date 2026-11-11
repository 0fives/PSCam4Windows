#pragma once
//
// Identity and registry layout of the DirectShow flavour of the virtual camera
// (the Windows 10 path -- Windows 11 uses the Media Foundation virtual camera).
//
// The DirectShow source filters live in PSCam4Win.dll alongside the MF media
// sources. They are separate COM classes, one per slot, so each slot is its own
// capture device in an application's device list. Unlike the MF path they are
// loaded INTO the consuming application's process (there is no Frame Server in
// between), so the DLL runs with that application's token -- see ControlBus.h
// for the ACL consequence.
//
#include <guiddef.h>

#include "VCamGuids.h"   // kVCamCount

// CLSID_PS3EyeDsCams[i] / kDsClsidStrings[i]: DirectShow filter for slot i.
inline constexpr GUID CLSID_PS3EyeDsCams[kVCamCount] = {
    { 0x7a1d4c30, 0x5e92, 0x4b6f, { 0x8c, 0x1a, 0x93, 0x0d, 0xe4, 0x62, 0x7f, 0xa0 } },
    { 0x7a1d4c31, 0x5e92, 0x4b6f, { 0x8c, 0x1a, 0x93, 0x0d, 0xe4, 0x62, 0x7f, 0xa0 } },
    { 0x7a1d4c32, 0x5e92, 0x4b6f, { 0x8c, 0x1a, 0x93, 0x0d, 0xe4, 0x62, 0x7f, 0xa0 } },
    { 0x7a1d4c33, 0x5e92, 0x4b6f, { 0x8c, 0x1a, 0x93, 0x0d, 0xe4, 0x62, 0x7f, 0xa0 } },
    { 0x7a1d4c34, 0x5e92, 0x4b6f, { 0x8c, 0x1a, 0x93, 0x0d, 0xe4, 0x62, 0x7f, 0xa0 } },
    { 0x7a1d4c35, 0x5e92, 0x4b6f, { 0x8c, 0x1a, 0x93, 0x0d, 0xe4, 0x62, 0x7f, 0xa0 } },
    { 0x7a1d4c36, 0x5e92, 0x4b6f, { 0x8c, 0x1a, 0x93, 0x0d, 0xe4, 0x62, 0x7f, 0xa0 } },
    { 0x7a1d4c37, 0x5e92, 0x4b6f, { 0x8c, 0x1a, 0x93, 0x0d, 0xe4, 0x62, 0x7f, 0xa0 } }
};

inline constexpr const wchar_t* kDsClsidStrings[kVCamCount] = {
    L"{7A1D4C30-5E92-4B6F-8C1A-930DE4627FA0}",
    L"{7A1D4C31-5E92-4B6F-8C1A-930DE4627FA0}",
    L"{7A1D4C32-5E92-4B6F-8C1A-930DE4627FA0}",
    L"{7A1D4C33-5E92-4B6F-8C1A-930DE4627FA0}",
    L"{7A1D4C34-5E92-4B6F-8C1A-930DE4627FA0}",
    L"{7A1D4C35-5E92-4B6F-8C1A-930DE4627FA0}",
    L"{7A1D4C36-5E92-4B6F-8C1A-930DE4627FA0}",
    L"{7A1D4C37-5E92-4B6F-8C1A-930DE4627FA0}"
};

// CLSID_VideoInputDeviceCategory, spelled out so this header needs no DirectShow
// includes: the key under which capture devices are listed for applications.
inline constexpr const wchar_t* kVideoInputCategoryClsid =
    L"{860BB310-5D01-11D0-BD3B-00A0C911CE86}";
