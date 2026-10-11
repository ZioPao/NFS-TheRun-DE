// Wheel force-feedback tuning.
//
// Ported from the standalone NFSTheRun-FFBFix dinput8.dll proxy. That proxy
// worked, but it occupies the dinput8.dll slot, which is also where the ASI
// loader usually lives, so it could never be installed next to this .asi. This
// is the same interception done in-process instead.
//
// WHAT THE GAME GETS WRONG. The Run exposes no force-feedback settings at all,
// so the centering spring runs at full strength on every wheel and there is no
// menu option to turn it down. The only lever is the magnitudes of the effects
// the game hands to DirectInput, which is what this scales.
//
// WHY AN IAT HOOK AND NOT A PROXY DLL. A proxy intercepts DirectInput8Create at
// the module boundary by replacing dinput8.dll — and that name is taken by the
// ASI loader. An ASI is already inside the process, so it can instead point the
// game's import-table entry at its own function and leave the real dinput8.dll
// (and any loader sitting on that name) untouched. Nothing is disassembled and
// no trampoline is needed: the original pointer is saved and called directly.
//
// The wrappers below are refcounted pass-throughs. Only the force-feedback
// effect objects are modified, and only their magnitudes; device state, polling
// and button/axis data are forwarded byte for byte.

#define WIN32_LEAN_AND_MEAN
#define DIRECTINPUT_VERSION 0x0800
// Instantiate the DirectInput/COM GUIDs in this translation unit so no extra
// import library (dxguid) is needed. This file is the only one that touches
// DirectInput, so there is no duplicate-definition risk elsewhere.
#define INITGUID
#include <windows.h>
#include <dinput.h>
#include <cstring>

#include "features.h"
#include "../config.h"
#include "../memory.h"
#include "../logger.h"

namespace {

    // ---- GUID helpers ------------------------------------------------------
    bool IsConditionGuid(REFGUID g) {
        return IsEqualGUID(g, GUID_Spring) ||
               IsEqualGUID(g, GUID_Damper) ||
               IsEqualGUID(g, GUID_Inertia) ||
               IsEqualGUID(g, GUID_Friction);
    }

    // Which configured scalar applies to this effect type. Conditions (the
    // centering spring above all) get their own, constant force gets its own,
    // and everything periodic shares the last.
    float TypeScale(REFGUID g) {
        if (IsConditionGuid(g))                 return g_Config.FfbSpringScale;
        if (IsEqualGUID(g, GUID_ConstantForce)) return g_Config.FfbConstantScale;
        return g_Config.FfbPeriodicScale;
    }

    // Scale a DIEFFECT in place before it is forwarded to the real device.
    void ScaleEffectParams(REFGUID guid, DIEFFECT& eff) {
        // Overall gain. The original proxy gated this on the literal 0x00000004,
        // which is not one of the DIEFF_* flags the struct actually defines, so
        // that branch never ran; here it is applied directly. At the default
        // scale of 1.0 the value is left byte-for-byte alone.
        if (g_Config.FfbGainScale != 1.0f) {
            double g = static_cast<double>(eff.dwGain) * g_Config.FfbGainScale;
            if (g < 0.0) g = 0.0;
            if (g > static_cast<double>(DI_FFNOMINALMAX)) g = static_cast<double>(DI_FFNOMINALMAX);
            eff.dwGain = static_cast<DWORD>(g);
        }

        const float typeScale = TypeScale(guid);
        if (!eff.lpvTypeSpecificParams || eff.cbTypeSpecificParams == 0) return;

        if (IsEqualGUID(guid, GUID_ConstantForce) &&
            eff.cbTypeSpecificParams >= sizeof(DICONSTANTFORCE)) {
            auto* cf = reinterpret_cast<DICONSTANTFORCE*>(eff.lpvTypeSpecificParams);
            cf->lMagnitude = static_cast<LONG>(cf->lMagnitude * typeScale);
            return;
        }

        if (IsConditionGuid(guid) && eff.cbTypeSpecificParams >= sizeof(DICONDITION)) {
            // Condition effects carry one DICONDITION per axis.
            DWORD count = eff.cbTypeSpecificParams / sizeof(DICONDITION);
            auto* cond = reinterpret_cast<DICONDITION*>(eff.lpvTypeSpecificParams);
            for (DWORD i = 0; i < count; ++i) {
                cond[i].lPositiveCoefficient = static_cast<LONG>(cond[i].lPositiveCoefficient * typeScale);
                cond[i].lNegativeCoefficient = static_cast<LONG>(cond[i].lNegativeCoefficient * typeScale);
                cond[i].dwPositiveSaturation = static_cast<DWORD>(cond[i].dwPositiveSaturation * typeScale);
                cond[i].dwNegativeSaturation = static_cast<DWORD>(cond[i].dwNegativeSaturation * typeScale);
            }
            return;
        }

        // Periodic (sine, square, triangle, sawtooth). The size check keeps the
        // smaller RampForce struct from being misread as one of these.
        if (eff.cbTypeSpecificParams >= sizeof(DIPERIODIC)) {
            auto* per = reinterpret_cast<DIPERIODIC*>(eff.lpvTypeSpecificParams);
            per->dwMagnitude = static_cast<DWORD>(per->dwMagnitude * typeScale);
        }
    }

    // ---- ProxyDirectInputEffect -------------------------------------------
    // Wraps IDirectInputEffect. CreateEffect scales once at creation, but the
    // game re-sends parameters during play (the centering spring above all), so
    // SetParameters has to scale too or the effect drifts back to stock.
    struct ProxyDirectInputEffect : public IDirectInputEffect {
        explicit ProxyDirectInputEffect(IDirectInputEffect* real, REFGUID guid)
            : m_real(real), m_effGuid(guid), m_refCount(1) {}

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
            if (IsEqualGUID(riid, IID_IUnknown) || IsEqualGUID(riid, IID_IDirectInputEffect)) {
                *ppv = this; AddRef(); return S_OK;
            }
            return m_real->QueryInterface(riid, ppv);
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refCount; }
        ULONG STDMETHODCALLTYPE Release() override {
            if (--m_refCount == 0) { m_real->Release(); delete this; return 0; }
            return m_refCount;
        }

        HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE h, DWORD v, REFGUID g) override
            { return m_real->Initialize(h, v, g); }
        HRESULT STDMETHODCALLTYPE GetEffectGuid(LPGUID g) override
            { return m_real->GetEffectGuid(g); }
        HRESULT STDMETHODCALLTYPE GetParameters(LPDIEFFECT p, DWORD f) override
            { return m_real->GetParameters(p, f); }
        HRESULT STDMETHODCALLTYPE SetParameters(LPCDIEFFECT peff, DWORD flags) override {
            if (!peff) return m_real->SetParameters(peff, flags);
            // Copy before mutating: the caller's struct must not be touched.
            DIEFFECT copy = *peff;
            ScaleEffectParams(m_effGuid, copy);
            return m_real->SetParameters(&copy, flags);
        }
        HRESULT STDMETHODCALLTYPE Start(DWORD iter, DWORD flags) override
            { return m_real->Start(iter, flags); }
        HRESULT STDMETHODCALLTYPE Stop() override
            { return m_real->Stop(); }
        HRESULT STDMETHODCALLTYPE GetEffectStatus(LPDWORD p) override
            { return m_real->GetEffectStatus(p); }
        HRESULT STDMETHODCALLTYPE Download() override
            { return m_real->Download(); }
        HRESULT STDMETHODCALLTYPE Unload() override
            { return m_real->Unload(); }
        HRESULT STDMETHODCALLTYPE Escape(LPDIEFFESCAPE p) override
            { return m_real->Escape(p); }

        IDirectInputEffect* m_real;
        GUID                m_effGuid;
        LONG                m_refCount;
    };

    // ---- ProxyDirectInputDevice8A -----------------------------------------
    // Wraps IDirectInputDevice8A; everything is a pass-through except
    // CreateEffect, which hands the game back a wrapped effect.
    struct ProxyDirectInputDevice8A : public IDirectInputDevice8A {
        explicit ProxyDirectInputDevice8A(IDirectInputDevice8A* real)
            : m_real(real), m_refCount(1) {}

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
            if (IsEqualGUID(riid, IID_IUnknown) || IsEqualGUID(riid, IID_IDirectInputDevice8A)) {
                *ppv = this; AddRef(); return S_OK;
            }
            return m_real->QueryInterface(riid, ppv);
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refCount; }
        ULONG STDMETHODCALLTYPE Release() override {
            if (--m_refCount == 0) { m_real->Release(); delete this; return 0; }
            return m_refCount;
        }

        HRESULT STDMETHODCALLTYPE GetCapabilities(LPDIDEVCAPS p) override { return m_real->GetCapabilities(p); }
        HRESULT STDMETHODCALLTYPE EnumObjects(LPDIENUMDEVICEOBJECTSCALLBACKA cb, LPVOID ref, DWORD f) override { return m_real->EnumObjects(cb, ref, f); }
        HRESULT STDMETHODCALLTYPE GetProperty(REFGUID g, LPDIPROPHEADER p) override { return m_real->GetProperty(g, p); }
        HRESULT STDMETHODCALLTYPE SetProperty(REFGUID g, LPCDIPROPHEADER p) override { return m_real->SetProperty(g, p); }
        HRESULT STDMETHODCALLTYPE Acquire() override { return m_real->Acquire(); }
        HRESULT STDMETHODCALLTYPE Unacquire() override { return m_real->Unacquire(); }
        HRESULT STDMETHODCALLTYPE GetDeviceState(DWORD s, LPVOID d) override { return m_real->GetDeviceState(s, d); }
        HRESULT STDMETHODCALLTYPE GetDeviceData(DWORD s, LPDIDEVICEOBJECTDATA d, LPDWORD n, DWORD f) override { return m_real->GetDeviceData(s, d, n, f); }
        HRESULT STDMETHODCALLTYPE SetDataFormat(LPCDIDATAFORMAT p) override { return m_real->SetDataFormat(p); }
        HRESULT STDMETHODCALLTYPE SetEventNotification(HANDLE h) override { return m_real->SetEventNotification(h); }
        HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND h, DWORD f) override { return m_real->SetCooperativeLevel(h, f); }
        HRESULT STDMETHODCALLTYPE GetObjectInfo(LPDIDEVICEOBJECTINSTANCEA p, DWORD o, DWORD h) override { return m_real->GetObjectInfo(p, o, h); }
        HRESULT STDMETHODCALLTYPE GetDeviceInfo(LPDIDEVICEINSTANCEA p) override { return m_real->GetDeviceInfo(p); }
        HRESULT STDMETHODCALLTYPE RunControlPanel(HWND h, DWORD f) override { return m_real->RunControlPanel(h, f); }
        HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE h, DWORD v, REFGUID g) override { return m_real->Initialize(h, v, g); }

        HRESULT STDMETHODCALLTYPE CreateEffect(REFGUID rguid, LPCDIEFFECT peff,
                                               LPDIRECTINPUTEFFECT* ppeff, LPUNKNOWN pOuter) override {
            // Scale before the effect exists, then wrap what comes back so the
            // game's later SetParameters calls are scaled as well.
            DIEFFECT scaled = {};
            if (peff) {
                scaled = *peff;
                ScaleEffectParams(rguid, scaled);
                peff = &scaled;
            }

            IDirectInputEffect* realEffect = nullptr;
            HRESULT hr = m_real->CreateEffect(rguid, peff, &realEffect, pOuter);
            if (SUCCEEDED(hr) && realEffect) {
                static bool logged = false;
                if (!logged) {
                    Logger::Log("ForceFeedback: first effect intercepted (guid %08X-%04X-%04X...).",
                        rguid.Data1, rguid.Data2, rguid.Data3);
                    logged = true;
                }
                *ppeff = new ProxyDirectInputEffect(realEffect, rguid);
            }
            return hr;
        }

        HRESULT STDMETHODCALLTYPE EnumEffects(LPDIENUMEFFECTSCALLBACKA cb, LPVOID ref, DWORD t) override { return m_real->EnumEffects(cb, ref, t); }
        HRESULT STDMETHODCALLTYPE GetEffectInfo(LPDIEFFECTINFOA p, REFGUID g) override { return m_real->GetEffectInfo(p, g); }
        HRESULT STDMETHODCALLTYPE GetForceFeedbackState(LPDWORD p) override { return m_real->GetForceFeedbackState(p); }
        HRESULT STDMETHODCALLTYPE SendForceFeedbackCommand(DWORD f) override { return m_real->SendForceFeedbackCommand(f); }
        HRESULT STDMETHODCALLTYPE EnumCreatedEffectObjects(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK cb, LPVOID ref, DWORD f) override { return m_real->EnumCreatedEffectObjects(cb, ref, f); }
        HRESULT STDMETHODCALLTYPE Escape(LPDIEFFESCAPE p) override { return m_real->Escape(p); }
        HRESULT STDMETHODCALLTYPE Poll() override { return m_real->Poll(); }
        HRESULT STDMETHODCALLTYPE SendDeviceData(DWORD s, LPCDIDEVICEOBJECTDATA d, LPDWORD n, DWORD f) override { return m_real->SendDeviceData(s, d, n, f); }
        HRESULT STDMETHODCALLTYPE EnumEffectsInFile(LPCSTR fn, LPDIENUMEFFECTSINFILECALLBACK cb, LPVOID ref, DWORD f) override { return m_real->EnumEffectsInFile(fn, cb, ref, f); }
        HRESULT STDMETHODCALLTYPE WriteEffectToFile(LPCSTR fn, DWORD n, LPDIFILEEFFECT e, DWORD f) override { return m_real->WriteEffectToFile(fn, n, e, f); }
        HRESULT STDMETHODCALLTYPE BuildActionMap(LPDIACTIONFORMATA p, LPCSTR u, DWORD f) override { return m_real->BuildActionMap(p, u, f); }
        HRESULT STDMETHODCALLTYPE SetActionMap(LPDIACTIONFORMATA p, LPCSTR u, DWORD f) override { return m_real->SetActionMap(p, u, f); }
        HRESULT STDMETHODCALLTYPE GetImageInfo(LPDIDEVICEIMAGEINFOHEADERA p) override { return m_real->GetImageInfo(p); }

        IDirectInputDevice8A* m_real;
        LONG                  m_refCount;
    };

    // ---- ProxyDirectInput8A ------------------------------------------------
    // Wraps IDirectInput8A; only CreateDevice is intercepted, so the wheel gets
    // a wrapped device object.
    struct ProxyDirectInput8A : public IDirectInput8A {
        explicit ProxyDirectInput8A(IDirectInput8A* real)
            : m_real(real), m_refCount(1) {}

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
            if (IsEqualGUID(riid, IID_IUnknown) || IsEqualGUID(riid, IID_IDirectInput8A)) {
                *ppv = this; AddRef(); return S_OK;
            }
            return m_real->QueryInterface(riid, ppv);
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refCount; }
        ULONG STDMETHODCALLTYPE Release() override {
            if (--m_refCount == 0) { m_real->Release(); delete this; return 0; }
            return m_refCount;
        }

        HRESULT STDMETHODCALLTYPE CreateDevice(REFGUID rguid, LPDIRECTINPUTDEVICE8A* ppdev, LPUNKNOWN pOuter) override {
            IDirectInputDevice8A* realDev = nullptr;
            HRESULT hr = m_real->CreateDevice(rguid, &realDev, pOuter);
            if (SUCCEEDED(hr) && realDev)
                *ppdev = new ProxyDirectInputDevice8A(realDev);
            return hr;
        }

        HRESULT STDMETHODCALLTYPE EnumDevices(DWORD t, LPDIENUMDEVICESCALLBACKA cb, LPVOID ref, DWORD f) override { return m_real->EnumDevices(t, cb, ref, f); }
        HRESULT STDMETHODCALLTYPE GetDeviceStatus(REFGUID g) override { return m_real->GetDeviceStatus(g); }
        HRESULT STDMETHODCALLTYPE RunControlPanel(HWND h, DWORD f) override { return m_real->RunControlPanel(h, f); }
        HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE h, DWORD v) override { return m_real->Initialize(h, v); }
        HRESULT STDMETHODCALLTYPE FindDevice(REFGUID g, LPCSTR n, LPGUID pg) override { return m_real->FindDevice(g, n, pg); }
        HRESULT STDMETHODCALLTYPE EnumDevicesBySemantics(LPCSTR u, LPDIACTIONFORMATA p, LPDIENUMDEVICESBYSEMANTICSCBA cb, LPVOID ref, DWORD f) override { return m_real->EnumDevicesBySemantics(u, p, cb, ref, f); }
        HRESULT STDMETHODCALLTYPE ConfigureDevices(LPDICONFIGUREDEVICESCALLBACK cb, LPDICONFIGUREDEVICESPARAMSA p, DWORD f, LPVOID ref) override { return m_real->ConfigureDevices(cb, p, f, ref); }

        IDirectInput8A* m_real;
        LONG            m_refCount;
    };

    // ---- The hooked export -------------------------------------------------
    typedef HRESULT (WINAPI* PFN_DirectInput8Create)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    PFN_DirectInput8Create g_realDI8Create = nullptr;
    bool                   g_Hooked       = false;

    HRESULT WINAPI HookedDirectInput8Create(HINSTANCE hinst, DWORD version, REFIID riid,
                                            LPVOID* ppvOut, LPUNKNOWN punkOuter) {
        if (!g_realDI8Create) return E_FAIL;

        // Only the ANSI interface is wrapped; the game uses it and the wide
        // variant is passed straight through.
        if (IsEqualGUID(riid, IID_IDirectInput8A)) {
            IDirectInput8A* real = nullptr;
            HRESULT hr = g_realDI8Create(hinst, version, riid,
                                         reinterpret_cast<LPVOID*>(&real), punkOuter);
            if (SUCCEEDED(hr) && real)
                *ppvOut = new ProxyDirectInput8A(real);
            return hr;
        }
        return g_realDI8Create(hinst, version, riid, ppvOut, punkOuter);
    }

    // ---- Import-table hook -------------------------------------------------
    // Finds the game's DirectInput8Create thunk and repoints it at our wrapper,
    // keeping the original so the wrapper can call through. Reads only the main
    // module's headers, so it cannot touch another image's imports.
    bool InstallIatHook() {
        uintptr_t base = Memory::GetGameBase();
        if (!base) { Logger::Log("ForceFeedback: no game base."); return false; }

        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

        DWORD importsRva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
        if (!importsRva) { Logger::Log("ForceFeedback: no import directory."); return false; }

        auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + importsRva);
        for (; desc->Name != 0; ++desc) {
            const char* dllName = reinterpret_cast<const char*>(base + desc->Name);
            if (lstrcmpiA(dllName, "dinput8.dll") != 0) continue;

            // Names live in the original thunk array; without it the import is
            // by ordinal only and there is nothing to match on.
            if (!desc->OriginalFirstThunk) continue;

            auto* nameThunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk);
            auto* iatThunk  = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
            for (; nameThunk->u1.AddressOfData != 0; ++nameThunk, ++iatThunk) {
                if (IMAGE_SNAP_BY_ORDINAL(nameThunk->u1.Ordinal)) continue;
                auto* ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + nameThunk->u1.AddressOfData);
                if (std::strcmp(reinterpret_cast<const char*>(ibn->Name), "DirectInput8Create") != 0)
                    continue;

                g_realDI8Create = reinterpret_cast<PFN_DirectInput8Create>(iatThunk->u1.Function);

                DWORD oldProtect = 0;
                if (!VirtualProtect(&iatThunk->u1.Function, sizeof(ULONG_PTR), PAGE_READWRITE, &oldProtect)) {
                    Logger::Log("ForceFeedback: VirtualProtect on the import entry failed.");
                    g_realDI8Create = nullptr;
                    return false;
                }
                iatThunk->u1.Function = reinterpret_cast<ULONG_PTR>(&HookedDirectInput8Create);
                VirtualProtect(&iatThunk->u1.Function, sizeof(ULONG_PTR), oldProtect, &oldProtect);

                g_Hooked = true;
                return true;
            }
        }

        Logger::Log("ForceFeedback: DirectInput8Create not found in the game's imports.");
        return false;
    }

} // namespace

namespace Features {
    void InitForceFeedback() {
        if (!g_Config.EnableForceFeedbackTuning) {
            Logger::Log("ForceFeedback: disabled in the INI, wheels left at stock.");
            return;
        }

        if (!InstallIatHook()) {
            Logger::Log("ForceFeedback: could not intercept DirectInput8Create; wheel FFB left at stock.");
            return;
        }

        Logger::Log("ForceFeedback: hooked (Gain=%.2f  Spring=%.2f  Constant=%.2f  Periodic=%.2f).",
            g_Config.FfbGainScale, g_Config.FfbSpringScale,
            g_Config.FfbConstantScale, g_Config.FfbPeriodicScale);
    }
}
