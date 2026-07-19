// blends the depth-only ambient occlusion term (computed at half resolution by
// ssao.frag) back into the offscreen scene color, and is also the point where
// the offscreen scene gets copied back to the real framebuffer

uniform sampler2D u_sceneColor;
uniform sampler2D u_ao;

in vec2 v_uv;

out vec4 fragColor;

void main()
{
    vec4 color = texture(u_sceneColor, v_uv);

    // bilinear filtered on purpose: upsampling the half-res AO term this way
    // is a free, cheap substitute for a dedicated blur pass
    float ao = texture(u_ao, v_uv).r;

    color.rgb *= ao;
    fragColor = color;
}
