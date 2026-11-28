#pragma once

#include <cstddef>
#include <d3d9.h>
#include <game_sa/rw/rwcore.h>
#include "include/internal/cef_types.h"

// Bridges a D3D9 texture with an optional RenderWare texture so the pixel buffer
// can be used for OSR (CEF) overlays and (when requested) by the GTA:SA engine.
class ViewTexture
{
public:
    ViewTexture() = default;
    ~ViewTexture();

    ViewTexture(const ViewTexture&) = delete;
    ViewTexture& operator=(const ViewTexture&) = delete;
    ViewTexture(ViewTexture&&) = delete;
    ViewTexture& operator=(ViewTexture&&) = delete;

    ViewTexture(LPDIRECT3DDEVICE9 device, int width, int height, bool createRwTexture = false);

    bool Create(LPDIRECT3DDEVICE9 device, int width, int height, bool createRwTexture = false);
    void Update(const void* pixels, int width, int height, bool updateRwRaster = true);
    void UpdatePartial(const void* pixels, int bufferWidth, int bufferHeight, int x, int y, int width, int height, bool updateRwRaster = true);
    void UpdateDirtyRects(const void* pixels, int bufferWidth, int bufferHeight, const cef_rect_t* dirtyRects, size_t count, bool updateRwRaster = false);

    static void BeginDrawPass(LPDIRECT3DDEVICE9 device);
    void Draw(int x, int y);

    void Clear();

    LPDIRECT3DTEXTURE9 GetD3DTexture() const { return texture_; }
    RwTexture* GetRwTexture() { return rwTexture_; }

    void OnDeviceLost();
    void OnDeviceReset(LPDIRECT3DDEVICE9 device);

private:
    void ReleaseResources();

    LPDIRECT3DDEVICE9 device_ = nullptr;
    LPDIRECT3DTEXTURE9 texture_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    bool createRwTexture_ = false;

    RwTexture* rwTexture_ = nullptr;
    RwRaster* rwRaster_ = nullptr;

    bool isLost_ = false;
};
