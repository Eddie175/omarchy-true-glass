#pragma once

#include <unordered_map>
#include <string>

inline const std::unordered_map<std::string, const char*> SHADERS = {
    {"glass.frag", R"GLSL(
#version 300 es
precision highp float;

/*
 * Glass fragment shader: thick-glass refraction model
 *
 * The window is modeled as a thick convex glass slab:
 *   - Center: flat surface → clean frosted blur, no distortion
 *   - Edges: curved surface → refraction pulls in content from beyond
 *     the window boundary, creating natural color bleeding
 *
 * Rendering layers:
 * 1. Edge refraction via smooth outward direction (optionally along the edges,
 *    optionally rim-only) + exponential proximity
 * 2. Chromatic aberration (per-channel refraction scale)
 * 3. Edge raw-texture blend for vivid color pickup
 * 4. Subtle center dome lens magnification
 * 5. Frosted tint (brightness boost + desaturation)
 * 6. Configurable color tint overlay
 * 7. Bevel (thin lit line at the edge)
 * 8. Fresnel edge glow (white or tinted by the background)
 * 9. Specular highlight (top)
 * 10. Inner shadow (bottom rim)
 */

uniform sampler2D tex;
uniform vec2 fullSize;
uniform vec2 invFullSize;      // = 1.0 / fullSize, hoisted out of the per-pixel divisions below
uniform vec4 radii;            // per-corner radius: top-left, top-right, bottom-right, bottom-left

uniform float refractionStrength;
uniform float chromaticAberration;
uniform float fresnelStrength;
uniform float specularStrength;
uniform float glassOpacity;
uniform float edgeThickness;
uniform float invBezelWidthPx; // = 1.0 / (edgeThickness * minDim), hoisted per-draw
uniform vec3 tintColor;
uniform float tintAlpha;
uniform float lensDistortion;
uniform float lensMaxPx;       // = lensDistortion * minDim * 0.006, hoisted per-draw
uniform float brightness;
uniform float contrast;
uniform float saturation;
uniform float vibrancy;
uniform float vibrancyDarkness;
uniform float adaptiveDim;
uniform float adaptiveBoost;
uniform float roundingPower;
uniform float invRoundingPower; // = 1.0 / roundingPower, hoisted per-draw
uniform float refractionFlow;
uniform float refractionSpread;
uniform float fresnelTint;
uniform float bevelStrength;
uniform float bevelSize;
uniform float monitorScale;
uniform vec3 fresnelColor;
uniform float fresnelColorAlpha;
uniform vec3 bevelColor;
uniform float bevelColorAlpha;
uniform float bevelTint;
uniform float bevelAngle;
uniform float bevelShadow;
uniform float specularAngle;

// Physical optics (thickness > 0): the glass is a solid of refractive index
// `ior` whose rim follows a convex squircle profile across `bezelPx`.
// Every pixel traces the eye ray through that surface with Snell's law.
uniform float thicknessPx;     // glass height at the flat interior, framebuffer px
uniform float abbe;            // Abbe number: dispersion, 0 = none
uniform float bezelPx;         // width of the curved rim, framebuffer px
uniform float materializeU;    // 0..1: how much of the lens has formed (fade-in progress)
uniform sampler2D presenceTex; // livePresence: this frame's content visibility (presence.frag)
uniform int   livePresence;    // 1: the glass fades (and, closing, shrinks) with presenceTex
uniform float presenceRef;     // the card's settled coverage: the glass is full at it
float materialize;             // materializeU, times the live presence when there is one
uniform int   sdfSource;       // 0 = analytic rounded box, 1 = distance field of the surface's own shape
uniform sampler2D sdfTex;      // sdfSource 1: inside (R) and outside (G) distance in px, text coverage (B), unblurred signed distance (A), over sdfRect
uniform sampler2D styleTex;    // adaptiveStyle: backdrop brightness along the bar (barstyle.frag)
uniform vec4  sdfRect;         // the field's rect in this quad's px
uniform int   adaptiveStyle;   // 1: flip between a light and a dark pane, and the surface's glyphs with it
uniform float shadowOpacity;   // layer glass drop shadow (0 = none)
uniform float shadowRangePx;   // its softness, framebuffer px
uniform vec2  shadowOffsetPx;  // and how far the light above pushes it down
uniform vec4  contentBox;      // contour layers: where anything is drawn, quad px (z < 0: everywhere; z == 0: not measured yet)
uniform vec4  openAnim;        // xy: anchor in this quad's px, zw: scale across, down (1 = settled)
// A card in motion (the morph, motion/morph.hpp): its shape drawn as it is now,
// not its settled field stretched. A rounded rect (x0 y0 x1 y1, quad px) joined
// by a smooth union (neck px) to the bar it comes out of, whose own glass is the
// liquid (the bar draws itself: nothing is drawn inside it); the content scaled
// evenly (never stretched), faded, blurred and cut to the shape.
uniform vec4  morphRect;       // the card's shape now, quad px
uniform vec4  morphSource;     // the bar it comes out of, quad px (empty: none)
uniform vec4  morphCard;       // the settled card it runs to or from, quad px (its field's shape)
uniform float depth;           // 0..1: how thick the pane looks, as a thicker glass behaves: more
                               // pronounced lensing at the rim, a deeper, softer shadow, softer light
uniform float darkGlass;       // 0..1: dark glass (the Glass menu's Tint: Dark)
uniform float lightGlass;      // 0..1: light glass (the Glass menu's Tint: Light)
uniform float cornerMiter;     // 1: square corners are mitred (the two edges' rims meet on the diagonal)
uniform float lightUi;         // 1: a light theme (dark text on light cards)
uniform vec4  paneRect;        // the card's own box, quad px (x1 <= x0: use the glass box)
uniform float outFade;         // the whole result faded (a bar's tooltip fading in, glass and text together)
uniform vec4  morphShape;      // x: rect radius, y: the bar's radius, z: neck px, w: 1 morphing, 2 out of the bar,
                               // 3: the bar's own part of a card's liquid (its shape is the union)
uniform vec2  morphBarLens;    // the bar's glass height and rim px: a card's lens at the seam with it
uniform vec2  morphHand;       // x: how far the shape has handed over to the card's own field (0 drawn, 1 the
                               // field, as settled), y: how joined the card still is to the bar (its tint there)
uniform vec4  barGrade1;       // the bar's grading: brightness, adaptive dim, contrast, saturation (w < 0: none)
uniform vec4  barTint;         // ... its tint rgb, alpha
uniform vec4  barStyle;        // ... its style strip (styleTex) from this quad's px: x offset, 1/w (< 0: none), y offset, 1/h
uniform vec4  contentXform;    // content px = xy + (quad px - zw) / contentFx.x
uniform vec4  contentFx;       // x: content scale, y: its alpha, z: its blur px, w: how far the card overlaps the bar it comes out of, px
// The waist between the bar and a card dripping out of it :
// an hourglass across from the bar to the card at the item, thick at both ends,
// thinning in the middle as it opens less, pinching to a point and leaving two
// stubs that draw back into the bar and the card.
uniform vec4  morphWaist;      // x: along-centre, y: from (in the bar), z: to (in the card), w: open 0..1
uniform float morphWaistBar;   // its half width at the bar: the item it comes out of (0: as wide as at the card)
uniform vec4  morphWaist2;     // x: widest half width px, y: 1 = the bar runs down a side, z: hold (< 1 stays thick
                               // until late), w: the open value it snaps at (0: it melts, never snaps)
// A closing card drawn by the compositor from its last settled frame (heldTex, over
// sdfRect) on its own timeline. x: 0 off, 1 armed (switch to it the frame the content
// starts to fade, read on the GPU), 2 running; y: content alpha; z: glass formed
uniform vec4  ghost;
uniform sampler2D heldTex;
uniform vec4  fillKey;         // rgb: a surface fill colour to see through, a: the alpha it keeps (a < 0: off)
uniform int   fillKeyTop;      // 1: a thin line in the key colour along the bottom edge is drawn along the top edge instead
uniform float textBacking;     // how much of the card's content is still shown (CPU, when no live reading): the text backing follows it
// per-draw constants computed on the CPU (the GPU would redo them every pixel)
uniform vec4  sampleXform;     // backdrop texture uv = box uv * xy + zw
uniform vec3  nRGB;            // refractive index per channel (Cauchy from ior and abbe)
uniform float fresnelF0;       // ((n - 1) / (n + 1))^2
uniform vec2  lightUv;         // direction the rim light comes from, in uv axes (0: straight above)
// The active window (windows only): x how active (eases in and out), y a light
// sweeping across it once on focus (0..1, < 0: none), z strength (0 = off)
uniform vec4  gleam;          // the active window's light: strength, its position in this quad's px (yz), on
uniform float gleamPulse;     // 0..1: the flash when the window gains focus
uniform vec3  gleamShape;     // x: how tightly the light gathers (cosine power), y: the focused rim's own faint line, z: the light's own strength
uniform vec2  bevelLightUv;    // direction of the edge-line light, in uv axes
uniform mat2  screenToUv;      // screen directions (x right, y down) -> this quad's uv axes (monitor transform)
uniform vec3  pressGlow;       // xy: press point in this quad's px, z: glow age 0..1 (< 0 = none)
uniform float pressReachPx;    // how far the glow spreads, framebuffer px
uniform float pressBulge;      // the glass giving under the press: -1..1 of its swell (a kicked spring)

uniform sampler2D maskTex;
uniform int useMask;
uniform vec2 maskUVOffset;
uniform vec2 maskUVScale;
uniform float maskAlphaThreshold;
uniform int maskMode;          // 0 = alpha threshold, 1 = protocol region, 2 = shape box (layer's fixed shape)
uniform int regionRectCount;   // 0..16
uniform vec4 regionRects[16];  // box-local pixels: xy = offset from box top-left, zw = size

// Subsurface items and shape-box layers: the rounded-box SDF (getCornerSDF
// below) is evaluated over this sub-rect of the drawn box instead of the full
// box — the item's blur-region extents, which can be smaller than its own
// surface box (e.g. a capsule pill inside a wider hit-test area), or a layer's
// inset shape. Box-local pixels, same space as regionRects. Windows and
// alpha-mask layers pass offset (0,0) and size == fullSize, so the SDF is
// unchanged from the old whole-box math.
uniform vec2 glassBoxOffsetPx;
uniform vec2 glassBoxSizePx;

in vec2 v_texcoord;
layout(location = 0) out vec4 fragColor;

// ============================================================================
// TEXTURE SAMPLING (window UV -> padded texture UV)
// ============================================================================

// Box UV -> backdrop texture UV (sampleXform): the remap into a sample box
// smaller than this quad (region and contour layers) and the sample's padding,
// folded on the CPU into one multiply-add.
// Frost (Frost.hpp): the same backdrop blurred (half size, same uv), mixed in by
// frost.x, its colour kept vivid (a blur alone greys colours as it averages them,
// so saturation rises with it); frost.y (milk) can also
// calm it (less colour and contrast, a little darker).
uniform sampler2D frostTex;
uniform vec2 frost;            // amount, milk
// Frosted glass is frosted all over, rim and reflection too (a clear band round a
// frosted face read as uneven); frostLocal only lifts it where a card is one with
// the bar, which is clear glass.
float frostLocal = 1.0;
// A card out of the bar sees the bar above it, icons and all, in what is behind
// it: what its rim bent or mirrored from inside the bar showed the bar's own
// glyphs as slivers along the seam. Glass that is one with the bar has the bar's
// backdrop there, not its labels: a sample that lands in the bar is folded back
// out across the bar's edge (a mirror of what is just below it).
// (the same for a settled card that hangs from the bar: switched off as the motion
// ended, its rim picked the bar up again, a step along its top edge after landing)
uniform vec4 foldRect;   // the bar (quad px), x1 <= x0: none
uniform vec2 foldInfo;   // x: 1 the bar runs down a side (fold across x), y: 1 the card is past the bar's far edge
bool foldBar = false;
vec2 barFold(vec2 wuv) {
    vec2 p = wuv * fullSize;
    // (out to whole backdrop px and half a texel past them: at a fractional scale the
    // bar's edge falls inside a px, 69.33 at 2.667x, and that row of the backdrop is
    // part bar; read bilinearly, it drew the bar's edge as a line across the neck
    // where the liquid meets it)
    vec4 s = foldRect + vec4(-1.5, -1.5, 1.5, 1.5);
    if (any(lessThan(p, s.xy)) || any(greaterThan(p, s.zw))) return wuv;
    if (foldInfo.x > 0.5)
        p.x = foldInfo.y > 0.5 ? 2.0 * s.z - p.x : 2.0 * s.x - p.x;
    else
        p.y = foldInfo.y > 0.5 ? 2.0 * s.w - p.y : 2.0 * s.y - p.y;
    return p * invFullSize;
}

vec4 sampleBlurred(vec2 wuv) {
    if (foldBar) wuv = barFold(wuv);
    vec2 t = clamp(wuv * sampleXform.xy + sampleXform.zw, 0.001, 0.999);
    vec4 c = texture(tex, t);
    float fx = frost.x * frostLocal;
    if (fx > 0.001) {
        c = mix(c, texture(frostTex, t), fx);
        float l = dot(c.rgb, vec3(0.2126, 0.7152, 0.0722));
        c.rgb = clamp(mix(vec3(l), c.rgb, 1.0 + 0.35 * fx), 0.0, 1.0);
        // (a blur averages light text on a dark page into a flat grey: frosted
        // glass takes the colour of what is behind it, a little deeper, never a
        // grey or white veil)
        c.rgb *= 1.0 - 0.10 * fx;
        vec3 calm = mix(vec3(l), c.rgb, 0.55) * 0.62 + 0.035;
        c.rgb = mix(c.rgb, calm, fx * frost.y);
    }
    return c;
}

// ============================================================================
// SDF
// ============================================================================

float lpNorm(vec2 v, float p, float invP) {
    // Exact identity: pow(x^2+y^2, 0.5) == length(v) when p == 2.0 (the
    // Hyprland default). Native sqrt is a single correctly-rounded hardware
    // op vs. two pow()s (exp2/log2-based) + a third pow() for the outer root.
    if (p == 2.0) return length(v);
    return pow(pow(abs(v.x), p) + pow(abs(v.y), p), invP);
}

// Quadrant lookup for the per-corner radius. p is measured from the box
// center (as getRoundedBoxSDF/getBevelSDF compute it below): negative y is
// the top half (v_texcoord grows downward — see the specular highlight's
// 1.0 - uv.y), negative x is the left half. cornerRadii is (top-left,
// top-right, bottom-right, bottom-left).
float pickCornerRadius(vec2 p, vec4 cornerRadii) {
    float top    = p.x < 0.0 ? cornerRadii.x : cornerRadii.y;
    float bottom = p.x < 0.0 ? cornerRadii.w : cornerRadii.z;
    return p.y < 0.0 ? top : bottom;
}

// posPx/boxSizePx: pixel-space position relative to (and size of) the box the
// SDF is measured against — the glass box (see glassBoxOffsetPx/SizePx above),
// not necessarily the fragment's full drawn box.
float getRoundedBoxSDF(vec2 posPx, vec2 boxSizePx, vec4 cornerRadii) {
    vec2 p = posPx - boxSizePx * 0.5;
    vec2 halfSize = boxSizePx * 0.5;
    float r = pickCornerRadius(p, cornerRadii);
    float clampedR = min(r, min(halfSize.x, halfSize.y));
    vec2 q = abs(p) - halfSize + clampedR;
    return min(max(q.x, q.y), 0.0) + lpNorm(max(q, 0.0), roundingPower, invRoundingPower) - clampedR;
}

// (the bar, drawing its part of a card's liquid: its outline is the union, so it is
// covered and edge-lit where the liquid joins it; its surface, the lens band along
// its edge, stays its own: the union's took the band away above the neck, and the
// liquid looked to start inside the bar, a few px above its line)
bool barMerge = false;
float edgeOpen = 0.0;    // (the bar: 1 where its edge line opens into the liquid)
float morphSdf(vec2 px);
float barMergeSdf(vec2 px);
float getCornerSDF(vec2 uv) {
    if (barMerge) return barMergeSdf(uv * fullSize);
    vec2 boxLocalPx = uv * fullSize - glassBoxOffsetPx;
    return getRoundedBoxSDF(boxLocalPx, glassBoxSizePx, radii);
}

// Edge distance for the bevel, crease-free. Inside the window, getCornerSDF is
// max(q.x, q.y) - r: the distance to whichever edge is nearer. That has a crease
// along each corner's 45-degree diagonal, so the edge refraction (and anything
// else driven by edgeProximity) shows a straight seam in all four corners.
//
// Here the contour lines are rounded rectangles that match the window outline
// exactly at the edge (depth 0) and get rounder going inward: at depth d the box
// is inset by d and its corner radius is r + d. The radius only grows, so the
// top and side bevels always blend round the corner. Per pixel the depth is a
// closed-form quadratic in the corner zone:
//   |a + 2d| = r + d,  a = |p| - halfSize + r   =>   7d^2 + (4(ax+ay) - 2r) d + |a|^2 - r^2 = 0
// and the plain straight-edge distance elsewhere. Past depth (h - r) / 2, with h
// the smaller half-size, the radius would outgrow the inset box: from there the
// contours are stadiums, i.e. the plain distance to the box rounded by h (so a
// capsule gets its exact distance everywhere).
const float CORNER_FADE = 1.5;   // how far from a square corner's point its bend fades in (of the rim)
const float MITER_P = 4.0;    // a square corner's depth: the 4-norm of the distances to its two edges
// Square corners (cornerMiter): one rule for windows, settled cards and cards in
// motion, so a card that lands is lit as it was moving.
// Depth inside box b (x0 y0 x1 y1, px): a 4-norm of the distances to its two
// nearest edges, so the edges stay straight but the contours round off near the
// diagonal instead of meeting on it (a seam ran in from each corner). Outside: < 0.
float miterDepth(vec2 px, vec4 b) {
    vec2 e = min(px - b.xy, b.zw - px);
    if (e.x <= 0.0 || e.y <= 0.0) return min(e.x, e.y);
    vec2 q = e / min(e.x, e.y);   // (scaled: no overflow far from the corner)
    return min(e.x, e.y) * pow(pow(q.x, -MITER_P) + pow(q.y, -MITER_P), -1.0 / MITER_P);
}
// the bend fades out toward each corner's point, where the two rims meet (full
// there, it gathered the scene behind into a dark curl)
float miterFade(vec2 px, vec4 b, float rim) {
    vec2 e = max(min(px - b.xy, b.zw - px), vec2(0.0));
    return smoothstep(0.0, CORNER_FADE * rim, max(e.x, e.y));
}
float getBevelSDF(vec2 uv) {
    // (a square corner, mitred: each edge's rim runs straight into the corner and the
    // two meet on its diagonal, as on a cut glass block. The rounded contours below
    // close to a point there, and the bend converging on it drew a spike)
    if (cornerMiter > 0.5) {
        // (inside, the depth is a smooth blend of the distances to the two edges, a
        // 4-norm: the edges stay straight, but the contours round off near the
        // diagonal instead of meeting on it, so no seam runs in from the corner)
        float m = miterDepth(uv * fullSize, vec4(glassBoxOffsetPx, glassBoxOffsetPx + glassBoxSizePx));
        return m > 0.0 ? -m : getCornerSDF(uv);
    }
    vec2  H = glassBoxSizePx * 0.5;
    float h = min(H.x, H.y);
    vec2  signedP = uv * fullSize - glassBoxOffsetPx - H;
    float r = min(pickCornerRadius(signedP, radii), h);
    vec2  p = abs(signedP);
    vec2  a = p - H + r;
    float d = min(r - a.x, r - a.y);                      // straight-edge depth
    float B = 4.0 * (a.x + a.y) - 2.0 * r;
    float C = dot(a, a) - r * r;
    float disc = B * B - 28.0 * C;
    if (disc >= 0.0) {
        float dc = (-B + sqrt(disc)) / 14.0;
        if (a.x + 2.0 * dc >= 0.0 && a.y + 2.0 * dc >= 0.0)
            d = dc;                                        // in a corner zone: the rounded contour
    }
    vec2 s = p - H + h;
    return -max(d, h - length(max(s, 0.0)) - min(max(s.x, s.y), 0.0));
}

// ============================================================================
// REFRACTION DIRECTION
// Pixel-space direction toward window center — perfectly smooth everywhere,
// no SDF gradient needed (optional edge-following blend below). On straight edges the perpendicular pixel distance
// dominates, giving approximately edge-normal direction. At corners it
// naturally follows the diagonal.
// ============================================================================

vec2 refractionDir(vec2 uv) {
    vec2 toCenterPx = (vec2(0.5) - uv) * fullSize;
    float len = length(toCenterPx);
    return len > 0.1 ? toCenterPx / len : vec2(0.0);
}

// Smooth edge-following field: points into the box, hugging each edge's normal
// away from the corners and blending crease-free through the diagonals.
vec2 edgeDir(vec2 posPx) {
    vec2 halfSize = fullSize * 0.5;
    vec2 n = abs(posPx) / halfSize;
    vec2 g = sign(posPx) * pow(n, vec2(7.0)) / halfSize;
    float len = length(g);
    return len > 0.0 ? -g / len : vec2(0.0);   // points INTO the box
}

// light direction for a clockwise angle in degrees, 0 = from the top (screen y grows downward)
vec2 lightDir(float angleDeg) {
    float a = radians(angleDeg);
    return vec2(sin(a), -cos(a));
}

// exact outward normal from the SDF gradient; only meaningful right at the edge
vec2 sdfOutwardNormal(vec2 uv) {
    vec2 h = vec2(1.0) / fullSize;
    vec2 grad = vec2(
        getCornerSDF(uv + vec2(h.x, 0.0)) - getCornerSDF(uv - vec2(h.x, 0.0)),
        getCornerSDF(uv + vec2(0.0, h.y)) - getCornerSDF(uv - vec2(0.0, h.y))
    );
    float len = length(grad);
    return len > 0.0 ? grad / len : vec2(0.0, -1.0);
}

// ============================================================================
// PHYSICAL OPTICS
// Height profile across the rim: t = 0 at the edge, 1 where the flat top
// begins. Convex y = (1 - (1 - t)^3)^(1/3): vertical at the edge, curving down
// to the flat top over the whole band. (The squircle's fourth power bent nearly
// all of the view in the band's outer third and stopped short: its inner end read
// as a line across the glass; this one tapers out.)
// ============================================================================

float rimHeight(float t) {
    float s = 1.0 - t;
    return pow(max(1.0 - s * s * s, 0.0), 1.0 / 3.0);            // (1 - s^3)^(1/3)
}

float rimSlope(float t) {
    float s = 1.0 - t;
    float g = max(1.0 - s * s * s, 1e-4);
    return s * s * pow(g, -2.0 / 3.0);                           // s^2 * g^(-2/3)
}

// The morph's shape: signed distance (negative inside), px. Inside, the depth
// follows rounded contours (as getBevelSDF: no seams along the corner diagonals),
// so the lens over a growing card is as smooth as over a settled one.
float bevelBoxSdf(vec2 p, vec4 r, float rad) {
    vec2  H = max((r.zw - r.xy) * 0.5, vec2(0.0));
    vec2  P = abs(p - (r.xy + r.zw) * 0.5);
    float h = min(H.x, H.y);
    rad = min(rad, h);
    vec2  q = P - H + rad;
    float outside = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - rad;
    if (outside > 0.0) return outside;
    if (cornerMiter > 0.5 && rad < 0.5) return -miterDepth(p, r);
    float d = min(rad - q.x, rad - q.y);
    float B = 4.0 * (q.x + q.y) - 2.0 * rad, C = dot(q, q) - rad * rad, disc = B * B - 28.0 * C;
    if (disc >= 0.0) { float dc = (-B + sqrt(disc)) / 14.0; if (q.x + 2.0 * dc >= 0.0 && q.y + 2.0 * dc >= 0.0) d = dc; }
    vec2 sP = P - H + h;
    return -max(d, h - length(max(sP, 0.0)) - min(max(sP.x, sP.y), 0.0));
}
// a settled card's signed depth (+ inside): its unblurred outline, and under
// cornerMiter the square-corner depth of its own box (paneRect) where that is less
float cardSigned(vec2 px) {
    vec2  f = (px - sdfRect.xy) / sdfRect.zw;
    float a = textureLod(sdfTex, f, 0.0).a;
    return paneRect.z > paneRect.x ? min(a, miterDepth(px, paneRect)) : a;
}
float smin(float a, float b, float k) {
    if (k <= 0.01) return min(a, b);
    float h = clamp(0.5 + 0.5 * (b - a) / k, 0.0, 1.0);
    return mix(b, a, h) - k * h * (1.0 - h);
}
// The waist: signed distance (approximate: the half width's slope folded in) and
// its half width here (for the rim). 1e4 where there is none.
float waistHalfHere = 0.0;
float waistSdf(vec2 px) {
    float H = morphWaist2.x, n = morphWaist.w;
    waistHalfHere = 0.0;
    if (H <= 0.5 || n <= 0.002) return 1e4;
    vec2  q  = morphWaist2.y > 0.5 ? px.yx : px;
    float u  = abs(q.x - morphWaist.x);
    float y0 = min(morphWaist.y, morphWaist.z), y1 = max(morphWaist.y, morphWaist.z);
    if (y1 - y0 < 2.0) return 1e4;   // (no length left: the card is home)
    float L  = y1 - y0;
    float hEnd = H * (0.4 + 0.6 * n);
    // (at the bar, the item's own width: a funnel from a small icon, not an hourglass
    // over its neighbours; the bar end is `from`, morphWaist.y)
    bool  barLow = morphWaist.y < morphWaist.z;
    float hBar   = morphWaistBar > 0.5 ? min(hEnd, morphWaistBar * (0.6 + 0.4 * n)) : hEnd;
    float w, dw;   // half width at q.y and its slope
    float cap;
    float snapAt = morphWaist2.w, hold = max(morphWaist2.z, 0.05);
    if (n >= snapAt) {
        // open: an hourglass, its middle thinning as it opens less (hold < 1: it
        // keeps its width until late, then pinches quickly)
        float r    = snapAt > 0.0 ? (n - snapAt) / (1.0 - snapAt) : n;
        float hMid = min(H * 0.55 * pow(clamp(r, 0.0, 1.0), hold), hBar);
        // (-1 .. 1 across, and held there past its ends: the profile kept growing
        // out there, so just past the waist its distance was the same flat value as
        // the bar's across the whole width and the smooth union filled a strip
        // along the bar: the tails, wings and slivers)
        float s2 = clamp(2.0 * (q.y - y0) / L - 1.0, -1.0, 1.0);
        float hE = (s2 < 0.0) == barLow ? hBar : hEnd;
        w   = hMid + (hE - hMid) * s2 * s2;
        dw  = abs(s2) < 1.0 ? (hE - hMid) * 2.0 * s2 * 2.0 / L : 0.0;
        cap = max(y0 - q.y, q.y - y1);
        if (snapAt <= 0.0) { w *= min(1.0, n * 4.0); dw *= min(1.0, n * 4.0); }   // (melting: the whole waist goes)
    } else {
        // snapped: two stubs, each half the waist, shrinking into its end (eased
        // out: the bar's own edge reforms gently instead of in two frames)
        float g   = sqrt(n / snapAt);
        bool  top = q.y - y0 < y1 - q.y;                  // nearer y0's end
        float hb  = (top == barLow ? min(H * (0.4 + 0.6 * snapAt), max(morphWaistBar, 0.0) > 0.5 ? morphWaistBar : 1e4) : H * (0.4 + 0.6 * snapAt)) * g;
        float len = 0.5 * L * g;
        float t   = clamp(top ? (q.y - y0) / max(len, 1e-3) : (y1 - q.y) / max(len, 1e-3), 0.0, 1.0);
        float r   = 1.0 - t;
        w   = hb * r * r;
        dw  = t > 0.0 && t < 1.0 ? 2.0 * hb * r / max(len, 1e-3) : 0.0;
        cap = top ? max(y0 - q.y, q.y - (y0 + len)) : max((y1 - len) - q.y, q.y - y1);
    }
    waistHalfHere = w;
    // (the slope folded in, at most 2: a short waist's slope is huge, which flattened
    // the distance to nothing along a whole row, a thin line far out to the sides)
    return max((u - w) * inversesqrt(1.0 + min(dw * dw, 4.0)), cap);
}
// the card, the waist and the bar as one liquid: smooth unions (polynomial smooth
// min), so their joins flow into each other instead of meeting at a corner
// Landing: the drawn shape hands over to the card's own measured outline (its
// field, which the settled card is drawn with), so the frame after the motion
// ends is the frame before it; leaving, the other way. (A switch at the end
// moved every edge pixel and its light at once: a snap after it had landed.)
float morphHandOver(vec2 px, float d) {
    if (morphHand.x <= 0.001 || sdfSource != 1) return d;
    // (the field as the card is now: its outline carried to the drawn rect. Mixed
    // as it lies, a full-size outline with a shrinking one, the in-between shapes
    // were chamfered boxes: straight shoulders on a closing card)
    vec2 cs = max(morphCard.zw - morphCard.xy, vec2(1.0)), rs = max(morphRect.zw - morphRect.xy, vec2(1.0));
    vec2 k  = cs / rs;
    vec2 cp = morphCard.xy + (px - morphRect.xy) * k;
    vec2 f = (cp - sdfRect.xy) / sdfRect.zw;
    if (any(lessThan(f, vec2(0.0))) || any(greaterThan(f, vec2(1.0)))) return d;
    vec4 fd = textureLod(sdfTex, f, 0.0);
    // (mitred: the outline the settled card is drawn with is the unblurred one; the
    // blurred one rounded the corners for the frame between landing and rest)
    float fs = cornerMiter > 0.5 ? -min(fd.a, miterDepth(cp, morphCard)) : fd.g - fd.r;
    return mix(d, fs / max(k.x, k.y), morphHand.x);
}
const float MERGE_NEAR = 2.0, MERGE_FAR = 4.5;   // logical px of gap: joined below, apart above (a settled card rests ~5.3 off its bar: never joined)
// 1 inside the neck that joins a card to the bar: it is the bar's glass flowing out,
// its lens and tone the bar's whatever the join's strength (a thinning neck took the
// card's thicker lens while still at the bar, and saw much further: a dark neck
// under the bar's light last rows, the bar's line drawn across where they meet); it
// shrinks to nothing as the neck pinches off, so nothing pops
float inNeck(vec2 px) {
    float h = waistHalfHere;
    float k = 1.0 - smoothstep(-1.0 * monitorScale, 1.0 * monitorScale, waistSdf(px));
    waistHalfHere = h;
    return k;
}
float morphSdf(vec2 px) {
    // the card: its own outline carried to the rect once the rect is near its size
    // (handed over while it still moves fast), the analytic box while it is a drop;
    // the waist and the bar join that
    float a = morphHandOver(px, bevelBoxSdf(px, morphRect, morphShape.x));
    if (morphSource.z <= morphSource.x || morphSource.w <= morphSource.y) return a;
    float b = bevelBoxSdf(px, morphSource, morphShape.y);
    float k = morphShape.z;
    // The waist flows into the bar and into the card. The card flows into the bar
    // directly only while it overlaps it (pushing out, or going back in): joined
    // along its whole top edge across the few px it hangs under the bar, the gap
    // became a sliver of thick glass, a sheared band (and a blend that faded with
    // distance from the waist left thin tails and ears)
    float w  = waistSdf(px);
    float u;
    // The card and the bar are one liquid whenever the card is at the bar (touching,
    // inside it, or within a meniscus of it): its sides flare into the bar's edge.
    // (Joined only once it was well inside, a card at the bar's edge merely touched
    // it: the bar drew its own edge straight across the card's top, two pieces.)
    // (a drop meets the bar only once it is close: further off, two drops apart)
    float kc = max(k, 16.0 * monitorScale) * (1.0 - smoothstep(MERGE_NEAR * monitorScale, MERGE_FAR * monitorScale, -contentFx.w));   // contentFx.w: overlap px (< 0: a gap)
    // (and only while some of the card is still outside the bar: a smooth union
    // with a card just inside the bar's edge bulged that edge out a px or two, which
    // vanished in one frame when the bar let go: its bottom line snapping back)
    vec4  q    = morphSource;
    float prot = max(max(morphRect.z - q.z, q.x - morphRect.x), max(morphRect.w - q.w, q.y - morphRect.y));
    kc *= smoothstep(0.0, 8.0 * monitorScale, prot);
    u = min(smin(smin(a, w, k), b, kc), smin(w, b, k));
    return u;
}
// the rim the morph's shape allows here: each part's own half thickness (a thin
// part given the card's rim bent the view off screen, a black stroke), blended
// across the neck as the union blends them
float morphRimHere(vec2 px) {
    vec4  r  = morphRect, q = morphSource;
    float hr = 0.5 * min(r.z - r.x, r.w - r.y);
    if (q.z <= q.x || q.w <= q.y) return hr;
    float hs = 0.5 * min(q.z - q.x, q.w - q.y);
    float a = bevelBoxSdf(px, r, morphShape.x), b = bevelBoxSdf(px, q, morphShape.y);
    float k = max(morphShape.z, 0.01);
    float rim = mix(hs, hr, clamp(0.5 + 0.5 * (b - a) / k, 0.0, 1.0));
    // (in the waist: its own half width, a slender lens)
    float w = waistSdf(px);
    return w < min(a, b) ? max(waistHalfHere, 1.0) : rim;
}
bool morphing = false;
float wBar = 0.0;        // (out of the bar: how near the bar's edge this pixel of the card is, 1 at it)

// Signed distance to the glass outline (negative inside) for the analytic shape.
// the bar drawing its part of the liquid: the union, handed back to its own box as
// the card hands over to its settled outline (they finish together)
float barMergeSdf(vec2 px) {
    return morphSdf(px);
}
// The bar joined by a card: one surface with the liquid where they meet. Along its
// edge line the bar's surface is the liquid's (the union's), so nothing shows where
// they meet; from there up it eases back into the bar's own band within the band's
// height, so the band runs on at full height, its top edge straight (the union's
// band dipped into the bar round the joint, and the liquid looked to start a few px
// above the line; the bar's own band to its last row drew its edge across the neck,
// a bright line over the liquid). Only where the edge opens into the liquid (open).
// (open where the liquid is right past the bar's edge, straight across from here:
// measured how far the union reaches past the edge, a thinning neck kept the bar's
// rim across its top, a faint line, until it was gone)
float barOpenAt(vec2 px) {
    vec2  lo = glassBoxOffsetPx, hi = glassBoxOffsetPx + glassBoxSizePx;
    vec2  card = 0.5 * (morphRect.xy + morphRect.zw), mid = 0.5 * (lo + hi);
    float k = 1.5 * monitorScale;
    vec2  q = glassBoxSizePx.x >= glassBoxSizePx.y ? vec2(px.x, card.y > mid.y ? hi.y + k : lo.y - k)
                                                   : vec2(card.x > mid.x ? hi.x + k : lo.x - k, px.y);
    return 1.0 - smoothstep(-0.75 * monitorScale, 0.25 * monitorScale, barMergeSdf(q));
}
float barSurfaceSdf(vec2 px) {
    float sB = getRoundedBoxSDF(px - glassBoxOffsetPx, glassBoxSizePx, radii);
    float sU = barMergeSdf(px);
    float open = barOpenAt(px);
    float band = 0.5 * min(glassBoxSizePx.x, glassBoxSizePx.y);
    float v = open * (1.0 - smoothstep(0.0, band, -sB));
    return mix(sB, sU, v);
}
// the surface's own shape (a bar joined by a card: see barSurfaceSdf)
float shapeSdf(vec2 uv) {
    if (barMerge) return barSurfaceSdf(uv * fullSize);
    return roundingPower == 2.0 ? getBevelSDF(uv) : getCornerSDF(uv);
}

// How fast the distance actually changes per px where the last shapeNormal() ran
// (1 for a true distance). The card field is smoothed to round its corner creases,
// so in a corner it changes slower: bands measured in it (the edge line, the lit
// arc, where reflections start) spread there into a soft white smear. Dividing by
// this keeps them as thin round the corner as along the edges.
float distanceGain = 1.0;

// The outward unit normal of the outline (a 4-tap gradient: only computed where
// the rim or the edge line uses it, never on the flat top).
// How much the outline's direction can be trusted here (the morph's analytic
// shape): at the cusps where the waist meets the bar's edge and the card's top,
// the distance folds and its gradient shrinks and swings from pixel to pixel; the
// lens there bent the view into radial streaks (a starburst at the pinch).
float normalTrust = 1.0;
// A moving card already at rest on its own outline (at its own size, apart from the
// bar): drawn exactly as the settled card is, so the frame it settles in is identical
bool restingMorph = false;
// a square-cornered glass's own box: the shape in motion, a settled card's, a window's
vec4 miterBox() {
    return morphing ? morphRect
         : sdfSource == 1 && paneRect.z > paneRect.x ? paneRect
         : vec4(glassBoxOffsetPx, glassBoxOffsetPx + glassBoxSizePx);
}
// where the content is sampled for this pixel (set with surfacePixel), for the text
// backing's look at the text around it
vec2 g_cpx = vec2(0.0), g_maskUV = vec2(0.0), g_uv = vec2(0.0);
bool g_held = false;
// How far, along a rounded box's outline, the point nearest p is from s0: both
// measured clockwise from where the top edge's straight part begins, the
// difference wrapped round the lap
float cometDistance(vec2 p, vec2 size, float s0, bool twin) {
    float R = min(pickCornerRadius(p - 0.5 * size, radii), 0.5 * min(size.x, size.y));
    float W = size.x, H = size.y, A = 1.5707963 * R, a = W - 2.0 * R, b = H - 2.0 * R;
    float P = 2.0 * (a + b) + 4.0 * A;
    float s;
    bool  L = p.x < R, Rt = p.x > W - R, T = p.y < R, B = p.y > H - R;
    if (Rt && T)      s = a + (atan(p.y - R, p.x - (W - R)) + 1.5707963) * R;
    else if (Rt && B) s = a + A + b + atan(p.y - (H - R), p.x - (W - R)) * R;
    else if (L && B)  s = 2.0 * a + 2.0 * A + b + (atan(p.y - (H - R), p.x - R) - 1.5707963) * R;
    else if (L && T)  { float t = atan(p.y - R, p.x - R); s = 2.0 * a + 3.0 * A + 2.0 * b + ((t < 0.0 ? t + 6.2831853 : t) - 3.1415927) * R; }
    else {
        // a straight part: the nearest edge
        float dt = p.y, dr = W - p.x, db = H - p.y, dl = p.x, m = min(min(dt, dr), min(db, dl));
        if (m == dt)      s = p.x - R;
        else if (m == dr) s = a + A + (p.y - R);
        else if (m == db) s = a + 2.0 * A + b + (W - R - p.x);
        else              s = 2.0 * a + 3.0 * A + b + (H - R - p.y);
    }
    float d = mod(s - s0 + 0.5 * P, P) - 0.5 * P;
    // (a twin half a lap on: the nearer of the two)
    return twin ? min(abs(d), abs(mod(s - s0, P) - 0.5 * P)) : d;
}
vec2 shapeNormal(vec2 uv) {
    vec2 g;
    // (a moving card wholly on its own outline, at its own size, apart from the bar:
    // the same normals as the settled card, so the frame it settles in is identical)
    bool settledLike = restingMorph;
    if (morphing && !settledLike) {
        vec2 px = uv * fullSize;
        g = vec2(morphSdf(px + vec2(1.0, 0.0)) - morphSdf(px - vec2(1.0, 0.0)),
                 morphSdf(px + vec2(0.0, 1.0)) - morphSdf(px - vec2(0.0, 1.0)));
        distanceGain = clamp(length(g) / 2.0, 0.35, 1.0);   // (taps 2 px apart)
        if (morphSource.z > morphSource.x) normalTrust = smoothstep(0.55, 0.9, length(g) / 2.0);   // (the cusps are at the waist)
    } else if (sdfSource == 1) {
        vec2 f = (uv * fullSize - sdfRect.xy) / sdfRect.zw;
        vec2 e = 1.5 / sdfRect.zw;
        // the signed distance (inside - outside): unlike the inside channel alone it
        // isn't clamped at the outline, so the outermost rim pixels get a true normal
        const vec2 sg = vec2(1.0, -1.0);
        g = -vec2(dot(textureLod(sdfTex, f + vec2(e.x, 0.0), 0.0).rg - textureLod(sdfTex, f - vec2(e.x, 0.0), 0.0).rg, sg),
                  dot(textureLod(sdfTex, f + vec2(0.0, e.y), 0.0).rg - textureLod(sdfTex, f - vec2(0.0, e.y), 0.0).rg, sg));
        // (square corners: the normals of the depth the card is drawn with)
        if (cornerMiter > 0.5) {
            vec2 px = uv * fullSize;
            g = -vec2(cardSigned(px + vec2(1.5, 0.0)) - cardSigned(px - vec2(1.5, 0.0)),
                      cardSigned(px + vec2(0.0, 1.5)) - cardSigned(px - vec2(0.0, 1.5)));
        }
        distanceGain = clamp(length(g) / 3.0, 0.35, 1.0);   // (taps 3 px apart)
    } else {
        vec2 e = invFullSize;
        g = vec2(shapeSdf(uv + vec2(e.x, 0.0)) - shapeSdf(uv - vec2(e.x, 0.0)),
                 shapeSdf(uv + vec2(0.0, e.y)) - shapeSdf(uv - vec2(0.0, e.y)));
        distanceGain = clamp(length(g) / 2.0, 0.35, 1.0);   // (taps 2 px apart)
        if (barMerge) normalTrust = smoothstep(0.55, 0.9, length(g) / 2.0);
    }
    float l2 = dot(g, g);
    return l2 > 1e-10 ? g * inversesqrt(l2) : vec2(0.0);
}

// Where the eye ray through this pixel lands on the backdrop, in px: refract
// the straight-down ray at the top surface (air -> glass, Snell's law), then
// follow it down through the glass height z to the backdrop plane.
// The layer's fill colour seen through (fill_key): premultiplied in and out.
vec4 keyFill(vec4 px) {
    float surfA = px.a;
    if (fillKey.a < 0.0 || surfA <= 0.001) return px;
    vec3 surfRGB = px.rgb / surfA;
    if (fillKeyTop > 0) {
        // (the bar: its key is the open item's accent line, never under anything)
        float k = 1.0 - smoothstep(0.03, 0.09, distance(surfRGB, fillKey.rgb));
        surfA = mix(surfA, min(surfA, fillKey.a), k);
    } else {
        // A client that paints its card (or a button on it) in the fill colour:
        // that colour becomes the glass's tint, and what is drawn on it stays as it
        // is, its antialiased edges included. Each pixel is split into the fill and
        // what lies over it ("colour to alpha": the least opaque colour that, over
        // the fill, gives this pixel): keyed as a whole colour, a button's outline
        // and its labels' edges, part fill, part white, kept the fill's darkness
        // and stood out as dark, dotted rims round every chip.
        vec3  K  = fillKey.rgb;
        vec3  up = max(surfRGB - K, 0.0) / max(1.0 - K, vec3(1e-3));
        vec3  dn = max(K - surfRGB, 0.0) / max(K, vec3(1e-3));
        float af = clamp(max(max(max(up.r, up.g), up.b), max(max(dn.r, dn.g), dn.b)), 0.0, 1.0);
        // (a shade a hair off the fill, as a client's own blending leaves it, is the
        // fill: split, it became a faint black or white film over the whole card)
        af *= smoothstep(0.02, 0.06, distance(surfRGB, K));
        vec3  fg = af > 1e-3 ? K + (surfRGB - K) / af : K;
        float fgA = surfA * af;
        float kA  = min(surfA * (1.0 - af), fillKey.a);
        float outA = fgA + kA * (1.0 - af);
        surfRGB = outA > 1e-4 ? (fg * fgA + K * kA * (1.0 - af)) / outA : K;
        surfA   = outA;
    }
    return vec4(surfRGB * surfA, surfA);
}

vec2 refractOffsetPx(vec3 N, float n, float z, float capPx) {
    vec3 T = refract(vec3(0.0, 0.0, -1.0), N, 1.0 / n);
    if (T.z > -1e-3) return vec2(0.0);
    // At most half the rim: at a corner both edges' bends add up and the view
    // folded back on itself (a dark U-shaped smear); straight edges stay below it.
    vec2 o = T.xy * (z / -T.z);
    float l = length(o);
    // (eased into the cap, not clipped at it: clipped, the bend went from growing to
    // flat in one pixel, a crease that ran along the glass inside the rim)
    return l > 1e-4 ? o * (capPx * tanh(l / max(capPx, 1e-3)) / l) : o;
}

// The content of a card in motion at content px cpx: the held settled card
// (closing) or the live surface (opening); nothing outside it.
vec4 contentTap(vec2 cpx, bool fromHeld) {
    if (fromHeld) {
        vec2 f = (cpx - sdfRect.xy) / sdfRect.zw;
        if (any(lessThan(f, vec2(0.0))) || any(greaterThan(f, vec2(1.0)))) return vec4(0.0);
        return texture(heldTex, f);
    }
    vec2 cuv = cpx * invFullSize;
    if (any(lessThan(cuv, vec2(0.0))) || any(greaterThan(cuv, vec2(1.0)))) return vec4(0.0);
    return texture(maskTex, cuv * maskUVScale + maskUVOffset);
}

// ... softened by its blur (a disc of taps, Vogel's spiral: even at any radius)
vec4 contentAt(vec2 cpx, bool fromHeld, float blurPx) {
    if (blurPx < 0.5) {
        // (drawn at a size not its own: a pixel covers 1/scale content px, sampled
        // 2x2 across it; one bilinear tap made fine patterns, a weather radar's
        // dither dots, shimmer in moire as the scale changed)
        float s = contentFx.x;
        if (abs(s - 1.0) < 0.004) return contentTap(cpx, fromHeld);
        float o = 0.25 / max(s, 0.05);
        return 0.25 * (contentTap(cpx + vec2(-o, -o), fromHeld) + contentTap(cpx + vec2(o, -o), fromHeld) +
                       contentTap(cpx + vec2(-o, o), fromHeld) + contentTap(cpx + vec2(o, o), fromHeld));
    }
    vec4 c = contentTap(cpx, fromHeld);
    for (int i = 0; i < 12; i++) {
        float a = float(i) * 2.39996, r = sqrt((float(i) + 0.5) / 12.0) * blurPx;
        c += contentTap(cpx + vec2(cos(a), sin(a)) * r, fromHeld);
    }
    return c / 13.0;
}

// The content's alpha d px from this pixel, from where this pixel's content comes
float surfAlphaAt(vec2 d) {
    if (morphing) return contentTap(g_cpx + d / max(contentFx.x, 1e-3), g_held).a * contentFx.y;
    if (g_held)   return texture(heldTex, clamp((g_uv * fullSize + d - sdfRect.xy) / sdfRect.zw, 0.001, 0.999)).a * ghost.y;
    return texture(maskTex, clamp(g_maskUV + d * invFullSize * maskUVScale, 0.001, 0.999)).a;
}

// A shadow's colour: what it falls on with less light on it, not black laid over
// it (black read as a grey smudge on a coloured picture). The backdrop here,
// deeper and a little richer, as a shade falls in daylight; composited at the
// shadow's strength, so the strongest shadow keeps about a third of the light.
vec3 shadeColor(vec2 uv) {
    vec3 b = texture(tex, clamp(uv * sampleXform.xy + sampleXform.zw, 0.001, 0.999)).rgb;
    float l = dot(b, vec3(0.2126, 0.7152, 0.0722));
    return clamp(mix(vec3(l), b, 1.5), 0.0, 1.0) * 0.32;
}

// Dark glass, smoked: what is seen through it is taken down, the brighter the more
// (a white backdrop to about 40 %, so light text on it reads; a dark one barely,
// so it doesn't go black), its colour kept. The bar's text tone is chosen from
// the same darkened brightness, so its glyphs stay light on it.
float darkGain(float l) { return 1.0 - darkGlass * 0.58 * (0.35 + 0.65 * smoothstep(0.1, 0.85, l)); }
// The menu's Tint. Dark: smoked, the brighter the backdrop the more (light text on a
// bright wallpaper). Light: milky white, the darker the backdrop the more, so all of it
// lands in the light range and dark text reads on it (a light theme); a light backdrop
// stays nearly as it is, still glass.
// (lightBase: frosted glass and windows take more milk. Frosted, the backdrop's own
// contrast is what showed: a window behind a menu read as a grey block, its edge bent
// by the rim into a soft line; a light theme's window is its text's page.)
float lightBase = 0.25;
float lightLift(float l) { return lightGlass * min(lightBase + 0.4 * (1.0 - smoothstep(0.3, 0.85, l)), 0.85); }
vec3 glassTone(vec3 c) {
    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
    c *= darkGain(l);
    return mix(c, vec3(1.0), lightLift(dot(c, vec3(0.2126, 0.7152, 0.0722))));
}
float glassToneOf(float v) { v *= darkGain(v); return mix(v, 1.0, lightLift(v)); }

// Soft drop shadow of the glass outside its outline: the outline's outside
// distance, displaced away from the light. 0 where there's no shadow.
float dropShadow(vec2 uv) {
    if (shadowOpacity <= 0.0) return 0.0;
    vec2 q = uv - (screenToUv * shadowOffsetPx * (1.0 + 1.2 * depth)) * invFullSize;
    float dOut;
    if (morphing)   // (out of the bar: the card's own shadow; the bar casts none here)
        dOut = max(morphShape.w > 1.5 ? morphHandOver(q * fullSize, bevelBoxSdf(q * fullSize, morphRect, morphShape.x)) : morphSdf(q * fullSize), 0.0);
    else if (sdfSource == 1) {
        vec2 f = (q * fullSize - sdfRect.xy) / sdfRect.zw;
        dOut = any(lessThan(f, vec2(0.0))) || any(greaterThan(f, vec2(1.0))) ? 1e4 : texture(sdfTex, f).g;
    }
    else                dOut = max(barMerge ? barMergeSdf(q * fullSize) : shapeSdf(q), 0.0);
    float k = 1.0 - smoothstep(0.0, shadowRangePx * (1.0 + 0.6 * depth), dOut);
    // (a card hanging from the bar casts none at the joint: it darkened the half
    // covered pixels where its sides flare into the bar, a dark dot either side)
    // (and none up onto the bar once it has landed either: there it came back in one
    // frame as the motion ended, darkening the bar's edge, a late pop)
    if (foldBar)
        k *= smoothstep(0.0, 20.0 * monitorScale, bevelBoxSdf(uv * fullSize, foldRect, 0.0));
    return shadowOpacity * (1.0 + 0.5 * depth) * k * k;
}

// ============================================================================
// MAIN — Thick-glass refraction model
// ============================================================================

void main() {
    // A card that just appeared scales up from its anchor (where it was opened
    // from): its shape and content are drawn shrunk, while what is behind the
    // glass stays where it is (uvBg).
    vec2 uvBg = v_texcoord;
    vec2 uv = v_texcoord;
    if (contentBox.z == 0.0 && contentBox.w > 0.0) {
        // contour layer not measured yet: hidden for its first frames (w 2: its
        // open runs from when it appeared), else its surface as it is, no glass
        if (contentBox.w > 1.5) discard;
        fragColor = (texture(maskTex, clamp(uv * maskUVScale + maskUVOffset, 0.001, 0.999))) * outFade;
        return;
    }
    if (contentBox.z > 0.0) {
        // nothing is drawn out here: no glass, no shadow, nothing to composite
        vec2 px = uvBg * fullSize;
        if (any(lessThan(px, contentBox.xy)) || any(greaterThan(px, contentBox.xy + contentBox.zw))) discard;
    }
    // a closing card: the glass follows the content's visibility in this very frame
    // (eased: the glass stays a little ahead of the text, so text never floats
    // without it, and both still reach nothing together)
    morphing     = morphShape.w > 0.5 && morphShape.w < 2.5;
    barMerge     = morphShape.w > 2.5;
    restingMorph = morphing && morphHand.x > 0.999 && abs(contentFx.x - 1.0) < 0.004 && contentFx.y > 0.999 && contentFx.z < 0.5 &&
                   (morphSource.z <= morphSource.x || (morphWaist.w <= 0.002 && -contentFx.w > MERGE_FAR * monitorScale)) &&
                   all(lessThan(abs(morphRect - morphCard), vec4(0.5)));
    foldBar      = !barMerge && foldRect.z > foldRect.x && foldRect.w > foldRect.y;
    // (0.95 and up is fully there: the settled card's own reference is read a
    // little after it lands, and a reading of 0.98 shrank the glass by a percent in
    // one step; a real fade drops far below that within a frame or two)
    float rawP   = livePresence == 1 && !morphing ? clamp(texture(presenceTex, vec2(0.5)).r / presenceRef / 0.95, 0.0, 1.0) : 1.0;
    // the held card replaces the client's fading frames from the first one (below
    // 98.5 % of the settled card: the same line the held copy is taken above, so a
    // barely faded first frame is never both shown and kept as the card)
    float presRatio = livePresence == 1 && !morphing ? texture(presenceTex, vec2(0.5)).r / max(presenceRef, 1e-4) : 1.0;
    bool  held   = ghost.x > 1.5 || (ghost.x > 0.5 && presRatio < 0.985);
    g_held = held;
    // (armed, the held copy standing in, whole, for a fade the CPU hasn't seen yet:
    // the card stays as it was until the close takes it over)
    bool  armedHeld = held && ghost.x < 1.5;
    float liveP  = morphing || armedHeld ? 1.0 : held ? ghost.z : (livePresence == 1 ? pow(rawP, 0.6) : 1.0);
    // (a held card keeps its whole lens and light while it goes: the object fades,
    // glass and shadow together, it never flattens into a tinted pane)
    materialize  = materializeU * (held && !morphing && !armedHeld ? 1.0 : liveP);
    float objFade = held && !morphing ? ghost.z : 1.0;
    // (armed: the held card at rest until the close starts on the CPU; a morph is
    // never stretched: its shape is drawn as it is)
    vec2 scale2  = morphing ? vec2(1.0)
                 : held ? (ghost.x > 1.5 ? openAnim.zw : vec2(1.0))
                        : (livePresence == 1 ? mix(openAnim.zw, vec2(1.0), liveP) : openAnim.zw);
    // (both ways: a spring's overshoot takes it a little past 1)
    if (any(greaterThan(abs(scale2 - 1.0), vec2(1e-4)))) {
        vec2 a = openAnim.xy * invFullSize;
        uv = a + (uv - a) / max(scale2, vec2(1e-3));
        // (shrunk past the quad: nothing of the card is here)
        if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) discard;
    }
    float textFade = held && !morphing ? ghost.y : 1.0;

    // Layers only: sample the temp FBO to get the rendered surface pixel.
    // Discard fully transparent fragments so glass only covers visible content.
    // For windows, hasMask is false and this block is skipped entirely.
    vec4 surfacePixel = vec4(0.0);
    // (the content's own colour at this point, unblended: a morph draws it at a size
    // not its own, and a coloured dot blended with the card's dark fill read as grey)
    float classChroma = -1.0;
    float contourSigned = 0.0, contourInside = 0.0, contourText = 0.0, contourCover = 0.0;
    bool hasMask = (useMask == 1);
    if (hasMask) {
        vec2 maskUV = uv * maskUVScale + maskUVOffset;
        g_maskUV = maskUV;
        g_uv     = uv;
        vec2 cpx = vec2(0.0);
        if (morphing) {
            cpx = contentXform.xy + (uv * fullSize - contentXform.zw) / max(contentFx.x, 1e-3);
            g_cpx = cpx;
            surfacePixel = contentAt(cpx, held, contentFx.z) * contentFx.y;
            vec4 np = held ? texelFetch(heldTex, ivec2(clamp(cpx - sdfRect.xy, vec2(0.0), sdfRect.zw - 1.0)), 0)
                           : texture(maskTex, clamp(cpx * invFullSize, 0.0, 1.0) * maskUVScale + maskUVOffset);
            if (np.a > 0.02) {
                vec3 nc = np.rgb / np.a;
                classChroma = max(nc.r, max(nc.g, nc.b)) - min(nc.r, min(nc.g, nc.b));
            }
        } else
            surfacePixel = held ? texture(heldTex, clamp((uv * fullSize - sdfRect.xy) / sdfRect.zw, 0.001, 0.999)) * ghost.y
                                : texture(maskTex, clamp(maskUV, 0.001, 0.999));

        if (morphing) {
            // (inside the bar it comes out of, the bar draws itself)
            vec2 bp = uv * fullSize;
            if (morphShape.w > 1.5 && all(greaterThanEqual(bp, morphSource.xy)) && all(lessThan(bp, morphSource.zw))) discard;
            // the shape as it is now; the content cut to it (with the same
            // antialiased edge: it never pokes out of a shape still growing)
            float sd = morphSdf(uv * fullSize);
            // (only what the liquid adds to the bar: right under the bar's own edge the
            // union is the bar, a px from its outline, and half covered that row: a
            // line along the bar's whole edge)
            if (morphShape.w > 1.5 && sd >= bevelBoxSdf(bp, morphSource, morphShape.y) - 0.01) sd = max(sd, 1.0);
            contourSigned = -sd;
            contourInside = max(-sd, 0.0);
            // (the held copy too: it is the same card, over the same field; without
            // its text coverage the backdrop behind light content lost its darkening
            // as the close began, and vibrancy turned the radar map's dither dots
            // white: a pebbled, glassy map for a few frames)
            if (sdfSource == 1) {
                vec2 f = (cpx - sdfRect.xy) / sdfRect.zw;
                contourText = all(greaterThanEqual(f, vec2(0.0))) && all(lessThanEqual(f, vec2(1.0))) ? texture(sdfTex, f).b * contentFx.y : 0.0;
            }
            contourCover = clamp(contourSigned * 0.8 + 0.5, 0.0, 1.0);
            // (at rest: the client's own pixels uncut, as the settled card draws them;
            // cut to the outline, its antialiased edge just outside appeared at settle)
            if (!restingMorph) surfacePixel *= contourCover;
            if (contourCover < 0.001) {
                float sh = dropShadow(uv) * materialize;
                if (sh < 0.003 && (!restingMorph || surfacePixel.a < 0.001)) discard;
                vec4 kp = keyFill(surfacePixel);
                fragColor = (restingMorph ? kp + vec4(shadeColor(uvBg) * sh, sh) * (1.0 - kp.a) : vec4(shadeColor(uvBg) * sh, sh)) * outFade;
                return;
            }
        } else if (maskMode == 1) {
            vec2 pixelPos = uv * fullSize;
            bool insideRegion = false;
            for (int i = 0; i < regionRectCount; i++) {
                vec4 r = regionRects[i];
                if (pixelPos.x >= r.x && pixelPos.y >= r.y &&
                    pixelPos.x <= r.x + r.z && pixelPos.y <= r.y + r.w) {
                    insideRegion = true;
                    break;
                }
            }
            if (!insideRegion) { fragColor = (surfacePixel) * outFade; return; } // premultiplied, output as-is
        } else if (maskMode == 0 && sdfSource == 1) {
            // a contour's outline comes from its (smoothed) distance field, so its
            // edge is antialiased evenly however faint the client's own fill is
            vec4 fd = texture(sdfTex, (uv * fullSize - sdfRect.xy) / sdfRect.zw);
            // (mitred: the outline and depth from the unblurred distance in A, so a
            // square card keeps its square corners; the blur rounded them)
            contourSigned = cornerMiter > 0.5 ? fd.a : fd.r - fd.g;
            contourInside = cornerMiter > 0.5 ? max(cardSigned(uv * fullSize), 0.0) : fd.r;
            contourText   = fd.b;
            // How much of this pixel the outline covers: a ramp of +-0.625 px (a
            // pixel's footprint across an edge at 45 deg). Uncovered pixels take the
            // shadow path, decided by this same value, so none falls between the two
            // (a straight edge puts pixel centres at exactly +-0.5: a strict test on
            // the distance left a bare line of wallpaper along every side).
            contourCover = clamp(contourSigned * 0.8 + 0.5, 0.0, 1.0);
            if (contourCover < 0.001) {
                float sh = dropShadow(uv) * materialize * objFade;
                if (sh < 0.003) discard;
                // (the client's own pixels just outside the outline, keyed as on the glass:
                // a dark fill drawn there, a tooltip's corner, showed as black)
                vec4 kp = keyFill(surfacePixel);
                fragColor = (kp + vec4(shadeColor(uvBg) * sh, sh) * (1.0 - kp.a)) * outFade;
                return;
            }
        } else if (maskMode == 0 && surfacePixel.a < maskAlphaThreshold) {
            float sh = dropShadow(uv) * materialize * objFade;
            if (sh < 0.003) discard;
            fragColor = (vec4(shadeColor(uvBg) * sh, sh)) * outFade;
            return;
        }
    }

    float cornerSdf, cornerAlpha;
    if (sdfSource == 1 || morphing) {
        cornerSdf   = -contourSigned;
        cornerAlpha = contourCover;
    } else {
        cornerSdf   = getCornerSDF(uv);
        cornerAlpha = 1.0 - smoothstep(-1.5, 0.5, cornerSdf);
    }

    if (maskMode == 1 || maskMode == 2) {
        // Subsurface items and shape-box layers: the glass box (glassBoxOffsetPx/
        // SizePx) can be smaller than the drawn box/region — e.g. a capsule glass
        // shape inside a rectangular blur region. Outside it, fall back to the
        // plain surface pixel instead of dropping it (a hard discard here would
        // silently delete client content, like an icon, that simply isn't under
        // the glass). cornerAlpha fades smoothly across the boundary (the same
        // curve already used for glassA below), so this is an antialiased
        // "glass * coverage, surface over" blend, not a hard cutover — the
        // actual blend happens in the hasMask composite at the bottom of main().
        if (cornerAlpha < 0.001) {
            // outside the glass shape: the surface over the glass's own shadow
            float sh = maskMode == 2 ? dropShadow(uv) * materialize * objFade : 0.0;
            fragColor = (surfacePixel + vec4(shadeColor(uvBg) * sh, sh) * (1.0 - surfacePixel.a)) * outFade;
            return;
        }
    } else {
        // Windows, and alpha-mask layers: no surface pixel to fall back to
        // outside the glass shape, so this is exactly the old hard-edged cutoff.
        // (A contour's edge pixels are covered by how far the outline crosses
        // them: cutting at its zero would leave a half-pixel of bare backdrop,
        // brighter than the glass and its shadow, stepping round every corner.)
        if (sdfSource != 1 && !morphing && cornerSdf > 0.0) discard;
        if (cornerAlpha < 0.001) discard;
    }

    float minDim = min(fullSize.x, fullSize.y);
    float bezelWidthPx = edgeThickness * minDim;

    bool  physical = thicknessPx > 0.0;
    vec3  color;
    vec2  outlineN = vec2(0.0, -1.0);
    float insideD  = 0.0;
    vec3  surfN = vec3(0.0, 0.0, 1.0);
    vec3  surfR = surfN;   // (the normal the backdrop is bent by)
    float edgeProximity = 0.0;

    if (physical) {
        float d   = sdfSource == 1 || morphing ? contourInside : -shapeSdf(uv);
        if (barMerge) edgeOpen = barOpenAt(uv * fullSize);
        float rim = max(min(bezelPx * (1.0 + 0.9 * depth), morphing ? morphRimHere(uv * fullSize) : 0.5 * min(glassBoxSizePx.x, glassBoxSizePx.y)), 1.0);
        // (out of the bar: at the seam the card's glass is the bar's, its height and
        // rim and no frost, so the join is one glass)
        if (morphing && morphShape.w > 1.5 && morphBarLens.y > 0.0) {
            wBar = (1.0 - smoothstep(0.0, 40.0 * monitorScale, bevelBoxSdf(uv * fullSize, morphSource, 0.0))) * max(morphHand.y, inNeck(uv * fullSize));
            rim  = mix(rim, max(morphBarLens.y, 1.0), wBar);
        }
        float t   = clamp(d / rim, 0.0, 1.0);
        // (the edge line is a pixel or two wide, always inside the rim band)
        vec2 nOut = t < 1.0 ? shapeNormal(uv) : vec2(0.0);
        // in true px, so the rim keeps its width round a smoothed corner
        if (t < 1.0) {
            d = d / distanceGain;
            t = clamp(d / rim, 0.0, 1.0);
        }
        outlineN  = nOut;
        insideD   = d;
        // (a small part of a morph is as much lower: its lens a small dome, not a
        // card's height over a few px, which mirrored what was under it)
        float H   = thicknessPx * (1.0 + 0.35 * depth) * materialize * (morphing ? clamp(rim / max(bezelPx, 1.0), 0.15, 1.0) : 1.0);
        if (wBar > 0.0)
            H = mix(H, morphBarLens.x * materialize, wBar);
        float z   = H * rimHeight(t);
        // dz/dd; capped so the vertical wall at t = 0 stays finite
        float slope = t < 1.0 ? min(H * rimSlope(t) / rim, 40.0) * normalTrust : 0.0;
        // (a square corner: the bend fades out toward the corner's point, where the
        // two edges' rims meet; full there, it gathered the scene into a dark curl.
        // Only the bend: the rim's light runs on into the corner, or it had none)
        float bendFade = 1.0;
        if (cornerMiter > 0.5 && !barMerge) {
            bendFade = miterFade(uv * fullSize, miterBox(), rim);
        }
        // a press swells the glass under it into a small dome that springs back
        // with a wobble (motion token pressBulge): a lens over what is behind
        float bump = 0.0;
        vec2  bumpN = vec2(0.0);
        if (abs(pressBulge) > 0.002 && pressGlow.z >= 0.0) {
            float sg  = 60.0 * monitorScale;
            vec2  dv  = uv * fullSize - pressGlow.xy;
            float g   = exp(-dot(dv, dv) / (sg * sg));
            float amp = 0.9 * thicknessPx * materialize * pressBulge;
            bump  = amp * g;
            bumpN = 2.0 * dv / (sg * sg) * bump;   // -grad of the dome
        }
        z += bump;
        surfN = normalize(vec3(nOut * slope + bumpN, 1.0));
        surfR = surfN;
        // (a square corner: the light on the rim follows each edge straight into the
        // corner, mitred like a frame, so the corner is lit as a corner; only the bend
        // takes the smooth depth above, which keeps any seam out of the glass)
        if (cornerMiter > 0.5 && !barMerge && t < 1.0) {
            vec4  fb = miterBox();
            vec2  px = uv * fullSize, lo = px - fb.xy, hi = fb.zw - px;
            vec2  e  = max(min(lo, hi), vec2(0.0));
            vec2  nM = e.x < e.y ? vec2(lo.x < hi.x ? -1.0 : 1.0, 0.0) : vec2(0.0, lo.y < hi.y ? -1.0 : 1.0);
            // (only the direction is the box's: the tilt is the rim's own, so the light
            // ends exactly where the rim does; measured from the box, it ended a few px
            // off, a one-px step in brightness drawn round the glass's inside)
            surfN = normalize(vec3(nM * slope + bumpN, 1.0));
        }
        edgeProximity = 1.0 - t;

        frostLocal = 1.0 - wBar;

        // (nRGB: Cauchy dispersion fitted to n_d and the Abbe number, from the CPU)
        // (a deeper pane bends no further: its view stays inside the sampled backdrop)
        // (a thick slab has a wider bevel: the same bend spread over more of it, so it
        // stretches what is behind less, not more; its stretch across the rim is ~cap/rim)
        float cap = 0.5 * min(rim, bezelPx) * (1.0 + 0.25 * depth);
        // (a card scaled by its open or close bends the view by as much less: at
        // full strength a sliver's rim reached far inside it, drawing what was
        // in the middle along its edge, a light border over a dark backdrop)
        vec2 bendScale = scale2 * invFullSize * bendFade;
        vec2 offG = refractOffsetPx(surfR, nRGB.g, z, cap) * bendScale;
        // (a thick pane also sets what is behind it a little further off: slightly
        // smaller about the pane's middle, eased in from the rim so the rim's own
        // bend stays continuous)
        vec2 farOff = vec2(0.0);
        if (depth > 0.001) {
            vec4  lb = morphing ? morphRect : paneRect.z > paneRect.x ? paneRect
                                                                    : vec4(glassBoxOffsetPx, glassBoxOffsetPx + glassBoxSizePx);
            vec2  o  = (uv * fullSize - 0.5 * (lb.xy + lb.zw)) * (0.035 * depth);
            float lim = 22.0 * monitorScale, l = length(o);
            if (l > 1e-3) o *= lim * tanh(l / lim) / l;
            farOff = o * smoothstep(0.0, 1.0, t) * scale2 * invFullSize * materialize;
            offG  += farOff;
        }
        if (abbe > 0.0 && slope > 0.01 && depth < 0.999) {
            color.r = sampleBlurred(uvBg + refractOffsetPx(surfR, nRGB.r, z, cap) * bendScale + farOff).r;
            color.g = sampleBlurred(uvBg + offG).g;
            color.b = sampleBlurred(uvBg + refractOffsetPx(surfR, nRGB.b, z, cap) * bendScale + farOff).b;
        } else {
            color = sampleBlurred(uvBg + offG).rgb;
        }
    } else {

    // ========================================
    // EDGE PROXIMITY + DIRECTION
    // edgeProximity: 1.0 at boundary, exponential decay inward
    // inwardDir: pixel-space direction toward center (smooth everywhere)
    // Clamped to 1.0: cornerSdf can be positive here (maskMode==1's glass box
    // can be smaller than fullSize, so fragments just outside it still reach
    // this code with cornerAlpha > 0.001), and exp() of a positive value would
    // otherwise overshoot every effect that scales off edgeProximity.
    // ========================================
    // crease-free bevel distance, so the edge has no seam along the corner diagonals;
    // it assumes circular corners, so a superellipse outline keeps the exact SDF
    float bevelSdf = roundingPower == 2.0 ? getBevelSDF(uv) : cornerSdf;
    edgeProximity = min(exp(bevelSdf * invBezelWidthPx), 1.0);
    vec2 inwardDir = refractionDir(uv);
    vec2 posPx = (uv - 0.5) * fullSize; // pixel-space position for the edge-flow direction below

    // ========================================
    // EDGE REFRACTION
    // Offset sampling UV inward (toward center) at edges — like looking
    // through the curved thick edge of a glass slab. This compresses
    // and distorts what's already behind the window, without reaching
    // beyond the window boundary.
    // ========================================
    float refractionPx = refractionStrength * 50.0;
    float lensFalloff = edgeProximity;
    if (refractionSpread < 0.999) {
        // rim-only lens: window the exponential tail so the centre stays flat
        float depth = -bevelSdf;
        float tailWindow = 1.0 - smoothstep(1.5 * bezelWidthPx, 3.0 * bezelWidthPx, depth);
        lensFalloff = mix(edgeProximity * tailWindow, edgeProximity, refractionSpread);
    }
    float refractionMag = lensFalloff * refractionPx;
    vec2 dir = inwardDir;
    if (refractionFlow > 0.001) {
        // pull along the edges instead of toward the centre
        vec2 mixedDir = mix(inwardDir, edgeDir(posPx), refractionFlow);
        float mixedLen = length(mixedDir);
        dir = mixedLen > 0.0001 ? mixedDir / mixedLen : inwardDir;
    }
    vec2 baseOffset = dir * refractionMag * invFullSize;

    // ========================================
    // CHROMATIC ABERRATION — per-channel refraction scale
    // Blue refracts more than red → natural spectral fringing at edges.
    // ========================================
    float chromaSpread = chromaticAberration * 0.35;
    vec2 offsetR = baseOffset * (1.0 - chromaSpread);
    vec2 offsetG = baseOffset;
    vec2 offsetB = baseOffset * (1.0 + chromaSpread);

    // ========================================
    // CENTER DOME LENS (subtle magnification in the flat interior)
    // Fades near edges so it doesn't interfere with edge refraction.
    // ========================================
    vec2 domeUV = vec2(0.0);
    if (lensDistortion > 0.001) {
        vec2 c = (uv - 0.5) * 2.0;
        vec2 dGrad = vec2(
            -4.0 * c.x * (1.0 - c.y * c.y),
            -4.0 * c.y * (1.0 - c.x * c.x)
        );
        float lensFade = 1.0 - edgeProximity;
        domeUV = dGrad * lensMaxPx * lensFade * invFullSize;
    }

    // ========================================
    // BACKGROUND SAMPLING (frosted blur only)
    // Nearby color influence comes naturally from the Gaussian blur
    // kernel crossing the window boundary — no explicit raw sampling.
    // ========================================
    vec2 uvR = uv + offsetR + domeUV;
    vec2 uvG = uv + offsetG + domeUV;
    vec2 uvB = uv + offsetB + domeUV;

    if (chromaticAberration > 0.001 && edgeProximity > 0.01) {
        color.r = sampleBlurred(uvR).r;
        color.g = sampleBlurred(uvG).g;
        color.b = sampleBlurred(uvB).b;
    } else {
        color = sampleBlurred(uvG).rgb;
    }
    } // legacy refraction

    // (what is behind, before this glass's own grading: next to the bar the card is
    // graded as the bar is, not twice)
    vec3 rawColor = color;

    // ========================================
    // FROSTED TINT (per-theme tone mapping)
    // ========================================
    float blurredLum = dot(color, vec3(0.2126, 0.7152, 0.0722));

    // Frosted desaturation
    color = mix(vec3(blurredLum), color, saturation);

    // Tight smoothstep range maps the blur-compressed luminance (~0.3-0.7)
    // to the full [0,1] adaptive range, creating visible per-region differentiation
    float lumCurve = smoothstep(0.25, 0.55, blurredLum);

    // Dim: multiplicative — effective at darkening bright areas
    color *= brightness * (1.0 - adaptiveDim * lumCurve);

    // Boost: additive lift — multiplicative can't brighten near-black content
    color += vec3(adaptiveBoost * (1.0 - lumCurve) * 0.5);

    // Contrast (pivot around midpoint)
    color = mix(vec3(0.5), color, contrast);

    // Vibrancy (selective saturation boost scaled by existing saturation)
    float currentLum = dot(color, vec3(0.2126, 0.7152, 0.0722));
    float sat = max(color.r, max(color.g, color.b)) - min(color.r, min(color.g, color.b));
    float darkFactor = 1.0 - vibrancyDarkness * (1.0 - blurredLum);
    color = mix(vec3(currentLum), color, 1.0 + vibrancy * sat * darkFactor);

    // ========================================
    // COLOR TINT OVERLAY
    // ========================================
    color = mix(color, tintColor, tintAlpha);

    // (clear glass passes a bright backdrop as it is: taking the top of the range
    // down here smoked every pane over a white wallpaper, a third darker. Light
    // text over bright glass is what Dark mode is for, and cards keep their text
    // backing below.)
    lightBase = 0.25 + (hasMask ? 0.3 * frost.x * frostLocal : 0.35 + 0.15 * frost.x);
    if (physical && (darkGlass > 0.0 || lightGlass > 0.0))
        color = glassTone(color);

    // Light text over a bright backdrop: the glass stays clear, and only the
    // backdrop right around the card's text is darkened (neutrally), the way
    // vibrancy keeps labels readable on clear glass. A light theme's dark text the
    // other way round: over a dark backdrop, a light veil right around it.
    if (physical && hasMask && sdfSource == 1) {
        float l      = dot(color, vec3(0.2126, 0.7152, 0.0722));
        float bright = lightUi > 0.5 ? 1.0 - smoothstep(0.30, 0.70, l) : smoothstep(0.35, 0.75, l);
        if (bright > 0.0) {
            // the text coverage, blurred with the field: a smooth soft halo (read with the outline)
            // (from a fair coverage up: hugs the strokes like a text shadow; from the
            // faintest, it spread into a muddy box round large bold type)
            float tn = smoothstep(0.08, 0.40, contourText);
            // How visible the content is, exactly: the open's or close's own content fade
            // while it moves, the held copy's fade, full on an open card (textBacking 2),
            // the live reading only while a card without a held copy fades out.
            // (One copy can't say: one grabbed part way through a fade, textBacking < 1,
            // and a live fade. There the backing is also kept no stronger than the
            // content right here, this frame. Applied to every moving card, it held the
            // backing back under the semi-transparent "Apps" pill through the open, and it
            // appeared in one step the moment the menu landed.)
            float tb   = min(textBacking, 1.0);
            bool  live = livePresence == 1 && textBacking < 1.5 && !held && !morphing;
            float vis  = morphing ? contentFx.y * tb : held ? ghost.y * tb : live ? rawP : tb;
            if (tb < 0.999 || (live && rawP < 0.999)) {
                float near = surfAlphaAt(vec2(0.0));
                for (int i = 0; i < 12; i++) {
                    float a = float(i) * 0.5236, r = i < 6 ? 4.0 : 9.0;
                    near = max(near, surfAlphaAt(vec2(cos(a), sin(a)) * r * monitorScale));
                }
                vis = min(vis, near);
            }
            // (gone before the text starts to fade: light text half faded over bright
            // glass is all but invisible, while a dark backing at any strength still
            // reads, as dark ghosts of the labels and pills; squared was not enough at
            // 30 fps on the tall screen)
            float shown = smoothstep(0.6, 0.97, vis);
            color = mix(color, lightUi > 0.5 ? vec3(mix(l, 1.0, 0.78)) : vec3(l * 0.32), tn * bright * 0.7 * textFade * shown);
        }
    }

    // A thin bar over a wallpaper that changes along it (gold on the left,
    // silver on the right) reads the backdrop locally, from the brightness strip
    // built with each resample (barstyle.frag). The glass leans a touch lighter
    // or darker and its monochrome glyphs take the opposite tone.
    float styleLight = 0.0, styleV = 0.5;
    if (adaptiveStyle == 1) {
        // (the strip's own orientation: it runs along the layer's long side, which
        // an inset shape doesn't always share)
        ivec2 strip  = textureSize(styleTex, 0);
        bool  across = strip.x > strip.y;
        // (a narrow switch: mid tones pick the side that contrasts better, never grey on gold)
        styleV     = texture(styleTex, across ? vec2(uvBg.x, 0.5) : vec2(0.5, uvBg.y)).r;
        styleV     = glassToneOf(styleV);
        styleLight = smoothstep(0.44, 0.52, styleV);
        color = mix(color * 0.94, mix(color, vec3(1.0), 0.07), styleLight);
    }

    // Out of the bar, the card near it is the bar's own glass flowing out: there it
    // is graded exactly as the bar is (the bar's own dim, contrast, tint and its
    // light or dark pane, by the same steps as above), fading into the card's own
    // glass further down, so where the two meet there is no line at all: one glass.
    // (Estimating the bar's tint from the screen left a step: at its edge the bar
    // bends the wallpaper, so the readings were never a pure tint.)
    if (morphing && morphShape.w > 1.5 && morphSource.z > morphSource.x && barGrade1.w >= 0.0) {
        vec2  bp    = uv * fullSize;
        float wTone = (1.0 - smoothstep(0.0, 70.0 * monitorScale, bevelBoxSdf(bp, morphSource, 0.0))) * max(morphHand.y, inNeck(bp));
        if (wTone > 0.001) {
            vec3  c  = rawColor;
            float bl = dot(c, vec3(0.2126, 0.7152, 0.0722));
            c  = mix(vec3(bl), c, barGrade1.w);
            c *= barGrade1.x * (1.0 - barGrade1.y * smoothstep(0.25, 0.55, bl));
            c  = mix(vec3(0.5), c, barGrade1.z);
            c  = mix(c, barTint.rgb, barTint.a);
            if (barStyle.y > 0.0) {
                ivec2 strip  = textureSize(styleTex, 0);
                bool  across = strip.x > strip.y;
                float su     = across ? (bp.x + barStyle.x) * barStyle.y : (bp.y + barStyle.z) * barStyle.w;
                float sv     = texture(styleTex, across ? vec2(su, 0.5) : vec2(0.5, su)).r;
                float sl     = smoothstep(0.44, 0.52, glassToneOf(sv));
                c = mix(c * 0.94, mix(c, vec3(1.0), 0.07), sl);
            }
            // (dark glass: the bar's own grade, as the bar draws it)
            if (darkGlass > 0.0 || lightGlass > 0.0)
                c = glassTone(c);
            color = mix(color, c, wTone);
        }
    }

    if (physical) {
        // Fresnel (Schlick): the rim, tilted away from the eye, reflects more of
        // the bright surroundings than the flat top does.
        float f0  = fresnelF0;
        // the outermost (partly covered) pixels would mirror almost everything:
        // reflection and glints start a couple of pixels inside the edge
        float edgeIn = smoothstep(0.0, 2.0 * monitorScale, insideD);
        float cv  = clamp(surfN.z, 0.0, 1.0);
        float m1  = 1.0 - cv, m2 = m1 * m1;
        float fr  = f0 + (1.0 - f0) * m2 * m2 * m1;
        // what the tilted rim mirrors is the scene around it, as bright as it is:
        // glass adds no white of its own (a lift here read as a white sheen)
        float fw  = clamp(fr * fresnelStrength * 2.2 * materialize, 0.0, 0.85) * edgeIn;
        // (the flat top reflects ~2 %: its four taps were most of the shader's work
        // across a pane's middle for nothing anyone could see)
        if (fw > 0.0 && edgeProximity > 0.0) {
            // (surfN comes from the outline in uv axes already: no screen rotation)
            vec2 eo   = uvBg + surfN.xy * 20.0 * scale2 * invFullSize;
            // (taps close together: a polished surface mirrors sharply; spread 22 px
            // apart they blurred it into a soft, matte sheen)
            vec2 es   = 6.0 * scale2 * invFullSize;
            vec3 env  = (sampleBlurred(eo + vec2(es.x, es.y)).rgb + sampleBlurred(eo + vec2(-es.x, es.y)).rgb +
                         sampleBlurred(eo + vec2(es.x, -es.y)).rgb + sampleBlurred(eo - es).rgb) * 0.25;
            if (fresnelColorAlpha > 0.001) env = mix(env, fresnelColor, fresnelColorAlpha);
            // (tinted as the glass is: untinted, the rim's ~2 % of reflection made the
            // whole rim band a shade off the face, with a line where the rim ended)
            if (darkGlass > 0.0 || lightGlass > 0.0) env = glassTone(env);
            color = mix(color, env, fw);
        }

        // A directional light above the screen: an arc along the rim facing it,
        // fainter on the far rim (light reflected inside the glass). A tight 3D
        // lobe lit only the one spot where a corner faced the light (a speck).
        // Kept to the outermost pixels, where the rim turns over: wider, it reads
        // as a painted white border; none on the flat top. (Across the whole steep
        // part of the rim, ~10 px, it lit a soft glow in the one corner facing the
        // light; a light on a curved rim reflects as a thin line.)
        if (surfN.z < 0.985) {
            float nl   = length(surfN.xy);
            // (a thicker pane scatters its light: the lit arc softer and a little wider)
            float band = smoothstep(0.955 - 0.18 * depth, 0.996, nl) * edgeIn * (1.0 - 0.35 * depth);
            if (band > 0.0) {
                float f   = dot(surfN.xy, lightUv) / nl;
                float f2  = f * f, f6 = f2 * f2 * f2;
                float arc = f > 0.0 ? f6 : 0.35 * f6;
                color += vec3(1.0, 0.99, 0.97) * arc * band * specularStrength * materialize * textFade * (1.0 - edgeOpen);
            }
        }

        // A thin line where the rim turns over, bright on the sides facing the
        // light and dark on the far side: one continuous edge, one light.
        if (bevelStrength > 0.001) {
            float sizePx = max(bevelSize * monitorScale, 1.0);
            float rs     = cornerSdf / distanceGain;   // (true px: as thin in the corners)
            float ring   = (1.0 - smoothstep(-0.35 * sizePx, 0.0, rs)) * smoothstep(-sizePx, -0.35 * sizePx, rs);
            float facing = clamp(dot(outlineN, bevelLightUv) * 0.5 + 0.5, 0.0, 1.0);
            // (lit where the edge faces the light and fading round it, as glass catches
            // a light: lit half-strength all round it read as a white outline; from the
            // half-lit facing, cubed, a straight edge 45 degrees off the light still had
            // 0.62 of it, an outline along the whole top and side: the true cosine, to
            // the 4th, a glint where the corner faces the light, 0.25 along the edges)
            float fc  = max(dot(outlineN, bevelLightUv), 0.0);
            float fc2 = fc * fc;
            float lit = fc2 * fc2;
            // (closing, the edge light goes with the content: on the empty pane that
            // shrinks back into the bar it read as a white outline)
            color = mix(color, vec3(1.0), ring * lit * bevelStrength * materialize * textFade * (1.0 - 0.7 * wBar));
            // (none where the card flows into the bar: the fillets face down, and their
            // dark side drew short dark ledges under the bar)
            color = mix(color, vec3(0.0), ring * (1.0 - facing) * bevelShadow * materialize * textFade * (1.0 - wBar));
        }

        // The active window, in place of a bright border: one light in the room,
        // moving slowly round the screen (the same light for every window; only
        // the active one shows it). The glass doesn't glow: its rim catches the
        // light where it faces it, a bright line where the rim turns over and a
        // little light inside the rim behind it, brightest where the edge faces
        // the light squarely and fading along the edge (the spot slides along and
        // round the corners as the light moves). The rim splits it a little into
        // colour, and the far rim shows a faint copy (light reflected inside).
        if (gleam.w > 0.5 && !hasMask && (gleam.x > 0.001 || gleamPulse > 0.001) && edgeProximity > 0.0) {
            // (styles: 2 comet/halo, 3 twin: a place along the outline; 4 outline: the
            // whole rim, steady, brightest facing the room's light; 5 aura: the whole rim,
            // with a broad crest going round; 6 prism: a direction the light comes from)
            bool  aura    = gleam.w > 4.5 && gleam.w < 5.5;
            bool  comet   = (gleam.w > 1.5 && gleam.w < 3.5) || aura;
            bool  outlineL = gleam.w > 3.5 && gleam.w < 4.5;
            bool  prism   = gleam.w > 5.5;
            vec2  toL  = comet ? -outlineN : gleam.yz;
            vec2  ld   = toL / max(length(toL), 1e-3);
            // (dispersion: red and blue see the light from a hair either side)
            const float k = 0.035;
            vec2  ldR  = vec2(ld.x - k * ld.y, ld.y + k * ld.x), ldB = vec2(ld.x + k * ld.y, ld.y - k * ld.x);
            vec3  f    = vec3(dot(outlineN, ldR), dot(outlineN, ld), dot(outlineN, ldB));
            vec3  fp   = max(f, 0.0), fn = max(-f, 0.0);
            // (each style its own spread: a tight spot running round, a broad light
            // from the pointer or overhead; the far rim a faint copy)
            float pw   = max(gleamShape.x, 1.0);
            vec3  lobe = pow(fp, vec3(pw)) + 0.22 * pow(fn, vec3(pw * 1.5));
            if (comet) {
                // (this rim pixel's place along the outline, from the top edge's start,
                // clockwise; the streak a gaussian of it, wrapped round the lap)
                float d = cometDistance(uv * fullSize - glassBoxOffsetPx, glassBoxSizePx, gleam.y, gleam.w > 2.5 && gleam.w < 3.5);
                float g = exp(-0.5 * d * d / max(gleam.z * gleam.z, 1.0));
                // (the aura never lets the rim go dark: the focused window always reads)
                lobe = vec3(aura ? 0.42 + 0.58 * g : g);
            }
            if (outlineL)
                // (every edge lit, the ones facing the light more, the far ones never
                // below half: as a pane of glass catches a window's light all round)
                lobe = vec3(0.5 + 0.5 * smoothstep(-0.35, 0.9, f.g));
            float nl   = length(surfN.xy);
            float line = smoothstep(0.86, 0.985, nl) * edgeIn;
            // (a sheen across the outer rim behind the bright line, as polished glass
            // catches a light: the 1-2 px line alone hardly read as the focused window)
            float in12 = 1.0 - clamp(insideD / (12.0 * monitorScale), 0.0, 1.0);  // (inside the strip redrawn for it)
            float sheen = in12 * in12 * in12 * edgeIn;                            // (a soft glow, brightest at the rim)
            // (the light tinted a touch by what it falls on: glass catching a light
            // looks lit, not painted white)
            float maxC  = max(max(color.r, color.g), color.b);
            vec3  tintL = mix(vec3(1.0, 0.99, 0.97), maxC > 0.001 ? color / maxC : vec3(1.0), 0.25);
            // (the prism: the lit line split into colour across its width, red outside,
            // violet inside, as an edge of glass spreads white light; softened with white)
            vec3  lineC = vec3(1.0);
            if (prism) {
                float q = clamp(insideD / (3.5 * monitorScale), 0.0, 1.0);
                vec3  sp = clamp(vec3(1.6 - 2.6 * q, 1.0 - 2.4 * abs(q - 0.45), 2.6 * q - 0.9), 0.0, 1.0);
                lineC = mix(vec3(1.0), sp / max(max(sp.r, max(sp.g, sp.b)), 0.001), 0.7);
            }
            color += lobe * (1.0 * line * lineC + 0.55 * sheen * (prism ? mix(tintL, lineC, 0.5) : tintL)) * gleam.x * gleamShape.z;
            // (and the whole rim's line, faintly: the focused window reads as such
            // wherever the light is)
            color += vec3(1.0, 0.99, 0.97) * gleamShape.y * line * gleam.x;
            // the focus flash: the whole rim at once, strongest facing the light
            if (gleamPulse > 0.001) {
                float wide = gleamPulse * (0.35 + 0.65 * fp.g * fp.g);
                color += vec3(1.0, 0.99, 0.97) * wide * (0.55 * line + 0.14 * sheen);
            }
        }

        // Pressed glass lights up from inside: light enters under the pointer and
        // scatters out through the glass, a soft front that spreads and fades.
        // Where it reaches the rim the curved surface sends more of it to the eye.
        if (pressGlow.z >= 0.0) {
            float t      = pressGlow.z;
            float u      = 1.0 - t;
            float spread = pressReachPx * (0.2 + 0.8 * (1.0 - u * u * u));
            vec2  dv     = uv * fullSize - pressGlow.xy;
            // (gone before the edge of the box that gets redrawn: no square seam)
            float body   = exp(-dot(dv, dv) / (spread * spread)) * (1.0 - smoothstep(0.9, 1.12, length(dv) / pressReachPx));
            float fade   = 1.0 - smoothstep(0.2, 1.0, t);
            float rim    = 1.0 - surfN.z;
            float maxC   = max(max(color.r, color.g), color.b);
            vec3  light  = mix(vec3(1.0), maxC > 0.001 ? color / maxC : vec3(1.0), 0.35);
            color += light * body * fade * (0.12 + 0.6 * rim) * materialize;
        }
    } else {
    // ========================================
    // BEVEL — thin lit line hugging the edge, brightest on the side facing the light
    // ========================================
    if (bevelStrength > 0.001) {
        float sizePx = max(bevelSize * monitorScale, 1.0);   // logical px, uniform across monitor scales
        float core = 0.25 * sizePx;
        float tail = sizePx;
        float ring = (1.0 - smoothstep(-core, 0.0, cornerSdf)) * smoothstep(-tail, -core, cornerSdf);

        // lit side faces the light, the far side fades out and can be shadowed
        float facing = clamp(dot(sdfOutwardNormal(uv), lightDir(bevelAngle)) * 0.5 + 0.5, 0.0, 1.0);

        vec3 bevelLight = vec3(1.0);
        if (bevelColorAlpha > 0.001) bevelLight = mix(vec3(1.0), bevelColor, bevelColorAlpha);   // a dark colour gives a dark line
        if (bevelTint > 0.001) {
            float maxC = max(max(color.r, color.g), color.b);
            bevelLight = mix(bevelLight, maxC > 0.001 ? color / maxC : vec3(1.0), bevelTint);
        }

        color = mix(color, bevelLight, ring * facing * bevelStrength);
        if (bevelShadow > 0.001)
            color = mix(color, vec3(0.0), ring * (1.0 - facing) * bevelShadow);
    }

    // ========================================
    // FRESNEL RIM GLOW (edge zone)
    // ========================================
    if (fresnelStrength > 0.001) {
        float fresnel = edgeProximity * edgeProximity * fresnelStrength * 0.15;
        vec3 fresnelLight = vec3(1.0);
        if (fresnelColorAlpha > 0.001) fresnelLight = mix(vec3(1.0), fresnelColor, fresnelColorAlpha);   // chosen colour, then the tint below
        if (fresnelTint > 0.001) {
            // rim light in the background's own hue, at full brightness so the gain matches white
            float maxC = max(max(color.r, color.g), color.b);
            fresnelLight = mix(fresnelLight, maxC > 0.001 ? color / maxC : vec3(1.0), fresnelTint);
        }
        color += fresnelLight * fresnel;
    }

    // ========================================
    // SPECULAR — subtle top highlight (edge zone)
    // ========================================
    if (specularStrength > 0.001) {
        float specT = max(1.0 - uv.y, 0.0);
        if (abs(specularAngle) > 0.001) {
            // rotate the highlight gradient toward the light: at angle 0, lightDir gives
            // (0,-1) and dot(uv-0.5, L) = 0.5-uv.y, so 0.5+dot(...) reduces to 1-uv.y exactly
            vec2 L = lightDir(specularAngle);
            specT = clamp(0.5 + dot(uv - 0.5, L), 0.0, 1.0);
        }
        float topBias = pow(specT, 2.0);
        float spec = topBias * edgeProximity * edgeProximity * specularStrength * 0.08;
        color += vec3(1.0, 0.99, 0.97) * spec;
    }

    // ========================================
    // INNER SHADOW (bottom rim)
    // ========================================
    {
        float bottomBias = pow(uv.y, 2.0);
        float shadow = bottomBias * edgeProximity * edgeProximity * 0.06;
        color *= 1.0 - shadow;
    }
    } // legacy lighting

    // float framebuffers (FP16 under wide-gamut cm) store unbounded values and
    // the glass re-samples its own output: unclamped color diverges over frames
    color = clamp(color, 0.0, 1.0);
    float glassA = clamp(glassOpacity * liveP * cornerAlpha, 0.0, 1.0);

    if (hasMask) {
        // Layers only: composite the rendered surface over the glass effect
        // in a single pass. surfacePixel is premultiplied alpha from Hyprland's
        // surface rendering, so we unpremultiply before the 'over' blend.
        float surfA = surfacePixel.a;
        vec3 surfRGB = surfA > 0.001 ? surfacePixel.rgb / surfA : vec3(0.0);
        if (fillKey.a >= 0.0 && surfA > 0.001) {
            vec4 kp = keyFill(vec4(surfRGB * surfA, surfA));
            surfA   = kp.a;
            surfRGB = kp.a > 1e-4 ? kp.rgb / kp.a : fillKey.rgb;
        }
        if (fillKeyTop > 0 && fillKey.a >= 0.0) {
            // The bar marks the item whose panel is open with a line along its
            // inner edge, where the card now flows out: the line goes to the
            // mirrored place along the outer edge instead (fades and all), so the
            // bar still shows which item is open and the drop leaves it uninterrupted.
            // Only a thin run of the key colour in the inner quarter mirrors (a
            // blue icon there would be thick: the pixels 4 px outward are blue too).
            // (uv is 0..1 over the surface; the mask is the whole monitor's buffer,
            // the surface a strip in it: mirrored in uv first. Which way is across
            // the bar, and which end is outer, from fillKeyTop: the tall screen's
            // bar runs down its buffer)
            bool  acrossY = fillKeyTop <= 2;
            float t       = acrossY ? (fillKeyTop == 1 ? uv.y : 1.0 - uv.y) : (fillKeyTop == 3 ? uv.x : 1.0 - uv.x);
            float along   = acrossY ? uv.x : uv.y;
            if (t >= 0.0 && t < 0.25 && along > 0.0 && along < 1.0) {
                vec2  tsz   = vec2(textureSize(maskTex, 0));
                float rowUv = 1.0 / max(acrossY ? maskUVScale.y * tsz.y : maskUVScale.x * tsz.x, 1.0);
                // (outward: toward t = 0)
                float outw  = (fillKeyTop == 1 || fillKeyTop == 3) ? -1.0 : 1.0;
                vec2  m     = acrossY ? vec2(uv.x, 1.0 - uv.y) : vec2(1.0 - uv.x, uv.y);
                vec2  step4 = acrossY ? vec2(0.0, 4.0 * rowUv * outw) : vec2(4.0 * rowUv * outw, 0.0);
                vec4  mp    = texture(maskTex, m * maskUVScale + maskUVOffset);
                vec4  above = texture(maskTex, (m + step4) * maskUVScale + maskUVOffset);
                vec3  mRGB  = mp.a > 0.001 ? mp.rgb / mp.a : vec3(0.0);
                vec3  aRGB  = above.a > 0.001 ? above.rgb / above.a : vec3(0.0);
                float mk    = (1.0 - smoothstep(0.03, 0.09, distance(mRGB, fillKey.rgb))) * mp.a;
                float ak    = (1.0 - smoothstep(0.03, 0.09, distance(aRGB, fillKey.rgb))) * above.a;
                mk *= 1.0 - ak;
                if (mk > 0.001) {
                    float a = mk + surfA * (1.0 - mk);
                    surfRGB = (fillKey.rgb * mk + surfRGB * surfA * (1.0 - mk)) / max(a, 1e-4);
                    surfA   = a;
                }
            }
        }
        if (adaptiveStyle == 1) {
            // Monochrome glyphs take the tone opposite the backdrop, keeping their
            // emphasis (how far each is from its own background tone, so dimmed
            // items stay dimmed) whichever way the client drew them. Colour stays.
            float lum    = dot(surfRGB, vec3(0.2126, 0.7152, 0.0722));
            float chroma = max(surfRGB.r, max(surfRGB.g, surfRGB.b)) - min(surfRGB.r, min(surfRGB.g, surfRGB.b));
            // (compressed, so dimmed glyphs stay readable on either tone)
            float emph   = 0.62 + 0.38 * (lum > 0.5 ? lum : 1.0 - lum);
            // (over mid tones the tone the client drew, which the shell picked for the
            // whole bar; turned over only where the backdrop is clearly the other
            // way, in a step: blended across the mid tones, glyphs went grey over a
            // red-orange wallpaper, a clock fading from dark to grey along its "PM")
            float wantLight = lum > 0.5 ? 1.0 - smoothstep(0.55, 0.57, styleV) : 1.0 - smoothstep(0.39, 0.41, styleV);
            float tone   = mix(1.0 - emph, emph, wantLight) * 0.92 + 0.04;
            surfRGB = mix(surfRGB, vec3(tone), 1.0 - smoothstep(0.12, 0.30, chroma));
        }

        // the glass's own shadow under the part of an edge pixel it leaves uncovered
        float shA   = glassA < 0.999 && maskMode != 1 ? dropShadow(uv) * materialize * objFade * (1.0 - glassA) : 0.0;
        float underA = glassA + shA;
        if (sdfSource == 1 && adaptiveStyle == 0 && surfA > 0.14) {
            // Vibrancy: over bright glass a light-on-dark label (drawn for the
            // card's dark fill) would go grey on gold. Monochrome text becomes white
            // at the emphasis its client gave it, over the glass behind it, so a
            // dimmed label stays dimmed but readable. Over dark glass nothing changes.
            float lum    = dot(surfRGB, vec3(0.2126, 0.7152, 0.0722));
            float chroma = max(surfRGB.r, max(surfRGB.g, surfRGB.b)) - min(surfRGB.r, min(surfRGB.g, surfRGB.b));
            if (classChroma >= 0.0) chroma = max(chroma, classChroma);
            float glassL = dot(color, vec3(0.2126, 0.7152, 0.0722));
            float v = (1.0 - smoothstep(0.12, 0.30, chroma)) * smoothstep(0.14, 0.50, surfA) *
                      smoothstep(0.28, 0.50, glassL) * step(0.2, lum);
            if (v > 0.0) {
                float emph = clamp((lum - 0.10) / 0.86, 0.0, 1.0);
                surfRGB = mix(surfRGB, mix(color, vec3(1.0), 0.35 + 0.65 * emph), v);
            }
        }
        float compA = surfA + underA * (1.0 - surfA);
        vec3 compRGB = compA > 0.001
            ? (surfRGB * surfA + (color * glassA + (shA > 0.0 ? shadeColor(uvBg) * shA : vec3(0.0))) * (1.0 - surfA)) / compA
            : vec3(0.0);

        // Hyprland's compositor expects premultiplied alpha (blend GL_ONE, GL_ONE_MINUS_SRC_ALPHA).
        fragColor = (vec4(compRGB * compA, compA)) * outFade;
    } else {
        // Windows: output the glass effect alone, surface is rendered separately by Hyprland.
        // Premultiplied: without this, a fading window's glass keeps full RGB contribution
        // because the GL_ONE source factor adds raw color regardless of alpha.
        fragColor = (vec4(color * glassA, glassA)) * outFade;
    }
}
)GLSL"},

    // Shape field: the distance field of a layer's own drawn shape, by jump
    // flooding (Rong & Tan 2006). Texels hold the offsets in px to the nearest
    // outside (rg) and inside (ba) texel; 16384 marks "none found yet".
    {"fieldseed.frag", R"GLSL(
#version 300 es
precision highp float;

uniform sampler2D tex;        // the surface as rendered into the layer temp FBO
uniform vec2 maskUVOffset;
uniform vec2 maskUVScale;
uniform float threshold;
uniform sampler2D peakTex;    // unit 1: G, the strongest content in this same frame (usePeak)
uniform float usePeak;

in vec2 v_texcoord;
layout(location = 0) out vec4 fragColor;

void main() {
    float a = texture(tex, v_texcoord * maskUVScale + maskUVOffset).a;
    // (relative to this frame's own strongest content: the CPU's reading of it lands
    // a frame or two late, and on the first frame of a fade-out a card's faint fill
    // fell under a threshold scaled by the reading while its opaque parts didn't)
    float threshold = usePeak > 0.5 ? threshold * max(texture(peakTex, vec2(0.5)).g, 0.05) : threshold;
    // rg: offset to the nearest outside texel; ba: to the nearest inside texel
    fragColor = a < threshold ? vec4(0.0, 0.0, 16384.0, 16384.0) : vec4(16384.0, 16384.0, 0.0, 0.0);
}
)GLSL"},

    {"fieldstep.frag", R"GLSL(
#version 300 es
precision highp float;

uniform sampler2D tex;
uniform float stepPx;

in vec2 v_texcoord;
layout(location = 0) out vec4 fragColor;

void main() {
    ivec2 size = textureSize(tex, 0);
    ivec2 p    = ivec2(gl_FragCoord.xy);
    vec4 self  = texelFetch(tex, p, 0);
    vec2 bestOut = self.rg, bestIn = self.ba;
    float lenOut = bestOut.x > 8000.0 ? 1e12 : dot(bestOut, bestOut);
    float lenIn  = bestIn.x > 8000.0 ? 1e12 : dot(bestIn, bestIn);
    int k = int(stepPx);
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            if (dx == 0 && dy == 0) continue;
            ivec2 q = p + ivec2(dx, dy) * k;
            if (q.x < 0 || q.y < 0 || q.x >= size.x || q.y >= size.y) continue;
            vec4 o = texelFetch(tex, q, 0);
            vec2 d = vec2(q - p);
            if (o.x < 8000.0) {
                vec2 c = o.rg + d; float l = dot(c, c);
                if (l < lenOut) { bestOut = c; lenOut = l; }
            }
            if (o.z < 8000.0) {
                vec2 c = o.ba + d; float l = dot(c, c);
                if (l < lenIn) { bestIn = c; lenIn = l; }
            }
        }
    }
    fragColor = vec4(bestOut, bestIn);
}
)GLSL"},

    // Offsets -> distance in px (the box border counts as outside too), smoothed
    // with a 5x5 binomial kernel so the normals taken from its gradient are clean.
    {"fieldresolve.frag", R"GLSL(
#version 300 es
precision highp float;

uniform sampler2D tex;
uniform float maxDist;
uniform sampler2D maskTex;     // the surface: its text coverage rides along in G
uniform vec2 maskUVOffset;
uniform vec2 maskUVScale;
uniform float lightUi;         // 1: a light theme: its text is the dark drawing, its cards light

in vec2 v_texcoord;
layout(location = 0) out vec4 fragColor;

// x: distance to the outline from inside, y: from outside
vec2 distAt(ivec2 q, ivec2 size) {
    q = clamp(q, ivec2(0), size - 1);
    vec4 o = texelFetch(tex, q, 0);
    float dIn  = o.x > 8000.0 ? maxDist : length(o.rg) - 0.5;
    float dOut = o.z > 8000.0 ? maxDist : length(o.ba) - 0.5;
    vec2 c = vec2(q) + 0.5;
    float border = min(min(c.x, c.y), min(float(size.x) - c.x, float(size.y) - c.y));
    return vec2(clamp(min(dIn, border), 0.0, maxDist), clamp(dOut, 0.0, maxDist));
}

void main() {
    ivec2 size = textureSize(tex, 0);
    ivec2 p    = ivec2(gl_FragCoord.xy);
    float w[5] = float[5](1.0, 4.0, 6.0, 4.0, 1.0);
    vec2 sum   = vec2(0.0);
    for (int dy = -2; dy <= 2; dy++)
        for (int dx = -2; dx <= 2; dx++)
            sum += w[dx + 2] * w[dy + 2] * distAt(p + ivec2(dx, dy), size);
    sum /= 256.0;
    // signed (inside positive) for the blur passes that follow, and how much of
    // this texel is text/icons (opaque drawing on the faint card fill)
    // (from just above a card's faint fill, so dimmed labels get their backing too)
    // (and only light pixels: an opaque dark fill, like the volume pop-up's, is not
    // text; counted as text, its whole pill got the dark backing, a grey veil)
    vec4  sp   = texture(maskTex, v_texcoord * maskUVScale + maskUVOffset);
    // (a light theme: the dark drawing is the text, and its light card fill never is;
    // counted as text, a whole light card took the backing, a grey slab)
    float lum  = dot(sp.a > 0.001 ? sp.rgb / sp.a : vec3(0.0), vec3(0.2126, 0.7152, 0.0722));
    float text = smoothstep(0.14, 0.50, sp.a) *
                 (lightUi > 0.5 ? 1.0 - smoothstep(0.45, 0.70, lum) : smoothstep(0.22, 0.40, lum));
    // (B: the signed distance again, carried unblurred through the blur passes)
    fragColor = vec4(sum.x - sum.y, text, sum.x - sum.y, 1.0);
}
)GLSL"},

    // Gaussian blur of the signed distance: a straight edge's distance is linear and
    // passes through unchanged, while the crease every distance field has along a
    // corner's diagonal is rounded off, so the rim has no seam there. The second
    // pass splits it back into inside (R) and outside (G) distances.
    {"fieldblur.frag", R"GLSL(
#version 300 es
precision highp float;

uniform sampler2D tex;
uniform vec2 stepPx;           // (1, 0) or (0, 1)
uniform float sigma;
uniform float finish;          // 1: write inside/outside instead of signed

in vec2 v_texcoord;
layout(location = 0) out vec4 fragColor;

void main() {
    ivec2 size = textureSize(tex, 0);
    ivec2 p    = ivec2(gl_FragCoord.xy);
    vec2 sum = vec2(0.0);
    float wsum = 0.0;
    int r = int(ceil(sigma * 2.5));
    for (int i = -r; i <= r; i++) {
        ivec2 q = clamp(p + ivec2(stepPx) * i, ivec2(0), size - 1);
        float w = exp(-0.5 * float(i * i) / (sigma * sigma));
        sum += w * texelFetch(tex, q, 0).rg;
        wsum += w;
    }
    vec2 s = sum / wsum;
    float exact = texelFetch(tex, p, 0).b;
    // R: inside distance, G: outside distance, B: text coverage softened into a halo,
    // A: the signed distance unblurred (a square outline's own corners)
    fragColor = finish > 0.5 ? vec4(max(s.x, 0.0), max(-s.x, 0.0), s.y, exact) : vec4(s, exact, 1.0);
}
)GLSL"},

    // How bright the backdrop is along an adaptive bar: one texel per 8 px, each a
    // tent-weighted mean over +-halfWindow along the bar and three rows across it,
    // so the bar's light/dark style varies smoothly (point taps on a sharp
    // backdrop stepped whenever one crossed a stripe edge: seams). Built only when
    // the backdrop is resampled; the glass reads one texel instead of seven.
    {"barstyle.frag", R"GLSL(
#version 300 es
precision highp float;

uniform sampler2D tex;         // the layer's backdrop sample, padded
uniform vec2 padding;          // its padding ratio
uniform float halfWindow;      // half the window, in layer uv along the bar
uniform float across;          // 1: the bar runs along x

in vec2 v_texcoord;
layout(location = 0) out vec4 fragColor;

void main() {
    float s = across > 0.5 ? v_texcoord.x : v_texcoord.y;
    float acc = 0.0, wsum = 0.0;
    for (int i = -24; i <= 24; i++) {
        float w = 25.0 - abs(float(i));
        // (clamped to the layer: past its ends the sample is cleared padding)
        float t = clamp(s + halfWindow * float(i) * (1.0 / 24.0), 0.0, 1.0);
        for (int j = 0; j < 3; j++) {
            float o  = 0.2 + 0.3 * float(j);
            vec2 luv = across > 0.5 ? vec2(t, o) : vec2(o, t);
            acc += w * dot(texture(tex, luv * (1.0 - 2.0 * padding) + padding).rgb, vec3(0.2126, 0.7152, 0.0722));
        }
        wsum += 3.0 * w;
    }
    fragColor = vec4(vec3(acc / wsum), 1.0);
}
)GLSL"},

    // Where a layer's content is: one texel per cell of the box, set where any
    // pixel in the cell (sampled on a 6x6 grid) is drawn. Read back once per
    // content change, so the glass only ever works over that area.
    {"fieldbox.frag", R"GLSL(
#version 300 es
precision highp float;

uniform sampler2D tex;
uniform vec2 maskUVOffset;
uniform vec2 maskUVScale;
uniform float threshold;
uniform vec2 cells;            // cells across and down the box

in vec2 v_texcoord;
layout(location = 0) out vec4 fragColor;

void main() {
    vec2 base = floor(gl_FragCoord.xy);
    float a = 0.0;
    for (int j = 0; j < 6; j++)
        for (int i = 0; i < 6; i++) {
            vec2 t = (base + (vec2(float(i), float(j)) + 0.5) / 6.0) / cells;
            a = max(a, texture(tex, t * maskUVScale + maskUVOffset).a);
        }
    // the cell's strongest alpha (the CPU thresholds it and also reads how
    // visible the content is: a card fading in or out)
    fragColor = vec4(a >= threshold ? max(a, 1.0 / 255.0) : 0.0, 0.0, 0.0, 1.0);
}
)GLSL"},

    // How visible a layer's content is right now: the mean cell (fieldbox.frag)
    // over the card's cells, into one texel the glass reads in the same frame.
    // (The mean, not the strongest cell: that is a sampled estimate that varied
    // with small shifts, and the glass jumped with it.)
    // Used while a card closes, so its glass fades exactly with the client's own
    // fade (an asynchronous readback lands a frame or two late and stepped).
    {"presence.frag", R"GLSL(
#version 300 es
precision highp float;

uniform sampler2D tex;         // the cells
uniform vec4 range;            // first cell x, y; cells across, down

layout(location = 0) out vec4 fragColor;

void main() {
    ivec2 o = ivec2(range.xy), n = ivec2(range.zw);
    float m = 0.0, peak = 0.0;
    for (int y = 0; y < n.y; y++)
        for (int x = 0; x < n.x; x++)
        {
            float c = texelFetch(tex, o + ivec2(x, y), 0).r;
            m += c * c;
            peak = max(peak, c);
        }
    // (R: root mean square, as the CPU's coverage, led by what is drawn solid;
    // G: the strongest cell, as the CPU's presence)
    m = sqrt(m / float(n.x * n.y));
    fragColor = vec4(m, peak, m, 1.0);
}
)GLSL"},

    // The settled card, kept for its close: a copy of the content's box from the
    // layer's work buffer, written only while this frame's content is fully
    // visible (its live presence, read on the GPU in the same frame), so the
    // first frame of a fade never replaces it.
    {"copy.frag", R"GLSL(
#version 300 es
precision highp float;

uniform sampler2D tex;         // a window's kept glass (GlassRenderer::drawOutputCache)

in vec2 v_texcoord;
layout(location = 0) out vec4 fragColor;

void main() {
    fragColor = texture(tex, v_texcoord);
}
)GLSL"},

    {"hold.frag", R"GLSL(
#version 300 es
precision highp float;

uniform sampler2D tex;         // the layer's work buffer
uniform vec2 maskUVOffset;
uniform vec2 maskUVScale;
uniform float presenceRef;
uniform sampler2D presenceTex; // unit 1

in vec2 v_texcoord;
layout(location = 0) out vec4 fragColor;

void main() {
    // (gated on the strongest cell, G: a fade dims it, new content doesn't; the
    // coverage, R, moves with every change of content and left the copy unwritten)
    if (texture(presenceTex, vec2(0.5)).g < 0.97 * presenceRef) discard;
    fragColor = texture(tex, v_texcoord * maskUVScale + maskUVOffset);
}
)GLSL"},

    {"gaussianblur.frag", R"GLSL(
#version 300 es
precision highp float;

uniform sampler2D tex;
uniform vec2 direction; // (1.0/width, 0.0) for horizontal, (0.0, 1.0/height) for vertical
uniform float blurRadius; // kernel radius in pixels

in vec2 v_texcoord;
layout(location = 0) out vec4 fragColor;

void main() {
    // Compute sigma from radius (covers ~3 sigma)
    float sigma = max(blurRadius / 3.0, 0.001);
    float invSigma2 = -0.5 / (sigma * sigma);

    int samples = min(int(ceil(blurRadius)), 8);

    // Center tap, clamped: an out-of-range texel from a float framebuffer would otherwise dominate the kernel
    float w0 = 1.0;
    vec4 result = clamp(texture(tex, v_texcoord), 0.0, 1.0) * w0;
    float totalWeight = w0;

    // Linear sampling: pair adjacent taps (i, i+1) into a single bilinear fetch.
    // The interpolated offset between two texels yields their weighted average
    // in one texture() call, halving the total tap count.
    for (int i = 1; i <= samples; i += 2) {
        float x1 = float(i);
        float x2 = float(i + 1);
        float w1 = exp(x1 * x1 * invSigma2);
        float w2 = (i + 1 <= samples) ? exp(x2 * x2 * invSigma2) : 0.0;
        float wSum = w1 + w2;
        if (wSum < 0.0001) continue;

        // Offset biased toward the heavier weight
        float offset = (x1 * w1 + x2 * w2) / wSum;

        result += clamp(texture(tex, v_texcoord + direction * offset), 0.0, 1.0) * wSum;
        result += clamp(texture(tex, v_texcoord - direction * offset), 0.0, 1.0) * wSum;
        totalWeight += 2.0 * wSum;
    }

    fragColor = result / totalWeight;
}
)GLSL"},
};
