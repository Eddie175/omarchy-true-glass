#include <ctime>
#include "GlassRenderer.hpp"
#include <hyprland/src/managers/input/InputManager.hpp>
#include "motion/motion.hpp"
#include "BuiltInPresets.hpp"
#include "Diagnostics.hpp"
#include "Globals.hpp"
#include "WindowGeometry.hpp"

#include <algorithm>
#include <vector>
#include <array>
#include <optional>
#include <GLES3/gl32.h>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

namespace GlassRenderer {

static GLuint fbId(const SP<Render::IFramebuffer>& framebuffer) {
    return dynamic_cast<Render::GL::CGLFramebuffer*>(framebuffer.get())->getFBID();
}

static void uploadThemeUniforms(const SResolveContext& ctx) {
    const auto& uniforms = g_pGlobalState->shaderManager.glassUniforms;
    const auto& glassShader = g_pGlobalState->shaderManager.glassShader;
    const auto& defaults = ctx.isDark ? DARK_THEME_DEFAULTS : LIGHT_THEME_DEFAULTS;

    glassShader->setUniformFloat(SHADER_BRIGHTNESS, resolvePresetFloat(ctx, &SPresetValues::brightness, &SOverridableConfig::brightness, defaults.brightness));
    glassShader->setUniformFloat(SHADER_CONTRAST,   resolvePresetFloat(ctx, &SPresetValues::contrast, &SOverridableConfig::contrast, defaults.contrast));
    glUniform1f(uniforms.saturation,                 resolvePresetFloat(ctx, &SPresetValues::saturation, &SOverridableConfig::saturation, defaults.saturation));
    glassShader->setUniformFloat(SHADER_VIBRANCY,   resolvePresetFloat(ctx, &SPresetValues::vibrancy, &SOverridableConfig::vibrancy, defaults.vibrancy));
    glUniform1f(uniforms.vibrancyDarkness,           resolvePresetFloat(ctx, &SPresetValues::vibrancyDarkness, &SOverridableConfig::vibrancyDarkness, defaults.vibrancyDarkness));

    glUniform1f(uniforms.adaptiveDim,   resolvePresetFloat(ctx, &SPresetValues::adaptiveDim, &SOverridableConfig::adaptiveDim, defaults.adaptiveDim));
    glUniform1f(uniforms.adaptiveBoost, resolvePresetFloat(ctx, &SPresetValues::adaptiveBoost, &SOverridableConfig::adaptiveBoost, defaults.adaptiveBoost));
}

CBox SSampleMap::toSample(const CBox& framebufferBox) const {
    return CBox{(framebufferBox.x - srcX0) * scaleX, (framebufferBox.y - srcY0) * scaleY,
                framebufferBox.width * scaleX, framebufferBox.height * scaleY};
}

SSampleMap sampleMapFor(const CBox& box, int downscale) {
    SSampleMap map;
    map.fullWidth  = static_cast<int>(box.width) + 2 * SAMPLE_PADDING_PX;
    map.fullHeight = static_cast<int>(box.height) + 2 * SAMPLE_PADDING_PX;

    // Reduced resolution when blur is strong enough to hide it.
    // Weak blur at half-res shows pixelation.
    map.width  = std::max(1, map.fullWidth / downscale);
    map.height = std::max(1, map.fullHeight / downscale);

    map.srcX0  = static_cast<int>(box.x) - SAMPLE_PADDING_PX;
    map.srcY0  = static_cast<int>(box.y) - SAMPLE_PADDING_PX;
    map.srcX1  = static_cast<int>(box.x + box.width) + SAMPLE_PADDING_PX;
    map.srcY1  = static_cast<int>(box.y + box.height) + SAMPLE_PADDING_PX;
    map.scaleX = static_cast<float>(map.width) / map.fullWidth;
    map.scaleY = static_cast<float>(map.height) / map.fullHeight;
    return map;
}

bool sampleRegionCovered(const CBox& box, const SP<Render::IFramebuffer>& source, const CRegion& damage) {
    if (!source)
        return false;

    // The exact source rect the blit reads, clipped like the blit itself: Hyprland
    // clips element damage to the monitor, and the clipped-off padding is cleared.
    const auto   map = sampleMapFor(box, 1);
    const double x1  = std::max(map.srcX0, 0);
    const double y1  = std::max(map.srcY0, 0);
    const double x2  = std::min(map.srcX1, static_cast<int>(source->m_size.x));
    const double y2  = std::min(map.srcY1, static_cast<int>(source->m_size.y));
    if (x2 <= x1 || y2 <= y1)
        return true;

    return boxCovered(CBox{x1, y1, x2 - x1, y2 - y1}, damage);
}

bool boxCovered(const CBox& box, const CRegion& damage) {
    // `box` is in transformed framebuffer pixels while the frame's damage is in
    // untransformed monitor pixels: on a rotated monitor the two never matched,
    // so every resample was deferred and its glass was never drawn.
    CRegion damageInBoxSpace = damage.copy();
    if (const auto monitor = g_pHyprRenderer->m_renderData.pMonitor.lock(); monitor && monitor->m_transform != WL_OUTPUT_TRANSFORM_NORMAL)
        damageInBoxSpace.transform(Math::wlTransformToHyprutils(Math::invertTransform(monitor->m_transform)),
                                   monitor->m_transformedSize.x, monitor->m_transformedSize.y);
    return CRegion(box).subtract(damageInBoxSpace).empty();
}

SFoldedBlur foldBlurPasses(float radius, int iterations) noexcept {
    const float totalRadius      = radius * std::sqrt(static_cast<float>(iterations));
    const float cappedRatio      = totalRadius / BLUR_SHADER_TAP_CAP;
    const int   foldedIterations = std::max(1, static_cast<int>(std::ceil(cappedRatio * cappedRatio)));

    if (foldedIterations >= iterations)
        return {radius, iterations};

    return {totalRadius / std::sqrt(static_cast<float>(foldedIterations)), foldedIterations};
}

int sampleDownscale(float blurStrength) {
    const auto& c = g_pGlobalState->config;
    if (c.lowPower && **c.lowPower > 0.5f)
        return BLUR_DOWNSCALE_MAX;
    return blurStrength >= BLUR_DOWNSCALE_THRESHOLD ? BLUR_DOWNSCALE_MAX : 1;
}

void sampleBackground(SP<Render::IFramebuffer>& sampleFramebuffer, SP<Render::IFramebuffer> sourceFramebuffer,
                       CBox box, Vector2D& outPaddingRatio, int downscale) {
    if (!sourceFramebuffer)
        return;

    Diagnostics::CScopedStageTimer stageTimer(Diagnostics::EStage::SampleBackground);

    const int  pad = SAMPLE_PADDING_PX;
    const auto map = sampleMapFor(box, downscale);

    int fullWidth  = map.fullWidth;
    int fullHeight = map.fullHeight;

    // Full-res source pixels this call blits, before any downscale — what the
    // GPU actually reads off the source framebuffer, not the (possibly
    // half-res) destination the sample FBO ends up holding.
    if (const auto monitor = g_pHyprRenderer->m_renderData.pMonitor.lock())
        Diagnostics::recordSampledPixels(monitor->m_id, static_cast<double>(fullWidth) * static_cast<double>(fullHeight));

    int sampleWidth  = map.width;
    int sampleHeight = map.height;

    if (!sampleFramebuffer)
        sampleFramebuffer = g_pHyprRenderer->createFB("hyprglass-sample");

    // the format follows the monitor framebuffer, which changes with cm/bitdepth
    if (sampleFramebuffer->m_size.x != sampleWidth || sampleFramebuffer->m_size.y != sampleHeight ||
        sampleFramebuffer->m_drmFormat != sourceFramebuffer->m_drmFormat)
        sampleFramebuffer->alloc(sampleWidth, sampleHeight, sourceFramebuffer->m_drmFormat);

    int srcX0 = map.srcX0;
    int srcX1 = map.srcX1;
    int srcY0 = map.srcY0;
    int srcY1 = map.srcY1;

    // Clamp source coordinates to framebuffer bounds to avoid reading black/undefined pixels
    int framebufferWidth  = static_cast<int>(sourceFramebuffer->m_size.x);
    int framebufferHeight = static_cast<int>(sourceFramebuffer->m_size.y);

    // Destination coords in downscaled FBO space
    int dstX0 = 0, dstY0 = 0, dstX1 = sampleWidth, dstY1 = sampleHeight;

    // Scale destination adjustments proportionally for the downscaled FBO
    const float xScale = map.scaleX;
    const float yScale = map.scaleY;

    // Tracks whether any clamp shrank the destination rect below the full FBO,
    // which is the only case that can leave uninitialized texels after the blit.
    bool destinationClamped = false;

    if (srcX0 < 0) { dstX0 += static_cast<int>(-srcX0 * xScale); srcX0 = 0; destinationClamped = true; }
    if (srcY0 < 0) { dstY0 += static_cast<int>(-srcY0 * yScale); srcY0 = 0; destinationClamped = true; }
    if (srcX1 > framebufferWidth)  { dstX1 -= static_cast<int>((srcX1 - framebufferWidth) * xScale);  srcX1 = framebufferWidth; destinationClamped = true; }
    if (srcY1 > framebufferHeight) { dstY1 -= static_cast<int>((srcY1 - framebufferHeight) * yScale); srcY1 = framebufferHeight; destinationClamped = true; }

    // Padding ratio is relative to the logical content area (resolution-independent)
    outPaddingRatio = Vector2D(
        static_cast<double>(pad) / fullWidth,
        static_cast<double>(pad) / fullHeight
    );

    // The render pass scissors each element to its damage region.
    // That scissor state leaks here and clips glBlitFramebuffer on the
    // DRAW framebuffer, causing partial writes and stale noise artifacts.
    g_pHyprOpenGL->setCapStatus(GL_SCISSOR_TEST, false);
    // the tracker skips glDisable when it believes the test is already off
    if (glIsEnabled(GL_SCISSOR_TEST)) {
        glDisable(GL_SCISSOR_TEST);
        Diagnostics::recordStateDesync("scissor on before the background blit");
    }

    // Clear the sample FBO before blitting only when the blit destination
    // doesn't cover the whole FBO. Clamped regions (near monitor edges)
    // would otherwise leave uninitialized GPU memory (pink artifacts) outside
    // the blit; a full-rect blit overwrites every texel, so the clear is redundant.
    if (destinationClamped) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbId(sampleFramebuffer));
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbId(sourceFramebuffer));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbId(sampleFramebuffer));
    glBlitFramebuffer(srcX0, srcY0, srcX1, srcY1,
                      dstX0, dstY0, dstX1, dstY1,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
}

bool patchSample(SP<Render::IFramebuffer>& sampleFramebuffer, SP<Render::IFramebuffer> sourceFramebuffer, const CBox& box, const CBox& patch) {
    if (!sampleFramebuffer || !sourceFramebuffer)
        return false;
    const auto map = sampleMapFor(box, 1);
    if (sampleFramebuffer->m_size.x != map.width || sampleFramebuffer->m_size.y != map.height ||
        sampleFramebuffer->m_drmFormat != sourceFramebuffer->m_drmFormat)
        return false;

    // the patch, clipped to what the sample reads and to the framebuffer
    const int x0 = std::max({static_cast<int>(std::floor(patch.x)), map.srcX0, 0});
    const int y0 = std::max({static_cast<int>(std::floor(patch.y)), map.srcY0, 0});
    const int x1 = std::min({static_cast<int>(std::ceil(patch.x + patch.w)), map.srcX1, static_cast<int>(sourceFramebuffer->m_size.x)});
    const int y1 = std::min({static_cast<int>(std::ceil(patch.y + patch.h)), map.srcY1, static_cast<int>(sourceFramebuffer->m_size.y)});
    if (x1 <= x0 || y1 <= y0)
        return true;

    if (const auto monitor = g_pHyprRenderer->m_renderData.pMonitor.lock())
        Diagnostics::recordSampledPixels(monitor->m_id, static_cast<double>(x1 - x0) * static_cast<double>(y1 - y0));

    // (as sampleBackground: the pass's scissor would clip the blit)
    g_pHyprOpenGL->setCapStatus(GL_SCISSOR_TEST, false);
    if (glIsEnabled(GL_SCISSOR_TEST))
        glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbId(sourceFramebuffer));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbId(sampleFramebuffer));
    glBlitFramebuffer(x0, y0, x1, y1, x0 - map.srcX0, y0 - map.srcY0, x1 - map.srcX0, y1 - map.srcY0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    return true;
}

void blendOwnContent(SP<Render::IFramebuffer>& sampleFramebuffer, PHLWINDOW window, PHLMONITOR monitor,
                      const CBox& box, int downscale, float amount, float cornerRadius, float roundingPower) {
    if (amount <= 0.0f || !sampleFramebuffer || !window || !monitor)
        return;

    // The sample framebuffer holds already-rotated framebuffer-space pixels, while a
    // texture drawn under RPT_EXPORT lands axis-aligned: on a 90/270 output the self
    // image would come out rotated. Skipping keeps those outputs at today's look.
    if (monitor->m_transform != WL_OUTPUT_TRANSFORM_NORMAL)
        return;

    // Hyprland renders a transformed window through a redirected pass and blits the
    // result; our untransformed copy would not match what the user sees.
    if (!window->m_transformers.empty())
        return;

    const auto hlSurface = window->wlSurface();
    const auto root      = hlSurface ? hlSurface->resource() : nullptr;
    if (!root)
        return;

    const auto     map    = sampleMapFor(box, downscale);
    const Vector2D fbSize = sampleFramebuffer->m_size;

    // The map only describes this framebuffer if sampleBackground() sized it from the
    // same box; otherwise boxes would project against one size and rasterise into another.
    if (fbSize.x != map.width || fbSize.y != map.height)
        return;

    auto& renderData = g_pHyprRenderer->m_renderData;

    // Leave the caller the state sampleBackground leaves: scissor off, viewport from
    // the re-bound framebuffer's own size (monitor sizes are wrong here, #41).
    // Declared before the FB guard so it runs after that framebuffer is back, on the
    // throwing path too.
    const Hyprutils::Utils::CScopeGuard restoreGLState([&] {
        g_pHyprOpenGL->scissor(nullptr);
        if (const auto& restored = renderData.currentFB)
            g_pHyprOpenGL->setViewport(0, 0, static_cast<int>(restored->m_size.x), static_cast<int>(restored->m_size.y));
    });

    auto guard = g_pHyprRenderer->bindTempFB(sampleFramebuffer);

    const auto  savedProjection      = renderData.projectionType;
    const auto  savedFbSize          = renderData.fbSize;
    const auto  savedRenderModif     = renderData.renderModif;
    const auto  savedWindow          = renderData.currentWindow;
    const auto  savedSurface         = renderData.surface;
    const auto  savedClipBox         = renderData.clipBox;
    const auto  savedUVTopLeft       = renderData.primarySurfaceUVTopLeft;
    const auto  savedUVBottomRight   = renderData.primarySurfaceUVBottomRight;
    const bool  savedTransformDamage = renderData.transformDamage;

    // draw() allocates a pass element per surface, so the restores must survive a
    // throw: every later element would otherwise project through the sample size.
    // Only render data, no GL state, so running after the FB rebind below is safe.
    const Hyprutils::Utils::CScopeGuard restoreRenderData([&] {
        renderData.primarySurfaceUVBottomRight = savedUVBottomRight;
        renderData.primarySurfaceUVTopLeft     = savedUVTopLeft;
        renderData.clipBox                     = savedClipBox;
        renderData.surface                     = savedSurface;
        renderData.currentWindow               = savedWindow;
        renderData.renderModif                 = savedRenderModif;
        renderData.transformDamage             = savedTransformDamage;
        // before setProjectionType: RPT_FB recomputes the projection from fbSize
        renderData.fbSize = savedFbSize;
        g_pHyprRenderer->setProjectionType(savedProjection);
    });

    renderData.fbSize = fbSize;
    g_pHyprRenderer->setProjectionType(Render::RPT_EXPORT);
    // our boxes and damage rects are sample-framebuffer pixels, not monitor space
    renderData.transformDamage = false;
    // an overview plugin's modifier would otherwise be applied a second time
    renderData.renderModif = {};
    // a monitor-space clip would scissor the injection to the wrong rectangle
    renderData.clipBox = {};
    // Only for the rgbx shader variant, the one thing the texture draw cannot be told
    // any other way. It also lets dim_inactive and the not-responding tint into the
    // self image of those windows: accepted, there is no allowDim on this draw path.
    if (window->m_ruleApplicator && window->m_ruleApplicator->RGBX().valueOrDefault())
        renderData.currentWindow = window;

    g_pHyprOpenGL->setViewport(0, 0, static_cast<int>(fbSize.x), static_cast<int>(fbSize.y));
    // Premultiplied source-over. Not restored afterwards: m_blend is private, and every
    // element boundary already leaves blending on with this same function.
    g_pHyprRenderer->blend(true);

    // Must be non-empty: renderTextureInternal drops empty damage, and an empty
    // region here makes Hyprland substitute its own, which is in monitor space.
    const CRegion fullDamage = CBox{0, 0, fbSize.x, fbSize.y};

    root->breadthfirst(
        [&](SP<CWLSurfaceResource> surface, const Vector2D& offset, void*) {
            const auto texture = surface->m_current.texture;
            // renderTextureInternal asserts on both, and an assert kills the compositor
            if (!texture || !texture->ok())
                return;
            if (surface->m_current.size.x < 1 || surface->m_current.size.y < 1)
                return;

            const bool mainSurface = surface == root;

            CBox       framebufferBox = box;
            if (!mainSurface) {
                framebufferBox = CBox{box.x + offset.x * monitor->m_scale, box.y + offset.y * monitor->m_scale,
                                      surface->m_current.size.x * monitor->m_scale, surface->m_current.size.y * monitor->m_scale};
                framebufferBox.round();
            }

            // Viewporter source crop, the one surface-state correction video players
            // need. Small and misaligned surfaces keep their uncorrected placement.
            // Each surface also blends at the same alpha, so where a subsurface covers
            // the main one the desktop share is (1-amount)^2: visible as a ghost of the
            // main surface at mid values, gone at 1.0. Fixing it needs a scratch target.
            const auto& viewport   = surface->m_current.viewport;
            const auto& bufferSize = surface->m_current.bufferSize;
            bool        customUV   = false;
            if (viewport.hasSource && bufferSize.x > 0 && bufferSize.y > 0) {
                const Vector2D uvTopLeft{viewport.source.x / bufferSize.x, viewport.source.y / bufferSize.y};
                const Vector2D uvBottomRight{(viewport.source.x + viewport.source.width) / bufferSize.x,
                                             (viewport.source.y + viewport.source.height) / bufferSize.y};
                if (uvBottomRight.x > 0.00001 && uvBottomRight.y > 0.00001) {
                    renderData.primarySurfaceUVTopLeft     = uvTopLeft;
                    renderData.primarySurfaceUVBottomRight = uvBottomRight;
                    customUV                               = true;
                }
            }
            if (!customUV) {
                renderData.primarySurfaceUVTopLeft     = Vector2D(-1, -1);
                renderData.primarySurfaceUVBottomRight = Vector2D(-1, -1);
            }

            g_pHyprRenderer->draw(CTexPassElement::SRenderData{
                .tex           = texture,
                .box           = map.toSample(framebufferBox),
                .a             = amount,
                .damage        = fullDamage,
                .round         = mainSurface ? static_cast<int>(cornerRadius * map.scaleX) : 0,
                .roundingPower = roundingPower,
                .allowCustomUV = customUV,
                .surface       = surface,
            });

            // The texture path tracks no buffer of its own, and the window's real draw
            // may be discarded as occluded, releasing the buffer we just read.
            if (surface->m_current.buffer && !surface->m_current.buffer->isSynchronous())
                g_pHyprRenderer->m_usedAsyncBuffers.emplace_back(surface->m_current.buffer);
        },
        nullptr);

    guard.reset();
}

void blurBackground(SP<Render::IFramebuffer> sampleFramebuffer, float radius, int iterations,
                    SP<Render::IFramebuffer> callerFramebuffer) {
    auto& shaderManager = g_pGlobalState->shaderManager;
    if (!sampleFramebuffer || !callerFramebuffer || radius <= 0.0f || iterations <= 0 || !shaderManager.isInitialized())
        return;

    Diagnostics::CScopedStageTimer stageTimer(Diagnostics::EStage::BlurBackground);

    // Two ping-pong passes (horizontal, vertical) per iteration.
    if (const auto monitor = g_pHyprRenderer->m_renderData.pMonitor.lock())
        Diagnostics::recordBlurPasses(monitor->m_id, static_cast<uint64_t>(iterations) * 2);

    int width  = static_cast<int>(sampleFramebuffer->m_size.x);
    int height = static_cast<int>(sampleFramebuffer->m_size.y);

    // A temp buffer per size: one shared buffer was reallocated whenever two
    // glass surfaces of different sizes blurred in the same frame (a frosted
    // card and a frosted window: 140 us of driver work per blur, every resample).
    auto&     temps    = g_pGlobalState->blurTemps;
    uint64_t& useClock = g_pGlobalState->blurTempClock;
    auto*     pick     = static_cast<SGlobalState::SBlurTemp*>(nullptr);
    for (auto& t : temps)
        if (t.fb && t.fb->m_size.x == width && t.fb->m_size.y == height && t.fb->m_drmFormat == sampleFramebuffer->m_drmFormat) {
            pick = &t;
            break;
        }
    if (!pick) {
        pick = &*std::min_element(temps.begin(), temps.end(), [](const auto& a, const auto& b) { return a.used < b.used; });
        if (!pick->fb)
            pick->fb = g_pHyprRenderer->createFB("hyprglass-blur-temp");
        pick->fb->alloc(width, height, sampleFramebuffer->m_drmFormat);
    }
    pick->used = ++useClock;
    auto& blurTempFramebuffer = pick->fb;

    // Fullscreen quad projection: maps VAO positions [0,1] to clip space [-1,1]
    static constexpr std::array<float, 9> FULLSCREEN_PROJECTION = {
        2.0f, 0.0f, 0.0f,
        0.0f, 2.0f, 0.0f,
       -1.0f,-1.0f, 1.0f,
    };

    const auto& blurUniforms = shaderManager.blurUniforms;

    // Each pass overwrites its whole target: blending would mix in the temp FBO's
    // previous contents, a leaked scissor or stencil would leave texels unwritten.
    g_pHyprRenderer->blend(false);
    if (glIsEnabled(GL_SCISSOR_TEST)) {
        glDisable(GL_SCISSOR_TEST);
        Diagnostics::recordStateDesync("scissor on before the blur passes");
    }
    if (glIsEnabled(GL_STENCIL_TEST)) {
        glDisable(GL_STENCIL_TEST);
        Diagnostics::recordStateDesync("stencil on before the blur passes");
    }

    auto shader = g_pHyprOpenGL->useShader(shaderManager.blurShader);
    shader->setUniformMatrix3fv(SHADER_PROJ, 1, GL_FALSE, FULLSCREEN_PROJECTION);
    shader->setUniformInt(SHADER_TEX, 0);
    glUniform1f(blurUniforms.radius, radius);
    glBindVertexArray(shader->getUniformLocation(SHADER_SHADER_VAO));
    g_pHyprOpenGL->setViewport(0, 0, width, height);
    glActiveTexture(GL_TEXTURE0);

    // Ping-pong at full resolution: sampleFramebuffer ↔ blurTempFramebuffer
    for (int iteration = 0; iteration < iterations; iteration++) {
        // Horizontal pass: sampleFramebuffer → blurTempFramebuffer
        glBindFramebuffer(GL_FRAMEBUFFER, fbId(blurTempFramebuffer));
        sampleFramebuffer->getTexture()->bind();
        glUniform2f(blurUniforms.direction, 1.0f / width, 0.0f);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        // Vertical pass: blurTempFramebuffer → sampleFramebuffer
        glBindFramebuffer(GL_FRAMEBUFFER, fbId(sampleFramebuffer));
        blurTempFramebuffer->getTexture()->bind();
        glUniform2f(blurUniforms.direction, 0.0f, 1.0f / height);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    // Restore caller's GL state without querying (avoids pipeline stalls).
    // The viewport must match the re-bound framebuffer's own size: monitor
    // sizes are wrong here on 90°/270° monitors, where m_transformedSize is
    // swapped relative to the framebuffer's native orientation (#41).
    g_pHyprRenderer->blend(true); // Hyprland's state at every element boundary
    glBindFramebuffer(GL_FRAMEBUFFER, fbId(callerFramebuffer));
    glBindVertexArray(0);
    g_pHyprOpenGL->setViewport(0, 0,
        static_cast<int>(callerFramebuffer->m_size.x),
        static_cast<int>(callerFramebuffer->m_size.y));
}

// Orientation of the screen -> uv rotation per quarter turn (found on a rotated monitor)
static constexpr float SCREEN_ROT_SIGN_C = 1.0f;
static constexpr float SCREEN_ROT_SIGN_S = -1.0f;

// The glass program's uniform values persist between draws: most are the same
// for every draw of a preset, so a value already set is not sent again (about
// 70 driver calls a draw before, mostly repeats).
namespace {
    struct SUniformShadow {
        GLuint                            program = 0;
        uint64_t                          serial  = 0;
        std::array<std::array<float, 4>, 256> v{};
        std::array<bool, 256>             set{};
    } g_uniformShadow;

    void shadowUniformsFor(GLuint program, uint64_t serial) {
        if (g_uniformShadow.program != program || g_uniformShadow.serial != serial) {
            g_uniformShadow.program = program;
            g_uniformShadow.serial  = serial;
            g_uniformShadow.set.fill(false);
        }
    }
    bool shadowSame(GLint loc, float a, float b, float c, float d) {
        if (loc < 0)
            return true; // (not in the program: GL would ignore it too)
        if (loc >= 256)
            return false;
        auto& v = g_uniformShadow.v[loc];
        if (g_uniformShadow.set[loc] && v[0] == a && v[1] == b && v[2] == c && v[3] == d)
            return true;
        v = {a, b, c, d};
        g_uniformShadow.set[loc] = true;
        return false;
    }
    void shadowUniform1f(GLint loc, float a) {
        if (!shadowSame(loc, a, 0, 0, 0))
            glUniform1f(loc, a);
    }
    void shadowUniform2f(GLint loc, float a, float b) {
        if (!shadowSame(loc, a, b, 0, 0))
            glUniform2f(loc, a, b);
    }
    void shadowUniform3f(GLint loc, float a, float b, float c) {
        if (!shadowSame(loc, a, b, c, 0))
            glUniform3f(loc, a, b, c);
    }
    void shadowUniform4f(GLint loc, float a, float b, float c, float d) {
        if (!shadowSame(loc, a, b, c, d))
            glUniform4f(loc, a, b, c, d);
    }
    void shadowUniform1i(GLint loc, int a) {
        // (ints kept apart from floats of the same bits: a NaN pattern never matches)
        if (!shadowSame(loc, static_cast<float>(a), 1.5f, 0, 0))
            glUniform1i(loc, a);
    }
}

void applyGlassEffect(SP<Render::IFramebuffer> sampleFramebuffer, SP<Render::IFramebuffer> targetFramebuffer,
                       CBox& rawBox, CBox& transformedBox,
                       float alpha, const std::array<float, 4>& radii, float roundingPower,
                       const Vector2D& paddingRatio, const SResolveContext& resolveContext,
                       const SMaskInfo* mask, const Frost::SFrost* frost, const std::array<float, 4>* gleam,
                       const SOutputCache* intoCache) {
    if (!sampleFramebuffer || !targetFramebuffer || (intoCache && !intoCache->fb))
        return;

    Diagnostics::CScopedStageTimer stageTimer(Diagnostics::EStage::ApplyGlassEffect);

    // (counted where it is drawn: only this frame's damage inside the box is shaded)

    auto& shaderManager  = g_pGlobalState->shaderManager;
    const auto& uniforms = shaderManager.glassUniforms;

    const auto transform = Math::wlTransformToHyprutils(
        Math::invertTransform(g_pHyprRenderer->m_renderData.pMonitor->m_transform));

    Mat3x3 glMatrix = g_pHyprRenderer->projectBoxToTarget(rawBox, transform);
    auto texture    = sampleFramebuffer->getTexture();

    glMatrix.transpose();
    // into the kept glass: the quad fills it, uv for uv
    static constexpr std::array<float, 9> CACHE_PROJECTION = {
        2.0f, 0.0f, 0.0f,
        0.0f, 2.0f, 0.0f,
       -1.0f,-1.0f, 1.0f,
    };

    if (intoCache) {
        g_pHyprRenderer->blend(false); // (the shader's output as it is: the copy blends it later)
        g_pHyprOpenGL->setViewport(0, 0, static_cast<int>(intoCache->fb->m_size.x), static_cast<int>(intoCache->fb->m_size.y));
        glBindFramebuffer(GL_FRAMEBUFFER, fbId(intoCache->fb));
        if (intoCache->clear) {
            g_pHyprOpenGL->setCapStatus(GL_SCISSOR_TEST, false);
            glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            glClear(GL_COLOR_BUFFER_BIT);
        }
    } else
        glBindFramebuffer(GL_FRAMEBUFFER, fbId(targetFramebuffer));
    glActiveTexture(GL_TEXTURE0);
    texture->bind();

    // Layers only: bind the temp FBO texture (rendered surface) on texture unit 1.
    // The shader samples it to mask glass to visible content and composite surface on top.
    // Windows pass mask=nullptr so this block is skipped.
    if (mask && mask->textureId != 0) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(mask->target, mask->textureId);
        glActiveTexture(GL_TEXTURE0);
    }
    if (mask && mask->sdfTextureId != 0) {
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, mask->sdfTextureId);
        glActiveTexture(GL_TEXTURE0);
    }
    if (mask && mask->styleTextureId != 0) {
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, mask->styleTextureId);
        glActiveTexture(GL_TEXTURE0);
    }
    if (mask && mask->presenceTextureId != 0) {
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D, mask->presenceTextureId);
        glActiveTexture(GL_TEXTURE0);
    }
    if (mask && mask->heldTextureId != 0) {
        glActiveTexture(GL_TEXTURE5);
        glBindTexture(GL_TEXTURE_2D, mask->heldTextureId);
        glActiveTexture(GL_TEXTURE0);
    }
    const bool frostOn = frost && frost->textureId != 0 && frost->amount > 0.001f;
    if (frostOn) {
        glActiveTexture(GL_TEXTURE6);
        glBindTexture(GL_TEXTURE_2D, frost->textureId);
        glActiveTexture(GL_TEXTURE0);
    }

    auto shader = g_pHyprOpenGL->useShader(shaderManager.glassShader);
    shadowUniformsFor(shaderManager.glassShader->program(), shaderManager.compileSerial);

    if (intoCache)
        shader->setUniformMatrix3fv(SHADER_PROJ, 1, GL_FALSE, CACHE_PROJECTION);
    else
        shader->setUniformMatrix3fv(SHADER_PROJ, 1, GL_FALSE, glMatrix.getMatrix());
    shader->setUniformInt(SHADER_TEX, 0);

    const auto fullSize = Vector2D(transformedBox.width, transformedBox.height);
    shader->setUniformFloat2(SHADER_FULL_SIZE,
        static_cast<float>(fullSize.x), static_cast<float>(fullSize.y));

    // a rounding_power window rule bypasses Hyprland's [2, 10] clamp; <= 0 is NaN in the SDF
    const float safeRoundingPower = std::max(roundingPower, 1.0f);

    // Reciprocals computed once per draw instead of once per pixel in the shader.
    const float minDimensionPx = static_cast<float>(std::min(fullSize.x, fullSize.y));
    shadowUniform2f(uniforms.invFullSize,
        1.0f / static_cast<float>(fullSize.x), 1.0f / static_cast<float>(fullSize.y));
    shadowUniform1f(uniforms.invRoundingPower, 1.0f / safeRoundingPower);

    const float edgeThicknessValue = resolvePresetFloat(resolveContext, &SPresetValues::edgeThickness, &SOverridableConfig::edgeThickness);
    const float lensDistortionValue = resolvePresetFloat(resolveContext, &SPresetValues::lensDistortion, &SOverridableConfig::lensDistortion);
    // Epsilon guard matches the shader's own degenerate-size behaviour (bezelWidthPx == 0 would divide by zero).
    shadowUniform1f(uniforms.invBezelWidthPx, 1.0f / std::max(edgeThicknessValue * minDimensionPx, 1e-4f));
    shadowUniform1f(uniforms.lensMaxPx, lensDistortionValue * minDimensionPx * 0.006f);

    shadowUniform1f(uniforms.refractionStrength,  resolvePresetFloat(resolveContext, &SPresetValues::refractionStrength, &SOverridableConfig::refractionStrength));
    shadowUniform1f(uniforms.chromaticAberration, resolvePresetFloat(resolveContext, &SPresetValues::chromaticAberration, &SOverridableConfig::chromaticAberration));
    shadowUniform1f(uniforms.fresnelStrength,     resolvePresetFloat(resolveContext, &SPresetValues::fresnelStrength, &SOverridableConfig::fresnelStrength));
    shadowUniform1f(uniforms.specularStrength,    resolvePresetFloat(resolveContext, &SPresetValues::specularStrength, &SOverridableConfig::specularStrength));
    shadowUniform1f(uniforms.specularAngle,       resolvePresetFloat(resolveContext, &SPresetValues::specularAngle, &SOverridableConfig::specularAngle));
    // Physical glass materializes instead of fading:
    // the pane is there almost at once while its lensing and light grow with the
    // fade (materialize below), so it never reads as a flat translucent plate.
    const float thicknessValue = resolvePresetFloat(resolveContext, &SPresetValues::thickness, &SOverridableConfig::thickness, 0.0f);
    // (form: an opening card's glass fades in with its lens on the card's own curve)
    const float paneAlpha      = thicknessValue > 0.0f ? std::min(1.0f, alpha * 2.5f) * (mask ? mask->form : 1.0f) : alpha;
    shadowUniform1f(uniforms.glassOpacity,        resolvePresetFloat(resolveContext, &SPresetValues::glassOpacity, &SOverridableConfig::glassOpacity) * paneAlpha);
    shadowUniform1f(uniforms.edgeThickness,       edgeThicknessValue);
    shadowUniform1f(uniforms.lensDistortion,      lensDistortionValue);
    shadowUniform1f(uniforms.refractionFlow,      resolvePresetFloat(resolveContext, &SPresetValues::refractionFlow, &SOverridableConfig::refractionFlow));
    shadowUniform1f(uniforms.refractionSpread,    resolvePresetFloat(resolveContext, &SPresetValues::refractionSpread, &SOverridableConfig::refractionSpread));
    shadowUniform1f(uniforms.fresnelTint,         resolvePresetFloat(resolveContext, &SPresetValues::fresnelTint, &SOverridableConfig::fresnelTint));
    shadowUniform1f(uniforms.bevelStrength,       resolvePresetFloat(resolveContext, &SPresetValues::bevelStrength, &SOverridableConfig::bevelStrength));
    shadowUniform1f(uniforms.bevelSize,           resolvePresetFloat(resolveContext, &SPresetValues::bevelSize, &SOverridableConfig::bevelSize));
    shadowUniform1f(uniforms.bevelTint,           resolvePresetFloat(resolveContext, &SPresetValues::bevelTint, &SOverridableConfig::bevelTint));
    shadowUniform1f(uniforms.bevelAngle,          resolvePresetFloat(resolveContext, &SPresetValues::bevelAngle, &SOverridableConfig::bevelAngle));
    shadowUniform1f(uniforms.bevelShadow,         resolvePresetFloat(resolveContext, &SPresetValues::bevelShadow, &SOverridableConfig::bevelShadow));

    // bevel width is defined in logical px; scale it to framebuffer px per monitor
    const auto monitor = g_pHyprRenderer->m_renderData.pMonitor.lock();
    shadowUniform1f(uniforms.monitorScale, monitor && monitor->m_scale > 0.0f ? monitor->m_scale : 1.0f);

    uploadThemeUniforms(resolveContext);

    {
        const float scale = monitor && monitor->m_scale > 0.0f ? monitor->m_scale : 1.0f;
        const float bezelWidthValue = resolvePresetFloat(resolveContext, &SPresetValues::bezelWidth, &SOverridableConfig::bezelWidth, 0.0f);
        shadowUniform1f(uniforms.thicknessPx,    std::max(thicknessValue, 0.0f) * scale);
        if (uniforms.depth >= 0) {
            const auto& c = g_pGlobalState->config;
            // (not the bar and its tooltips: shape-box glass is a thin strip)
            const bool thin = mask && mask->maskMode == 2;
            glUniform1f(uniforms.depth, c.depth && !thin ? std::clamp(static_cast<float>(**c.depth), 0.0f, 1.0f) : 0.0f);
        }
        if (uniforms.darkGlass >= 0) {
            const auto& c = g_pGlobalState->config;
            glUniform1f(uniforms.darkGlass, g_pGlobalState->darkShown >= 0.0f ? g_pGlobalState->darkShown :
                                                c.darkGlass ? std::clamp(static_cast<float>(**c.darkGlass), 0.0f, 1.0f) : 0.0f);
        }
        if (uniforms.cornerMiter >= 0) {
            const auto& c = g_pGlobalState->config;
            glUniform1f(uniforms.cornerMiter, c.cornerMiter && **c.cornerMiter > 0.5 ? 1.0f : 0.0f);
        }
        if (uniforms.lightUi >= 0) {
            const auto& c = g_pGlobalState->config;
            glUniform1f(uniforms.lightUi, c.lightUi && **c.lightUi > 0.5 ? 1.0f : 0.0f);
        }
        if (uniforms.lightGlass >= 0) {
            const auto& c = g_pGlobalState->config;
            glUniform1f(uniforms.lightGlass, g_pGlobalState->lightShown >= 0.0f ? g_pGlobalState->lightShown :
                                                 c.lightGlass ? std::clamp(static_cast<float>(**c.lightGlass), 0.0f, 1.0f) : 0.0f);
        }
        shadowUniform1f(uniforms.abbe,           std::max(resolvePresetFloat(resolveContext, &SPresetValues::abbe, &SOverridableConfig::abbe, 0.0f), 0.0f));
        shadowUniform1f(uniforms.bezelPx,        bezelWidthValue > 0.0f ? bezelWidthValue * scale : edgeThicknessValue * minDimensionPx);
        shadowUniform1f(uniforms.materialize,    std::clamp(alpha * (mask ? mask->form : 1.0f), 0.0f, 1.0f));
    }

    const int64_t tintColorValue = resolvePresetInt(resolveContext, &SPresetValues::tintColor, &SOverridableConfig::tintColor);
    shadowUniform3f(uniforms.tintColor,
        static_cast<float>((tintColorValue >> 24) & 0xFF) / 255.0f,
        static_cast<float>((tintColorValue >> 16) & 0xFF) / 255.0f,
        static_cast<float>((tintColorValue >> 8) & 0xFF) / 255.0f);
    shadowUniform1f(uniforms.tintAlpha,
        static_cast<float>(tintColorValue & 0xFF) / 255.0f);

    const int64_t fresnelColorValue = resolvePresetInt(resolveContext, &SPresetValues::fresnelColor, &SOverridableConfig::fresnelColor);
    shadowUniform3f(uniforms.fresnelColor,
        static_cast<float>((fresnelColorValue >> 24) & 0xFF) / 255.0f,
        static_cast<float>((fresnelColorValue >> 16) & 0xFF) / 255.0f,
        static_cast<float>((fresnelColorValue >> 8) & 0xFF) / 255.0f);
    shadowUniform1f(uniforms.fresnelColorAlpha,
        static_cast<float>(fresnelColorValue & 0xFF) / 255.0f);

    const int64_t bevelColorValue = resolvePresetInt(resolveContext, &SPresetValues::bevelColor, &SOverridableConfig::bevelColor);
    shadowUniform3f(uniforms.bevelColor,
        static_cast<float>((bevelColorValue >> 24) & 0xFF) / 255.0f,
        static_cast<float>((bevelColorValue >> 16) & 0xFF) / 255.0f,
        static_cast<float>((bevelColorValue >> 8) & 0xFF) / 255.0f);
    shadowUniform1f(uniforms.bevelColorAlpha,
        static_cast<float>(bevelColorValue & 0xFF) / 255.0f);

    // Layers only: enable mask and provide UV mapping from the glass quad into
    // the monitor-sized temp FBO. Windows use useMask=0 (no masking).
    if (mask && mask->textureId != 0) {
        shadowUniform1i(uniforms.useMask, 1);
        shadowUniform1i(uniforms.maskTex, 1);
        shadowUniform2f(uniforms.maskUVOffset,
            static_cast<float>(mask->uvOffset.x),
            static_cast<float>(mask->uvOffset.y));
        shadowUniform2f(uniforms.maskUVScale,
            static_cast<float>(mask->uvScale.x),
            static_cast<float>(mask->uvScale.y));
        shadowUniform1f(uniforms.maskAlphaThreshold, mask->alphaThreshold);
        shadowUniform1i(uniforms.maskMode, mask->maskMode);
        shadowUniform1i(uniforms.regionRectCount, mask->regionRectCount);
        if (mask->regionRectCount > 0)
            glUniform4fv(uniforms.regionRects, mask->regionRectCount,
                         reinterpret_cast<const float*>(mask->regionRects.data()));
    } else {
        shadowUniform1i(uniforms.useMask, 0);
        shadowUniform1f(uniforms.maskAlphaThreshold, 0.001f);
        shadowUniform1i(uniforms.maskMode, 0);
        shadowUniform1i(uniforms.regionRectCount, 0);
    }

    shadowUniform1i(uniforms.sdfTex, 2);
    shadowUniform1i(uniforms.styleTex, 3);
    shadowUniform1i(uniforms.presenceTex, 4);
    shadowUniform1i(uniforms.livePresence, mask && mask->presenceTextureId != 0 ? 1 : 0);
    shadowUniform1f(uniforms.presenceRef, mask ? mask->presenceRef : 1.0f);
    shadowUniform1i(uniforms.heldTex, 5);
    shadowUniform1i(uniforms.frostTex, 6);
    {
        // the gleam's light: it runs round this window's own outline (gleam[1]: s on
        // its clock) on a rounded path at one speed (motion tokens gleam*). The light
        // stands out from the path along its normal, so the rim catches it at the
        // path's point and less either side; round a corner it comes in close, so
        // the lit stretch narrows into the corner and widens out of it (at full
        // distance the next edge's whole stretch lit up at once, a sudden exit).
        // The active window's light, by style:
        // 1 Comet: a short streak glides round the rim at one speed, corners and all
        //   (its place along the outline, gleam.y, and its length, gleam.z; the shader
        //   measures every rim pixel's place along the same outline)
        // 2 Halo: a long, soft arc of light, a share of this window's own lap (so a
        //   small window and a large one are lit alike), round once every few seconds
        // 3 Twin: two shorter streaks half a lap apart, a little slower
        // 4 Outline: the whole rim lit, steady, brightest on the edges facing the room's
        //   light (upper left, as the glass's own glints), never below half elsewhere
        //   (gleam.yz: the light's direction, uv axes)
        // 5 Aura: the whole rim lit, with a broad brighter crest going round once every
        //   few seconds: always marked, gently alive
        // 6 Prism: a light drifting slowly round the room; where the rim faces it, the
        //   edge splits its light into colour (gleam.yz: its direction)
        float on = 0.0f, lx = 0.0f, ly = 0.0f, spread = 6.0f, gain = 1.0f;
        const int gleamStyle = GlassRenderer::gleamStyle();
        if (gleam && ((*gleam)[0] * (*gleam)[2] > 0.001f || (*gleam)[3] > 0.001f) && monitor) {
            const double   sc = monitor->m_scale > 0.0f ? monitor->m_scale : 1.0;
            const double   W = transformedBox.w, H = transformedBox.h, t = std::max<double>((*gleam)[1], 0.0);
            // (along the outline: the rim's lap, in px)
            const double R = std::min<double>(radii[0], 0.5 * std::min(W, H));
            const double P = 2.0 * (W + H) - 8.0 * R + 6.2831853 * R;
            // a direction on the screen, in this quad's uv axes (a rotated or flipped
            // monitor turns them, as for the room's light below)
            auto dirUv = [&](double sx, double sy) {
                const int   tr = static_cast<int>(monitor->m_transform), turns = tr & 3;
                const float c4[4] = {1.0f, 0.0f, -1.0f, 0.0f}, s4[4] = {0.0f, 1.0f, 0.0f, -1.0f};
                const float cs = c4[turns] * SCREEN_ROT_SIGN_C, sn = s4[turns] * SCREEN_ROT_SIGN_S, fx = tr >= 4 ? -1.0f : 1.0f;
                return Vector2D{cs * fx * sx - sn * fx * sy, sn * sx + cs * sy};
            };
            if (gleamStyle == 4) {
                const auto d = dirUv(std::sin(-0.785), -std::cos(-0.785));
                lx     = static_cast<float>(d.x);
                ly     = static_cast<float>(d.y);
                on     = 4.0f;
                gain   = 0.75f;
            } else if (gleamStyle == 5) {
                lx   = static_cast<float>(std::fmod(t / motion::tokens::gleamAuraLap, 1.0) * P);
                ly   = static_cast<float>(motion::tokens::gleamAuraSpread * P);
                on   = 5.0f;
                gain = 0.8f;
            } else if (gleamStyle == 6) {
                const double az = std::fmod(t / motion::tokens::gleamPrismLap, 1.0) * 6.2831853;
                const auto   d  = dirUv(std::sin(az), -std::cos(az));
                lx     = static_cast<float>(d.x);
                ly     = static_cast<float>(d.y);
                on     = 6.0f;
                spread = 7.0f;
                gain   = 1.15f;
            } else if (gleamStyle == 2) {
                lx   = static_cast<float>(std::fmod(t / motion::tokens::gleamHaloLap, 1.0) * P);
                ly   = static_cast<float>(motion::tokens::gleamHaloSpread * P);
                on   = 2.0f;
                gain = 0.8f; // (it lights much more of the rim: a little softer)
            } else {
                // (a full lap at gleamEdgeSpeed logical px per s)
                const bool twin = gleamStyle == 3;
                lx     = static_cast<float>(std::fmod(t * (twin ? motion::tokens::gleamTwinSpeed : motion::tokens::gleamEdgeSpeed) * sc, P));
                ly     = static_cast<float>((twin ? motion::tokens::gleamTwinLength : motion::tokens::gleamCometLength) * sc);
                on     = twin ? 3.0f : 2.0f;
            }
        }
        shadowUniform4f(uniforms.gleam, gleam ? (*gleam)[0] * (*gleam)[2] : 0.0f, lx, ly, on);
        shadowUniform1f(uniforms.gleamPulse, gleam ? (*gleam)[3] * (*gleam)[2] : 0.0f);
        shadowUniform3f(uniforms.gleamShape, spread, 0.12f, gain);
    }
    shadowUniform2f(uniforms.frost, frostOn ? frost->amount : 0.0f, frostOn ? frost->milk : 0.0f);
    const bool ghostOn = mask && mask->heldTextureId != 0;
    shadowUniform4f(uniforms.ghost, ghostOn ? mask->ghost[0] : 0.0f, ghostOn ? mask->ghost[1] : 1.0f, ghostOn ? mask->ghost[2] : 1.0f, 0.0f);
    shadowUniform1i(uniforms.sdfSource, mask && mask->sdfTextureId != 0 ? 1 : 0);
    shadowUniform1i(uniforms.adaptiveStyle, mask && mask->adaptiveStyle ? 1 : 0);
    {
        // Light and shadow are given in screen directions; a rotated or flipped
        // monitor turns the quad's uv axes against the screen's.
        const int  t       = monitor ? static_cast<int>(monitor->m_transform) : 0;
        const int  turns   = t & 3;
        const bool flipped = t >= 4;
        const float c[4] = {1.0f, 0.0f, -1.0f, 0.0f}, s[4] = {0.0f, 1.0f, 0.0f, -1.0f};
        const float cs = c[turns] * SCREEN_ROT_SIGN_C, sn = s[turns] * SCREEN_ROT_SIGN_S;
        // column-major mat2: rotate, then mirror x for the flipped transforms
        const float fx = flipped ? -1.0f : 1.0f;
        const float m[4] = {cs * fx, sn, -sn * fx, cs};
        glUniformMatrix2fv(uniforms.screenToUv, 1, GL_FALSE, m);
        // screen direction -> uv direction (column-major m)
        auto toUv = [&m](float x, float y) { return std::array<float, 2>{m[0] * x + m[2] * y, m[1] * x + m[3] * y}; };

        // Per-draw constants the shader would otherwise recompute for every pixel.
        // Backdrop uv: the sample-box remap and the padding remap as one fma.
        const float sx = mask ? static_cast<float>(mask->sampleUVScale.x) : 1.0f, sy = mask ? static_cast<float>(mask->sampleUVScale.y) : 1.0f;
        const float ox = mask ? static_cast<float>(mask->sampleUVOffset.x) : 0.0f, oy = mask ? static_cast<float>(mask->sampleUVOffset.y) : 0.0f;
        const float px = static_cast<float>(paddingRatio.x), py = static_cast<float>(paddingRatio.y);
        const float kx = (1.0f - 2.0f * px) / sx, ky = (1.0f - 2.0f * py) / sy;
        shadowUniform4f(uniforms.sampleXform, kx, ky, px - ox * kx, py - oy * ky);

        // Cauchy dispersion fitted to n_d and the Abbe number: n(l) = A + B/l^2,
        // n_F - n_C = (n_d - 1) / V; channels at 611, 549 and 464 nm.
        const float n_d  = std::max(resolvePresetFloat(resolveContext, &SPresetValues::ior, &SOverridableConfig::ior, 1.5f), 1.0001f);
        const float abbeV = std::max(resolvePresetFloat(resolveContext, &SPresetValues::abbe, &SOverridableConfig::abbe, 0.0f), 0.0f);
        if (abbeV > 0.0f) {
            const float B = (n_d - 1.0f) / (abbeV * 1.9106f), A = n_d - B / 0.34527f;
            shadowUniform3f(uniforms.nRGB, A + B / 0.37332f, A + B / 0.30140f, A + B / 0.21530f);
        } else
            shadowUniform3f(uniforms.nRGB, n_d, n_d, n_d);
        const float r0 = (n_d - 1.0f) / (n_d + 1.0f);
        shadowUniform1f(uniforms.fresnelF0, r0 * r0);

        // where the rim light comes from across the screen (azimuth clockwise from
        // the top); a light straight above (elevation 90) lights no side of the rim
        const float az = resolvePresetFloat(resolveContext, &SPresetValues::specularAngle, &SOverridableConfig::specularAngle) * 0.0174533f;
        const float el = std::clamp(resolvePresetFloat(resolveContext, &SPresetValues::lightElevation, &SOverridableConfig::lightElevation, 40.0f), 0.0f, 90.0f) * 0.0174533f;
        const auto  lu = toUv(std::sin(az) * std::cos(el), -std::cos(az) * std::cos(el));
        const float ll = std::sqrt(lu[0] * lu[0] + lu[1] * lu[1]);
        shadowUniform2f(uniforms.lightUv, ll > 1e-4f ? lu[0] / ll : 0.0f, ll > 1e-4f ? lu[1] / ll : 0.0f);
        const float ba = resolvePresetFloat(resolveContext, &SPresetValues::bevelAngle, &SOverridableConfig::bevelAngle) * 0.0174533f;
        const auto  bl = toUv(std::sin(ba), -std::cos(ba));
        shadowUniform2f(uniforms.bevelLightUv, bl[0], bl[1]);
    }
    {
        // the press point in this quad's own px: monitor px, then the same
        // transform the quad's box went through
        float age = g_pGlobalState->pressGlowAge();
        Vector2D local{-1e5, -1e5};
        if (age >= 0.0f && monitor) {
            CBox p{(g_pGlobalState->pressGlow.pos - monitor->m_position) * monitor->m_scale, {1.0, 1.0}};
            p.transform(Math::wlTransformToHyprutils(Math::invertTransform(monitor->m_transform)),
                        monitor->m_transformedSize.x, monitor->m_transformedSize.y);
            local = p.pos() - transformedBox.pos();
            if (local.x < 0 || local.y < 0 || local.x > transformedBox.w || local.y > transformedBox.h)
                age = -1.0f; // pressed somewhere else: only the glass that was pressed glows
        }
        shadowUniform3f(uniforms.pressGlow, local.x, local.y, age);
        // the glass giving: the pressBulge spring kicked from rest, its first swell 1
        float bulge = 0.0f;
        if (age >= 0.0f) {
            static const motion::Spring spring = motion::tokens::pressBulge();
            static const double         peak   = [] {
                const motion::SpringMotion m(spring, 0.0, 0.0, 1.0, 0.0);
                double hi = 0.0;
                for (double t = 0.0; t < 0.5; t += 0.0005) hi = std::max(hi, m.value(t));
                return hi;
            }();
            const motion::SpringMotion m(spring, 0.0, 0.0, 1.0 / peak, 0.0);
            bulge = static_cast<float>(m.value(age * SGlobalState::PRESS_GLOW_SECONDS));
        }
        shadowUniform1f(uniforms.pressBulge, bulge);
        // (the glow fades out by 1.12x this, inside the PRESS_GLOW_REACH box main.cpp damages)
        static_assert(300.0f * 1.12f < SGlobalState::PRESS_GLOW_REACH);
        shadowUniform1f(uniforms.pressReachPx, 300.0f * (monitor ? monitor->m_scale : 1.0f));
    }
    shadowUniform4f(uniforms.contentBox, mask ? static_cast<float>(mask->contentBox.x) : 0.0f, mask ? static_cast<float>(mask->contentBox.y) : 0.0f,
                mask ? static_cast<float>(mask->contentBox.w) : -1.0f, mask ? static_cast<float>(mask->contentBox.h) : -1.0f);
    shadowUniform4f(uniforms.sdfRect, mask ? static_cast<float>(mask->sdfRect.x) : 0.0f, mask ? static_cast<float>(mask->sdfRect.y) : 0.0f,
                mask ? static_cast<float>(std::max(mask->sdfRect.w, 1.0)) : 1.0f, mask ? static_cast<float>(std::max(mask->sdfRect.h, 1.0)) : 1.0f);
    shadowUniform4f(uniforms.openAnim, mask ? static_cast<float>(mask->openAnchorPx.x) : 0.0f, mask ? static_cast<float>(mask->openAnchorPx.y) : 0.0f,
                mask ? mask->openScale : 1.0f, mask ? (mask->openScaleY < 0.0f ? mask->openScale : mask->openScaleY) : 1.0f);
    if (uniforms.paneRect >= 0)
        glUniform4f(uniforms.paneRect, mask ? mask->paneRect[0] : 0.0f, mask ? mask->paneRect[1] : 0.0f, mask ? mask->paneRect[2] : 0.0f, mask ? mask->paneRect[3] : 0.0f);
    if (uniforms.outFade >= 0)
        glUniform1f(uniforms.outFade, mask ? mask->outFade : 1.0f);
    if (uniforms.foldRect >= 0)
        glUniform4f(uniforms.foldRect, mask ? mask->foldRect[0] : 0.0f, mask ? mask->foldRect[1] : 0.0f, mask ? mask->foldRect[2] : 0.0f, mask ? mask->foldRect[3] : 0.0f);
    if (uniforms.foldInfo >= 0)
        glUniform2f(uniforms.foldInfo, mask ? mask->foldInfo[0] : 0.0f, mask ? mask->foldInfo[1] : 0.0f);
    {
        const bool m = mask && (mask->morph || mask->morphBarSide);
        shadowUniform4f(uniforms.morphRect, m ? mask->morphRect[0] : 0.0f, m ? mask->morphRect[1] : 0.0f, m ? mask->morphRect[2] : 0.0f, m ? mask->morphRect[3] : 0.0f);
        shadowUniform4f(uniforms.morphCard, m ? mask->morphCard[0] : 0.0f, m ? mask->morphCard[1] : 0.0f, m ? mask->morphCard[2] : 0.0f, m ? mask->morphCard[3] : 0.0f);
        shadowUniform4f(uniforms.morphSource, m ? mask->morphSource[0] : 0.0f, m ? mask->morphSource[1] : 0.0f, m ? mask->morphSource[2] : 0.0f,
                        m ? mask->morphSource[3] : 0.0f);
        shadowUniform4f(uniforms.morphShape, m ? mask->morphRadius : 0.0f, m ? mask->morphSourceRadius : 0.0f, m ? mask->morphNeck : 0.0f, !m ? 0.0f : mask->morphBarSide ? 3.0f : mask->morphBar ? 2.0f : 1.0f);
        shadowUniform4f(uniforms.contentXform, m ? static_cast<float>(mask->contentAnchor.x) : 0.0f, m ? static_cast<float>(mask->contentAnchor.y) : 0.0f,
                        m ? static_cast<float>(mask->screenAnchor.x) : 0.0f, m ? static_cast<float>(mask->screenAnchor.y) : 0.0f);
        // (low power: the content comes in sharp, one read a pixel instead of thirteen)
        const bool lowPower = g_pGlobalState->config.lowPower && **g_pGlobalState->config.lowPower > 0.5f;
        shadowUniform4f(uniforms.contentFx, m ? mask->contentScale : 1.0f, m ? mask->contentAlpha : 1.0f, m && !lowPower ? mask->contentBlurPx : 0.0f,
                        m ? mask->morphOverlap : 0.0f);
        shadowUniform4f(uniforms.morphWaist, m ? mask->morphWaist[0] : 0.0f, m ? mask->morphWaist[1] : 0.0f, m ? mask->morphWaist[2] : 0.0f, m ? mask->morphWaist[3] : 0.0f);
        shadowUniform2f(uniforms.morphHand, m ? mask->morphHand[0] : 0.0f, m ? mask->morphHand[1] : 0.0f);
        const bool bg = m && mask->barGradeOn;
        shadowUniform4f(uniforms.barGrade1, bg ? mask->barGrade1[0] : 1.0f, bg ? mask->barGrade1[1] : 0.0f, bg ? mask->barGrade1[2] : 1.0f, bg ? mask->barGrade1[3] : -1.0f);
        shadowUniform4f(uniforms.barTint, bg ? mask->barTint[0] : 0.0f, bg ? mask->barTint[1] : 0.0f, bg ? mask->barTint[2] : 0.0f, bg ? mask->barTint[3] : 0.0f);
        const bool bs = bg && mask->barStyleOn;
        shadowUniform4f(uniforms.barStyle, bs ? mask->barStyleMap[0] : 0.0f, bs ? mask->barStyleMap[1] : -1.0f, bs ? mask->barStyleMap[2] : 0.0f, bs ? mask->barStyleMap[3] : 0.0f);
        shadowUniform2f(uniforms.morphBarLens, m ? static_cast<float>(mask->morphBarLens.x) : 0.0f, m ? static_cast<float>(mask->morphBarLens.y) : 0.0f);
        if (uniforms.morphWaistBar >= 0)
            glUniform1f(uniforms.morphWaistBar, m ? mask->morphWaistBar : 0.0f);
        shadowUniform4f(uniforms.morphWaist2, m ? mask->morphWaistHalf : 0.0f, m && !mask->morphWaistAlongX ? 1.0f : 0.0f, m ? mask->morphWaistHold : 1.0f,
                        m ? mask->morphWaistSnap : 0.0f);
    }
    shadowUniform4f(uniforms.fillKey, mask ? mask->fillKey[0] : 0.0f, mask ? mask->fillKey[1] : 0.0f, mask ? mask->fillKey[2] : 0.0f,
                mask ? mask->fillKey[3] : -1.0f);
    shadowUniform1i(uniforms.fillKeyTop, mask ? mask->fillKeyTop : 0);
    shadowUniform1f(uniforms.textBacking, mask ? mask->textBacking : 1.0f);
    shadowUniform1f(uniforms.shadowOpacity, mask ? mask->shadowOpacity : 0.0f);
    shadowUniform1f(uniforms.shadowRangePx, mask ? std::max(mask->shadowRangePx, 1.0f) : 1.0f);
    // straight down, as a light overhead casts them (sideways it read as a heavy band down one side)
    shadowUniform2f(uniforms.shadowOffsetPx, 0.0f, mask ? mask->shadowOffsetPx : 0.0f);

    shadowUniform4f(uniforms.radii, radii[0], radii[1], radii[2], radii[3]);
    shader->setUniformFloat(SHADER_ROUNDING_POWER, safeRoundingPower);

    // Subsurface items only: SDF sub-rect within the drawn box (see
    // SMaskInfo::glassBoxSizePx). Sentinel (negative) falls back to the whole
    // box — windows and alpha-mask layers always take this path, unchanged
    // from before this uniform existed.
    Vector2D glassBoxOffsetPx{0.0, 0.0};
    Vector2D glassBoxSizePx = fullSize;
    if (mask && mask->glassBoxSizePx.x >= 0.0 && mask->glassBoxSizePx.y >= 0.0) {
        glassBoxOffsetPx = mask->glassBoxOffsetPx;
        glassBoxSizePx   = mask->glassBoxSizePx;
    }
    shadowUniform2f(uniforms.glassBoxOffsetPx, static_cast<float>(glassBoxOffsetPx.x), static_cast<float>(glassBoxOffsetPx.y));
    shadowUniform2f(uniforms.glassBoxSizePx, static_cast<float>(glassBoxSizePx.x), static_cast<float>(glassBoxSizePx.y));

    glBindVertexArray(shader->getUniformLocation(SHADER_SHADER_VAO));

    if (intoCache) {
        // the kept glass's own px (uv axes): only what changed is computed again
        double shaded = 0.0;
        if (intoCache->rects.empty()) {
            g_pHyprOpenGL->setCapStatus(GL_SCISSOR_TEST, false);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            shaded = intoCache->fb->m_size.x * intoCache->fb->m_size.y;
        } else {
            g_pHyprOpenGL->setCapStatus(GL_SCISSOR_TEST, true);
            for (const auto& r : intoCache->rects) {
                // (through Hyprland, never glScissor: it skips a box it believes is already
                // set, so a raw one stayed in force and clipped the next window to this rim
                // strip; a fullscreen video showed the wallpaper)
                g_pHyprOpenGL->scissor(static_cast<int>(std::floor(r.x)), static_cast<int>(std::floor(r.y)), static_cast<int>(std::ceil(r.w)) + 1,
                                       static_cast<int>(std::ceil(r.h)) + 1, false);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                shaded += r.w * r.h;
            }
            g_pHyprOpenGL->setCapStatus(GL_SCISSOR_TEST, false);
        }
        g_pHyprRenderer->blend(true);
        glBindFramebuffer(GL_FRAMEBUFFER, fbId(targetFramebuffer));
        g_pHyprOpenGL->setViewport(0, 0, static_cast<int>(targetFramebuffer->m_size.x), static_cast<int>(targetFramebuffer->m_size.y));
        g_pHyprOpenGL->scissor(nullptr);
        if (const auto monitor = g_pHyprRenderer->m_renderData.pMonitor.lock())
            Diagnostics::recordGlassPixels(monitor->m_id, shaded);
        return;
    }

    // Only the damage, rect by rect, never more: what lies above the glass is redrawn
    // only where this frame is damaged, so glass shaded anywhere else stays on screen
    // over it. (The bounding box of many rects, or the whole box when the damage
    // missed it, covered a fullscreen YouTube window with the wallpaper.)
    CRegion inBox = g_pHyprRenderer->m_renderData.damage.copy().intersect(rawBox);
    // A layer's glass only where something of it is drawn (a contour card: its
    // measured content, with room for its shadow): the shader discards the rest,
    // but each pixel still started it, and a full-screen layer over a scrolling
    // terminal started it over the whole screen every frame.
    if (mask && mask->contentBox.w > 0.0 && mask->contentBox.h > 0.0 && !inBox.empty()) {
        const CBox inQuad = mask->contentBox.copy().translate(transformedBox.pos()).expand(1.0);
        inBox.intersect(WindowGeometry::unapplyMonitorTransform(inQuad, g_pHyprRenderer->m_renderData.pMonitor.lock()));
    }
    // (an empty region: this frame's damage misses the glass, nothing to do)
    double shaded = 0.0;
    for (const auto& r : inBox.getRects()) {
        const CBox b{static_cast<double>(r.x1), static_cast<double>(r.y1), static_cast<double>(r.x2 - r.x1), static_cast<double>(r.y2 - r.y1)};
        g_pHyprOpenGL->scissor(b);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        shaded += b.w * b.h;
    }
    g_pHyprOpenGL->scissor(nullptr);
    if (const auto monitor = g_pHyprRenderer->m_renderData.pMonitor.lock())
        Diagnostics::recordGlassPixels(monitor->m_id, shaded);
}

void drawOutputCache(const SP<Render::IFramebuffer>& cache, SP<Render::IFramebuffer> targetFramebuffer, CBox& rawBox, GLuint& filterSetFor) {
    auto& shaderManager = g_pGlobalState->shaderManager;
    if (!cache || !targetFramebuffer || !shaderManager.isInitialized())
        return;
    Diagnostics::CScopedStageTimer stageTimer(Diagnostics::EStage::ApplyGlassEffect);

    const auto transform = Math::wlTransformToHyprutils(Math::invertTransform(g_pHyprRenderer->m_renderData.pMonitor->m_transform));
    Mat3x3     glMatrix  = g_pHyprRenderer->projectBoxToTarget(rawBox, transform);
    glMatrix.transpose();

    glBindFramebuffer(GL_FRAMEBUFFER, fbId(targetFramebuffer));
    glActiveTexture(GL_TEXTURE0);
    const auto tex = cache->getTexture();
    tex->bind();
    // (texel for pixel: the glass box sits on whole pixels; set once per texture,
    // as each change makes the driver validate the texture again: 4 us a copy)
    if (tex->m_texID != filterSetFor) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        filterSetFor = tex->m_texID;
    }
    auto shader = g_pHyprOpenGL->useShader(shaderManager.copy.shader);
    shader->setUniformMatrix3fv(SHADER_PROJ, 1, GL_FALSE, glMatrix.getMatrix());
    shader->setUniformInt(SHADER_TEX, 0);
    glBindVertexArray(shader->getUniformLocation(SHADER_SHADER_VAO));

    // (only the damage, rect by rect: as in applyGlassEffect)
    CRegion inBox = g_pHyprRenderer->m_renderData.damage.copy().intersect(rawBox);
    for (const auto& r : inBox.getRects()) {
        const CBox b{static_cast<double>(r.x1), static_cast<double>(r.y1), static_cast<double>(r.x2 - r.x1), static_cast<double>(r.y2 - r.y1)};
        g_pHyprOpenGL->scissor(b);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    g_pHyprOpenGL->scissor(nullptr);
    glBindVertexArray(0);
}

} // namespace GlassRenderer

bool GlassRenderer::reducedMotion() {
    // (looked up each time: a config reload may move the value)
    const auto* const ENABLED = reinterpret_cast<Hyprlang::INT* const*>(Config::mgr()->getConfigValue("animations:enabled").dataptr);
    return ENABLED && *ENABLED && !**ENABLED;
}

int GlassRenderer::gleamStyle() {
    const int style = g_pGlobalState->config.gleamStyle ? static_cast<int>(**g_pGlobalState->config.gleamStyle) : 1;
    return reducedMotion() ? 4 : style; // (4: the whole edge lit, steady)
}

void GlassRenderer::stepTone() {
    if (!g_pGlobalState)
        return;
    auto&       g     = *g_pGlobalState;
    const auto& c     = g.config;
    const float dark  = c.darkGlass ? std::clamp(static_cast<float>(**c.darkGlass), 0.0f, 1.0f) : 0.0f;
    const float light = c.lightGlass ? std::clamp(static_cast<float>(**c.lightGlass), 0.0f, 1.0f) : 0.0f;
    const auto  now   = std::chrono::steady_clock::now();
    // (the first frame, or animations off: as set)
    if (g.darkShown < 0.0f || reducedMotion()) {
        const bool changed = g.darkShown >= 0.0f && (g.darkShown != dark || g.lightShown != light);
        g.darkShown = dark;
        g.lightShown = light;
        g.toneAt = now;
        if (changed)
            g.configSerial++;
        return;
    }
    // (after a still spell the change starts now: one frame's step, as frost does)
    float dt = std::chrono::duration<float>(now - g.toneAt).count();
    dt       = dt > 0.05f || dt < 0.0f ? 1.0f / 60.0f : dt;
    g.toneAt = now;
    bool moved = false;
    for (auto [shown, target] : {std::pair{&g.darkShown, dark}, std::pair{&g.lightShown, light}}) {
        if (*shown == target)
            continue;
        moved = true;
        *shown = std::abs(target - *shown) < 0.004f ? target : *shown + (target - *shown) * (1.0f - std::exp(-dt / 0.09f));
    }
    if (!moved)
        return;
    // every pane is drawn again with it, this frame and the next
    g.configSerial++;
    for (const auto& m : State::monitorState()->monitors())
        g_pHyprRenderer->damageMonitor(m);
}
