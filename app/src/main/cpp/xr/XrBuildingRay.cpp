#if defined(ANDROID_XR)

#include "StdAfxRD.h"
#include "ObjMesh.h"
#include "MeshTri.h"
#include "XrBuildingRay.h"

void xrIntersectBuildingRays(cObjectNodeRoot& model, XrWorldRay (&rays)[2]) {
    if (model.GetAttr(ATTRUNKOBJ_IGNORE)) return;
    // PreDraw normally refreshes animation matrices, but laser hits are
    // prepared before either eye is drawn. Refresh them for this frame now.
    model.Update();
    for (cObjMesh* mesh : model.GetMeshChild()) {
        if (!mesh || !mesh->IsVisibleForXrRay()) continue;
        MatXf inverse = mesh->GetGlobalMatrix();
        if (std::abs(inverse.rot().det()) < 1.0e-12f) continue;
        inverse.Invert(); // Full inverse includes model scale.
        sBox6f bounds;
        mesh->GetBoundBox(bounds);
        Vect3f localOrigins[2], localDirections[2];
        bool intersects[2]{};
        for (unsigned hand = 0; hand < 2; ++hand) {
            if (rays[hand].distance <= 0.0f) continue;
            localOrigins[hand] = inverse * rays[hand].origin;
            localDirections[hand] = inverse.rot() * rays[hand].direction;
            intersects[hand] = xrRayIntersectsBox(localOrigins[hand], localDirections[hand],
                                                 bounds.min, bounds.max, rays[hand].distance);
        }
        if (!intersects[0] && !intersects[1]) continue;
        const cMeshTri* triangles = mesh->GetTri();
        if (!triangles->VertexBuffer || !triangles->PolygonBuffer) continue;
        // Read each triangle once; only rays that passed its mesh bounds test
        // perform the detailed intersection. No allocations or vertex copies.
        for (int index = 0; index < triangles->NumPolygon; ++index) {
            const sPolygon& polygon = triangles->PolygonBuffer[index];
            Vect3f a, b, c;
            triangles->VertexBuffer[polygon.p1 - triangles->OffsetVertex].getPos(a);
            triangles->VertexBuffer[polygon.p2 - triangles->OffsetVertex].getPos(b);
            triangles->VertexBuffer[polygon.p3 - triangles->OffsetVertex].getPos(c);
            for (unsigned hand = 0; hand < 2; ++hand) {
                if (!intersects[hand]) continue;
                const float hit = xrRayTriangleDistance(localOrigins[hand], localDirections[hand], a, b, c);
                if (hit > 0.0f && hit < rays[hand].distance) rays[hand].distance = hit;
            }
        }
    }
}

#endif // ANDROID_XR
