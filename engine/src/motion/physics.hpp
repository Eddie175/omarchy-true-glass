// motion/physics.hpp — coasting and stretching past an edge.
//
//  Decay: a fling that slows by a constant factor per millisecond, as UIKit's
//  scroll views do (UIScrollView.DecelerationRate: normal 0.998, fast 0.99 per
//  ms; documented values). Exact: x(t) = x0 + v0 * (r^{1000 t} - 1) / (1000 ln r)
//  with v0 in units per second.
//  RubberBand: the stretch past a limit, the widely used public formula
//  f(x) = (1 - 1 / (x c / d + 1)) d, c = 0.55,
//  d the dimension: it gives way less and less, never past d.
#pragma once

#include <algorithm>
#include <cmath>
#include <limits>

namespace motion {

struct Decay {
    double from = 0.0, velocity = 0.0, rate = 0.998, startTime = 0.0;

    [[nodiscard]] double k() const { return 1000.0 * std::log(rate); } // per second (negative)
    [[nodiscard]] double value(double t) const {
        const double dt = std::max(t - startTime, 0.0), kk = k();
        // (e^{k dt} - 1) / k, without the cancellation for a rate near 1, and dt at 1
        return from + velocity * (kk == 0.0 ? dt : std::expm1(kk * dt) / kk);
    }
    [[nodiscard]] double velocityAt(double t) const { return velocity * std::exp(k() * std::max(t - startTime, 0.0)); }
    // where it comes to rest (a rate of 1 or more never slows: it goes on forever)
    [[nodiscard]] double restingValue() const {
        if (k() < 0.0)
            return from - velocity / k();
        return velocity == 0.0 ? from : std::copysign(std::numeric_limits<double>::infinity(), velocity);
    }
    // when the speed falls under `epsilon` units per second (never, if it doesn't slow)
    [[nodiscard]] double settleTime(double epsilon = 1.0) const {
        if (std::abs(velocity) <= epsilon)
            return startTime;
        if (!(k() < 0.0))
            return std::numeric_limits<double>::infinity();
        return startTime + std::log(epsilon / std::abs(velocity)) / k();
    }
};

[[nodiscard]] inline double rubberBand(double overshoot, double dimension, double constant = 0.55) {
    if (dimension <= 0.0)
        return 0.0;
    const double s = std::abs(overshoot);
    const double r = (1.0 - 1.0 / (s * constant / dimension + 1.0)) * dimension;
    return overshoot < 0.0 ? -r : r;
}

} // namespace motion
