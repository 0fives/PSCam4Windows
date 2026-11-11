//
// DirectShow source filter for the Windows 10 virtual camera. See DsCamera.h.
//
#include "DsCamera.h"

#include <dshow.h>
#include <atomic>
#include <memory>
#include <new>
#include <vector>

#include "../common/DsGuids.h"
#include "../common/FrameBus.h"
#include "../common/ControlBus.h"
#include "../common/Yuv.h"

void DllAddRef();
void DllRelease();

namespace
{

// ---- constants not worth a ksmedia.h include -----------------------------------

// AMPROPSETID_Pin / AMPROPERTY_PIN_CATEGORY / PIN_CATEGORY_CAPTURE: how an
// application learns this pin is a capture pin rather than a preview pin.
const GUID kPropSetPin = { 0x9b00f101, 0x1567, 0x11d1, { 0xb3, 0xf1, 0x00, 0xaa, 0x00, 0x37, 0x61, 0xc5 } };
const GUID kPinCategoryCaptureDs = { 0xfb6c4281, 0x0353, 0x11d1, { 0x90, 0x5f, 0x00, 0x00, 0xc0, 0xcc, 0x16, 0xba } };
constexpr DWORD kPropPinCategory = 0;
constexpr DWORD kSupportGet = 1;   // KSPROPERTY_SUPPORT_GET

constexpr wchar_t kPinName[] = L"Capture";

constexpr uint32_t kFallbackWidth  = 640;
constexpr uint32_t kFallbackHeight = 480;
constexpr uint32_t kFallbackFps    = 30;

constexpr DWORD kFccYuy2 = 0x32595559;   // 'YUY2'

// ---- small helpers -------------------------------------------------------------

class CsLock
{
public:
    explicit CsLock(CRITICAL_SECTION& cs) : _cs(cs) { EnterCriticalSection(&_cs); }
    ~CsLock() { LeaveCriticalSection(&_cs); }
    CsLock(const CsLock&) = delete;
    CsLock& operator=(const CsLock&) = delete;
private:
    CRITICAL_SECTION& _cs;
};

template <class T>
void SafeRelease(T*& p)
{
    if (p) { p->Release(); p = nullptr; }
}

enum class Fmt { Yuy2, Rgb24 };

struct TypeEntry
{
    Fmt      fmt;
    uint32_t w, h, fps;
};

inline const GUID& SubtypeOf(Fmt f) { return f == Fmt::Yuy2 ? MEDIASUBTYPE_YUY2 : MEDIASUBTYPE_RGB24; }
inline uint32_t RgbStride(uint32_t w) { return (w * 3 + 3) & ~3u; }
inline uint32_t SampleBytes(Fmt f, uint32_t w, uint32_t h)
{
    return f == Fmt::Yuy2 ? w * h * 2 : RgbStride(w) * h;
}

void FreeMediaType(AM_MEDIA_TYPE& mt)
{
    if (mt.cbFormat && mt.pbFormat)
        CoTaskMemFree(mt.pbFormat);
    mt.cbFormat = 0;
    mt.pbFormat = nullptr;
    if (mt.pUnk)
    {
        mt.pUnk->Release();
        mt.pUnk = nullptr;
    }
}

void DeleteMediaType(AM_MEDIA_TYPE* mt)
{
    if (!mt)
        return;
    FreeMediaType(*mt);
    CoTaskMemFree(mt);
}

// Deep copy; dst must not own anything (it is overwritten).
HRESULT CopyMediaType(AM_MEDIA_TYPE* dst, const AM_MEDIA_TYPE* src)
{
    *dst = *src;
    dst->pUnk = nullptr;
    dst->pbFormat = nullptr;
    dst->cbFormat = 0;
    if (src->cbFormat && src->pbFormat)
    {
        dst->pbFormat = static_cast<BYTE*>(CoTaskMemAlloc(src->cbFormat));
        if (!dst->pbFormat)
            return E_OUTOFMEMORY;
        memcpy(dst->pbFormat, src->pbFormat, src->cbFormat);
        dst->cbFormat = src->cbFormat;
    }
    return S_OK;
}

AM_MEDIA_TYPE* CloneMediaType(const AM_MEDIA_TYPE& src)
{
    auto* mt = static_cast<AM_MEDIA_TYPE*>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
    if (!mt)
        return nullptr;
    if (FAILED(CopyMediaType(mt, &src)))
    {
        CoTaskMemFree(mt);
        return nullptr;
    }
    return mt;
}

// Fills `mt` (owning a CoTaskMem VIDEOINFOHEADER) for one entry.
bool BuildMediaType(const TypeEntry& e, AM_MEDIA_TYPE& mt)
{
    memset(&mt, 0, sizeof(mt));
    auto* vih = static_cast<VIDEOINFOHEADER*>(CoTaskMemAlloc(sizeof(VIDEOINFOHEADER)));
    if (!vih)
        return false;
    memset(vih, 0, sizeof(*vih));

    const bool yuy2 = (e.fmt == Fmt::Yuy2);
    const uint32_t bpp = yuy2 ? 16 : 24;
    const uint32_t image = SampleBytes(e.fmt, e.w, e.h);

    // (Assigned directly: SetRect would pull user32 into a DLL loaded by other apps.)
    vih->rcSource = { 0, 0, static_cast<LONG>(e.w), static_cast<LONG>(e.h) };
    vih->rcTarget = vih->rcSource;
    vih->dwBitRate = static_cast<DWORD>(static_cast<uint64_t>(image) * 8 * e.fps);
    vih->AvgTimePerFrame = e.fps ? 10000000LL / e.fps : 0;
    vih->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    vih->bmiHeader.biWidth = static_cast<LONG>(e.w);
    vih->bmiHeader.biHeight = static_cast<LONG>(e.h);     // positive: bottom-up RGB, normal YUV
    vih->bmiHeader.biPlanes = 1;
    vih->bmiHeader.biBitCount = static_cast<WORD>(bpp);
    vih->bmiHeader.biCompression = yuy2 ? kFccYuy2 : BI_RGB;
    vih->bmiHeader.biSizeImage = image;

    mt.majortype = MEDIATYPE_Video;
    mt.subtype = SubtypeOf(e.fmt);
    mt.bFixedSizeSamples = TRUE;
    mt.bTemporalCompression = FALSE;
    mt.lSampleSize = image;
    mt.formattype = FORMAT_VideoInfo;
    mt.cbFormat = sizeof(VIDEOINFOHEADER);
    mt.pbFormat = reinterpret_cast<BYTE*>(vih);
    return true;
}

// Reads back (format, w, h, fps) from a type an application handed us. Only
// VIDEOINFOHEADER YUY2/RGB24 types of our own shape are understood.
bool ParseMediaType(const AM_MEDIA_TYPE* pmt, TypeEntry& out, bool* topDown = nullptr)
{
    if (!pmt || pmt->majortype != MEDIATYPE_Video || pmt->formattype != FORMAT_VideoInfo ||
        !pmt->pbFormat || pmt->cbFormat < sizeof(VIDEOINFOHEADER))
        return false;
    const auto* vih = reinterpret_cast<const VIDEOINFOHEADER*>(pmt->pbFormat);
    const BITMAPINFOHEADER& bih = vih->bmiHeader;
    if (bih.biWidth <= 0 || bih.biHeight == 0 || bih.biPlanes != 1)
        return false;

    if (pmt->subtype == MEDIASUBTYPE_YUY2)
    {
        if (bih.biBitCount != 16 || bih.biCompression != kFccYuy2)
            return false;
        out.fmt = Fmt::Yuy2;
    }
    else if (pmt->subtype == MEDIASUBTYPE_RGB24)
    {
        if (bih.biBitCount != 24 || bih.biCompression != BI_RGB)
            return false;
        out.fmt = Fmt::Rgb24;
    }
    else
        return false;

    out.w = static_cast<uint32_t>(bih.biWidth);
    out.h = static_cast<uint32_t>(bih.biHeight < 0 ? -bih.biHeight : bih.biHeight);
    out.fps = vih->AvgTimePerFrame > 0
        ? static_cast<uint32_t>((10000000LL + vih->AvgTimePerFrame / 2) / vih->AvgTimePerFrame)
        : 0;
    if (topDown)
        *topDown = bih.biHeight < 0;
    return true;
}

bool IsPartialType(const AM_MEDIA_TYPE* pmt)
{
    return !pmt || pmt->majortype == GUID_NULL || pmt->formattype == GUID_NULL || !pmt->pbFormat;
}

// ---- the media type list -------------------------------------------------------
//
// What the camera can deliver RIGHT NOW: the capability block the tray publishes,
// filtered by what the bus frame can be turned into (ScalePlanFor -- the same rule
// the MF path applies) and capped at the frame rate the bus is actually running.
// With no tray (bus absent) a single fallback type keeps the device connectable;
// it shows black until the tray is up.

std::vector<TypeEntry> BuildTypeList(int slot)
{
    std::vector<TypeEntry> out;

    framebus::Reader reader;
    framebus::HotHeader hdr{};
    framebus::ColdBlock cold{};
    const bool haveBus = reader.TryOpen(slot);
    const bool haveFormat = haveBus && reader.ReadFormat(hdr);
    const bool haveCold = haveFormat && reader.ReadColdBlock(cold);

    if (!haveFormat || !haveCold)
    {
        out.push_back({ Fmt::Yuy2,  kFallbackWidth, kFallbackHeight, kFallbackFps });
        out.push_back({ Fmt::Rgb24, kFallbackWidth, kFallbackHeight, kFallbackFps });
        return out;
    }

    const uint32_t busFps = hdr.fpsNum / hdr.fpsDen ? hdr.fpsNum / hdr.fpsDen : 1;

    auto add = [&](uint32_t w, uint32_t h, uint32_t fps)
    {
        for (Fmt f : { Fmt::Yuy2, Fmt::Rgb24 })
        {
            bool dup = false;
            for (const TypeEntry& e : out)
                dup = dup || (e.fmt == f && e.w == w && e.h == h && e.fps == fps);
            if (!dup)
                out.push_back({ f, w, h, fps });
        }
    };

    // The bus's own mode first: it is the cheapest to deliver and what an
    // application that takes the default gets.
    add(hdr.width, hdr.height, busFps);
    for (uint32_t i = 0; i < cold.modeCount; ++i)
    {
        const framebus::ColdMode& m = cold.modes[i];
        if (yuv::ScalePlanFor(hdr.width, hdr.height, m.width, m.height) == yuv::ScalePlan::Unsupported)
            continue;
        if (m.fps > busFps)      // cannot deliver faster than the camera is running
            continue;
        add(m.width, m.height, m.fps);
    }
    return out;
}

// ---- conversion ----------------------------------------------------------------

inline uint8_t Clamp8(int v) { return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v)); }

// YUY2 (BT.601, limited range) -> BGR24, written bottom-up (a classic DIB) unless
// topDown is set.
void Yuy2ToRgb24(const uint8_t* src, uint8_t* dst, uint32_t w, uint32_t h, bool topDown)
{
    const uint32_t dstStride = RgbStride(w);
    for (uint32_t y = 0; y < h; ++y)
    {
        const uint8_t* s = src + static_cast<size_t>(y) * w * 2;
        uint8_t* d = dst + static_cast<size_t>(topDown ? y : (h - 1 - y)) * dstStride;
        for (uint32_t x = 0; x < w; x += 2, s += 4, d += 6)
        {
            const int u = s[1] - 128, v = s[3] - 128;
            const int rv = 409 * v + 128, guv = -100 * u - 208 * v + 128, bu = 516 * u + 128;
            const int c0 = 298 * (s[0] - 16), c1 = 298 * (s[2] - 16);
            d[0] = Clamp8((c0 + bu) >> 8);  d[1] = Clamp8((c0 + guv) >> 8);  d[2] = Clamp8((c0 + rv) >> 8);
            d[3] = Clamp8((c1 + bu) >> 8);  d[4] = Clamp8((c1 + guv) >> 8);  d[5] = Clamp8((c1 + rv) >> 8);
        }
    }
}

// ---- enumerators ---------------------------------------------------------------

class EnumMediaTypesImpl final : public IEnumMediaTypes
{
public:
    explicit EnumMediaTypesImpl(std::vector<TypeEntry> types, size_t pos = 0)
        : _types(std::move(types)), _pos(pos) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IEnumMediaTypes)
        {
            *ppv = static_cast<IEnumMediaTypes*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&_ref)); }
    STDMETHODIMP_(ULONG) Release() override
    {
        const LONG r = InterlockedDecrement(&_ref);
        if (r == 0) delete this;
        return static_cast<ULONG>(r);
    }

    STDMETHODIMP Next(ULONG count, AM_MEDIA_TYPE** out, ULONG* fetched) override
    {
        if (!out || (!fetched && count != 1)) return E_POINTER;
        ULONG n = 0;
        while (n < count && _pos < _types.size())
        {
            AM_MEDIA_TYPE tmp;
            if (!BuildMediaType(_types[_pos], tmp))
                break;
            AM_MEDIA_TYPE* mt = CloneMediaType(tmp);
            FreeMediaType(tmp);
            if (!mt)
                break;
            out[n++] = mt;
            ++_pos;
        }
        if (fetched) *fetched = n;
        return n == count ? S_OK : S_FALSE;
    }
    STDMETHODIMP Skip(ULONG count) override
    {
        _pos += count;
        if (_pos > _types.size()) { _pos = _types.size(); return S_FALSE; }
        return S_OK;
    }
    STDMETHODIMP Reset() override { _pos = 0; return S_OK; }
    STDMETHODIMP Clone(IEnumMediaTypes** out) override
    {
        if (!out) return E_POINTER;
        *out = new (std::nothrow) EnumMediaTypesImpl(_types, _pos);
        return *out ? S_OK : E_OUTOFMEMORY;
    }

private:
    LONG _ref = 1;
    std::vector<TypeEntry> _types;
    size_t _pos;
};

class DsFilter;

class EnumPinsImpl final : public IEnumPins
{
public:
    EnumPinsImpl(IPin* pin, size_t pos = 0) : _pin(pin), _pos(pos) { _pin->AddRef(); }
    ~EnumPinsImpl() { _pin->Release(); }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IEnumPins)
        {
            *ppv = static_cast<IEnumPins*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&_ref)); }
    STDMETHODIMP_(ULONG) Release() override
    {
        const LONG r = InterlockedDecrement(&_ref);
        if (r == 0) delete this;
        return static_cast<ULONG>(r);
    }

    STDMETHODIMP Next(ULONG count, IPin** out, ULONG* fetched) override
    {
        if (!out || (!fetched && count != 1)) return E_POINTER;
        ULONG n = 0;
        if (count > 0 && _pos == 0)
        {
            _pin->AddRef();
            out[n++] = _pin;
            _pos = 1;
        }
        if (fetched) *fetched = n;
        return n == count ? S_OK : S_FALSE;
    }
    STDMETHODIMP Skip(ULONG count) override
    {
        if (_pos + count > 1) { _pos = 1; return S_FALSE; }
        _pos += count;
        return S_OK;
    }
    STDMETHODIMP Reset() override { _pos = 0; return S_OK; }
    STDMETHODIMP Clone(IEnumPins** out) override
    {
        if (!out) return E_POINTER;
        *out = new (std::nothrow) EnumPinsImpl(_pin, _pos);
        return *out ? S_OK : E_OUTOFMEMORY;
    }

private:
    LONG _ref = 1;
    IPin* _pin;
    size_t _pos;
};

// ---- the pin -------------------------------------------------------------------
//
// The pin's lifetime is its filter's: AddRef/Release forward to the filter, which
// owns the pin as a member (the usual DirectShow arrangement).

class DsPin final : public IPin, public IAMStreamConfig, public IKsPropertySet
{
public:
    DsPin(DsFilter* filter, int slot);
    ~DsPin();

    // IUnknown (forwarded to the owning filter)
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IPin
    STDMETHODIMP Connect(IPin* receivePin, const AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP ReceiveConnection(IPin*, const AM_MEDIA_TYPE*) override { return E_UNEXPECTED; }
    STDMETHODIMP Disconnect() override;
    STDMETHODIMP ConnectedTo(IPin** pin) override;
    STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP QueryPinInfo(PIN_INFO* info) override;
    STDMETHODIMP QueryDirection(PIN_DIRECTION* dir) override;
    STDMETHODIMP QueryId(LPWSTR* id) override;
    STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP EnumMediaTypes(IEnumMediaTypes** out) override;
    STDMETHODIMP QueryInternalConnections(IPin**, ULONG*) override { return E_NOTIMPL; }
    STDMETHODIMP EndOfStream() override { return E_UNEXPECTED; }
    STDMETHODIMP BeginFlush() override { return E_UNEXPECTED; }
    STDMETHODIMP EndFlush() override { return E_UNEXPECTED; }
    STDMETHODIMP NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) override { return S_OK; }

    // IAMStreamConfig
    STDMETHODIMP SetFormat(AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP GetFormat(AM_MEDIA_TYPE** ppmt) override;
    STDMETHODIMP GetNumberOfCapabilities(int* count, int* size) override;
    STDMETHODIMP GetStreamCaps(int index, AM_MEDIA_TYPE** ppmt, BYTE* scc) override;

    // IKsPropertySet
    STDMETHODIMP Set(REFGUID, DWORD, LPVOID, DWORD, LPVOID, DWORD) override { return E_NOTIMPL; }
    STDMETHODIMP Get(REFGUID set, DWORD id, LPVOID instance, DWORD instanceBytes,
                     LPVOID data, DWORD dataBytes, DWORD* returned) override;
    STDMETHODIMP QuerySupported(REFGUID set, DWORD id, DWORD* support) override;

    // Driven by the filter (its lock held by the caller for Activate).
    HRESULT Activate();       // Stopped -> Paused: commit the allocator, start streaming
    void    Deactivate();     // -> Stopped: stop streaming (blocks until the thread is gone)
    bool    Connected() const { return _connected != nullptr; }
    void    NoteRun();        // Run(): restart the timestamp base

private:
    struct StreamParams
    {
        TypeEntry type{};
        bool      topDown = false;
        uint32_t  sampleBytes = 0;
    };

    void RefreshTypes();
    bool Matches(const TypeEntry& want, TypeEntry* found) const;
    HRESULT TryConnectWith(IPin* receivePin, const AM_MEDIA_TYPE& mt);
    HRESULT DecideAllocator(uint32_t sampleBytes);
    void BreakConnection();

    static DWORD WINAPI ThreadThunk(LPVOID self);
    void StreamLoop();
    bool DeliverFrame(const StreamParams& sp, const uint8_t* yuy2, bool discontinuity,
                      LONGLONG startTime, LONGLONG endTime);

    DsFilter* _filter;
    int       _slot;

    std::vector<TypeEntry> _types;                  // under the filter lock
    bool                   _havePreferred = false;
    TypeEntry              _preferred{};

    IPin*          _connected = nullptr;
    IMemInputPin*  _memInput = nullptr;
    IMemAllocator* _alloc = nullptr;
    AM_MEDIA_TYPE  _mt{};
    StreamParams   _sp{};

    HANDLE _thread = nullptr;
    HANDLE _stopEvt = nullptr;
    volatile LONG64 _runQpc = 0;                    // QPC at Run(); 0 while merely paused
};

// ---- the filter ----------------------------------------------------------------

class DsFilter final : public IBaseFilter
{
public:
    static HRESULT Create(int slot, REFIID riid, void** ppv)
    {
        DsFilter* f = new (std::nothrow) DsFilter(slot);
        if (!f)
            return E_OUTOFMEMORY;
        const HRESULT hr = f->QueryInterface(riid, ppv);
        f->Release();
        return hr;
    }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_IPersist || riid == IID_IMediaFilter || riid == IID_IBaseFilter)
            *ppv = static_cast<IBaseFilter*>(this);
        else
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&_ref)); }
    STDMETHODIMP_(ULONG) Release() override
    {
        const LONG r = InterlockedDecrement(&_ref);
        if (r == 0)
            delete this;
        return static_cast<ULONG>(r);
    }

    // IPersist
    STDMETHODIMP GetClassID(CLSID* id) override
    {
        if (!id) return E_POINTER;
        *id = CLSID_PS3EyeDsCams[_slot];
        return S_OK;
    }

    // IMediaFilter
    STDMETHODIMP Stop() override
    {
        {
            CsLock lock(_cs);
            if (_state == State_Stopped)
                return S_OK;
            _state = State_Stopped;
        }
        // Outside the lock: joining the streaming thread must not hold anything a
        // downstream filter's Receive could be waiting on.
        _pin.Deactivate();
        return S_OK;
    }
    STDMETHODIMP Pause() override
    {
        CsLock lock(_cs);
        if (_state == State_Stopped)
        {
            const HRESULT hr = _pin.Activate();
            if (FAILED(hr))
                return hr;
        }
        _state = State_Paused;
        return S_OK;
    }
    STDMETHODIMP Run(REFERENCE_TIME) override
    {
        CsLock lock(_cs);
        if (_state == State_Running)
            return S_OK;
        if (_state == State_Stopped)
        {
            const HRESULT hr = _pin.Activate();
            if (FAILED(hr))
                return hr;
        }
        _pin.NoteRun();
        _state = State_Running;
        return S_OK;
    }
    STDMETHODIMP GetState(DWORD, FILTER_STATE* state) override
    {
        if (!state) return E_POINTER;
        CsLock lock(_cs);
        *state = _state;
        // A live source cannot cue: Paused never produces a preroll guarantee.
        return _state == State_Paused ? VFW_S_CANT_CUE : S_OK;
    }
    STDMETHODIMP SetSyncSource(IReferenceClock* clock) override
    {
        CsLock lock(_cs);
        if (clock) clock->AddRef();
        SafeRelease(_clock);
        _clock = clock;
        return S_OK;
    }
    STDMETHODIMP GetSyncSource(IReferenceClock** clock) override
    {
        if (!clock) return E_POINTER;
        CsLock lock(_cs);
        *clock = _clock;
        if (_clock) _clock->AddRef();
        return S_OK;
    }

    // IBaseFilter
    STDMETHODIMP EnumPins(IEnumPins** out) override
    {
        if (!out) return E_POINTER;
        *out = new (std::nothrow) EnumPinsImpl(static_cast<IPin*>(&_pin));
        return *out ? S_OK : E_OUTOFMEMORY;
    }
    STDMETHODIMP FindPin(LPCWSTR id, IPin** pin) override
    {
        if (!pin) return E_POINTER;
        if (id && _wcsicmp(id, kPinName) == 0)
        {
            *pin = static_cast<IPin*>(&_pin);
            (*pin)->AddRef();
            return S_OK;
        }
        *pin = nullptr;
        return VFW_E_NOT_FOUND;
    }
    STDMETHODIMP QueryFilterInfo(FILTER_INFO* info) override
    {
        if (!info) return E_POINTER;
        CsLock lock(_cs);
        wcscpy_s(info->achName, _name);
        info->pGraph = _graph;
        if (_graph) _graph->AddRef();
        return S_OK;
    }
    STDMETHODIMP JoinFilterGraph(IFilterGraph* graph, LPCWSTR name) override
    {
        CsLock lock(_cs);
        _graph = graph;          // weak, per the DirectShow contract
        if (name) wcsncpy_s(_name, name, _TRUNCATE);
        else _name[0] = 0;
        return S_OK;
    }
    STDMETHODIMP QueryVendorInfo(LPWSTR*) override { return E_NOTIMPL; }

    // For the pin.
    CRITICAL_SECTION& Lock() { return _cs; }
    int               Slot() const { return _slot; }
    FILTER_STATE      State() const { return _state; }
    IFilterGraph*     Graph() const { return _graph; }

private:
    explicit DsFilter(int slot) : _pin(this, slot), _slot(slot)
    {
        InitializeCriticalSection(&_cs);
        wcscpy_s(_name, L"PSCam4Win Camera");
        DllAddRef();
    }
    ~DsFilter()
    {
        _pin.Deactivate();
        SafeRelease(_clock);
        DeleteCriticalSection(&_cs);
        DllRelease();
    }

    LONG              _ref = 1;
    CRITICAL_SECTION  _cs;
    DsPin             _pin;
    int               _slot;
    FILTER_STATE      _state = State_Stopped;
    IFilterGraph*     _graph = nullptr;
    IReferenceClock*  _clock = nullptr;
    wchar_t           _name[128] = {};
};

// ============================================================================
// DsPin
// ============================================================================

DsPin::DsPin(DsFilter* filter, int slot) : _filter(filter), _slot(slot)
{
    _stopEvt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    RefreshTypes();
}

DsPin::~DsPin()
{
    // The owning filter has already stopped streaming (DsFilter::~DsFilter) and its
    // lock no longer exists, so only the connection state is released here.
    BreakConnection();
    if (_stopEvt)
        CloseHandle(_stopEvt);
}

STDMETHODIMP DsPin::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (riid == IID_IUnknown || riid == IID_IPin)
        *ppv = static_cast<IPin*>(this);
    else if (riid == IID_IAMStreamConfig)
        *ppv = static_cast<IAMStreamConfig*>(this);
    else if (riid == IID_IKsPropertySet)
        *ppv = static_cast<IKsPropertySet*>(this);
    else
        return E_NOINTERFACE;
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) DsPin::AddRef()  { return _filter->AddRef(); }
STDMETHODIMP_(ULONG) DsPin::Release() { return _filter->Release(); }

void DsPin::RefreshTypes()
{
    _types = BuildTypeList(_slot);
}

// Finds the advertised entry equal to `want` in format and size. A requested frame
// rate of 0 matches any; otherwise the nearest advertised rate wins.
bool DsPin::Matches(const TypeEntry& want, TypeEntry* found) const
{
    const TypeEntry* best = nullptr;
    for (const TypeEntry& e : _types)
    {
        if (e.fmt != want.fmt || e.w != want.w || e.h != want.h)
            continue;
        if (!best)
        {
            best = &e;
            continue;
        }
        const auto dist = [&](const TypeEntry& t) {
            return t.fps > want.fps ? t.fps - want.fps : want.fps - t.fps;
        };
        if (want.fps && dist(e) < dist(*best))
            best = &e;
    }
    if (!best)
        return false;
    if (found)
        *found = *best;
    return true;
}

STDMETHODIMP DsPin::Connect(IPin* receivePin, const AM_MEDIA_TYPE* pmt)
{
    if (!receivePin)
        return E_POINTER;
    CsLock lock(_filter->Lock());
    if (_connected)
        return VFW_E_ALREADY_CONNECTED;
    if (_filter->State() != State_Stopped)
        return VFW_E_NOT_STOPPED;

    RefreshTypes();

    HRESULT last = VFW_E_NO_ACCEPTABLE_TYPES;

    if (pmt && !IsPartialType(pmt))
    {
        TypeEntry want{};
        if (!ParseMediaType(pmt, want) || !Matches(want, nullptr))
            return VFW_E_TYPE_NOT_ACCEPTED;
        return TryConnectWith(receivePin, *pmt);
    }

    // Preferred (SetFormat) type first, then everything advertised, restricted to
    // the major/sub type the caller named if it named one.
    std::vector<TypeEntry> order;
    if (_havePreferred)
        order.push_back(_preferred);
    for (const TypeEntry& e : _types)
        order.push_back(e);

    for (const TypeEntry& e : order)
    {
        if (pmt && pmt->majortype != GUID_NULL && pmt->majortype != MEDIATYPE_Video)
            break;
        if (pmt && pmt->subtype != GUID_NULL && pmt->subtype != SubtypeOf(e.fmt))
            continue;
        AM_MEDIA_TYPE mt;
        if (!BuildMediaType(e, mt))
            return E_OUTOFMEMORY;
        last = TryConnectWith(receivePin, mt);
        FreeMediaType(mt);
        if (SUCCEEDED(last))
            return last;
    }
    return FAILED(last) ? last : VFW_E_NO_ACCEPTABLE_TYPES;
}

HRESULT DsPin::TryConnectWith(IPin* receivePin, const AM_MEDIA_TYPE& mt)
{
    TypeEntry chosen{};
    bool topDown = false;
    if (!ParseMediaType(&mt, chosen, &topDown))
        return VFW_E_TYPE_NOT_ACCEPTED;

    HRESULT hr = receivePin->ReceiveConnection(static_cast<IPin*>(this), &mt);
    if (FAILED(hr))
        return hr;

    receivePin->AddRef();
    _connected = receivePin;
    if (FAILED(CopyMediaType(&_mt, &mt)))
    {
        receivePin->Disconnect();
        BreakConnection();
        return E_OUTOFMEMORY;
    }
    _sp.type = chosen;
    _sp.topDown = topDown;
    _sp.sampleBytes = SampleBytes(chosen.fmt, chosen.w, chosen.h);

    hr = DecideAllocator(_sp.sampleBytes);
    if (FAILED(hr))
    {
        receivePin->Disconnect();
        BreakConnection();
        return hr;
    }
    return S_OK;
}

HRESULT DsPin::DecideAllocator(uint32_t sampleBytes)
{
    IMemInputPin* input = nullptr;
    HRESULT hr = _connected->QueryInterface(IID_IMemInputPin, reinterpret_cast<void**>(&input));
    if (FAILED(hr))
        return hr;

    IMemAllocator* alloc = nullptr;
    hr = input->GetAllocator(&alloc);
    if (FAILED(hr) || !alloc)
    {
        alloc = nullptr;
        hr = CoCreateInstance(CLSID_MemoryAllocator, nullptr, CLSCTX_INPROC_SERVER,
                              IID_IMemAllocator, reinterpret_cast<void**>(&alloc));
        if (FAILED(hr))
        {
            input->Release();
            return hr;
        }
    }

    ALLOCATOR_PROPERTIES req{};
    input->GetAllocatorRequirements(&req);          // optional; failure leaves req zeroed

    ALLOCATOR_PROPERTIES want{};
    want.cBuffers = req.cBuffers > 4 ? req.cBuffers : 4;
    want.cbBuffer = static_cast<long>(sampleBytes) > req.cbBuffer ? static_cast<long>(sampleBytes) : req.cbBuffer;
    want.cbAlign  = req.cbAlign > 0 ? req.cbAlign : 1;
    want.cbPrefix = req.cbPrefix;

    ALLOCATOR_PROPERTIES actual{};
    hr = alloc->SetProperties(&want, &actual);
    if (SUCCEEDED(hr) && actual.cbBuffer < static_cast<long>(sampleBytes))
        hr = E_FAIL;
    if (SUCCEEDED(hr))
        hr = input->NotifyAllocator(alloc, FALSE);
    if (FAILED(hr))
    {
        alloc->Release();
        input->Release();
        return hr;
    }

    _alloc = alloc;
    _memInput = input;
    return S_OK;
}

void DsPin::BreakConnection()
{
    SafeRelease(_alloc);
    SafeRelease(_memInput);
    SafeRelease(_connected);
    FreeMediaType(_mt);
    memset(&_mt, 0, sizeof(_mt));
}

STDMETHODIMP DsPin::Disconnect()
{
    CsLock lock(_filter->Lock());
    if (_filter->State() != State_Stopped || _thread)
        return VFW_E_NOT_STOPPED;
    if (!_connected)
        return S_FALSE;
    BreakConnection();
    return S_OK;
}

STDMETHODIMP DsPin::ConnectedTo(IPin** pin)
{
    if (!pin) return E_POINTER;
    CsLock lock(_filter->Lock());
    *pin = _connected;
    if (!_connected)
        return VFW_E_NOT_CONNECTED;
    _connected->AddRef();
    return S_OK;
}

STDMETHODIMP DsPin::ConnectionMediaType(AM_MEDIA_TYPE* pmt)
{
    if (!pmt) return E_POINTER;
    CsLock lock(_filter->Lock());
    if (!_connected)
    {
        memset(pmt, 0, sizeof(*pmt));
        return VFW_E_NOT_CONNECTED;
    }
    return CopyMediaType(pmt, &_mt);
}

STDMETHODIMP DsPin::QueryPinInfo(PIN_INFO* info)
{
    if (!info) return E_POINTER;
    info->pFilter = static_cast<IBaseFilter*>(_filter);
    info->pFilter->AddRef();
    info->dir = PINDIR_OUTPUT;
    wcscpy_s(info->achName, kPinName);
    return S_OK;
}

STDMETHODIMP DsPin::QueryDirection(PIN_DIRECTION* dir)
{
    if (!dir) return E_POINTER;
    *dir = PINDIR_OUTPUT;
    return S_OK;
}

STDMETHODIMP DsPin::QueryId(LPWSTR* id)
{
    if (!id) return E_POINTER;
    *id = static_cast<LPWSTR>(CoTaskMemAlloc(sizeof(kPinName)));
    if (!*id) return E_OUTOFMEMORY;
    memcpy(*id, kPinName, sizeof(kPinName));
    return S_OK;
}

STDMETHODIMP DsPin::QueryAccept(const AM_MEDIA_TYPE* pmt)
{
    CsLock lock(_filter->Lock());
    TypeEntry want{};
    return (ParseMediaType(pmt, want) && Matches(want, nullptr)) ? S_OK : S_FALSE;
}

STDMETHODIMP DsPin::EnumMediaTypes(IEnumMediaTypes** out)
{
    if (!out) return E_POINTER;
    CsLock lock(_filter->Lock());
    if (!_connected)
        RefreshTypes();
    std::vector<TypeEntry> list;
    if (_havePreferred)
        list.push_back(_preferred);
    for (const TypeEntry& e : _types)
        list.push_back(e);
    *out = new (std::nothrow) EnumMediaTypesImpl(std::move(list));
    return *out ? S_OK : E_OUTOFMEMORY;
}

// ---- IAMStreamConfig -----------------------------------------------------------

STDMETHODIMP DsPin::SetFormat(AM_MEDIA_TYPE* pmt)
{
    if (!pmt)
        return E_POINTER;
    IFilterGraph* graph = nullptr;
    {
        CsLock lock(_filter->Lock());
        if (_filter->State() != State_Stopped)
            return VFW_E_NOT_STOPPED;
        RefreshTypes();
        TypeEntry want{}, found{};
        if (!ParseMediaType(pmt, want) || !Matches(want, &found))
            return VFW_E_INVALIDMEDIATYPE;
        _preferred = found;
        _havePreferred = true;
        if (!_connected)
            return S_OK;
        graph = _filter->Graph();
        if (graph) graph->AddRef();
    }
    // Already connected: the graph has to rebuild the connection with the new type.
    if (!graph)
        return VFW_E_NOT_STOPPED;
    const HRESULT hr = graph->Reconnect(static_cast<IPin*>(this));
    graph->Release();
    return hr;
}

STDMETHODIMP DsPin::GetFormat(AM_MEDIA_TYPE** ppmt)
{
    if (!ppmt) return E_POINTER;
    CsLock lock(_filter->Lock());
    if (_connected)
    {
        *ppmt = CloneMediaType(_mt);
        return *ppmt ? S_OK : E_OUTOFMEMORY;
    }
    RefreshTypes();
    const TypeEntry& e = _havePreferred ? _preferred : _types.front();
    AM_MEDIA_TYPE tmp;
    if (!BuildMediaType(e, tmp))
        return E_OUTOFMEMORY;
    *ppmt = CloneMediaType(tmp);
    FreeMediaType(tmp);
    return *ppmt ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP DsPin::GetNumberOfCapabilities(int* count, int* size)
{
    if (!count || !size) return E_POINTER;
    CsLock lock(_filter->Lock());
    if (!_connected)
        RefreshTypes();
    *count = static_cast<int>(_types.size());
    *size = sizeof(VIDEO_STREAM_CONFIG_CAPS);
    return S_OK;
}

STDMETHODIMP DsPin::GetStreamCaps(int index, AM_MEDIA_TYPE** ppmt, BYTE* scc)
{
    if (!ppmt || !scc) return E_POINTER;
    CsLock lock(_filter->Lock());
    if (index < 0 || static_cast<size_t>(index) >= _types.size())
        return S_FALSE;
    const TypeEntry& e = _types[static_cast<size_t>(index)];

    AM_MEDIA_TYPE tmp;
    if (!BuildMediaType(e, tmp))
        return E_OUTOFMEMORY;
    *ppmt = CloneMediaType(tmp);
    FreeMediaType(tmp);
    if (!*ppmt)
        return E_OUTOFMEMORY;

    auto* caps = reinterpret_cast<VIDEO_STREAM_CONFIG_CAPS*>(scc);
    memset(caps, 0, sizeof(*caps));
    caps->guid = FORMAT_VideoInfo;
    caps->InputSize = { static_cast<LONG>(e.w), static_cast<LONG>(e.h) };
    caps->MinCroppingSize = caps->MaxCroppingSize = caps->InputSize;
    caps->CropGranularityX = static_cast<int>(e.w);
    caps->CropGranularityY = static_cast<int>(e.h);
    caps->MinOutputSize = caps->MaxOutputSize = caps->InputSize;
    const LONGLONG interval = e.fps ? 10000000LL / e.fps : 0;
    caps->MinFrameInterval = caps->MaxFrameInterval = interval;
    const LONG bits = static_cast<LONG>(static_cast<uint64_t>(SampleBytes(e.fmt, e.w, e.h)) * 8 * e.fps);
    caps->MinBitsPerSecond = caps->MaxBitsPerSecond = bits;
    return S_OK;
}

// ---- IKsPropertySet ------------------------------------------------------------

STDMETHODIMP DsPin::Get(REFGUID set, DWORD id, LPVOID, DWORD, LPVOID data, DWORD dataBytes, DWORD* returned)
{
    if (set != kPropSetPin)
        return E_PROP_SET_UNSUPPORTED;
    if (id != kPropPinCategory)
        return E_PROP_ID_UNSUPPORTED;
    if (returned)
        *returned = sizeof(GUID);
    if (!data)
        return S_OK;
    if (dataBytes < sizeof(GUID))
        return E_UNEXPECTED;
    *static_cast<GUID*>(data) = kPinCategoryCaptureDs;
    return S_OK;
}

STDMETHODIMP DsPin::QuerySupported(REFGUID set, DWORD id, DWORD* support)
{
    if (set != kPropSetPin)
        return E_PROP_SET_UNSUPPORTED;
    if (id != kPropPinCategory)
        return E_PROP_ID_UNSUPPORTED;
    if (support)
        *support = kSupportGet;
    return S_OK;
}

// ---- streaming -----------------------------------------------------------------

HRESULT DsPin::Activate()
{
    if (!_connected || _thread)
        return S_OK;
    HRESULT hr = _alloc->Commit();
    if (FAILED(hr))
        return hr;
    ResetEvent(_stopEvt);
    InterlockedExchange64(&_runQpc, 0);
    _thread = CreateThread(nullptr, 0, ThreadThunk, this, 0, nullptr);
    if (!_thread)
    {
        _alloc->Decommit();
        return HRESULT_FROM_WIN32(GetLastError());
    }
    return S_OK;
}

void DsPin::Deactivate()
{
    HANDLE thread = nullptr;
    {
        CsLock lock(_filter->Lock());
        thread = _thread;
    }
    if (!thread)
        return;
    SetEvent(_stopEvt);
    if (_alloc)
        _alloc->Decommit();          // unblocks a GetBuffer waiting for a free sample
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    // Cleared only now, so Disconnect cannot release the allocator under a thread
    // that is still winding down.
    CsLock lock(_filter->Lock());
    _thread = nullptr;
}

void DsPin::NoteRun()
{
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    InterlockedExchange64(&_runQpc, qpc.QuadPart);
}

DWORD WINAPI DsPin::ThreadThunk(LPVOID self)
{
    static_cast<DsPin*>(self)->StreamLoop();
    return 0;
}

bool DsPin::DeliverFrame(const StreamParams& sp, const uint8_t* yuy2, bool discontinuity,
                         LONGLONG startTime, LONGLONG endTime)
{
    IMediaSample* sample = nullptr;
    if (FAILED(_alloc->GetBuffer(&sample, nullptr, nullptr, 0)) || !sample)
        return false;

    BYTE* data = nullptr;
    bool ok = SUCCEEDED(sample->GetPointer(&data)) && data &&
              static_cast<uint32_t>(sample->GetSize()) >= sp.sampleBytes;
    if (ok)
    {
        if (sp.type.fmt == Fmt::Yuy2)
            memcpy(data, yuy2, sp.sampleBytes);
        else
            Yuy2ToRgb24(yuy2, data, sp.type.w, sp.type.h, sp.topDown);
        sample->SetActualDataLength(static_cast<long>(sp.sampleBytes));
        sample->SetTime(&startTime, &endTime);
        sample->SetSyncPoint(TRUE);
        sample->SetPreroll(FALSE);
        sample->SetDiscontinuity(discontinuity ? TRUE : FALSE);
        ok = (_memInput->Receive(sample) == S_OK);   // S_FALSE / error: downstream is done
    }
    sample->Release();
    return ok;
}

void DsPin::StreamLoop()
{
    const StreamParams sp = _sp;                     // immutable while streaming
    const uint32_t outW = sp.type.w, outH = sp.type.h;
    const uint32_t fps = sp.type.fps ? sp.type.fps : kFallbackFps;
    const LONGLONG period = 10000000LL / fps;        // 100 ns units

    framebus::Reader reader;
    controlbus::Pinger ping;
    std::unique_ptr<uint8_t[]> busBuf(new (std::nothrow) uint8_t[framebus::kAbsMaxFrameBytes]);
    std::unique_ptr<uint8_t[]> scaled(new (std::nothrow) uint8_t[framebus::Yuy2Bytes(outW, outH)]);
    std::unique_ptr<uint8_t[]> black(new (std::nothrow) uint8_t[framebus::Yuy2Bytes(outW, outH)]);
    if (!busBuf || !scaled || !black)
        return;
    yuv::FillBlack(black.get(), outW, outH);

    LARGE_INTEGER freq, qpc;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&qpc);
    const LONGLONG threadStart = qpc.QuadPart;

    ULONGLONG nextOpenTick = 0, nextPingTick = 0, lastWakeTick = 0;
    LONG64 lastFrameId = 0;
    LONGLONG lastDeliverQpc = 0, lastEnd = 0, lastRunQpc = 0;
    bool discontinuity = true;

    // How long to wait for the bus's frame event. The event is auto-reset, so with
    // several applications reading one camera only one of them is woken per frame;
    // the others fall through on this timeout and poll, which keeps them live at
    // roughly the frame rate without any reader starving.
    const DWORD waitMs = fps >= 120 ? 2 : (fps >= 50 ? 4 : 10);

    while (WaitForSingleObject(_stopEvt, 0) != WAIT_OBJECT_0)
    {
        const ULONGLONG tick = GetTickCount64();

        // ControlBus: tell the tray a client is consuming, so it wakes the camera
        // and keeps it awake. Stamp before pulsing the wake event.
        if (!ping.IsOpen() && tick >= nextPingTick)
        {
            nextPingTick = tick + 500;
            ping.TryOpen(_slot);
        }
        if (ping.IsOpen())
        {
            ping.Stamp();
            ping.SetConsumerMask(controlbus::kConsumeYUY2);
            if (tick - lastWakeTick >= 1000)
            {
                lastWakeTick = tick;
                ping.SignalWake();
            }
        }

        if (!reader.IsOpen() && tick >= nextOpenTick)
        {
            nextOpenTick = tick + 250;
            reader.TryOpen(_slot);
        }

        const uint8_t* frame = nullptr;
        bool deliver = false;

        if (!reader.IsOpen())
        {
            // No tray / camera yet: keep the graph alive with black at the nominal rate.
            WaitForSingleObject(_stopEvt, static_cast<DWORD>(period / 10000));
            frame = black.get();
            deliver = true;
        }
        else
        {
            reader.WaitFrame(waitMs);
            framebus::HotHeader hdr{};
            if (!reader.ReadFormat(hdr))
            {
                WaitForSingleObject(_stopEvt, waitMs);
                continue;
            }
            const uint32_t busBytes = framebus::Yuy2Bytes(hdr.width, hdr.height);
            const LONG64 id = reader.TryReadNewer(busBuf.get(), busBytes, lastFrameId, hdr.width, hdr.height);
            if (!id)
                continue;                                // nothing new yet
            lastFrameId = id;

            QueryPerformanceCounter(&qpc);
            // Rate limiting for a mode slower than the camera's current one.
            if (lastDeliverQpc &&
                (qpc.QuadPart - lastDeliverQpc) * 10000000LL / freq.QuadPart < period * 9 / 10)
                continue;

            switch (yuv::ScalePlanFor(hdr.width, hdr.height, outW, outH))
            {
            case yuv::ScalePlan::Direct:
                frame = busBuf.get();
                break;
            case yuv::ScalePlan::Upscale2x:
                yuv::UpscaleYuy2_2x(busBuf.get(), scaled.get(), hdr.width, hdr.height);
                frame = scaled.get();
                break;
            case yuv::ScalePlan::Downscale2x:
                yuv::DownscaleYuy2_2x(busBuf.get(), scaled.get(), hdr.width, hdr.height);
                frame = scaled.get();
                break;
            default:
                frame = black.get();                     // size no longer producible: show black
                break;
            }
            deliver = true;
        }

        if (!deliver)
            continue;

        QueryPerformanceCounter(&qpc);
        const LONGLONG run = _runQpc;
        if (run != lastRunQpc)
        {
            lastRunQpc = run;
            discontinuity = true;                        // timestamp base changed
        }
        const LONGLONG base = run ? run : threadStart;
        LONGLONG start = (qpc.QuadPart - base) * 10000000LL / freq.QuadPart;
        if (start < 0) start = 0;
        if (start < lastEnd && !discontinuity) start = lastEnd;
        const LONGLONG end = start + period;

        if (!DeliverFrame(sp, frame, discontinuity, start, end))
            break;                                       // flushing, stopped, or downstream gone

        discontinuity = false;
        lastEnd = end;
        lastDeliverQpc = qpc.QuadPart;
    }

    ping.SetConsumerMask(0);
}

// ============================================================================
// Class factory
// ============================================================================

class DsClassFactory final : public IClassFactory
{
public:
    explicit DsClassFactory(int slot) : _slot(slot) { DllAddRef(); }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_IClassFactory)
            *ppv = static_cast<IClassFactory*>(this);
        else
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&_ref)); }
    STDMETHODIMP_(ULONG) Release() override
    {
        const LONG r = InterlockedDecrement(&_ref);
        if (r == 0)
        {
            delete this;
            DllRelease();
        }
        return static_cast<ULONG>(r);
    }
    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override
    {
        if (outer)
            return CLASS_E_NOAGGREGATION;
        return DsFilter::Create(_slot, riid, ppv);
    }
    STDMETHODIMP LockServer(BOOL lock) override
    {
        if (lock) DllAddRef(); else DllRelease();
        return S_OK;
    }

private:
    LONG _ref = 1;
    int  _slot;
};

} // namespace

HRESULT DsGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv, bool* handled)
{
    *handled = false;
    for (int i = 0; i < kVCamCount; ++i)
    {
        if (rclsid != CLSID_PS3EyeDsCams[i])
            continue;
        *handled = true;
        DsClassFactory* factory = new (std::nothrow) DsClassFactory(i);
        if (!factory)
            return E_OUTOFMEMORY;
        const HRESULT hr = factory->QueryInterface(riid, ppv);
        factory->Release();
        return hr;
    }
    return CLASS_E_CLASSNOTAVAILABLE;
}
