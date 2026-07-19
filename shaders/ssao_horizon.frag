// horizon-based ambient occlusion, ported down from NVIDIA's reference HBAO
// implementation (as shipped in hzqst/MetaHookSv's svencoop renderer, itself
// taken from NVIDIA's old HBAO SDK sample - recognizable by ComputeCoarseAO/
// RadiusPixels/RayPixels naming) to this engine's non-deinterleaved, non-
// compute GLSL 140 / GL 3.1 target.
//
// dropped entirely: the quarter-res deinterleaved gl_PrimitiveID + layered
// image2DArray rendering path, since it needs GL 4.3+ (image load/store,
// layered geometry-shader-free rendering) that doesn't exist on GL 3.1. we
// already get most of that path's win for free by running this pass at half
// resolution (see ssao.cpp's CreateTargets).
//
// kept faithfully: the per-sample NdotV+falloff AO integrator (sums every
// step's contribution instead of tracking a max horizon angle per direction -
// a real, deliberate difference from classic Bavoil-paper HBAO, but it's what
// NVIDIA's own shipped sample actually does), the MinDiff normal
// reconstruction, the pixel-snapped ray marching (round() to the nearest
// full-res texel instead of marching continuous uv - avoids interpolation
// artifacts), and the tiled per-pixel random-rotation texture jitter.
//
// swapped: their hardcoded normal sign flip (-ReconstructNormal) for
// faceforward(), since we can't be sure their winding convention matches
// ours and getting that wrong means fully inside-out ao - faceforward is
// what ssao.frag already relies on and it's proven correct there.

uniform sampler2D u_depth;
uniform sampler2D u_random;

// x/y: 1 / projectionMatrix[0][0], 1 / projectionMatrix[1][1]
// z/w: projectionMatrix[2][2], projectionMatrix[3][2]
uniform vec4 u_projParams;

uniform float u_radius;
uniform float u_intensity;

// world-space radius projected to full-res screen pixels at 1 unit of view
// depth (radius * 0.5 * fullResHeight / tan(fovY * 0.5)), divided by the
// per-pixel view depth below to get the actual sampling radius in pixels
uniform float u_radiusToScreen;

// 1 / full-res depth buffer resolution (not the half-res resolution this
// pass itself renders at - the depth buffer being marched is full-res)
uniform vec2 u_invFullRes;

in vec2 v_uv;

out vec4 fragColor;

const float kNumSteps = 4.0;
const float kNumDirections = 8.0; // texRandom's rotation granularity assumes this
const float kTwoPi = 6.28318530718;

// how many texels wide/tall the tiled random-rotation texture is
const float kRandomTexSize = 4.0;

// minimum NdotV a sample has to clear before it counts as an occluder -
// same purpose as ssao.frag's world-space bias, keeps a flat surface's own
// depth-quantization noise from self-occluding
const float kNDotVBias = 0.02;

// empirical contrast/brightness shaping applied to the raw integrated
// occlusion - matches NVIDIA's reference, which always tunes these
const float kAoMultiplier = 1.6;
const float kPowExponent = 1.4;

vec3 ViewPositionFromDepth(vec2 uv, float depth)
{
    vec3 ndc = vec3(uv * 2.0 - 1.0, depth * 2.0 - 1.0);

    float viewZ = -u_projParams.w / (ndc.z + u_projParams.z);
    float viewX = ndc.x * -viewZ * u_projParams.x;
    float viewY = ndc.y * -viewZ * u_projParams.y;

    return vec3(viewX, viewY, viewZ);
}

vec3 FetchViewPos(vec2 uv)
{
    return ViewPositionFromDepth(uv, texture(u_depth, uv).r);
}

// picks whichever of the two neighbor-to-center deltas is shorter, so
// reconstructing the normal near a depth discontinuity uses the side that's
// actually still on the same surface as the center pixel instead of
// smearing the normal across the edge
vec3 MinDiff(vec3 p, vec3 pr, vec3 pl)
{
    vec3 v1 = pr - p;
    vec3 v2 = p - pl;
    return (dot(v1, v1) < dot(v2, v2)) ? v1 : v2;
}

vec3 ReconstructNormal(vec2 uv, vec3 p)
{
    vec3 pr = FetchViewPos(uv + vec2(u_invFullRes.x, 0.0));
    vec3 pl = FetchViewPos(uv - vec2(u_invFullRes.x, 0.0));
    vec3 pt = FetchViewPos(uv + vec2(0.0, u_invFullRes.y));
    vec3 pb = FetchViewPos(uv - vec2(0.0, u_invFullRes.y));

    vec3 normal = normalize(cross(MinDiff(p, pr, pl), MinDiff(p, pt, pb)));
    return faceforward(normal, p, normal);
}

// linear falloff to zero at u_radius - algebraically the same curve as
// NVIDIA's "distanceSquare * negInvRadiusSq + 1.0", just written the clamped
// way ssao.frag already uses elsewhere in this codebase
float Falloff(float distanceSquare)
{
    return clamp(1.0 - distanceSquare / (u_radius * u_radius), 0.0, 1.0);
}

// P = view-space position at the kernel center, N = its normal,
// S = view-space position of the current marched sample
float ComputeAO(vec3 P, vec3 N, vec3 S)
{
    vec3 V = S - P;
    float VdotV = dot(V, V);
    float NdotV = dot(N, V) * inversesqrt(VdotV);
    return clamp(NdotV - kNDotVBias, 0.0, 1.0) * Falloff(VdotV);
}

vec2 RotateDirection(vec2 dir, vec2 cosSin)
{
    return vec2(dir.x * cosSin.x - dir.y * cosSin.y,
                dir.x * cosSin.y + dir.y * cosSin.x);
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
    vec3 normal = ReconstructNormal(v_uv, viewPos);

    // project the world-space radius to a per-pixel screen-space pixel
    // radius, foreshortened by this pixel's own view depth
    float radiusPixels = u_radiusToScreen / max(-viewPos.z, 1.0);

    // radius rounds down to nothing at this distance: every marched sample
    // would land back on the center texel and contribute zero real signal,
    // so skip the full direction/step loop below entirely
    if (radiusPixels < 1.0)
    {
        fragColor = vec4(1.0);
        return;
    }

    // divide by steps+1 so the farthest sample isn't sitting right at the
    // radius edge, where Falloff() would fully zero it out anyway
    float stepSizePixels = radiusPixels / (kNumSteps + 1.0);

    vec4 rand = texture(u_random, gl_FragCoord.xy / kRandomTexSize);

    float occlusion = 0.0;

    for (float dirIndex = 0.0; dirIndex < kNumDirections; dirIndex += 1.0)
    {
        float angle = (kTwoPi / kNumDirections) * dirIndex;
        vec2 direction = RotateDirection(vec2(cos(angle), sin(angle)), rand.xy);

        // jitter the first step's start within the step itself, so the ray's
        // sample positions don't line up identically for every pixel
        float rayPixels = rand.z * stepSizePixels + 1.0;

        for (float stepIndex = 0.0; stepIndex < kNumSteps; stepIndex += 1.0)
        {
            vec2 snappedUv = round(rayPixels * direction) * u_invFullRes + v_uv;

            // off-screen: the reference relies on CLAMP_TO_EDGE here, but that
            // means every remaining step in this direction would keep
            // resampling the exact same edge texel and contribute the same
            // nonzero occlusion over and over, biasing every screen edge.
            // marching is monotonic outward, so once we're off-screen every
            // later step in this direction is too - stop instead
            if (any(lessThan(snappedUv, vec2(0.0))) || any(greaterThan(snappedUv, vec2(1.0))))
            {
                break;
            }

            vec3 samplePos = FetchViewPos(snappedUv);

            rayPixels += stepSizePixels;

            occlusion += ComputeAO(viewPos, normal, samplePos);
        }
    }

    occlusion *= (u_intensity * kAoMultiplier) / (kNumDirections * kNumSteps);

    float ao = clamp(1.0 - occlusion * 2.0, 0.0, 1.0);
    fragColor = vec4(pow(ao, kPowExponent));
}
