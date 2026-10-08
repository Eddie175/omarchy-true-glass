#version 440
// Glass lock screen: the compositor's glass optics, in a Qt shader.
// The clock digits and the password capsule are clear glass over the wallpaper.
// Their shapes are exact signed distances (the digits from an atlas made by
// make-digits.sh, the capsule analytic), so every rim has the same profile as
// the cards on the desktop: a convex squircle across the rim, flat on top. Each
// pixel refracts the eye ray through it (Snell's law, per colour from the Abbe
// number), the tilted rim mirrors the scene around it (Schlick Fresnel), one
// thin edge catches the light, and the light's direction follows the pointer a
// little (the desktop's tilt).
layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec2 size;        // item size, px
    vec4 field;       // password capsule x y w h, px
    vec4 clock;       // the digit cells' top-left (xy, px), scale from atlas px (z), atlas height (w)
    vec4 g0;          // per glyph: cell left (atlas px from clock.xy), atlas x, atlas width, on
    vec4 g1;
    vec4 g2;
    vec4 g3;
    vec4 g4;
    vec2 atlasSize;
    float range;      // atlas: +-range px around the outline
    float form;       // 0..1: how far the glass has formed
    float thick;      // capsule glass height, px
    float bezel;      // capsule rim width, px
    float digitThick; // digit glass height, px
    float digitBezel; // digit rim width, px
    float ior;
    float abbe;
    vec2 light;       // unit direction the light comes from, screen axes (upper left: -0.7, -0.7)
    float unit;       // px per design px (1440 px short side)
    vec4 wallCrop;    // the part of the wallpaper on screen: uv offset (xy), uv size (zw); a centred cover crop
    float darkGlass;  // 0..1: Dark mode (the Glass menu), smoked glass as on the desktop
    float lightUi;    // 1: a light theme: the wallpaper as it is (the text is dark)
};
layout(binding = 1) uniform sampler2D wall;
layout(binding = 2) uniform sampler2D digits;

// rounded box whose inside depth follows rounded contours (no corner seams)
float sdBox(vec2 p, vec2 c, vec2 H, float r) {
    vec2 P = abs(p - c);
    float h = min(H.x, H.y); r = min(r, h);
    vec2 q = P - H + r;
    float outside = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
    if (outside > 0.0) return outside;
    vec2 a = P - H + r;
    float d = min(r - a.x, r - a.y);
    float B = 4.0 * (a.x + a.y) - 2.0 * r, C = dot(a, a) - r * r, disc = B * B - 28.0 * C;
    if (disc >= 0.0) { float dc = (-B + sqrt(disc)) / 14.0; if (a.x + 2.0 * dc >= 0.0 && a.y + 2.0 * dc >= 0.0) d = dc; }
    vec2 s = P - H + h;
    return -max(d, h - length(max(s, 0.0)) - min(max(s.x, s.y), 0.0));
}
// + inside, px
float fieldIn(vec2 p) { return -sdBox(p, field.xy + field.zw * 0.5, field.zw * 0.5, field.w * 0.5); }

float glyphIn(vec2 q, vec4 g) {
    float lx = q.x - g.x;
    if (g.w < 0.5 || lx < 0.0 || lx > g.z) return -range;
    // (16 bits over two channels, high in r, low in g: 8 bits stepped the outline by
    // half a screen px, a staircase along each lit edge)
    vec2 v = texture(digits, vec2((g.y + lx) / atlasSize.x, q.y / atlasSize.y)).rg;
    return (dot(v, vec2(65280.0, 255.0) / 65535.0) - 0.5) * 2.0 * range;
}
// + inside the digits, px
float clockIn(vec2 p) {
    vec2 q = (p - clock.xy) / clock.z;
    if (q.y < 0.0 || q.y > clock.w) return -range * clock.z;
    float d = max(max(glyphIn(q, g0), glyphIn(q, g1)), max(max(glyphIn(q, g2), glyphIn(q, g3)), glyphIn(q, g4)));
    return d * clock.z;
}

// convex squircle across the rim (t: 0 at the edge, 1 where the top is flat)
float rimHeight(float t) { float s = 1.0 - clamp(t, 0.0, 1.0); return pow(max(1.0 - s * s * s, 0.0), 1.0 / 3.0); } // (the compositor's rim: tapering out over the whole band)

// dark glass, as the desktop's: what is seen through it taken down, the brighter the more
float darkGain(float l) { return 1.0 - darkGlass * 0.58 * (0.35 + 0.65 * smoothstep(0.1, 0.85, l)); }

// glass height here: the capsule's or the digits', whichever is there
float heightAt(vec2 p) {
    float h = 0.0;
    float df = fieldIn(p);
    if (df > 0.0) h = thick * rimHeight(df / bezel);
    float dc = clockIn(p);
    if (dc > 0.0) h = max(h, digitThick * rimHeight(dc / digitBezel));
    return h * form;
}
// the wallpaper, a little darker toward the top so the time always reads (as a
// phone's lock screen does), also as the glass sees it
vec3 wallAt(vec2 p, float lod) {
    float shade = lightUi > 0.5 ? 1.0 : 1.0 - 0.2 * (1.0 - smoothstep(0.0, 0.5 * size.y, p.y));
    vec3  c     = textureLod(wall, wallCrop.xy + clamp(p / size, 0.001, 0.999) * wallCrop.zw, lod).rgb;
    // (a very bright picture taken down at the top of its range, as the desktop's
    // glass does: white text over white read as nothing; dark pictures as they are)
    // (a light theme's dark text reads on it as it is)
    if (lightUi < 0.5) c *= 1.0 - 0.38 * smoothstep(0.55, 0.95, dot(c, vec3(0.2126, 0.7152, 0.0722)));
    return c * shade;
}

// Everything the glass shows at one point (screen px): its colour premultiplied
// by how much of the point it covers, and that cover.
vec4 shade(vec2 p) {
    float df = fieldIn(p), dc = clockIn(p);
    // (a px-wide ramp of the exact distances; four of these per pixel at an edge)
    float cover = clamp(max(df, dc) + 0.5, 0.0, 1.0) * smoothstep(0.0, 0.25, form);
    if (cover <= 0.0)
        return vec4(0.0);
    float e = 0.5, hc = heightAt(p);
    vec2 g = vec2(heightAt(p + vec2(e, 0.0)) - heightAt(p - vec2(e, 0.0)),
                  heightAt(p + vec2(0.0, e)) - heightAt(p - vec2(0.0, e))) / (2.0 * e);
    vec3 N = normalize(vec3(-clamp(g, vec2(-40.0), vec2(40.0)), 1.0));
    // Cauchy dispersion fitted to n_d and the Abbe number (as the compositor's)
    float B = (ior - 1.0) / (abbe * 1.9106), A = ior - B / 0.34527;
    vec3 nRGB = A + B / vec3(0.37332, 0.30140, 0.21530);
    // clear glass: the wallpaper through it, bent by the rim (no frost, no white
    // lift: both read as milky digits; glass adds no white of its own)
    vec3 col;
    for (int c = 0; c < 3; c++) {
        vec3 T = refract(vec3(0.0, 0.0, -1.0), N, 1.0 / nRGB[c]);
        vec2 off = T.z < -0.001 ? T.xy * (hc / -T.z) : vec2(0.0);
        col[c] = wallAt(p + off, 0.0)[c];
    }
    float lum = dot(col, vec3(0.2126, 0.7152, 0.0722));
    // the capsule lightly smoked behind its placeholder (darker, never lighter)
    // (a light theme's: lightly milky, for its dark placeholder)
    vec3 veiled = lightUi > 0.5 ? mix(col, vec3(1.0), 0.18 * (1.0 - smoothstep(0.55, 0.9, lum)))
                                : col * (1.0 - 0.12 * smoothstep(0.25, 0.55, lum));
    col = mix(veiled, col, clamp(dc + 0.5, 0.0, 1.0));
    // Fresnel: the tilted rim mirrors the scene around it as bright as it is
    float f0 = pow((ior - 1.0) / (ior + 1.0), 2.0);
    float F = f0 + (1.0 - f0) * pow(1.0 - clamp(N.z, 0.0, 1.0), 5.0);
    // (a light mirror: strong, it read as a thick bright border round every stroke)
    col = mix(col, wallAt(p + N.xy * 40.0 * unit, 1.5), clamp(F * 0.4, 0.0, 0.5));
    col *= darkGain(dot(col, vec3(0.2126, 0.7152, 0.0722)));
    // the light along the rim facing it, only where the rim turns over, fainter
    // opposite (light inside the glass)
    float nl = length(N.xy);
    if (nl > 1e-4) {
        float fl = dot(N.xy / nl, light);
        float f6 = fl * fl * fl * fl * fl * fl;
        col += vec3(1.0, 0.99, 0.96) * (fl > 0.0 ? f6 : 0.3 * f6) * smoothstep(0.86, 0.985, nl) * form * 0.32;
    }
    // one thin edge where the glass turns over: lit facing the light, dark opposite
    // (exact distances: as thin round every curve as along the straights; a soft
    // inner side, so four samples blend it instead of stepping it)
    float edgeD = min(df > 0.0 ? df : 1e4, dc > 0.0 ? dc : 1e4);
    // (a little over 1.5 px: thinner, a line drifting across the pixel grid on a
    // slanted stroke, the 5's stem, pulsed brighter and dimmer along its length)
    float ring = (1.0 - smoothstep(0.6 * unit, 2.0 * unit, edgeD)) * smoothstep(-0.6, 0.6, edgeD);
    vec2 gn = length(g) > 1e-4 ? -normalize(g) : vec2(0.0);
    float facing = clamp(dot(gn, light) * 0.5 + 0.5, 0.0, 1.0);
    // (the capsule: lit only along the arc facing the light, as glass catches it;
    // a full bright outline read as a web form field)
    bool  onField = df > 0.0 && (dc <= 0.0 || df < dc);
    float lit     = onField ? 0.08 + 0.92 * pow(facing, 3.0) : 0.25 + 0.75 * facing * facing;
    col = mix(col, vec3(1.0), ring * lit * (onField ? 0.7 : 0.75) * form);
    col = mix(col, vec3(0.0), ring * (1.0 - facing) * 0.18 * form);
    return vec4(clamp(col, 0.0, 1.0) * cover, cover);
}

void main() {
    vec2 p = qt_TexCoord0 * size;
    vec3 bg = wallAt(p, 0.0);
    float df = fieldIn(p), dc = clockIn(p);
    // soft shadows straight down (the light is above): the capsule's and the digits'
    float sh = 0.15 * pow(1.0 - smoothstep(0.0, 34.0 * unit, -fieldIn(p - vec2(0.0, 9.0 * unit))), 2.0);
    sh = max(sh, 0.11 * pow(1.0 - smoothstep(0.0, 22.0 * unit, -clockIn(p - vec2(0.0, 10.0 * unit))), 2.0));
    // (a shade of the picture itself, deeper and a little richer, as the desktop's
    // shadows: black laid over it read as a grey smudge)
    float bl = dot(bg, vec3(0.2126, 0.7152, 0.0722));
    vec3 under = mix(bg, clamp(mix(vec3(bl), bg, 1.5), 0.0, 1.0) * 0.32, sh * form);
    if (max(df, dc) < -1.5) {
        fragColor = vec4(under, 1.0) * qt_Opacity;
        return;
    }
    // At an edge and across the rim, where the bend changes fastest, one sample
    // per pixel stepped the edges and aliased the bent picture: four, on a rotated
    // grid (as film renderers sample), averaged. The flat top keeps one.
    // Right at the edge, where the lit hairline is under 2 px wide, eight (an
    // 8-rooks pattern: every row and column of the pixel sampled once).
    bool edge = abs(df) < 2.5 || abs(dc) < 2.5;
    bool rim  = (df > -1.5 && df < bezel + 1.0) || (dc > -1.5 && dc < digitBezel + 1.0);
    vec4 c;
    if (edge)
        c = 0.125 * (shade(p + vec2(-0.4375, -0.0625)) + shade(p + vec2(-0.3125, 0.3125)) +
                     shade(p + vec2(-0.1875, -0.3125)) + shade(p + vec2(-0.0625, 0.0625)) +
                     shade(p + vec2(0.0625, 0.4375)) + shade(p + vec2(0.1875, -0.1875)) +
                     shade(p + vec2(0.3125, 0.1875)) + shade(p + vec2(0.4375, -0.4375)));
    else if (rim)
        c = 0.25 * (shade(p + vec2(-0.375, -0.125)) + shade(p + vec2(0.125, -0.375)) +
                    shade(p + vec2(0.375, 0.125)) + shade(p + vec2(-0.125, 0.375)));
    else
        c = shade(p);
    fragColor = vec4(under * (1.0 - c.a) + c.rgb, 1.0) * qt_Opacity;
}
