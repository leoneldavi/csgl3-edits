# SSAO cvar roadmap

Tracks future `gl_ssao_*` cvars that were considered but not implemented, because
they'd need to bake into the shader *source* (a `#define`, a fixed-size array, an
`#if` branch) rather than just being a uniform read each frame. Nothing here is
built yet — this is a list to pick from later, not a plan in progress.

## Already done (no recompile involved)

`gl_ssao` (on/off), `gl_ssao_radius`, `gl_ssao_strength` — plain uniforms read
every frame in `ssaoEndSceneAndComposite`. Changing them takes effect
immediately, no shader rebuild. See `render/ssao.cpp`.

`gl_ssao_technique` (`0` = kernel sampling, `1` = horizon marching, see
`shaders/ssao_horizon.frag`) — this one turned out not to need the recompile
machinery either, and isn't really the same kind of item as the rest of this
list. It's not a macro baked into *one* shader, it's a straight pick between
*two already-compiled programs* (`s_ssaoKernelShader`/`s_ssaoHorizonShader` in
`render/ssao.cpp`), same as how the renderer already juggles many distinct
programs (lightmapped/sprite/water/sky) — just a `glUseProgram` choice at draw
time. No `ShaderOption`, no `AddMacro`, no `shaderUpdate()` involved. Worth
remembering as the default answer for "swap between two different algorithms"
in the future: only reach for the macro/recompile system in this doc when it's
genuinely the *same* shader with a compile-time constant changing, not a
different algorithm entirely.

## How the existing recompile-on-cvar-change mechanism actually works

Worth understanding before adding to this list, since it's the thing that makes
each item cheap or not. `gl_overbright`/`gamma`/`brightness`/`lightgamma` are the
only precedent (`render/gamma.cpp` + `render/shader.cpp`):

1. `ShaderManagerState` (`shader.cpp:52`) has hardcoded fields: `brightness`,
   `gamma`, `lightgamma`, `overbright`.
2. `GenerateVariantSource()` (`shader.cpp:200`) unconditionally `AddMacro`s all
   four into *every* shader's source as `#define V_BRIGHTNESS ...` etc.
3. `shaderUpdateGamma()` (`shader.cpp:388`) compares incoming values against the
   stored ones and sets `recompileQueued = true` on any change.
4. `gammaUpdate()` (`gamma.cpp:89`) calls that every frame; it's a cheap no-op
   check when nothing changed.
5. `shaderUpdate()` (`shader.cpp:346`), also called every frame from
   `RenderScene()`, recompiles *all* registered shaders' *all* variants when
   `recompileQueued` is set.

There is no generic "register a named macro cvar" API — it's four fields,
hand-wired. Adding one more macro this way (copy the pattern: one field, one
`AddMacro` call, one comparison in a setter) is maybe 15-20 minutes of
mechanical work per macro. Adding a *third* or *fourth* one starts to make
`ShaderManagerState`/`GenerateVariantSource` worth generalizing into an actual
small registry instead of more copy-pasted fields — not required up front, just
flag it if this list grows past 2-3 implemented entries.

Note this is a *global* recompile (every registered shader gets the macro,
whether it uses it or not — `AddMacro` only skips emitting the `#define` if the
macro name isn't textually present in that shader's source, so it's harmless
noise for unrelated shaders). This is different from the per-surface
`ShaderOption`/`shaderSelect` permutation system (`ALPHA_TEST`, `MULTI_STYLE`,
`DETAIL`, etc.) used by `lightmapped.frag` — that picks between *already
compiled* variants per draw call based on per-surface data, which doesn't fit
a single global quality knob like these would be.

## Candidate cvars

| cvar | what it'd do | why it needs a recompile | effort |
|---|---|---|---|
| `gl_ssao_debug` | `0`=off, `1`=show raw AO buffer, `2`=show reconstructed normal, `3`=show linearized depth, instead of compositing | debug output paths should be `#if`-gated out of the normal build entirely, not an extra runtime branch every pixel takes | Low |
| `gl_ssao_falloff` | switch the range-check curve (`smoothstep` vs linear vs a harder cutoff) in `ssao.frag` | it's a shape of a curve, not a numeric knob — needs an `#if` around the rangeCheck line | Low |
| `gl_ssao_samples` | tiered sample count (e.g. 4 / 8 / 16) instead of the fixed `kSampleCount = 8` | `kKernel[]` is a compile-time GLSL array literal; a tier means swapping in a differently-sized precomputed kernel. **Could instead** be done as a plain uniform loop bound over one max-size array with no recompile — worth doing that way unless the fixed-size unrolled loop turns out to matter for perf on the low-end GPUs this project targets (dynamic loop bounds block compiler unrolling) | Low-Medium (Low if done as a uniform instead) |
| `gl_ssao_quality` | single tiered knob bundling sample count + hemisphere-vs-sphere sampling (see below) + maybe AO buffer resolution, instead of several orthogonal cvars | same as above, plus it's the more user-friendly shape (`0/1/2` instead of five separate cvars nobody remembers) | Medium — mostly the design work of picking what each tier bundles |
| `gl_ssao_sphere_fallback` | drop the per-pixel normal reconstruction (`dFdx`/`dFdy` + TBN in `ssao.frag`) and go back to the plain full-sphere kernel from before the self-occlusion fix, as a cheaper-but-noisier mode | toggling out the normal-reconstruction code path entirely (not just its result) means `#if`-ing around it, otherwise you pay the cost without using it | Low |
| `gl_ssao_blur` | add an actual small depth-aware blur pass instead of relying on the half-res→full-res bilinear upsample as a free blur | not really a "recompile" item — this is a new shader (`ssao_blur.vert/frag`) plus a new render target and pass in `ssao.cpp`, closer to a small feature than a cvar | Medium-High |
| `gl_ssao_resolution` | full-res / half-res (current) / quarter-res AO buffer | **not a shader recompile at all** — `s_aoWidth`/`s_aoHeight` are just computed from a divisor in `ssao.cpp`'s `CreateTargets()`. Listed here for completeness since people will ask for it, but it's actually the cheapest item on this page | Low (and doesn't belong in this doc, really) |

## Is this too much work?

Not per-item, no — the recompile plumbing already exists and each new macro is
small once you copy the gamma pattern. What adds up is testing: every tier/mode
needs eyeballing across a few maps to make sure it doesn't reintroduce the kind
of self-occlusion artifact the hemisphere fix just solved, and that's real time
regardless of how small the code change is.

If picking this back up, suggested order: `gl_ssao_debug` first (cheap, and
makes eyeballing the *other* items on this list much faster), then
`gl_ssao_resolution` (cheapest real win, not even a recompile), then decide
whether `gl_ssao_quality` as one bundled tier is worth it over shipping
`gl_ssao_samples` alone. `gl_ssao_blur` last, since it's the only item that's
an actual new pass rather than a knob on the existing one.
