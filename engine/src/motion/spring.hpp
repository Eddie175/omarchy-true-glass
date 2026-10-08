// motion/spring.hpp — damped springs, solved exactly (closed form).
//
// A spring pulls a value toward a target. Its state at any time is computed
// from the time alone (no stepping), so it is the same at any frame rate and
// can be asked for any moment, past or future, at the cost of a few exp/cos.
//
// Parameters follow SwiftUI's Spring (WWDC23 "Animate with springs"):
//   duration  the perceptual duration: how long the change feels (s), and
//   bounce    0 = no overshoot (critically damped), up to ~0.4 playful,
//             negative = overdamped (slower to arrive, no overshoot).
// with mass 1, stiffness = (2 pi / duration)^2, and damping
//   (1 - bounce) * 4 pi / duration      for bounce >= 0,
//   4 pi / (duration * (1 + bounce))    for bounce <  0
// (the corrected form; the WWDC slide had a typo, see docs/sources.md), so the
// damping ratio is 1 - bounce for bounce >= 0. Also constructible from
// response/dampingRatio and from mass/stiffness/damping, as SwiftUI and WebKit's
// SpringSolver are; checked against it to 1e-12.
//
// A Spring is the rule; a SpringMotion is one run of it: from a value with a
// velocity toward a target, started at a time. Retargeting mid-flight keeps the
// position and the velocity (SwiftUI's behavior), so an interrupted motion turns
// smoothly instead of restarting.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace motion {

inline constexpr double PI = 3.14159265358979323846;

struct Spring {
    double mass      = 1.0;
    double stiffness = 157.91367041742973; // duration 0.5
    double damping   = 25.132741228718345; // bounce 0

    // SwiftUI: Spring(duration:bounce:)
    static Spring fromDuration(double duration, double bounce = 0.0) {
        duration = std::max(duration, 1e-4);
        bounce   = std::clamp(bounce, -1.0 + 1e-6, 1.0 - 1e-6);
        Spring s;
        s.mass      = 1.0;
        s.stiffness = std::pow(2.0 * PI / duration, 2.0);
        s.damping   = bounce >= 0.0 ? (1.0 - bounce) * 4.0 * PI / duration : 4.0 * PI / (duration * (1.0 + bounce));
        return s;
    }
    // SwiftUI: Spring(response:dampingRatio:)
    static Spring fromResponse(double response, double dampingRatio) {
        response = std::max(response, 1e-4);
        Spring s;
        s.mass      = 1.0;
        s.stiffness = std::pow(2.0 * PI / response, 2.0);
        s.damping   = 4.0 * PI * std::max(dampingRatio, 0.0) / response; // (as fromPhysics: no negative damping)
        return s;
    }
    static Spring fromPhysics(double mass, double stiffness, double damping) {
        return Spring{std::max(mass, 1e-9), std::max(stiffness, 1e-9), std::max(damping, 0.0)};
    }

    // SwiftUI's named springs (documented defaults: duration 0.5)
    static Spring smooth(double duration = 0.5) { return fromDuration(duration, 0.0); }
    static Spring snappy(double duration = 0.5) { return fromDuration(duration, 0.15); }
    static Spring bouncy(double duration = 0.5) { return fromDuration(duration, 0.3); }

    [[nodiscard]] double omega0() const { return std::sqrt(stiffness / mass); }
    [[nodiscard]] double dampingRatio() const { return damping / (2.0 * std::sqrt(stiffness * mass)); }
    [[nodiscard]] double duration() const { return 2.0 * PI / omega0(); }
    [[nodiscard]] double bounce() const {
        const double z = dampingRatio();
        return z <= 1.0 ? 1.0 - z : 1.0 / z - 1.0; // inverse of fromDuration (mass 1)
    }
    // the largest overshoot past the target, as a fraction of the travel, for a
    // motion starting at rest (0 when not underdamped)
    [[nodiscard]] double overshoot() const {
        const double z = dampingRatio();
        return z >= 1.0 ? 0.0 : std::exp(-PI * z / std::sqrt(1.0 - z * z));
    }
};

// One run of a spring: x(t) is the displacement from the target, solved exactly.
class SpringMotion {
  public:
    SpringMotion() = default;
    SpringMotion(const Spring& spring, double from, double to, double velocity = 0.0, double startTime = 0.0) {
        start(spring, from, to, velocity, startTime);
    }

    void start(const Spring& spring, double from, double to, double velocity, double startTime) {
        m_spring = spring;
        m_target = to;
        m_t0     = startTime;
        m_x0     = from - to;
        m_v0     = velocity;
        prepare();
    }

    // A new target at time t: keeps where it is and how fast it moves.
    void retarget(double to, double t) {
        const double x = value(t), v = velocity(t);
        start(m_spring, x, to, v, t);
    }
    // A new spring at time t (and target): the same continuity.
    void respring(const Spring& spring, double to, double t) {
        const double x = value(t), v = velocity(t);
        start(spring, x, to, v, t);
    }

    [[nodiscard]] double value(double t) const { return m_target + displacement(t - m_t0); }
    [[nodiscard]] double velocity(double t) const { return displacementVelocity(t - m_t0); }
    [[nodiscard]] double target() const { return m_target; }
    [[nodiscard]] double startTime() const { return m_t0; }
    [[nodiscard]] const Spring& spring() const { return m_spring; }

    // The time (absolute) from which the value stays within `epsilon` of the
    // target and moves slower than `epsilon` per second, from the solution's
    // envelope: a guarantee, not an estimate.
    [[nodiscard]] double settleTime(double epsilon = 1e-3) const {
        // (a hair past the bound's crossing: the bound may touch epsilon exactly,
        // and rounding the absolute time back to a duration must not land before)
        const double after = settleAfter(epsilon);
        return after <= 0.0 ? m_t0 : m_t0 + after * (1.0 + 1e-9) + 1e-9; // (at rest already: now)
    }
    [[nodiscard]] bool settled(double t, double epsilon = 1e-3) const { return t >= settleTime(epsilon); }

    // The furthest the value goes past the target on either side over the whole
    // run from t onward: [lowest, highest] (exact for t at or after the start).
    // For drawing bounds that are never too small and never guessed.
    [[nodiscard]] std::pair<double, double> range(double t) const { return range(t, std::numeric_limits<double>::infinity()); }
    // ... and only from t to `until` (exact)
    [[nodiscard]] std::pair<double, double> range(double t, double until) const {
        until = std::max(until, t);
        // (run out, it ends at the target)
        const double a = value(t), b = std::isfinite(until) ? value(until) : m_target;
        double lo = std::min(a, b), hi = std::max(a, b);
        // extremes are where the velocity is zero: the first few after t carry
        // the largest amplitudes (each later one is smaller)
        const double dt = std::max(t - m_t0, 0.0);
        for (double e : extremaAfter(dt)) {
            if (m_t0 + e > until)
                continue;
            const double v = m_target + displacement(e);
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        return {lo, hi};
    }

  private:
    enum class Regime { Under, Critical, Over };
    Spring m_spring;
    double m_target = 0.0, m_t0 = 0.0, m_x0 = 0.0, m_v0 = 0.0;
    // solution constants
    Regime m_regime = Regime::Critical;
    double m_w0 = 1.0, m_zeta = 1.0, m_wd = 0.0, m_B = 0.0, m_r1 = 0.0, m_r2 = 0.0, m_c1 = 0.0, m_c2 = 0.0;

    void prepare() {
        m_w0   = m_spring.omega0();
        m_zeta = m_spring.dampingRatio();
        // The underdamped form stays exact as zeta -> 1 (B sin(wd t) is
        // (v0 + zeta w0 x0) sin(wd t) / wd); the overdamped one cancels, c1 ~ -c2
        // ~ 1 / sqrt(zeta - 1), so just above 1 the critical form stands in for
        // it: 1e-11 balances the two errors (each ~1e-11 of the travel).
        if (m_zeta >= 1.0 && m_zeta < 1.0 + 1e-11) {
            m_regime = Regime::Critical;
            m_B      = m_v0 + m_w0 * m_x0;                        // x = (x0 + B t) e^{-w0 t}
        } else if (m_zeta < 1.0) {
            m_regime = Regime::Under;
            m_wd     = m_w0 * std::sqrt((1.0 - m_zeta) * (1.0 + m_zeta)); // (no cancellation near 1)
            m_B      = (m_v0 + m_zeta * m_w0 * m_x0) / m_wd;       // x = e^{-z w0 t}(x0 cos wd t + B sin wd t)
        } else {
            m_regime = Regime::Over;
            const double s = std::sqrt((m_zeta - 1.0) * (m_zeta + 1.0));
            m_r2 = -m_w0 * (m_zeta + s);
            m_r1 = -m_w0 / (m_zeta + s);                          // the slow root, -w0 (zeta - s) without the cancellation
            m_c1 = (m_v0 - m_r2 * m_x0) / (m_r1 - m_r2);
            m_c2 = m_x0 - m_c1;                                   // x = c1 e^{r1 t} + c2 e^{r2 t}
        }
    }

    [[nodiscard]] double displacement(double t) const {
        if (t <= 0.0)
            return m_x0; // (before the start: where it started)
        switch (m_regime) {
            case Regime::Critical: return (m_x0 + m_B * t) * std::exp(-m_w0 * t);
            case Regime::Under: {
                const double e = std::exp(-m_zeta * m_w0 * t);
                return e * (m_x0 * std::cos(m_wd * t) + m_B * std::sin(m_wd * t));
            }
            case Regime::Over: return m_c1 * std::exp(m_r1 * t) + m_c2 * std::exp(m_r2 * t);
        }
        return 0.0;
    }
    [[nodiscard]] double displacementVelocity(double t) const {
        if (t <= 0.0)
            return m_v0;
        switch (m_regime) {
            case Regime::Critical: {
                const double e = std::exp(-m_w0 * t);
                return (m_B - m_w0 * (m_x0 + m_B * t)) * e;
            }
            case Regime::Under: {
                const double e = std::exp(-m_zeta * m_w0 * t), c = std::cos(m_wd * t), s = std::sin(m_wd * t);
                const double a = m_zeta * m_w0;
                return e * ((m_B * m_wd - a * m_x0) * c - (m_x0 * m_wd + a * m_B) * s);
            }
            case Regime::Over: return m_c1 * m_r1 * std::exp(m_r1 * t) + m_c2 * m_r2 * std::exp(m_r2 * t);
        }
        return 0.0;
    }

    // seconds after the start until settled: the larger of the times the
    // displacement's and the velocity's envelopes fall to epsilon for good.
    // Each has two envelopes, both proven bounds for all t, so the earlier of
    // their times is one too: a pure exponential (tight late, loose near
    // critical damping, where its amplitude blows up) and (p + q t) e^{-a t}
    // (from |sin u| <= u and |e^{-u} - 1| <= u: tight near critical damping).
    [[nodiscard]] double settleAfter(double eps) const {
        eps = std::max(eps, 1e-12);
        const double ax0 = std::abs(m_x0), av0 = std::abs(m_v0);
        switch (m_regime) {
            case Regime::Under: {
                // x = e^{-a t}(x0 cos + B sin), B wd = v0 + a x0;
                // v = e^{-a t}(v0 cos - Q sin),  Q wd = x0 w0^2 + a v0
                const double a = m_zeta * m_w0, amp = std::hypot(m_x0, m_B);
                const double tx = std::min(expBelow(amp, a, eps), linExpBelow(ax0, std::abs(m_v0 + a * m_x0), a, eps));
                const double tv = std::min(expBelow(amp * m_w0, a, eps),
                                           linExpBelow(av0, std::abs(m_x0 * m_w0 * m_w0 + a * m_v0), a, eps));
                return std::max(tx, tv);
            }
            case Regime::Critical:
                // x = (x0 + B t) e^{-w0 t}; v = (v0 - w0 B t) e^{-w0 t}
                return std::max(linExpBelow(ax0, std::abs(m_B), m_w0, eps), linExpBelow(av0, m_w0 * std::abs(m_B), m_w0, eps));
            case Regime::Over: {
                // x = e^{r1 t}(x0 + c2 (e^{(r2 - r1) t} - 1)), c2 (r1 - r2) = x0 r1 - v0
                const double a = -m_r1, k = std::abs(m_x0 * m_r1 - m_v0);
                auto xb = [&](double t) { return std::abs(m_c1) * std::exp(m_r1 * t) + std::abs(m_c2) * std::exp(m_r2 * t); };
                auto vb = [&](double t) { return std::abs(m_c1 * m_r1) * std::exp(m_r1 * t) + std::abs(m_c2 * m_r2) * std::exp(m_r2 * t); };
                const double tx = std::min(fallsBelow(xb, 0.0, 1.0 / a, eps), linExpBelow(ax0, k, a, eps));
                const double tv = std::min(fallsBelow(vb, 0.0, 1.0 / a, eps), linExpBelow(av0, std::abs(m_r2) * k, a, eps));
                return std::max(tx, tv);
            }
        }
        return 0.0;
    }
    static constexpr double NEVER = std::numeric_limits<double>::infinity();
    // the first time from which c e^{-a t} stays at or below eps
    static double expBelow(double c, double a, double eps) {
        if (c <= eps)
            return 0.0;
        return a > 0.0 ? std::log(c / eps) / a : NEVER;
    }
    // the first time from which (p + q t) e^{-a t} (p, q >= 0) stays at or
    // below eps: it rises to one peak, at max(0, 1/a - p/q), then falls
    static double linExpBelow(double p, double q, double a, double eps) {
        if (q == 0.0)
            return expBelow(p, a, eps);
        if (!(a > 0.0))
            return NEVER;
        auto f = [&](double t) { return (p + q * t) * std::exp(-a * t); };
        const double peak = std::max(0.0, 1.0 / a - p / q);
        if (f(peak) <= eps)
            return 0.0; // (rising to the peak: below it before)
        // past the peak g = ln(p + q t) - a t - ln eps falls and is concave, so
        // Newton's method from a point past the crossing stays past it (every
        // step is a guarantee) and closes in quadratically
        double t = peak + 1.0 / a;
        while (f(t) > eps) {
            if (t > 1e300)
                return NEVER;
            t = peak + 2.0 * (t - peak);
        }
        const double lnEps = std::log(eps);
        for (int i = 0; i < 100; i++) {
            const double g = std::log(p + q * t) - a * t - lnEps, slope = q / (p + q * t) - a;
            const double next = t - g / slope;
            if (!(next < t) || f(next) > eps)
                break;
            const double moved = t - next;
            t = next;
            if (moved <= 1e-13 * t)
                break;
        }
        return t;
    }
    // the first time from `from` on at which f, falling from there on, is at
    // or below eps (`scale`: its time scale, where to start looking)
    template <class F>
    static double fallsBelow(F f, double from, double scale, double eps) {
        if (f(from) <= eps)
            return from;
        double lo = from, step = std::isfinite(scale) && scale > 0.0 ? scale : 1.0, hi = from + step;
        while (f(hi) > eps) {
            if (hi > 1e300)
                return NEVER;
            lo = hi;
            step *= 2.0;
            hi = from + step;
        }
        // (hi always has f <= eps; to 1e-12 relative, well inside settleTime's slack)
        while (hi - lo > 1e-12 * hi) {
            const double mid = lo + 0.5 * (hi - lo);
            (f(mid) > eps ? lo : hi) = mid;
        }
        return hi;
    }

    // times (after the start) where the velocity is zero, from dt on: at most a
    // few, in order (each underdamped extreme is smaller than the one before)
    [[nodiscard]] std::array<double, 3> extremaAfter(double dt) const;
};

inline std::array<double, 3> SpringMotion::extremaAfter(double dt) const {
    std::array<double, 3> out{dt, dt, dt};
    switch (m_regime) {
        case Regime::Under: {
            // v(t) = e^{-a t} (P cos wd t - Q sin wd t) = 0  ->  tan(wd t) = P / Q
            const double a = m_zeta * m_w0;
            const double P = m_B * m_wd - a * m_x0, Q = m_x0 * m_wd + a * m_B;
            const double root = std::atan2(P, Q) / m_wd, period = PI / m_wd; // one root; others every pi / wd
            // the first at or after dt (in one step: dt may be many periods in)
            double t = root + std::ceil((dt - root) / period) * period;
            if (t < dt) t += period; // (rounding)
            for (int i = 0; i < 3; i++) out[i] = t + i * period;
            break;
        }
        case Regime::Critical: {
            // v = (B - w0 x0 - w0 B t) e^{-w0 t} = 0  ->  t = (B - w0 x0) / (w0 B)
            if (std::abs(m_B) > 0.0) {
                const double t = (m_B - m_w0 * m_x0) / (m_w0 * m_B);
                if (t > dt) out = {t, t, t};
            }
            break;
        }
        case Regime::Over: {
            // c1 r1 e^{r1 t} + c2 r2 e^{r2 t} = 0  ->  t = ln(-c2 r2 / (c1 r1)) / (r1 - r2)
            const double num = -m_c2 * m_r2, den = m_c1 * m_r1;
            if (den != 0.0 && num / den > 0.0) {
                const double t = std::log(num / den) / (m_r1 - m_r2);
                if (t > dt) out = {t, t, t};
            }
            break;
        }
    }
    return out;
}
} // namespace motion
