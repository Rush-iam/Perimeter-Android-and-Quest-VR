#pragma once

#if defined(ANDROID_XR)
#include "IRenderDevice.h"
#include "DrawBuffer.h"
#include "VertexFormat.h"

// World geometry preserves stereo depth and XR-up orientation under head roll.
inline void xrWriteWorldQuad(sVertexXYZDT1* vertices, const Vect3f& topLeft,
                             const Vect3f& right, const Vect3f& down,
                             uint32_t diffuse, float u = 0, float v = 0,
                             float du = 1, float dv = 1) {
    const Vect3f positions[4] = {topLeft, topLeft + down,
                               topLeft + right, topLeft + right + down};
    for (unsigned i = 0; i < 4; ++i) {
        vertices[i].setPos(positions[i]);
        vertices[i].diffuse = diffuse;
        vertices[i].u1() = u + (i >= 2 ? du : 0);
        vertices[i].v1() = v + (i % 2 ? dv : 0);
    }
}

inline void xrWorldQuad(cInterfaceRenderDevice* renderer, const Vect3f& topLeft,
                        const Vect3f& right, const Vect3f& down,
                        const sColor4c& color, float u = 0, float v = 0,
                        float du = 1, float dv = 1) {
    auto* buffer = renderer->GetDrawBuffer(sVertexXYZDT1::fmt, PT_TRIANGLES);
    auto* vertices = buffer->LockQuad<sVertexXYZDT1>(1);
    xrWriteWorldQuad(vertices, topLeft, right, down, renderer->ConvertColor(color), u, v, du, dv);
    buffer->Unlock();
}

inline void xrWorldBox(cInterfaceRenderDevice* renderer, const Vect3f& base,
                       const Vect3f& dx, const Vect3f& dy, const Vect3f& dz,
                       const sColor4c& color) {
    auto* buffer = renderer->GetDrawBuffer(sVertexXYZDT1::fmt, PT_TRIANGLES);
    auto* vertices = buffer->LockQuad<sVertexXYZDT1>(6);
    const uint32_t diffuse = renderer->ConvertColor(color);
    xrWriteWorldQuad(vertices, base + dz, dx, dy, diffuse);
    xrWriteWorldQuad(vertices + 4, base, dx, dy, diffuse);
    xrWriteWorldQuad(vertices + 8, base, dx, dz, diffuse);
    xrWriteWorldQuad(vertices + 12, base + dy, dx, dz, diffuse);
    xrWriteWorldQuad(vertices + 16, base, dy, dz, diffuse);
    xrWriteWorldQuad(vertices + 20, base + dx, dy, dz, diffuse);
    buffer->Unlock();
}

class XrWorldOverlayDraw {
    cInterfaceRenderDevice* renderer_;
    uint32_t zwrite_, zenable_, zfunc_, cull_;
public:
    XrWorldOverlayDraw(cInterfaceRenderDevice* renderer, cCamera* camera,
                       bool depthTest = true, bool depthWrite = false)
        : renderer_(renderer), zwrite_(renderer->GetRenderState(RS_ZWRITEENABLE)),
          zenable_(renderer->GetRenderState(RS_ZENABLE)),
          zfunc_(renderer->GetRenderState(RS_ZFUNC)),
          cull_(renderer->GetRenderState(RS_CULLMODE)) {
        renderer->SetDrawTransform(camera);
        renderer->SetWorldMat4f(nullptr);
        renderer->SetRenderState(RS_ZWRITEENABLE, depthWrite ? 1 : 0);
        renderer->SetRenderState(RS_ZENABLE, depthTest ? 1 : 0);
        renderer->SetRenderState(RS_ZFUNC, CMP_LESSEQUAL);
        renderer->SetRenderState(RS_CULLMODE, CULL_NONE);
    }
    ~XrWorldOverlayDraw() {
        renderer_->FlushPrimitive3D();
        renderer_->SetRenderState(RS_CULLMODE, cull_);
        renderer_->SetRenderState(RS_ZFUNC, zfunc_);
        renderer_->SetRenderState(RS_ZENABLE, zenable_);
        renderer_->SetRenderState(RS_ZWRITEENABLE, zwrite_);
    }
    XrWorldOverlayDraw(const XrWorldOverlayDraw&) = delete;
    XrWorldOverlayDraw& operator=(const XrWorldOverlayDraw&) = delete;
};
#endif
