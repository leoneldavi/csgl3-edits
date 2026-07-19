// small depth-aware blur over the (half-res) ao buffer. required, not
// cosmetic: ssao_horizon.frag's per-pixel sample rotation comes from
// InterleavedGradientNoise, a structured low-discrepancy dither pattern, not
// true randomness - every real implementation of this kind of technique
// blurs it away afterward. skipping this and relying only on the free
// half-res -> full-res bilinear upsample in the composite pass does nothing
// to cancel the dither structure *within* the half-res buffer, so it survives
// as a visible dot lattice that scales with radius (sample spacing) and
// strength (intensity multiplier) - exactly the "dots" artifact this fixes

uniform sampler2D u_ao;
uniform sampler2D u_depth;

// x/y: 1 / projectionMatrix[0][0], 1 / projectionMatrix[1][1]
// z/w: projectionMatrix[2][2], projectionMatrix[3][2]
uniform vec4 u_projParams;

uniform vec2 u_texelSize;

in vec2 v_uv;

out vec4 fragColor;

float LinearizeDepth(float depth)
{
    float ndcZ = depth * 2.0 - 1.0;
    return -u_projParams.w / (ndcZ + u_projParams.z);
}

void main()
{
    const int kRadius = 1; // 3x3
    const float kDepthSharpness = 0.05;

    float centerDepth = LinearizeDepth(texture(u_depth, v_uv).r);
    float centerAo = texture(u_ao, v_uv).r;

    float totalAo = centerAo;
    float totalWeight = 1.0;

    for (int y = -kRadius; y <= kRadius; y++)
    {
        for (int x = -kRadius; x <= kRadius; x++)
        {
            if (x == 0 && y == 0)
            {
                continue;
            }

            vec2 offset = vec2(x, y) * u_texelSize;
            vec2 sampleUv = v_uv + offset;

            float sampleDepth = LinearizeDepth(texture(u_depth, sampleUv).r);
            float sampleAo = texture(u_ao, sampleUv).r;

            // orthogonal neighbors weighted a bit more than diagonal ones,
            // roughly approximating a small gaussian
            float spatialWeight = (x == 0 || y == 0) ? 1.0 : 0.7;

            // reject neighbors whose depth diverges too much from the
            // center's so the blur doesn't bleed occlusion across real edges
            float depthWeight = exp(-abs(centerDepth - sampleDepth) * kDepthSharpness);

            float weight = spatialWeight * depthWeight;
            totalAo += sampleAo * weight;
            totalWeight += weight;
        }
    }

    fragColor = vec4(totalAo / totalWeight);
}
