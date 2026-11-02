// Rewritten's corrected lighting: the scene's point lights for the Modern
// renderer (native/lighting.h).
//
// The original folds the strongest light near an object into one of its
// directional slots (CAnimated::Draw, UpdateLightingParams), a direction
// toward it. With corrected lighting that fold is skipped and every visible
// light lights characters and props per pixel, from where it is.

#include "game/draw_context.h"
#include "game/light.h"
#include "game/matrix.h"
#include "native/hooks.h"
#include "native/lighting.h"
#include "native/render.h"

void NativeGatherSceneLights(const CLightDrawList* list, const CMatrix& worldToCamera) {
    if (NativeCorrectedLighting() == 0) {
        return;
    }
    f32 view[3][4];
    worldToCamera.GetGAMECUBEMatrix34(view);
    NativePointLight lights[NATIVE_MAX_POINT_LIGHTS];
    int count = 0;
    for (int i = 0; list != NULL && i < list->count && count < NATIVE_MAX_POINT_LIGHTS; i++) {
        CLight* light = list->items[i];
        CVector3 pos;
        light->GetPosition(pos);
        NativePointLight& out = lights[count++];
        PSMTXMultVec(view, &pos.x, out.pos);  // as the compartments place them (CCPTMatBin::Link)
        out.radius = light->m_radius;
        for (int c = 0; c < 3; c++) {
            out.color[c] = light->m_color[c] / 255.0f;  // (0..255, as the folds and GX take them)
        }
    }
    NativeSceneLights(lights, count);
}
