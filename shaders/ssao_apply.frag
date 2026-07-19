// multiplies the ambient occlusion term into the offscreen scene's own color
// attachment via hardware blending (GL_DST_COLOR, GL_ZERO - see ssao.cpp's
// ssaoApplyOcclusion). outputs the ao term alone instead of doing the
// multiply in-shader: this pass runs with the scene color texture still
// bound as the draw target, so sampling it here too would be a framebuffer
// feedback loop

uniform sampler2D u_ao;

in vec2 v_uv;

out vec4 fragColor;

void main()
{
    // bilinear filtered on purpose: upsampling the half-res ao term this way
    // is a free, cheap substitute for a dedicated blur pass
    float ao = texture(u_ao, v_uv).r;
    fragColor = vec4(ao, ao, ao, 1.0);
}
