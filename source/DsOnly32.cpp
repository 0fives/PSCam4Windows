//
// PSCam4WinDS32.dll -- the 32-bit build of the DirectShow camera filter.
//
// 32-bit DirectShow applications cannot load the x64 PSCam4Win.dll, so the same
// filter (source/DsCamera.cpp, unchanged) is built for x86 with just the COM entry
// points around it. It has no Media Foundation sources, so it needs no libjpeg
// and no mfplat. Registration lands in the 32-bit registry view automatically
// because regsvr32 (SysWOW64) runs as a 32-bit process.
//
#include <windows.h>
#include "DsCamera.h"
#include "ComRegister.h"
#include "../common/DsGuids.h"
#include "../common/DsCategory.h"

static HMODULE g_module = nullptr;
static volatile LONG g_objectCount = 0;

void DllAddRef()  { InterlockedIncrement(&g_objectCount); }
void DllRelease() { InterlockedDecrement(&g_objectCount); }

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_module = hinst;
        DisableThreadLibraryCalls(hinst);
    }
    return TRUE;
}

STDAPI DllCanUnloadNow()
{
    return g_objectCount == 0 ? S_OK : S_FALSE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv)
{
    bool handled = false;
    return DsGetClassObject(rclsid, riid, ppv, &handled);
}

STDAPI DllRegisterServer()
{
    wchar_t modulePath[MAX_PATH];
    if (GetModuleFileNameW(g_module, modulePath, MAX_PATH) == 0)
        return HRESULT_FROM_WIN32(GetLastError());

    LSTATUS overall = ERROR_SUCCESS;
    for (int i = 0; i < kVCamCount; ++i)
    {
        wchar_t description[128];
        swprintf_s(description, L"PSCam4Win DirectShow Camera #%d (x86)", i);
        const LSTATUS status = RegisterClass32(kDsClsidStrings[i], description, modulePath);
        if (status != ERROR_SUCCESS)
            overall = status;
    }
    return overall == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(overall);
}

STDAPI DllUnregisterServer()
{
    for (int i = 0; i < kVCamCount; ++i)
    {
        dscategory::RemoveSlot(i);      // never leave a listed device with no class behind
        wchar_t keyPath[128];
        swprintf_s(keyPath, L"Software\\Classes\\CLSID\\%s", kDsClsidStrings[i]);
        RegDeleteTreeW(HKEY_LOCAL_MACHINE, keyPath);
    }
    return S_OK;
}
