#include "ShaderManager.hpp"
#include "Globals.hpp"
#include "Shaders.hpp"

#include <GLES3/gl32.h>
#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/render/OpenGL.hpp>

std::string CShaderManager::loadShaderSource(const char* fileName) {
    if (SHADERS.contains(fileName))
        return SHADERS.at(fileName);

    const std::string message = std::format("[{}] Failed to load shader: {}", PLUGIN_NAME, fileName);
    HyprlandAPI::addNotification(PHANDLE, message, CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
    throw std::runtime_error(message);
}

bool CShaderManager::compileGlassShader() {
    compileSerial++; // (a new program, maybe under a reused name: its uniforms start unset)
    if (!glassShader->createProgram(
            g_pHyprOpenGL->m_shaders->TEXVERTSRC,
            loadShaderSource("glass.frag"),
            true
        )) {
        HyprlandAPI::addNotification(PHANDLE,
            std::format("[{}] Failed to compile glass shader", PLUGIN_NAME),
            CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        return false;
    }

    const auto program = glassShader->program();

    glassUniforms.refractionStrength  = glGetUniformLocation(program, "refractionStrength");
    glassUniforms.chromaticAberration = glGetUniformLocation(program, "chromaticAberration");
    glassUniforms.fresnelStrength     = glGetUniformLocation(program, "fresnelStrength");
    glassUniforms.specularStrength    = glGetUniformLocation(program, "specularStrength");
    glassUniforms.glassOpacity        = glGetUniformLocation(program, "glassOpacity");
    glassUniforms.edgeThickness       = glGetUniformLocation(program, "edgeThickness");
    glassUniforms.invBezelWidthPx     = glGetUniformLocation(program, "invBezelWidthPx");
    glassUniforms.tintColor           = glGetUniformLocation(program, "tintColor");
    glassUniforms.tintAlpha           = glGetUniformLocation(program, "tintAlpha");
    glassUniforms.lensDistortion      = glGetUniformLocation(program, "lensDistortion");
    glassUniforms.lensMaxPx           = glGetUniformLocation(program, "lensMaxPx");
    glassUniforms.saturation          = glGetUniformLocation(program, "saturation");
    glassUniforms.vibrancyDarkness    = glGetUniformLocation(program, "vibrancyDarkness");
    glassUniforms.adaptiveDim         = glGetUniformLocation(program, "adaptiveDim");
    glassUniforms.adaptiveBoost       = glGetUniformLocation(program, "adaptiveBoost");
    glassUniforms.refractionFlow      = glGetUniformLocation(program, "refractionFlow");
    glassUniforms.refractionSpread    = glGetUniformLocation(program, "refractionSpread");
    glassUniforms.fresnelTint         = glGetUniformLocation(program, "fresnelTint");
    glassUniforms.bevelStrength       = glGetUniformLocation(program, "bevelStrength");
    glassUniforms.bevelSize           = glGetUniformLocation(program, "bevelSize");
    glassUniforms.monitorScale        = glGetUniformLocation(program, "monitorScale");
    glassUniforms.fresnelColor        = glGetUniformLocation(program, "fresnelColor");
    glassUniforms.fresnelColorAlpha   = glGetUniformLocation(program, "fresnelColorAlpha");
    glassUniforms.bevelColor          = glGetUniformLocation(program, "bevelColor");
    glassUniforms.bevelColorAlpha     = glGetUniformLocation(program, "bevelColorAlpha");
    glassUniforms.bevelTint           = glGetUniformLocation(program, "bevelTint");
    glassUniforms.bevelAngle          = glGetUniformLocation(program, "bevelAngle");
    glassUniforms.bevelShadow         = glGetUniformLocation(program, "bevelShadow");
    glassUniforms.specularAngle       = glGetUniformLocation(program, "specularAngle");
    glassUniforms.invFullSize         = glGetUniformLocation(program, "invFullSize");
    glassUniforms.invRoundingPower    = glGetUniformLocation(program, "invRoundingPower");
    glassUniforms.radii               = glGetUniformLocation(program, "radii");
    glassUniforms.maskTex             = glGetUniformLocation(program, "maskTex");
    glassUniforms.useMask             = glGetUniformLocation(program, "useMask");
    glassUniforms.maskUVOffset        = glGetUniformLocation(program, "maskUVOffset");
    glassUniforms.maskUVScale         = glGetUniformLocation(program, "maskUVScale");
    glassUniforms.maskAlphaThreshold  = glGetUniformLocation(program, "maskAlphaThreshold");
    glassUniforms.maskMode            = glGetUniformLocation(program, "maskMode");
    glassUniforms.regionRectCount     = glGetUniformLocation(program, "regionRectCount");
    glassUniforms.regionRects         = glGetUniformLocation(program, "regionRects[0]");
    if (glassUniforms.regionRects == -1)
        glassUniforms.regionRects = glGetUniformLocation(program, "regionRects");
    glassUniforms.glassBoxOffsetPx    = glGetUniformLocation(program, "glassBoxOffsetPx");
    glassUniforms.glassBoxSizePx      = glGetUniformLocation(program, "glassBoxSizePx");
    glassUniforms.thicknessPx         = glGetUniformLocation(program, "thicknessPx");
    glassUniforms.abbe                = glGetUniformLocation(program, "abbe");
    glassUniforms.bezelPx             = glGetUniformLocation(program, "bezelPx");
    glassUniforms.materialize         = glGetUniformLocation(program, "materializeU");
    glassUniforms.sdfSource           = glGetUniformLocation(program, "sdfSource");
    glassUniforms.sdfTex              = glGetUniformLocation(program, "sdfTex");
    glassUniforms.styleTex            = glGetUniformLocation(program, "styleTex");
    glassUniforms.presenceTex         = glGetUniformLocation(program, "presenceTex");
    glassUniforms.livePresence        = glGetUniformLocation(program, "livePresence");
    glassUniforms.presenceRef         = glGetUniformLocation(program, "presenceRef");
    glassUniforms.ghost               = glGetUniformLocation(program, "ghost");
    glassUniforms.frostTex            = glGetUniformLocation(program, "frostTex");
    glassUniforms.frost               = glGetUniformLocation(program, "frost");
    glassUniforms.gleam               = glGetUniformLocation(program, "gleam");
    glassUniforms.gleamPulse          = glGetUniformLocation(program, "gleamPulse");
    glassUniforms.gleamShape          = glGetUniformLocation(program, "gleamShape");
    glassUniforms.heldTex             = glGetUniformLocation(program, "heldTex");
    glassUniforms.adaptiveStyle       = glGetUniformLocation(program, "adaptiveStyle");
    glassUniforms.shadowOpacity       = glGetUniformLocation(program, "shadowOpacity");
    glassUniforms.shadowRangePx       = glGetUniformLocation(program, "shadowRangePx");
    glassUniforms.shadowOffsetPx      = glGetUniformLocation(program, "shadowOffsetPx");
    glassUniforms.screenToUv          = glGetUniformLocation(program, "screenToUv");
    glassUniforms.fillKey             = glGetUniformLocation(program, "fillKey");
    glassUniforms.fillKeyTop          = glGetUniformLocation(program, "fillKeyTop");
    glassUniforms.textBacking         = glGetUniformLocation(program, "textBacking");
    glassUniforms.openAnim            = glGetUniformLocation(program, "openAnim");
    glassUniforms.morphRect           = glGetUniformLocation(program, "morphRect");
    glassUniforms.morphCard           = glGetUniformLocation(program, "morphCard");
    glassUniforms.foldRect            = glGetUniformLocation(program, "foldRect");
    glassUniforms.outFade             = glGetUniformLocation(program, "outFade");
    glassUniforms.depth               = glGetUniformLocation(program, "depth");
    glassUniforms.darkGlass           = glGetUniformLocation(program, "darkGlass");
    glassUniforms.cornerMiter         = glGetUniformLocation(program, "cornerMiter");
    glassUniforms.lightUi             = glGetUniformLocation(program, "lightUi");
    glassUniforms.lightGlass          = glGetUniformLocation(program, "lightGlass");
    glassUniforms.paneRect            = glGetUniformLocation(program, "paneRect");
    glassUniforms.foldInfo            = glGetUniformLocation(program, "foldInfo");
    glassUniforms.morphSource         = glGetUniformLocation(program, "morphSource");
    glassUniforms.morphShape          = glGetUniformLocation(program, "morphShape");
    glassUniforms.contentXform        = glGetUniformLocation(program, "contentXform");
    glassUniforms.contentFx           = glGetUniformLocation(program, "contentFx");
    glassUniforms.morphWaist          = glGetUniformLocation(program, "morphWaist");
    glassUniforms.morphWaist2         = glGetUniformLocation(program, "morphWaist2");
    glassUniforms.morphWaistBar       = glGetUniformLocation(program, "morphWaistBar");
    glassUniforms.morphBarLens        = glGetUniformLocation(program, "morphBarLens");
    glassUniforms.barGrade1           = glGetUniformLocation(program, "barGrade1");
    glassUniforms.morphHand           = glGetUniformLocation(program, "morphHand");
    glassUniforms.barTint             = glGetUniformLocation(program, "barTint");
    glassUniforms.barStyle            = glGetUniformLocation(program, "barStyle");
    glassUniforms.contentBox          = glGetUniformLocation(program, "contentBox");
    glassUniforms.sdfRect             = glGetUniformLocation(program, "sdfRect");
    glassUniforms.sampleXform         = glGetUniformLocation(program, "sampleXform");
    glassUniforms.nRGB                = glGetUniformLocation(program, "nRGB");
    glassUniforms.fresnelF0           = glGetUniformLocation(program, "fresnelF0");
    glassUniforms.lightUv             = glGetUniformLocation(program, "lightUv");
    glassUniforms.bevelLightUv        = glGetUniformLocation(program, "bevelLightUv");
    glassUniforms.pressGlow           = glGetUniformLocation(program, "pressGlow");
    glassUniforms.pressReachPx        = glGetUniformLocation(program, "pressReachPx");
    glassUniforms.pressBulge          = glGetUniformLocation(program, "pressBulge");

    return true;
}

bool CShaderManager::compileBlurShader() {
    if (!blurShader->createProgram(
            g_pHyprOpenGL->m_shaders->TEXVERTSRC,
            loadShaderSource("gaussianblur.frag"),
            true
        )) {
        HyprlandAPI::addNotification(PHANDLE,
            std::format("[{}] Failed to compile blur shader", PLUGIN_NAME),
            CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        return false;
    }

    const auto program = blurShader->program();

    blurUniforms.direction = glGetUniformLocation(program, "direction");
    blurUniforms.radius    = glGetUniformLocation(program, "blurRadius");

    return true;
}

bool CShaderManager::compileShapeFieldShaders() {
    auto build = [](SShapeFieldProgram& prog, const char* file, const char* a, const char* b, const char* c, const char* d) {
        if (!prog.shader->createProgram(g_pHyprOpenGL->m_shaders->TEXVERTSRC, loadShaderSource(file), true)) {
            HyprlandAPI::addNotification(PHANDLE, std::format("[{}] Failed to compile {}", PLUGIN_NAME, file),
                CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
            return false;
        }
        const auto program = prog.shader->program();
        prog.a = a ? glGetUniformLocation(program, a) : -1;
        prog.b = b ? glGetUniformLocation(program, b) : -1;
        prog.c = c ? glGetUniformLocation(program, c) : -1;
        prog.d = d ? glGetUniformLocation(program, d) : -1;
        return true;
    };
    return build(fieldSeed, "fieldseed.frag", "maskUVOffset", "maskUVScale", "threshold", "usePeak") &&
           build(fieldStep, "fieldstep.frag", "stepPx", nullptr, nullptr, nullptr) &&
           build(fieldResolve, "fieldresolve.frag", "maxDist", "maskUVOffset", "maskUVScale", "maskTex") &&
           build(fieldBlur, "fieldblur.frag", "stepPx", "sigma", "finish", nullptr) &&
           build(fieldBox, "fieldbox.frag", "maskUVOffset", "maskUVScale", "threshold", "cells") &&
           build(barStyle, "barstyle.frag", "padding", "halfWindow", "across", nullptr) &&
           build(presence, "presence.frag", "range", nullptr, nullptr, nullptr) &&
           build(hold, "hold.frag", "maskUVOffset", "maskUVScale", "presenceRef", "presenceTex") &&
           build(copy, "copy.frag", nullptr, nullptr, nullptr, nullptr);
}

void CShaderManager::initializeIfNeeded() {
    // (a failed compile is not tried again every frame: on an older GPU each attempt
    // took long enough, with a notification each time, to freeze the desktop. The
    // glass stays off, everything draws as it would without it, until a reload.)
    if (m_initialized || m_compileFailed)
        return;
    if (m_renderer.empty())
        if (const auto* r = reinterpret_cast<const char*>(glGetString(GL_RENDERER)))
            m_renderer = r;

    m_initialized   = compileGlassShader() && compileBlurShader() && compileShapeFieldShaders();
    m_compileFailed = !m_initialized;
}

void CShaderManager::destroy() noexcept {
    glassShader->destroy();
    blurShader->destroy();
    fieldSeed.shader->destroy();
    fieldStep.shader->destroy();
    fieldResolve.shader->destroy();
    fieldBlur.shader->destroy();
    fieldBox.shader->destroy();
    barStyle.shader->destroy();
    presence.shader->destroy();
    hold.shader->destroy();
    copy.shader->destroy();
    m_initialized   = false;
    m_compileFailed = false;
}
