// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 dox187
//
// hypr-window-rotate: rotate individual Hyprland windows in 90 degree steps.
//
// The window keeps its layout box (tile), so borders, shadows, hit-testing and the layout stay untouched.
// Only the client side is rotated, the same way a buffer transform works:
//  - Size: for 90/270 the client is configured with the swapped size (CWindow::realToReportSize hook).
//  - Rendering: the window's surfaces (main surface + subsurfaces) get their boxes rotated around the window
//    center (CSurfacePassElement::getTexBox hook), and their texture is drawn with an extra transform
//    (IElementRenderer::drawSurface hook). UV fitting is computed in the unrotated frame
//    (IElementRenderer::calculateUVForSurface hook).
//  - Input: pointer coordinates sent to the client are rotated back (CSeatManager hooks) and surface picking
//    uses the unrotated point (CViewHitTester::windowSurfaceAt hook).
//  - Damage: partial surface damage would land on the wrong spot, so any damage repaints the whole window.
//
// Popups (menus, tooltips) are not rotated; they keep their normal position and input.

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/ViewHitTester.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/ElementRenderer.hpp>
#include <hyprland/src/render/Texture.hpp>
#include <hyprland/src/render/pass/SurfacePassElement.hpp>
#include <hyprland/src/helpers/math/Math.hpp>
#include <hyprland/src/config/ConfigManager.hpp>

extern "C" {
#include <lua.h>
}

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

#ifndef PLUGIN_VERSION
#define PLUGIN_VERSION "dev"
#endif

using namespace Desktop::View;

static HANDLE PHANDLE = nullptr;

struct SRotation {
    PHLWINDOWREF window;
    int          quarters = 0; // clockwise quarter turns, 1..3
};

static std::vector<SRotation> g_rotations;

// quarters of the surface currently drawn by hkDrawSurface, read by hkCalculateUV
static int            g_drawingQuarters = 0;

static CFunctionHook* g_reportSizeHook        = nullptr;
static CFunctionHook* g_getTexBoxHook         = nullptr;
static CFunctionHook* g_drawSurfaceHook       = nullptr;
static CFunctionHook* g_calculateUVHook       = nullptr;
static CFunctionHook* g_damageSurfaceHook     = nullptr;
static CFunctionHook* g_windowSurfaceAtHook   = nullptr;
static CFunctionHook* g_setPointerFocusHook   = nullptr;
static CFunctionHook* g_sendPointerMotionHook = nullptr;

// ---------------------------------------------------------------------------------------------------------------------
// math

static int normalizeQuarters(int q) {
    return ((q % 4) + 4) % 4;
}

// Clockwise on screen (y points down).
static Vector2D rotateVec(const Vector2D& v, int q) {
    switch (normalizeQuarters(q)) {
        case 1: return {-v.y, v.x};
        case 2: return {-v.x, -v.y};
        case 3: return {v.y, -v.x};
        default: return v;
    }
}

static CBox rotateBoxAround(const CBox& box, const Vector2D& center, int q) {
    const Vector2D corners[4] = {box.pos(), box.pos() + Vector2D{box.w, 0.0}, box.pos() + Vector2D{0.0, box.h}, box.pos() + box.size()};

    Vector2D       min = {INFINITY, INFINITY};
    Vector2D       max = {-INFINITY, -INFINITY};
    for (const auto& c : corners) {
        const Vector2D r = center + rotateVec(c - center, q);
        min              = {std::min(min.x, r.x), std::min(min.y, r.y)};
        max              = {std::max(max.x, r.x), std::max(max.y, r.y)};
    }

    return {min, max - min};
}

// Offset from the layout box origin to the origin of the unrotated client frame (same center, swapped size).
static Vector2D frameOffset(const Vector2D& size, int q) {
    if (q % 2 == 0)
        return {};
    return (size - Vector2D{size.y, size.x}) / 2.0;
}

static Hyprutils::Math::eTransform rotationTransform(int q) {
    // HYPRUTILS_TRANSFORM_* follow wl_output_transform, which counts counter-clockwise
    switch (normalizeQuarters(q)) {
        case 1: return HYPRUTILS_TRANSFORM_270;
        case 2: return HYPRUTILS_TRANSFORM_180;
        case 3: return HYPRUTILS_TRANSFORM_90;
        default: return HYPRUTILS_TRANSFORM_NORMAL;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// state helpers

static int quartersFor(const CWindow* w) {
    if (!w)
        return 0;

    for (const auto& r : g_rotations) {
        if (!r.window.expired() && r.window.get() == w)
            return r.quarters;
    }
    return 0;
}

static bool inMainTree(const PHLWINDOW& w, const SP<CWLSurfaceResource>& surf) {
    if (!w || !surf || !w->wlSurface() || !w->wlSurface()->resource())
        return false;

    bool found = false;
    w->wlSurface()->resource()->breadthfirst([&found, &surf](SP<CWLSurfaceResource> s, const Vector2D&, void*) { found = found || s == surf; }, nullptr);
    return found;
}

// Rotated window whose main surface tree (main surface + subsurfaces, not popups) contains surf.
static PHLWINDOW windowForSurface(const SP<CWLSurfaceResource>& surf) {
    if (!surf)
        return nullptr;

    for (const auto& r : g_rotations) {
        const auto w = r.window.lock();
        if (w && w->m_isMapped && inMainTree(w, surf))
            return w;
    }

    return nullptr;
}

// Input-side geometry (no render offsets, like the input manager).
static CBox inputBoxFor(const PHLWINDOW& w) {
    return CBox{w->position(IGeometric::GEOMETRIC_CURRENT), w->size(IGeometric::GEOMETRIC_CURRENT)};
}

// What has to be added to a surface-local coordinate computed by Hyprland for the visual point pos,
// to get the coordinate in the client's (unrotated) frame.
static Vector2D localDelta(const PHLWINDOW& w, const Vector2D& pos, int q) {
    const CBox     BOX = inputBoxFor(w);
    const Vector2D C   = BOX.middle();
    const Vector2D U   = C + rotateVec(pos - C, -q);
    return (U - pos) - frameOffset(BOX.size(), q);
}

// ---------------------------------------------------------------------------------------------------------------------
// hooks: size + rendering

typedef Vector2D (*origRealToReportSize)(CWindow*);
static Vector2D hkRealToReportSize(CWindow* thisptr) {
    const Vector2D SIZE = ((origRealToReportSize)g_reportSizeHook->m_original)(thisptr);
    return quartersFor(thisptr) % 2 == 1 ? Vector2D{SIZE.y, SIZE.x} : SIZE;
}

static int quartersForElement(const CSurfacePassElement* el) {
    if (!el->m_data.pWindow || el->m_data.popup || !el->m_data.surface)
        return 0;
    return quartersFor(el->m_data.pWindow.get());
}

typedef CBox (*origGetTexBox)(CSurfacePassElement*);
static CBox hkGetTexBox(CSurfacePassElement* thisptr) {
    const int Q = g_rotations.empty() ? 0 : quartersForElement(thisptr);
    if (Q == 0)
        return ((origGetTexBox)g_getTexBoxHook->m_original)(thisptr);

    const auto&    D      = thisptr->m_data;
    const Vector2D ORIGIN = D.pos - D.pMonitor->m_position; // layout box origin, monitor-local
    const Vector2D SIZE   = {D.w, D.h};
    const Vector2D CENTER = ORIGIN + SIZE / 2.0;

    // the main surface fills the box after rotation, exactly like an unrotated window does
    if (D.mainSurface)
        return CBox{ORIGIN + D.localPos, SIZE};

    // subsurfaces: place them in the unrotated client frame, then rotate that placement
    CBox box = ((origGetTexBox)g_getTexBoxHook->m_original)(thisptr);
    box.translate(frameOffset(SIZE, Q));
    return rotateBoxAround(box, CENTER, Q);
}

typedef void (*origDrawSurface)(void*, WP<CSurfacePassElement>, const CRegion&);
static void hkDrawSurface(void* thisptr, WP<CSurfacePassElement> element, const CRegion& damage) {
    const auto ORIG = (origDrawSurface)g_drawSurfaceHook->m_original;
    // pass elements are unique pointers: their weak refs cannot be locked, only dereferenced
    auto*      EL = element ? element.get() : nullptr;
    const int  Q  = (EL && !g_rotations.empty()) ? quartersForElement(EL) : 0;

    if (Q == 0 || !EL->m_data.texture) {
        ORIG(thisptr, element, damage);
        return;
    }

    const auto TEX = EL->m_data.texture;
    const auto OLD = TEX->m_transform;

    TEX->m_transform  = Math::composeTransform(rotationTransform(Q), OLD);
    g_drawingQuarters = Q;

    ORIG(thisptr, element, damage);

    g_drawingQuarters = 0;
    TEX->m_transform  = OLD;
}

typedef void (*origCalculateUV)(void*, PHLWINDOW, SP<CWLSurfaceResource>, PHLMONITOR, bool, const Vector2D&, const Vector2D&, bool);
static void hkCalculateUV(void* thisptr, PHLWINDOW window, SP<CWLSurfaceResource> surface, PHLMONITOR monitor, bool main, const Vector2D& projSize,
                          const Vector2D& projSizeUnscaled, bool fixMisalignedFSV1) {
    const auto ORIG = (origCalculateUV)g_calculateUVHook->m_original;

    // the projection box is rotated, the buffer is not: fit UVs in the unrotated frame
    if (g_drawingQuarters % 2 == 1)
        ORIG(thisptr, window, surface, monitor, main, Vector2D{projSize.y, projSize.x}, Vector2D{projSizeUnscaled.y, projSizeUnscaled.x}, fixMisalignedFSV1);
    else
        ORIG(thisptr, window, surface, monitor, main, projSize, projSizeUnscaled, fixMisalignedFSV1);
}

typedef void (*origDamageSurface)(void*, SP<CWLSurfaceResource>, double, double, double);
static void hkDamageSurface(void* thisptr, SP<CWLSurfaceResource> surf, double x, double y, double scale) {
    ((origDamageSurface)g_damageSurfaceHook->m_original)(thisptr, surf, x, y, scale);

    if (g_rotations.empty())
        return;

    if (const auto w = windowForSurface(surf))
        g_pHyprRenderer->damageWindow(w);
}

// ---------------------------------------------------------------------------------------------------------------------
// hooks: input

typedef SP<CWLSurfaceResource> (*origWindowSurfaceAt)(const void*, const Vector2D&, PHLWINDOW, Vector2D&);
static SP<CWLSurfaceResource> hkWindowSurfaceAt(const void* thisptr, const Vector2D& pos, PHLWINDOW window, Vector2D& surfaceLocal) {
    const auto ORIG = (origWindowSurfaceAt)g_windowSurfaceAtHook->m_original;
    const int  Q    = window ? quartersFor(window.get()) : 0;

    if (Q == 0)
        return ORIG(thisptr, pos, window, surfaceLocal);

    // popups are drawn unrotated, so they are hit-tested as-is
    Vector2D sl;
    auto     surf = ORIG(thisptr, pos, window, sl);
    if (surf && !inMainTree(window, surf)) {
        surfaceLocal = sl;
        return surf;
    }

    // ask for the surface under the unrotated point, expressed in Hyprland's frame for this window
    const Vector2D DELTA = localDelta(window, pos, Q);
    surf                 = ORIG(thisptr, pos + DELTA, window, sl);

    // the CSeatManager hooks add DELTA again, so hand back the visual-space value
    surfaceLocal = sl - DELTA;
    return surf;
}

static Vector2D remapLocal(const SP<CWLSurfaceResource>& surf, const Vector2D& local) {
    const auto w = windowForSurface(surf);
    if (!w)
        return local;

    Vector2D delta = localDelta(w, g_pInputManager->getMouseCoordsInternal(), quartersFor(w.get()));
    if (w->m_isX11)
        delta = delta * w->m_X11SurfaceScaledBy;

    return local + delta;
}

typedef void (*origSetPointerFocus)(void*, SP<CWLSurfaceResource>, const Vector2D&);
static void hkSetPointerFocus(void* thisptr, SP<CWLSurfaceResource> surf, const Vector2D& local) {
    const Vector2D LOCAL = g_rotations.empty() ? local : remapLocal(surf, local);
    ((origSetPointerFocus)g_setPointerFocusHook->m_original)(thisptr, surf, LOCAL);
}

typedef void (*origSendPointerMotion)(void*, uint32_t, const Vector2D&);
static void hkSendPointerMotion(void* thisptr, uint32_t timeMs, const Vector2D& local) {
    const Vector2D LOCAL = g_rotations.empty() ? local : remapLocal(g_pSeatManager->m_state.pointerFocus.lock(), local);
    ((origSendPointerMotion)g_sendPointerMotionHook->m_original)(thisptr, timeMs, LOCAL);
}

// ---------------------------------------------------------------------------------------------------------------------
// commands

static void setRotation(const PHLWINDOW& w, int quarters) {
    quarters      = normalizeQuarters(quarters);
    const int OLD = quartersFor(w.get());
    if (OLD == quarters)
        return;

    std::erase_if(g_rotations, [&w](const SRotation& r) { return r.window.expired() || r.window.get() == w.get(); });
    if (quarters != 0)
        g_rotations.push_back({.window = w, .quarters = quarters});

    if ((OLD % 2) != (quarters % 2))
        w->sendWindowSize(true);

    g_pHyprRenderer->damageWindow(w);
}

// Rotation command on the active window, shared by the Lua function and the dispatcher.
//   integer (degrees, multiple of 90): absolute clockwise angle, e.g. 0, 90, 180, 270, -90
//   "cw" (default), "ccw", "flip": relative quarter turns; "reset": back to 0
// Returns an error message on bad input.
static std::optional<std::string> runCommand(std::string arg) {
    std::erase_if(arg, [](unsigned char c) { return std::isspace(c); });

    const auto PWINDOW = Desktop::focusState()->window();
    if (!PWINDOW)
        return "no active window";

    const int CURRENT = quartersFor(PWINDOW.get());

    if (arg.empty() || arg == "cw")
        setRotation(PWINDOW, CURRENT + 1);
    else if (arg == "ccw")
        setRotation(PWINDOW, CURRENT - 1);
    else if (arg == "flip")
        setRotation(PWINDOW, CURRENT + 2);
    else if (arg == "reset")
        setRotation(PWINDOW, 0);
    else {
        int        degrees = 0;
        const auto END     = arg.data() + arg.size();
        const auto RES     = std::from_chars(arg.data(), END, degrees);
        if (RES.ec != std::errc{} || RES.ptr != END || degrees % 90 != 0)
            return "expected cw, ccw, flip, reset or a multiple of 90 degrees, got \"" + arg + "\"";
        setRotation(PWINDOW, degrees / 90);
    }

    return std::nullopt;
}

// hl.plugin.window_rotate.rotate([arg]): arg is a number of degrees or one of the command strings
static int luaRotate(lua_State* L) {
    bool failed = false;
    {
        std::string arg;
        if (lua_gettop(L) >= 1 && lua_type(L, 1) == LUA_TNUMBER)
            arg = std::to_string(sc<long long>(std::llround(lua_tonumber(L, 1))));
        else if (lua_gettop(L) >= 1 && lua_type(L, 1) == LUA_TSTRING)
            arg = lua_tostring(L, 1);

        if (const auto ERR = runCommand(arg); ERR) {
            lua_pushstring(L, ("hypr-window-rotate: " + *ERR).c_str());
            failed = true;
        }
    }

    // lua_error longjmps, so no C++ object may be alive here
    return failed ? lua_error(L) : 0;
}

// window_rotate dispatcher, for hyprlang (hyprland.conf) configs
static SDispatchResult dispatchRotate(std::string arg) {
    if (const auto ERR = runCommand(arg); ERR)
        return {.success = false, .error = *ERR};
    return {};
}

// ---------------------------------------------------------------------------------------------------------------------
// plugin entry points

static CFunctionHook* hookByName(const std::string& name, const std::string& demangledPrefix, void* destination) {
    for (const auto& fn : HyprlandAPI::findFunctionsByName(PHANDLE, name)) {
        if (!fn.demangled.starts_with(demangledPrefix))
            continue;

        auto* hook = HyprlandAPI::createFunctionHook(PHANDLE, fn.address, destination);
        if (!hook || !hook->hook())
            throw std::runtime_error("[hypr-window-rotate] failed to hook " + demangledPrefix);
        return hook;
    }

    throw std::runtime_error("[hypr-window-rotate] function not found: " + demangledPrefix);
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    const std::string HASH        = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();
    if (HASH != CLIENT_HASH) {
        HyprlandAPI::addNotification(PHANDLE, "[hypr-window-rotate] built for a different Hyprland version, rebuild it", CHyprColor{1.0, 0.2, 0.2, 1.0}, 8000);
        throw std::runtime_error("[hypr-window-rotate] version mismatch");
    }

    g_reportSizeHook        = hookByName("realToReportSize", "Desktop::View::CWindow::realToReportSize(", (void*)&hkRealToReportSize);
    g_getTexBoxHook         = hookByName("getTexBox", "CSurfacePassElement::getTexBox(", (void*)&hkGetTexBox);
    g_drawSurfaceHook       = hookByName("drawSurface", "Render::IElementRenderer::drawSurface(", (void*)&hkDrawSurface);
    g_calculateUVHook       = hookByName("calculateUVForSurface", "Render::IElementRenderer::calculateUVForSurface(", (void*)&hkCalculateUV);
    g_damageSurfaceHook     = hookByName("damageSurface", "Render::IHyprRenderer::damageSurface(", (void*)&hkDamageSurface);
    g_windowSurfaceAtHook   = hookByName("windowSurfaceAt", "Desktop::CViewHitTester::windowSurfaceAt(", (void*)&hkWindowSurfaceAt);
    g_setPointerFocusHook   = hookByName("setPointerFocus", "CSeatManager::setPointerFocus(", (void*)&hkSetPointerFocus);
    g_sendPointerMotionHook = hookByName("sendPointerMotion", "CSeatManager::sendPointerMotion(", (void*)&hkSendPointerMotion);

    // the Lua function only exists with a Lua config, the dispatcher serves hyprlang configs
    if (Config::mgr() && Config::mgr()->type() == Config::CONFIG_LUA && !HyprlandAPI::addLuaFunction(PHANDLE, "window_rotate", "rotate", luaRotate))
        throw std::runtime_error("[hypr-window-rotate] failed to register hl.plugin.window_rotate.rotate");

    if (!HyprlandAPI::addDispatcherV2(PHANDLE, "window_rotate", dispatchRotate))
        throw std::runtime_error("[hypr-window-rotate] failed to register the window_rotate dispatcher");

    return {"hypr-window-rotate", "Rotate individual windows in 90 degree steps", "dox187", PLUGIN_VERSION};
}

APICALL EXPORT void PLUGIN_EXIT() {
    // give every rotated client its normal size back while the hooks are still active
    const auto ROTATIONS = g_rotations;
    for (const auto& r : ROTATIONS) {
        if (const auto w = r.window.lock())
            setRotation(w, 0);
    }
    g_rotations.clear();
}
