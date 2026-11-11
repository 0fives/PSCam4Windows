#pragma once
//
// Writes one in-process COM class under HKLM\Software\Classes\CLSID so the process
// that will CoCreate it can find it: the Frame Server service (which never reads
// HKCU) for the MF media sources, any DirectShow application for the camera
// filters. Shared by the x64 PSCam4Win.dll and the x86 PSCam4WinDS32.dll; each
// writes into the registry view of its own bitness.
//
#include <windows.h>
#include <cwchar>

inline LSTATUS RegisterClass32(const wchar_t* clsid, const wchar_t* description,
                               const wchar_t* modulePath)
{
    wchar_t keyPath[128];
    swprintf_s(keyPath, L"Software\\Classes\\CLSID\\%s", clsid);

    HKEY key = nullptr;
    LSTATUS overall = RegCreateKeyExW(HKEY_LOCAL_MACHINE, keyPath, 0, nullptr, 0,
                                      KEY_WRITE, nullptr, &key, nullptr);
    if (overall != ERROR_SUCCESS)
        return overall;

    LSTATUS status = RegSetValueExW(key, nullptr, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(description),
                       static_cast<DWORD>((wcslen(description) + 1) * sizeof(wchar_t)));
    if (status != ERROR_SUCCESS)
        overall = status;

    HKEY inproc = nullptr;
    status = RegCreateKeyExW(key, L"InprocServer32", 0, nullptr, 0,
                             KEY_WRITE, nullptr, &inproc, nullptr);
    if (status == ERROR_SUCCESS)
    {
        status = RegSetValueExW(inproc, nullptr, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(modulePath),
                       static_cast<DWORD>((wcslen(modulePath) + 1) * sizeof(wchar_t)));
        if (status != ERROR_SUCCESS)
            overall = status;
        const wchar_t threading[] = L"Both";
        status = RegSetValueExW(inproc, L"ThreadingModel", 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(threading), sizeof(threading));
        if (status != ERROR_SUCCESS)
            overall = status;
        RegCloseKey(inproc);
    }
    else
    {
        overall = status;
    }
    RegCloseKey(key);
    return overall;
}
