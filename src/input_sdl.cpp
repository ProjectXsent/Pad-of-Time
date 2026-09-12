// =============================================================================
//  input_sdl.cpp  -  SDL2 GameController -> DIJOYSTATE[2] translation
//
//  Ported from Pad-Within's input_sdl.cpp (Warrior Within), generalized to
//  write into either DIJOYSTATE (32 buttons) or DIJOYSTATE2 (128 buttons)
//  via buttonsCapacity, since we don't yet know (no hardware trace done in
//  this environment - see README) which one PCDeviceJoystick actually uses.
//  Both formats share byte-identical axis/slider/POV layout in their first
//  36 bytes, so a single filler covers both.
// =============================================================================

#define WIN32_LEAN_AND_MEAN
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include <SDL.h>
#include <SDL_gamecontroller.h>

#include "proxy.h"
#include "log.h"
#include "config.h"

static SDL_GameController* g_pad = nullptr;
static SDL_Joystick*       g_joy = nullptr;
#define MAX_PADS 8
static SDL_GameController* g_pads[MAX_PADS] = {};
static int                 g_padCount = 0;

// Instance IDs (not device indices!) parallel to g_pads / g_joy. SDL device
// indices shift around as controllers come and go, but instance IDs are
// stable for the lifetime of a connection - SDL_JOYDEVICEREMOVED reports an
// instance ID, so this is what lets hotplug removal find the right slot.
static SDL_JoystickID g_padInstanceIds[MAX_PADS];
static SDL_JoystickID g_joyInstanceId = -1;
// Raw-fallback devices are opened after a short delay rather than instantly
// (see MaybeOpenRawFallback), so we track when we first noticed an unmapped
// joystick sitting around with nothing else claiming it.
static DWORD g_rawFallbackFirstSeenUnmapped = 0;

void Proxy_LogJoystickCountOnce()
{
    static DWORD last = 0;
    DWORD now = GetTickCount();
    if (now - last < 1000) return;
    last = now;
    int nj = SDL_NumJoysticks();
    LOG("periodic: SDL_NumJoysticks()=%d, g_padCount=%d, g_pad=%p", nj, g_padCount, (void*)g_pad);
    for (int i = 0; i < nj; ++i) {
        LOG("  joy[%d] name='%s' isGC=%d", i, SDL_JoystickNameForIndex(i), SDL_IsGameController(i));
    }
}

// ---------------------------------------------------------------------------
//  Hotplug helpers
//
//  These are the only places that call SDL_GameControllerOpen/Close and
//  SDL_JoystickOpen/Close for the "real" pad slots, so g_pads/g_padCount and
//  g_joy stay consistent whether a device was opened at startup or plugged
//  in later.
// ---------------------------------------------------------------------------
static bool OpenControllerAtDeviceIndex(int deviceIndex)
{
    if (g_padCount >= MAX_PADS) return false;
    SDL_GameController* c = SDL_GameControllerOpen(deviceIndex);
    if (!c) {
        LOG("SDL_GameControllerOpen(%d) failed: %s", deviceIndex, SDL_GetError());
        return false;
    }
    SDL_JoystickID instanceId = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(c));
    g_pads[g_padCount] = c;
    g_padInstanceIds[g_padCount] = instanceId;
    g_padCount++;
    if (!g_pad) g_pad = c;
    LOG("opened GameController (instance %d): %s", instanceId, SDL_GameControllerName(c));
    return true;
}

static void RemoveControllerByInstanceId(SDL_JoystickID instanceId)
{
    for (int i = 0; i < g_padCount; ++i) {
        if (g_padInstanceIds[i] != instanceId) continue;

        SDL_GameController* removed = g_pads[i];
        LOG("hotplug: GameController disconnected (instance %d): %s",
            instanceId, removed ? SDL_GameControllerName(removed) : "?");
        if (removed) SDL_GameControllerClose(removed);

        // Compact the array - shift everything after i down by one so there
        // are no holes for SelectActivePad()/RescanIfNeeded() to trip over.
        for (int j = i; j < g_padCount - 1; ++j) {
            g_pads[j]           = g_pads[j + 1];
            g_padInstanceIds[j] = g_padInstanceIds[j + 1];
        }
        g_padCount--;
        g_pads[g_padCount]           = nullptr;
        g_padInstanceIds[g_padCount] = -1;

        if (g_pad == removed) {
            // Fall back to whatever's left, if anything. If the ini pins a
            // specific controllerIndex the user just unplugged, we still
            // fall back rather than going silent - SelectActivePad() only
            // auto-switches when controllerIndex is unset (-1) anyway.
            g_pad = (g_padCount > 0) ? g_pads[0] : nullptr;
            LOG("hotplug: active pad disconnected, %s",
                g_pad ? SDL_GameControllerName(g_pad) : "no pads remain");
        }
        return;
    }
}

static void OpenRawJoystick(int deviceIndex)
{
    if (g_joy) return; // already have one open
    g_joy = SDL_JoystickOpen(deviceIndex);
    if (!g_joy) return;
    g_joyInstanceId = SDL_JoystickInstanceID(g_joy);
    LOG("opened raw Joystick (instance %d) '%s' (axes=%d buttons=%d hats=%d)",
        g_joyInstanceId, SDL_JoystickName(g_joy), SDL_JoystickNumAxes(g_joy),
        SDL_JoystickNumButtons(g_joy), SDL_JoystickNumHats(g_joy));
}

static void RemoveRawJoystickIfMatches(SDL_JoystickID instanceId)
{
    if (!g_joy || g_joyInstanceId != instanceId) return;
    LOG("hotplug: raw Joystick disconnected (instance %d)", instanceId);
    SDL_JoystickClose(g_joy);
    g_joy = nullptr;
    g_joyInstanceId = -1;
    g_rawFallbackFirstSeenUnmapped = 0; // let the fallback re-arm cleanly
}

// ---------------------------------------------------------------------------
//  Event-driven hotplug
//
//  Drains the SDL event queue looking for SDL_JOYDEVICEADDED/REMOVED. These
//  fire for every joystick - GameController-mapped or not - unlike the
//  SDL_CONTROLLERDEVICE* variants, which is what the raw fallback path
//  needs. SDL_JOYDEVICEADDED reports a device index (usable with
//  SDL_GameControllerOpen/SDL_JoystickOpen); SDL_JOYDEVICEREMOVED reports an
//  instance ID (device indices aren't stable across a removal, since the
//  slot gets reused).
//
//  Safe to call every frame - if nothing changed, SDL_PollEvent just
//  returns false immediately. This DLL doesn't have an SDL window and isn't
//  otherwise pumping the SDL event queue, so it's fine to fully drain it
//  here; if that ever changes, switch to SDL_PeepEvents so other event
//  types aren't silently eaten.
// ---------------------------------------------------------------------------
void Proxy_HandleHotplugEvents()
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_JOYDEVICEADDED: {
            int deviceIndex = ev.jdevice.which;
            if (SDL_IsGameController(deviceIndex)) {
                OpenControllerAtDeviceIndex(deviceIndex);
                // A GameController showed up - if the raw-fallback timer was
                // running for some other unmapped device, let it re-check
                // from scratch next call rather than firing on stale state.
                g_rawFallbackFirstSeenUnmapped = 0;
            } else {
                LOG("hotplug: joystick added, no GameController mapping (device %d): %s",
                    deviceIndex, SDL_JoystickNameForIndex(deviceIndex));
                // Actual opening (if AllowRawFallback=1) happens in
                // MaybeOpenRawFallback() after its debounce delay, not here.
            }
            break;
        }
        case SDL_JOYDEVICEREMOVED: {
            SDL_JoystickID instanceId = ev.jdevice.which;
            RemoveControllerByInstanceId(instanceId);
            RemoveRawJoystickIfMatches(instanceId);
            break;
        }
        default:
            break;
        }
    }
}

// Opt-in raw fallback (AllowRawFallback=1 in PadOfTime.ini): if nothing with
// a real GameController mapping ever shows up, wait a bit - in case HIDAPI
// is about to claim the device and give it one - then grab the first
// unmapped joystick and read it generically. See the original design note
// on Proxy_FillJoyBuffer's raw-Joystick branch for what "generic" means.
static void MaybeOpenRawFallback()
{
    if (!g_cfg.allowRawFallback || g_padCount != 0 || g_joy) {
        g_rawFallbackFirstSeenUnmapped = 0;
        return;
    }
    int nj = SDL_NumJoysticks();
    if (nj == 0) {
        g_rawFallbackFirstSeenUnmapped = 0;
        return;
    }
    DWORD now = GetTickCount();
    if (g_rawFallbackFirstSeenUnmapped == 0) g_rawFallbackFirstSeenUnmapped = now;
    if (now - g_rawFallbackFirstSeenUnmapped > 3000) {
        for (int i = 0; i < nj; ++i) {
            if (SDL_IsGameController(i)) continue; // shouldn't happen (g_padCount==0), but be safe
            OpenRawJoystick(i);
            if (g_joy) break;
        }
    }
}

// ---------------------------------------------------------------------------
void Proxy_InitInput()
{
    SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
    SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "1");
    // NOT forcing RAWINPUT on: on this SDL2 build it was grabbing the Switch
    // Pro Controller BEFORE the HIDAPI backend could claim it, exposing it
    // under the generic Windows HID name with no SDL_GameController mapping
    // (confirmed via log: SDL_IsGameController()==0, name='HID-compliant
    // game controller'). HIDAPI's Switch driver knows the real name/mapping;
    // let it take priority instead of forcing RAWINPUT to compete for it.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    // DualSense/DualShock are NOT XInput devices - they only show up via the
    // HIDAPI backend (confirmed present in this SDL2.dll build, including a
    // full DualSense mapping). Force it on explicitly rather than relying on
    // whatever the build's compiled-in default is.
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_SWITCH, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_JOY_CONS, "1");

    // HIDAPI's device-detection thread dispatches through the events
    // subsystem; without SDL_INIT_EVENTS explicitly up, the very first
    // enumeration (which we do a few lines below, synchronously, milliseconds
    // after SDL_Init) can race the thread and see zero devices.
    if (SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_EVENTS) != 0) {
        LOG("SDL_Init failed: %s", SDL_GetError());
        return;
    }

    // Give the HIDAPI background thread a moment to finish its first scan
    // before we enumerate. This runs once, at the game's very early startup
    // (first DirectInput8Create call), well before the game creates its
    // window, so a short blocking wait here is harmless.
    for (int attempt = 0; attempt < 10; ++attempt) {
        SDL_PumpEvents();
        if (SDL_NumJoysticks() > 0) break;
        SDL_Delay(100);
    }

    int nj = SDL_NumJoysticks();
    LOG("SDL sees %d joystick(s):", nj);
    for (int i = 0; i < nj; ++i)
        LOG("  joy[%d] name='%s' isGC=%d", i, SDL_JoystickNameForIndex(i), SDL_IsGameController(i));

    for (int i = 0; i < MAX_PADS; ++i) g_padInstanceIds[i] = -1;

    for (int i = 0; i < nj && g_padCount < MAX_PADS; ++i) {
        if (SDL_IsGameController(i)) {
            OpenControllerAtDeviceIndex(i);
        }
    }

    if (g_cfg.controllerIndex >= 0 && g_cfg.controllerIndex < g_padCount) {
        g_pad = g_pads[g_cfg.controllerIndex];
        LOG("using controller by ini index %d: %s", g_cfg.controllerIndex, SDL_GameControllerName(g_pad));
    } else if (g_padCount > 0) {
        g_pad = g_pads[0];
        LOG("auto mode: defaulting to controller 0, will switch to active pad");
    }

    if (g_padCount == 0 && nj > 0) {
        OpenRawJoystick(0);
    }
    if (!g_pad && !g_joy) LOG("no SDL device opened at init");

    // The device-detection thread (and SDL_Init itself) queues one
    // SDL_JOYDEVICEADDED per already-connected joystick. We've just handled
    // that initial connect state by hand above, so drop those events now -
    // otherwise the first Proxy_HandleHotplugEvents() call would try to open
    // every device a second time (SDL_GameControllerOpen on an already-open
    // instance just hands back the same handle, which would corrupt g_pads).
    SDL_PumpEvents();
    SDL_FlushEvent(SDL_JOYDEVICEADDED);
}

static void RescanIfNeeded()
{
    // Event-driven hotplug: opens newly-connected GameControllers and closes
    // (and un-registers) ones that disappear, keyed by stable instance ID.
    // Replaces the old "re-poll SDL_NumJoysticks every 500ms" approach,
    // which could never detect a disconnect at all - a pad could unplug and
    // Proxy_FillJoyBuffer would keep calling SDL_GameControllerGetButton on
    // a dead handle indefinitely (harmless in modern SDL2, which just
    // returns 0/neutral for a detached device, but it never noticed and
    // never freed the slot for a replacement pad either).
    Proxy_HandleHotplugEvents();

    // Raw fallback (AllowRawFallback=1) still runs on its own debounce
    // timer rather than opening the instant we see an unmapped device - see
    // MaybeOpenRawFallback for why.
    MaybeOpenRawFallback();
}

static void SelectActivePad()
{
    RescanIfNeeded();
    if (g_cfg.controllerIndex >= 0) return;
    if (g_padCount <= 1) return;
    for (int i = 0; i < g_padCount; ++i) {
        SDL_GameController* c = g_pads[i];
        if (!c) continue;
        for (int a = 0; a < SDL_CONTROLLER_AXIS_MAX; ++a) {
            if (abs(SDL_GameControllerGetAxis(c, (SDL_GameControllerAxis)a)) > 12000) {
                if (g_pad != c) LOG("switching active pad to %d: %s", i, SDL_GameControllerName(c));
                g_pad = c; return;
            }
        }
        for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; ++b) {
            if (SDL_GameControllerGetButton(c, (SDL_GameControllerButton)b)) {
                if (g_pad != c) LOG("switching active pad to %d: %s", i, SDL_GameControllerName(c));
                g_pad = c; return;
            }
        }
    }
}

void Proxy_ShutdownInput()
{
    for (int i = 0; i < g_padCount; ++i) {
        if (g_pads[i]) SDL_GameControllerClose(g_pads[i]);
        g_padInstanceIds[i] = -1;
    }
    g_padCount = 0; g_pad = nullptr;
    if (g_joy) { SDL_JoystickClose(g_joy); g_joy = nullptr; }
    g_joyInstanceId = -1;
    g_rawFallbackFirstSeenUnmapped = 0;
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK);
}

void Proxy_LogRawPad()
{
    SDL_PumpEvents();
    SDL_GameControllerUpdate();
    SDL_JoystickUpdate();

    char buf[768]; int n = 0;
    n += snprintf(buf+n, sizeof(buf)-n, "  [gc] ");
    if (g_pad) {
        n += snprintf(buf+n, sizeof(buf)-n, "btn:");
        for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; ++b)
            if (SDL_GameControllerGetButton(g_pad, (SDL_GameControllerButton)b))
                n += snprintf(buf+n, sizeof(buf)-n, " %s",
                    SDL_GameControllerGetStringForButton((SDL_GameControllerButton)b));
        n += snprintf(buf+n, sizeof(buf)-n, " axis:");
        for (int a = 0; a < SDL_CONTROLLER_AXIS_MAX; ++a) {
            int v = SDL_GameControllerGetAxis(g_pad, (SDL_GameControllerAxis)a);
            if (v > 8000 || v < -8000)
                n += snprintf(buf+n, sizeof(buf)-n, " %s=%d",
                    SDL_GameControllerGetStringForAxis((SDL_GameControllerAxis)a), v);
        }
    } else {
        n += snprintf(buf+n, sizeof(buf)-n, "(no GameController)");
    }
    LOG("%s", buf);
}

// ---------------------------------------------------------------------------
static void RadialDeadzone(float& x, float& y, float dz)
{
    float mag = std::sqrt(x*x + y*y);
    if (mag < dz) { x = 0.f; y = 0.f; return; }
    float scaled = (mag - dz) / (1.f - dz);
    if (scaled > 1.f) scaled = 1.f;
    x = (x / mag) * scaled;
    y = (y / mag) * scaled;
}

static void ApplyMaxInput(float& x, float& y, int maxInputPercent)
{
    if (maxInputPercent >= 100) return;
    float maxInput = maxInputPercent / 100.f;
    if (maxInput < 0.5f) maxInput = 0.5f;
    x = x / maxInput; if (x > 1.f) x = 1.f; if (x < -1.f) x = -1.f;
    y = y / maxInput; if (y > 1.f) y = 1.f; if (y < -1.f) y = -1.f;
}

static void AxisSnap(float& x, float& y)
{
    float ratio = g_cfg.axisSnapRatio;
    if (ratio <= 0.f) return;
    float ax = std::fabs(x), ay = std::fabs(y);
    if (ax < ay * ratio) x = 0.f;
    else if (ay < ax * ratio) y = 0.f;
}

static long AxisDI(float norm)
{
    if (norm < -1.f) norm = -1.f;
    if (norm >  1.f) norm =  1.f;
    long v = (long)lroundf(norm * 32767.f);
    if (v >  32767) v =  32767;
    if (v < -32768) v = -32768;
    return v;
}

// ---------------------------------------------------------------------------
// Standard predefined-format byte offsets (identical for DIJOYSTATE and
// DIJOYSTATE2 in their shared prefix):
//   lX=0 lY=4 lZ=8 lRx=12 lRy=16 lRz=20 rglSlider[2]=24,28 rgdwPOV[4]=32..47
//   rgbButtons[N]=48..  (N=32 for DIJOYSTATE, 128 for DIJOYSTATE2)
// ---------------------------------------------------------------------------
void Proxy_FillJoyBuffer(void* buf, int buttonsCapacity)
{
    if (!buf) return;
    SDL_GameControllerUpdate();
    SelectActivePad();
    unsigned char* base = reinterpret_cast<unsigned char*>(buf);

    if (!g_pad && !g_joy) return;   // buffer was already zeroed by the caller

    if (!g_pad && g_joy) {
        // ---- raw SDL_Joystick fallback (no GameController mapping) ----
        // Best-effort generic layout: axis0/1 = move stick, axis2/3 = camera
        // stick if present, hat0 = D-pad, button i -> game button i (identity).
        // This is a starting point, not a calibrated mapping - if buttons/axes
        // land wrong in-game, enable EnableLog=1 and watch the "[raw]" lines
        // below while pressing each button/moving each axis on the real pad
        // to see its actual index, then we can wire up a proper remap.
        SDL_JoystickUpdate();
        auto W = [&](int ofs, long val){ *reinterpret_cast<long*>(base + ofs) = val; };

        int nAxes = SDL_JoystickNumAxes(g_joy);
        float lx = nAxes > 0 ? SDL_JoystickGetAxis(g_joy, 0) / 32767.f : 0.f;
        float ly = nAxes > 1 ? SDL_JoystickGetAxis(g_joy, 1) / 32767.f : 0.f;
        ApplyMaxInput(lx, ly, g_cfg.moveMaxRange);
        RadialDeadzone(lx, ly, g_cfg.moveDeadzone);
        AxisSnap(lx, ly);
        if (g_cfg.invertMoveY) ly = -ly;
        W(0, AxisDI(lx));
        W(4, AxisDI(ly));

        float rx = nAxes > 2 ? SDL_JoystickGetAxis(g_joy, 2) / 32767.f : 0.f;
        float ry = nAxes > 3 ? SDL_JoystickGetAxis(g_joy, 3) / 32767.f : 0.f;
        float camMul = g_cfg.cameraSensitivity / 50.f;
        if (camMul < 0.f) camMul = 0.f;
        ApplyMaxInput(rx, ry, g_cfg.cameraMaxRange);
        RadialDeadzone(rx, ry, g_cfg.cameraDeadzone);
        AxisSnap(rx, ry);
        rx *= camMul; ry *= camMul;
        if (g_cfg.invertCameraX) rx = -rx;
        if (g_cfg.invertCameraY) ry = -ry;
        W(12, AxisDI(rx));
        W(16, AxisDI(ry));

        int nButtons = SDL_JoystickNumButtons(g_joy);
        for (int i = 0; i < nButtons && i < buttonsCapacity; ++i)
            if (SDL_JoystickGetButton(g_joy, i)) base[48 + i] = 0x80;

        DWORD pov = 0xFFFFFFFF;
        if (SDL_JoystickNumHats(g_joy) > 0) {
            Uint8 hat = SDL_JoystickGetHat(g_joy, 0);
            switch (hat) {
                case SDL_HAT_UP: pov = 0; break;
                case SDL_HAT_RIGHTUP: pov = 4500; break;
                case SDL_HAT_RIGHT: pov = 9000; break;
                case SDL_HAT_RIGHTDOWN: pov = 13500; break;
                case SDL_HAT_DOWN: pov = 18000; break;
                case SDL_HAT_LEFTDOWN: pov = 22500; break;
                case SDL_HAT_LEFT: pov = 27000; break;
                case SDL_HAT_LEFTUP: pov = 31500; break;
                default: break;
            }
        }
        *reinterpret_cast<DWORD*>(base + 32) = pov;

        if (g_cfg.enableLog) {
            static DWORD last = 0; DWORD now = GetTickCount();
            if (now - last > 250) {
                last = now;
                char btns[256] = {0}; int n = 0;
                for (int i = 0; i < nButtons && n < (int)sizeof(btns) - 8; ++i)
                    if (SDL_JoystickGetButton(g_joy, i)) n += snprintf(btns + n, sizeof(btns) - n, "%d,", i);
                LOG("  [raw] axes(0-3)=%.2f,%.2f,%.2f,%.2f pov=%lu pressed=[%s]", lx, ly, rx, ry, pov, btns);
            }
        }
        return;
    }

    // ---- normal path: SDL_GameController with a real mapping ----
    auto W = [&](int ofs, long val){ *reinterpret_cast<long*>(base + ofs) = val; };

    // --- movement: left stick -> X / Y ---
    float lx = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTX) / 32767.f;
    float ly = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTY) / 32767.f;
    ApplyMaxInput(lx, ly, g_cfg.moveMaxRange);
    RadialDeadzone(lx, ly, g_cfg.moveDeadzone);
    AxisSnap(lx, ly);
    if (g_cfg.invertMoveY) ly = -ly;
    W(0, AxisDI(lx));
    W(4, AxisDI(ly));

    // --- camera: right stick -> Rx / Ry ---
    float camMul = g_cfg.cameraSensitivity / 50.f;
    if (camMul < 0.f) camMul = 0.f;
    float rx = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTX) / 32767.f;
    float ry = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTY) / 32767.f;
    ApplyMaxInput(rx, ry, g_cfg.cameraMaxRange);
    RadialDeadzone(rx, ry, g_cfg.cameraDeadzone);
    AxisSnap(rx, ry);
    rx *= camMul;
    ry *= camMul;
    if (g_cfg.invertCameraX) rx = -rx;
    if (g_cfg.invertCameraY) ry = -ry;

    // --- triggers -> combined axis ---
    //float rt = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) / 32767.f;
    //float lt = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT)  / 32767.f;
    const Sint16 triggerThreshold = 16384; // Approximately 50% pressed
    Sint16 rt = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    Sint16 lt = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT);

    // Left Trigger
    if (lt >= triggerThreshold)
    {
        base[48 + g_cfg.btnLT] = 0x80;
    }

    // Right Trigger
    if (rt >= triggerThreshold)
    {
        base[48 + g_cfg.btnRT] = 0x80;
    }

    if (rt < 0.f) rt = 0.f;
    if (lt < 0.f) lt = 0.f;
    float z = g_cfg.swapTriggers ? (lt - rt) : (rt - lt);

    if (!g_cfg.cameraOnZRz) {
        W(12, AxisDI(rx));   // Rx
        W(16, AxisDI(ry));   // Ry
        W(8,  AxisDI(z));    // Z
    } else {
        W(8,  AxisDI(rx));
        W(20, AxisDI(ry));   // Rz
        W(12, AxisDI(z));    // Rx (triggers)
    }

    // --- buttons ---
    auto set = [&](SDL_GameControllerButton b, int idx){
        if (idx < 0 || idx >= buttonsCapacity) return;
        if (!SDL_GameControllerGetButton(g_pad, b)) return;
        int ofs = 48 + idx;
        base[ofs] = 0x80;
    };
    set(SDL_CONTROLLER_BUTTON_A,             g_cfg.btnX);
    set(SDL_CONTROLLER_BUTTON_B,             g_cfg.btnY);
    set(SDL_CONTROLLER_BUTTON_X,             g_cfg.btnA);
    set(SDL_CONTROLLER_BUTTON_Y,             g_cfg.btnB);
    set(SDL_CONTROLLER_BUTTON_LEFTSHOULDER,  g_cfg.btnLB);
    set(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, g_cfg.btnRB);
    // Start is fully repurposed for the MenuMode Escape hotkey when
    // enabled (see Proxy_HandleGlobalHotkeys) - don't also forward it to
    // the game as a joystick button in that case.
    if (!g_cfg.enableMenuMode)
    set(SDL_CONTROLLER_BUTTON_BACK,          g_cfg.btnBack);
    set(SDL_CONTROLLER_BUTTON_START,         g_cfg.btnStart);
    set(SDL_CONTROLLER_BUTTON_LEFTSTICK,     g_cfg.btnLS);
    set(SDL_CONTROLLER_BUTTON_RIGHTSTICK,    g_cfg.btnRS);

    // --- D-pad -> POV hat ---
    bool up    = SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_DPAD_UP);
    bool down  = SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    bool left  = SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    bool right = SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    DWORD pov = 0xFFFFFFFF;
    if      (up && right) pov = 4500;
    else if (right&&down) pov = 13500;
    else if (down&&left)  pov = 22500;
    else if (left&&up)    pov = 31500;
    else if (up)          pov = 0;
    else if (right)       pov = 9000;
    else if (down)        pov = 18000;
    else if (left)        pov = 27000;
    *reinterpret_cast<DWORD*>(base + 32) = pov;

    if (g_cfg.enableLog) {
        static DWORD last = 0; DWORD now = GetTickCount();
        if (now - last > 250) {
            last = now;
            LOG("  [fill cap=%d] LX=%.2f LY=%.2f RX=%.2f RY=%.2f Z=%.2f pov=%lu",
                buttonsCapacity, lx, ly, rx, ry, z, pov);
        }
    }
}

// =============================================================================
//  Menu-mouse mode (see [MenuMode] in PadOfTime.ini)
//
//  The game can't navigate menus with a controller at all. This turns a
//  button into an on/off toggle: while active, the right stick drives the
//  real Windows cursor via SendInput, and a second button fires left-clicks.
//  It doesn't touch DirectInput at all - it drives the OS cursor directly,
//  the same way any other application moving the mouse would.
// =============================================================================
// =============================================================================
//  Global hotkey: Start -> Escape (see [MenuMode] in PadOfTime.ini)
//
//  Active in every mode, independent of menu-mouse. The game doesn't answer
//  to Start as pause over DirectInput, so this sends a real Escape keypress
//  instead. Start is suppressed from the normal button forwarding in
//  Proxy_FillJoyBuffer when this is enabled, since it's fully repurposed.
// =============================================================================
static SDL_GameControllerButton g_startEscBtn = SDL_CONTROLLER_BUTTON_INVALID;
static bool g_startEscResolved = false;
static bool g_startEscWasDown  = false;

void Proxy_HandleGlobalHotkeys()
{
    if (!g_cfg.enableMenuMode) return;
    SelectActivePad();
    if (!g_pad) return;

    if (!g_startEscResolved) {
        g_startEscResolved = true;
        g_startEscBtn = SDL_GameControllerGetButtonFromString(g_cfg.escapeButton.c_str());
        if (g_startEscBtn == SDL_CONTROLLER_BUTTON_INVALID)
            LOG("MenuMode: EscapeButton '%s' is not a recognized button name - disabled",
                g_cfg.escapeButton.c_str());
    }
    if (g_startEscBtn == SDL_CONTROLLER_BUTTON_INVALID) return;

    SDL_GameControllerUpdate();
    bool down = SDL_GameControllerGetButton(g_pad, g_startEscBtn);
    if (down != g_startEscWasDown) {
        // Scan-code injection, not virtual-key: this game (like many
        // DirectInput-era titles) reads its keyboard through the DirectInput
        // GUID_SysKeyboard device, which works off hardware scan codes -
        // a plain VK_ESCAPE SendInput can silently fail to register there
        // even though GetAsyncKeyState/window messages would see it fine.
        static const WORD escScanCode = (WORD)MapVirtualKey(VK_ESCAPE, MAPVK_VK_TO_VSC);
        INPUT key = {};
        key.type = INPUT_KEYBOARD;
        key.ki.wScan = escScanCode;
        key.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
        SendInput(1, &key, sizeof(INPUT));
        g_startEscWasDown = down;
        if (g_cfg.enableLog) LOG("MenuMode Escape: %s (scancode=0x%02X)", down ? "DOWN" : "UP", escScanCode);
    }
}

// =============================================================================
//  Menu-mouse mode (see [MenuMode] in PadOfTime.ini)
//
//  The game can't navigate menus with a controller at all. ToggleButton
//  flips an on/off virtual mouse: while active, the right stick drives the
//  real Windows cursor via SendInput, and ClickButton fires left-clicks.
//  Doesn't touch DirectInput at all - it drives the OS cursor directly, the
//  same way any other application moving the mouse would.
// =============================================================================
static bool g_menuModeActive = false;
static bool g_toggleWasDown  = false;
static bool g_clickWasDown   = false;
static SDL_GameControllerButton g_toggleBtn = SDL_CONTROLLER_BUTTON_INVALID;
static SDL_GameControllerButton g_clickBtn  = SDL_CONTROLLER_BUTTON_INVALID;
static bool g_menuButtonsResolved = false;

bool Proxy_UpdateMenuMouse()
{
    if (!g_cfg.enableMenuMode) return false;

    SelectActivePad();
    if (!g_pad) return false;

    if (!g_menuButtonsResolved) {
        g_menuButtonsResolved = true;
        g_toggleBtn = SDL_GameControllerGetButtonFromString(g_cfg.mouseToggleButton.c_str());
        g_clickBtn  = SDL_GameControllerGetButtonFromString(g_cfg.mouseClickButton.c_str());
        if (g_toggleBtn == SDL_CONTROLLER_BUTTON_INVALID)
            LOG("MenuMode: ToggleButton '%s' is not a recognized button name - menu-mouse toggle disabled",
                g_cfg.mouseToggleButton.c_str());
        if (g_clickBtn == SDL_CONTROLLER_BUTTON_INVALID)
            LOG("MenuMode: ClickButton '%s' is not a recognized button name - left-click disabled",
                g_cfg.mouseClickButton.c_str());
    }
    if (g_toggleBtn == SDL_CONTROLLER_BUTTON_INVALID) return false;

    SDL_GameControllerUpdate();

    bool toggleDown = SDL_GameControllerGetButton(g_pad, g_toggleBtn);
    if (toggleDown && !g_toggleWasDown) {
        g_menuModeActive = !g_menuModeActive;
        LOG("MenuMode: %s", g_menuModeActive ? "ON (right stick = mouse)" : "OFF (pad back to normal)");
        if (!g_menuModeActive && g_clickWasDown) {
            // Releasing while still "clicking" - make sure we don't leave
            // the mouse button stuck down.
            INPUT up = {}; up.type = INPUT_MOUSE; up.mi.dwFlags = MOUSEEVENTF_LEFTUP;
            SendInput(1, &up, sizeof(INPUT));
            g_clickWasDown = false;
        }
    }
    g_toggleWasDown = toggleDown;

    if (!g_menuModeActive) return false;

    // --- move the real cursor from the right stick ---
    static DWORD lastTick = 0;
    DWORD now = GetTickCount();
    float dt = lastTick ? (now - lastTick) / 1000.0f : 0.0f;
    if (dt > 0.1f) dt = 0.1f;   // clamp: first call / hitch shouldn't teleport the cursor
    lastTick = now;

    float rx = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTX) / 32767.f;
    float ry = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTY) / 32767.f;
    RadialDeadzone(rx, ry, g_cfg.menuDeadzone);
    if (g_cfg.menuInvertY) ry = -ry;

    static float accumX = 0.0f, accumY = 0.0f;
    accumX += rx * g_cfg.menuCursorSpeed * dt;
    accumY += ry * g_cfg.menuCursorSpeed * dt;
    int moveX = (int)accumX;   // truncate towards zero, keep the fractional
    int moveY = (int)accumY;   // remainder so slow drift isn't rounded away
    accumX -= moveX;
    accumY -= moveY;

    if (moveX != 0 || moveY != 0) {
        INPUT mv = {};
        mv.type = INPUT_MOUSE;
        mv.mi.dx = moveX;
        mv.mi.dy = moveY;
        mv.mi.dwFlags = MOUSEEVENTF_MOVE;
        SendInput(1, &mv, sizeof(INPUT));
    }

    // --- left click ---
    if (g_clickBtn != SDL_CONTROLLER_BUTTON_INVALID) {
        bool clickDown = SDL_GameControllerGetButton(g_pad, g_clickBtn);
        if (clickDown != g_clickWasDown) {
            INPUT click = {};
            click.type = INPUT_MOUSE;
            click.mi.dwFlags = clickDown ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
            SendInput(1, &click, sizeof(INPUT));
            g_clickWasDown = clickDown;
        }
    }

    if (g_cfg.enableLog) {
        static DWORD lastLog = 0;
        if (now - lastLog > 250) {
            lastLog = now;
            LOG("  [menu-mouse] rx=%.2f ry=%.2f move=(%d,%d) click=%d", rx, ry, moveX, moveY, g_clickWasDown);
        }
    }
    return true;
}
