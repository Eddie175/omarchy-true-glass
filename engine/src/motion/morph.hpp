// motion/morph.hpp — a shape that morphs out of a source and back (the Dynamic
// Island's way of opening a card), as channels of springs that can be
// interrupted at any moment.
//
// Each channel is one spring-driven number. Showing and hiding each run a short
// script per channel: steps of (delay, spring, target), every step taking over
// from wherever the channel is, at the speed it has (SwiftUI's retargeting). An
// open interrupted by a close is not a special case: the hide script simply
// takes over every channel from where the show left it.
//
// What the channels mean is up to the spec (motion/tokens.hpp): for a card they
// are its edges' progress from the source to the card, the source drop's size,
// the content's scale, opacity and the glass forming. The shape and the content
// are separate channels on purpose: the shape morphs (each edge on its own
// spring: a drip, a lead, a lag), the content only ever scales uniformly, so
// text is never squashed or stretched.
//
// Everything is an exact function of time; advance(t) applies the steps that
// are due, so ask with non-decreasing times.
#pragma once

#include "spring.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace motion {

struct Step {
    double delay  = 0.0;   // s after show()/hide() that this step starts
    Spring spring = Spring::smooth(0.3);
    double target = 1.0;
};

// One channel's scripts. Up to MAX_STEPS steps each, in order of delay.
struct Track {
    static constexpr int MAX_STEPS = 3;
    std::array<Step, MAX_STEPS> show{}, hide{};
    int    showCount = 0, hideCount = 0;
    double hidden    = 0.0; // where a fresh show() starts it

    static Track make(double hidden, std::initializer_list<Step> show, std::initializer_list<Step> hide) {
        Track t;
        t.hidden = hidden;
        for (const auto& s : show)
            if (t.showCount < MAX_STEPS) t.show[t.showCount++] = s;
        for (const auto& s : hide)
            if (t.hideCount < MAX_STEPS) t.hide[t.hideCount++] = s;
        return t;
    }
    // where the channel ends up when shown / hidden (its script's last target)
    [[nodiscard]] double shownValue() const { return showCount ? show[showCount - 1].target : hidden; }
    [[nodiscard]] double hiddenValue() const { return hideCount ? hide[hideCount - 1].target : hidden; }
};

struct MorphSpec {
    static constexpr int MAX_CHANNELS = 8;
    std::array<Track, MAX_CHANNELS> tracks{};
    int count = 0;
};

class Morph {
  public:
    explicit Morph(const MorphSpec& spec = {}) { setSpec(spec, 0.0); }

    // A new spec. Channels keep where they are and how fast they go; the new
    // scripts apply from the next show()/hide().
    void setSpec(const MorphSpec& spec, double t) {
        const int old = m_spec.count;
        m_spec = spec;
        for (int i = old; i < spec.count; i++)
            m_ch[i].start(Spring::smooth(), spec.tracks[i].hidden, spec.tracks[i].hidden, 0.0, t);
    }
    [[nodiscard]] const MorphSpec& spec() const { return m_spec; }

    // Appear, from wherever it is; fresh: from fully hidden, at rest. Showing what
    // is already showing changes nothing (safe to call every frame).
    void show(double t, bool fresh = false) {
        if (m_shown && !fresh)
            return;
        m_shown = true;
        for (int i = 0; i < m_spec.count; i++) {
            if (fresh)
                m_ch[i].start(Spring::smooth(), m_spec.tracks[i].hidden, m_spec.tracks[i].hidden, 0.0, t);
            run(i, t, m_spec.tracks[i].show, m_spec.tracks[i].showCount);
        }
    }
    // Disappear, from wherever it is. Hiding what is already hiding changes nothing.
    void hide(double t) {
        if (!m_shown)
            return;
        m_shown = false;
        for (int i = 0; i < m_spec.count; i++)
            run(i, t, m_spec.tracks[i].hide, m_spec.tracks[i].hideCount);
    }
    // At rest, fully shown, from t (something already there when the motion takes it over).
    void snapShown(double t) {
        m_shown = true;
        for (int i = 0; i < m_spec.count; i++) {
            m_pending[i] = {};
            const double v = m_spec.tracks[i].shownValue();
            m_ch[i].start(Spring::smooth(), v, v, 0.0, t);
        }
    }

    // Channel c is at v from t (something outside the motion already took it there:
    // a client's own fade that began before the motion took over): it stays there
    // until its next step, which then applies from there.
    void setValue(int c, double v, double t) {
        if (c < 0 || c >= m_spec.count)
            return;
        advanceChannel(c, t);
        // (held there until its next step, if one is still to come; else on its way
        // to where it was going, on the same spring: never back the way it came)
        const bool pending = m_pending[c].next < m_pending[c].count;
        m_ch[c].start(m_ch[c].spring(), v, pending ? v : m_ch[c].target(), 0.0, t);
    }

    // Channel c goes to `to` on `spring` from t, from where it is at the speed it
    // has, and its script's steps still to come are dropped (a close that turns
    // into a different one part way: the card fades where it is instead).
    void retarget(int c, const Spring& spring, double to, double t) {
        if (c < 0 || c >= m_spec.count)
            return;
        advanceChannel(c, t);
        m_pending[c] = {};
        m_ch[c].respring(spring, to, t);
    }

    // apply the steps due by t (call before reading at t)
    void advance(double t) {
        for (int i = 0; i < m_spec.count; i++)
            advanceChannel(i, t);
    }

    [[nodiscard]] double value(int c, double t) const { return m_ch[c].value(t); }
    [[nodiscard]] double velocity(int c, double t) const { return m_ch[c].velocity(t); }
    [[nodiscard]] bool   shown() const { return m_shown; }

    // done moving: no step left to start and every channel settled
    [[nodiscard]] bool settled(double t, double eps = 1e-3) const {
        for (int i = 0; i < m_spec.count; i++)
            if (m_pending[i].count || !m_ch[i].settled(t, eps))
                return false;
        return true;
    }
    [[nodiscard]] bool gone(double t) const { return !m_shown && settled(t); }
    // when it will have settled, as things stand (for scheduling, tests)
    [[nodiscard]] double settleTime(double eps = 1e-3) const {
        double end = 0.0;
        for (int i = 0; i < m_spec.count; i++) {
            SpringMotion m = m_ch[i];
            const auto&  p = m_pending[i];
            for (int k = 0; k < p.count; k++)
                m.respring(p.steps[k].spring, p.steps[k].target, p.at[k]);
            end = std::max(end, m.settleTime(eps));
        }
        return end;
    }

    // The lowest and highest value channel c takes from t on (for drawing
    // bounds), exact as long as show() and hide() aren't called again: the
    // running spring up to the next step, and each step from the state it
    // takes over in.
    [[nodiscard]] std::pair<double, double> range(int c, double t) const {
        SpringMotion m  = m_ch[c];
        const auto&  p  = m_pending[c];
        double       lo = std::numeric_limits<double>::infinity(), hi = -lo, from = t;
        for (int k = 0; k < p.count; k++) {
            if (p.at[k] > from) {
                const auto r = m.range(from, p.at[k]);
                lo = std::min(lo, r.first);
                hi = std::max(hi, r.second);
            }
            m.respring(p.steps[k].spring, p.steps[k].target, p.at[k]);
            from = std::max(from, p.at[k]);
        }
        const auto r = m.range(from);
        return {std::min(lo, r.first), std::max(hi, r.second)};
    }

  private:
    struct Pending {
        std::array<Step, Track::MAX_STEPS>   steps{};
        std::array<double, Track::MAX_STEPS> at{};
        int count = 0, next = 0; // steps[next..count) still to start
    };
    MorphSpec                                       m_spec;
    std::array<SpringMotion, MorphSpec::MAX_CHANNELS> m_ch{};
    std::array<Pending, MorphSpec::MAX_CHANNELS>      m_pending{};
    bool m_shown = false;

    void advanceChannel(int i, double t) {
        auto& p = m_pending[i];
        while (p.next < p.count && p.at[p.next] <= t) {
            m_ch[i].respring(p.steps[p.next].spring, p.steps[p.next].target, p.at[p.next]); // (exactly when it was due)
            p.next++;
        }
        if (p.next >= p.count)
            p = {};
    }
    // a new script for channel i, decided at `now` (replacing whatever was still
    // to come: the channel goes on as it was until its first step)
    void run(int i, double now, const std::array<Step, Track::MAX_STEPS>& steps, int n) {
        advanceChannel(i, now);
        m_pending[i] = {};
        auto& p = m_pending[i];
        for (int k = 0; k < n; k++) {
            const double at = now + steps[k].delay;
            if (at <= now && p.count == 0)
                m_ch[i].respring(steps[k].spring, steps[k].target, now);
            else {
                p.steps[p.count] = steps[k];
                p.at[p.count]    = std::max(at, now);
                p.count++;
            }
        }
        // (a pending list's range() walk relies on the starts being in order)
        for (int a = 1; a < p.count; a++)
            for (int b = a; b > 0 && p.at[b] < p.at[b - 1]; b--) {
                std::swap(p.at[b], p.at[b - 1]);
                std::swap(p.steps[b], p.steps[b - 1]);
            }
    }
};

} // namespace motion
