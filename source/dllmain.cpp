//
// COM plumbing for PSCam4Win.dll: the IMFActivate the Frame Server CoCreates
// (via the CLSID passed to MFCreateVirtualCamera), the class factory, the
// self-registration entry points, and module lifetime tracking.
//
#include "VCamSource.h"
#include "DsCamera.h"
#include "ComRegister.h"
#include "../common/DsGuids.h"
#include "../common/DsCategory.h"
#include <new>

static HMODULE g_module = nullptr;
static volatile LONG g_objectCount = 0;

void DllAddRef()  { InterlockedIncrement(&g_objectCount); }
void DllRelease() { InterlockedDecrement(&g_objectCount); }

// ===========================================================================
// Activator — IMFActivate (which is an IMFAttributes). The Frame Server may
// set configuration attributes on it before calling ActivateObject; we keep
// them in a real attribute store and hand them to the media source.
// ===========================================================================

class Activator final : public IMFActivate
{
public:
    static HRESULT CreateInstance(REFIID riid, void** ppv, int cameraIndex)
    {
        if (!ppv)
            return E_POINTER;
        Activator* activator = new (std::nothrow) Activator(cameraIndex);
        if (!activator)
            return E_OUTOFMEMORY;
        HRESULT hr = activator->Initialize();
        if (SUCCEEDED(hr))
            hr = activator->QueryInterface(riid, ppv);
        activator->Release();
        return hr;
    }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv)
            return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == __uuidof(IMFActivate) ||
            riid == __uuidof(IMFAttributes))
            *ppv = static_cast<IMFActivate*>(this);
        else
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&_refCount); }
    STDMETHODIMP_(ULONG) Release() override
    {
        const ULONG ref = InterlockedDecrement(&_refCount);
        if (ref == 0)
        {
            delete this;
            DllRelease();
        }
        return ref;
    }

    // IMFActivate
    STDMETHODIMP ActivateObject(REFIID riid, void** ppv) override
    {
        if (!ppv)
            return E_POINTER;
        *ppv = nullptr;
        AutoLock lock(_lock);
        ComPtr<MediaSource> source;
        source.Attach(new (std::nothrow) MediaSource(_cameraIndex));
        if (!source)
            return E_OUTOFMEMORY;
        HRESULT hr = source->Initialize(_attributes.Get());
        if (FAILED(hr))
        {
            VCamTrace(L"activator: source init failed 0x%08X", hr);
            return hr;
        }
        hr = source.CopyTo(riid, ppv);
        if (FAILED(hr))
        {
            source->Shutdown();
            return hr;
        }
        // Every activation builds its own source: the Frame Server shuts a
        // source down when its client leaves, and handing that dead object to
        // the next activation would fail every reopen. The activator remembers
        // only the latest one, for ShutdownObject/DetachObject; earlier ones
        // belong to whoever activated them and are shut down by their owners.
        _source = source;
        return S_OK;
    }

    STDMETHODIMP ShutdownObject() override
    {
        AutoLock lock(_lock);
        if (_source)
        {
            _source->Shutdown();
            _source.Reset();
        }
        return S_OK;
    }

    STDMETHODIMP DetachObject() override
    {
        AutoLock lock(_lock);
        _source.Reset();
        return S_OK;
    }

    // IMFAttributes — forward everything to the inner store.
    STDMETHODIMP GetItem(REFGUID k, PROPVARIANT* v) override { return _attributes->GetItem(k, v); }
    STDMETHODIMP GetItemType(REFGUID k, MF_ATTRIBUTE_TYPE* t) override { return _attributes->GetItemType(k, t); }
    STDMETHODIMP CompareItem(REFGUID k, REFPROPVARIANT v, BOOL* r) override { return _attributes->CompareItem(k, v, r); }
    STDMETHODIMP Compare(IMFAttributes* a, MF_ATTRIBUTES_MATCH_TYPE t, BOOL* r) override { return _attributes->Compare(a, t, r); }
    STDMETHODIMP GetUINT32(REFGUID k, UINT32* v) override { return _attributes->GetUINT32(k, v); }
    STDMETHODIMP GetUINT64(REFGUID k, UINT64* v) override { return _attributes->GetUINT64(k, v); }
    STDMETHODIMP GetDouble(REFGUID k, double* v) override { return _attributes->GetDouble(k, v); }
    STDMETHODIMP GetGUID(REFGUID k, GUID* v) override { return _attributes->GetGUID(k, v); }
    STDMETHODIMP GetStringLength(REFGUID k, UINT32* l) override { return _attributes->GetStringLength(k, l); }
    STDMETHODIMP GetString(REFGUID k, LPWSTR v, UINT32 s, UINT32* l) override { return _attributes->GetString(k, v, s, l); }
    STDMETHODIMP GetAllocatedString(REFGUID k, LPWSTR* v, UINT32* l) override { return _attributes->GetAllocatedString(k, v, l); }
    STDMETHODIMP GetBlobSize(REFGUID k, UINT32* s) override { return _attributes->GetBlobSize(k, s); }
    STDMETHODIMP GetBlob(REFGUID k, UINT8* b, UINT32 s, UINT32* bs) override { return _attributes->GetBlob(k, b, s, bs); }
    STDMETHODIMP GetAllocatedBlob(REFGUID k, UINT8** b, UINT32* s) override { return _attributes->GetAllocatedBlob(k, b, s); }
    STDMETHODIMP GetUnknown(REFGUID k, REFIID riid, LPVOID* v) override { return _attributes->GetUnknown(k, riid, v); }
    STDMETHODIMP SetItem(REFGUID k, REFPROPVARIANT v) override { return _attributes->SetItem(k, v); }
    STDMETHODIMP DeleteItem(REFGUID k) override { return _attributes->DeleteItem(k); }
    STDMETHODIMP DeleteAllItems() override { return _attributes->DeleteAllItems(); }
    STDMETHODIMP SetUINT32(REFGUID k, UINT32 v) override { return _attributes->SetUINT32(k, v); }
    STDMETHODIMP SetUINT64(REFGUID k, UINT64 v) override { return _attributes->SetUINT64(k, v); }
    STDMETHODIMP SetDouble(REFGUID k, double v) override { return _attributes->SetDouble(k, v); }
    STDMETHODIMP SetGUID(REFGUID k, REFGUID v) override { return _attributes->SetGUID(k, v); }
    STDMETHODIMP SetString(REFGUID k, LPCWSTR v) override { return _attributes->SetString(k, v); }
    STDMETHODIMP SetBlob(REFGUID k, const UINT8* b, UINT32 s) override { return _attributes->SetBlob(k, b, s); }
    STDMETHODIMP SetUnknown(REFGUID k, IUnknown* u) override { return _attributes->SetUnknown(k, u); }
    STDMETHODIMP LockStore() override { return _attributes->LockStore(); }
    STDMETHODIMP UnlockStore() override { return _attributes->UnlockStore(); }
    STDMETHODIMP GetCount(UINT32* c) override { return _attributes->GetCount(c); }
    STDMETHODIMP GetItemByIndex(UINT32 i, GUID* k, PROPVARIANT* v) override { return _attributes->GetItemByIndex(i, k, v); }
    STDMETHODIMP CopyAllItems(IMFAttributes* dest) override { return _attributes->CopyAllItems(dest); }

private:
    explicit Activator(int cameraIndex) : _cameraIndex(cameraIndex) { DllAddRef(); }
    virtual ~Activator() = default;

    HRESULT Initialize() { return MFCreateAttributes(&_attributes, 4); }

    LONG    _refCount = 1;
    CritSec _lock;
    int     _cameraIndex;
    ComPtr<IMFAttributes> _attributes;
    ComPtr<MediaSource>   _source;      // the most recent activation only
};

// ===========================================================================
// Class factory
// ===========================================================================

class ClassFactory final : public IClassFactory
{
public:
    explicit ClassFactory(int cameraIndex) : _cameraIndex(cameraIndex) { DllAddRef(); }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv)
            return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_IClassFactory)
            *ppv = static_cast<IClassFactory*>(this);
        else
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&_refCount); }
    STDMETHODIMP_(ULONG) Release() override
    {
        const ULONG ref = InterlockedDecrement(&_refCount);
        if (ref == 0)
        {
            delete this;
            DllRelease();
        }
        return ref;
    }

    STDMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv) override
    {
        if (pUnkOuter)
            return CLASS_E_NOAGGREGATION;
        return Activator::CreateInstance(riid, ppv, _cameraIndex);
    }

    STDMETHODIMP LockServer(BOOL fLock) override
    {
        if (fLock) DllAddRef(); else DllRelease();
        return S_OK;
    }

private:
    virtual ~ClassFactory() = default;
    LONG _refCount = 1;
    int  _cameraIndex;
};

// ===========================================================================
// DLL exports
// ===========================================================================

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
    // DirectShow camera classes (Windows 10 path) live in this DLL too.
    bool handledByDs = false;
    const HRESULT dsHr = DsGetClassObject(rclsid, riid, ppv, &handledByDs);
    if (handledByDs)
        return dsHr;

    int cameraIndex = -1;
    for (int i = 0; i < kVCamCount; ++i)
    {
        if (rclsid == CLSID_PS3EyeVCams[i])
        {
            cameraIndex = i;
            break;
        }
    }
    if (cameraIndex == -1)
        return CLASS_E_CLASSNOTAVAILABLE;

    ClassFactory* factory = new (std::nothrow) ClassFactory(cameraIndex);
    if (!factory)
        return E_OUTOFMEMORY;
    HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

// regsvr32 must run elevated.
STDAPI DllRegisterServer()
{
    wchar_t modulePath[MAX_PATH];
    if (GetModuleFileNameW(g_module, modulePath, MAX_PATH) == 0)
        return HRESULT_FROM_WIN32(GetLastError());

    LSTATUS overallStatus = ERROR_SUCCESS;

    for (int i = 0; i < kVCamCount; ++i)
    {
        wchar_t description[128];

        swprintf_s(description, L"PSCam4Win Virtual Camera Media Source #%d", i);
        LSTATUS status = RegisterClass32(kVCamClsidStrings[i], description, modulePath);
        if (status != ERROR_SUCCESS)
            overallStatus = status;

        swprintf_s(description, L"PSCam4Win DirectShow Camera #%d", i);
        status = RegisterClass32(kDsClsidStrings[i], description, modulePath);
        if (status != ERROR_SUCCESS)
            overallStatus = status;
    }
    return overallStatus == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(overallStatus);
}

STDAPI DllUnregisterServer()
{
    // RegDeleteTreeW removes the whole CLSID node regardless of what subkeys
    // a given build version created — no dangling InprocServer32 paths for
    // the Frame Server to trip over after an uninstall.
    for (int i = 0; i < kVCamCount; ++i)
    {
        wchar_t keyPath[128];
        swprintf_s(keyPath, L"Software\\Classes\\CLSID\\%s", kVCamClsidStrings[i]);
        RegDeleteTreeW(HKEY_LOCAL_MACHINE, keyPath);

        // A DirectShow camera that is still listed as a device must not outlive
        // its class registration (applications would list a camera they cannot open).
        dscategory::RemoveSlot(i);
        swprintf_s(keyPath, L"Software\\Classes\\CLSID\\%s", kDsClsidStrings[i]);
        RegDeleteTreeW(HKEY_LOCAL_MACHINE, keyPath);
    }
    return S_OK;
}
