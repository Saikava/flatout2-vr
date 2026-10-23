#include "controllers.h"

#include "background.h"
#include "game.h"
#include "pacing.h"
#include "record.h"
#include "util.h"
#include "xr.h"

#include <windows.h>
#include <initguid.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <xinput.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// What the game does with its game controllers. Its input (0x55B240) makes its DirectInput (0x561270), lists the game
// controllers (0x5612C0: EnumDevices(DI8DEVCLASS_GAMECTRL, attached only)) and opens the first two (0x5613B0): their
// type (joystick, gamepad or wheel), name and product GUID go to its list of controllers after the keyboard (the
// options' Controller: 0 the keyboard, 1 and 2 these; the chosen one's GUID is kept as ControllerGuid and its controls
// in Savegame\am<GUID>.dat). Each is set up for DIJOYSTATE2 with every axis in -10000..10000 (0x561590), for the game's
// exclusive use while its window is in front (0x56156A: SetCooperativeLevel(window, DISCL_EXCLUSIVE |
// DISCL_FOREGROUND)) and without autocentre. A controller without saved controls gets DirectInput's action mapper's
// (BuildActionMap with the genre "driving race"; 0x55E190), which the game reads back through GetObjectInfo: the axis
// (by its GUID) or the button (by its instance) behind each of its 13 actions, kept as 20 controls (the consoles'
// buttons: A the handbrake, B the nitro, ... START, the arrows the throttle, brake and steering), each an axis, a button
// or a key of the keyboard (the defaults of those the controller lacks). Every frame the chosen controller is polled
// (acquired again when that fails) and read (GetDeviceState; 0x561610). The menus take only the keyboard (see
// ControllersMenuKeys). With Force feedback on in the options,
// the game plays DirectInput effects on a controller that has force-feedback axes (EnumObjects: DIDOI_FFACTUATOR):
// constant force, spring, damper and periodic ones; their gain (DIPROP_FFGAIN) is the options' Force level, and the
// actuators are switched on and off with the option (SendForceFeedbackCommand).
//
// The mod puts its own DirectInput between the game and the game's (InputProxy, from just before the game lists its
// controllers), which
// - adds "VR controllers" to the list (VrPad): the VR controllers read as a gamepad, with force feedback played as their
//   vibration;
// - in VR opens the game controllers for use without the window in front (the window on the monitor is not what the
//   player looks at: started from the headset, it may never have had the focus, and the controller did nothing);
// - gives an Xbox controller force feedback (PadProxy): DirectInput has none for them, only XInput can make them
//   vibrate.
// The VR controllers and Xbox controllers also press the keys of the menus and the race's pause (ControllersMenuKeys).

namespace {

// ---- force feedback -----------------------------------------------------------------------------------------------
// The effects a controller plays, summed into the strength of a gamepad's two motors: the heavy (low-frequency) one and
// the light one. A gamepad or a VR controller cannot push back as a wheel does: what only changes how a wheel feels
// (the springs and dampers that centre it) is left out; jolts and vibrations are kept.
struct Motors
{
    float low = 0, high = 0;
};

class ForceFeedback;

const char* EffectName(REFGUID g)
{
    if (g == GUID_ConstantForce) return "constant";
    if (g == GUID_RampForce) return "ramp";
    if (g == GUID_Square) return "square";
    if (g == GUID_Sine) return "sine";
    if (g == GUID_Triangle) return "triangle";
    if (g == GUID_SawtoothUp) return "saw up";
    if (g == GUID_SawtoothDown) return "saw down";
    if (g == GUID_Spring) return "spring";
    if (g == GUID_Damper) return "damper";
    if (g == GUID_Inertia) return "inertia";
    if (g == GUID_Friction) return "friction";
    if (g == GUID_CustomForce) return "custom";
    return nullptr;
}

DWORD EffectType(REFGUID g)
{
    if (g == GUID_ConstantForce) return DIEFT_CONSTANTFORCE;
    if (g == GUID_RampForce) return DIEFT_RAMPFORCE;
    if (g == GUID_Spring || g == GUID_Damper || g == GUID_Inertia || g == GUID_Friction) return DIEFT_CONDITION;
    if (g == GUID_CustomForce) return DIEFT_CUSTOMFORCE;
    return DIEFT_PERIODIC;
}

const GUID* const kEffects[] = { &GUID_ConstantForce, &GUID_RampForce, &GUID_Square, &GUID_Sine, &GUID_Triangle, &GUID_SawtoothUp,
                                 &GUID_SawtoothDown, &GUID_Spring, &GUID_Damper, &GUID_Inertia, &GUID_Friction };

class Effect : public IDirectInputEffect
{
public:
    Effect(ForceFeedback* owner, REFGUID type) : m_owner(owner), m_type(type) {}
    void Detach() { m_owner = nullptr; }
    void Add(double now, Motors& m);  // what it plays now
    void Describe(char* out, size_t n) const;

    // IUnknown
    STDMETHOD(QueryInterface)(REFIID iid, LPVOID* out) override
    {
        if (!out) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == IID_IDirectInputEffect)
        {
            *out = this;
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return ++m_refs; }
    STDMETHOD_(ULONG, Release)() override;
    // IDirectInputEffect
    STDMETHOD(Initialize)(HINSTANCE, DWORD, REFGUID) override { return DI_OK; }
    STDMETHOD(GetEffectGuid)(LPGUID out) override
    {
        if (!out) return E_POINTER;
        *out = m_type;
        return DI_OK;
    }
    STDMETHOD(GetParameters)(LPDIEFFECT e, DWORD flags) override;
    STDMETHOD(SetParameters)(LPCDIEFFECT e, DWORD flags) override;
    STDMETHOD(Start)(DWORD iterations, DWORD flags) override;
    STDMETHOD(Stop)() override;
    STDMETHOD(GetEffectStatus)(LPDWORD out) override
    {
        if (!out) return E_POINTER;
        *out = m_playing ? DIEGES_PLAYING : 0;
        return DI_OK;
    }
    STDMETHOD(Download)() override { return DI_OK; }
    STDMETHOD(Unload)() override
    {
        m_playing = false;
        return DI_OK;
    }
    STDMETHOD(Escape)(LPDIEFFESCAPE) override { return DIERR_UNSUPPORTED; }

private:
    ULONG m_refs = 1;
    ForceFeedback* m_owner;
    GUID m_type;
    // The parameters as last set (DIEFFECT's, the envelope's and the type's own).
    DWORD m_flags = 0, m_duration = INFINITE, m_samplePeriod = 0, m_gain = 10000, m_trigger = DIEB_NOTRIGGER, m_triggerRepeat = 0,
          m_startDelay = 0;
    DWORD m_axisCount = 0, m_axes[2] = {};
    LONG m_direction[2] = {};
    bool m_hasEnvelope = false;
    DIENVELOPE m_envelope = { sizeof(DIENVELOPE) };
    std::vector<BYTE> m_specific;
    bool m_playing = false;
    double m_started = 0;
    DWORD m_iterations = 1;
    LONG m_loggedConstant = 0;
};

class ForceFeedback
{
public:
    explicit ForceFeedback(const char* name) : m_name(name) {}
    ~ForceFeedback()
    {
        for (Effect* e : m_effects) e->Detach();
    }
    const char* Name() const { return m_name; }

    HRESULT CreateEffect(REFGUID type, LPCDIEFFECT params, LPDIRECTINPUTEFFECT* out)
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!EffectName(type) || type == GUID_CustomForce) return DIERR_DEVICENOTREG;
        Effect* e = new Effect(this, type);
        if (params) e->SetParameters(params, DIEP_ALLPARAMS);
        m_effects.push_back(e);
        *out = e;
        if (g_cfg.testControllersLog)
        {
            char d[160];
            e->Describe(d, sizeof(d));
            Log("force feedback (%s): effect %p made: %s", m_name, (void*)e, d);
        }
        return DI_OK;
    }
    void Forget(Effect* e) { m_effects.erase(std::remove(m_effects.begin(), m_effects.end(), e), m_effects.end()); }

    HRESULT EnumEffects(LPDIENUMEFFECTSCALLBACKA cb, LPVOID ref, DWORD type)
    {
        if (!cb) return DIERR_INVALIDPARAM;
        for (const GUID* g : kEffects)
        {
            DIEFFECTINFOA info = { sizeof(info) };
            GetEffectInfo(&info, *g);
            if (type != DIEFT_ALL && DIEFT_GETTYPE(info.dwEffType) != DIEFT_GETTYPE(type)) continue;
            if (cb(&info, ref) == DIENUM_STOP) break;
        }
        return DI_OK;
    }
    HRESULT GetEffectInfo(LPDIEFFECTINFOA info, REFGUID type)
    {
        if (!info || info->dwSize != sizeof(DIEFFECTINFOA)) return DIERR_INVALIDPARAM;
        const char* name = EffectName(type);
        if (!name) return DIERR_DEVICENOTREG;
        info->guid = type;
        info->dwEffType = EffectType(type) | DIEFT_FFATTACK | DIEFT_FFFADE;
        info->dwStaticParams = info->dwDynamicParams = DIEP_DURATION | DIEP_GAIN | DIEP_AXES | DIEP_DIRECTION | DIEP_ENVELOPE |
                                                       DIEP_TYPESPECIFICPARAMS | DIEP_STARTDELAY;
        strncpy(info->tszName, name, MAX_PATH - 1);
        return DI_OK;
    }
    HRESULT GetState(LPDWORD out)
    {
        if (!out) return E_POINTER;
        *out = (m_actuators ? DIGFFS_ACTUATORSON : DIGFFS_ACTUATORSOFF) | (m_paused ? DIGFFS_PAUSED : 0) | DIGFFS_POWERON |
               (m_effects.empty() ? DIGFFS_EMPTY : 0);
        return DI_OK;
    }
    void StopAll()
    {
        for (Effect* e : m_effects) e->Stop();
    }
    HRESULT Command(DWORD flags)
    {
        if (g_cfg.testControllersLog) Log("force feedback (%s): command %X", m_name, (unsigned)flags);
        if (flags & (DISFFC_RESET | DISFFC_STOPALL)) StopAll();
        if (flags & DISFFC_PAUSE) m_paused = true;
        if (flags & (DISFFC_CONTINUE | DISFFC_RESET)) m_paused = false;
        if (flags & DISFFC_SETACTUATORSON) m_actuators = true;
        if (flags & DISFFC_SETACTUATORSOFF) m_actuators = false;
        return DI_OK;
    }
    HRESULT EnumCreated(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK cb, LPVOID ref)
    {
        if (!cb) return DIERR_INVALIDPARAM;
        std::vector<Effect*> list = m_effects;
        for (Effect* e : list)
            if (cb(e, ref) == DIENUM_STOP) break;
        return DI_OK;
    }

    DWORD gain = 10000;  // DIPROP_FFGAIN
    // The motors now: the effects' sum times the controller's gain (nothing with the actuators off or paused).
    Motors Output(double now)
    {
        Motors m;
        if (!m_actuators || m_paused) return m;
        for (Effect* e : m_effects) e->Add(now, m);
        float g = (float)gain / 10000.0f * g_cfg.vrRumble;
        m.low = std::min(m.low * g, 1.0f);
        m.high = std::min(m.high * g, 1.0f);
        return m;
    }

private:
    const char* m_name;
    std::vector<Effect*> m_effects;
    bool m_actuators = true, m_paused = false;
};

STDMETHODIMP_(ULONG) Effect::Release()
{
    ULONG n = --m_refs;
    if (n == 0)
    {
        if (m_owner) m_owner->Forget(this);
        delete this;
    }
    return n;
}

STDMETHODIMP Effect::SetParameters(LPCDIEFFECT e, DWORD flags)
{
    if (!e) return DIERR_INVALIDPARAM;
    if (flags & DIEP_DURATION) m_duration = e->dwDuration;
    if (flags & DIEP_SAMPLEPERIOD) m_samplePeriod = e->dwSamplePeriod;
    if (flags & DIEP_GAIN) m_gain = e->dwGain;
    if (flags & DIEP_TRIGGERBUTTON) m_trigger = e->dwTriggerButton;
    if (flags & DIEP_TRIGGERREPEATINTERVAL) m_triggerRepeat = e->dwTriggerRepeatInterval;
    if ((flags & DIEP_AXES) && e->rgdwAxes)
    {
        m_axisCount = std::min<DWORD>(e->cAxes, 2);
        memcpy(m_axes, e->rgdwAxes, m_axisCount * sizeof(DWORD));
    }
    if ((flags & DIEP_DIRECTION) && e->rglDirection)
    {
        m_flags = e->dwFlags;
        memcpy(m_direction, e->rglDirection, std::min<DWORD>(e->cAxes, 2) * sizeof(LONG));
    }
    if (flags & DIEP_ENVELOPE)
    {
        m_hasEnvelope = e->lpEnvelope != nullptr;
        if (m_hasEnvelope) m_envelope = *e->lpEnvelope;
    }
    if ((flags & DIEP_TYPESPECIFICPARAMS) && e->lpvTypeSpecificParams)
        m_specific.assign((const BYTE*)e->lpvTypeSpecificParams, (const BYTE*)e->lpvTypeSpecificParams + e->cbTypeSpecificParams);
    if ((flags & DIEP_STARTDELAY) && e->dwSize >= sizeof(DIEFFECT)) m_startDelay = e->dwStartDelay;
    // (Logged without the springs' and dampers' and the constant force's small changes, which come every frame.)
    DWORD type = EffectType(m_type);
    LONG constant = type == DIEFT_CONSTANTFORCE && m_specific.size() >= sizeof(DICONSTANTFORCE) ? ((const DICONSTANTFORCE*)m_specific.data())->lMagnitude : 0;
    if (g_cfg.testControllersLog && m_owner && type != DIEFT_CONDITION && (type != DIEFT_CONSTANTFORCE || std::abs(constant - m_loggedConstant) >= 1000))
    {
        m_loggedConstant = constant;
        char d[160];
        Describe(d, sizeof(d));
        Log("force feedback (%s): effect %p set (%X): %s", m_owner->Name(), (void*)this, (unsigned)flags, d);
    }
    if (flags & DIEP_START) Start(1, 0);
    return DI_OK;
}

STDMETHODIMP Effect::GetParameters(LPDIEFFECT e, DWORD flags)
{
    if (!e) return DIERR_INVALIDPARAM;
    if (flags & DIEP_DURATION) e->dwDuration = m_duration;
    if (flags & DIEP_SAMPLEPERIOD) e->dwSamplePeriod = m_samplePeriod;
    if (flags & DIEP_GAIN) e->dwGain = m_gain;
    if (flags & DIEP_TRIGGERBUTTON) e->dwTriggerButton = m_trigger;
    if (flags & DIEP_TRIGGERREPEATINTERVAL) e->dwTriggerRepeatInterval = m_triggerRepeat;
    if (flags & (DIEP_AXES | DIEP_DIRECTION))
    {
        if (e->cAxes < m_axisCount)
        {
            e->cAxes = m_axisCount;
            return DIERR_MOREDATA;
        }
        e->cAxes = m_axisCount;
        if ((flags & DIEP_AXES) && e->rgdwAxes) memcpy(e->rgdwAxes, m_axes, m_axisCount * sizeof(DWORD));
        if ((flags & DIEP_DIRECTION) && e->rglDirection)
        {
            memcpy(e->rglDirection, m_direction, m_axisCount * sizeof(LONG));
            e->dwFlags = (e->dwFlags & ~(DWORD)(DIEFF_CARTESIAN | DIEFF_POLAR | DIEFF_SPHERICAL)) |
                         (m_flags & (DIEFF_CARTESIAN | DIEFF_POLAR | DIEFF_SPHERICAL));
        }
    }
    if ((flags & DIEP_ENVELOPE) && e->lpEnvelope)
    {
        if (m_hasEnvelope)
            *e->lpEnvelope = m_envelope;
        else
            e->lpEnvelope = nullptr;
    }
    if (flags & DIEP_TYPESPECIFICPARAMS)
    {
        if (e->cbTypeSpecificParams < m_specific.size())
        {
            e->cbTypeSpecificParams = (DWORD)m_specific.size();
            return DIERR_MOREDATA;
        }
        e->cbTypeSpecificParams = (DWORD)m_specific.size();
        if (e->lpvTypeSpecificParams && !m_specific.empty()) memcpy(e->lpvTypeSpecificParams, m_specific.data(), m_specific.size());
    }
    if ((flags & DIEP_STARTDELAY) && e->dwSize >= sizeof(DIEFFECT)) e->dwStartDelay = m_startDelay;
    return DI_OK;
}

STDMETHODIMP Effect::Start(DWORD iterations, DWORD flags)
{
    if ((flags & DIES_SOLO) && m_owner) m_owner->StopAll();
    m_playing = true;
    m_started = NowSeconds();
    m_iterations = iterations ? iterations : 1;
    if (g_cfg.testControllersLog && m_owner) Log("force feedback (%s): effect %p started (%u times)", m_owner->Name(), (void*)this, (unsigned)iterations);
    return DI_OK;
}

STDMETHODIMP Effect::Stop()
{
    if (g_cfg.testControllersLog && m_owner && m_playing) Log("force feedback (%s): effect %p stopped", m_owner->Name(), (void*)this);
    m_playing = false;
    return DI_OK;
}

void Effect::Describe(char* out, size_t n) const
{
    int len = snprintf(out, n, "%s, duration %d ms, gain %u, delay %u ms", EffectName(m_type), m_duration == INFINITE ? -1 : (int)(m_duration / 1000),
                       (unsigned)m_gain, (unsigned)(m_startDelay / 1000));
    if (len < 0 || (size_t)len >= n) return;
    out += len, n -= len;
    if (m_hasEnvelope)
    {
        len = snprintf(out, n, ", attack %u for %u ms, fade %u for %u ms", (unsigned)m_envelope.dwAttackLevel, (unsigned)(m_envelope.dwAttackTime / 1000),
                       (unsigned)m_envelope.dwFadeLevel, (unsigned)(m_envelope.dwFadeTime / 1000));
        if (len < 0 || (size_t)len >= n) return;
        out += len, n -= len;
    }
    DWORD type = EffectType(m_type);
    if (type == DIEFT_CONSTANTFORCE && m_specific.size() >= sizeof(DICONSTANTFORCE))
        snprintf(out, n, ", magnitude %d", (int)((const DICONSTANTFORCE*)m_specific.data())->lMagnitude);
    else if (type == DIEFT_RAMPFORCE && m_specific.size() >= sizeof(DIRAMPFORCE))
        snprintf(out, n, ", ramp %d to %d", (int)((const DIRAMPFORCE*)m_specific.data())->lStart, (int)((const DIRAMPFORCE*)m_specific.data())->lEnd);
    else if (type == DIEFT_PERIODIC && m_specific.size() >= sizeof(DIPERIODIC))
    {
        const DIPERIODIC& p = *(const DIPERIODIC*)m_specific.data();
        snprintf(out, n, ", magnitude %u, offset %d, period %u ms", (unsigned)p.dwMagnitude, (int)p.lOffset, (unsigned)(p.dwPeriod / 1000));
    }
    else if (type == DIEFT_CONDITION && m_specific.size() >= sizeof(DICONDITION))
    {
        const DICONDITION& c = *(const DICONDITION*)m_specific.data();
        snprintf(out, n, ", coefficient %d/%d, offset %d, deadband %d", (int)c.lPositiveCoefficient, (int)c.lNegativeCoefficient, (int)c.lOffset,
                 (int)c.lDeadBand);
    }
}

// What a wheel would feel, as a gamepad's motors. FlatOut 2 plays a constant force (the steering's weight, changed every
// frame: mostly below a tenth of full force while driving, full in crashes and big slides), a spring and a damper
// (centring the wheel), a square wave of 100 ms (the road's roughness: a few ten-thousandths) and, for a hit, a square
// wave of 75 ms for 150 ms as strong as the hit. The constant force's strong part (past a fifth) and the slow waves
// rumble the heavy motor (the slow waves the light one at half), fast waves (a buzz) the light one; springs and dampers
// nothing.
void Effect::Add(double now, Motors& m)
{
    if (!m_playing) return;
    double t = (now - m_started) * 1e6 - m_startDelay;  // microseconds into it
    if (t < 0) return;
    if (m_duration != INFINITE)
    {
        if (m_iterations != INFINITE && t >= (double)m_duration * m_iterations)
        {
            m_playing = false;
            return;
        }
        if (m_duration > 0) t = fmod(t, (double)m_duration);
    }
    DWORD type = EffectType(m_type);
    float level = 0;
    bool heavy = true;
    bool strongPart = false;
    if (type == DIEFT_CONSTANTFORCE && m_specific.size() >= sizeof(DICONSTANTFORCE))
    {
        level = (float)std::abs(((const DICONSTANTFORCE*)m_specific.data())->lMagnitude);
        strongPart = true;
    }
    else if (type == DIEFT_RAMPFORCE && m_specific.size() >= sizeof(DIRAMPFORCE))
    {
        const DIRAMPFORCE& r = *(const DIRAMPFORCE*)m_specific.data();
        float f = m_duration != INFINITE && m_duration > 0 ? (float)(t / m_duration) : 0.0f;
        level = std::abs((float)r.lStart + ((float)r.lEnd - (float)r.lStart) * f);
        strongPart = true;
    }
    else if (type == DIEFT_PERIODIC && m_specific.size() >= sizeof(DIPERIODIC))
    {
        const DIPERIODIC& p = *(const DIPERIODIC*)m_specific.data();
        level = (float)p.dwMagnitude;
        heavy = p.dwPeriod >= 40000;  // 25 Hz and slower
    }
    else
        return;
    if (m_hasEnvelope)
    {
        if (m_envelope.dwAttackTime > 0 && t < m_envelope.dwAttackTime)
            level = (float)m_envelope.dwAttackLevel + (level - (float)m_envelope.dwAttackLevel) * (float)(t / m_envelope.dwAttackTime);
        double left = m_duration != INFINITE ? (double)m_duration - t : 1e12;
        if (m_envelope.dwFadeTime > 0 && left < m_envelope.dwFadeTime)
            level = (float)m_envelope.dwFadeLevel + (level - (float)m_envelope.dwFadeLevel) * (float)(left / m_envelope.dwFadeTime);
    }
    level = std::clamp(level / 10000.0f, 0.0f, 1.0f);
    if (strongPart) level = std::max(level - 0.2f, 0.0f) / 0.8f;
    level *= (float)m_gain / 10000.0f;
    if (heavy)
    {
        m.low += level;
        if (!strongPart) m.high += 0.5f * level;
    }
    else
        m.high += level;
}

// ---- XInput ---------------------------------------------------------------------------------------------------------
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using XInputSetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);
XInputGetStateFn g_xinputGetState;
XInputSetStateFn g_xinputSetState;

bool LoadXInput()
{
    static bool tried;
    if (tried) return g_xinputSetState != nullptr;
    tried = true;
    for (const wchar_t* name : { L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll" })
        if (HMODULE m = LoadLibraryW(name))
        {
            g_xinputGetState = (XInputGetStateFn)GetProcAddress(m, "XInputGetState");
            g_xinputSetState = (XInputSetStateFn)GetProcAddress(m, "XInputSetState");
            if (g_xinputGetState && g_xinputSetState) return true;
        }
    g_xinputGetState = nullptr;
    g_xinputSetState = nullptr;
    return false;
}

// Which XInput users have a controller. Asking about a user without one can take a millisecond (more than a frame can
// spare), so those are looked at again only every two seconds, and not while the player drives (ControllersMenuKeys
// does it; a user whose controller stops answering is crossed out).
bool g_xinputConnected[4];
double g_xinputScanned = -10;

void ScanXInput(bool mayTakeTime)
{
    double now = NowSeconds();
    if (!mayTakeTime || now - g_xinputScanned < 2.0) return;
    g_xinputScanned = now;
    for (DWORD u = 0; u < 4; u++)
    {
        if (g_xinputConnected[u]) continue;
        XINPUT_STATE x;
        g_xinputConnected[u] = g_xinputGetState(u, &x) == ERROR_SUCCESS;
    }
}

// The XInput user of the n-th Xbox controller the game opened: the n-th one connected (DirectInput and XInput do not
// tell which is which; with one controller it is that one). -1: none.
int XInputUser(int n)
{
    for (int u = 0; u < 4; u++)
        if (g_xinputConnected[u] && n-- == 0) return u;
    return -1;
}

std::string Lower(std::string s)
{
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// An Xbox controller (or one that works as one): XInput's own controllers have "IG_" in their device path; a name
// like "Xbox 360 Controller" otherwise (the Steam release's controllers, which come from SDL).
bool IsXInputController(IDirectInputDevice8A* d)
{
    DIPROPGUIDANDPATH gp = {};
    gp.diph.dwSize = sizeof(gp);
    gp.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    gp.diph.dwHow = DIPH_DEVICE;
    if (SUCCEEDED(d->GetProperty(DIPROP_GUIDANDPATH, &gp.diph)))
    {
        std::wstring path = gp.wszPath;
        for (wchar_t& c : path) c = (wchar_t)towlower(c);
        if (path.find(L"ig_") != std::wstring::npos) return true;
    }
    DIDEVICEINSTANCEA di = { sizeof(di) };
    if (SUCCEEDED(d->GetDeviceInfo(&di)))
    {
        std::string name = Lower(di.tszProductName) + " " + Lower(di.tszInstanceName);
        return name.find("xbox") != std::string::npos || name.find("xinput") != std::string::npos || name.find("x-box") != std::string::npos;
    }
    return false;
}

bool InVr() { return xr::Available(); }

// The property a DIPROP_* "GUID" names (DirectInput passes small numbers as their addresses).
uintptr_t Prop(REFGUID g) { return (uintptr_t)&g; }

// ---- the game's game controllers -------------------------------------------------------------------------------
// A game controller as DirectInput gives it, passed through, but opened for use in the background in VR, and with
// force feedback made of XInput's vibration for an Xbox controller.
class PadProxy : public IDirectInputDevice8A
{
public:
    explicit PadProxy(IDirectInputDevice8A* inner) : m_inner(inner)
    {
        DIDEVICEINSTANCEA di = { sizeof(di) };
        if (SUCCEEDED(inner->GetDeviceInfo(&di))) m_name = di.tszProductName;
        DIDEVCAPS caps = { sizeof(caps) };
        bool ff = SUCCEEDED(inner->GetCapabilities(&caps)) && (caps.dwFlags & DIDC_FORCEFEEDBACK);
        if (!ff && g_cfg.vrRumble > 0 && IsXInputController(inner) && LoadXInput())
        {
            static int xinputPads;
            m_xinput = xinputPads++;
            m_ff = new ForceFeedback("Xbox controller");
        }
        Log("controllers: \"%s\" opened%s", m_name.c_str(),
            ff ? " (force feedback of its own)" : m_ff ? " (an Xbox controller: force feedback as XInput's vibration)" : "");
    }

    // IUnknown
    STDMETHOD(QueryInterface)(REFIID iid, LPVOID* out) override
    {
        if (!out) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == IID_IDirectInputDevice8A)
        {
            *out = this;
            AddRef();
            return S_OK;
        }
        return m_inner->QueryInterface(iid, out);
    }
    STDMETHOD_(ULONG, AddRef)() override { return ++m_refs; }
    STDMETHOD_(ULONG, Release)() override
    {
        ULONG n = --m_refs;
        if (n == 0)
        {
            if (m_ff) Vibrate({});
            delete m_ff;
            m_inner->Release();
            delete this;
        }
        return n;
    }
    // IDirectInputDevice8A
    STDMETHOD(GetCapabilities)(LPDIDEVCAPS caps) override
    {
        HRESULT hr = m_inner->GetCapabilities(caps);
        if (SUCCEEDED(hr) && m_ff && caps)
        {
            caps->dwFlags |= DIDC_FORCEFEEDBACK | DIDC_FFATTACK | DIDC_FFFADE;
            if (caps->dwSize >= sizeof(DIDEVCAPS))
            {
                caps->dwFFSamplePeriod = 10000;
                caps->dwFFMinTimeResolution = 1000;
                caps->dwFFDriverVersion = 1;
            }
        }
        return hr;
    }
    STDMETHOD(EnumObjects)(LPDIENUMDEVICEOBJECTSCALLBACKA cb, LPVOID ref, DWORD flags) override
    {
        if (!m_ff || !cb) return m_inner->EnumObjects(cb, ref, flags);
        // The X and Y axes are made force-feedback axes (the game plays effects only on a controller with some).
        struct Context
        {
            LPDIENUMDEVICEOBJECTSCALLBACKA cb;
            LPVOID ref;
            bool onlyActuators;
        } c = { cb, ref, (flags & DIDFT_FFACTUATOR) != 0 };
        return m_inner->EnumObjects(
            [](LPCDIDEVICEOBJECTINSTANCEA o, LPVOID p) -> BOOL {
                auto& c = *(Context*)p;
                DIDEVICEOBJECTINSTANCEA copy = *o;
                bool actuator = MarkActuator(copy);
                if (c.onlyActuators && !actuator) return DIENUM_CONTINUE;
                return c.cb(&copy, c.ref);
            },
            &c, flags & ~(DWORD)DIDFT_FFACTUATOR);
    }
    STDMETHOD(GetProperty)(REFGUID prop, LPDIPROPHEADER h) override
    {
        if (m_ff && h && h->dwSize >= sizeof(DIPROPDWORD))
        {
            if (Prop(prop) == Prop(DIPROP_FFGAIN)) return ((DIPROPDWORD*)h)->dwData = m_ff->gain, DI_OK;
            if (Prop(prop) == Prop(DIPROP_FFLOAD)) return ((DIPROPDWORD*)h)->dwData = 0, DI_OK;
            if (Prop(prop) == Prop(DIPROP_AUTOCENTER)) return ((DIPROPDWORD*)h)->dwData = m_autocenter, DI_OK;
        }
        return m_inner->GetProperty(prop, h);
    }
    STDMETHOD(SetProperty)(REFGUID prop, LPCDIPROPHEADER h) override
    {
        if (m_ff && h && h->dwSize >= sizeof(DIPROPDWORD))
        {
            if (Prop(prop) == Prop(DIPROP_FFGAIN))
            {
                m_ff->gain = std::min<DWORD>(((const DIPROPDWORD*)h)->dwData, 10000);
                return DI_OK;
            }
            if (Prop(prop) == Prop(DIPROP_AUTOCENTER))
            {
                m_autocenter = ((const DIPROPDWORD*)h)->dwData;
                m_inner->SetProperty(prop, h);
                return DI_OK;
            }
        }
        return m_inner->SetProperty(prop, h);
    }
    STDMETHOD(Acquire)() override { return m_inner->Acquire(); }
    STDMETHOD(Unacquire)() override
    {
        if (m_ff) Vibrate({});
        return m_inner->Unacquire();
    }
    STDMETHOD(GetDeviceState)(DWORD size, LPVOID data) override
    {
        Update();
        return m_inner->GetDeviceState(size, data);
    }
    STDMETHOD(GetDeviceData)(DWORD size, LPDIDEVICEOBJECTDATA data, LPDWORD inOut, DWORD flags) override
    {
        return m_inner->GetDeviceData(size, data, inOut, flags);
    }
    STDMETHOD(SetDataFormat)(LPCDIDATAFORMAT f) override { return m_inner->SetDataFormat(f); }
    STDMETHOD(SetEventNotification)(HANDLE e) override { return m_inner->SetEventNotification(e); }
    STDMETHOD(SetCooperativeLevel)(HWND window, DWORD flags) override
    {
        // In VR (and in background mode) the controller is the game's whether or not its window is in front.
        if ((InVr() || BackgroundMode()) && (flags & DISCL_FOREGROUND))
        {
            DWORD background = (flags & ~(DWORD)DISCL_FOREGROUND) | DISCL_BACKGROUND;
            HRESULT hr = m_inner->SetCooperativeLevel(window, background);
            Log("controllers: \"%s\" read with the game's window in the background too: %08X", m_name.c_str(), (unsigned)hr);
            if (SUCCEEDED(hr)) return hr;
        }
        return m_inner->SetCooperativeLevel(window, flags);
    }
    STDMETHOD(GetObjectInfo)(LPDIDEVICEOBJECTINSTANCEA o, DWORD obj, DWORD how) override
    {
        HRESULT hr = m_inner->GetObjectInfo(o, obj, how);
        if (SUCCEEDED(hr) && m_ff && o) MarkActuator(*o);
        return hr;
    }
    STDMETHOD(GetDeviceInfo)(LPDIDEVICEINSTANCEA i) override { return m_inner->GetDeviceInfo(i); }
    STDMETHOD(RunControlPanel)(HWND w, DWORD f) override { return m_inner->RunControlPanel(w, f); }
    STDMETHOD(Initialize)(HINSTANCE i, DWORD v, REFGUID g) override { return m_inner->Initialize(i, v, g); }
    STDMETHOD(CreateEffect)(REFGUID g, LPCDIEFFECT e, LPDIRECTINPUTEFFECT* out, LPUNKNOWN outer) override
    {
        return m_ff ? m_ff->CreateEffect(g, e, out) : m_inner->CreateEffect(g, e, out, outer);
    }
    STDMETHOD(EnumEffects)(LPDIENUMEFFECTSCALLBACKA cb, LPVOID ref, DWORD type) override
    {
        return m_ff ? m_ff->EnumEffects(cb, ref, type) : m_inner->EnumEffects(cb, ref, type);
    }
    STDMETHOD(GetEffectInfo)(LPDIEFFECTINFOA info, REFGUID g) override
    {
        return m_ff ? m_ff->GetEffectInfo(info, g) : m_inner->GetEffectInfo(info, g);
    }
    STDMETHOD(GetForceFeedbackState)(LPDWORD out) override { return m_ff ? m_ff->GetState(out) : m_inner->GetForceFeedbackState(out); }
    STDMETHOD(SendForceFeedbackCommand)(DWORD flags) override
    {
        return m_ff ? m_ff->Command(flags) : m_inner->SendForceFeedbackCommand(flags);
    }
    STDMETHOD(EnumCreatedEffectObjects)(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK cb, LPVOID ref, DWORD flags) override
    {
        return m_ff ? m_ff->EnumCreated(cb, ref) : m_inner->EnumCreatedEffectObjects(cb, ref, flags);
    }
    STDMETHOD(Escape)(LPDIEFFESCAPE e) override { return m_inner->Escape(e); }
    STDMETHOD(Poll)() override
    {
        Update();
        return m_inner->Poll();
    }
    STDMETHOD(SendDeviceData)(DWORD size, LPCDIDEVICEOBJECTDATA data, LPDWORD inOut, DWORD flags) override
    {
        return m_inner->SendDeviceData(size, data, inOut, flags);
    }
    STDMETHOD(EnumEffectsInFile)(LPCSTR file, LPDIENUMEFFECTSINFILECALLBACK cb, LPVOID ref, DWORD flags) override
    {
        return m_inner->EnumEffectsInFile(file, cb, ref, flags);
    }
    STDMETHOD(WriteEffectToFile)(LPCSTR file, DWORD n, LPDIFILEEFFECT e, DWORD flags) override
    {
        return m_inner->WriteEffectToFile(file, n, e, flags);
    }
    STDMETHOD(BuildActionMap)(LPDIACTIONFORMATA f, LPCSTR user, DWORD flags) override { return m_inner->BuildActionMap(f, user, flags); }
    STDMETHOD(SetActionMap)(LPDIACTIONFORMATA f, LPCSTR user, DWORD flags) override { return m_inner->SetActionMap(f, user, flags); }
    STDMETHOD(GetImageInfo)(LPDIDEVICEIMAGEINFOHEADERA h) override { return m_inner->GetImageInfo(h); }

private:
    static bool MarkActuator(DIDEVICEOBJECTINSTANCEA& o)
    {
        if (o.guidType != GUID_XAxis && o.guidType != GUID_YAxis) return false;
        o.dwType |= DIDFT_FFACTUATOR;
        o.dwFlags |= DIDOI_FFACTUATOR;
        if (o.dwSize >= sizeof(DIDEVICEOBJECTINSTANCEA))
        {
            o.dwFFMaxForce = 10000;
            o.dwFFForceResolution = 1;
        }
        return true;
    }

    // The effects into the motors (the game polls and reads the controller every frame).
    void Update()
    {
        if (m_ff) Vibrate(m_ff->Output(NowSeconds()));
    }
    void Vibrate(Motors m)
    {
        int user = XInputUser(m_xinput);
        XINPUT_VIBRATION v = { (WORD)(m.low * 65535.0f + 0.5f), (WORD)(m.high * 65535.0f + 0.5f) };
        double now = NowSeconds();
        if (user == m_user && v.wLeftMotorSpeed == m_sent.wLeftMotorSpeed && v.wRightMotorSpeed == m_sent.wRightMotorSpeed && now - m_sentAt < 1.0)
            return;
        if (m_user >= 0 && user != m_user)
        {
            XINPUT_VIBRATION off = {};
            g_xinputSetState((DWORD)m_user, &off);
        }
        m_user = user;
        m_sent = v;
        m_sentAt = now;
        if (user >= 0 && g_xinputSetState((DWORD)user, &v) == ERROR_DEVICE_NOT_CONNECTED) g_xinputConnected[user] = false;
        if (g_cfg.testControllersLog) Log("force feedback (Xbox controller): XInput user %d, motors %u %u", user, v.wLeftMotorSpeed, v.wRightMotorSpeed);
    }

    ULONG m_refs = 1;
    IDirectInputDevice8A* m_inner;
    std::string m_name;
    ForceFeedback* m_ff = nullptr;  // an Xbox controller's (nullptr: the controller's own force feedback, or none)
    DWORD m_autocenter = DIPROPAUTOCENTER_ON;
    int m_xinput = -1;              // which of the Xbox controllers the game opened
    int m_user = -1;                // the XInput user last sent to
    XINPUT_VIBRATION m_sent = {};
    double m_sentAt = -10;
};

// ---- the VR controllers as a gamepad ------------------------------------------------------------------------------
// Laid out like an Xbox controller in DirectInput (an XInput controller's buttons in the same order), but with the
// triggers on axes of their own: the left thumbstick X and Y, the left trigger Z, the right thumbstick Rx and Ry, the
// right trigger Rz; buttons 0 A, 1 B, 2 X, 3 Y, 4 and 5 the grips, 6 (none), 7 the menu button, 8 and 9 the thumbsticks
// pressed. The game's controls for it, until changed in its options (Configure controls): steering on the left
// thumbstick, accelerate and brake on the right and left triggers, nitro A, handbrake B, the camera X, reset Y, pause
// the menu button, look back the right thumbstick pressed, gears up and down (manual) the right and left grips.
const GUID kVrPadInstance = { 0x3C2E1F6A, 0x4D5B, 0x704F, { 0x9C, 0x2A, 0x46, 0x4F, 0x32, 0x56, 0x52, 0x31 } };
const GUID kVrPadProduct = { 0xC0B3F2F0, 0xA156, 0x524E, { 0x8F, 0x4F, 0x32, 0x56, 0x52, 0x43, 0x54, 0x4C } };
const char kVrPadName[] = "VR controllers";
constexpr int kVrPadRecordIndex = 8;  // its index among the controllers in input recordings (record.h)
constexpr int kAxes = 6, kButtons = 10;

struct PadObject
{
    const GUID* type;
    DWORD offset, id;
    const char* name;
    WORD usage;  // HID usage (generic desktop page for the axes)
    bool actuator;
};

const PadObject kObjects[] = {
    { &GUID_XAxis, DIJOFS_X, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(0), "Left thumbstick X", 0x30, true },
    { &GUID_YAxis, DIJOFS_Y, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(1), "Left thumbstick Y", 0x31, true },
    { &GUID_ZAxis, DIJOFS_Z, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(2), "Left trigger", 0x32, false },
    { &GUID_RxAxis, DIJOFS_RX, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(3), "Right thumbstick X", 0x33, false },
    { &GUID_RyAxis, DIJOFS_RY, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(4), "Right thumbstick Y", 0x34, false },
    { &GUID_RzAxis, DIJOFS_RZ, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(5), "Right trigger", 0x35, false },
    { &GUID_POV, DIJOFS_POV(0), DIDFT_POV | DIDFT_MAKEINSTANCE(0), "Hat switch", 0x39, false },
    { &GUID_Button, DIJOFS_BUTTON(0), DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(0), "A", 1, false },
    { &GUID_Button, DIJOFS_BUTTON(1), DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(1), "B", 2, false },
    { &GUID_Button, DIJOFS_BUTTON(2), DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(2), "X", 3, false },
    { &GUID_Button, DIJOFS_BUTTON(3), DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(3), "Y", 4, false },
    { &GUID_Button, DIJOFS_BUTTON(4), DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(4), "Left grip", 5, false },
    { &GUID_Button, DIJOFS_BUTTON(5), DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(5), "Right grip", 6, false },
    { &GUID_Button, DIJOFS_BUTTON(6), DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(6), "Button 7", 7, false },
    { &GUID_Button, DIJOFS_BUTTON(7), DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(7), "Menu", 8, false },
    { &GUID_Button, DIJOFS_BUTTON(8), DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(8), "Left thumbstick press", 9, false },
    { &GUID_Button, DIJOFS_BUTTON(9), DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(9), "Right thumbstick press", 10, false },
};

// The game's 13 actions (its DIACTION list at 0x69BB08) by their semantics, and what of the pad each gets by default
// (an index in kObjects). The rest (Accelerate/Brake on one axis, Player List) get nothing.
struct DefaultControl
{
    DWORD semantic;
    int object;
};
const DefaultControl kDefaults[] = {
    { DIAXIS_DRIVINGR_STEER, 0 },         // Steer: the left thumbstick
    { DIAXIS_DRIVINGR_ACCELERATE, 5 },    // Accelerate: the right trigger
    { DIAXIS_DRIVINGR_BRAKE, 2 },         // Brake: the left trigger
    { DIBUTTON_DRIVINGR_BRAKE, 8 },       // Handbrake: B
    { DIBUTTON_DRIVINGR_BOOST, 7 },       // Nitro / Eject: A
    { DIBUTTON_DRIVINGR_VIEW, 9 },        // Change View: X
    { DIBUTTON_DRIVINGR_SHIFTUP, 12 },    // Shift Up: the right grip
    { DIBUTTON_DRIVINGR_SHIFTDOWN, 11 },  // Shift Down: the left grip
    { 0xFF004400, 16 },                   // Reverse Camera (the game's own semantics): the right thumbstick pressed
    { 0xFF004401, 10 },                   // Reset: Y
    { DIBUTTON_DRIVINGR_PAUSE, 14 },      // Pause: the menu button
};
int DefaultObject(DWORD semantic)
{
    for (const DefaultControl& d : kDefaults)
        if (d.semantic == semantic) return d.object;
    return -1;
}

class VrPad : public IDirectInputDevice8A
{
public:
    VrPad() { Log("controllers: \"%s\" opened", kVrPadName); }

    // IUnknown
    STDMETHOD(QueryInterface)(REFIID iid, LPVOID* out) override
    {
        if (!out) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == IID_IDirectInputDevice8A)
        {
            *out = this;
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return ++m_refs; }
    STDMETHOD_(ULONG, Release)() override
    {
        ULONG n = --m_refs;
        if (n == 0)
        {
            Vibrate({});
            delete this;
        }
        return n;
    }
    // IDirectInputDevice8A
    STDMETHOD(GetCapabilities)(LPDIDEVCAPS caps) override
    {
        if (!caps || (caps->dwSize != sizeof(DIDEVCAPS) && caps->dwSize != sizeof(DIDEVCAPS_DX3))) return DIERR_INVALIDPARAM;
        DWORD size = caps->dwSize;
        memset(caps, 0, size);
        caps->dwSize = size;
        caps->dwFlags = DIDC_ATTACHED | DIDC_EMULATED | (Haptic() ? DIDC_FORCEFEEDBACK | DIDC_FFATTACK | DIDC_FFFADE : 0);
        caps->dwDevType = DevType();
        caps->dwAxes = kAxes;
        caps->dwButtons = kButtons;
        caps->dwPOVs = 1;
        if (size >= sizeof(DIDEVCAPS) && Haptic())
        {
            caps->dwFFSamplePeriod = 10000;
            caps->dwFFMinTimeResolution = 1000;
            caps->dwFFDriverVersion = 1;
        }
        return DI_OK;
    }
    STDMETHOD(EnumObjects)(LPDIENUMDEVICEOBJECTSCALLBACKA cb, LPVOID ref, DWORD flags) override
    {
        if (!cb) return DIERR_INVALIDPARAM;
        for (int i = 0; i < (int)std::size(kObjects); i++)
        {
            DIDEVICEOBJECTINSTANCEA o;
            Describe(i, o);
            DWORD kinds = DIDFT_GETTYPE(flags);
            if (kinds && !(DIDFT_GETTYPE(o.dwType) & kinds)) continue;
            if ((flags & DIDFT_FFACTUATOR) && !(o.dwType & DIDFT_FFACTUATOR)) continue;
            if (cb(&o, ref) == DIENUM_STOP) break;
        }
        return DI_OK;
    }
    STDMETHOD(GetProperty)(REFGUID prop, LPDIPROPHEADER h) override
    {
        if (!h) return DIERR_INVALIDPARAM;
        uintptr_t p = Prop(prop);
        int axis = Axis(h);
        if (p == Prop(DIPROP_RANGE) && h->dwSize >= sizeof(DIPROPRANGE))
        {
            int a = axis >= 0 ? axis : 0;
            ((DIPROPRANGE*)h)->lMin = m_min[a];
            ((DIPROPRANGE*)h)->lMax = m_max[a];
            return DI_OK;
        }
        if (h->dwSize >= sizeof(DIPROPDWORD))
        {
            DWORD& out = ((DIPROPDWORD*)h)->dwData;
            int a = axis >= 0 ? axis : 0;
            if (p == Prop(DIPROP_DEADZONE)) return out = m_deadzone[a], DI_OK;
            if (p == Prop(DIPROP_SATURATION)) return out = m_saturation[a], DI_OK;
            if (p == Prop(DIPROP_FFGAIN)) return out = m_ff.gain, DI_OK;
            if (p == Prop(DIPROP_FFLOAD)) return out = 0, DI_OK;
            if (p == Prop(DIPROP_AUTOCENTER)) return out = m_autocenter, DI_OK;
            if (p == Prop(DIPROP_BUFFERSIZE)) return out = 0, DI_OK;
            if (p == Prop(DIPROP_AXISMODE)) return out = DIPROPAXISMODE_ABS, DI_OK;
            if (p == Prop(DIPROP_GRANULARITY)) return out = 1, DI_OK;
            if (p == Prop(DIPROP_JOYSTICKID)) return out = 15, DI_OK;
            if (p == Prop(DIPROP_VIDPID)) return out = 0, DI_OK;
        }
        if ((p == Prop(DIPROP_PRODUCTNAME) || p == Prop(DIPROP_INSTANCENAME)) && h->dwSize >= sizeof(DIPROPSTRING))
        {
            MultiByteToWideChar(CP_ACP, 0, kVrPadName, -1, ((DIPROPSTRING*)h)->wsz, MAX_PATH);
            return DI_OK;
        }
        return DIERR_UNSUPPORTED;
    }
    STDMETHOD(SetProperty)(REFGUID prop, LPCDIPROPHEADER h) override
    {
        if (!h) return DIERR_INVALIDPARAM;
        uintptr_t p = Prop(prop);
        int axis = Axis(h);
        auto each = [&](auto set) {
            for (int a = 0; a < kAxes; a++)
                if (axis < 0 || axis == a) set(a);
        };
        if (p == Prop(DIPROP_RANGE) && h->dwSize >= sizeof(DIPROPRANGE))
        {
            const DIPROPRANGE& r = *(const DIPROPRANGE*)h;
            if (r.lMin >= r.lMax) return DIERR_INVALIDPARAM;
            each([&](int a) { m_min[a] = r.lMin, m_max[a] = r.lMax; });
            return DI_OK;
        }
        if (h->dwSize >= sizeof(DIPROPDWORD))
        {
            DWORD v = ((const DIPROPDWORD*)h)->dwData;
            if (p == Prop(DIPROP_DEADZONE)) return each([&](int a) { m_deadzone[a] = std::min<DWORD>(v, 10000); }), DI_OK;
            if (p == Prop(DIPROP_SATURATION)) return each([&](int a) { m_saturation[a] = std::min<DWORD>(v, 10000); }), DI_OK;
            if (p == Prop(DIPROP_FFGAIN)) return m_ff.gain = std::min<DWORD>(v, 10000), DI_OK;
            if (p == Prop(DIPROP_AUTOCENTER)) return m_autocenter = v, DI_OK;
            if (p == Prop(DIPROP_BUFFERSIZE) || p == Prop(DIPROP_AXISMODE) || p == Prop(DIPROP_CALIBRATIONMODE)) return DI_OK;
        }
        return DIERR_UNSUPPORTED;
    }
    STDMETHOD(Acquire)() override { return DI_OK; }
    STDMETHOD(Unacquire)() override
    {
        Vibrate({});
        return DI_OK;
    }
    STDMETHOD(GetDeviceState)(DWORD size, LPVOID data) override
    {
        if (!data || (size != sizeof(DIJOYSTATE2) && size != sizeof(DIJOYSTATE))) return DIERR_INVALIDPARAM;
        Update();
        DIJOYSTATE2 s;
        Read(s);
        memcpy(data, &s, size);
        // An input recording keeps it as it keeps the game's other controllers, and a replay gives it back.
        rec::ControllerState(kVrPadRecordIndex, size, data);
        if (!m_read) Log("controllers: the game reads \"%s\"", kVrPadName);
        m_read = true;
        if (g_cfg.testControllersLog && memcmp(&m_last, data, size) != 0)
        {
            const DIJOYSTATE2& d = *(const DIJOYSTATE2*)data;
            unsigned b = 0;
            for (int i = 0; i < kButtons; i++) b |= d.rgbButtons[i] ? 1u << i : 0;
            Log("controllers: \"%s\": X %ld Y %ld Z %ld Rx %ld Ry %ld Rz %ld, buttons %03X", kVrPadName, d.lX, d.lY, d.lZ, d.lRx, d.lRy, d.lRz, b);
            memcpy(&m_last, data, size);
        }
        return DI_OK;
    }
    STDMETHOD(GetDeviceData)(DWORD, LPDIDEVICEOBJECTDATA, LPDWORD inOut, DWORD) override
    {
        if (inOut) *inOut = 0;
        return DI_OK;
    }
    STDMETHOD(SetDataFormat)(LPCDIDATAFORMAT f) override
    {
        if (!f || (f->dwDataSize != sizeof(DIJOYSTATE2) && f->dwDataSize != sizeof(DIJOYSTATE)))
        {
            Log("controllers: \"%s\": the game's data format (%u bytes) is not a joystick's - it reads nothing", kVrPadName,
                f ? (unsigned)f->dwDataSize : 0);
            return DIERR_INVALIDPARAM;
        }
        return DI_OK;
    }
    STDMETHOD(SetEventNotification)(HANDLE) override { return DI_OK; }
    STDMETHOD(SetCooperativeLevel)(HWND, DWORD) override { return DI_OK; }
    STDMETHOD(GetObjectInfo)(LPDIDEVICEOBJECTINSTANCEA o, DWORD obj, DWORD how) override
    {
        if (!o) return DIERR_INVALIDPARAM;
        for (int i = 0; i < (int)std::size(kObjects); i++)
        {
            bool match = how == DIPH_BYOFFSET ? kObjects[i].offset == obj
                         : how == DIPH_BYID   ? (kObjects[i].id & 0x00FFFFFF) == (obj & 0x00FFFFFF)
                                              : false;
            if (!match) continue;
            DWORD size = o->dwSize;
            DIDEVICEOBJECTINSTANCEA d;
            Describe(i, d);
            memcpy(o, &d, std::min<DWORD>(size, sizeof(d)));
            o->dwSize = size;
            return DI_OK;
        }
        return DIERR_OBJECTNOTFOUND;
    }
    STDMETHOD(GetDeviceInfo)(LPDIDEVICEINSTANCEA i) override
    {
        if (!i || (i->dwSize != sizeof(DIDEVICEINSTANCEA) && i->dwSize != sizeof(DIDEVICEINSTANCE_DX3A))) return DIERR_INVALIDPARAM;
        DIDEVICEINSTANCEA d;
        Instance(d);
        DWORD size = i->dwSize;
        memcpy(i, &d, size);
        i->dwSize = size;
        return DI_OK;
    }
    STDMETHOD(RunControlPanel)(HWND, DWORD) override { return DI_OK; }
    STDMETHOD(Initialize)(HINSTANCE, DWORD, REFGUID) override { return DI_OK; }
    STDMETHOD(CreateEffect)(REFGUID g, LPCDIEFFECT e, LPDIRECTINPUTEFFECT* out, LPUNKNOWN) override
    {
        return Haptic() ? m_ff.CreateEffect(g, e, out) : DIERR_UNSUPPORTED;
    }
    STDMETHOD(EnumEffects)(LPDIENUMEFFECTSCALLBACKA cb, LPVOID ref, DWORD type) override
    {
        return Haptic() ? m_ff.EnumEffects(cb, ref, type) : DI_OK;
    }
    STDMETHOD(GetEffectInfo)(LPDIEFFECTINFOA info, REFGUID g) override { return Haptic() ? m_ff.GetEffectInfo(info, g) : DIERR_UNSUPPORTED; }
    STDMETHOD(GetForceFeedbackState)(LPDWORD out) override { return Haptic() ? m_ff.GetState(out) : DIERR_UNSUPPORTED; }
    STDMETHOD(SendForceFeedbackCommand)(DWORD flags) override { return Haptic() ? m_ff.Command(flags) : DIERR_UNSUPPORTED; }
    STDMETHOD(EnumCreatedEffectObjects)(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK cb, LPVOID ref, DWORD) override
    {
        return m_ff.EnumCreated(cb, ref);
    }
    STDMETHOD(Escape)(LPDIEFFESCAPE) override { return DIERR_UNSUPPORTED; }
    STDMETHOD(Poll)() override
    {
        Update();
        return DI_OK;
    }
    STDMETHOD(SendDeviceData)(DWORD, LPCDIDEVICEOBJECTDATA, LPDWORD inOut, DWORD) override
    {
        if (inOut) *inOut = 0;
        return DI_OK;
    }
    STDMETHOD(EnumEffectsInFile)(LPCSTR, LPDIENUMEFFECTSINFILECALLBACK, LPVOID, DWORD) override { return DIERR_UNSUPPORTED; }
    STDMETHOD(WriteEffectToFile)(LPCSTR, DWORD, LPDIFILEEFFECT, DWORD) override { return DIERR_UNSUPPORTED; }
    // The game's default controls for the pad (see kDefaults).
    STDMETHOD(BuildActionMap)(LPDIACTIONFORMATA f, LPCSTR, DWORD) override
    {
        if (!f || !f->rgoAction) return DIERR_INVALIDPARAM;
        for (DWORD i = 0; i < f->dwNumActions; i++)
        {
            DIACTIONA& a = f->rgoAction[i];
            int o = DefaultObject(a.dwSemantic);
            if (o < 0)
            {
                a.dwHow = DIAH_UNMAPPED;
                continue;
            }
            a.guidInstance = kVrPadInstance;
            a.dwObjID = kObjects[o].id;
            a.dwHow = DIAH_HWDEFAULT;
        }
        Log("controllers: \"%s\": the game's default controls made", kVrPadName);
        return DI_OK;
    }
    STDMETHOD(SetActionMap)(LPDIACTIONFORMATA, LPCSTR, DWORD) override { return DI_OK; }
    STDMETHOD(GetImageInfo)(LPDIDEVICEIMAGEINFOHEADERA) override { return DIERR_UNSUPPORTED; }

    static DWORD DevType() { return DI8DEVTYPE_GAMEPAD | (DI8DEVTYPEGAMEPAD_STANDARD << 8); }
    static void Instance(DIDEVICEINSTANCEA& d)
    {
        memset(&d, 0, sizeof(d));
        d.dwSize = sizeof(d);
        d.guidInstance = kVrPadInstance;
        d.guidProduct = kVrPadProduct;
        d.dwDevType = DevType();
        strcpy(d.tszInstanceName, kVrPadName);
        strcpy(d.tszProductName, kVrPadName);
        d.wUsagePage = 1;
        d.wUsage = 5;  // game pad
    }

private:
    // The force-feedback effects play on the VR controllers when the runtime can make them vibrate.
    static bool Haptic() { return g_cfg.vrRumble > 0; }

    void Describe(int i, DIDEVICEOBJECTINSTANCEA& o) const
    {
        const PadObject& p = kObjects[i];
        memset(&o, 0, sizeof(o));
        o.dwSize = sizeof(o);
        o.guidType = *p.type;
        o.dwOfs = p.offset;
        bool actuator = p.actuator && Haptic();
        o.dwType = p.id | (actuator ? DIDFT_FFACTUATOR : 0);
        o.dwFlags = (DIDFT_GETTYPE(p.id) & DIDFT_AXIS ? DIDOI_ASPECTPOSITION : 0) | (actuator ? DIDOI_FFACTUATOR : 0);
        strcpy(o.tszName, p.name);
        if (actuator) o.dwFFMaxForce = 10000, o.dwFFForceResolution = 1;
        o.wUsagePage = *p.type == GUID_Button ? 9 : 1;
        o.wUsage = p.usage;
    }

    // The axis a property header names (by offset or id): its index, or -1 for the whole device.
    int Axis(LPCDIPROPHEADER h) const
    {
        if (h->dwHow == DIPH_DEVICE) return -1;
        for (int a = 0; a < kAxes; a++)
            if ((h->dwHow == DIPH_BYOFFSET && kObjects[a].offset == h->dwObj) ||
                (h->dwHow == DIPH_BYID && (kObjects[a].id & 0x00FFFFFF) == (h->dwObj & 0x00FFFFFF)))
                return a;
        return -1;
    }

    // A value from -1 to 1 into the axis's range, past its dead zone and saturation (as DirectInput does them).
    LONG Scale(int a, float v, bool deadzone) const
    {
        if (deadzone)
        {
            float dz = m_deadzone[a] / 10000.0f, sat = std::max(m_saturation[a] / 10000.0f, dz + 0.01f), m = std::abs(v);
            v = m <= dz ? 0.0f : std::copysign(std::min((m - dz) / (sat - dz), 1.0f), v);
        }
        v = std::clamp(v, -1.0f, 1.0f);
        return (LONG)lround(m_min[a] + (v + 1.0) * 0.5 * ((double)m_max[a] - m_min[a]));
    }

    void Read(DIJOYSTATE2& s)
    {
        memset(&s, 0, sizeof(s));
        for (DWORD& pov : s.rgdwPOV) pov = 0xFFFFFFFF;
        xr::Controller l = xr::Hand(0), r = xr::Hand(1);
        // The thumbsticks rest a little off the middle: a small round dead zone of their own first.
        auto stick = [](const float in[2], float out[2]) {
            float m = std::hypot(in[0], in[1]), k = m > 0.08f ? std::min((m - 0.08f) / 0.92f, 1.0f) / m : 0.0f;
            out[0] = in[0] * k, out[1] = in[1] * k;
        };
        auto trigger = [](float t) { return 1.0f - 2.0f * std::clamp((t - 0.03f) / 0.94f, 0.0f, 1.0f); };  // rest at the top, pulled -1
        float ls[2], rs[2];
        stick(l.stick, ls);
        stick(r.stick, rs);
        s.lX = Scale(0, ls[0], true);
        s.lY = Scale(1, -ls[1], true);
        s.lZ = Scale(2, trigger(l.trigger), false);
        s.lRx = Scale(3, rs[0], true);
        s.lRy = Scale(4, -rs[1], true);
        s.lRz = Scale(5, trigger(r.trigger), false);
        auto grip = [](bool& held, float g) { return held = held ? g > 0.4f : g > 0.6f; };
        const bool buttons[kButtons] = { r.lower, r.button, l.lower, l.button, grip(m_grip[0], l.grip), grip(m_grip[1], r.grip), false, l.menu, l.thumb, r.thumb };
        for (int b = 0; b < kButtons; b++) s.rgbButtons[b] = buttons[b] ? 0x80 : 0;
    }

    void Update() { Vibrate(m_ff.Output(NowSeconds())); }
    void Vibrate(Motors m)
    {
        // One motor in each hand: as strong as the stronger of the two.
        float a = std::min(std::max(m.low, m.high), 1.0f);
        xr::Haptics(0, a);
        xr::Haptics(1, a);
        if (g_cfg.testControllersLog && xr::HapticsAmplitude(1) != m_logged)
        {
            m_logged = xr::HapticsAmplitude(1);
            Log("force feedback (VR controllers): vibration %.2f", m_logged);
        }
    }

    ULONG m_refs = 1;
    ForceFeedback m_ff{ "VR controllers" };
    LONG m_min[kAxes] = { 0, 0, 0, 0, 0, 0 }, m_max[kAxes] = { 65535, 65535, 65535, 65535, 65535, 65535 };
    DWORD m_deadzone[kAxes] = {}, m_saturation[kAxes] = { 10000, 10000, 10000, 10000, 10000, 10000 };
    DWORD m_autocenter = DIPROPAUTOCENTER_OFF;
    bool m_grip[2] = {};
    float m_logged = 0;
    bool m_read = false;
    DIJOYSTATE2 m_last = {};
};

bool VrPadWanted() { return g_cfg.vrMotionControllers && xr::Available(); }

bool IsKeyboardOrMouse(REFGUID g)
{
    return g == GUID_SysKeyboard || g == GUID_SysMouse || g == GUID_SysKeyboardEm || g == GUID_SysMouseEm || g == GUID_SysKeyboardEm2 ||
           g == GUID_SysMouseEm2;
}

// ---- the game's DirectInput ---------------------------------------------------------------------------------------
class InputProxy : public IDirectInput8A
{
public:
    explicit InputProxy(IDirectInput8A* inner) : m_inner(inner) {}

    // IUnknown
    STDMETHOD(QueryInterface)(REFIID iid, LPVOID* out) override
    {
        if (!out) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == IID_IDirectInput8A)
        {
            *out = this;
            AddRef();
            return S_OK;
        }
        return m_inner->QueryInterface(iid, out);
    }
    STDMETHOD_(ULONG, AddRef)() override { return ++m_refs; }
    STDMETHOD_(ULONG, Release)() override
    {
        ULONG n = --m_refs;
        if (n == 0)
        {
            m_inner->Release();
            delete this;
        }
        return n;
    }
    // IDirectInput8A
    STDMETHOD(CreateDevice)(REFGUID guid, LPDIRECTINPUTDEVICE8A* out, LPUNKNOWN outer) override
    {
        if (!out) return E_POINTER;
        if ((guid == kVrPadInstance || guid == kVrPadProduct) && VrPadWanted())
        {
            *out = new VrPad();
            return DI_OK;
        }
        HRESULT hr = m_inner->CreateDevice(guid, out, outer);
        if (SUCCEEDED(hr) && *out && !IsKeyboardOrMouse(guid)) *out = new PadProxy(*out);
        return hr;
    }
    STDMETHOD(EnumDevices)(DWORD type, LPDIENUMDEVICESCALLBACKA cb, LPVOID ref, DWORD flags) override
    {
        struct Context
        {
            LPDIENUMDEVICESCALLBACKA cb;
            LPVOID ref;
            bool stopped;
        } c = { cb, ref, false };
        HRESULT hr = m_inner->EnumDevices(
            type,
            [](LPCDIDEVICEINSTANCEA d, LPVOID p) -> BOOL {
                auto& c = *(Context*)p;
                BOOL r = c.cb(d, c.ref);
                if (r == DIENUM_STOP) c.stopped = true;
                return r;
            },
            &c, flags);
        // The VR controllers come after the controllers there are (whose places in the game's list stay as they were).
        bool listed = type == DI8DEVCLASS_ALL || type == DI8DEVCLASS_GAMECTRL || GET_DIDEVICE_TYPE(type) == DI8DEVTYPE_GAMEPAD;
        if (SUCCEEDED(hr) && cb && !c.stopped && listed && VrPadWanted() && !((flags & DIEDFL_FORCEFEEDBACK) && g_cfg.vrRumble <= 0))
        {
            DIDEVICEINSTANCEA d;
            VrPad::Instance(d);
            if (!m_listed) Log("controllers: \"%s\" added to the game's game controllers", kVrPadName);
            m_listed = true;
            cb(&d, ref);
        }
        return hr;
    }
    STDMETHOD(GetDeviceStatus)(REFGUID guid) override
    {
        if (guid == kVrPadInstance || guid == kVrPadProduct) return VrPadWanted() ? DI_OK : DIERR_DEVICENOTREG;
        return m_inner->GetDeviceStatus(guid);
    }
    STDMETHOD(RunControlPanel)(HWND w, DWORD f) override { return m_inner->RunControlPanel(w, f); }
    STDMETHOD(Initialize)(HINSTANCE i, DWORD v) override { return m_inner->Initialize(i, v); }
    STDMETHOD(FindDevice)(REFGUID g, LPCSTR name, LPGUID out) override { return m_inner->FindDevice(g, name, out); }
    STDMETHOD(EnumDevicesBySemantics)(LPCSTR user, LPDIACTIONFORMATA f, LPDIENUMDEVICESBYSEMANTICSCBA cb, LPVOID ref, DWORD flags) override
    {
        return m_inner->EnumDevicesBySemantics(user, f, cb, ref, flags);
    }
    STDMETHOD(ConfigureDevices)(LPDICONFIGUREDEVICESCALLBACK cb, LPDICONFIGUREDEVICESPARAMSA p, DWORD flags, LPVOID ref) override
    {
        return m_inner->ConfigureDevices(cb, p, flags, ref);
    }

private:
    ULONG m_refs = 1;
    IDirectInput8A* m_inner;
    bool m_listed = false;
};

// Just before the game lists its controllers (its DirectInput made): the mod's DirectInput takes the game's place.
void __cdecl WrapDirectInput()
{
    auto slot = (IDirectInput8A**)fo2::kDirectInput;
    if (!*slot)
    {
        Log("controllers: the game has no DirectInput - its controllers stay as they are");
        return;
    }
    *slot = new InputProxy(*slot);
    Log("controllers: the game's DirectInput goes through the mod (VR controllers %s, vibration %.2f)",
        VrPadWanted() ? "on" : g_cfg.vrMotionControllers ? "off: no VR" : "off", g_cfg.vrRumble);
}

uintptr_t g_controllerEnum = fo2::kControllerEnum;
void(__cdecl* g_wrapDirectInput)() = WrapDirectInput;

// The call of the game's controller list (the input in esi, kept).
__declspec(naked) void ControllerEnumHook()
{
    __asm {
        pushad
        call dword ptr [g_wrapDirectInput]
        popad
        jmp dword ptr [g_controllerEnum]
    }
}

}  // namespace

bool InstallControllers()
{
    if (!PatchCall(fo2::kControllerEnumCall, fo2::kControllerEnum, (const void*)&ControllerEnumHook))
    {
        Log("controllers: the game's controller list is called differently - no VR controllers, no vibration for Xbox controllers");
        return false;
    }
    return true;
}

void FocusGameWindow(HWND window)
{
    if (!window || !InVr() || BackgroundMode()) return;
    HWND front = GetForegroundWindow();
    if (front == window) return;
    // Windows lets a program take the focus only from the program in front (or one that has just been used): the
    // thread of the window in front is asked to share its input for the moment.
    DWORD frontThread = front ? GetWindowThreadProcessId(front, nullptr) : 0, ours = GetCurrentThreadId();
    bool attached = frontThread && frontThread != ours && AttachThreadInput(ours, frontThread, TRUE);
    BOOL ok = SetForegroundWindow(window);
    BringWindowToTop(window);
    if (attached) AttachThreadInput(ours, frontThread, FALSE);
    char title[128] = "";
    if (front) GetWindowTextA(front, title, sizeof(title));
    Log("controllers: the game's window was not in front (\"%s\" was): put in front %s", title, ok ? "" : "- FAILED");
}

bool ControllersRecenter()
{
    static bool wasDown;
    bool down = xr::Hand(0).thumb && xr::Hand(1).thumb;
    bool pressed = down && !wasDown;
    wasDown = down;
    return pressed;
}

// ---- the menus' keys ----------------------------------------------------------------------------------------------
// The game's menus take only the keyboard: its game controllers drive, and do nothing in the menus (on the PC; the
// consoles' menus are the same scripts, fed by their pads), nor pause a race. So the VR controllers and Xbox controllers
// (XInput, whether or not one is the game's chosen controller) press its keys, as its keyboard hook would: in the menus
// the thumbsticks and the D-pad the arrows (repeating while held), A Enter, B Esc, the VR controllers' menu button Esc,
// Start Enter ("press START") and Back Esc; while driving the menu button and Start Esc (the pause). What a button
// presses is chosen as it goes down and held until it comes up. Not while a control waits for its new key or button
// (Configure controls), nor in a replay.
namespace {

enum Press
{
    kNone,
    kArrowUp,
    kArrowDown,
    kArrowLeft,
    kArrowRight,
    kEnter,  // in the menus
    kEsc,    // in the menus
    kStart,  // Start: Enter in the menus, Esc while driving
    kMenu,   // the VR controllers' menu button: Esc
};

struct Source
{
    bool down = false;
    int vk = 0;       // the key it holds
    double next = 0;  // when an arrow repeats
};

int g_keyHolders[256];

void GameKey(int vk, bool down)
{
    BYTE& state = ((BYTE*)fo2::kKeyState)[vk];
    int& held = *(int*)fo2::kKeysHeld;
    if (down && !(state & 0x80))
    {
        held++;
        state = (BYTE)((state ^ 1) | 0x80);
        ((BYTE*)fo2::kKeyPresses)[vk]++;
    }
    else if (!down && (state & 0x80))
    {
        held--;
        state &= 0x7F;
    }
}

// The player drives: a race running (its countdown too; not its pause, its "start race" or its end).
bool Driving()
{
    static int lastSteps, framesWithoutStep;
    int steps = PhysicsSteps();
    framesWithoutStep = steps != lastSteps ? 0 : framesWithoutStep + 1;
    lastSteps = steps;
    BYTE* gf = *(BYTE**)fo2::kGameFlow;
    if (!gf || *(int*)(gf + fo2::kGameState) != 2) return false;
    int race = *(int*)(gf + fo2::kRaceState);
    return race == 7 || (race == 8 && framesWithoutStep < 2);
}

bool Remapping()
{
    BYTE* input = *(BYTE**)fo2::kInput;
    if (!input) return false;
    int n = *(int*)(input + fo2::kInputControllers);
    for (int i = 0; i < n && i < 4; i++)
    {
        BYTE* c = ((BYTE**)(input + fo2::kInputControllerList))[i];
        if (c && *(int*)(c + fo2::kControllerRemap) != -1) return true;
    }
    return false;
}

void Feed(Source& src, bool down, Press press, bool driving, bool off, double now)
{
    if (down && !src.down)
    {
        int vk = 0;
        if (!off)
            switch (press)
            {
            case kArrowUp: vk = driving ? 0 : VK_UP; break;
            case kArrowDown: vk = driving ? 0 : VK_DOWN; break;
            case kArrowLeft: vk = driving ? 0 : VK_LEFT; break;
            case kArrowRight: vk = driving ? 0 : VK_RIGHT; break;
            case kEnter: vk = driving ? 0 : VK_RETURN; break;
            case kEsc: vk = driving ? 0 : VK_ESCAPE; break;
            case kStart: vk = driving ? VK_ESCAPE : VK_RETURN; break;
            case kMenu: vk = VK_ESCAPE; break;
            default: break;
            }
        src.vk = vk;
        src.next = now + 0.4;
        if (vk && g_keyHolders[vk]++ == 0) GameKey(vk, true);
        if (g_cfg.testControllersLog) Log("controllers: menu key %02X (%s)", vk, driving ? "driving" : off ? "off" : "menus");
    }
    else if (!down && src.down)
    {
        if (src.vk && --g_keyHolders[src.vk] == 0) GameKey(src.vk, false);
        src.vk = 0;
    }
    else if (down && src.vk && press >= kArrowUp && press <= kArrowRight && now >= src.next)
    {
        GameKey(src.vk, false);
        GameKey(src.vk, true);
        src.next = now + 0.12;
    }
    src.down = down;
}

// A thumbstick's four ways (the way it leans most, past most of the way).
void StickWays(const float s[2], bool ways[4])
{
    float x = s[0], y = s[1];
    if (std::max(std::abs(x), std::abs(y)) < 0.6f) return;
    if (std::abs(x) > std::abs(y))
        ways[x > 0 ? 3 : 2] = true;
    else
        ways[y > 0 ? 0 : 1] = true;
}

Source g_vrWays[4], g_vrA, g_vrB, g_vrMenu;
Source g_padWays[4][4], g_padA[4], g_padB[4], g_padStart[4], g_padBack[4];

}  // namespace

void ControllersMenuKeys()
{
    if (!g_cfg.vrEnabled) return;
    double now = NowSeconds();
    bool driving = Driving(), off = Remapping() || rec::Replaying();
    static int was = -1;
    if (g_cfg.testControllersLog && (int)driving != was)
    {
        was = driving;
        BYTE* gf = *(BYTE**)fo2::kGameFlow;
        Log("controllers: %s (game state %d, race state %d)", driving ? "driving" : "menus", gf ? *(int*)(gf + fo2::kGameState) : -1,
            gf ? *(int*)(gf + fo2::kRaceState) : -1);
    }
    const Press ways[4] = { kArrowUp, kArrowDown, kArrowLeft, kArrowRight };
    if (VrPadWanted())
    {
        xr::Controller l = xr::Hand(0), r = xr::Hand(1);
        bool w[4] = {};
        StickWays(l.stick, w);
        StickWays(r.stick, w);
        for (int i = 0; i < 4; i++) Feed(g_vrWays[i], w[i], ways[i], driving, off, now);
        Feed(g_vrA, r.lower, kEnter, driving, off, now);
        Feed(g_vrB, r.button, kEsc, driving, off, now);
        Feed(g_vrMenu, l.menu, kMenu, driving, off, now);
    }
    if (LoadXInput())
    {
        ScanXInput(!driving);
        for (DWORD u = 0; u < 4; u++)
        {
            XINPUT_STATE x = {};
            bool connected = g_xinputConnected[u] && g_xinputGetState(u, &x) == ERROR_SUCCESS;
            g_xinputConnected[u] = connected;
            const XINPUT_GAMEPAD& g = x.Gamepad;
            const float stick[2] = { g.sThumbLX / 32767.0f, g.sThumbLY / 32767.0f };
            bool w[4] = {};
            if (connected) StickWays(stick, w);
            const WORD dpad[4] = { XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_RIGHT };
            for (int i = 0; i < 4; i++) Feed(g_padWays[u][i], w[i] || (g.wButtons & dpad[i]), ways[i], driving, off, now);
            Feed(g_padA[u], (g.wButtons & XINPUT_GAMEPAD_A) != 0, kEnter, driving, off, now);
            Feed(g_padB[u], (g.wButtons & XINPUT_GAMEPAD_B) != 0, kEsc, driving, off, now);
            Feed(g_padStart[u], (g.wButtons & XINPUT_GAMEPAD_START) != 0, kStart, driving, off, now);
            Feed(g_padBack[u], (g.wButtons & XINPUT_GAMEPAD_BACK) != 0, kEsc, driving, off, now);
        }
    }
}
