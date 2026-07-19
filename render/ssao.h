#ifndef SSAO_H
#define SSAO_H

namespace Render
{

void ssaoInit();

// call right before opaque scene rendering. if ambient occlusion is enabled,
// redirects scene rendering (world, entities, sky, everything) into an
// offscreen target sized to width x height and returns true, in which case
// the caller must follow up with ssaoApplyOcclusion and then
// ssaoEndSceneAndComposite once the scene is done drawing
bool ssaoBeginScene(int width, int height);

// call once opaque geometry has been drawn (and its draw calls actually
// executed, not just recorded) and before any translucent geometry (sprites,
// particles, beams, transparent triapi, viewmodel) is drawn. computes
// occlusion from the opaque-only depth buffer and multiplies it into the
// offscreen scene color in place, so translucents drawn afterward composite
// on top of already-occluded opaque shading without ever contributing to
// (or receiving) ambient occlusion themselves - which is what keeps smoke
// and other additive/alpha sprites from leaking AO through them. only call
// this if ssaoBeginScene returned true this frame
void ssaoApplyOcclusion();

// copies the offscreen scene (opaque + translucents, ao already baked into
// the opaque part) back into the currently bound (real) framebuffer at the
// given viewport rectangle. only call this if ssaoBeginScene returned true
// this frame
void ssaoEndSceneAndComposite(int viewportX, int viewportY, int viewportW, int viewportH);

}

#endif // SSAO_H
