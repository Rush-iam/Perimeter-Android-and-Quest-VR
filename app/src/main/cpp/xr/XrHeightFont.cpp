#include "XrHeightFont.h"

#if defined(ANDROID_XR)
#include "StdAfxRD.h"
#include "Font.h"
#include "XrWorldQuad.h"
#include <array>
#include <limits>
#include <string>
#include "D3DRender.h"

namespace {
// Match the engine's text material: take color from the vertex and coverage
// from the font texture. Restore the state after submitting the glyphs.
class FontDrawState {
    cInterfaceRenderDevice* renderer_;
    uint32_t previous_ = 0;
    uint32_t samplerState_[3]{};
    uint32_t colorOp_ = 0;
    uint32_t colorArg_ = 0;
    bool d3d_ = false;
public:
    explicit FontDrawState(cInterfaceRenderDevice* renderer) : renderer_(renderer) {
        if (gb_RenderDevice3D) {
            d3d_ = true;
            samplerState_[0] = gb_RenderDevice3D->GetSamplerState(0, D3DSAMP_MINFILTER);
            samplerState_[1] = gb_RenderDevice3D->GetSamplerState(0, D3DSAMP_MAGFILTER);
            samplerState_[2] = gb_RenderDevice3D->GetSamplerState(0, D3DSAMP_MIPFILTER);
            colorOp_ = gb_RenderDevice3D->GetTextureStageState(0, D3DTSS_COLOROP);
            colorArg_ = gb_RenderDevice3D->GetTextureStageState(0, D3DTSS_COLORARG2);
            gb_RenderDevice3D->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
            gb_RenderDevice3D->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
            gb_RenderDevice3D->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            gb_RenderDevice3D->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            gb_RenderDevice3D->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            return;
        }
        previous_ = renderer->GetRenderState(RS_BILINEAR);
        renderer->SetRenderState(RS_BILINEAR, 1);
    }
    ~FontDrawState() {
        renderer_->FlushPrimitive3D();
        if (d3d_) {
            gb_RenderDevice3D->SetTextureStageState(0, D3DTSS_COLOROP, colorOp_);
            gb_RenderDevice3D->SetTextureStageState(0, D3DTSS_COLORARG2, colorArg_);
            gb_RenderDevice3D->SetSamplerState(0, D3DSAMP_MINFILTER, samplerState_[0]);
            gb_RenderDevice3D->SetSamplerState(0, D3DSAMP_MAGFILTER, samplerState_[1]);
            gb_RenderDevice3D->SetSamplerState(0, D3DSAMP_MIPFILTER, samplerState_[2]);
            return;
        }
        renderer_->SetRenderState(RS_BILINEAR, previous_);
    }
    FontDrawState(const FontDrawState&) = delete;
    FontDrawState& operator=(const FontDrawState&) = delete;
};
}

void xrDrawHeightReadout(cInterfaceRenderDevice* renderer, cFont* font,
                         int value, const Vect3f& anchor,
                         const Vect3f& right, const Vect3f& up,
                         float height, float offset, bool rightSide) {
    if (!font || !font->GetTexture() || font->GetHeight() <= 0.0f || height <= 0.0f) return;
    cTexture* texture = font->GetTexture();
    const auto* internal = font->GetInternal();
    const float scale = height / font->GetHeight();
    const float textureWidth = static_cast<float>(texture->GetWidth());
    // An int needs at most digits10 + 1 digits and a minus sign. Lay out once
    // on the stack, then reuse the geometry for every shadow layer.
    struct Glyph { Vect3f uv; float width; float offset; };
    std::array<Glyph, std::numeric_limits<int>::digits10 + 2> glyphs;
    size_t glyphCount = 0;
    float advance = 0.0f, textWidth = 0.0f;
    for (unsigned char ch : std::to_string(value)) {
        if (ch >= internal->Font.size()) continue;
        const Vect3f& uv = internal->Font[ch];
        const float width = uv.z * textureWidth * scale;
        glyphs[glyphCount++] = {uv, width, advance};
        textWidth = advance + width;
        advance += std::max(0.0f, width - scale);
    }
    if (!glyphCount) return;
    const Vect3f topLeft = anchor + right * (rightSide ? offset : -offset - textWidth)
        + up * (height * 0.5f);
    renderer->SetNoMaterial(ALPHA_BLEND, 0, texture);
    FontDrawState state(renderer);
    auto* buffer = renderer->GetDrawBuffer(sVertexXYZDT1::fmt, PT_TRIANGLES);
    // Nine shadow samples plus the white foreground, in alpha-blending order.
    auto* vertices = buffer->LockQuad<sVertexXYZDT1>(glyphCount * 10);
    const auto drawText = [&](const Vect3f& position, const sColor4c& color) {
        const uint32_t diffuse = renderer->ConvertColor(color);
        for (size_t i = 0; i < glyphCount; ++i) {
            const auto& glyph = glyphs[i];
            xrWriteWorldQuad(vertices, position + right * glyph.offset,
                             right * glyph.width, up * -height, diffuse,
                             glyph.uv.x, glyph.uv.y, glyph.uv.z, internal->FontHeight);
            vertices += 4;
        }
    };
    // A small weighted blur reuses the bundled glyphs without another texture.
    // Offset and softness scale with the text and follow its world billboard.
    const Vect3f shadow = topLeft + right * (height * 0.03f) - up * (height * 0.045f);
    const float spread = height * 0.025f;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            const int alpha = (x == 0 ? 2 : 1) * (y == 0 ? 2 : 1) * 16;
            drawText(shadow + right * (x * spread) + up * (y * spread),
                     sColor4c(0, 0, 0, alpha));
        }
    }
    drawText(topLeft, sColor4c(255, 255, 255, 255));
    buffer->Unlock();
}
#endif
