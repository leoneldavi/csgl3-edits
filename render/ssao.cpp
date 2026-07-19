#include "stdafx.h"
#include "ssao.h"
#include "texture.h"

namespace Render
{

struct SsaoShader : BaseShader
{
    GLint u_projParams;
    GLint u_radius;
    GLint u_intensity;
};

// horizon technique ported from a real HBAO implementation (see ssao_horizon.frag)
// needs two extra uniforms the kernel technique has no use for
struct SsaoHorizonShader : SsaoShader
{
    GLint u_radiusToScreen;
    GLint u_invFullRes;
};

struct SsaoCompositeShader : BaseShader
{
    // no per-instance uniforms, both samplers are constant bindings
};

struct SsaoBlurShader : BaseShader
{
    GLint u_projParams;
    GLint u_texelSize;
};

// texture units are constant for the lifetime of the program, set once at link time
static const ShaderUniform s_ssaoUniforms[] = {
    { "u_depth", 0 },
    { "u_projParams", &SsaoShader::u_projParams },
    { "u_radius", &SsaoShader::u_radius },
    { "u_intensity", &SsaoShader::u_intensity }
};

static const ShaderUniform s_ssaoHorizonUniforms[] = {
    { "u_depth", 0 },
    { "u_random", 1 },
    { "u_projParams", &SsaoHorizonShader::u_projParams },
    { "u_radius", &SsaoHorizonShader::u_radius },
    { "u_intensity", &SsaoHorizonShader::u_intensity },
    { "u_radiusToScreen", &SsaoHorizonShader::u_radiusToScreen },
    { "u_invFullRes", &SsaoHorizonShader::u_invFullRes }
};

static const ShaderUniform s_compositeUniforms[] = {
    { "u_sceneColor", 0 },
    { "u_ao", 1 }
};

static const ShaderUniform s_blurUniforms[] = {
    { "u_ao", 0 },
    { "u_depth", 1 },
    { "u_projParams", &SsaoBlurShader::u_projParams },
    { "u_texelSize", &SsaoBlurShader::u_texelSize }
};

static SsaoShader s_ssaoKernelShader;
static SsaoHorizonShader s_ssaoHorizonShader;
static SsaoCompositeShader s_compositeShader;
static SsaoBlurShader s_blurShader;

// tiled 4x4 rotation/jitter texture sampled by ssao_horizon.frag: rg = cos/sin
// of a per-texel random rotation angle, b = a random [0,1] step-start jitter.
// resolution-independent (GL_REPEAT-tiled across the screen), so it's created
// once at init instead of alongside the resize-triggered render targets below
static GLuint s_randomTexture;

static cvar_t *gl_ssao;
static cvar_t *gl_ssao_technique;
static cvar_t *gl_ssao_radius;
static cvar_t *gl_ssao_strength;

static int s_width, s_height;
static int s_aoWidth, s_aoHeight;

static GLuint s_sceneFbo;
static GLuint s_sceneColorTexture;
static GLuint s_sceneDepthTexture;

static GLuint s_aoFbo;
static GLuint s_aoTexture;

// only used by the horizon technique - see the comment on ssao_blur.frag for
// why the kernel technique doesn't need this
static GLuint s_aoBlurFbo;
static GLuint s_aoBlurTexture;

static bool s_active;

// framebuffer that was actually bound when ssaoBeginScene redirected rendering
// away from it. must not assume this is 0: the engine may already have its own
// fbo bound (see the -nofbo launch option, and the read/draw save+restore
// internal_goldsrc.cpp does around MakeTiledTextureAtlas for the same reason)
static GLuint s_targetFbo;

static void CreateRandomTexture()
{
    const int kSize = 4;
    float data[kSize * kSize * 4];

    // xorshift32, seeded fixed: this only needs to be spatially varied, not
    // actually random every run, and avoids disturbing global rand() state
    uint32_t seed = 1;
    auto NextRand = [&seed]() -> float {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return (seed & 0xFFFFFF) / static_cast<float>(0xFFFFFF);
    };

    for (int i = 0; i < kSize * kSize; i++)
    {
        float angle = NextRand() * 6.28318530718f;
        data[i * 4 + 0] = cosf(angle);
        data[i * 4 + 1] = sinf(angle);
        data[i * 4 + 2] = NextRand();
        data[i * 4 + 3] = 0.0f;
    }

    textureGenTextures(1, &s_randomTexture);
    glBindTexture(GL_TEXTURE_2D, s_randomTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    // must tile: sampled via gl_FragCoord.xy / 4.0, wrapping is what gives
    // every screen pixel a jitter value in the first place
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, kSize, kSize, 0, GL_RGBA, GL_FLOAT, data);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void ssaoInit()
{
    gl_ssao = g_engfuncs.pfnRegisterVariable("gl_ssao", "1", FCVAR_ARCHIVE);
    // 0 = kernel sampling (cheaper), 1 = horizon marching (pricier, catches thinner occluders)
    gl_ssao_technique = g_engfuncs.pfnRegisterVariable("gl_ssao_technique", "0", FCVAR_ARCHIVE);
    gl_ssao_radius = g_engfuncs.pfnRegisterVariable("gl_ssao_radius", "48", FCVAR_ARCHIVE);
    gl_ssao_strength = g_engfuncs.pfnRegisterVariable("gl_ssao_strength", "1", FCVAR_ARCHIVE);

    shaderRegister(s_ssaoKernelShader, "ssao", {}, s_ssaoUniforms);
    shaderRegister(s_ssaoHorizonShader, "ssao_horizon", {}, s_ssaoHorizonUniforms);
    shaderRegister(s_compositeShader, "ssao_composite", {}, s_compositeUniforms);
    shaderRegister(s_blurShader, "ssao_blur", {}, s_blurUniforms);

    CreateRandomTexture();
}

static void DeleteTargets()
{
    if (!s_sceneFbo)
    {
        return;
    }

    glDeleteFramebuffers(1, &s_sceneFbo);
    glDeleteTextures(1, &s_sceneColorTexture);
    glDeleteTextures(1, &s_sceneDepthTexture);
    glDeleteFramebuffers(1, &s_aoFbo);
    glDeleteTextures(1, &s_aoTexture);
    glDeleteFramebuffers(1, &s_aoBlurFbo);
    glDeleteTextures(1, &s_aoBlurTexture);

    s_sceneFbo = 0;
    s_sceneColorTexture = 0;
    s_sceneDepthTexture = 0;
    s_aoFbo = 0;
    s_aoTexture = 0;
    s_aoBlurFbo = 0;
    s_aoBlurTexture = 0;
}

static void CreateTargets(int width, int height)
{
    DeleteTargets();

    s_width = width;
    s_height = height;

    // ao term is computed at half resolution: this is the main cost saver,
    // the noise it introduces gets smoothed back out by the bilinear upsample
    // in the composite pass
    s_aoWidth = Q_max(width / 2, 1);
    s_aoHeight = Q_max(height / 2, 1);

    textureGenTextures(1, &s_sceneColorTexture);
    glBindTexture(GL_TEXTURE_2D, s_sceneColorTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    textureGenTextures(1, &s_sceneDepthTexture);
    glBindTexture(GL_TEXTURE_2D, s_sceneDepthTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, width, height, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);

    glGenFramebuffers(1, &s_sceneFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s_sceneFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_sceneColorTexture, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, s_sceneDepthTexture, 0);
    GL3_ASSERT(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

    textureGenTextures(1, &s_aoTexture);
    glBindTexture(GL_TEXTURE_2D, s_aoTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, s_aoWidth, s_aoHeight, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);

    glGenFramebuffers(1, &s_aoFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s_aoFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_aoTexture, 0);
    GL3_ASSERT(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

    textureGenTextures(1, &s_aoBlurTexture);
    glBindTexture(GL_TEXTURE_2D, s_aoBlurTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, s_aoWidth, s_aoHeight, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);

    glGenFramebuffers(1, &s_aoBlurFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s_aoBlurFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_aoBlurTexture, 0);
    GL3_ASSERT(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

    glBindTexture(GL_TEXTURE_2D, 0);
}

bool ssaoBeginScene(int width, int height)
{
    if (!gl_ssao->value || width <= 0 || height <= 0)
    {
        s_active = false;
        return false;
    }

    if (!s_sceneFbo || width != s_width || height != s_height)
    {
        CreateTargets(width, height);
    }

    GLint currentFbo = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &currentFbo);
    s_targetFbo = static_cast<GLuint>(currentFbo);

    glBindFramebuffer(GL_FRAMEBUFFER, s_sceneFbo);

    s_active = true;
    return true;
}

void ssaoEndSceneAndComposite(int viewportX, int viewportY, int viewportW, int viewportH)
{
    GL3_ASSERT(s_active);
    s_active = false;

    // fullscreen triangle passes, no depth/blend/culling wanted for either of them
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);

    // pass 1: estimate occlusion from the depth buffer alone, at half resolution
    glBindFramebuffer(GL_FRAMEBUFFER, s_aoFbo);
    glViewport(0, 0, s_aoWidth, s_aoHeight);

    bool useHorizon = (gl_ssao_technique->value != 0.0f);
    // SsaoHorizonShader derives from SsaoShader, so the fields they share can
    // be set through one pointer regardless of which technique is active
    const SsaoShader *activeShader = useHorizon
        ? static_cast<const SsaoShader *>(&s_ssaoHorizonShader)
        : &s_ssaoKernelShader;

    glUseProgram(activeShader->program);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_sceneDepthTexture);

    const Matrix4 &proj = g_state.projectionMatrix;
    float radius = Q_max(gl_ssao_radius->value, 1.0f);
    glUniform4f(activeShader->u_projParams, 1.0f / proj.m00, 1.0f / proj.m11, proj.m22, proj.m32);
    glUniform1f(activeShader->u_radius, radius);
    glUniform1f(activeShader->u_intensity, gl_ssao_strength->value);

    if (useHorizon)
    {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, s_randomTexture);

        // radius * 0.5 * fullResHeight * proj.m11 (proj.m11 = 1/tan(fovY*0.5)
        // for a standard perspective matrix) - cross-checked against this
        // file's own uvPerWorldUnit formula (0.5 / (u_projParams.xy * dist))
        // used by the old lightweight technique, converted from uv to pixels
        // by multiplying through by height
        float radiusToScreen = radius * static_cast<float>(s_height) * proj.m11 * 0.5f;
        glUniform1f(s_ssaoHorizonShader.u_radiusToScreen, radiusToScreen);
        glUniform2f(s_ssaoHorizonShader.u_invFullRes, 1.0f / s_width, 1.0f / s_height);
    }

    glDrawArrays(GL_TRIANGLES, 0, 3);

    // pass 1.5: blur away the horizon technique's per-pixel sample rotation
    // dither (see ssao_blur.frag). the kernel technique doesn't need this -
    // its noise characteristics are gentle enough that the free bilinear
    // upsample below already hides it, and blurring an already-clean signal
    // would only needlessly soften real contact-shadow detail
    GLuint aoResult = s_aoTexture;
    if (useHorizon)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, s_aoBlurFbo);
        glViewport(0, 0, s_aoWidth, s_aoHeight);

        glUseProgram(s_blurShader.program);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s_aoTexture);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, s_sceneDepthTexture);

        glUniform4f(s_blurShader.u_projParams, 1.0f / proj.m00, 1.0f / proj.m11, proj.m22, proj.m32);
        glUniform2f(s_blurShader.u_texelSize, 1.0f / s_aoWidth, 1.0f / s_aoHeight);

        glDrawArrays(GL_TRIANGLES, 0, 3);

        aoResult = s_aoBlurTexture;
    }

    // pass 2: composite the ao term back over the offscreen scene color, and
    // copy the result into the framebuffer the engine actually presents
    // (not necessarily 0, see s_targetFbo)
    glBindFramebuffer(GL_FRAMEBUFFER, s_targetFbo);

    SCREENINFO screenInfo{};
    screenInfo.iSize = sizeof(screenInfo);
    g_engfuncs.pfnGetScreenInfo(&screenInfo);

    int y = screenInfo.iHeight - viewportY - viewportH;
    glViewport(viewportX, y, viewportW, viewportH);

    glUseProgram(s_compositeShader.program);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_sceneColorTexture);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, aoResult);

    glDrawArrays(GL_TRIANGLES, 0, 3);

    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);

    glEnable(GL_DEPTH_TEST);
}

}
