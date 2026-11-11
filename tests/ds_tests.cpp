// Tests for the DirectShow camera filter (the Windows 10 path).
//
// The filter is compiled into this test and driven through its real COM
// interfaces: a hand-written sink pin plays the part of the application's
// renderer, and a FrameBus writer plays the tray. Kernel object names are
// remapped to Local\ so nothing touches a running installation, and the registry
// and the system device list are never written.
#include <windows.h>
#include <objbase.h>
#include <sddl.h>
#include <dshow.h>
#include <atomic>
#include <mutex>
#include <vector>
#include "TestSupport.h"

static std::wstring TestName(LPCWSTR name)
{ return L"Local\\PSCam4Win.DsTests." + std::to_wstring(GetCurrentProcessId()) + L"." + (name + 7); }
static HANDLE TestCreateMap(HANDLE file, LPSECURITY_ATTRIBUTES sa, DWORD protect, DWORD hi, DWORD lo, LPCWSTR name)
{ return CreateFileMappingW(file, sa, protect, hi, lo, TestName(name).c_str()); }
static HANDLE TestOpenMap(DWORD access, BOOL inherit, LPCWSTR name)
{ return OpenFileMappingW(access, inherit, TestName(name).c_str()); }
static HANDLE TestCreateEvent(LPSECURITY_ATTRIBUTES sa, BOOL manual, BOOL initial, LPCWSTR name)
{ return CreateEventW(sa, manual, initial, name ? TestName(name).c_str() : nullptr); }   // the filter also makes unnamed events
static HANDLE TestOpenEvent(DWORD access, BOOL inherit, LPCWSTR name)
{ return OpenEventW(access, inherit, TestName(name).c_str()); }
#define CreateFileMappingW TestCreateMap
#define OpenFileMappingW TestOpenMap
#define CreateEventW TestCreateEvent
#define OpenEventW TestOpenEvent
#include "../common/FrameBus.h"
#include "../common/ControlBus.h"
#include "../source/DsCamera.cpp"
#undef CreateFileMappingW
#undef OpenFileMappingW
#undef CreateEventW
#undef OpenEventW

static LONG g_objects = 0;
void DllAddRef()  { InterlockedIncrement(&g_objects); }
void DllRelease() { InterlockedDecrement(&g_objects); }

// ---- a sink standing in for the application's renderer --------------------------

class Sink final : public IPin, public IMemInputPin
{
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_IPin) *ppv = static_cast<IPin*>(this);
        else if (riid == IID_IMemInputPin) *ppv = static_cast<IMemInputPin*>(this);
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }     // lives on the stack
    STDMETHODIMP_(ULONG) Release() override { return 1; }

    // IPin
    STDMETHODIMP Connect(IPin*, const AM_MEDIA_TYPE*) override { return E_UNEXPECTED; }
    STDMETHODIMP ReceiveConnection(IPin* pin, const AM_MEDIA_TYPE* mt) override
    {
        if (!mt || mt->majortype != MEDIATYPE_Video) return VFW_E_TYPE_NOT_ACCEPTED;
        connectedType = *reinterpret_cast<const VIDEOINFOHEADER*>(mt->pbFormat);
        subtype = mt->subtype;
        upstream = pin;
        return S_OK;
    }
    STDMETHODIMP Disconnect() override { upstream = nullptr; return S_OK; }
    STDMETHODIMP ConnectedTo(IPin** p) override { *p = upstream; return upstream ? S_OK : VFW_E_NOT_CONNECTED; }
    STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE*) override { return E_NOTIMPL; }
    STDMETHODIMP QueryPinInfo(PIN_INFO* i) override { memset(i, 0, sizeof(*i)); i->dir = PINDIR_INPUT; return S_OK; }
    STDMETHODIMP QueryDirection(PIN_DIRECTION* d) override { *d = PINDIR_INPUT; return S_OK; }
    STDMETHODIMP QueryId(LPWSTR*) override { return E_NOTIMPL; }
    STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE*) override { return S_OK; }
    STDMETHODIMP EnumMediaTypes(IEnumMediaTypes**) override { return E_NOTIMPL; }
    STDMETHODIMP QueryInternalConnections(IPin**, ULONG*) override { return E_NOTIMPL; }
    STDMETHODIMP EndOfStream() override { return S_OK; }
    STDMETHODIMP BeginFlush() override { return S_OK; }
    STDMETHODIMP EndFlush() override { return S_OK; }
    STDMETHODIMP NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) override { return S_OK; }

    // IMemInputPin: no allocator of our own, so the filter must supply one.
    STDMETHODIMP GetAllocator(IMemAllocator**) override { return VFW_E_NO_ALLOCATOR; }
    STDMETHODIMP NotifyAllocator(IMemAllocator*, BOOL) override { notified = true; return S_OK; }
    STDMETHODIMP GetAllocatorRequirements(ALLOCATOR_PROPERTIES*) override { return E_NOTIMPL; }
    STDMETHODIMP Receive(IMediaSample* s) override
    {
        BYTE* p = nullptr;
        s->GetPointer(&p);
        const long n = s->GetActualDataLength();
        REFERENCE_TIME t0 = 0, t1 = 0;
        const HRESULT timeHr = s->GetTime(&t0, &t1);
        std::lock_guard<std::mutex> g(lock);
        last.assign(p, p + n);
        times.push_back(SUCCEEDED(timeHr) ? t0 : -1);
        if (s->IsDiscontinuity() == S_OK) ++discontinuities;
        ++count;
        return S_OK;
    }
    STDMETHODIMP ReceiveMultiple(IMediaSample**, long, long*) override { return E_NOTIMPL; }
    STDMETHODIMP ReceiveCanBlock() override { return S_FALSE; }

    bool WaitFor(int frames, DWORD ms)
    {
        for (DWORD waited = 0; waited < ms; waited += 5)
        {
            if (count >= frames) return true;
            Sleep(5);
        }
        return count >= frames;
    }
    std::vector<uint8_t> Last()
    {
        std::lock_guard<std::mutex> g(lock);
        return last;
    }

    VIDEOINFOHEADER connectedType{};
    GUID subtype{};
    IPin* upstream = nullptr;
    bool notified = false;
    std::atomic<int> count{ 0 };
    int discontinuities = 0;
    std::vector<LONGLONG> times;
    std::mutex lock;
    std::vector<uint8_t> last;
};

// ---- helpers --------------------------------------------------------------------

static void PublishSolid(framebus::Writer& bus, uint32_t w, uint32_t h, uint8_t y, uint8_t u, uint8_t v)
{
    std::vector<uint8_t> frame(framebus::Yuy2Bytes(w, h));
    for (size_t i = 0; i < frame.size(); i += 4)
    {
        frame[i] = y; frame[i + 1] = u; frame[i + 2] = y; frame[i + 3] = v;
    }
    bus.Publish(frame.data(), static_cast<uint32_t>(frame.size()));
}

// Publishes `frame` repeatedly until the sink has `want` frames or time runs out.
template <class F>
static bool PumpUntil(Sink& sink, int want, F publish)
{
    for (int i = 0; i < 600 && sink.count < want; ++i)
    {
        publish();
        Sleep(8);
    }
    return sink.count >= want;
}

static IBaseFilter* MakeFilter(int slot)
{
    IClassFactory* factory = nullptr;
    bool handled = false;
    if (FAILED(DsGetClassObject(CLSID_PS3EyeDsCams[slot], IID_IClassFactory,
                                reinterpret_cast<void**>(&factory), &handled)) || !handled)
        return nullptr;
    IBaseFilter* filter = nullptr;
    factory->CreateInstance(nullptr, IID_IBaseFilter, reinterpret_cast<void**>(&filter));
    factory->Release();
    return filter;
}

static bool SolidMatches(const std::vector<uint8_t>& frame, uint8_t y, uint8_t u, uint8_t v)
{
    if (frame.empty() || frame.size() % 4) return false;
    for (size_t i = 0; i < frame.size(); i += 4)
        if (frame[i] != y || frame[i + 1] != u || frame[i + 2] != y || frame[i + 3] != v)
            return false;
    return true;
}

static int FindType(IAMStreamConfig* cfg, const GUID& subtype, LONG w, LONG h)
{
    int count = 0, size = 0;
    cfg->GetNumberOfCapabilities(&count, &size);
    for (int i = 0; i < count; ++i)
    {
        AM_MEDIA_TYPE* mt = nullptr;
        VIDEO_STREAM_CONFIG_CAPS caps{};
        if (cfg->GetStreamCaps(i, &mt, reinterpret_cast<BYTE*>(&caps)) != S_OK)
            continue;
        const auto* vih = reinterpret_cast<const VIDEOINFOHEADER*>(mt->pbFormat);
        const bool hit = mt->subtype == subtype && vih->bmiHeader.biWidth == w && vih->bmiHeader.biHeight == h;
        DeleteMediaType(mt);
        if (hit) return i;
    }
    return -1;
}

// Connects the filter's pin to `sink` (optionally after choosing a format), runs
// the graph states, and returns the pin for the caller to stop.
static bool Start(IBaseFilter* filter, Sink& sink, IPin** pinOut)
{
    if (*pinOut) { (*pinOut)->Release(); *pinOut = nullptr; }   // the caller's earlier reference
    IPin* pin = nullptr;
    if (FAILED(filter->FindPin(L"Capture", &pin))) return false;
    if (FAILED(pin->Connect(&sink, nullptr))) { pin->Release(); return false; }
    if (FAILED(filter->Pause()) || FAILED(filter->Run(0))) { pin->Release(); return false; }
    *pinOut = pin;
    return true;
}

static void Stop(IBaseFilter* filter, IPin* pin)
{
    filter->Stop();
    pin->Disconnect();
    pin->Release();
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);   // a crash must not swallow the results so far
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    constexpr int kSlot = 3;       // slot with a bus
    constexpr int kEmptySlot = 6;  // slot with no bus at all

    framebus::Writer bus;
    // Sized like the tray sizes every slot (kAbsMaxFrameBytes), so the 1280x800 mode
    // below is a valid entry in the capability block rather than invalidating it.
    Check(bus.Create(kSlot, 640, 480, 30, 1, framebus::kAbsMaxFrameBytes, 0), "bus created");
    const framebus::ColdMode modes[] = {
        { 640, 480, 30, framebus::kFmtYUY2 | framebus::kFmtNV12 },
        { 320, 240, 30, framebus::kFmtYUY2 },
        { 640, 480, 60, framebus::kFmtYUY2 },     // faster than the bus: must not be advertised
        { 1280, 800, 30, framebus::kFmtYUY2 },    // not producible from 640x480: must not be advertised
    };
    bus.WriteColdBlock(framebus::kFmtYUY2, framebus::kFmtYUY2, modes, 4);

    controlbus::Host control;
    Check(control.Create(kSlot, true), "control bus created with interactive-user access");
    if (testFailures) return 1;

    // ---- class plumbing -----------------------------------------------------------
    {
        bool handled = false;
        IClassFactory* f = nullptr;
        Check(DsGetClassObject(CLSID_PS3EyeVCams[0], IID_IClassFactory, reinterpret_cast<void**>(&f), &handled) ==
                  CLASS_E_CLASSNOTAVAILABLE && !handled,
              "MF media source CLSIDs are not claimed by the DirectShow factory");
        bool allSlots = true;
        for (int i = 0; i < kVCamCount; ++i)
        {
            IBaseFilter* flt = MakeFilter(i);
            CLSID id{};
            allSlots = allSlots && flt && SUCCEEDED(flt->GetClassID(&id)) && id == CLSID_PS3EyeDsCams[i];
            if (flt) flt->Release();
        }
        Check(allSlots, "every slot's CLSID creates a filter that reports that CLSID");
        Check(g_objects == 0, "no objects leaked by creating and releasing filters");
    }

    IBaseFilter* filter = MakeFilter(kSlot);
    Check(filter != nullptr, "filter created for the bus slot");
    if (!filter) return 1;

    // ---- pins and capabilities ----------------------------------------------------
    IPin* pin = nullptr;
    Check(SUCCEEDED(filter->FindPin(L"Capture", &pin)) && pin, "output pin found by name");
    {
        IEnumPins* en = nullptr;
        IPin* first = nullptr;
        ULONG got = 0;
        Check(SUCCEEDED(filter->EnumPins(&en)) && en->Next(1, &first, &got) == S_OK && got == 1 &&
                  en->Next(1, &first, &got) == S_FALSE,
              "exactly one pin is enumerated");
        if (first) first->Release();
        if (en) en->Release();
    }
    PIN_DIRECTION dir{};
    pin->QueryDirection(&dir);
    Check(dir == PINDIR_OUTPUT, "the pin is an output pin");

    IKsPropertySet* props = nullptr;
    GUID category{};
    DWORD returned = 0;
    Check(SUCCEEDED(pin->QueryInterface(IID_IKsPropertySet, reinterpret_cast<void**>(&props))) &&
              props->Get(kPropSetPin, kPropPinCategory, nullptr, 0, &category, sizeof(category), &returned) == S_OK &&
              category == kPinCategoryCaptureDs,
          "pin reports the capture pin category");
    if (props) props->Release();

    IAMStreamConfig* cfg = nullptr;
    Check(SUCCEEDED(pin->QueryInterface(IID_IAMStreamConfig, reinterpret_cast<void**>(&cfg))), "IAMStreamConfig available");
    if (!cfg) return 1;

    const int yuy640 = FindType(cfg, MEDIASUBTYPE_YUY2, 640, 480);
    const int rgb640 = FindType(cfg, MEDIASUBTYPE_RGB24, 640, 480);
    const int yuy320 = FindType(cfg, MEDIASUBTYPE_YUY2, 320, 240);
    Check(yuy640 == 0, "the bus's own mode is the first (default) type");
    Check(rgb640 > 0 && yuy320 > 0, "RGB24 and the scalable 320x240 mode are advertised");
    Check(FindType(cfg, MEDIASUBTYPE_YUY2, 1280, 800) < 0, "a size the bus frame cannot be turned into is not advertised");
    {
        int count = 0, size = 0;
        cfg->GetNumberOfCapabilities(&count, &size);
        bool fastAdvertised = false;
        for (int i = 0; i < count; ++i)
        {
            AM_MEDIA_TYPE* mt = nullptr;
            VIDEO_STREAM_CONFIG_CAPS caps{};
            if (cfg->GetStreamCaps(i, &mt, reinterpret_cast<BYTE*>(&caps)) == S_OK)
            {
                fastAdvertised = fastAdvertised || reinterpret_cast<const VIDEOINFOHEADER*>(mt->pbFormat)->AvgTimePerFrame < 333333;
                DeleteMediaType(mt);
            }
        }
        Check(!fastAdvertised, "no mode faster than the camera is currently running is advertised");
        Check(size == sizeof(VIDEO_STREAM_CONFIG_CAPS), "capability record size reported");
    }
    {
        AM_MEDIA_TYPE* mt = nullptr;
        VIDEO_STREAM_CONFIG_CAPS caps{};
        cfg->GetStreamCaps(yuy640, &mt, reinterpret_cast<BYTE*>(&caps));
        Check(mt && mt->lSampleSize == 640 * 480 * 2 && caps.MinOutputSize.cx == 640 &&
                  caps.MaxFrameInterval == 333333,
              "YUY2 640x480@30 type and caps are consistent");
        DeleteMediaType(mt);
    }

    // ---- YUY2 streaming -----------------------------------------------------------
    {
        Sink sink;
        Check(Start(filter, sink, &pin), "connected, paused and run");
        Check(sink.notified, "an allocator was negotiated with the sink");
        Check(PumpUntil(sink, 12, [&] { PublishSolid(bus, 640, 480, 100, 120, 140); }), "frames are delivered");
        Check(SolidMatches(sink.Last(), 100, 120, 140) && sink.Last().size() == 640 * 480 * 2,
              "YUY2 frames arrive untouched at the negotiated size");
        bool monotonic = true;
        {
            std::lock_guard<std::mutex> g(sink.lock);
            for (size_t i = 1; i < sink.times.size(); ++i)
                monotonic = monotonic && sink.times[i] >= sink.times[i - 1] && sink.times[i] >= 0;
        }
        Check(monotonic, "sample timestamps are present and never go backwards");
        Check(control.ActivityAgeMs() < 2000, "the keepalive is stamped while streaming");
        Check(control.ConsumerMask() == controlbus::kConsumeYUY2, "the consumer format is published");
        Stop(filter, pin);
        Sleep(30);
        Check(control.ConsumerMask() == 0, "the consumer format is cleared when streaming stops");
        const int frozen = sink.count;
        Sleep(60);
        Check(sink.count == frozen, "no frames are delivered after Stop");
        pin = nullptr;
        filter->FindPin(L"Capture", &pin);
    }

    // ---- RGB24 conversion and orientation ------------------------------------------
    {
        AM_MEDIA_TYPE* mt = nullptr;
        VIDEO_STREAM_CONFIG_CAPS caps{};
        cfg->GetStreamCaps(rgb640, &mt, reinterpret_cast<BYTE*>(&caps));
        Check(mt && cfg->SetFormat(mt) == S_OK, "SetFormat accepts an advertised RGB24 type");
        DeleteMediaType(mt);

        Sink sink;
        Check(Start(filter, sink, &pin), "connected with the chosen format");
        Check(sink.subtype == MEDIASUBTYPE_RGB24, "the preferred format is what was connected");

        // Top half white, bottom half black: a bottom-up DIB stores black rows first.
        auto halves = [&] {
            std::vector<uint8_t> f(framebus::Yuy2Bytes(640, 480));
            for (uint32_t y = 0; y < 480; ++y)
                for (uint32_t x = 0; x < 640; x += 2)
                {
                    uint8_t* p = &f[(y * 640 + x) * 2];
                    const uint8_t lum = y < 240 ? 235 : 16;
                    p[0] = lum; p[1] = 128; p[2] = lum; p[3] = 128;
                }
            bus.Publish(f.data(), static_cast<uint32_t>(f.size()));
        };
        Check(PumpUntil(sink, 6, halves), "RGB24 frames are delivered");
        const auto frame = sink.Last();
        const size_t stride = 640 * 3;
        Check(frame.size() == stride * 480, "RGB24 sample has the expected size");
        const bool bottomUp = frame.size() == stride * 480 && frame[0] < 8 && frame[stride * 479] > 247;
        Check(bottomUp, "RGB24 is bottom-up: black rows first, white rows last");
        Stop(filter, pin);
        pin = nullptr;
        filter->FindPin(L"Capture", &pin);
    }

    // ---- 2:1 downscale -------------------------------------------------------------
    {
        // Choose 320x240 from a 640x480 bus: the DLL halves the frame.
        Check(FindType(cfg, MEDIASUBTYPE_YUY2, 320, 240) >= 0, "320x240 is advertised from a 640x480 bus");
        AM_MEDIA_TYPE* mt = nullptr;
        VIDEO_STREAM_CONFIG_CAPS caps{};
        cfg->GetStreamCaps(FindType(cfg, MEDIASUBTYPE_YUY2, 320, 240), &mt, reinterpret_cast<BYTE*>(&caps));
        Check(mt && cfg->SetFormat(mt) == S_OK, "SetFormat accepts the scaled type");
        DeleteMediaType(mt);

        Sink sink;
        Check(Start(filter, sink, &pin), "connected at 320x240");
        Check(PumpUntil(sink, 6, [&] { PublishSolid(bus, 640, 480, 60, 100, 200); }), "scaled frames are delivered");
        Check(sink.Last().size() == 320 * 240 * 2 && SolidMatches(sink.Last(), 60, 100, 200),
              "a solid frame survives the 2:1 downscale exactly");
        Stop(filter, pin);
        pin = nullptr;
        filter->FindPin(L"Capture", &pin);
    }

    // ---- rejecting what was never advertised ---------------------------------------
    {
        AM_MEDIA_TYPE bogus{};
        TypeEntry odd{ Fmt::Yuy2, 1280, 800, 30 };
        BuildMediaType(odd, bogus);
        Check(cfg->SetFormat(&bogus) == VFW_E_INVALIDMEDIATYPE, "SetFormat rejects a type that is not advertised");
        Check(pin->QueryAccept(&bogus) == S_FALSE, "QueryAccept rejects it too");
        Sink sink;
        Check(pin->Connect(&sink, &bogus) == VFW_E_TYPE_NOT_ACCEPTED, "Connect with it fails");
        FreeMediaType(bogus);
    }

    // ---- a state change while connected is refused / disconnect ordering ------------
    {
        Sink sink;
        IPin* p2 = nullptr;
        filter->FindPin(L"Capture", &p2);
        Check(SUCCEEDED(p2->Connect(&sink, nullptr)), "reconnect works after earlier sessions");
        Check(p2->Connect(&sink, nullptr) == VFW_E_ALREADY_CONNECTED, "a second Connect is refused");
        filter->Pause();
        Check(p2->Disconnect() == VFW_E_NOT_STOPPED, "Disconnect while paused is refused");
        FILTER_STATE st{};
        Check(filter->GetState(0, &st) == VFW_S_CANT_CUE && st == State_Paused, "a paused live source reports CANT_CUE");
        filter->Stop();
        Check(p2->Disconnect() == S_OK, "Disconnect after Stop works");
        p2->Release();
    }

    cfg->Release();
    pin->Release();
    filter->Release();

    // ---- no tray running: still connectable, shows black ----------------------------
    {
        IBaseFilter* f = MakeFilter(kEmptySlot);
        Sink sink;
        IPin* p = nullptr;
        Check(f && Start(f, sink, &p), "a slot with no bus still connects (fallback type)");
        if (f && p)
        {
            Check(sink.WaitFor(3, 2000), "black frames keep the graph alive with no tray");
            Check(sink.Last().size() == 640 * 480 * 2 && SolidMatches(sink.Last(), 16, 128, 128),
                  "the fallback frames are black");
            Stop(f, p);
        }
        if (f) f->Release();
    }

    Check(g_objects == 0, "no objects leaked");
    printf("%s (%d failure(s))\n", testFailures ? "FAILED" : "ALL PASSED", testFailures);
    CoUninitialize();
    return testFailures ? 1 : 0;
}
