#include <ctime>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include "GlassDecoration.hpp"
#include "motion/motion.hpp"
#include "BackgroundDamageObserver.hpp"
#include "BuiltInPresets.hpp"
#include "Diagnostics.hpp"
#include "GlassPassElement.hpp"
#include "GlassRenderer.hpp"
#include "Globals.hpp"
#include "Hash.hpp"
#include "RenderGuards.hpp"
#include "WindowGeometry.hpp"
#include "WorkspaceAnimation.hpp"

#include <algorithm>
#include <GLES3/gl32.h>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/managers/SessionLockManager.hpp>
#include <hyprland/src/desktop/rule/windowRule/WindowRuleApplicator.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>

CGlassDecoration::CGlassDecoration(PHLWINDOW window)
    : IHyprWindowDecoration(window), m_window(window) {
}

CGlassDecoration::~CGlassDecoration() {
    withdrawNoBlur();
    // (its timer calls back into this)
    if (m_owedTimer && g_pEventLoopManager)
        g_pEventLoopManager->removeTimer(m_owedTimer);
}

// Glass replaces Hyprland's blur for this window. Mark glassed windows with
// the noblur property so Hyprland composites their translucency against the
// live framebuffer (which contains the glass) instead of its pre-frame cached
// blur snapshot, which is captured before plugin decorations render (#46).
void CGlassDecoration::updateNoBlurProp(bool glassEnabled) {
    const auto& config = g_pGlobalState->config;
    const bool manage = config.manageWindowBlur && **config.manageWindowBlur;

    if (!manage || !glassEnabled) {
        withdrawNoBlur();
        return;
    }

    if (m_noBlurApplied)
        return;

    try {
        const auto window = m_window.lock();
        if (window && window->m_ruleApplicator) {
            window->m_ruleApplicator->noBlur().set(true, Desktop::Types::PRIORITY_SET_PROP);
            m_noBlurApplied = true;
            damageEntire();
        }
    } catch (...) {}
}

void CGlassDecoration::withdrawNoBlur() {
    if (!m_noBlurApplied)
        return;
    m_noBlurApplied = false;

    try {
        const auto window = m_window.lock();
        if (window && window->m_ruleApplicator) {
            window->m_ruleApplicator->noBlur().unset(Desktop::Types::PRIORITY_SET_PROP);
            damageEntire();
        }
    } catch (...) {}
}

// Fullscreen toggles re-apply window rules, which can drop the noblur prop
// while m_noBlurApplied still claims it's held.
void CGlassDecoration::onFullscreenStateChanged() {
    m_noBlurApplied = false;
    damageEntire();
}

static float selfSampleFor(const SResolveContext& ctx) {
    return std::clamp(resolvePresetFloat(ctx, &SPresetValues::selfSample, &SOverridableConfig::selfSample), 0.0f, 1.0f);
}

CGlassDecoration::EEnabledResolution CGlassDecoration::resolveEnabled() const {
    // (asked many times a frame while a window moves: once per frame and config)
    const uint64_t serial = g_pGlobalState->frameSerial, cfg = g_pGlobalState->configSerial;
    if (m_enabledValid && serial == m_enabledFrame && cfg == m_enabledConfig)
        return m_enabledCached;
    m_enabledCached = resolveEnabledUncached();
    m_enabledFrame  = serial;
    m_enabledConfig = cfg;
    m_enabledValid  = serial != 0; // (no frames counted yet: never trust it)
    return m_enabledCached;
}

CGlassDecoration::EEnabledResolution CGlassDecoration::resolveEnabledUncached() const {
    const auto& config = g_pGlobalState->config;
    const bool globalEnabled = config.enabled && **config.enabled;
    const bool skipOpaque    = config.skipOpaqueWindows && **config.skipOpaqueWindows;

    try {
        const auto window = m_window.lock();
        if (window && window->m_ruleApplicator) {
            const auto& tags = window->m_ruleApplicator->m_tagKeeper;
            // isTagged() already matches dynamic tags ("tag*") — no stripping needed here.
            // Disabled tag wins over enabled tag if both are present.
            if (tags.isTagged(std::string(TAG_DISABLED)))
                return EEnabledResolution::Disabled;
            if (tags.isTagged(std::string(TAG_ENABLED)))
                return EEnabledResolution::Enabled;
        }

        // A global disable must pre-empt the opaque-skip check below: otherwise
        // an opaque window with the plugin off entirely would still resolve to
        // DisabledBecauseOpaque and inflate the opaque-skip counter in draw().
        if (!globalEnabled)
            return EEnabledResolution::Disabled;

        // Nothing behind an opaque window is visible, unless the window
        // self-samples: then the glass shows the window's own content, which
        // does change, so the opaque-skip must yield to it.
        if (skipOpaque && window && window->opaque()) {
            const bool         isDark = resolveThemeIsDark();
            const std::string  preset = resolvePresetName();
            const SResolveContext ctx = {preset, isDark, config, g_pGlobalState->customPresets};
            if (selfSampleFor(ctx) <= 0.0f)
                return EEnabledResolution::DisabledBecauseOpaque;
        }
    } catch (...) {}

    return globalEnabled ? EEnabledResolution::Enabled : EEnabledResolution::Disabled;
}

bool CGlassDecoration::resolveThemeIsDark() const {
    try {
        const auto window = m_window.lock();
        if (window && window->m_ruleApplicator) {
            const std::string lightTag = std::string(TAG_THEME_PREFIX) + "light";
            const std::string darkTag  = std::string(TAG_THEME_PREFIX) + "dark";
            if (window->m_ruleApplicator->m_tagKeeper.isTagged(lightTag))
                return false;
            if (window->m_ruleApplicator->m_tagKeeper.isTagged(darkTag))
                return true;
        }

        const auto& config = g_pGlobalState->config;
        const auto theme = readStringConfig(config.defaultTheme);
        if (!theme.empty())
            return theme != "light";
    } catch (...) {}

    return true;
}

std::string CGlassDecoration::resolvePresetName() const {
    try {
        const auto window = m_window.lock();
        if (window && window->m_ruleApplicator) {
            for (const auto& tag : window->m_ruleApplicator->m_tagKeeper.getTags()) {
                if (tag.starts_with(TAG_PRESET_PREFIX))
                    return stripDynamicTagMarker(tag.substr(TAG_PRESET_PREFIX.size()));
            }
        }

        const auto& config = g_pGlobalState->config;
        const auto preset = readStringConfig(config.defaultPreset);
        if (!preset.empty())
            return std::string(preset);
    } catch (...) {}

    return "default";
}

SDecorationPositioningInfo CGlassDecoration::getPositioningInfo() {
    SDecorationPositioningInfo info;
    info.priority       = 10000;
    info.policy         = DECORATION_POSITION_ABSOLUTE;
    info.desiredExtents = {{0, 0}, {0, 0}};
    return info;
}

void CGlassDecoration::onPositioningReply(const SDecorationPositioningReply& reply) {}

void CGlassDecoration::queueGlassPass(float alpha) {
    // A duplicate copy is redirected into the dedupe sink and dropped whole: it
    // needs no glass, and stamping it would leave the surviving element stale.
    if (g_pGlobalState->dedupe.guard)
        return;

    CGlassPassElement::SGlassPassData data{m_self, alpha};

    // Only the real monitor pass is de-duplicated: snapshots, screencopy and
    // overview framebuffers render the window once, out of frame order.
    const bool managed = g_pGlobalState->frameSerial != 0 && !g_pHyprRenderer->m_bRenderingSnapshot &&
        g_pHyprRenderer->m_renderData.projectionType == Render::RPT_MONITOR;

    if (managed) {
        if (m_glassFrameSerial != g_pGlobalState->frameSerial) {
            m_glassFrameSerial = g_pGlobalState->frameSerial;
            m_glassQueueIndex  = 0;
        } else
            ++m_glassQueueIndex;

        data.frameSerial = m_glassFrameSerial;
        data.queueIndex  = m_glassQueueIndex;
    }

    // m_renderPass, never addPassElement: draw() runs inside Hyprland's own
    // per-window redirect for transformed windows (motion blur), whose pass
    // renders into a work buffer cleared to transparent — nothing to sample.
    g_pHyprRenderer->m_renderPass.add(makeUnique<CGlassPassElement>(data));
}

void CGlassDecoration::draw(PHLMONITOR monitor, float const& alpha) {
    if (!g_pGlobalState)
        return;
    // A throttled background change still owed. The one dirty mark taken from a
    // render: the throttled mark already damaged the padded box, so this frame
    // covers the resample it asks for.
    if (m_trailingDirty)
        markBackgroundDirty();

    // Render-order fingerprint: fold this window's identity/geometry/alpha into
    // the monitor's running hash in z-order, before resolveEnabled() so background
    // windows still count. Guarded by isForeignRender(), not a bare mainFB check,
    // since a foreign replay's z-order isn't this frame's real one. Folded at most
    // once per frameSerial: draw() itself runs 2-3x per monitor frame for a floating
    // window over fullscreen, and folding every copy would make the hash depend on
    // how many of those copies happened to render this frame rather than on the scene.
    if (monitor && !RenderGuards::isForeignRender() &&
        (g_pGlobalState->frameSerial == 0 || m_lastFoldedFrameSerial != g_pGlobalState->frameSerial)) {
        if (const auto window = m_window.lock()) {
            const auto workspace = window->m_workspace;
            const Vector2D workspaceRenderOffset =
                (workspace && !window->m_pinned) ? workspace->m_renderOffset->value() : Vector2D();
            const auto fullscreenMode = Fullscreen::controller()->getFullscreenModes(window).internal;

            auto& fingerprint = g_pGlobalState->renderFingerprints[monitor->m_id];
            Hash::hashCombine(fingerprint.runningHash, window.get(), window->positionAnimation()->value(),
                               window->sizeAnimation()->value(), alpha, fullscreenMode, workspaceRenderOffset);

            m_lastFoldedFrameSerial = g_pGlobalState->frameSerial;
        }
    }

    const auto enabledResolution = resolveEnabled();
    const bool enabled = enabledResolution == EEnabledResolution::Enabled;
    updateNoBlurProp(enabled);
    if (!enabled) {
        m_lastSelfSample = 0.0f;

        // Only count a skip that resolveEnabled() itself attributes to
        // skip_opaque_windows — a tag or global-disable skip doesn't have a
        // counter of its own (see EEnabledResolution) and isn't counted here.
        if (monitor && enabledResolution == EEnabledResolution::DisabledBecauseOpaque)
            Diagnostics::recordWindowOpaqueSkipped(monitor->m_id);
        return;
    }

    // A foreign render must not queue a pass element at all: it would sample its own
    // framebuffer into this window's real, persistent background cache.
    if (RenderGuards::isForeignRender())
        return;

    queueGlassPass(alpha);

    // A slide translates the scene under us without any geometry change, and
    // Hyprland's per-tick window damage carries none of our sampling padding.
    // Damage only: no cache state may be touched from a render.
    const auto window = m_window.lock();
    if (window && !window->m_pinned) {
        const auto workspace = window->m_workspace;
        if (workspace && workspace->m_renderOffset->isBeingAnimated())
            damageEntire();
    }
}

PHLWINDOW CGlassDecoration::getOwner() {
    return m_window.lock();
}

// Too soon for another resample: owed, not dropped (the change might be the last
// one, the end of a wallpaper transition). Paid by one timer when its turn comes;
// damaging now instead drew a frame that asked again, still too soon, and damaged
// again: the whole pane redrawn every frame while it waited.
void CGlassDecoration::oweResample(std::chrono::steady_clock::duration wait) {
    m_trailingDirty = true;
    if (m_owedTimer && m_owedTimer->armed())
        return;
    if (!m_owedTimer) {
        m_owedTimer = makeShared<CEventLoopTimer>(std::nullopt, [this](SP<CEventLoopTimer>, void*) {
            if (m_trailingDirty)
                markBackgroundDirty();
        }, nullptr);
        g_pEventLoopManager->addTimer(m_owedTimer);
    }
    m_owedTimer->updateTimeout(std::chrono::duration_cast<Time::steady_dur>(wait) + std::chrono::milliseconds(1));
}

void CGlassDecoration::markBackgroundDirty() {
    if (m_backgroundDirty) {
        m_trailingDirty = false; // the resample already pending covers it
        return;
    }

    const auto& config = g_pGlobalState->config;
    int64_t fps = config.windowsLiveResampleFps ? **config.windowsLiveResampleFps : 0;
    // frosted, what is behind shows as a soft blur: 10 updates a second follow it
    // as well as 24 (text scrolling under a frosted floating window)
    if (m_frost.amount() > 0.6f)
        fps = fps > 0 ? std::min<int64_t>(fps, 10) : 10;
    const auto now = std::chrono::steady_clock::now();
    if (fps > 0 && now - m_lastDirtyMark < std::chrono::nanoseconds(1'000'000'000 / fps)) {
        oweResample(m_lastDirtyMark + std::chrono::nanoseconds(1'000'000'000 / fps) - now);
        return;
    }
    m_lastDirtyMark = now;
    m_trailingDirty = false;

    m_backgroundDirty = true;
    // damage the full sample region: outside the committed area the framebuffer
    // still holds our previous glass output, which must not be re-sampled
    damageEntire();
}

void CGlassDecoration::markEdgeDirty(const CBox& damaged) {
    const auto window = m_window.lock();
    // (a full resample pending covers it; a blurred sample would need the patch
    // blurred and its blur reaches past it: there the rim's reflection is soft
    // anyway, and it follows at the next resample, as before)
    if (!window || m_backgroundDirty || !m_samplePatchable)
        return;
    const auto sampled = WindowGeometry::computePaddedGlobalBox(window, GlassRenderer::SAMPLE_PADDING_PX);
    const auto glass   = WindowGeometry::computePaddedGlobalBox(window, 0.0f);
    if (!sampled || !glass)
        return;
    const CBox rect = damaged.intersection(*sampled);
    if (rect.empty())
        return;
    // (a scrolling neighbour commits every frame: one growing rect, not a list)
    if (!m_edgeDirty.empty() && m_edgeDirty.back().overlaps(rect)) {
        auto& last = m_edgeDirty.back();
        const double x0 = std::min(last.x, rect.x), y0 = std::min(last.y, rect.y);
        last = CBox{x0, y0, std::max(last.x + last.w, rect.x + rect.w) - x0, std::max(last.y + last.h, rect.y + rect.h) - y0};
    } else if (m_edgeDirty.size() < 8)
        m_edgeDirty.push_back(rect);
    else {
        markBackgroundDirty(); // (changes all round: one full resample)
        return;
    }
    // the rim that mirrors it: the glass within the reflection's reach of the change
    const auto  monitor = window->m_monitor.lock();
    const float reach   = GlassRenderer::SAMPLE_PADDING_PX / (monitor ? monitor->m_scale : 1.0f);
    const CBox  rim     = rect.copy().expand(reach).intersection(*glass);
    if (!rim.empty())
        g_pHyprRenderer->damageBox(rim);
}

bool CGlassDecoration::wantsBackgroundResample(PHLMONITOR monitor, const CBox& transformBox) const {
    const auto& config = g_pGlobalState->config;
    if (!config.windowsBackgroundCache || !**config.windowsBackgroundCache)
        return true; // kill switch off: exact pre-cache behavior, no other state consulted

    if (!m_hasCachedSample)
        return true; // first frame: nothing to reuse yet

    // Per-monitor generation (see m_lastGenerationMonitor's declaration):
    // covers future commit/close-behind-cache invalidation as well as
    // existing event bumps, and forces a resample the instant a window
    // lands on a different monitor even if that monitor's counter happens
    // to numerically match the cached value.
    if (!monitor || monitor->m_id != m_lastGenerationMonitor || g_pGlobalState->getSceneGeneration(monitor) != m_lastSceneGeneration)
        return true;

    if (m_backgroundDirty)
        return true; // markBackgroundDirty() mark not yet escalated into a scene-generation bump

    const auto window = m_window.lock();
    if (!window)
        return true;

    // Own move/resize animation, re-checked every call rather than latched
    // once: updateWindow() bumps scene generation only at the animation's
    // start (no per-tick decoration callback exists), so every later frame of
    // the same still-interpolating animation needs this live poll to keep
    // resampling.
    if (window->positionAnimation()->isBeingAnimated() || window->sizeAnimation()->isBeingAnimated())
        return true;

    // A workspace slide or fade moves the whole scene behind us without any
    // geometry change of our own.
    if (WorkspaceAnimation::anyWorkspaceAnimating(monitor))
        return true;

    const auto source = g_pHyprRenderer->m_renderData.currentFB;
    if (!m_sampleFramebuffer || !source)
        return true;

    // Monitor/scale/DPI/HDR change: recompute the size sampleBackground()
    // would allocate this frame (never cached) and compare against the FBO's
    // actual size/format from the last real sample.
    const bool isDark          = resolveThemeIsDark();
    const std::string preset   = resolvePresetName();
    const SResolveContext ctx  = {preset, isDark, config, g_pGlobalState->customPresets};
    const float blurStrength   = resolvePresetFloat(ctx, &SPresetValues::blurStrength, &SOverridableConfig::blurStrength);
    const int   downscale     = GlassRenderer::sampleDownscale(blurStrength);

    const int fullWidth  = static_cast<int>(transformBox.w) + 2 * GlassRenderer::SAMPLE_PADDING_PX;
    const int fullHeight = static_cast<int>(transformBox.h) + 2 * GlassRenderer::SAMPLE_PADDING_PX;
    const int requiredWidth  = std::max(1, fullWidth / downscale);
    const int requiredHeight = std::max(1, fullHeight / downscale);

    if (m_sampleFramebuffer->m_size.x != requiredWidth || m_sampleFramebuffer->m_size.y != requiredHeight ||
        m_sampleFramebuffer->m_drmFormat != source->m_drmFormat)
        return true;

    return false;
}

void CGlassDecoration::renderPass(PHLMONITOR monitor, const float& alpha) {
    // Belt and braces: draw() already refuses to queue a pass element for a foreign
    // render, but a caller that reaches renderPass() during one anyway must not sample
    // the foreign framebuffer into this decoration's persistent background cache.
    if (RenderGuards::isForeignRender())
        return;

    auto& shaderManager = g_pGlobalState->shaderManager;
    shaderManager.initializeIfNeeded();

    if (!shaderManager.isInitialized())
        return;

    const auto window = m_window.lock();
    if (!window)
        return;

    const auto source = g_pHyprRenderer->m_renderData.currentFB;
    if (!source)
        return;

    auto optBox = WindowGeometry::computeWindowBox(window, monitor);
    if (!optBox)
        return;

    if (monitor)
        Diagnostics::recordWindowGlassDraw(monitor->m_id);

    CBox windowBox    = *optBox;
    CBox transformBox = WindowGeometry::applyMonitorTransform(windowBox, monitor);

    // Every non-discarded frame needs this regardless of cache hit/miss: a
    // cache-hit frame has no sampleBackground()/blurBackground() call for it
    // to sit after, and reusing a stale value here (e.g. glassAlpha after a
    // fade, or cornerRadius after a fullscreen toggle) would silently
    // mis-render the composite even though the cached sample itself is fine.
    const bool isDark          = resolveThemeIsDark();
    const std::string preset   = resolvePresetName();
    const SResolveContext ctx  = {preset, isDark, g_pGlobalState->config, g_pGlobalState->customPresets};

    float monitorScale = monitor->m_scale;

    // Hyprland renders internal-fullscreen windows unrounded (dontRound), we need to
    // match, or the glass would show rounded gaps at the screen corners
    const bool fsUnrounded = Fullscreen::controller()->getFullscreenModes(window).internal == Fullscreen::FSMODE_FULLSCREEN;
    float cornerRadius  = fsUnrounded ? 0.0f : window->rounding() * monitorScale;
    float roundingPower = window->roundingPower();

    // Resolved here, not inside the cache branch below, so it stays fresh on a
    // cache-hit frame too, when neither sampleBackground() nor blendOwnContent() run.
    m_lastSelfSample = selfSampleFor(ctx);
    if (m_lastSelfSample > 0.0f && !g_pGlobalState->selfSampleConfigured) {
        // hyprctl keyword emits no config.reloaded, and a setup without layer
        // surfaces has no other place that would notice self_sample turning on
        g_pGlobalState->selfSampleConfigured = true;
        BackgroundDamageObserver::refreshEnabled();
    }

    // The render alpha Hyprland hands decorations is activeInactive * fade.
    // Glass must follow fades (open/close, fullscreen, workspace moves) but
    // not the active/inactive dimming or opacity rules: those make the surface
    // more translucent — revealing more glass — and shouldn't wash out the
    // glass pane itself. Rebuild the fade-only alpha from its components.
    float glassAlpha = window->alphaTotalWithout(Desktop::View::WINDOW_ALPHA_ACTIVE);
    if (const auto workspace = window->m_workspace; workspace && !window->m_pinned)
        glassAlpha *= workspace->m_alpha->value();

    const MONITORID monitorId = monitor ? monitor->m_id : -1; // -1 mirrors Hyprland's own MONITOR_INVALID

    if (!wantsBackgroundResample(monitor, transformBox)) {
        // Background unchanged since the last real sample — reuse it, skip
        // the most expensive GPU work (blit + blur passes) entirely.
        Diagnostics::recordWindowCacheHit(monitorId);
        // what changed beside it since: copied into the sample where this frame
        // drew it fresh (still owed where it didn't)
        if (!m_edgeDirty.empty()) {
            std::erase_if(m_edgeDirty, [&](const CBox& rect) {
                CBox t = rect.copy().translate(-monitor->m_position).scale(monitor->m_scale).round();
                t      = WindowGeometry::applyMonitorTransform(t, monitor);
                // (off the screen there is nothing to copy, and no frame's damage reaches there)
                t = t.intersection(CBox{0.0, 0.0, source->m_size.x, source->m_size.y});
                if (t.empty())
                    return true;
                // the rim that mirrors it
                const float reach = GlassRenderer::SAMPLE_PADDING_PX / monitor->m_scale;
                const auto  glass = WindowGeometry::computePaddedGlobalBox(window, 0.0f);
                const CBox  rim   = glass ? rect.copy().expand(reach).intersection(*glass) : CBox{};
                if (!GlassRenderer::boxCovered(t, g_pHyprRenderer->m_renderData.damage)) {
                    // (the rim too: the frame that patches it must also put it on screen,
                    // and only the commit's own frame had it damaged)
                    g_pHyprRenderer->damageBox(rect);
                    if (!rim.empty())
                        g_pHyprRenderer->damageBox(rim);
                    return false;
                }
                if (!GlassRenderer::patchSample(m_sampleFramebuffer, source, transformBox, t)) {
                    markBackgroundDirty();
                    return true;
                }
                // the kept glass: that rim, computed again
                if (rim.empty())
                    ;
                else if (m_outDirty.size() < 16)
                    m_outDirty.push_back(rim);
                else
                    m_outValid = false;
                return true;
            });
        }
    } else {
        const bool covered = GlassRenderer::sampleRegionCovered(transformBox, source, g_pHyprRenderer->m_renderData.damage);

        if (covered) {
            float blurStrength   = resolvePresetFloat(ctx, &SPresetValues::blurStrength, &SOverridableConfig::blurStrength);
            int downscale        = GlassRenderer::sampleDownscale(blurStrength);
            // (a blurred sample can't take a sharp patch: see markEdgeDirty)
            m_samplePatchable    = blurStrength <= 0.0f;

            GlassRenderer::sampleBackground(m_sampleFramebuffer, source, transformBox, m_samplePaddingRatio, downscale);

            // Only here, on a real (non-cached) sample: blending onto a cache-hit
            // frame would double-composite our own content over an already-blurred FBO.
            if (m_lastSelfSample > 0.0f)
                GlassRenderer::blendOwnContent(m_sampleFramebuffer, window, monitor, transformBox, downscale,
                                               m_lastSelfSample, cornerRadius, roundingPower);

            float blurRadius     = blurStrength * 12.0f / downscale;
            int blurIterations   = std::clamp(static_cast<int>(resolvePresetInt(ctx, &SPresetValues::blurIterations, &SOverridableConfig::blurIterations)), 1, 5);

            if (ctx.config.blurFold && **ctx.config.blurFold) {
                const GlassRenderer::SFoldedBlur folded = GlassRenderer::foldBlurPasses(blurRadius, blurIterations);
                blurRadius     = folded.radius;
                blurIterations = folded.iterations;
            }

            GlassRenderer::blurBackground(m_sampleFramebuffer, blurRadius, blurIterations, source);
            m_frost.sampleChanged();

            m_hasCachedSample       = true;
            m_lastSceneGeneration   = g_pGlobalState->getSceneGeneration(monitor);
            m_lastGenerationMonitor = monitorId;
            m_backgroundDirty       = false;
            m_edgeDirty.clear();
            m_sampleSerial++;
            Diagnostics::recordWindowCacheMiss(monitorId);
        } else if (m_hasCachedSample) {
            // Not enough of the padded box is damaged yet to safely re-sample
            // (would pick up stale pixels outside this frame's damage).
            // Force it into next frame's damage and draw the stale cache for
            // now — a future pass scissors the draw to what's actually damaged.
            damageEntire();
            Diagnostics::recordWindowDeferredResample(monitorId);
        } else {
            // No cache yet and not enough damage to sample cleanly: nothing
            // valid to draw this frame.
            damageEntire();
            return;
        }
    }

    // frosted by how much of it lies over other windows (a floating window over
    // tiled ones), as frost.windows sets: every window, floating or tiled (it is
    // the Glass menu's Window glass; floating ones once took Menu glass, and the
    // Window glass setting did nothing to them)
    const auto& config = g_pGlobalState->config;
    const auto  frostKey     = config.frostWindows;
    const float frostSetting = frostKey ? static_cast<float>(**frostKey) : 0.0f;
    const float frostTarget  = frostSetting > 0.0f ? Frost::amountFor(frostSetting, Frost::coveredFraction(monitor, transformBox, window)) : 0.0f;
    if (m_frost.step(frostTarget))
        damageEntire();
    Frost::SFrost frost;
    m_frost.prepare(m_sampleFramebuffer, monitorScale, source, frost);

    // the active window's gleam: its rim catching a light that runs round its own
    // outline (steady along every edge, quick through the corners: motion tokens
    // gleamEdgeSpeed, gleamCornerTime; GlassRenderer walks the path), easing in and
    // out with focus (GLEAM_EASE). Only the rim strip redraws, every frame while it
    // shows; nothing while the session is locked or the window out of sight.
    // On gaining focus the whole rim catches a quick flash that settles back
    // (tokens focusFlash), so a switch reads at once.
    static constexpr float GLEAM_EASE = 0.6f;
    std::array<float, 4> gleam{0.0f, -1.0f, config.gleam ? std::clamp(static_cast<float>(**config.gleam), 0.0f, 2.0f) : 0.0f, 0.0f};
    if (gleam[2] > 0.0f) {
        const auto now    = std::chrono::steady_clock::now();
        // (while the window slides, a tile swap, focus follows the pointer across the
        // moving windows and changed several times a swap: the light held as it was
        // until the window lands, then eases to where focus ended up)
        const bool slidingNow = window->positionAnimation()->isBeingAnimated() || window->sizeAnimation()->isBeingAnimated();
        const bool active     = slidingNow ? m_wasActive : Desktop::focusState()->window() == window;
        if (active != m_wasActive) {
        if (slidingNow)
            m_slidAt = now;
            m_wasActive  = active;
            m_activeFrom = m_active;
            m_activeAt   = now;
            // (the flash only for a window that holds still: swapping tiles moves
            // another window under the pointer, focus follows it, and the whole rim
            // flashed white mid-slide)
            // (and no flash when it lands from a slide: just the light easing over)
            if (active && now - m_slidAt > std::chrono::milliseconds(250))
                m_pulseAt = now;
            else if (active)
                m_pulseAt = now - std::chrono::seconds(10); // (the sweep's clock as if long done)
        }
        // (the motion library's focusFlash spring kicked from rest: a critically
        // damped spring from 0 to 0 with a starting speed rises and settles back,
        // up at 1/omega; the kick sized so its peak is 1)
        float pulse = 0.0f;
        if (active && !GlassRenderer::reducedMotion()) {
            static const motion::Spring flash = motion::tokens::focusFlash();
            const double kick = flash.omega0() * std::exp(1.0);
            const motion::SpringMotion m(flash, 0.0, 0.0, kick, 0.0);
            const double since = std::chrono::duration<double>(now - m_pulseAt).count();
            pulse = since < m.settleTime(0.002) ? static_cast<float>(std::max(m.value(since), 0.0)) : 0.0f;
        }
        gleam[3] = pulse;
        const float ease = std::min(std::chrono::duration<float>(now - m_activeAt).count() / GLEAM_EASE, 1.0f);
        m_active = m_activeFrom + ((active ? 1.0f : 0.0f) - m_activeFrom) * (ease * ease * (3.0f - 2.0f * ease));
        // (its clock in s, wrapped well past any lap: the renderer walks the outline)
        static const auto epoch = now;
        gleam[0] = m_active;
        const int style = GlassRenderer::gleamStyle();
        gleam[1] = static_cast<float>(std::fmod(std::chrono::duration<double>(now - epoch).count(), 3600.0));
        const bool seen = !(g_pSessionLockManager && g_pSessionLockManager->isSessionLocked()) &&
                          window->m_workspace && window->m_workspace->isVisible() && !window->isHidden();
        if (seen && pulse > 0.002f) {
            // (the flash runs at the display's rate: it is quick)
            m_gleamTicked = true;
            damageRim();
        } else if (m_active > 0.001f && seen && (style >= 1 && style <= 3 || style == 5 || style == 6 ||
                                                  (m_active > 0.001f && m_active < 0.999f))) {
            // (a steady light, the outline or a finished sweep, redraws nothing once settled)
            // (every frame: at its speed a 30 fps light stepped ~35 px at a time)
            m_gleamDamageAt = now;
            m_gleamTicked   = true;
            damageRim();
        }
    }

    const std::array<float, 4> radii{cornerRadius, cornerRadius, cornerRadius, cornerRadius};

    // Kept glass: only for a window at rest (fully faded in, not pressed, its frost
    // and its gleam's focus ease settled); anything moving is computed every frame.
    // (gleam[0], not m_active: with the gleam off m_active is no longer stepped and
    // could stay frozen mid-ease, which kept the window off its cache for good)
    const bool restful = glassAlpha >= 0.999f && g_pGlobalState->pressGlowAge() < 0.0f && gleam[3] <= 0.002f &&
                         (gleam[0] <= 0.001f || gleam[0] >= 0.999f) && !(config.debugNoOutputCache && **config.debugNoOutputCache);
    if (!restful) {
        m_outValid = false;
        m_outDirty.clear();
        m_gleamTicked = false;
        GlassRenderer::applyGlassEffect(m_sampleFramebuffer, source, windowBox, transformBox, glassAlpha, radii, roundingPower,
                                         m_samplePaddingRatio, ctx, nullptr, &frost, &gleam);
        return;
    }
    // what it was computed for (a resample changes the scene generation)
    const std::array<double, 13> key{static_cast<double>(g_pGlobalState->configSerial), transformBox.w, transformBox.h, cornerRadius, roundingPower, frost.amount,
                                     static_cast<double>(g_pGlobalState->getSceneGeneration(monitor)), static_cast<double>(m_sampleSerial),
                                     static_cast<double>(monitorId), monitor->m_scale, static_cast<double>(monitor->m_transform),
                                     isDark ? 1.0 : 0.0, static_cast<double>(std::hash<std::string>{}(preset) & 0xffffff)};
    // (changing every frame, a drag or a slide: computed straight to the screen, as
    // keeping it would cost a copy on top; kept once it holds still for a frame)
    const bool steady = key == m_lastKey;
    m_lastKey = key;
    if (!steady && !(m_outValid && key == m_outKey)) {
        m_outValid = false;
        m_outDirty.clear();
        m_gleamTicked = false;
        GlassRenderer::applyGlassEffect(m_sampleFramebuffer, source, windowBox, transformBox, glassAlpha, radii, roundingPower,
                                         m_samplePaddingRatio, ctx, nullptr, &frost, &gleam);
        return;
    }
    GlassRenderer::SOutputCache into;
    bool compute = false;
    if (!m_outFB)
        m_outFB = g_pHyprRenderer->createFB("hyprglass-output");
    if (m_outFB->m_size.x != transformBox.w || m_outFB->m_size.y != transformBox.h || m_outFB->m_drmFormat != source->m_drmFormat) {
        m_outFB->alloc(static_cast<int>(transformBox.w), static_cast<int>(transformBox.h), source->m_drmFormat);
        m_outValid     = false;
        m_outFilterTex = 0;
    }
    if (!m_outValid || key != m_outKey) {
        into.clear = true; // (all of it)
        compute    = true;
    } else {
        // only what changed: the rim facing a change beside it, the rim the light moved on
        auto addLocal = [&](CBox g) {
            g.translate(-monitor->m_position).scale(monitor->m_scale).round();
            g = WindowGeometry::applyMonitorTransform(g, monitor).translate(-transformBox.pos()).expand(2.0);
            g = g.intersection(CBox{0.0, 0.0, transformBox.w, transformBox.h});
            if (!g.empty())
                into.rects.push_back(g);
        };
        for (const auto& g : m_outDirty)
            addLocal(g);
        if (m_gleamTicked && gleam[0] > 0.001f) {
            if (const auto box = WindowGeometry::computePaddedGlobalBox(window, 0.0f)) {
                const double w = std::min(16.0, std::min(box->w, box->h) / 2.0);
                addLocal(CBox{box->x, box->y, box->w, w});
                addLocal(CBox{box->x, box->y + box->h - w, box->w, w});
                addLocal(CBox{box->x, box->y + w, w, box->h - 2 * w});
                addLocal(CBox{box->x + box->w - w, box->y + w, w, box->h - 2 * w});
            }
        }
        compute = !into.rects.empty();
    }
    m_outDirty.clear();
    m_gleamTicked = false;
    if (compute) {
        into.fb = m_outFB;
        GlassRenderer::applyGlassEffect(m_sampleFramebuffer, source, windowBox, transformBox, glassAlpha, radii, roundingPower,
                                         m_samplePaddingRatio, ctx, nullptr, &frost, &gleam, &into);
        m_outKey   = key;
        m_outValid = true;
    }
    GlassRenderer::drawOutputCache(m_outFB, source, windowBox, m_outFilterTex);
}

eDecorationType CGlassDecoration::getDecorationType() {
    return DECORATION_CUSTOM;
}

// Driven by the real position/size variables and by map/layout/rule changes,
// unlike draw(), which sees whatever geometry the caller substituted. The
// PHLWINDOW argument is ignored on purpose: a third-party replay passes its own
// handle, while our state is keyed on the owner we were constructed with.
void CGlassDecoration::updateWindow(PHLWINDOW) {
    // A plugin may replay this mid-render under substituted geometry. mainFB is
    // set exactly between begin() and end(); currentFB also follows binds taken
    // outside a pass. Above the m_last* store, so the next real update bumps.
    if (g_pHyprRenderer->m_renderData.mainFB)
        return;

    damageEntire();

    if (!g_pGlobalState || resolveEnabled() != EEnabledResolution::Enabled)
        return;

    const auto ownWindow = m_window.lock();
    if (!ownWindow)
        return;

    const auto monitor = ownWindow->m_monitor.lock();
    if (!monitor)
        return;

    const auto currentPosition = ownWindow->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
    const auto currentSize     = ownWindow->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
    if (currentPosition == m_lastPosition && currentSize == m_lastSize)
        return;

    m_lastPosition = currentPosition;
    m_lastSize     = currentSize;

    // A workspace the monitor is not rendering changes nothing behind a glassed
    // layer; switching to it bumps on its own. Same predicate Hyprland renders
    // by: a slide or fade keeps drawing an already-invisible workspace.
    const auto workspace = ownWindow->m_workspace;
    if (workspace && !workspace->m_visible && !workspace->m_forceRendering && !workspace->m_renderOffset->isBeingAnimated() &&
        !workspace->m_alpha->isBeingAnimated() && !ownWindow->m_pinned)
        return;

    g_pGlobalState->bumpSceneGeneration(monitor);
}

void CGlassDecoration::damageRim() {
    const auto window = m_window.lock();
    if (!window)
        return;
    const auto box = WindowGeometry::computePaddedGlobalBox(window, 2.0f);
    if (!box)
        return;
    // (the gleam lives in the outer ~12 logical px of the glass)
    const double w = std::min(16.0, std::min(box->w, box->h) / 2.0);
    g_pHyprRenderer->damageBox(CBox{box->x, box->y, box->w, w});
    g_pHyprRenderer->damageBox(CBox{box->x, box->y + box->h - w, box->w, w});
    g_pHyprRenderer->damageBox(CBox{box->x, box->y, w, box->h});
    g_pHyprRenderer->damageBox(CBox{box->x + box->w - w, box->y, w, box->h});
}

void CGlassDecoration::damageEntire() {
    const auto window = m_window.lock();
    if (!window)
        return;

    // Padded so the render pass re-renders background content (wallpaper,
    // other windows) in the sampling margin too. Without this, the scissored
    // render pass leaves stale previous-frame content in the padding area,
    // causing noise artifacts.
    const auto box = WindowGeometry::computePaddedGlobalBox(window, GlassRenderer::SAMPLE_PADDING_PX);
    if (!box)
        return;

    g_pHyprRenderer->damageBox(*box);
}

eDecorationLayer CGlassDecoration::getDecorationLayer() {
    return DECORATION_LAYER_BOTTOM;
}

uint64_t CGlassDecoration::getDecorationFlags() {
    return DECORATION_NON_SOLID;
}

std::string CGlassDecoration::getDisplayName() {
    return "HyprGlass";
}
