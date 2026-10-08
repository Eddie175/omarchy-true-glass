/*
 * The curve solver below is a port of WebKit's UnitBezier.h, under this license:
 *
 * Copyright (C) 2008 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

// motion/bezier.hpp — cubic-bezier timing curves (CSS / Core Animation).
//
// A port of WebKit's UnitBezier (Apple Inc., BSD-2): the curve from (0,0) to
// (1,1) through control points (x1,y1), (x2,y2); for a time fraction x, find
// the curve's parameter t with x(t) = x by Newton's method, then bisection,
// and return y(t). Plus Core Animation's named curves (documented values).
#pragma once

#include <algorithm>
#include <cmath>

namespace motion {

class Bezier {
  public:
    constexpr Bezier(double x1, double y1, double x2, double y2) {
        // (CSS requires x1, x2 in [0, 1]: then x is monotone in t, so y is a
        // function of time and the solver's bracket holds)
        x1 = std::clamp(x1, 0.0, 1.0);
        x2 = std::clamp(x2, 0.0, 1.0);
        // polynomial coefficients (Horner form), as UnitBezier computes them
        cx = 3.0 * x1;
        bx = 3.0 * (x2 - x1) - cx;
        ax = 1.0 - cx - bx;
        cy = 3.0 * y1;
        by = 3.0 * (y2 - y1) - cy;
        ay = 1.0 - cy - by;
        // the slope at the ends, for inputs outside [0, 1]: toward the nearer
        // control point, or the farther one when the nearer sits on the end; a
        // vertical tangent counts as flat, and both on the end is a straight line
        startGradient = x1 > 0.0 ? y1 / x1 : (y1 == 0.0 && x2 > 0.0 ? y2 / x2 : (y1 == 0.0 && y2 == 0.0 ? 1.0 : 0.0));
        endGradient   = x2 < 1.0 ? (y2 - 1.0) / (x2 - 1.0) : (y2 == 1.0 && x1 < 1.0 ? (y1 - 1.0) / (x1 - 1.0) : (y1 == 1.0 && y2 == 1.0 ? 1.0 : 0.0));
    }

    // Core Animation's kCAMediaTimingFunction* curves
    static constexpr Bezier linear() { return {0.0, 0.0, 1.0, 1.0}; }
    static constexpr Bezier easeIn() { return {0.42, 0.0, 1.0, 1.0}; }
    static constexpr Bezier easeOut() { return {0.0, 0.0, 0.58, 1.0}; }
    static constexpr Bezier easeInOut() { return {0.42, 0.0, 0.58, 1.0}; }
    static constexpr Bezier defaultCurve() { return {0.25, 0.1, 0.25, 1.0}; }

    // y for the time fraction x (outside [0, 1]: continued along the end slopes)
    [[nodiscard]] double operator()(double x, double epsilon = 1e-7) const {
        if (x < 0.0)
            return startGradient * x;
        if (x > 1.0)
            return 1.0 + endGradient * (x - 1.0);
        return sampleY(solveX(x, epsilon));
    }

  private:
    double ax = 0, bx = 0, cx = 0, ay = 0, by = 0, cy = 0, startGradient = 0, endGradient = 0;

    [[nodiscard]] double sampleX(double t) const { return ((ax * t + bx) * t + cx) * t; }
    [[nodiscard]] double sampleY(double t) const { return ((ay * t + by) * t + cy) * t; }
    [[nodiscard]] double sampleDX(double t) const { return (3.0 * ax * t + 2.0 * bx) * t + cx; }

    [[nodiscard]] double solveX(double x, double epsilon) const {
        // Newton's method first: fast when the slope is healthy
        double t = x;
        for (int i = 0; i < 8; i++) {
            const double err = sampleX(t) - x;
            if (std::abs(err) < epsilon)
                return t;
            const double d = sampleDX(t);
            if (std::abs(d) < 1e-6)
                break;
            t -= err / d;
        }
        // then bisection: always converges
        double lo = 0.0, hi = 1.0;
        t = x;
        for (;;) {
            const double v = sampleX(t);
            if (std::abs(v - x) < epsilon)
                return t;
            (x > v ? lo : hi) = t;
            const double mid = 0.5 * (lo + hi);
            if (!(mid > lo && mid < hi))
                return t; // (no double left between: as close as t can get)
            t = mid;
        }
    }
};

} // namespace motion
