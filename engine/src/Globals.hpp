#pragma once

#include "GlassLayerSurface.hpp"
#include "PluginConfig.hpp"
#include "ShaderManager.hpp"

#include <array>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/render/Framebuffer.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/SharedDefs.hpp>
#include <hyprutils/signal/Listener.hpp>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

class CGlassDecoration;
class CGlassSubsurfaceState;

struct SGlobalState {
    // Event listeners are owned here so PLUGIN_EXIT unregisters them. Static
    // listeners outlived the plugin and fired after unload -> SEGV on reload.
    std::vector<Hyprutils::Signal::CHyprSignalListener> listeners;

    // These WPs wrap the UP<IHyprWindowDecoration> Hyprland owns: dereference with
    // .get() only, lock() asserts on a unique-owned pointer and terminates.
    std::vector<WP<CGlassDecoration>> decorations;
    CShaderManager                    shaderManager;
    SPluginConfig                     config;

    // User-defined presets (populated from config keyword, swapped in on configReloaded)
    std::unordered_map<std::string, SCustomPreset> customPresets;

    // Shared blur temp framebuffer (reused across all decorations since they render sequentially)
    // blur ping-pong buffers, one per size (GlassRenderer::blurBackground)
    struct SBlurTemp {
        SP<Render::IFramebuffer> fb;
        uint64_t                 used = 0;
    };
    std::array<SBlurTemp, 6> blurTemps;
    uint64_t                 blurTempClock = 0;

    // Per-monitor temp FBO for subsurface item glass, shared serially by every
    // glassed item on that monitor in a frame — a per-item copy would be one
    // full-monitor-sized allocation each. Safe because items on the same
    // monitor are visited one at a time: CGlassSubsurfacePassElement::draw()
    // (redirect) and CGlassSubsurfaceCompositeElement::draw()
    // (composite+restore) for one item always fully bracket each other before
    // the next item's pair starts (see main.cpp's hkRenderPassAdd) — no two
    // items on the same monitor ever have it redirected at once. Keyed by
    // MONITORID rather than CMonitor* (same reasoning as sceneGeneration
    // below); entries are dropped on monitor removal (see main.cpp).
    std::unordered_map<MONITORID, SP<Render::IFramebuffer>> subsurfaceTempFramebuffers;

    // Layer surface glass state (one per tracked layer, keyed by raw pointer).
    // shared_ptr so CGlassLayerPassElement can hold a copy that survives map erasure mid-frame.
    std::unordered_map<Desktop::View::CLayerSurface*, std::shared_ptr<CGlassLayerSurface>> layerSurfaces;
    // Glass under a layer's popups (bar flyouts, menus), one per layer, drawn in
    // Hyprland's separate popup pass and shaped by the popups' own contour.
    std::unordered_map<Desktop::View::CLayerSurface*, std::shared_ptr<CGlassLayerSurface>> layerPopupSurfaces;

    // Subsurface item glass state, one per glassed wl_subsurface, keyed by a
    // weak ref so a destroyed surface's entry is simply unreachable rather
    // than needing an explicit teardown signal (unlike layers/windows,
    // hyprglass has no destroy event for an arbitrary subsurface). hyprutils
    // provides std::hash<CWeakPointer<T>> (see WeakPtr.hpp), same as
    // watchedSurfaces below. Pruned lazily wherever it's walked (see main.cpp).
    std::unordered_map<WP<CWLSurfaceResource>, std::shared_ptr<CGlassSubsurfaceState>> subsurfaceGlass;

    // Parsed namespace whitelist (empty = match all when layers enabled)
    std::unordered_set<std::string> layerNamespaceFilter;
    // Parsed namespace blacklist (always excluded, takes priority over whitelist)
    std::unordered_set<std::string> layerNamespaceExclude;
    // Per-namespace preset overrides (namespace → preset name)
    std::unordered_map<std::string, std::string> layerNamespacePresets;
    std::unordered_map<std::string, std::string> layerNamespacePopupPresets; // popup_preset: the layer's popups' own glass
    std::unordered_map<std::string, float>       layerNamespacePopupShadow;  // popup_shadow: their drop shadow
    std::unordered_map<std::string, float>       layerNamespacePopupRadius;  // popup_radius: glass as an exact rounded box of each popup
    // Per-namespace mask alpha threshold (namespace → threshold, default 0.001)
    std::unordered_map<std::string, float> layerNamespaceMaskThresholds;
    // Per-namespace live resample override (namespace → enabled)
    std::unordered_map<std::string, bool> layerNamespaceLiveResample;
    // Per-namespace mask mode override (namespace → mode)
    std::unordered_map<std::string, ELayerMaskMode> layerNamespaceMaskModes;
    // Per-namespace glass shape inside the layer box: inset (logical px, top
    // right bottom left) and corner radius (-1 = capsule). Glass covers the
    // whole shape whatever the surface draws, e.g. a floating capsule bar.
    std::unordered_map<std::string, SLayerShape> layerNamespaceShapes;
    // Namespaces whose popups get glass too
    std::unordered_set<std::string> layerNamespacePopups;
    // Namespaces drawn as contour cards (seen so): the engine owns their closes, so
    // Hyprland's fade-out snapshot of them is left empty
    std::unordered_set<std::string> contourCardNamespaces;
    // Namespaces whose glass flips light/dark with its backdrop, glyphs included
    std::unordered_set<std::string> layerNamespaceAdaptive;
    // Per-namespace drop shadow opacity under the glass
    std::unordered_map<std::string, float> layerNamespaceShadow;
    std::unordered_set<std::string>        layerNamespaceNoHeldClose; // held_close = false
    std::unordered_set<std::string>        layerNamespaceFillKeyTop;  // fill_key_top = true: a thin line in the key colour along the bottom edge is drawn along the top instead
    // Per-namespace fill colour to see through: rgb 0..1 and the alpha it keeps
    std::unordered_map<std::string, std::array<float, 4>> layerNamespaceFillKey;
    // popup_fill_key: the same, for the layer's popups (the bar's tooltips)
    std::unordered_map<std::string, std::array<float, 4>> layerNamespacePopupFillKey;

    // Per-monitor generation counter, incremented when the scene behind layers
    // changes on that monitor. Layer surfaces compare to their cached value to
    // skip redundant blur work. Per-monitor avoids cross-monitor feedback loops
    // where re-sampling on an idle monitor captures its own stale glass output.
    // Keyed by MONITORID rather than CMonitor* so the code builds on both
    // Hyprland <= 0.55.x (global CMonitor) and git (Monitor::CMonitor), and a
    // stale entry can never alias a reallocated monitor object.
    std::unordered_map<MONITORID, uint64_t> sceneGeneration;

    // Press glow (glass lights up from within where it is pressed,
    // the glow spreading through it). The last pointer press, global logical.
    struct SPressGlow {
        Vector2D                              pos;
        std::chrono::steady_clock::time_point at;
        bool                                  live = false;
        float                                 hold = -1.0f; // test: freeze the glow at this age
    } pressGlow;
    static constexpr float PRESS_GLOW_SECONDS = 0.6f;
    static constexpr float PRESS_GLOW_REACH   = 340.0f; // logical px

    // 0..1 seconds-normalized age of a live press glow, or -1
    // When the user last changed what a card shows from the keyboard or the wheel
    // (typing into a search, scrolling a list): its content may shrink or thin out
    // for a moment then, which is not the card closing
    std::chrono::steady_clock::time_point contentInputAt{};
    // The pointer moving with no button down: what changes then is hover (a tooltip
    // coming and going, a row lighting up), a card's layout, never its close (a close
    // by pointer is a press: on the bar, or outside the card)
    std::chrono::steady_clock::time_point pointerMoveAt{}, pointerPressAt{};
    bool hoverRecent() const {
        const auto now = std::chrono::steady_clock::now();
        return pointerMoveAt.time_since_epoch().count() > 0 && now - pointerMoveAt < std::chrono::milliseconds(300) &&
            !(pointerPressAt.time_since_epoch().count() > 0 && now - pointerPressAt < std::chrono::milliseconds(600));
    }
    bool contentInputRecent() const {
        return contentInputAt.time_since_epoch().count() > 0 && std::chrono::steady_clock::now() - contentInputAt < std::chrono::milliseconds(450);
    }

    float pressGlowAge() const {
        if (!pressGlow.live)
            return -1.0f;
        if (pressGlow.hold >= 0.0f)
            return pressGlow.hold;
        const float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - pressGlow.at).count() / PRESS_GLOW_SECONDS;
        return t < 1.0f ? t : -1.0f;
    }

    // bumped by every config change (a reload, or a hl.plugin.hyprglass call): what
    // was computed from the old settings is stale (windows' kept glass)
    uint64_t configSerial = 0;

    // Dark glass and Light glass as drawn now, easing to their settings
    // (GlassRenderer::stepTone; -1: not drawn yet)
    float                                 darkShown = -1.0f, lightShown = -1.0f;
    std::chrono::steady_clock::time_point toneAt{};

    uint64_t getSceneGeneration(const PHLMONITOR& mon) const {
        if (!mon)
            return 0;
        auto it = sceneGeneration.find(mon->m_id);
        return it != sceneGeneration.end() ? it->second : 0;
    }
    void bumpSceneGeneration(const PHLMONITOR& mon) {
        if (mon)
            sceneGeneration[mon->m_id]++;
    }

    // Render-order fingerprint: a per-monitor running hash folded from every
    // glass-eligible window's identity/geometry/alpha in z-order, compared frame
    // to frame to catch stacking/membership changes (e.g. a window closing behind
    // a cached glass window) that no other event bumps scene generation for.
    struct SRenderFingerprint {
        size_t runningHash = 0;
        size_t lastHash    = 0;
    };
    std::unordered_map<MONITORID, SRenderFingerprint> renderFingerprints;

    // Surface-commit observation driving the layer live resample: the
    // subscriptions that discover surfaces, and one commit/destroy pair per
    // watched surface. Owned here for the same reason as `listeners`: PLUGIN_EXIT
    // must drop them before render teardown.
    struct SWatchedSurface {
        Hyprutils::Signal::CHyprSignalListener commit;
        Hyprutils::Signal::CHyprSignalListener destroy;
    };
    std::vector<Hyprutils::Signal::CHyprSignalListener>         observerListeners;
    std::unordered_map<WP<CWLSurfaceResource>, SWatchedSurface> watchedSurfaces;

    // Mirrors anySelfSampleConfigured(), refreshed with the observer. Read on every
    // watched commit, so it must stay a plain bool and not a config walk.
    bool selfSampleConfigured = false;

    // Bumped on RENDER_BEGIN, so one value per monitor frame. 0 is reserved for
    // renders we do not manage (snapshots, screencopy, overview framebuffers).
    uint64_t frameSerial = 0;

    // Hyprland renders a floating window that is allowed over fullscreen more
    // than once per frame. The redundant copy is redirected into this pass,
    // which is dropped instead of rendered.
    // sink before guard: members destruct in reverse declaration order, and the
    // guard must point m_currentPass away from sink before sink dies.
    struct SDedupeState {
        Render::CRenderPass                  sink;
        UP<Hyprutils::Utils::CScopeGuard>    guard;
        std::vector<Desktop::View::CWindow*> dropped; // identity only, never dereferenced
        bool                                 sawFullscreen = false;

        // one workspace pass
        void resetEpoch() {
            guard.reset();
            sink.clear();
            sawFullscreen = false;
        }

        // one monitor frame: `dropped` outlives the epoch, a window is rendered
        // by the pass of another visible workspace too and may only lose one copy
        void reset() {
            resetEpoch();
            dropped.clear();
        }
    } dedupe;

    // renderLayer hook
    CFunctionHook* renderLayerHook = nullptr;
    CFunctionHook* renderFadeoutsHook = nullptr; // a layer's close, drawn after it unmapped
    CFunctionHook* captureOutputHook  = nullptr; // a one-shot screen capture (a screenshot's freeze): closes end first

    // Render::CRenderPass::add hook (subsurface item glass)
    CFunctionHook* renderPassAddHook             = nullptr;
    bool           renderPassAddSymbolFound      = false; // for the failure notification text
    bool           subsurfaceHookFailureNotified = false;
};

using Render::GL::g_pHyprOpenGL;

inline HANDLE                        PHANDLE = nullptr;
inline std::unique_ptr<SGlobalState> g_pGlobalState;

// Decoration registered for this window, or nullptr. Borrowed, never owned.
CGlassDecoration* glassDecorationFor(const PHLWINDOW& window);

inline constexpr std::string_view PLUGIN_NAME        = "hyprglass";
inline constexpr std::string_view PLUGIN_DESCRIPTION = "Physically based glass for Hyprland";
inline constexpr std::string_view PLUGIN_AUTHOR      = "Hyprnux";
#ifndef HYPRGLASS_VERSION
#define HYPRGLASS_VERSION "dev"
#endif
inline constexpr std::string_view PLUGIN_VERSION     = HYPRGLASS_VERSION;
