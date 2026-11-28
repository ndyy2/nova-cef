#include "view_texture.hpp"
#include "system/logger.hpp"
#include <game_sa/RenderWare.h>
#include <algorithm>
#include <cstdint>
#include <cstring>

ViewTexture::ViewTexture(LPDIRECT3DDEVICE9 device, int width, int height, bool createRwTexture)
{
    Create(device, width, height, createRwTexture);
}

ViewTexture::~ViewTexture()
{
    ReleaseResources();
}

void ViewTexture::ReleaseResources()
{
    if (texture_) 
    { 
        texture_->Release(); 
        texture_ = nullptr; 
    }

    if (rwTexture_) 
    { 
        RwTextureDestroy(rwTexture_); 
        rwTexture_ = nullptr; 
        rwRaster_ = nullptr; 
    }
}

bool ViewTexture::Create(LPDIRECT3DDEVICE9 device, int width, int height, bool createRwTexture)
{
    ReleaseResources();

    device_ = device;
    width_ = width;
    height_ = height;
    createRwTexture_ = createRwTexture;
    isLost_ = false;
    LOG_DEBUG("[ViewTexture] Creating texture {}x{} (rwTexture={})", width, height, createRwTexture);

    HRESULT hr = device_->CreateTexture(width, height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture_, nullptr);
    if (FAILED(hr)) 
    { 
        LOG_ERROR("[ViewTexture] Failed to create D3D9 texture ({}x{})", width, height); 
        return false; 
    }

    if (!createRwTexture_)
        return true;

    rwRaster_ = RwRasterCreate(width, height, 32, rwRASTERTYPETEXTURE | rwRASTERFORMAT8888);
    if (!rwRaster_) 
    { 
        LOG_ERROR("[ViewTexture] Failed to create RwRaster"); 
        ReleaseResources(); 
        return false; 
    }

    rwTexture_ = RwTextureCreate(rwRaster_);
    if (!rwTexture_) 
    { 
        LOG_ERROR("[ViewTexture] Failed to create RwTexture from RwRaster"); 
        RwRasterDestroy(rwRaster_); 
        rwRaster_ = nullptr; 
        ReleaseResources(); 
        return false; 
    }

    strncpy_s(rwTexture_->name, "CEF_BROWSER", sizeof(rwTexture_->name));
    return true;
}

void ViewTexture::Clear()
{
    if (texture_)
    {
        D3DLOCKED_RECT rect;
        if (SUCCEEDED(texture_->LockRect(0, &rect, nullptr, 0)))
        {
            std::memset(rect.pBits, 0, static_cast<size_t>(rect.Pitch) * height_);
            texture_->UnlockRect(0);
        }
    }

    if (rwRaster_)
    {
        uint8_t* rasterPixels = RwRasterLock(rwRaster_, 0, rwRASTERLOCKWRITE);
        if (rasterPixels)
        {
            std::memset(rasterPixels, 0, static_cast<size_t>(rwRaster_->stride) * height_);
            RwRasterUnlock(rwRaster_);
        }
    }
}

void ViewTexture::Update(const void* pixels, int width, int height, bool updateRwRaster)
{
    if (!pixels || width <= 0 || height <= 0)
        return;

    const bool needRw = createRwTexture_;
    if (!texture_ || (needRw && !rwRaster_) || width != width_ || height != height_) 
    {
        LOG_WARN("[ViewTexture] Size mismatch or invalid state, recreating (old={}x{}, new={}x{})", width_, height_, width, height); 
        Create(device_, width, height, createRwTexture_);

        if (!texture_ || (needRw && !rwRaster_)) 
            return;
    }

    D3DLOCKED_RECT rect;
    if (SUCCEEDED(texture_->LockRect(0, &rect, nullptr, 0)))
    {
        const auto* src = static_cast<const uint8_t*>(pixels);
        auto* dst = static_cast<uint8_t*>(rect.pBits);
        const size_t rowBytes = static_cast<size_t>(width) * 4;

        if (rect.Pitch == static_cast<INT>(rowBytes)) 
        { 
            memcpy(dst, src, rowBytes * static_cast<size_t>(height)); 
        }
        else 
        {
            for (int y = 0; y < height; ++y) 
            {
                memcpy(dst + static_cast<size_t>(y) * rect.Pitch, src + static_cast<size_t>(y) * rowBytes, rowBytes);
            }
        }

        texture_->UnlockRect(0);
    }

    if (!updateRwRaster || !rwRaster_)
        return;

    uint8_t* rasterPixels = RwRasterLock(rwRaster_, 0, rwRASTERLOCKWRITE);
    if (rasterPixels) 
    {
        const auto* src = static_cast<const uint8_t*>(pixels);
        const int rwPitch = rwRaster_->stride;
        const int srcPitch = width * 4;

        if (rwPitch == srcPitch) 
        { 
            memcpy(rasterPixels, src, static_cast<size_t>(height) * srcPitch); 
        }
        else 
        {
            for (int y = 0; y < height; ++y) 
            {
                memcpy(rasterPixels + static_cast<size_t>(y) * rwPitch, src + static_cast<size_t>(y) * srcPitch, srcPitch);
            }
        }

        RwRasterUnlock(rwRaster_);
    }
}

void ViewTexture::UpdatePartial(const void* pixels, int bufferWidth, int bufferHeight, int x, int y, int width, int height, bool updateRwRaster)
{
    if (!texture_ || !pixels || width <= 0 || height <= 0) 
        return;

    if (bufferWidth != width_ || bufferHeight != height_)
    {
        Update(pixels, bufferWidth, bufferHeight, updateRwRaster);
        return;
    }

    const int left = std::max(0, x);
    const int top = std::max(0, y);
    const int right = std::min(width_, x + width);
    const int bottom = std::min(height_, y + height);
    if (left >= right || top >= bottom)
        return;

    const int clampedW = right - left;
    const int clampedH = bottom - top;

    RECT lockRect = { static_cast<LONG>(left), static_cast<LONG>(top), static_cast<LONG>(right), static_cast<LONG>(bottom) };
    D3DLOCKED_RECT rect;
    if (SUCCEEDED(texture_->LockRect(0, &rect, &lockRect, 0)))
    {
        const auto* src = static_cast<const uint8_t*>(pixels);
        auto* dst = static_cast<uint8_t*>(rect.pBits);
        const size_t copyBytes = static_cast<size_t>(clampedW) * 4;

        for (int row = 0; row < clampedH; ++row) 
        {
            const uint8_t* srcRow = src + (static_cast<size_t>(top + row) * static_cast<size_t>(bufferWidth) + static_cast<size_t>(left)) * 4;
            uint8_t* dstRow = dst + (static_cast<size_t>(row) * static_cast<size_t>(rect.Pitch));
            memcpy(dstRow, srcRow, copyBytes);
        }

        texture_->UnlockRect(0);
    }

    if (!updateRwRaster || !rwRaster_)
        return;

    uint8_t* rasterPixels = RwRasterLock(rwRaster_, 0, rwRASTERLOCKWRITE);
    if (rasterPixels) 
    {
        const int rwPitch = rwRaster_->stride;
        const auto* src = static_cast<const uint8_t*>(pixels);
        const size_t copyBytes = static_cast<size_t>(clampedW) * 4;

        for (int row = 0; row < clampedH; ++row) 
        {
            const uint8_t* srcRow = src + (static_cast<size_t>(top + row) * static_cast<size_t>(bufferWidth) + static_cast<size_t>(left)) * 4;
            uint8_t* dstRow = rasterPixels + (static_cast<size_t>(top + row) * static_cast<size_t>(rwPitch) + static_cast<size_t>(left) * 4);
            memcpy(dstRow, srcRow, copyBytes);
        }

        RwRasterUnlock(rwRaster_);
    }
}

void ViewTexture::UpdateDirtyRects(const void* pixels, int bufferWidth, int bufferHeight, const cef_rect_t* dirtyRects, size_t count, bool updateRwRaster)
{
    if (!pixels || bufferWidth <= 0 || bufferHeight <= 0)
        return;

    if (!texture_ || bufferWidth != width_ || bufferHeight != height_ || !dirtyRects || count == 0)
    {
        Update(pixels, bufferWidth, bufferHeight, updateRwRaster);
        return;
    }

    // Compute bounding box and total dirty pixel area to avoid excessive LockRect/UnlockRect
    // round-trips (especially costly on Wine/DXVK and D3D9 drivers).
    int minX = width_;
    int minY = height_;
    int maxX = 0;
    int maxY = 0;
    size_t validCount = 0;
    uint64_t dirtyArea = 0;

    for (size_t i = 0; i < count; ++i)
    {
        const int left = std::max(0, dirtyRects[i].x);
        const int top = std::max(0, dirtyRects[i].y);
        const int right = std::min(width_, dirtyRects[i].x + dirtyRects[i].width);
        const int bottom = std::min(height_, dirtyRects[i].y + dirtyRects[i].height);
        if (left >= right || top >= bottom)
            continue;

        minX = std::min(minX, left);
        minY = std::min(minY, top);
        maxX = std::max(maxX, right);
        maxY = std::max(maxY, bottom);
        dirtyArea += static_cast<uint64_t>(right - left) * static_cast<uint64_t>(bottom - top);
        ++validCount;
    }

    if (validCount == 0)
        return;

    const uint64_t fullArea = static_cast<uint64_t>(width_) * static_cast<uint64_t>(height_);
    if (dirtyArea * 2 >= fullArea)
    {
        Update(pixels, bufferWidth, bufferHeight, updateRwRaster);
        return;
    }

    if (validCount > 4)
    {
        UpdatePartial(pixels, bufferWidth, bufferHeight, minX, minY, maxX - minX, maxY - minY, updateRwRaster);
        return;
    }

    for (size_t i = 0; i < count; ++i)
    {
        const auto& r = dirtyRects[i];
        if (r.width <= 0 || r.height <= 0)
            continue;

        UpdatePartial(pixels, bufferWidth, bufferHeight, r.x, r.y, r.width, r.height, updateRwRaster);
    }
}

void ViewTexture::BeginDrawPass(LPDIRECT3DDEVICE9 device)
{
    if (!device)
        return;

    // Configure fixed-function state once per render pass for all browser quads.
    device->SetVertexShader(nullptr);
    device->SetPixelShader(nullptr);
    device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);

    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_FOGENABLE, FALSE);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    device->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED |
        D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
    device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_WRAP0, 0);

    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_RESULTARG, D3DTA_CURRENT);
    device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);

    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);

    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
}

void ViewTexture::Draw(int x, int y)
{
    if (!device_ || !texture_) 
        return;

    device_->SetTexture(0, texture_);

    const float w = static_cast<float>(width_);
    const float h = static_cast<float>(height_);

    struct Vertex { float x, y, z, rhw; DWORD color; float u, v; };
    const Vertex quad[] = {
        { static_cast<float>(x) - 0.5f,       static_cast<float>(y) - 0.5f,       0.0f, 1.0f, 0xFFFFFFFF, 0.0f, 0.0f },
        { static_cast<float>(x) + w - 0.5f,   static_cast<float>(y) - 0.5f,       0.0f, 1.0f, 0xFFFFFFFF, 1.0f, 0.0f },
        { static_cast<float>(x) - 0.5f,       static_cast<float>(y) + h - 0.5f,   0.0f, 1.0f, 0xFFFFFFFF, 0.0f, 1.0f },
        { static_cast<float>(x) + w - 0.5f,   static_cast<float>(y) + h - 0.5f,   0.0f, 1.0f, 0xFFFFFFFF, 1.0f, 1.0f },
    };

    device_->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vertex));
}

void ViewTexture::OnDeviceLost()
{
    LOG_DEBUG("[ViewTexture] Device lost, releasing D3D9 texture only");
    
    // Release ONLY the D3D9 texture (D3DPOOL_MANAGED resource)
    if (texture_) {
        texture_->Release();
        texture_ = nullptr;
    }
    
    isLost_ = true;
}

void ViewTexture::OnDeviceReset(LPDIRECT3DDEVICE9 device)
{
    if (!isLost_) 
    {
        LOG_DEBUG("[ViewTexture] OnDeviceReset called but device wasn't lost, skipping");
        return;
    }
    
    LOG_DEBUG("[ViewTexture] Device reset, recreating D3D9 texture {}x{}", width_, height_);
    
    device_ = device;
    
    // Recreate the D3D9 texture
    HRESULT hr = device_->CreateTexture(width_, height_, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture_, nullptr);
    if (FAILED(hr)) 
    {
        LOG_ERROR("[ViewTexture] Failed to recreate D3D9 texture after reset: HRESULT=0x{:08X}", static_cast<unsigned>(hr));
        return;
    }
    
    // Re-sync pixel data from RenderWare raster to D3D9 texture (if backed by RwRaster)
    if (rwRaster_) 
    {
        uint8_t* rasterPixels = RwRasterLock(rwRaster_, 0, rwRASTERLOCKREAD);
        if (rasterPixels) 
        {
            D3DLOCKED_RECT rect;
            if (SUCCEEDED(texture_->LockRect(0, &rect, nullptr, 0))) 
            {
                const int rwPitch = rwRaster_->stride;
                const int srcPitch = width_ * 4;
                auto* dst = static_cast<uint8_t*>(rect.pBits);
                
                if (rect.Pitch == rwPitch) 
                {
                    memcpy(dst, rasterPixels, static_cast<size_t>(height_) * srcPitch);
                } 
                else 
                {
                    for (int y = 0; y < height_; ++y) 
                    {
                        memcpy(dst + static_cast<size_t>(y) * rect.Pitch, rasterPixels + static_cast<size_t>(y) * rwPitch, srcPitch);
                    }
                }

                texture_->UnlockRect(0);
                LOG_DEBUG("[ViewTexture] Successfully restored pixel data from RwRaster");
            }
            
            RwRasterUnlock(rwRaster_);
        } 
        else 
        {
            LOG_WARN("[ViewTexture] Could not lock RwRaster to restore pixel data");
        }
    }
    
    isLost_ = false;
    LOG_DEBUG("[ViewTexture] D3D9 texture recreation complete");
}
