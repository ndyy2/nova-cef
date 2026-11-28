#include "view.hpp"
#include "system/logger.hpp"
#include <algorithm>

View::View() = default;

View::View(int id)
    : id_(id) {}

void View::Initialize(LPDIRECT3DDEVICE9 device)
{
    device_ = device;
}

void View::InternalCreateTexture(int width, int height)
{
    if (!device_) 
    {
        LOG_ERROR("[View] D3D device is not initialized");
        return;
    }

    wrapper_ = std::make_unique<ViewTexture>(device_, width, height, false);
    width_ = width;
    height_ = height;
}

void View::Create(int width, int height)
{
    InternalCreateTexture(std::max(1, width), std::max(1, height));
    active_ = true;
}

void View::Resize(int width, int height)
{
    if (!active_) 
        return;

    if (width == width_ && height == height_ && wrapper_) 
        return;

    InternalCreateTexture(std::max(1, width), std::max(1, height));
}

void View::UpdateTexture(const uint8_t* data, int bufferWidth, int bufferHeight, const cef_rect_t* dirtyRects, size_t count)
{
    if (!wrapper_ || !data || bufferWidth <= 0 || bufferHeight <= 0) 
        return;

    if (bufferWidth != width_ || bufferHeight != height_)
    {
        width_ = bufferWidth;
        height_ = bufferHeight;
    }

    wrapper_->UpdateDirtyRects(data, bufferWidth, bufferHeight, dirtyRects, count, false);
}

void View::OnPaint(const void* buffer, int width, int height)
{
    if (!wrapper_ || !buffer) 
        return;

    wrapper_->Update(buffer, width, height, false);
}

void View::Draw()
{
    if (!wrapper_ || !active_) 
        return;

    wrapper_->Draw(posX_, posY_);
}

void View::Clear()
{
    if (wrapper_)
        wrapper_->Clear();
}

cef_rect_t View::rect() const
{
    cef_rect_t rc{0,0, std::max(1, width_), std::max(1, height_)};
    return rc;
}

void View::OnDeviceLost()
{
    LOG_DEBUG("[View] OnDeviceLost for view {}", id_);

    if (wrapper_) 
        wrapper_->OnDeviceLost();
}

void View::OnDeviceReset(LPDIRECT3DDEVICE9 device)
{
    LOG_DEBUG("[View] OnDeviceReset for view {}", id_);

    device_ = device;
    if (wrapper_)
        wrapper_->OnDeviceReset(device);
}
