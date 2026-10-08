#include "Diagnostics.hpp"
#include "Frost.hpp"
#include "GlassRenderer.hpp"
#include "Globals.hpp"
#include "WindowGeometry.hpp"

#include <algorithm>
#include <cmath>
#include <GLES3/gl32.h>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/gl/GLFramebuffer.hpp>

namespace Frost {

// (a window counts behind `self` when it is drawn before it: tiled under floating
// under pinned, else lower in the stack)
static bool drawnBelow(const PHLWINDOW& other, size_t otherIdx, const PHLWINDOW& self, size_t selfIdx) {
    const int orank = other->m_pinned ? 2 : other->m_isFloating ? 1 : 0, srank = self->m_pinned ? 2 : self->m_isFloating ? 1 : 0;
    if (orank != srank)
        return orank < srank;
    return otherIdx < selfIdx;
}

bool drawnUnder(const PHLWINDOW& other, const PHLWINDOW& self) {
    if (!other || !self || other == self)
        return false;
    const auto& windows = Desktop::windowState()->windows();
    size_t oi = windows.size(), si = windows.size();
    for (size_t i = 0; i < windows.size(); i++) {
        if (windows[i] == other) oi = i;
        if (windows[i] == self) si = i;
    }
    return drawnBelow(other, oi, self, si);
}

float coveredFraction(PHLMONITOR monitor, const CBox& targetT, PHLWINDOW self) {
    if (!monitor || targetT.w <= 0.0 || targetT.h <= 0.0)
        return 0.0f;
    const auto& windows = Desktop::windowState()->windows();
    size_t selfIdx = windows.size();
    if (self)
        for (size_t i = 0; i < windows.size(); i++)
            if (windows[i] == self) { selfIdx = i; break; }

    double area = 0.0;
    for (size_t i = 0; i < windows.size(); i++) {
        const auto& w = windows[i];
        if (!w || w == self || !w->m_isMapped || w->isHidden() || !w->visibleByAlpha() || w->m_monitor.lock() != monitor)
            continue;
        if (!w->m_workspace || !w->m_workspace->isVisible())
            continue;
        if (self && !drawnBelow(w, i, self, selfIdx))
            continue;
        const auto box = WindowGeometry::computeWindowBox(w, monitor);
        if (!box)
            continue;
        const CBox t = WindowGeometry::applyMonitorTransform(*box, monitor).intersection(targetT);
        if (t.w > 0.0 && t.h > 0.0)
            area += t.w * t.h;
    }
    return static_cast<float>(std::clamp(area / (targetT.w * targetT.h), 0.0, 1.0));
}

float amountFor(float setting, float covered) {
    const float s  = std::clamp(setting, 0.0f, 1.0f);
    const float lo = std::max(0.0f, 2.0f * s - 1.0f), hi = std::min(1.0f, 2.0f * s);
    // a sliver of a window behind the edge is not "over windows"; most of it is
    const float c  = std::clamp((covered - 0.04f) / (0.45f - 0.04f), 0.0f, 1.0f);
    return lo + (hi - lo) * (c * c * (3.0f - 2.0f * c));
}

bool CState::step(float target) {
    const auto now = std::chrono::steady_clock::now();
    if (!m_started) {
        // the first frame starts where it should be (a card opening over windows
        // opens frosted, it doesn't frost as it opens)
        m_started = true;
        m_at      = now;
        m_amount  = target;
        return false;
    }
    // (after a still spell, nothing drawn, the change starts now: one frame's step,
    // not the whole spell's; clamped to 0.1 s, it jumped two thirds in one frame)
    float dt = std::chrono::duration<float>(now - m_at).count();
    dt       = dt > 0.05f || dt < 0.0f ? 1.0f / 60.0f : dt;
    m_at = now;
    if (std::abs(target - m_amount) < 0.004f) {
        m_amount = target;
        return false;
    }
    m_amount += (target - m_amount) * (1.0f - std::exp(-dt / 0.09f)); // ~0.25 s to settle
    return true;
}

void CState::prepare(SP<Render::IFramebuffer>& sample, float monitorScale, const SP<Render::IFramebuffer>& caller, SFrost& out) {
    out = {};
    const auto& config = g_pGlobalState->config;
    if (m_amount < 0.002f || !sample || !sample->isAllocated() || !caller)
        return;
    const int w = std::max(1, static_cast<int>(sample->m_size.x) / 2), h = std::max(1, static_cast<int>(sample->m_size.y) / 2);
    if (!m_fb)
        m_fb = g_pHyprRenderer->createFB("hyprglass-frost");
    if (m_fb->m_size.x != w || m_fb->m_size.y != h || m_fb->m_drmFormat != sample->m_drmFormat) {
        if (!m_fb->alloc(w, h, sample->m_drmFormat))
            return;
        m_fbValid = false;
    }
    if (!m_fbValid) {
        // half size (linear), then blurred at half the radius
        auto* src = dynamic_cast<Render::GL::CGLFramebuffer*>(sample.get());
        auto* dst = dynamic_cast<Render::GL::CGLFramebuffer*>(m_fb.get());
        g_pHyprOpenGL->setCapStatus(GL_SCISSOR_TEST, false);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, src->getFBID());
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst->getFBID());
        glBlitFramebuffer(0, 0, static_cast<int>(sample->m_size.x), static_cast<int>(sample->m_size.y), 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glBindFramebuffer(GL_FRAMEBUFFER, dynamic_cast<Render::GL::CGLFramebuffer*>(caller.get())->getFBID());
        const float radius = std::max(1.0f, (config.frostBlur ? static_cast<float>(**config.frostBlur) : 20.0f) * monitorScale * 0.5f);
        GlassRenderer::blurBackground(m_fb, radius, 2, caller);
        m_fbValid = true;
    }
    out.textureId = m_fb->getTexture()->m_texID;
    out.amount    = m_amount;
    out.milk      = config.frostMilk ? std::clamp(static_cast<float>(**config.frostMilk), 0.0f, 1.0f) : 0.3f;
}

} // namespace Frost
