// cheap ambient occlusion: no normal g-buffer is used, but an approximate
// view-space normal is reconstructed from the depth buffer via screen-space
// derivatives so the sample kernel can be oriented into a hemisphere above
// the local surface. this matters: a naive full-sphere kernel compared
// directly against depth self-occludes on any flat, camera-facing surface
// (about half the samples point "into" the surface and immediately register
// as occluded by that same surface), which reads as dark dithering over the
// whole scene rather than contact shadows near actual geometry

uniform sampler2D u_depth;

// x/y: 1 / projectionMatrix[0][0], 1 / projectionMatrix[1][1]
// z/w: projectionMatrix[2][2], projectionMatrix[3][2]
uniform vec4 u_projParams;

uniform float u_radius;
uniform float u_intensity;

in vec2 v_uv;

out vec4 fragColor;

const int kSampleCount = 8;

// local +z hemisphere, biased toward the origin for more detail close up.
// gets rotated into the surface's tangent space per pixel below
const vec3 kKernel[kSampleCount] = vec3[kSampleCount](
    vec3(0.20273, 0.00000, 0.01270),
    vec3(-0.16523, 0.15136, 0.04277),
    vec3(0.02310, -0.26318, 0.08691),
    vec3(0.19320, 0.25200, 0.15449),
    vec3(-0.36892, -0.06526, 0.25488),
    vec3(0.35423, -0.22533, 0.39746),
    vec3(-0.11019, 0.40992, 0.59160),
    vec3(-0.14485, -0.27890, 0.84668)
);

// Jimenez et al. 2014, cheap per-pixel dither with no noise texture needed
float InterleavedGradientNoise(vec2 fragCoord)
{
    return fract(52.98291893 * fract(dot(fragCoord, vec2(0.06711056, 0.00583715))));
}

vec3 ViewPositionFromDepth(vec2 uv, float depth)
{
    vec3 ndc = vec3(uv * 2.0 - 1.0, depth * 2.0 - 1.0);

    float viewZ = -u_projParams.w / (ndc.z + u_projParams.z);
    float viewX = ndc.x * -viewZ * u_projParams.x;
    float viewY = ndc.y * -viewZ * u_projParams.y;

    return vec3(viewX, viewY, viewZ);
}

void main()
{
    float depth = texture(u_depth, v_uv).r;

    // nothing was drawn here (sky/far plane): fully unoccluded
    if (depth >= 0.9999)
    {
        fragColor = vec4(1.0);
        return;
    }

    vec3 viewPos = ViewPositionFromDepth(v_uv, depth);

    // approximate surface normal from neighboring view-space positions.
    // viewPos points from the camera (origin) toward the surface, so a
    // normal facing back at the camera satisfies dot(normal, viewPos) < 0
    vec3 normal = normalize(cross(dFdx(viewPos), dFdy(viewPos)));
    normal = faceforward(normal, viewPos, normal);

    float angle = InterleavedGradientNoise(gl_FragCoord.xy) * 6.28318530718;
    vec3 randomVec = vec3(cos(angle), sin(angle), 0.0);

    vec3 tangent = normalize(randomVec - normal * dot(randomVec, normal));
    vec3 bitangent = cross(normal, tangent);
    mat3 tbn = mat3(tangent, bitangent, normal);

    float bias = 0.02 * u_radius;
    float occlusion = 0.0;

    for (int i = 0; i < kSampleCount; i++)
    {
        vec3 samplePos = viewPos + (tbn * kKernel[i]) * u_radius;

        float sampleClipW = -samplePos.z;
        vec2 sampleNdc = vec2(samplePos.x / (u_projParams.x * sampleClipW),
            samplePos.y / (u_projParams.y * sampleClipW));
        vec2 sampleUv = sampleNdc * 0.5 + 0.5;

        if (any(lessThan(sampleUv, vec2(0.0))) || any(greaterThan(sampleUv, vec2(1.0))))
        {
            continue;
        }

        float sceneDepth = texture(u_depth, sampleUv).r;
        vec3 sceneViewPos = ViewPositionFromDepth(sampleUv, sceneDepth);

        float rangeCheck = smoothstep(0.0, 1.0, u_radius / max(0.0001, abs(viewPos.z - sceneViewPos.z)));
        occlusion += (sceneViewPos.z >= samplePos.z + bias ? 1.0 : 0.0) * rangeCheck;
    }

    float ao = 1.0 - (occlusion / float(kSampleCount)) * u_intensity;

    fragColor = vec4(clamp(ao, 0.0, 1.0));
}
