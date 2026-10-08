#pragma once

#include "Frost.hpp"
#include "PluginConfig.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <GLES3/gl32.h>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/render/Framebuffer.hpp>
#include <hyprutils/math/Box.hpp>
#include <hyprutils/math/Region.hpp>
#include <hyprutils/math/Vector2D.hpp>

// Shared GL rendering pipeline used by both window decorations and layer surfaces.
// Callers own their sample framebuffers; these functions operate on passed-in state.
namespace GlassRenderer {

inline constexpr int SAMPLE_PADDING_PX = 60;

// Hyprland's animations turned off (misc: the user wants less motion): cards fade
// where they are instead of pouring out of the bar, and the window light holds still
[[nodiscard]] bool reducedMotion();
// the window light's style (plugin:hyprglass:gleam_style), a still one under reducedMotion()
[[nodiscard]] int gleamStyle();
// once a frame: Dark and Light glass ease to their settings over a quarter second
// (switched in one frame, every pane on screen changed tint at once)
void stepTone();

// Maximum downscale factor for blur sampling. Half-res (2) is 4x cheaper
// per blur pass. Only applied when blur is strong enough to hide the lower
// resolution — weak blur at half-res shows visible pixelation.
inline constexpr int   BLUR_DOWNSCALE_MAX       = 2;
inline constexpr float BLUR_DOWNSCALE_THRESHOLD = 0.35f; // min blur_strength for downscale
// The sample's downscale: half resolution for a strong blur, or always with
// low_power (older GPUs: a quarter of the pixels to copy and read)
[[nodiscard]] int sampleDownscale(float blurStrength);

// Tap cap in gaussianblur.frag's `min(int(ceil(blurRadius)), 8)` (Shaders.hpp) — the
// per-pass radius where taps stop tracking radius 1:1 and the shader's designed ~3 sigma
// coverage starts truncating. Named here so foldBlurPasses() cannot drift from the shader's cap.
inline constexpr float BLUR_SHADER_TAP_CAP = 8.0f;

// Result of folding N blur passes at a fixed radius into fewer, larger-radius passes.
// See foldBlurPasses().
struct SFoldedBlur {
    float radius;
    int   iterations;
};

// Gaussian semigroup identity: N passes at `radius` produce the same total blur as one pass
// at `radius * sqrt(N)`, so N passes of sigma compound rather than average (blurBackground()
// reuses the same radius every pass). Finds the smallest pass count N' whose own per-pass
// radius (r_total / sqrt(N')) stays at or under the shader's tap cap, preserving the
// requested total blur while cutting fetches. Only ever returns fewer passes than requested:
// when the derived N' would be >= the input iterations (the fold would cost the same amount
// of work or more — e.g. a wide single-pass radius needing many passes to stay untruncated),
// the input is returned unchanged.
[[nodiscard]] SFoldedBlur foldBlurPasses(float radius, int iterations) noexcept;

// Must match the `regionRects[16]` array size declared in Shaders.hpp.
inline constexpr int MAX_REGION_RECTS = 16;

// Box-local pixel rect uploaded to the shader's regionRects uniform array.
struct SRegionRect {
    float x = 0, y = 0, w = 0, h = 0;
};
static_assert(sizeof(SRegionRect) == 4 * sizeof(float));

// Layers only: alpha mask from the temp FBO that captured the rendered surface.
// Constrains the glass effect to regions where the layer has visible content.
// Windows do not use masking, they pass mask=nullptr to applyGlassEffect.
struct SMaskInfo {
    GLuint   textureId;
    GLenum   target;
    Vector2D uvOffset; // mapping from glass box UV → full surface UV
    Vector2D uvScale;
    float    alphaThreshold = 0.001f;

    // 0 = alpha-threshold mask, 1 = ext-background-effect-v1 protocol region,
    // 2 = shape box (the layer's fixed glass shape, glassBoxOffsetPx/SizePx)
    int                                        maskMode        = 0;
    std::array<SRegionRect, MAX_REGION_RECTS>  regionRects{};
    int                                        regionRectCount = 0;

    // Maps the glass quad's own UV (spanning the full drawn box) into the
    // sample texture's own normalized space, before its padding (both folded
    // into the shader's sampleXform). Identity (no-op) unless
    // sampleBackground() was given a box smaller than the drawn quad —
    // PROTOCOL_REGION and CONTOUR layers only; see GlassLayerSurface.cpp.
    Vector2D sampleUVOffset{0.0, 0.0};
    Vector2D sampleUVScale{1.0, 1.0};

    // Subsurface items and shape-box layers: the rounded-box SDF's own sub-rect
    // within the drawn box, box-local pixels (see Shaders.hpp's glassBoxOffsetPx/SizePx).
    // Sentinel (negative size) means "use the full drawn box", applyGlassEffect's
    // old, unconditional behaviour — every other caller (windows, alpha-mask
    // layers) leaves this at the default and sees no change at all.
    Vector2D glassBoxOffsetPx{0.0, 0.0};
    Vector2D glassBoxSizePx{-1.0, -1.0};

    // Contour layers: distance field of the surface's own shape
    // (ShapeField::build), covering sdfRect. 0 = analytic box SDF.
    GLuint sdfTextureId = 0;
    GLuint styleTextureId = 0; // adaptive bars: brightness strip (unit 3)
    GLuint presenceTextureId = 0; // contour cards: this frame's content visibility (unit 4)
    float  presenceRef = 1.0f;    // its settled value (the glass is full at it)
    // Flip light/dark style with the backdrop (small glass like a bar)
    bool   adaptiveStyle = false;
    // Drop shadow of layer glass, framebuffer px (opacity 0 = none)
    float  shadowOpacity = 0.0f;
    float  shadowRangePx = 0.0f;
    float  shadowOffsetPx = 0.0f;
    // Surface fill colour to see through (rgb 0..1) and the alpha it keeps; a < 0: off
    std::array<float, 4> fillKey{0.0f, 0.0f, 0.0f, -1.0f};
    float                textBacking = 1.0f; // the content's visibility when the shader has no live reading of it
    int                  fillKeyTop = 0; // the keyed line along the inner edge drawn along the outer one; which edge is outer: 1 uv.y 0, 2 uv.y 1, 3 uv.x 0, 4 uv.x 1
    // Open animation: anchor in the drawn box's px and scale (1 = none); openScaleY < 0: openScale
    Vector2D openAnchorPx{0.0, 0.0};
    float    openScale = 1.0f;
    float    openScaleY = -1.0f;
    // A closing card drawn from its held settled frame (over sdfRect): mode (0 off, 1 armed,
    // 2 running), content alpha, glass formed
    GLuint               heldTextureId = 0;
    std::array<float, 3> ghost{0.0f, 1.0f, 1.0f};
    // 0..1: how far the lens has formed (open animation)
    float    form = 1.0f;
    // Contour layers: the box-local px area anything is drawn in (w < 0: all;
    // w == 0: not measured yet, the surface as it is without glass)
    CBox     contentBox{0.0, 0.0, -1.0, -1.0};
    // the distance field's rect in the drawn box's px
    CBox     sdfRect{0.0, 0.0, 1.0, 1.0};
    // A card in motion (motion::Morph): its shape as it is now, drawn analytically,
    // and its content scaled evenly (never stretched). Quad px; rects x0 y0 x1 y1.
    bool                 morph = false;
    std::array<float, 4> morphRect{};          // the card's rounded rect
    float                outFade = 1.0f;       // the whole result faded
    std::array<float, 4> paneRect{};           // the card's own box (x0 y0 x1 y1, quad px; empty: the glass box)
    std::array<float, 4> foldRect{};           // the bar a card hangs from (its backdrop folded out of it)
    std::array<float, 2> foldInfo{};
    std::array<float, 4> morphCard{};          // the settled card it runs to/from (its field's shape)
    std::array<float, 4> morphSource{};        // the bar it comes out of (empty: none)
    bool                 morphBar = false;     // the bar draws itself: nothing of the card inside it
    std::array<float, 4> morphWaist{};         // the waist to the bar: along-centre, from, to (across), open 0..1
    float                morphWaistHalf = 0.0f; // its widest half width
    float                morphWaistBar  = 0.0f; // its half width at the bar (the item it comes out of; 0: as wide)
    bool                 morphWaistAlongX = true; // the bar runs across (x), else down a side
    float                morphWaistHold = 1.0f, morphWaistSnap = 0.0f; // see MorphGeometry
    std::array<float, 2> morphHand{0.0f, 0.0f}; // handed over to the field 0..1; joined to the bar 0..1
    float                morphOverlap = 0.0f;  // how far the card overlaps the bar it comes out of, px
    bool                 morphBarSide = false; // the bar's part of a card's liquid: its shape is the union (not a morph of its own)
    Vector2D             morphBarLens{0.0, 0.0}; // the bar's glass height and rim px (the card's lens at the seam)
    // the bar's grading, for the card's seam with it: brightness, adaptive dim,
    // contrast, saturation; its tint rgb + alpha; its style strip (styleTextureId)
    // mapped from this quad's px (x offset, 1/w, y offset, 1/h)
    bool                 barGradeOn = false, barStyleOn = false;
    std::array<float, 4> barGrade1{1.0f, 0.0f, 1.0f, 1.0f}, barTint{}, barStyleMap{};
    float                morphRadius = 0.0f, morphSourceRadius = 0.0f, morphNeck = 0.0f;
    // content px = contentAnchor + (quad px - screenAnchor) / contentScale
    Vector2D             contentAnchor{0.0, 0.0}, screenAnchor{0.0, 0.0};
    float                contentScale = 1.0f, contentAlpha = 1.0f, contentBlurPx = 0.0f;
};

// Affine map from source-framebuffer pixels into the sample framebuffer.
// sampleBackground() blits through it and blendOwnContent() draws through it;
// both derive every coordinate from here so the two can never drift apart.
struct SSampleMap {
    int   fullWidth = 1, fullHeight = 1;              // padded box, framebuffer pixels
    int   width = 1, height = 1;                      // sample framebuffer size
    int   srcX0 = 0, srcY0 = 0, srcX1 = 1, srcY1 = 1; // padded box in source pixels, (srcX0, srcY0) lands on sample (0, 0)
    float scaleX = 1, scaleY = 1;

    [[nodiscard]] CBox toSample(const CBox& framebufferBox) const;
};

[[nodiscard]] SSampleMap sampleMapFor(const CBox& box, int downscale);

// True when every pixel sampleBackground() would read for `box` lies inside
// `damage`. The only coverage predicate. `box` is in post-transform framebuffer
// pixels like `damage`, not the logical space boundingBox() pads in.
[[nodiscard]] bool sampleRegionCovered(const CBox& box, const SP<Render::IFramebuffer>& source, const CRegion& damage);
// `box` (transformed framebuffer px) lies wholly inside this frame's damage
[[nodiscard]] bool boxCovered(const CBox& box, const CRegion& damage);

// Refresh one rect of a cached full-size sample (downscale 1) in place: `patch`
// in the same framebuffer-space px as `box`, the box the sample was taken for. For
// what changes beside a window (in its padding, where the rim's reflections come
// from), so only that rect is copied and only the rim near it redraws. False when
// the cached sample isn't one this can patch (another size: take a full sample).
[[nodiscard]] bool patchSample(SP<Render::IFramebuffer>& sampleFramebuffer, SP<Render::IFramebuffer> sourceFramebuffer, const CBox& box, const CBox& patch);

void sampleBackground(SP<Render::IFramebuffer>& sampleFramebuffer, SP<Render::IFramebuffer> sourceFramebuffer,
                       CBox box, Vector2D& outPaddingRatio, int downscale = 1);

// Draws the window's own committed surfaces over the sampled background, so the
// blur that follows works on a mix of the desktop and the window's own content.
// box is the same framebuffer-space box sampleBackground() was given.
void blendOwnContent(SP<Render::IFramebuffer>& sampleFramebuffer, PHLWINDOW window, PHLMONITOR monitor,
                      const CBox& box, int downscale, float amount, float cornerRadius, float roundingPower);

// callerFramebuffer is re-bound after the blur ping-pong; the viewport is
// restored from its size so it always matches the re-bound framebuffer
// (monitor fields would be wrong on 90°/270° transformed monitors).
void blurBackground(SP<Render::IFramebuffer> sampleFramebuffer, float radius, int iterations,
                    SP<Render::IFramebuffer> callerFramebuffer);

// When mask is non-null (layers only), the shader composites the surface content
// over the glass effect in a single pass. When mask is null (windows), the shader
// outputs the glass effect alone.
//
// radii: per-corner radius (top-left, top-right, bottom-right, bottom-left).
// Windows and layers pass the same value four times — see CGlassDecoration::
// renderPass() and CGlassLayerSurface::compositeAndRestore(); only subsurface
// item glass (CGlassSubsurfaceState) ever passes unequal corners.
// A window's finished glass, kept: drawn into `fb` (the glass box's own px, uv
// axes) instead of the target, only over `rects` (all of it when empty, after a
// clear when `clear`). drawOutputCache() then puts it on screen.
struct SOutputCache {
    SP<Render::IFramebuffer> fb;
    std::vector<CBox>        rects;
    bool                     clear = false;
};

void applyGlassEffect(SP<Render::IFramebuffer> sampleFramebuffer, SP<Render::IFramebuffer> targetFramebuffer,
                       CBox& rawBox, CBox& transformedBox,
                       float alpha, const std::array<float, 4>& radii, float roundingPower,
                       const Vector2D& paddingRatio, const SResolveContext& resolveContext,
                       const SMaskInfo* mask = nullptr, const Frost::SFrost* frost = nullptr,
                       const std::array<float, 4>* gleam = nullptr,    // active (0..1), the light's place round its lap (0..1, < 0 none), strength
                       const SOutputCache* intoCache = nullptr);

// The kept glass onto the target where this frame is damaged: a plain copy, where
// the glass shader would have computed the same pixels again.
// (filterSetFor: the texture whose filtering was set for this, kept by the caller)
void drawOutputCache(const SP<Render::IFramebuffer>& cache, SP<Render::IFramebuffer> targetFramebuffer, CBox& rawBox, GLuint& filterSetFor);

} // namespace GlassRenderer
