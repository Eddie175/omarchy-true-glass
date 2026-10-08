#pragma once

#include <GLES3/gl32.h>
#include <hyprland/src/render/Shader.hpp>
#include <string>

struct SGlassUniforms {
    GLint refractionStrength = -1;
    GLint chromaticAberration = -1;
    GLint fresnelStrength = -1;
    GLint specularStrength = -1;
    GLint glassOpacity = -1;
    GLint edgeThickness = -1;
    GLint invBezelWidthPx = -1;
    GLint tintColor = -1;
    GLint tintAlpha = -1;
    GLint lensDistortion = -1;
    GLint lensMaxPx = -1;
    GLint saturation = -1;
    GLint vibrancyDarkness = -1;
    GLint adaptiveDim = -1;
    GLint adaptiveBoost = -1;
    GLint refractionFlow = -1;
    GLint refractionSpread = -1;
    GLint fresnelTint = -1;
    GLint bevelStrength = -1;
    GLint bevelSize = -1;
    GLint monitorScale = -1;
    GLint fresnelColor = -1;
    GLint fresnelColorAlpha = -1;
    GLint bevelColor = -1;
    GLint bevelColorAlpha = -1;
    GLint bevelTint = -1;
    GLint bevelAngle = -1;
    GLint bevelShadow = -1;
    GLint specularAngle = -1;
    GLint invFullSize = -1;
    GLint invRoundingPower = -1;
    GLint radii = -1; // per-corner radius: top-left, top-right, bottom-right, bottom-left

    // Layers only: temp FBO surface mask for content-aware glass
    GLint maskTex = -1;
    GLint useMask = -1;
    GLint maskUVOffset = -1;
    GLint maskUVScale = -1;
    GLint maskAlphaThreshold = -1;
    GLint maskMode = -1;
    GLint regionRectCount = -1;
    GLint regionRects = -1;

    // Subsurface items and shape-box layers: glass-box sub-rect the SDF is
    // measured against, in box-local pixels (see Shaders.hpp).
    GLint glassBoxOffsetPx = -1;
    GLint glassBoxSizePx = -1;

    // Physical optics, contour and shape-box layers, per-draw constants (see Shaders.hpp)
    GLint thicknessPx = -1;
    GLint abbe = -1;
    GLint bezelPx = -1;
    GLint materialize = -1;
    GLint sdfSource = -1;
    GLint sdfTex = -1;
    GLint styleTex = -1;
    GLint presenceTex = -1;
    GLint livePresence = -1;
    GLint presenceRef = -1;
    GLint ghost = -1;
    GLint frostTex = -1;
    GLint frost = -1;
    GLint gleam = -1;
    GLint gleamPulse = -1;
    GLint gleamShape = -1;
    GLint heldTex = -1;
    GLint adaptiveStyle = -1;
    GLint shadowOpacity = -1;
    GLint shadowRangePx = -1;
    GLint shadowOffsetPx = -1;
    GLint screenToUv = -1;
    GLint fillKey = -1;
    GLint fillKeyTop = -1;
    GLint textBacking = -1;
    GLint openAnim = -1;
    GLint morphRect = -1;
    GLint morphCard = -1;
    GLint foldRect = -1;
    GLint outFade = -1;
    GLint depth = -1;
    GLint darkGlass = -1;
    GLint cornerMiter = -1;
    GLint lightUi = -1;
    GLint lightGlass = -1;
    GLint paneRect = -1;
    GLint foldInfo = -1;
    GLint morphSource = -1;
    GLint morphShape = -1;
    GLint contentXform = -1;
    GLint contentFx = -1;
    GLint morphWaist = -1;
    GLint morphWaist2 = -1;
    GLint morphWaistBar = -1;
    GLint morphBarLens = -1;
    GLint barGrade1 = -1;
    GLint morphHand = -1;
    GLint barTint = -1;
    GLint barStyle = -1;
    GLint contentBox = -1;
    GLint sdfRect = -1;
    GLint sampleXform = -1;
    GLint nRGB = -1;
    GLint fresnelF0 = -1;
    GLint lightUv = -1;
    GLint bevelLightUv = -1;
    GLint pressGlow = -1;
    GLint pressReachPx = -1;
    GLint pressBulge = -1;
};

// Jump-flood distance field passes (ShapeField.cpp)
struct SShapeFieldProgram {
    SP<CShader> shader = makeShared<CShader>();
    GLint       a = -1, b = -1, c = -1, d = -1;   // pass-specific uniforms
};

struct SBlurUniforms {
    GLint direction = -1;
    GLint radius    = -1;
};

class CShaderManager {
  public:
    [[nodiscard]] bool isInitialized() const noexcept { return m_initialized; }
    // The last compile attempt failed; it is retried at the next glass draw.
    [[nodiscard]] bool compileFailed() const noexcept { return m_compileFailed; }
    // the GPU's name, read with the first compile (the GL context is current then)
    [[nodiscard]] const std::string& renderer() const noexcept { return m_renderer; }

    void initializeIfNeeded();
    void destroy() noexcept;

    SP<CShader>    glassShader = makeShared<CShader>();
    uint64_t       compileSerial = 0;   // bumped by every glass program build
    SGlassUniforms glassUniforms;

    SP<CShader>    blurShader = makeShared<CShader>();
    SBlurUniforms  blurUniforms;

    SShapeFieldProgram fieldSeed, fieldStep, fieldResolve, fieldBlur, fieldBox, barStyle, presence, hold, copy;

  private:
    bool m_initialized = false;
    bool m_compileFailed = false;
    std::string m_renderer;

    [[nodiscard]] static std::string loadShaderSource(const char* fileName);
    [[nodiscard]] bool compileGlassShader();
    [[nodiscard]] bool compileBlurShader();
    [[nodiscard]] bool compileShapeFieldShaders();
};
