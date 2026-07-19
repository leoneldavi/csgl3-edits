#ifndef SSAO_H
#define SSAO_H

namespace Render
{

void ssaoInit();

// call right before SceneRenderPass. if ambient occlusion is enabled, redirects
// scene rendering (world, entities, sky, everything) into an offscreen target
// sized to width x height and returns true, in which case the caller must
// follow up with ssaoEndSceneAndComposite once the scene is done drawing
bool ssaoBeginScene(int width, int height);

// runs the ambient occlusion pass and composites the offscreen scene back into
// the currently bound (real) framebuffer at the given viewport rectangle.
// only call this if ssaoBeginScene returned true this frame
void ssaoEndSceneAndComposite(int viewportX, int viewportY, int viewportW, int viewportH);

}

#endif // SSAO_H
