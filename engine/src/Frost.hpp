#pragma once

// Frost: legibility without giving up clearness. Glass over the wallpaper stays
// clear; glass over other windows (a card over tiled terminals, a floating window
// over a browser) frosts, by a per-kind setting: 0 always clear, 1 always
// frosted, between: frosted by how much of the glass lies over windows (0.5: from
// clear to fully frosted). The amount eases, so a window sliding behind a card
// frosts it smoothly. The frosted backdrop is a half-resolution blurred copy of
// the cached sample, built only when some frost is wanted.

#include <chrono>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/render/Framebuffer.hpp>
#include <hyprutils/math/Box.hpp>

namespace Frost {

// What the glass shader needs (unit 6)
struct SFrost {
    unsigned textureId = 0;
    float    amount    = 0.0f;
    float    milk      = 0.0f;
};

// How much of `targetT` (the glass, in the monitor's transformed framebuffer px:
// a glass quad's transformBox space) is covered by windows drawn behind it.
// self: the glass's own window (only windows below it count), nullptr for a
// card above every window.
[[nodiscard]] float coveredFraction(PHLMONITOR monitor, const CBox& targetT, PHLWINDOW self);

// Whether `other` is drawn before (underneath) `self`: tiled under floating under
// pinned, then by stacking order. Only such a window can be behind self's glass.
[[nodiscard]] bool drawnUnder(const PHLWINDOW& other, const PHLWINDOW& self);

// The frost wanted for a setting and a coverage
[[nodiscard]] float amountFor(float setting, float covered);

class CState {
  public:
    // eases toward `target`; true while it still moves (the caller keeps frames coming)
    bool step(float target);
    [[nodiscard]] float amount() const { return m_amount; }

    // the cached sample was taken again: the frosted copy is stale
    void sampleChanged() { m_fbValid = false; }
    // builds the frosted copy from `sample` when frost is wanted and it is stale;
    // fills `out` (amount 0 and no texture when there is none)
    void prepare(SP<Render::IFramebuffer>& sample, float monitorScale, const SP<Render::IFramebuffer>& caller, SFrost& out);

  private:
    float                                 m_amount = 0.0f;
    bool                                  m_started = false;
    std::chrono::steady_clock::time_point m_at{};
    SP<Render::IFramebuffer>              m_fb;
    bool                                  m_fbValid = false;
};

} // namespace Frost
