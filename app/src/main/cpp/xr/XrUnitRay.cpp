#if defined(ANDROID_XR)

#include "StdAfxRD.h"
#include "ObjMesh.h"
#include "MeshTri.h"
#include "XrUnitRay.h"

void xrIntersectUnitRays(cObjectNodeRoot& model, XrWorldRay (&rays)[2]) {
    if (model.GetAttr(ATTRUNKOBJ_IGNORE) ||
        (rays[0].distance <= 0.0f && rays[1].distance <= 0.0f)) return;
    const auto& meshes = model.GetMeshChild();
    if (meshes.empty()) return;
    sBox6f modelBounds;
    model.GetBoundBox(modelBounds);
    if (modelBounds.min.x > modelBounds.max.x ||
        modelBounds.min.y > modelBounds.max.y ||
        modelBounds.min.z > modelBounds.max.z) return;
    const MatXf& modelMatrix = model.GetGlobalMatrix();
    const Vect3f center = modelMatrix * ((modelBounds.min + modelBounds.max) * 0.5f);
    const Vect3f& scale = model.GetScale();
    const float radius = modelBounds.GetRadius() *
        std::max({scale.x, scale.y, scale.z});
    const float radiusSquared = radius * radius;
    bool candidate[2]{};
    for (unsigned hand = 0; hand < 2; ++hand) {
        if (rays[hand].distance <= 0.0f) continue;
        const Vect3f toCenter = center - rays[hand].origin;
        const float along = std::clamp(toCenter.dot(rays[hand].direction),
                                       0.0f, rays[hand].distance);
        if ((toCenter - rays[hand].direction * along).norm2() > radiusSquared)
            continue;
        candidate[hand] = true;
    }
    if (!candidate[0] && !candidate[1]) return;
    MatXf modelInverse = modelMatrix;
    if (std::abs(modelInverse.rot().det()) < 1.0e-12f) return;
    modelInverse.Invert();
    for (unsigned hand = 0; hand < 2; ++hand) {
        if (!candidate[hand]) continue;
        candidate[hand] = xrRayIntersectsBox(
            modelInverse * rays[hand].origin,
            modelInverse.rot() * rays[hand].direction,
            modelBounds.min, modelBounds.max, rays[hand].distance);
    }
    if (!candidate[0] && !candidate[1]) return;
    // PreDraw normally refreshes animation matrices, but laser hits are
    // prepared before either eye is drawn. Refresh them for this frame now.
    model.Update();
    for (cObjMesh* mesh : meshes) {
        if (!mesh || !mesh->IsVisibleForXrRay()) continue;
        MatXf inverse = mesh->GetGlobalMatrix();
        if (std::abs(inverse.rot().det()) < 1.0e-12f) continue;
        inverse.Invert(); // Full inverse includes model scale.
        sBox6f bounds;
        mesh->GetBoundBox(bounds);
        Vect3f localOrigins[2], localDirections[2];
        bool intersects[2]{};
        for (unsigned hand = 0; hand < 2; ++hand) {
            if (!candidate[hand] || rays[hand].distance <= 0.0f) continue;
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
