// motion/presence.hpp — something that appears and disappears (a card out of
// the bar, a tooltip, a toast), as a set of springs that can be interrupted.
//
// A Presence has channels (width scale, height scale, content opacity, glass
// opacity), each its own spring with its own delay for showing and for hiding.
// show() and hide() may be called at any moment: every channel turns from where
// it is, at the speed it has, toward its new target (SwiftUI's retargeting). An
// open interrupted by a close is not a special case, so it can't go wrong.
//
// Hiding may begin with a breath in (anticipation): the scales swell a little
// for `anticipation.time` before they leave.
//
// Everything is exact functions of time; advance(t) applies the delayed starts
// that are due, so ask with non-decreasing times.
#pragma once

#include "spring.hpp"

#include <algorithm>
#include <array>
#include <optional>

namespace motion {

struct Leg {
    Spring spring = Spring::smooth(0.3);
    double delay  = 0.0;   // s before this channel starts moving
};

struct ChannelSpec {
    Leg show, hide;
    double shown  = 1.0;   // the value when present
    double hidden = 0.0;   // ... and when gone
};

struct PresenceSpec {
    // [0] scale across (along the edge it comes out of), [1] scale down (away
    // from it), [2] content opacity, [3] glass opacity
    std::array<ChannelSpec, 4> channels;
    struct {
        double amount = 0.0; // swell, as a scale fraction (0.02 = 2 %)
        double time   = 0.09;
    } anticipation;
};

enum Channel : int { ScaleAlong = 0, ScaleAway = 1, Content = 2, Glass = 3 };

class Presence {
  public:
    explicit Presence(const PresenceSpec& spec = {}) : m_spec(spec) {
        for (int i = 0; i < 4; i++)
            m_ch[i].start(Spring::smooth(), spec.channels[i].hidden, spec.channels[i].hidden, 0.0, 0.0);
    }

    void setSpec(const PresenceSpec& spec) { m_spec = spec; }
    [[nodiscard]] const PresenceSpec& spec() const { return m_spec; }

    // Appear (from wherever it is). Jump: start from fully hidden (a fresh appearance).
    // Showing what is already showing changes nothing (so it can be called every
    // frame without restarting the delays).
    void show(double t, bool fromHidden = false) {
        if (m_shown && !fromHidden)
            return;
        m_shown = true;
        for (int i = 0; i < 4; i++) {
            const auto& c = m_spec.channels[i];
            if (fromHidden)
                m_ch[i].start(c.show.spring, c.hidden, c.hidden, 0.0, t);
            schedule(i, t, t + c.show.delay, c.show.spring, c.shown);
        }
    }
    // Disappear (from wherever it is). Hiding what is already hiding changes nothing.
    void hide(double t) {
        if (!m_shown)
            return;
        m_shown = false;
        const double a = m_spec.anticipation.amount, at = a > 0.0 ? m_spec.anticipation.time : 0.0;
        for (int i = 0; i < 4; i++) {
            const auto& c = m_spec.channels[i];
            if (a > 0.0 && i <= ScaleAway) {
                // a breath in: toward a little larger, then away (from that state,
                // with its speed: one continuous motion). The swell is in
                // proportion to how far out it is: a card barely out (a show
                // undone at once, or still waiting on its delay) doesn't puff up
                // to full size on its way back in.
                advanceChannel(i, t);
                const double span = c.shown - c.hidden;
                const double out  = span != 0.0 ? std::clamp((m_ch[i].value(t) - c.hidden) / span, 0.0, 1.0) : 1.0;
                m_ch[i].respring(Spring::fromDuration(at * 2.0, 0.0), c.hidden + (c.shown * (1.0 + a) - c.hidden) * out, t);
                m_pending[i] = Pending{t + std::max(at, c.hide.delay), c.hide.spring, c.hidden};
            } else
                schedule(i, t, t + c.hide.delay + (i <= ScaleAway ? 0.0 : at), c.hide.spring, c.hidden);
        }
    }

    // At rest, fully shown, from time t (something already present when the
    // motion takes it over).
    void snapShown(double t) {
        m_shown = true;
        for (int i = 0; i < 4; i++) {
            m_pending[i].reset();
            m_ch[i].start(m_spec.channels[i].show.spring, m_spec.channels[i].shown, m_spec.channels[i].shown, 0.0, t);
        }
    }

    // apply delayed starts due by t (call before reading at t)
    void advance(double t) {
        for (int i = 0; i < 4; i++)
            advanceChannel(i, t);
    }

    [[nodiscard]] double value(Channel c, double t) const { return m_ch[c].value(t); }
    [[nodiscard]] double velocity(Channel c, double t) const { return m_ch[c].velocity(t); }
    [[nodiscard]] bool   shown() const { return m_shown; }

    // done moving: no delayed start left and every channel settled
    [[nodiscard]] bool settled(double t, double eps = 1e-3) const {
        for (int i = 0; i < 4; i++)
            if (m_pending[i] || !m_ch[i].settled(t, eps))
                return false;
        return true;
    }
    [[nodiscard]] bool gone(double t) const { return !m_shown && settled(t); }

    // The largest scale either axis reaches from t on (for drawing bounds),
    // exact as long as show() and hide() aren't called again: the running leg up
    // to when a delayed one takes over, and that one from the state it takes
    // over in.
    [[nodiscard]] double maxScale(double t) const {
        double hi = 0.0;
        for (int i = 0; i <= ScaleAway; i++) {
            if (!m_pending[i]) {
                hi = std::max(hi, m_ch[i].range(t).second);
                continue;
            }
            const Pending& p = *m_pending[i];
            SpringMotion next = m_ch[i];
            next.respring(p.spring, p.target, p.at); // (what advanceChannel will do)
            if (p.at > t)
                hi = std::max(hi, m_ch[i].range(t, p.at).second);
            hi = std::max(hi, next.range(std::max(t, p.at)).second);
        }
        return hi;
    }

  private:
    struct Pending {
        double at;
        Spring spring;
        double target;
    };
    PresenceSpec           m_spec;
    std::array<SpringMotion, 4> m_ch{};
    std::array<std::optional<Pending>, 4> m_pending{};
    bool m_shown = false;

    void advanceChannel(int i, double t) {
        if (m_pending[i] && m_pending[i]->at <= t) {
            const auto p = *m_pending[i];
            m_pending[i].reset();
            m_ch[i].respring(p.spring, p.target, p.at); // (exactly when it was due)
        }
    }
    // a new leg for channel i, decided at `now`, starting at `at` (a newer
    // decision replaces one still waiting: the channel goes on as it was meanwhile)
    void schedule(int i, double now, double at, const Spring& s, double target) {
        advanceChannel(i, now);
        m_pending[i].reset();
        if (at <= now)
            m_ch[i].respring(s, target, now);
        else
            m_pending[i] = Pending{at, s, target};
    }
};

} // namespace motion
