#include "DsRegistration.h"

#include <windows.h>
#include <dshow.h>
#include <wrl/client.h>
#include <vector>

#include "../common/DsGuids.h"
#include "../common/DsCategory.h"

using Microsoft::WRL::ComPtr;

namespace dsregistration {

bool HasMediaFoundationVirtualCamera()
{
    const HMODULE module = LoadLibraryW(L"mfsensorgroup.dll");
    return module && GetProcAddress(module, "MFCreateVirtualCamera") != nullptr;
}

namespace
{

// IFilterMapper2 lists the camera in the tray's own (64-bit) registry view. A 32-bit
// application reads the 32-bit view, so when the 32-bit filter DLL is installed
// (its class registered there) copy the instance entry across. The FilterData
// blob is position-independent, so the copy is exact.
void MirrorToWow64(int slot, const wchar_t* friendlyName)
{
    wchar_t classPath[128];
    swprintf_s(classPath, L"Software\\Classes\\CLSID\\%s", kDsClsidStrings[slot]);
    HKEY probe = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, classPath, 0, KEY_READ | KEY_WOW64_32KEY, &probe) != ERROR_SUCCESS)
        return;                                     // no 32-bit DLL installed
    RegCloseKey(probe);

    wchar_t instancePath[320];
    swprintf_s(instancePath, L"Software\\Classes\\CLSID\\%s\\Instance\\%s",
               kVideoInputCategoryClsid, friendlyName);

    HKEY src = nullptr, dst = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, instancePath, 0, KEY_READ | KEY_WOW64_64KEY, &src) != ERROR_SUCCESS)
        return;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, instancePath, 0, nullptr, 0, KEY_WRITE | KEY_WOW64_32KEY,
                        nullptr, &dst, nullptr) == ERROR_SUCCESS)
    {
        for (DWORD i = 0; ; ++i)
        {
            wchar_t name[128];
            DWORD nameLen = ARRAYSIZE(name), type = 0, bytes = 0;
            if (RegEnumValueW(src, i, name, &nameLen, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS)
                break;
            std::vector<BYTE> data(bytes);
            nameLen = ARRAYSIZE(name);
            if (RegEnumValueW(src, i, name, &nameLen, nullptr, &type, data.data(), &bytes) == ERROR_SUCCESS)
                RegSetValueExW(dst, name, 0, type, data.data(), bytes);
        }
        RegCloseKey(dst);
    }
    RegCloseKey(src);
}

} // namespace

long Register(int slot, const wchar_t* friendlyName)
{
    if (slot < 0 || slot >= kVCamCount || !friendlyName || !*friendlyName)
        return E_INVALIDARG;

    // The device list is keyed by instance name, and the name follows the camera
    // that occupies the slot, so drop whatever this slot was listed as before.
    dscategory::RemoveSlot(slot);

    ComPtr<IFilterMapper2> mapper;
    HRESULT hr = CoCreateInstance(CLSID_FilterMapper2, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&mapper));
    if (FAILED(hr))
        return hr;

    // One output pin, video, in the capture pin category. Media types here are
    // only registry metadata for graph building; the live list is the pin's.
    REGPINTYPES pinType = { &MEDIATYPE_Video, &MEDIASUBTYPE_NULL };
    REGFILTERPINS pin = {};
    pin.strName = const_cast<LPWSTR>(L"Capture");
    pin.bRendered = FALSE;
    pin.bOutput = TRUE;
    pin.bZero = FALSE;
    pin.bMany = FALSE;
    pin.clsConnectsToFilter = nullptr;
    pin.strConnectsToPin = nullptr;
    pin.nMediaTypes = 1;
    pin.lpMediaType = &pinType;

    REGFILTER2 rf2 = {};
    rf2.dwVersion = 1;
    rf2.dwMerit = MERIT_DO_NOT_USE;
    rf2.cPins = 1;
    rf2.rgPins = &pin;

    IMoniker* moniker = nullptr;
    hr = mapper->RegisterFilter(CLSID_PS3EyeDsCams[slot], friendlyName, &moniker,
                                &CLSID_VideoInputDeviceCategory, friendlyName, &rf2);
    if (moniker)
        moniker->Release();
    if (SUCCEEDED(hr))
        MirrorToWow64(slot, friendlyName);
    return hr;
}

void Unregister(int slot)
{
    dscategory::RemoveSlot(slot);
}

} // namespace dsregistration
