#include "Diagnostics.hpp"
#include "ShapeField.hpp"
#include <vector>
#include "Globals.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/gl/GLFramebuffer.hpp>

namespace ShapeField {

static GLuint fbIdOf(const SP<Render::IFramebuffer>& fb) {
    return dynamic_cast<Render::GL::CGLFramebuffer*>(fb.get())->getFBID();
}

// Half-float colour targets: core in GLES 3.2, an extension on 3.0 (Hyprland
// falls back to a 3.0 context). Allocating one without support aborts the
// compositor (the framebuffer completeness assert), so ask first, once.
static bool halfFloatRenderable() {
    static const bool ok = [] {
        if (g_pHyprOpenGL->m_eglContextVersion == Render::GL::CHyprOpenGLImpl::EGL_CONTEXT_GLES_3_2)
            return true;
        GLint n = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &n);
        for (GLint i = 0; i < n; i++) {
            const auto* e = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, i));
            if (e && (std::string_view(e) == "GL_EXT_color_buffer_half_float" || std::string_view(e) == "GL_EXT_color_buffer_float"))
                return true;
        }
        return false;
    }();
    return ok;
}

// false when the GPU can't render to half-float textures: the layer then falls
// back to plain alpha-masked glass
static bool ensure(SP<Render::IFramebuffer>& fb, const char* name, int w, int h) {
    if (!fb)
        fb = g_pHyprRenderer->createFB(name);
    if (fb->m_size.x != w || fb->m_size.y != h || fb->m_drmFormat != DRM_FORMAT_ABGR16161616F)
        return fb->alloc(w, h, DRM_FORMAT_ABGR16161616F);
    return fb->isAllocated();
}

bool build(GLuint maskTexture, const Vector2D& maskUVOffset, const Vector2D& maskUVScale, int width, int height,
           float threshold, float maxDist, float sigma, SP<Render::IFramebuffer>& ping, SP<Render::IFramebuffer>& pong,
           SP<Render::IFramebuffer>& out, const SP<Render::IFramebuffer>& callerFramebuffer, GLuint peakTexture) {
    auto& shaders = g_pGlobalState->shaderManager;
    if (!shaders.isInitialized() || width <= 0 || height <= 0 || !callerFramebuffer || maskTexture == 0 || !halfFloatRenderable())
        return false;

    if (!ensure(ping, "hyprglass-field-a", width, height) || !ensure(pong, "hyprglass-field-b", width, height) ||
        !ensure(out, "hyprglass-field", width, height))
        return false;

    static constexpr std::array<float, 9> FULLSCREEN_PROJECTION = {
        2.0f, 0.0f, 0.0f,
        0.0f, 2.0f, 0.0f,
       -1.0f,-1.0f, 1.0f,
    };

    // through Hyprland's cap cache: a raw glDisable leaves it believing the test
    // is on, and its next scissor() would then never turn it back on
    g_pHyprRenderer->blend(false);
    g_pHyprOpenGL->setCapStatus(GL_SCISSOR_TEST, false);
    g_pHyprOpenGL->setCapStatus(GL_STENCIL_TEST, false);
    g_pHyprOpenGL->setViewport(0, 0, width, height);
    glActiveTexture(GL_TEXTURE0);

    // seed: outside texels are their own nearest outside point
    {
        auto shader = g_pHyprOpenGL->useShader(shaders.fieldSeed.shader);
        shader->setUniformMatrix3fv(SHADER_PROJ, 1, GL_FALSE, FULLSCREEN_PROJECTION);
        shader->setUniformInt(SHADER_TEX, 0);
        glUniform2f(shaders.fieldSeed.a, maskUVOffset.x, maskUVOffset.y);
        glUniform2f(shaders.fieldSeed.b, maskUVScale.x, maskUVScale.y);
        glUniform1f(shaders.fieldSeed.c, threshold);
        glUniform1f(shaders.fieldSeed.d, peakTexture ? 1.0f : 0.0f);
        static GLint peakLoc = -1;
        static GLuint peakProg = 0;
        if (peakProg != shaders.fieldSeed.shader->program()) {
            peakProg = shaders.fieldSeed.shader->program();
            peakLoc  = glGetUniformLocation(peakProg, "peakTex");
        }
        glUniform1i(peakLoc, 1);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, peakTexture);
        glActiveTexture(GL_TEXTURE0);
        glBindVertexArray(shader->getUniformLocation(SHADER_SHADER_VAO));
        glBindFramebuffer(GL_FRAMEBUFFER, fbIdOf(ping));
        glBindTexture(GL_TEXTURE_2D, maskTexture);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    // flood: steps of half the reach down to 1, plus one extra 1 (JFA+1)
    int reach = 1;
    while (reach < maxDist)
        reach <<= 1;
    {
        auto shader = g_pHyprOpenGL->useShader(shaders.fieldStep.shader);
        shader->setUniformMatrix3fv(SHADER_PROJ, 1, GL_FALSE, FULLSCREEN_PROJECTION);
        shader->setUniformInt(SHADER_TEX, 0);
        glBindVertexArray(shader->getUniformLocation(SHADER_SHADER_VAO));
        bool fromPing = true;
        auto pass = [&](int step) {
            glUniform1f(shaders.fieldStep.a, static_cast<float>(step));
            glBindFramebuffer(GL_FRAMEBUFFER, fbIdOf(fromPing ? pong : ping));
            (fromPing ? ping : pong)->getTexture()->bind();
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            fromPing = !fromPing;
        };
        for (int step = reach / 2; step >= 1; step >>= 1)
            pass(step);
        pass(1);
        if (!fromPing)
            std::swap(ping, pong); // the result always ends up in `ping`
    }

    // resolve into distances, smoothed
    {
        auto shader = g_pHyprOpenGL->useShader(shaders.fieldResolve.shader);
        shader->setUniformMatrix3fv(SHADER_PROJ, 1, GL_FALSE, FULLSCREEN_PROJECTION);
        shader->setUniformInt(SHADER_TEX, 0);
        glUniform1f(shaders.fieldResolve.a, maxDist);
        glUniform2f(shaders.fieldResolve.b, maskUVOffset.x, maskUVOffset.y);
        glUniform2f(shaders.fieldResolve.c, maskUVScale.x, maskUVScale.y);
        glUniform1i(shaders.fieldResolve.d, 1);
        // (a light theme: what counts as text, looked up as fieldSeed's peakTex is)
        static GLint  lightLoc  = -1;
        static GLuint lightProg = 0;
        if (lightProg != shaders.fieldResolve.shader->program()) {
            lightProg = shaders.fieldResolve.shader->program();
            lightLoc  = glGetUniformLocation(lightProg, "lightUi");
        }
        const auto& cfg = g_pGlobalState->config;
        glUniform1f(lightLoc, cfg.lightUi && **cfg.lightUi > 0.5 ? 1.0f : 0.0f);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, maskTexture);
        glActiveTexture(GL_TEXTURE0);
        glBindVertexArray(shader->getUniformLocation(SHADER_SHADER_VAO));
        glBindFramebuffer(GL_FRAMEBUFFER, fbIdOf(pong));
        ping->getTexture()->bind();
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    // round off the corner creases: signed blur across, then down (into `out`)
    {
        auto shader = g_pHyprOpenGL->useShader(shaders.fieldBlur.shader);
        shader->setUniformMatrix3fv(SHADER_PROJ, 1, GL_FALSE, FULLSCREEN_PROJECTION);
        shader->setUniformInt(SHADER_TEX, 0);
        glUniform1f(shaders.fieldBlur.b, std::clamp(sigma, 1.0f, 8.0f));
        glBindVertexArray(shader->getUniformLocation(SHADER_SHADER_VAO));
        glUniform2f(shaders.fieldBlur.a, 1.0f, 0.0f);
        glUniform1f(shaders.fieldBlur.c, 0.0f);
        glBindFramebuffer(GL_FRAMEBUFFER, fbIdOf(ping));
        pong->getTexture()->bind();
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glUniform2f(shaders.fieldBlur.a, 0.0f, 1.0f);
        glUniform1f(shaders.fieldBlur.c, 1.0f);
        glBindFramebuffer(GL_FRAMEBUFFER, fbIdOf(out));
        ping->getTexture()->bind();
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    g_pHyprRenderer->blend(true);
    glBindFramebuffer(GL_FRAMEBUFFER, fbIdOf(callerFramebuffer));
    glBindVertexArray(0);
    g_pHyprOpenGL->setViewport(0, 0,
        static_cast<int>(callerFramebuffer->m_size.x),
        static_cast<int>(callerFramebuffer->m_size.y));
    return true;
}

bool hold(GLuint maskTexture, const Vector2D& maskUVOffset, const Vector2D& maskUVScale, int width, int height,
          GLuint presenceTexture, float presenceRef, bool clear, SP<Render::IFramebuffer>& out,
          const SP<Render::IFramebuffer>& callerFramebuffer) {
    auto& shaders = g_pGlobalState->shaderManager;
    if (!shaders.isInitialized() || width <= 0 || height <= 0 || !callerFramebuffer || maskTexture == 0)
        return false;
    // (no presence texture: held as it is now, ungated: a close that came before
    // the card settled still leaves from what was on screen)
    const bool gated = presenceTexture != 0;
    if (!out)
        out = g_pHyprRenderer->createFB("hyprglass-held");
    if (out->m_size.x != width || out->m_size.y != height || out->m_drmFormat != DRM_FORMAT_ABGR8888) {
        if (!out->alloc(width, height, DRM_FORMAT_ABGR8888))
            return false;
        clear = true; // a new allocation holds garbage
    }

    static constexpr std::array<float, 9> FULLSCREEN_PROJECTION = {
        2.0f, 0.0f, 0.0f,
        0.0f, 2.0f, 0.0f,
       -1.0f,-1.0f, 1.0f,
    };
    g_pHyprRenderer->blend(false);
    g_pHyprOpenGL->setCapStatus(GL_SCISSOR_TEST, false);
    g_pHyprOpenGL->setCapStatus(GL_STENCIL_TEST, false);
    g_pHyprOpenGL->setViewport(0, 0, width, height);
    glBindFramebuffer(GL_FRAMEBUFFER, fbIdOf(out));
    if (clear) {
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    auto shader = g_pHyprOpenGL->useShader(shaders.hold.shader);
    shader->setUniformMatrix3fv(SHADER_PROJ, 1, GL_FALSE, FULLSCREEN_PROJECTION);
    shader->setUniformInt(SHADER_TEX, 0);
    glUniform2f(shaders.hold.a, maskUVOffset.x, maskUVOffset.y);
    glUniform2f(shaders.hold.b, maskUVScale.x, maskUVScale.y);
    glUniform1f(shaders.hold.c, gated ? presenceRef : -1.0f); // (-1: the gate always passes)
    glUniform1i(shaders.hold.d, 1);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, gated ? presenceTexture : maskTexture);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, maskTexture);
    glBindVertexArray(shader->getUniformLocation(SHADER_SHADER_VAO));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    g_pHyprRenderer->blend(true);
    glBindFramebuffer(GL_FRAMEBUFFER, fbIdOf(callerFramebuffer));
    glBindVertexArray(0);
    g_pHyprOpenGL->setViewport(0, 0, static_cast<int>(callerFramebuffer->m_size.x), static_cast<int>(callerFramebuffer->m_size.y));
    return true;
}

CContentProbe::~CContentProbe() {
    if (!m_pbo && !m_fence)
        return;
    if (g_pHyprOpenGL)
        g_pHyprOpenGL->makeEGLCurrent();
    if (m_fence)
        glDeleteSync(m_fence);
    if (m_pbo)
        glDeleteBuffers(1, &m_pbo);
}

bool buildStyleStrip(GLuint sampleTexture, const Vector2D& padding, bool across, int length, float halfWindowPx,
                     SP<Render::IFramebuffer>& out, const SP<Render::IFramebuffer>& callerFramebuffer) {
    auto& shaders = g_pGlobalState->shaderManager;
    if (!shaders.isInitialized() || length <= 0 || !callerFramebuffer || sampleTexture == 0)
        return false;

    const int n = std::max(2, (length + 7) / 8), w = across ? n : 1, h = across ? 1 : n;
    if (!out)
        out = g_pHyprRenderer->createFB("hyprglass-bar-style");
    if ((out->m_size.x != w || out->m_size.y != h || out->m_drmFormat != DRM_FORMAT_ABGR8888) && !out->alloc(w, h, DRM_FORMAT_ABGR8888))
        return false;

    static constexpr std::array<float, 9> FULLSCREEN_PROJECTION = {
        2.0f, 0.0f, 0.0f,
        0.0f, 2.0f, 0.0f,
       -1.0f,-1.0f, 1.0f,
    };
    g_pHyprRenderer->blend(false);
    g_pHyprOpenGL->setCapStatus(GL_SCISSOR_TEST, false);
    g_pHyprOpenGL->setViewport(0, 0, w, h);
    glActiveTexture(GL_TEXTURE0);
    auto shader = g_pHyprOpenGL->useShader(shaders.barStyle.shader);
    shader->setUniformMatrix3fv(SHADER_PROJ, 1, GL_FALSE, FULLSCREEN_PROJECTION);
    shader->setUniformInt(SHADER_TEX, 0);
    glUniform2f(shaders.barStyle.a, padding.x, padding.y);
    glUniform1f(shaders.barStyle.b, halfWindowPx / static_cast<float>(length));
    glUniform1f(shaders.barStyle.c, across ? 1.0f : 0.0f);
    glBindVertexArray(shader->getUniformLocation(SHADER_SHADER_VAO));
    glBindFramebuffer(GL_FRAMEBUFFER, fbIdOf(out));
    glBindTexture(GL_TEXTURE_2D, sampleTexture);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    g_pHyprRenderer->blend(true);
    glBindFramebuffer(GL_FRAMEBUFFER, fbIdOf(callerFramebuffer));
    glBindVertexArray(0);
    g_pHyprOpenGL->setViewport(0, 0, static_cast<int>(callerFramebuffer->m_size.x), static_cast<int>(callerFramebuffer->m_size.y));
    return true;
}

static constexpr std::array<float, 9> CELLS_PROJECTION = {
    2.0f, 0.0f, 0.0f,
    0.0f, 2.0f, 0.0f,
   -1.0f,-1.0f, 1.0f,
};

// fieldbox.frag over a width x height box into `cells` (one texel per CELL px);
// leaves `cells` bound. False when it can't run.
// (sub: only these cells, x y w h; the rest keep what they had)
static bool drawCells(SP<Render::IFramebuffer>& cells, GLuint maskTexture, const Vector2D& maskUVOffset, const Vector2D& maskUVScale,
                      int cw, int ch, float threshold, const std::array<int, 4>* sub = nullptr) {
    auto& shaders = g_pGlobalState->shaderManager;
    if (!cells)
        cells = g_pHyprRenderer->createFB("hyprglass-field-cells");
    if ((cells->m_size.x != cw || cells->m_size.y != ch || cells->m_drmFormat != DRM_FORMAT_ABGR8888) &&
        !cells->alloc(cw, ch, DRM_FORMAT_ABGR8888))
        return false;

    g_pHyprRenderer->blend(false);
    g_pHyprOpenGL->setCapStatus(GL_SCISSOR_TEST, sub != nullptr);
    // (through Hyprland when it is rendering, never raw then: see GlassRenderer's kept
    // glass; its scissor() asserts a monitor, so outside a render the raw call stays)
    if (sub && g_pHyprRenderer->m_renderData.pMonitor)
        g_pHyprOpenGL->scissor((*sub)[0], (*sub)[1], (*sub)[2], (*sub)[3], false);
    else if (sub)
        glScissor((*sub)[0], (*sub)[1], (*sub)[2], (*sub)[3]);
    g_pHyprOpenGL->setViewport(0, 0, cw, ch);
    glActiveTexture(GL_TEXTURE0);
    auto shader = g_pHyprOpenGL->useShader(shaders.fieldBox.shader);
    shader->setUniformMatrix3fv(SHADER_PROJ, 1, GL_FALSE, CELLS_PROJECTION);
    shader->setUniformInt(SHADER_TEX, 0);
    glUniform2f(shaders.fieldBox.a, maskUVOffset.x, maskUVOffset.y);
    glUniform2f(shaders.fieldBox.b, maskUVScale.x, maskUVScale.y);
    glUniform1f(shaders.fieldBox.c, threshold);
    glUniform2f(shaders.fieldBox.d, static_cast<float>(cw), static_cast<float>(ch));
    glBindVertexArray(shader->getUniformLocation(SHADER_SHADER_VAO));
    glBindFramebuffer(GL_FRAMEBUFFER, fbIdOf(cells));
    glBindTexture(GL_TEXTURE_2D, maskTexture);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    if (sub)
        g_pHyprOpenGL->setCapStatus(GL_SCISSOR_TEST, false);
    return true;
}

bool CContentProbe::drawLive(GLuint maskTexture, const Vector2D& maskUVOffset, const Vector2D& maskUVScale, int width, int height, float threshold,
                             const CBox& contentLocal, const SP<Render::IFramebuffer>& callerFramebuffer) {
    auto& shaders = g_pGlobalState->shaderManager;
    if (!shaders.isInitialized() || width <= 0 || height <= 0 || !callerFramebuffer || maskTexture == 0)
        return false;

    const int cw = std::max(1, (width + CELL - 1) / CELL), ch = std::max(1, (height + CELL - 1) / CELL);
    // only the content's cells (the card on a full-screen panel layer): all the
    // presence pass reads, a fifteenth of a panel layer's cells
    const int x0 = std::clamp(static_cast<int>(contentLocal.x) / CELL, 0, cw - 1), y0 = std::clamp(static_cast<int>(contentLocal.y) / CELL, 0, ch - 1);
    const int x1 = std::clamp(static_cast<int>(std::ceil((contentLocal.x + contentLocal.w) / CELL)), x0 + 1, cw);
    const int y1 = std::clamp(static_cast<int>(std::ceil((contentLocal.y + contentLocal.h) / CELL)), y0 + 1, ch);
    const std::array<int, 4> sub{x0, y0, x1 - x0, y1 - y0};
    bool ok = drawCells(m_liveCells, maskTexture, maskUVOffset, maskUVScale, cw, ch, threshold, &sub);
    if (ok && !m_live)
        m_live = g_pHyprRenderer->createFB("hyprglass-presence");
    ok = ok && ((m_live->m_size.x == 1 && m_live->m_size.y == 1 && m_live->m_drmFormat == DRM_FORMAT_ABGR8888) ||
                m_live->alloc(1, 1, DRM_FORMAT_ABGR8888));
    if (ok) {
        g_pHyprOpenGL->setViewport(0, 0, 1, 1);
        auto shader = g_pHyprOpenGL->useShader(shaders.presence.shader);
        shader->setUniformMatrix3fv(SHADER_PROJ, 1, GL_FALSE, CELLS_PROJECTION);
        shader->setUniformInt(SHADER_TEX, 0);
        glUniform4f(shaders.presence.a, static_cast<float>(x0), static_cast<float>(y0), static_cast<float>(x1 - x0), static_cast<float>(y1 - y0));
        glBindVertexArray(shader->getUniformLocation(SHADER_SHADER_VAO));
        glBindFramebuffer(GL_FRAMEBUFFER, fbIdOf(m_live));
        m_liveCells->getTexture()->bind();
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    g_pHyprRenderer->blend(true);
    glBindFramebuffer(GL_FRAMEBUFFER, fbIdOf(callerFramebuffer));
    glBindVertexArray(0);
    g_pHyprOpenGL->setViewport(0, 0, static_cast<int>(callerFramebuffer->m_size.x), static_cast<int>(callerFramebuffer->m_size.y));
    return ok;
}

GLuint CContentProbe::drawPeak(GLuint maskTexture, const Vector2D& maskUVOffset, const Vector2D& maskUVScale, int width, int height,
                                const CBox& contentLocal, const SP<Render::IFramebuffer>& callerFramebuffer) {
    auto& shaders = g_pGlobalState->shaderManager;
    if (!shaders.isInitialized() || width <= 0 || height <= 0 || !callerFramebuffer || maskTexture == 0)
        return 0;
    const int cw = std::max(1, (width + CELL - 1) / CELL), ch = std::max(1, (height + CELL - 1) / CELL);
    const int x0 = std::clamp(static_cast<int>(contentLocal.x) / CELL, 0, cw - 1), y0 = std::clamp(static_cast<int>(contentLocal.y) / CELL, 0, ch - 1);
    const int x1 = std::clamp(static_cast<int>(std::ceil((contentLocal.x + contentLocal.w) / CELL)), x0 + 1, cw);
    const int y1 = std::clamp(static_cast<int>(std::ceil((contentLocal.y + contentLocal.h) / CELL)), y0 + 1, ch);
    const std::array<int, 4> sub{x0, y0, x1 - x0, y1 - y0};
    // (the same cells and threshold as the probe whose reading it stands in for)
    bool ok = drawCells(m_peakCells, maskTexture, maskUVOffset, maskUVScale, cw, ch, 0.004f, &sub);
    if (ok && !m_peak)
        m_peak = g_pHyprRenderer->createFB("hyprglass-peak");
    ok = ok && ((m_peak->m_size.x == 1 && m_peak->m_size.y == 1 && m_peak->m_drmFormat == DRM_FORMAT_ABGR8888) || m_peak->alloc(1, 1, DRM_FORMAT_ABGR8888));
    if (ok) {
        g_pHyprOpenGL->setViewport(0, 0, 1, 1);
        auto shader = g_pHyprOpenGL->useShader(shaders.presence.shader);
        shader->setUniformMatrix3fv(SHADER_PROJ, 1, GL_FALSE, CELLS_PROJECTION);
        shader->setUniformInt(SHADER_TEX, 0);
        glUniform4f(shaders.presence.a, static_cast<float>(x0), static_cast<float>(y0), static_cast<float>(x1 - x0), static_cast<float>(y1 - y0));
        glBindVertexArray(shader->getUniformLocation(SHADER_SHADER_VAO));
        glBindFramebuffer(GL_FRAMEBUFFER, fbIdOf(m_peak));
        m_peakCells->getTexture()->bind();
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    g_pHyprRenderer->blend(true);
    glBindFramebuffer(GL_FRAMEBUFFER, fbIdOf(callerFramebuffer));
    glBindVertexArray(0);
    g_pHyprOpenGL->setViewport(0, 0, static_cast<int>(callerFramebuffer->m_size.x), static_cast<int>(callerFramebuffer->m_size.y));
    return ok ? m_peak->getTexture()->m_texID : 0;
}

void CContentProbe::request(GLuint maskTexture, const Vector2D& maskUVOffset, const Vector2D& maskUVScale, int width, int height, float threshold,
                            const SP<Render::IFramebuffer>& callerFramebuffer) {
    auto& shaders = g_pGlobalState->shaderManager;
    if (!shaders.isInitialized() || width <= 0 || height <= 0 || !callerFramebuffer || maskTexture == 0)
        return;

    const int cw = std::max(1, (width + CELL - 1) / CELL), ch = std::max(1, (height + CELL - 1) / CELL);
    if (!drawCells(m_cells, maskTexture, maskUVOffset, maskUVScale, cw, ch, threshold)) {
        g_pHyprRenderer->blend(true);
        glBindFramebuffer(GL_FRAMEBUFFER, fbIdOf(callerFramebuffer));
        g_pHyprOpenGL->setViewport(0, 0, static_cast<int>(callerFramebuffer->m_size.x), static_cast<int>(callerFramebuffer->m_size.y));
        return;
    }

    // into the pixel buffer: returns at once, the copy happens on the GPU
    if (!m_pbo)
        glGenBuffers(1, &m_pbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo);
    if (cw != m_cw || ch != m_ch)
        glBufferData(GL_PIXEL_PACK_BUFFER, static_cast<GLsizeiptr>(cw) * ch * 4, nullptr, GL_STREAM_READ);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, cw, ch, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    if (m_fence)
        glDeleteSync(m_fence); // a newer request replaces an unfinished one
    m_fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    m_requestAt = std::chrono::steady_clock::now();
    m_cw = cw;
    m_ch = ch;

    g_pHyprRenderer->blend(true);
    glBindFramebuffer(GL_FRAMEBUFFER, fbIdOf(callerFramebuffer));
    glBindVertexArray(0);
    g_pHyprOpenGL->setViewport(0, 0, static_cast<int>(callerFramebuffer->m_size.x), static_cast<int>(callerFramebuffer->m_size.y));
}

void CContentProbe::finish(int ms) {
    if (m_fence)
        glClientWaitSync(m_fence, GL_SYNC_FLUSH_COMMANDS_BIT, static_cast<GLuint64>(ms) * 1000000ull);
}

std::optional<std::optional<SContent>> CContentProbe::poll() {
    if (!m_fence)
        return std::nullopt;
    const GLenum state = glClientWaitSync(m_fence, 0, 0); // never waits
    if (state != GL_ALREADY_SIGNALED && state != GL_CONDITION_SATISFIED)
        return std::nullopt;
    glDeleteSync(m_fence);
    m_fence    = nullptr;
    m_resultAt = m_requestAt;

    glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo);
    const auto* px = static_cast<const uint8_t*>(glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, static_cast<GLsizeiptr>(m_cw) * m_ch * 4, GL_MAP_READ_BIT));
    std::optional<SContent> result;
    if (px) {
        int     x1 = m_cw, y1 = m_ch, x2 = -1, y2 = -1;
        uint8_t peak = 0;
        uint64_t sum = 0; // (of squares: see SContent::coverage)
        m_lastCells.resize(static_cast<size_t>(m_cw) * m_ch);
        m_lastW = m_cw;
        m_lastH = m_ch;
        const bool withRef = !m_refCells.empty() && m_refW == m_cw && m_refH == m_ch;
        double     left = 0.0;
        int        solid = 0, appeared = 0, kept = 0;
        for (int y = 0; y < m_ch; y++) {
            const uint8_t* row = px + static_cast<size_t>(y) * m_cw * 4;
            for (int x = 0; x < m_cw; x++) {
                const uint8_t v = row[x * 4];
                const size_t  i = static_cast<size_t>(y) * m_cw + x;
                m_lastCells[i]  = v;
                if (withRef) {
                    const uint8_t r = m_refCells[i];
                    if (v > r + 24)
                        appeared++;
                    if (r > 64) {
                        left += std::min(1.0, static_cast<double>(v) / r);
                        solid++;
                        if (v + 2 >= r)
                            kept++;
                    }
                }
                if (!v)
                    continue;
                peak = std::max(peak, v);
                sum += static_cast<uint64_t>(v) * v;
                x1 = std::min(x1, x); x2 = std::max(x2, x);
                y1 = std::min(y1, y); y2 = std::max(y2, y);
            }
        }
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        // rows are the texture's rows: row 0 is v = 0, the glass shader's uv
        if (x2 >= 0)
            result = SContent{CBox{static_cast<double>(x1 * CELL), static_cast<double>(y1 * CELL),
                                   static_cast<double>((x2 - x1 + 1) * CELL), static_cast<double>((y2 - y1 + 1) * CELL)},
                              peak / 255.0f,
                              std::sqrt(static_cast<float>(sum) / (255.0f * 255.0f * static_cast<float>((x2 - x1 + 1) * (y2 - y1 + 1)))),
                              // (a fade dims every cell at once; rows going away leave the
                              // rest exactly as they were: that is new content, not a fade)
                              solid > 0 && appeared * 50 < solid && kept * 4 < solid ? static_cast<float>(left / solid) : 1.0f,
                              solid > 0 && appeared * 50 >= solid};
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    return result;
}

bool exactBox(const SP<Render::IFramebuffer>& field, CBox& out, const SP<Render::IFramebuffer>& callerFramebuffer) {
    if (!field || !field->isAllocated())
        return false;
    const int w = static_cast<int>(field->m_size.x), h = static_cast<int>(field->m_size.y);
    if (w < 3 || h < 3)
        return false;
    std::vector<float> row(static_cast<size_t>(w) * 4), col(static_cast<size_t>(h) * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, fbIdOf(field));
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, h / 2, w, 1, GL_RGBA, GL_FLOAT, row.data());
    glReadPixels(w / 2, 0, 1, h, GL_RGBA, GL_FLOAT, col.data());
    glBindFramebuffer(GL_FRAMEBUFFER, fbIdOf(callerFramebuffer));
    // signed distance, positive inside (R inside, G outside)
    auto sd = [](const std::vector<float>& v, int i) { return v[static_cast<size_t>(i) * 4] - v[static_cast<size_t>(i) * 4 + 1]; };
    auto edges = [&](const std::vector<float>& v, int n, double& lo, double& hi) {
        lo = hi = -1.0;
        for (int i = 1; i < n; i++) {
            const float a = sd(v, i - 1), b = sd(v, i);
            if (a <= 0.0f && b > 0.0f && lo < 0.0)
                lo = (i - 1) + 0.5 + a / (a - b);
            if (a > 0.0f && b <= 0.0f)
                hi = (i - 1) + 0.5 + a / (a - b);
        }
        return lo >= 0.0 && hi > lo;
    };
    double x0, x1, y0, y1;
    if (!edges(row, w, x0, x1) || !edges(col, h, y0, y1))
        return false;
    out = CBox{x0, y0, x1 - x0, y1 - y0};
    return true;
}

bool accentSpan(const SP<Render::IFramebuffer>& fb, const CBox& rect, bool alongX, double anchor, double reach, uint32_t rgb, double& lo, double& hi,
                const SP<Render::IFramebuffer>& callerFramebuffer) {
    if (!fb || !fb->isAllocated())
        return false;
    const int FW = static_cast<int>(fb->m_size.x), FH = static_cast<int>(fb->m_size.y);
    const int x0 = std::clamp(static_cast<int>(rect.x), 0, FW), x1 = std::clamp(static_cast<int>(rect.x + rect.w), 0, FW);
    const int y0 = std::clamp(static_cast<int>(rect.y), 0, FH), y1 = std::clamp(static_cast<int>(rect.y + rect.h), 0, FH);
    const int w = x1 - x0, h = y1 - y0;
    if (w < 2 || h < 2 || static_cast<size_t>(w) * h > 4000000)
        return false;
    const float kr = ((rgb >> 16) & 255) / 255.0f, kg = ((rgb >> 8) & 255) / 255.0f, kb = (rgb & 255) / 255.0f;
    std::vector<unsigned char> px(static_cast<size_t>(w) * h * 4);
    const int len = alongX ? w : h, across = alongX ? h : w;
    std::vector<char> hit(static_cast<size_t>(len));
    bool found = false;
    // (the way up the buffer was last time first: one readback, not two)
    static int lastFlip = 0;
    for (int pass = 0; pass < 2 && !found; pass++) {
        const int flip = pass == 0 ? lastFlip : 1 - lastFlip;
        glBindFramebuffer(GL_FRAMEBUFFER, fbIdOf(fb));
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(x0, flip ? FH - y1 : y0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        glBindFramebuffer(GL_FRAMEBUFFER, fbIdOf(callerFramebuffer));
        std::fill(hit.begin(), hit.end(), 0);
        for (int i = 0; i < len; i++)
            for (int j = 0; j < across && !hit[i]; j++) {
                const int    x = alongX ? i : j, y = alongX ? j : i;
                const auto*  p = &px[(static_cast<size_t>(y) * w + x) * 4];
                const float  a = p[3] / 255.0f;
                // (only an underline drawn nearly in full: Omarchy fades the last item's
                // out as the new one fades in, and the fading one, found first, was the
                // wrong item: Bluetooth came out of Wi-Fi's place)
                if (a < 0.7f)
                    continue;
                // (premultiplied: the colour is the pixel over its alpha)
                const float r = p[0] / 255.0f / a, g = p[1] / 255.0f / a, b = p[2] / 255.0f / a;
                if (std::abs(r - kr) < 0.16f && std::abs(g - kg) < 0.16f && std::abs(b - kb) < 0.16f)
                    hit[i] = 1;
            }
        // the run nearest the anchor (gaps of a px or two bridged)
        const int ai = static_cast<int>(anchor - (alongX ? x0 : y0));
        int best = -1;
        for (int d = 0; d <= static_cast<int>(reach) && best < 0; d++)
            for (int s : {ai - d, ai + d})
                if (s >= 0 && s < len && hit[s]) {
                    best = s;
                    break;
                }
        if (best < 0)
            continue;
        int a = best, b = best;
        for (int gap = 0; a > 0 && gap <= 2; a--)
            gap = hit[a - 1] ? 0 : gap + 1;
        for (int gap = 0; b < len - 1 && gap <= 2; b++)
            gap = hit[b + 1] ? 0 : gap + 1;
        while (!hit[a]) a++;
        while (!hit[b]) b--;
        lo    = (alongX ? x0 : y0) + a;
        hi    = (alongX ? x0 : y0) + b + 1;
        found = hi - lo >= 2;
        if (found)
            lastFlip = flip;
    }
    return found;
}

} // namespace ShapeField
