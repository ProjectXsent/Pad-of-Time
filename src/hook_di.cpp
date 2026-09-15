// =============================================================================
//  hook_di.cpp  -  vtable interception for IDirectInput8 / IDirectInputDevice8
//
//  Ported from Pad-Within (Warrior Within) to Prince of Persia: The Sands of
//  Time. Same strategy: don't create a separate COM object, patch specific
//  vtable slots on the REAL objects the system dinput8.dll already created.
//
//  Hooks on IDirectInput8:        CreateDevice        (vtable 3)
//                                 EnumDevices          (vtable 4)
//  Hooks on IDirectInputDevice8:  EnumObjects          (vtable 4, diag only)
//                                 GetProperty          (vtable 5, VIDPID spoof)
//                                 GetDeviceState       (vtable 9)
//                                 GetDeviceData        (vtable 10)
//                                 SetDataFormat        (vtable 11, diag)
//
//  Mouse and keyboard devices are created through the SAME underlying
//  dinput8.dll device class as the joystick (confirmed in Pad-Within: they
//  share one vtable), so we patch the vtable once per unique vtable pointer
//  and decide whether to actually synthesize INSIDE each hook, keyed by the
//  real device POINTER, not the vtable. This is exactly Pad-Within's design;
//  see IsTaggedJoystick/IsPrimaryJoystick below.
// =============================================================================

#define WIN32_LEAN_AND_MEAN
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>

#include "proxy.h"
#include "log.h"
#include "config.h"

using CreateDevice_t   = HRESULT (STDMETHODCALLTYPE*)(IDirectInput8*, REFGUID, LPDIRECTINPUTDEVICE8*, LPUNKNOWN);
using EnumDevices_t    = HRESULT (STDMETHODCALLTYPE*)(IDirectInput8*, DWORD, LPDIENUMDEVICESCALLBACKA, LPVOID, DWORD);
using GetDeviceState_t = HRESULT (STDMETHODCALLTYPE*)(IDirectInputDevice8*, DWORD, LPVOID);
using GetDeviceData_t  = HRESULT (STDMETHODCALLTYPE*)(IDirectInputDevice8*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
using SetDataFormat_t  = HRESULT (STDMETHODCALLTYPE*)(IDirectInputDevice8*, LPCDIDATAFORMAT);
using GetProperty_t    = HRESULT (STDMETHODCALLTYPE*)(IDirectInputDevice8*, REFGUID, LPDIPROPHEADER);
using EnumObjects_t    = HRESULT (STDMETHODCALLTYPE*)(IDirectInputDevice8*, LPDIENUMDEVICEOBJECTSCALLBACKA, LPVOID, DWORD);

static CreateDevice_t g_origCreateDevice = nullptr;
static EnumDevices_t  g_origEnumDevices  = nullptr;

struct DevHook {
    void**            vtbl;
    GetDeviceState_t  origState;
    GetDeviceData_t   origData;
    SetDataFormat_t   origSetFmt;
    GetProperty_t     origGetProp;
    EnumObjects_t     origEnum;
};
static DevHook g_devHooks[8] = {};
static int     g_devHookCount = 0;

// Tag which real device POINTERS are the joystick (vs mouse/keyboard, which
// may share the exact same vtable - see file header).
static IDirectInputDevice8* g_joyDevices[8] = {};
static int g_joyDeviceCount = 0;
static bool IsTaggedJoystick(IDirectInputDevice8* dev) {
    for (int i = 0; i < g_joyDeviceCount; ++i) if (g_joyDevices[i] == dev) return true;
    return false;
}
static bool IsPrimaryJoystick(IDirectInputDevice8* dev) {
    return g_joyDeviceCount > 0 && g_joyDevices[0] == dev;
}

static DevHook* FindHook(void** vtbl) {
    for (int i = 0; i < g_devHookCount; ++i) if (g_devHooks[i].vtbl == vtbl) return &g_devHooks[i];
    return nullptr;
}

static void* PatchVtblSlot(void** vtbl, int index, void* newFn)
{
    void* old = vtbl[index];
    DWORD oldProt;
    if (VirtualProtect(&vtbl[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt)) {
        vtbl[index] = newFn;
        VirtualProtect(&vtbl[index], sizeof(void*), oldProt, &oldProt);
        return old;
    }
    LOG("VirtualProtect failed patching slot %d", index);
    return nullptr;
}

// ---------------------------------------------------------------------------
//  Shared synthesis core - writes a game-facing joystick state buffer from
//  the current SDL pad state (via Proxy_FillJoyBuffer), plus the menu-mouse /
//  global-hotkey side effects. Used both by Hook_GetDeviceState (patched REAL
//  device path, used when a physical controller was already attached when
//  the game enumerated) and by VirtualJoystickDevice::GetDeviceState (fully
//  synthetic device path, used when nothing was attached at enumeration
//  time - see EnumDevicesThunk / Hook_CreateDevice below). Keeping this in
//  one place means both paths stay identical as this logic evolves.
// ---------------------------------------------------------------------------
static HRESULT SynthesizeJoystickState(DWORD cbData, LPVOID lpvData)
{
    if (!lpvData) return DIERR_INVALIDPARAM;

    extern void Proxy_LogJoystickCountOnce();
    Proxy_LogJoystickCountOnce();

    Proxy_HandleGlobalHotkeys();

    if (Proxy_UpdateMenuMouse()) {
        // Menu-mouse mode active: the right stick is driving the real
        // Windows cursor and a button is clicking, via SendInput - not
        // through DirectInput at all. Feed the game a neutral pad state so
        // it doesn't also react to the toggle/click buttons as gameplay
        // input while we're just trying to move a cursor.
        ZeroMemory(lpvData, cbData);
        for (DWORD i = 32; i < cbData && i < 48; i += 4)
            *reinterpret_cast<DWORD*>((char*)lpvData + i) = 0xFFFFFFFF;  // POVs centered
        return DI_OK;
    }

    int buttonsCapacity = -1;
    if (cbData == sizeof(DIJOYSTATE))       buttonsCapacity = 32;
    else if (cbData == sizeof(DIJOYSTATE2)) buttonsCapacity = 128;

    if (buttonsCapacity < 0) {
        // Unknown/custom format - log once per size so we know what to add
        // support for, but don't touch a buffer we don't understand the
        // layout of.
        static DWORD lastSizes[8] = {}; static int nSizes = 0;
        bool seen = false;
        for (int i = 0; i < nSizes; ++i) if (lastSizes[i] == cbData) seen = true;
        if (!seen && nSizes < 8) { lastSizes[nSizes++] = cbData; LOG("GetDeviceState: unrecognized cbData=%lu, zeroing", (unsigned long)cbData); }
        ZeroMemory(lpvData, cbData);
        return DI_OK;
    }

    ZeroMemory(lpvData, cbData);
    Proxy_FillJoyBuffer(lpvData, buttonsCapacity);
    return DI_OK;
}

static HRESULT STDMETHODCALLTYPE Hook_GetDeviceState(IDirectInputDevice8* self, DWORD cbData, LPVOID lpvData)
{
    void** vtbl = *reinterpret_cast<void***>(self);
    DevHook* h = FindHook(vtbl);
    HRESULT hr = (h && h->origState) ? h->origState(self, cbData, lpvData) : DIERR_NOTINITIALIZED;

    if (!IsPrimaryJoystick(self) || !lpvData) return hr;

    // Unconditional (not gated by EnableLog's throttle) - fires exactly once,
    // so we can always tell whether the game ever calls GetDeviceState on the
    // joystick at all in a given session, regardless of ini settings.
    static bool loggedFirstCall = false;
    if (!loggedFirstCall) {
        loggedFirstCall = true;
        LOG("Hook_GetDeviceState: FIRST call for primary joystick, cbData=%lu", (unsigned long)cbData);
    }

    if (Proxy_Passthrough()) {
        // Still run hotkeys/menu-mouse even in passthrough, but never touch
        // lpvData for the actual pad values - that's the whole point of
        // passthrough (diagnostic capture of the untouched native state).
        extern void Proxy_LogJoystickCountOnce();
        Proxy_LogJoystickCountOnce();
        Proxy_HandleGlobalHotkeys();
        if (Proxy_LogEnabled()) {
            static DWORD lastP = 0; DWORD now = GetTickCount();
            if (now - lastP > 400) {
                lastP = now;
                const long* a = reinterpret_cast<const long*>(lpvData);
                LOG("NATIVE %luB: X=%ld Y=%ld Z=%ld Rx=%ld Ry=%ld Rz=%ld S0=%ld S1=%ld POV0=%lu",
                    (unsigned long)cbData, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7],
                    *reinterpret_cast<const DWORD*>((const char*)lpvData + 32));
            }
        }
        return hr;   // do NOT synthesize
    }

    return SynthesizeJoystickState(cbData, lpvData);
}

// ---------------------------------------------------------------------------
static HRESULT STDMETHODCALLTYPE Hook_GetDeviceData(
    IDirectInputDevice8* self, DWORD cbObjectData, LPDIDEVICEOBJECTDATA rgdod, LPDWORD pdwInOut, DWORD flags)
{
    void** vtbl = *reinterpret_cast<void***>(self);
    DevHook* h = FindHook(vtbl);
    HRESULT hr = (h && h->origData) ? h->origData(self, cbObjectData, rgdod, pdwInOut, flags) : DIERR_NOTINITIALIZED;

    // Buffered mode: we don't synthesize DIDEVICEOBJECTDATA events (would need
    // per-object dwOfs from SetDataFormat plus edge-detection against last
    // state). Same limitation as Pad-Within. Log so we notice if SoT actually
    // relies on this path for the pad instead of GetDeviceState.
    if (IsPrimaryJoystick(self) && Proxy_LogEnabled()) {
        static DWORD last = 0; DWORD now = GetTickCount();
        if (now - last > 1000) {
            last = now;
            LOG("GetDeviceData[JOY dev=%p]: hr=0x%08lX n=%lu (buffered mode not synthesized)",
                (void*)self, (unsigned long)hr, pdwInOut ? *pdwInOut : 0);
        }
    }
    return hr;
}

// ---------------------------------------------------------------------------
static const char* DofGuidName(const GUID* g)
{
    if (!g) return "any";
    if (IsEqualGUID(*g, GUID_XAxis))  return "X";
    if (IsEqualGUID(*g, GUID_YAxis))  return "Y";
    if (IsEqualGUID(*g, GUID_ZAxis))  return "Z";
    if (IsEqualGUID(*g, GUID_RxAxis)) return "Rx";
    if (IsEqualGUID(*g, GUID_RyAxis)) return "Ry";
    if (IsEqualGUID(*g, GUID_RzAxis)) return "Rz";
    if (IsEqualGUID(*g, GUID_Slider)) return "Slider";
    if (IsEqualGUID(*g, GUID_POV))    return "POV";
    if (IsEqualGUID(*g, GUID_Button)) return "Button";
    return "?";
}

static HRESULT STDMETHODCALLTYPE Hook_SetDataFormat(IDirectInputDevice8* self, LPCDIDATAFORMAT fmt)
{
    void** vtbl = *reinterpret_cast<void***>(self);
    DevHook* h = FindHook(vtbl);
    HRESULT hr = (h && h->origSetFmt) ? h->origSetFmt(self, fmt) : DIERR_NOTINITIALIZED;

    if (fmt && IsTaggedJoystick(self)) {
        LOG("SetDataFormat dev=%p: dwDataSize=%lu numObjs=%lu (DIJOYSTATE=%zu DIJOYSTATE2=%zu)",
            (void*)self, (unsigned long)fmt->dwDataSize, (unsigned long)fmt->dwNumObjs,
            sizeof(DIJOYSTATE), sizeof(DIJOYSTATE2));
        for (DWORD i = 0; i < fmt->dwNumObjs; ++i) {
            const DIOBJECTDATAFORMAT& o = fmt->rgodf[i];
            DWORD type = DIDFT_GETTYPE(o.dwType);
            const char* kind = (type & DIDFT_AXIS) ? "AXIS" : (type & DIDFT_BUTTON) ? "BUTTON" : (type & DIDFT_POV) ? "POV" : "?";
            LOG("   obj[%2lu] ofs=%3lu %-6s guid=%-6s inst=%lu",
                i, o.dwOfs, kind, DofGuidName(o.pguid), (unsigned long)DIDFT_GETINSTANCE(o.dwType));
        }
    }
    return hr;
}

// ---------------------------------------------------------------------------
// Diagnostic only (see file header): confirms the real device's NATIVE object
// layout before SetDataFormat remaps it. Doesn't drive GetDeviceState - that
// writes to the game-facing STANDARD offsets regardless (Pad-Within's own
// passthrough capture confirmed DirectInput does this remap for us).
static HRESULT STDMETHODCALLTYPE Hook_EnumObjects(
    IDirectInputDevice8* self, LPDIENUMDEVICEOBJECTSCALLBACKA cb, LPVOID ctx, DWORD flags)
{
    void** vtbl = *reinterpret_cast<void***>(self);
    DevHook* h = FindHook(vtbl);
    if (!h || !h->origEnum) return DIERR_NOTINITIALIZED;
    HRESULT hr = h->origEnum(self, cb, ctx, flags);
    if (IsTaggedJoystick(self) && Proxy_LogEnabled()) {
        LOG("EnumObjects dev=%p flags=0x%lX -> hr=0x%08lX", (void*)self, flags, (unsigned long)hr);
    }
    return hr;
}

// ---------------------------------------------------------------------------
static HRESULT STDMETHODCALLTYPE Hook_GetProperty(IDirectInputDevice8* self, REFGUID rguidProp, LPDIPROPHEADER pdiph)
{
    void** vtbl = *reinterpret_cast<void***>(self);
    DevHook* h = FindHook(vtbl);
    HRESULT hr = (h && h->origGetProp) ? h->origGetProp(self, rguidProp, pdiph) : DIERR_NOTINITIALIZED;

    if (g_cfg.spoofVidPid && IsTaggedJoystick(self)
        && &rguidProp == &DIPROP_VIDPID && pdiph && pdiph->dwSize >= sizeof(DIPROPDWORD)) {
        DIPROPDWORD* pd = reinterpret_cast<DIPROPDWORD*>(pdiph);
        DWORD oldVal = pd->dwData;
        WORD vid = (WORD)g_cfg.spoofVID;
        WORD pid = (WORD)(g_cfg.spoofPID ? g_cfg.spoofPID : HIWORD(oldVal));
        pd->dwData = MAKELONG(vid, pid);
        hr = DI_OK;
        LOG("GetProperty VIDPID spoof dev=%p: 0x%08lX -> 0x%08lX", (void*)self, oldVal, pd->dwData);
    }
    return hr;
}

// =============================================================================
//  Fully synthetic joystick device
//
//  If the game enumerates DirectInput BEFORE any physical controller is
//  plugged in, the real dinput8.dll reports zero joystick-class devices, so
//  Hook_CreateDevice never gets a real device to patch and the game never
//  polls anything - it simply believes there's no pad for the rest of the
//  session (see EnumDevicesThunk below, which is what notices this and
//  injects GUID_ProxyVirtualJoystick as a fake enumerated device).
//
//  This class is the device object handed back for that fake GUID. It's a
//  real (if minimal) implementation of IDirectInputDevice8 - not a patched
//  real object - so it exists and can be polled regardless of whether any
//  physical controller is ever connected; all the actual pad state still
//  comes from SDL via SynthesizeJoystickState()/Proxy_FillJoyBuffer(), so
//  hotplugging behaves identically to the "real device" path.
//
//  Deliberately unsupported / stubbed: force feedback (CreateEffect and
//  friends), buffered device data (GetDeviceData - matches the existing
//  Hook_GetDeviceData limitation noted in its own comment), action mapping
//  (BuildActionMap/SetActionMap), and image info. None of these are used by
//  a simple polled DIJOYSTATE/DIJOYSTATE2 consumer.
// =============================================================================

// {5B1E3A2E-7C1B-4E58-9F5B-6D6E1C0B2AA1} - arbitrary but fixed; only needs to
// be a value EnumDevicesThunk and Hook_CreateDevice agree on.
static const GUID GUID_ProxyVirtualJoystick =
{ 0x5b1e3a2e, 0x7c1b, 0x4e58, { 0x9f, 0x5b, 0x6d, 0x6e, 0x1c, 0x0b, 0x2a, 0xa1 } };

class VirtualJoystickDevice : public IDirectInputDevice8A
{
public:
    VirtualJoystickDevice() : m_refCount(1), m_acquired(false), m_dataSize(0),
                               m_bufferSize(0), m_hEvent(nullptr) {}

    // --- IUnknown ---
    STDMETHODIMP QueryInterface(REFIID riid, LPVOID* ppv) override
    {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) ||
            IsEqualIID(riid, IID_IDirectInputDeviceA) ||
            IsEqualIID(riid, IID_IDirectInputDevice2A) ||
            IsEqualIID(riid, IID_IDirectInputDevice7A) ||
            IsEqualIID(riid, IID_IDirectInputDevice8A)) {
            *ppv = this; AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&m_refCount); }
    STDMETHODIMP_(ULONG) Release() override
    {
        LONG r = InterlockedDecrement(&m_refCount);
        if (r == 0) delete this;
        return (ULONG)r;
    }

    // --- capabilities / enumeration (best-effort - see file header) ---
    STDMETHODIMP GetCapabilities(LPDIDEVCAPS caps) override
    {
        if (!caps || (caps->dwSize != sizeof(DIDEVCAPS))) return DIERR_INVALIDPARAM;
        ZeroMemory(caps, sizeof(DIDEVCAPS));
        caps->dwSize    = sizeof(DIDEVCAPS);
        caps->dwFlags   = DIDC_ATTACHED | DIDC_EMULATED;
        caps->dwDevType = DI8DEVTYPE_GAMEPAD | (DI8DEVTYPEGAMEPAD_STANDARD << 8);
        caps->dwAxes    = 6;   // X,Y,Z,Rx,Ry,Rz
        caps->dwButtons = 32;
        caps->dwPOVs    = 1;
        return DI_OK;
    }

    STDMETHODIMP EnumObjects(LPDIENUMDEVICEOBJECTSCALLBACKA cb, LPVOID ctx, DWORD flags) override
    {
        if (!cb) return DIERR_INVALIDPARAM;
        struct { REFGUID guid; DWORD ofs; const char* name; } axes[] = {
            { GUID_XAxis, 0,  "X Axis"  }, { GUID_YAxis,  4,  "Y Axis"  },
            { GUID_ZAxis, 8,  "Z Axis"  }, { GUID_RxAxis, 12, "X Rotation" },
            { GUID_RyAxis,16, "Y Rotation" }, { GUID_RzAxis, 20, "Z Rotation" },
        };
        if (flags == DIDFT_ALL || (flags & DIDFT_AXIS)) {
            for (int i = 0; i < 6; ++i) {
                DIDEVICEOBJECTINSTANCEA o = {};
                o.dwSize = sizeof(o);
                o.guidType = axes[i].guid;
                o.dwOfs = axes[i].ofs;
                o.dwType = DIDFT_MAKEINSTANCE(i) | DIDFT_ABSAXIS;
                lstrcpynA(o.tszName, axes[i].name, sizeof(o.tszName));
                if (cb(&o, ctx) == DIENUM_STOP) return DI_OK;
            }
        }
        if (flags == DIDFT_ALL || (flags & DIDFT_POV)) {
            DIDEVICEOBJECTINSTANCEA o = {};
            o.dwSize = sizeof(o);
            o.guidType = GUID_POV;
            o.dwOfs = 32;
            o.dwType = DIDFT_MAKEINSTANCE(0) | DIDFT_POV;
            lstrcpynA(o.tszName, "Hat Switch", sizeof(o.tszName));
            if (cb(&o, ctx) == DIENUM_STOP) return DI_OK;
        }
        if (flags == DIDFT_ALL || (flags & DIDFT_BUTTON)) {
            for (int i = 0; i < 32; ++i) {
                DIDEVICEOBJECTINSTANCEA o = {};
                o.dwSize = sizeof(o);
                o.guidType = GUID_Button;
                o.dwOfs = 48 + i;
                o.dwType = DIDFT_MAKEINSTANCE(i) | DIDFT_PSHBUTTON;
                char name[32]; snprintf(name, sizeof(name), "Button %d", i);
                lstrcpynA(o.tszName, name, sizeof(o.tszName));
                if (cb(&o, ctx) == DIENUM_STOP) return DI_OK;
            }
        }
        return DI_OK;
    }

    STDMETHODIMP GetObjectInfo(LPDIDEVICEOBJECTINSTANCEA info, DWORD obj, DWORD how) override
    {
        // Not exercised by a simple polled joystick consumer - minimal
        // support only. Fail rather than guess if something ever calls it.
        (void)info; (void)obj; (void)how;
        return DIERR_INVALIDPARAM;
    }

    // --- properties ---
    STDMETHODIMP GetProperty(REFGUID rguidProp, LPDIPROPHEADER pdiph) override
    {
        if (!pdiph) return DIERR_INVALIDPARAM;
        if (&rguidProp == &DIPROP_VIDPID && pdiph->dwSize >= sizeof(DIPROPDWORD)) {
            DIPROPDWORD* pd = reinterpret_cast<DIPROPDWORD*>(pdiph);
            // Default to a real Xbox 360 controller's VID/PID so games that
            // gate gamepad recognition on a known-good VID/PID still accept
            // this device even with SpoofVidPid off in the ini - this device
            // is entirely synthetic, so there's no "real" id to preserve.
            WORD vid = g_cfg.spoofVidPid ? (WORD)g_cfg.spoofVID : 0x045E;
            WORD pid = g_cfg.spoofVidPid && g_cfg.spoofPID ? (WORD)g_cfg.spoofPID : 0x028E;
            pd->dwData = MAKELONG(vid, pid);
            return DI_OK;
        }
        if (&rguidProp == &DIPROP_RANGE && pdiph->dwSize >= sizeof(DIPROPRANGE)) {
            DIPROPRANGE* pr = reinterpret_cast<DIPROPRANGE*>(pdiph);
            pr->lMin = -32768; pr->lMax = 32767;
            return DI_OK;
        }
        return DIERR_UNSUPPORTED;
    }
    STDMETHODIMP SetProperty(REFGUID rguidProp, LPCDIPROPHEADER pdiph) override
    {
        // Deadzone/range/axis-mode are already applied ourselves in
        // Proxy_FillJoyBuffer, so accept and ignore. Buffer size we do
        // track, purely so GetDeviceData can tell "unbuffered" from
        // "buffered, nothing pending" (see GetDeviceData below).
        if (&rguidProp == &DIPROP_BUFFERSIZE && pdiph && pdiph->dwSize >= sizeof(DIPROPDWORD))
            m_bufferSize = reinterpret_cast<const DIPROPDWORD*>(pdiph)->dwData;
        return DI_OK;
    }

    // --- acquisition ---
    STDMETHODIMP Acquire() override { m_acquired = true; return DI_OK; }
    STDMETHODIMP Unacquire() override { m_acquired = false; return DI_OK; }
    STDMETHODIMP SetCooperativeLevel(HWND, DWORD) override { return DI_OK; }
    STDMETHODIMP Initialize(HINSTANCE, DWORD, REFGUID) override { return DI_OK; }

    // --- state ---
    STDMETHODIMP GetDeviceState(DWORD cbData, LPVOID lpvData) override
    {
        if (!m_acquired) return DIERR_NOTACQUIRED;
        return SynthesizeJoystickState(cbData, lpvData);
    }
    STDMETHODIMP GetDeviceData(DWORD, LPDIDEVICEOBJECTDATA, LPDWORD pdwInOut, DWORD) override
    {
        // No buffered-mode event synthesis (same limitation as the real-
        // device path's Hook_GetDeviceData - would need per-object dwOfs
        // plus edge-detection against last state). If the game never called
        // SetProperty(DIPROP_BUFFERSIZE), the real API contract is to fail;
        // otherwise report "buffered, nothing pending" rather than crash a
        // caller that only uses this path opportunistically.
        if (m_bufferSize == 0) return DIERR_NOTBUFFERED;
        if (pdwInOut) *pdwInOut = 0;
        return DI_OK;
    }
    STDMETHODIMP SetDataFormat(LPCDIDATAFORMAT fmt) override
    {
        if (!fmt) return DIERR_INVALIDPARAM;
        m_dataSize = fmt->dwDataSize;
        LOG("VirtualJoystickDevice::SetDataFormat dev=%p: dwDataSize=%lu numObjs=%lu",
            (void*)this, (unsigned long)fmt->dwDataSize, (unsigned long)fmt->dwNumObjs);
        return DI_OK;
    }
    STDMETHODIMP SetEventNotification(HANDLE hEvent) override { m_hEvent = hEvent; return DI_OK; }
    STDMETHODIMP Poll() override { return DI_OK; }

    STDMETHODIMP GetDeviceInfo(LPDIDEVICEINSTANCEA info) override
    {
        if (!info || info->dwSize != sizeof(DIDEVICEINSTANCEA)) return DIERR_INVALIDPARAM;
        FillInstance(*info);
        return DI_OK;
    }
    STDMETHODIMP RunControlPanel(HWND, DWORD) override { return DI_OK; }

    // --- unsupported: force feedback / buffered file I/O / action mapping ---
    STDMETHODIMP CreateEffect(REFGUID, LPCDIEFFECT, LPDIRECTINPUTEFFECT*, LPUNKNOWN) override { return DIERR_UNSUPPORTED; }
    STDMETHODIMP EnumEffects(LPDIENUMEFFECTSCALLBACKA, LPVOID, DWORD) override { return DI_OK; }
    STDMETHODIMP GetEffectInfo(LPDIEFFECTINFOA, REFGUID) override { return DIERR_UNSUPPORTED; }
    STDMETHODIMP GetForceFeedbackState(LPDWORD) override { return DIERR_UNSUPPORTED; }
    STDMETHODIMP SendForceFeedbackCommand(DWORD) override { return DIERR_UNSUPPORTED; }
    STDMETHODIMP EnumCreatedEffectObjects(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK, LPVOID, DWORD) override { return DI_OK; }
    STDMETHODIMP Escape(LPDIEFFESCAPE) override { return DIERR_UNSUPPORTED; }
    STDMETHODIMP SendDeviceData(DWORD, LPCDIDEVICEOBJECTDATA, LPDWORD, DWORD) override { return DIERR_UNSUPPORTED; }
    STDMETHODIMP EnumEffectsInFile(LPCSTR, LPDIENUMEFFECTSINFILECALLBACK, LPVOID, DWORD) override { return DIERR_UNSUPPORTED; }
    STDMETHODIMP WriteEffectToFile(LPCSTR, DWORD, LPDIFILEEFFECT, DWORD) override { return DIERR_UNSUPPORTED; }
    STDMETHODIMP BuildActionMap(LPDIACTIONFORMATA, LPCSTR, DWORD) override { return DIERR_UNSUPPORTED; }
    STDMETHODIMP SetActionMap(LPDIACTIONFORMATA, LPCSTR, DWORD) override { return DIERR_UNSUPPORTED; }
    STDMETHODIMP GetImageInfo(LPDIDEVICEIMAGEINFOHEADERA) override { return DIERR_UNSUPPORTED; }

    static void FillInstance(DIDEVICEINSTANCEA& inst)
    {
        ZeroMemory(&inst, sizeof(inst));
        inst.dwSize = sizeof(inst);
        inst.guidInstance = GUID_ProxyVirtualJoystick;
        inst.guidProduct  = GUID_ProxyVirtualJoystick;
        inst.dwDevType = DI8DEVTYPE_GAMEPAD | (DI8DEVTYPEGAMEPAD_STANDARD << 8);
        lstrcpynA(inst.tszInstanceName, "Proxy Virtual Controller", sizeof(inst.tszInstanceName));
        const char* name = g_cfg.spoofVidPid ? "Controller (XBOX 360 For Windows)" : "Proxy Virtual Controller";
        lstrcpynA(inst.tszProductName, name, sizeof(inst.tszProductName));
    }

private:
    LONG   m_refCount;
    bool   m_acquired;
    DWORD  m_dataSize;
    DWORD  m_bufferSize;
    HANDLE m_hEvent;
};

// ---------------------------------------------------------------------------
static const char* GuidName(REFGUID g)
{
    if (IsEqualGUID(g, GUID_SysMouse))    return "SysMouse";
    if (IsEqualGUID(g, GUID_SysKeyboard)) return "SysKeyboard";
    return "other/joystick";
}
static bool IsJoystickGuid(REFGUID g) {
    return !IsEqualGUID(g, GUID_SysMouse) && !IsEqualGUID(g, GUID_SysKeyboard);
}

static HRESULT STDMETHODCALLTYPE Hook_CreateDevice(
    IDirectInput8* self, REFGUID rguid, LPDIRECTINPUTDEVICE8* out, LPUNKNOWN outer)
{
    if (out && IsEqualGUID(rguid, GUID_ProxyVirtualJoystick)) {
        // No real device backs this GUID - it was injected by EnumDevicesThunk
        // because nothing was physically attached at enumeration time. Hand
        // back our own COM object instead of forwarding to the real
        // CreateDevice (which has never heard of this GUID and would fail).
        VirtualJoystickDevice* vdev = new VirtualJoystickDevice();
        *out = vdev;
        if (g_joyDeviceCount < 8) {
            g_joyDevices[g_joyDeviceCount++] = vdev;
            LOG("CreateDevice: fabricated VIRTUAL joystick device %p (total joy devices=%d)",
                (void*)vdev, g_joyDeviceCount);
        }
        return DI_OK;
    }

    HRESULT hr = g_origCreateDevice(self, rguid, out, outer);
    LOG("CreateDevice guid=%s -> hr=0x%08lX dev=%p", GuidName(rguid), (unsigned long)hr, (out ? *out : nullptr));

    if (SUCCEEDED(hr) && out && *out) {
        bool isJoy = IsJoystickGuid(rguid);
        if (isJoy && g_joyDeviceCount < 8) {
            g_joyDevices[g_joyDeviceCount++] = *out;
            LOG("  -> tagged joystick device %p (total joy devices=%d)", (void*)*out, g_joyDeviceCount);
        }
        void** devVtbl = *reinterpret_cast<void***>(*out);
        if (!FindHook(devVtbl) && g_devHookCount < 8) {
            GetDeviceState_t oS = reinterpret_cast<GetDeviceState_t>(
                PatchVtblSlot(devVtbl, 9,  reinterpret_cast<void*>(&Hook_GetDeviceState)));
            GetDeviceData_t  oD = reinterpret_cast<GetDeviceData_t>(
                PatchVtblSlot(devVtbl, 10, reinterpret_cast<void*>(&Hook_GetDeviceData)));
            SetDataFormat_t  oF = reinterpret_cast<SetDataFormat_t>(
                PatchVtblSlot(devVtbl, 11, reinterpret_cast<void*>(&Hook_SetDataFormat)));
            GetProperty_t    oP = reinterpret_cast<GetProperty_t>(
                PatchVtblSlot(devVtbl, 5,  reinterpret_cast<void*>(&Hook_GetProperty)));
            EnumObjects_t    oE = reinterpret_cast<EnumObjects_t>(
                PatchVtblSlot(devVtbl, 4,  reinterpret_cast<void*>(&Hook_EnumObjects)));
            g_devHooks[g_devHookCount++] = { devVtbl, oS, oD, oF, oP, oE };
            LOG("  patched vtable %p (hooks=%d)", (void*)devVtbl, g_devHookCount);
        }
    }
    return hr;
}

// ---------------------------------------------------------------------------
static LPDIENUMDEVICESCALLBACKA g_gameEnumDevCb = nullptr;
static LPVOID                   g_gameEnumDevRef = nullptr;
static int                      g_realJoyCountThisEnum = 0;
static bool                     g_enumStoppedByGame    = false;

static BOOL CALLBACK EnumDevicesThunk(LPCDIDEVICEINSTANCEA inst, LPVOID ref)
{
    DIDEVICEINSTANCEA spoofed = *inst;
    BYTE devClass = (BYTE)GET_DIDEVICE_TYPE(inst->dwDevType);
    bool isJoy = (devClass != DI8DEVTYPE_KEYBOARD) && (devClass != DI8DEVTYPE_MOUSE);
    if (isJoy) ++g_realJoyCountThisEnum;

    if (g_cfg.spoofVidPid && isJoy) {
        DWORD orig = *reinterpret_cast<DWORD*>(&spoofed.guidProduct);
        DWORD vidpid = MAKELONG((WORD)g_cfg.spoofVID, (WORD)g_cfg.spoofPID);
        *reinterpret_cast<DWORD*>(&spoofed.guidProduct) = vidpid;
        lstrcpynA(spoofed.tszProductName, "Controller (XBOX 360 For Windows)", sizeof(spoofed.tszProductName));
        LOG("EnumDevices spoof: '%s' guidProduct 0x%08lX -> 0x%08lX", inst->tszProductName, orig, vidpid);
    }
    BOOL ret = g_gameEnumDevCb ? g_gameEnumDevCb(&spoofed, g_gameEnumDevRef) : DIENUM_CONTINUE;
    if (ret == DIENUM_STOP) g_enumStoppedByGame = true;
    return ret;
}

static HRESULT STDMETHODCALLTYPE Hook_EnumDevices(
    IDirectInput8* self, DWORD dwDevType, LPDIENUMDEVICESCALLBACKA cb, LPVOID ref, DWORD flags)
{
    if (!g_origEnumDevices) return DIERR_NOTINITIALIZED;
    g_gameEnumDevCb  = cb;
    g_gameEnumDevRef = ref;
    g_realJoyCountThisEnum = 0;
    g_enumStoppedByGame    = false;
    HRESULT hr = g_origEnumDevices(self, dwDevType, &EnumDevicesThunk, nullptr, flags);
    g_gameEnumDevCb = nullptr;

    // No real joystick-class device was found in this enumeration - inject
    // our fully synthetic one so the game creates SOME joystick device
    // regardless of launch order (see VirtualJoystickDevice's file header).
    // Only for enumerations that would even ask for one, and only if the
    // game's own callback hasn't already told the real enum to stop early.
    bool wantsGameCtrl = (dwDevType == 0 /*DI8DEVCLASS_ALL*/ || dwDevType == DI8DEVCLASS_GAMECTRL);
    if (SUCCEEDED(hr) && wantsGameCtrl && g_realJoyCountThisEnum == 0 && !g_enumStoppedByGame && cb) {
        DIDEVICEINSTANCEA fake;
        VirtualJoystickDevice::FillInstance(fake);
        LOG("EnumDevices: no real joystick present, injecting synthetic device '%s'", fake.tszProductName);
        cb(&fake, ref);
    }
    return hr;
}

// ---------------------------------------------------------------------------
void Proxy_HookDirectInput8(void** ppvOut)
{
    IDirectInput8* di = reinterpret_cast<IDirectInput8*>(*ppvOut);
    void** vtbl = *reinterpret_cast<void***>(di);

    if (!g_origCreateDevice) {
        g_origCreateDevice = reinterpret_cast<CreateDevice_t>(
            PatchVtblSlot(vtbl, 3, reinterpret_cast<void*>(&Hook_CreateDevice)));
        g_origEnumDevices = reinterpret_cast<EnumDevices_t>(
            PatchVtblSlot(vtbl, 4, reinterpret_cast<void*>(&Hook_EnumDevices)));
        LOG("patched IDirectInput8 vtable %p, CreateDevice=%p EnumDevices=%p",
            (void*)vtbl, (void*)g_origCreateDevice, (void*)g_origEnumDevices);
    }
}
