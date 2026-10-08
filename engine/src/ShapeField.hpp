#pragma once

#include <algorithm>
#include <chrono>

#include <GLES3/gl32.h>
#include <hyprland/src/render/Framebuffer.hpp>
#include <hyprutils/math/Box.hpp>
#include <hyprutils/math/Vector2D.hpp>
#include <optional>
#include <vector>

// Distance field of a layer surface's own drawn shape. The glass then
// follows whatever outline the client draws (pills, cards, popups) instead of
// the layer's rectangle: its rim, refraction and highlights come from this field.
namespace ShapeField {

// Seeds from `maskTexture` (sampled at box UV * maskUVScale + maskUVOffset,
// exactly as the glass shader samples it), jump-floods, and resolves into
// `out` (width x height, RGBA16F: R/G = distance to the outline from inside /
// outside in px, clamped to maxDist; B = text coverage, softened). false when
// it can't be built (e.g. no half-float render targets).
// Leaves `callerFramebuffer` bound with its viewport, as blurBackground() does.
// sigma: how far the corner creases are smoothed, px (from the rim's width: tied to
// maxDist, which the drop shadow sets, it planed a small pill's round ends into cuts)
// peakTexture: when not 0, a 1x1 texture whose G is this frame's strongest content
// (CContentProbe::drawPeak); the threshold is then relative to it
bool build(GLuint maskTexture, const Vector2D& maskUVOffset, const Vector2D& maskUVScale, int width, int height,
           float threshold, float maxDist, float sigma, SP<Render::IFramebuffer>& ping, SP<Render::IFramebuffer>& pong,
           SP<Render::IFramebuffer>& out, const SP<Render::IFramebuffer>& callerFramebuffer, GLuint peakTexture = 0);

// Copy the content's box (maskUVOffset/Scale into the layer's work buffer) into
// `out` (width x height), but only where this frame's live presence is at least
// 95 % of `presenceRef`: the settled card a close is drawn from. `clear` empties
// `out` first (a fresh map must not show the last card).
bool hold(GLuint maskTexture, const Vector2D& maskUVOffset, const Vector2D& maskUVScale, int width, int height,
          GLuint presenceTexture, float presenceRef, bool clear, SP<Render::IFramebuffer>& out,
          const SP<Render::IFramebuffer>& callerFramebuffer);

// An adaptive bar's backdrop brightness along its length (barstyle.frag): one
// texel per 8 px of `length`, into `out` (RGBA8). Call after each resample.
// Leaves `callerFramebuffer` bound with its viewport.
bool buildStyleStrip(GLuint sampleTexture, const Vector2D& padding, bool across, int length, float halfWindowPx,
                     SP<Render::IFramebuffer>& out, const SP<Render::IFramebuffer>& callerFramebuffer);

// What CContentProbe measures: the bounding box (box-local px, y as the glass
// shader's uv) of everything drawn in the mask over a width x height box, at CELL
// px resolution (conservative: whole cells), and how visible it is.
inline constexpr int CELL = 16;
struct SContent {
    CBox  box;      // box-local px, whole cells
    float presence; // strongest alpha drawn (0..1): how far a fading card has come in
    // the cells' alpha over `box`, root mean square: a stable measure of a fade
    // (presence.frag reads the same), linear in a uniform fade, and led by what is
    // drawn solid (a card's nearly clear fill, on most of its cells, diluted a plain
    // mean: the weather's text at 87 % read as 99 % there)
    float coverage;
    // Fading against the last fully visible frame (keepReference), cell by cell:
    // what is left of what was solid then (1: nothing faded). Only a frame in
    // which no cell appeared or brightened counts: hovering, typing, a list
    // filtering add cells as others go, and are not a fade. The weather fades its
    // text before its opaque pills, which kept the strongest cell at full.
    float fadeLeft = 1.0f;
    // something new is drawn (cells that appeared or brightened since the reference)
    bool  fresh = false;
};

// Where a layer's content is, read back without stalling: request() renders the
// cells and starts an asynchronous readback into a pixel buffer behind a fence;
// poll() returns the result once the GPU has finished it (a frame or so later).
// A synchronous glReadPixels would make the CPU wait for the whole frame so far,
// which costs real time on integrated GPUs every time a card fades.
class CContentProbe {
  public:
    CContentProbe() = default;
    ~CContentProbe();
    CContentProbe(const CContentProbe&)            = delete;
    CContentProbe& operator=(const CContentProbe&) = delete;

    void request(GLuint maskTexture, const Vector2D& maskUVOffset, const Vector2D& maskUVScale, int width, int height, float threshold,
                 const SP<Render::IFramebuffer>& callerFramebuffer);
    // How visible the content is in this very frame, into a 1x1 texture the glass
    // reads (no readback): the strongest cell over `contentLocal` (box-local px).
    // Leaves `callerFramebuffer` bound with its viewport.
    bool drawLive(GLuint maskTexture, const Vector2D& maskUVOffset, const Vector2D& maskUVScale, int width, int height, float threshold,
                  const CBox& contentLocal, const SP<Render::IFramebuffer>& callerFramebuffer);
    [[nodiscard]] GLuint liveTexture() const { return m_live && m_live->isAllocated() ? m_live->getTexture()->m_texID : 0; }
    // This frame's strongest content over contentLocal, into its own 1x1 texture (G),
    // for an outline traced from this same frame (build's peakTexture). 0 on failure.
    GLuint drawPeak(GLuint maskTexture, const Vector2D& maskUVOffset, const Vector2D& maskUVScale, int width, int height, const CBox& contentLocal,
                    const SP<Render::IFramebuffer>& callerFramebuffer);

    // the last result's cells become what "fully visible" is compared against
    void keepReference() {
        m_refCells = m_lastCells;
        m_refW     = m_lastW;
        m_refH     = m_lastH;
    }
    // (a card still coming in has no fully visible frame yet: each cell's
    // brightest so far, so a close before it lands reads as the fade it is)
    void keepBrightest() {
        if (m_refW != m_lastW || m_refH != m_lastH || m_refCells.size() != m_lastCells.size())
            return keepReference();
        for (size_t i = 0; i < m_refCells.size(); i++)
            m_refCells[i] = std::max(m_refCells[i], m_lastCells[i]);
    }
    // nullopt: nothing new yet; a value: the newest result (its inner nullopt: nothing drawn)
    std::optional<std::optional<SContent>> poll();
    [[nodiscard]] bool pending() const { return m_fence != nullptr; }
    // wait (at most `ms`) for the request in flight: its result is ready for poll()
    void finish(int ms);
    // when the frame the last result (poll) measured was drawn
    [[nodiscard]] std::chrono::steady_clock::time_point resultFrameAt() const { return m_resultAt; }

  private:
    SP<Render::IFramebuffer> m_cells;
    std::vector<uint8_t>     m_lastCells, m_refCells; // the last readback's cells; the reference
    int                      m_lastW = 0, m_lastH = 0, m_refW = 0, m_refH = 0;
    SP<Render::IFramebuffer> m_liveCells, m_live, m_peakCells, m_peak;
    GLuint                   m_pbo   = 0;
    GLsync                   m_fence = nullptr;
    int                      m_cw = 0, m_ch = 0;
    std::chrono::steady_clock::time_point m_requestAt{}, m_resultAt{};
};

// The exact box of the outline a field describes (field px, sub-pixel): where
// its signed distance crosses zero along the middle row and column. The probe's
// box is in whole 16 px cells; a motion aimed at it ended up to 15 px off the
// card's real edges and glided there as it handed over. Reads two lines back
// (synchronously: once per field built). false: no crossing found.
bool exactBox(const SP<Render::IFramebuffer>& field, CBox& out, const SP<Render::IFramebuffer>& callerFramebuffer);

// The run of a colour (rgb, 0xRRGGBB) along a bar drawn into fb (rect in fb px,
// top-left origin; tried both ways up), nearest to `anchor` (along the bar, fb px)
// within `reach`: Omarchy's bar underlines the item whose panel is open in the
// accent colour, so this is the clicked item's extent. Synchronous (once per open).
bool accentSpan(const SP<Render::IFramebuffer>& fb, const CBox& rect, bool alongX, double anchor, double reach, uint32_t rgb, double& lo, double& hi,
                const SP<Render::IFramebuffer>& callerFramebuffer);

} // namespace ShapeField
