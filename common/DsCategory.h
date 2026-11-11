#pragma once
//
// Registry side of listing a DirectShow filter as a video capture device.
//
// Applications enumerate cameras through the "Video Input Device" category:
//   HKLM\SOFTWARE\Classes\CLSID\{860BB310-...}\Instance\<instance name>
// Each instance key carries CLSID, FriendlyName and FilterData. Creating one is
// done by the tray through IFilterMapper2 (host/DsRegistration.cpp), because that
// is what produces a correct FilterData blob. REMOVING one needs no COM, so it
// lives here, where both the tray and PSCam4Win.dll's DllUnregisterServer can use
// it: instance keys are named after the (changing) friendly name, so a removal
// finds its key by the CLSID value inside it rather than by name.
//
#include <windows.h>
#include <cwchar>

#include "DsGuids.h"

namespace dscategory {

// Deletes every instance of the category whose CLSID value equals `clsid`
// (registry string form). Returns the number of keys removed.
inline int RemoveInstancesOf(const wchar_t* clsid, REGSAM view)
{
    wchar_t path[160];
    swprintf_s(path, L"Software\\Classes\\CLSID\\%s\\Instance", kVideoInputCategoryClsid);

    HKEY instances = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_READ | KEY_WRITE | view, &instances) != ERROR_SUCCESS)
        return 0;

    int removed = 0;
    for (;;)
    {
        // Collect first, delete after: deleting while enumerating shifts indexes.
        wchar_t victim[256] = {};
        bool found = false;
        for (DWORD i = 0; ; ++i)
        {
            wchar_t name[256];
            DWORD nameLen = ARRAYSIZE(name);
            if (RegEnumKeyExW(instances, i, name, &nameLen, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
                break;
            HKEY sub = nullptr;
            if (RegOpenKeyExW(instances, name, 0, KEY_READ, &sub) != ERROR_SUCCESS)
                continue;
            wchar_t value[80] = {};
            DWORD bytes = sizeof(value) - sizeof(wchar_t), type = 0;
            const bool match =
                RegQueryValueExW(sub, L"CLSID", nullptr, &type,
                                 reinterpret_cast<BYTE*>(value), &bytes) == ERROR_SUCCESS &&
                type == REG_SZ && _wcsicmp(value, clsid) == 0;
            RegCloseKey(sub);
            if (match)
            {
                wcscpy_s(victim, name);
                found = true;
                break;
            }
        }
        if (!found)
            break;
        if (RegDeleteTreeW(instances, victim) != ERROR_SUCCESS)
            break;      // cannot make progress; do not spin
        ++removed;
    }
    RegCloseKey(instances);
    return removed;
}

// A 64-bit and a 32-bit application each read their own registry view, so a
// camera is listed in (and must be removed from) both. On a 32-bit-only OS the
// two flags address the one and only view.
inline int RemoveSlot(int slot)
{
    if (slot < 0 || slot >= kVCamCount)
        return 0;
    return RemoveInstancesOf(kDsClsidStrings[slot], KEY_WOW64_64KEY) +
           RemoveInstancesOf(kDsClsidStrings[slot], KEY_WOW64_32KEY);
}

} // namespace dscategory
