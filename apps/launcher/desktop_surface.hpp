#pragma once

#include <Windows.h>
#include <windowsx.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <utility>

namespace cabbird::launcher {

// The launcher draws its own caption and rounded body. There is no native
// caption or redirected HWND bitmap underneath the composition visual.
inline constexpr DWORD kDesktopWindowStyle =
    WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
inline constexpr DWORD kDesktopWindowExtendedStyle =
    WS_EX_APPWINDOW | WS_EX_NOREDIRECTIONBITMAP;
inline constexpr float kDesktopClearColor[4]{0.0f, 0.0f, 0.0f, 0.0f};

inline DXGI_SWAP_CHAIN_DESC1 DesktopSwapChainDescription(
    const UINT width, const UINT height) noexcept {
    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = width;
    description.Height = height;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 2;
    description.Scaling = DXGI_SCALING_STRETCH;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    description.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    return description;
}

// Keep the visual tree alive beside Graphics, while the existing render-target,
// ResizeBuffers and retained-draw-data paths continue to own GPU submission.
class DesktopSurface final {
public:
    DesktopSurface() = default;
    DesktopSurface(const DesktopSurface&) = delete;
    DesktopSurface& operator=(const DesktopSurface&) = delete;
    DesktopSurface(DesktopSurface&& other) noexcept { *this = std::move(other); }
    DesktopSurface& operator=(DesktopSurface&& other) noexcept {
        if (this != &other) {
            Reset();
            device_ = std::move(other.device_);
            target_ = std::move(other.target_);
            visual_ = std::move(other.visual_);
        }
        return *this;
    }
    ~DesktopSurface() { Reset(); }

    HRESULT Create(HWND window, ID3D11Device* device, const UINT width, const UINT height,
        Microsoft::WRL::ComPtr<IDXGISwapChain1>& swap_chain) noexcept {
        if (window == nullptr || device == nullptr || width == 0 || height == 0) return E_INVALIDARG;
        // One top-level visual target may own this HWND at a time. Release
        // the previous device generation before binding a replacement.
        Reset();
        using Microsoft::WRL::ComPtr;
        ComPtr<IDXGIDevice> dxgi_device;
        HRESULT result = device->QueryInterface(IID_PPV_ARGS(&dxgi_device));
        if (FAILED(result)) return result;
        ComPtr<IDXGIAdapter> adapter;
        result = dxgi_device->GetAdapter(&adapter);
        if (FAILED(result)) return result;
        ComPtr<IDXGIFactory2> factory;
        result = adapter->GetParent(IID_PPV_ARGS(&factory));
        if (FAILED(result)) return result;
        const auto description = DesktopSwapChainDescription(width, height);
        ComPtr<IDXGISwapChain1> created;
        result = factory->CreateSwapChainForComposition(device, &description, nullptr, &created);
        if (FAILED(result)) return result;
        DesktopSurface surface;
        result = DCompositionCreateDevice(dxgi_device.Get(), IID_PPV_ARGS(&surface.device_));
        if (FAILED(result)) return result;
        result = surface.device_->CreateTargetForHwnd(window, TRUE, &surface.target_);
        if (FAILED(result)) return result;
        result = surface.device_->CreateVisual(&surface.visual_);
        if (FAILED(result)) return result;
        result = surface.visual_->SetContent(created.Get());
        if (FAILED(result)) return result;
        result = surface.target_->SetRoot(surface.visual_.Get());
        if (FAILED(result)) return result;
        result = surface.device_->Commit();
        if (FAILED(result)) return result;
        *this = std::move(surface);
        swap_chain = std::move(created);
        return S_OK;
    }

    void Reset() noexcept {
        if (target_ != nullptr) static_cast<void>(target_->SetRoot(nullptr));
        if (device_ != nullptr) static_cast<void>(device_->Commit());
        visual_.Reset();
        target_.Reset();
        device_.Reset();
    }

private:
    Microsoft::WRL::ComPtr<IDCompositionDevice> device_;
    Microsoft::WRL::ComPtr<IDCompositionTarget> target_;
    Microsoft::WRL::ComPtr<IDCompositionVisual> visual_;
};

// The DPI-scaled perimeter retains the OS's eight native resize directions.
// Caption dragging stays in the ImGui input flow: entering Windows' modal
// HTCAPTION loop can consume the mouse-up needed by the launcher's buttons.
inline LRESULT HitTestClient(const POINT point, const SIZE size, const UINT dpi,
    const bool maximized, const bool client_control = false) noexcept {
    if (point.x < 0 || point.y < 0 || point.x >= size.cx || point.y >= size.cy) return HTNOWHERE;
    if (client_control) return HTCLIENT;
    if (!maximized) {
        const LONG border = MulDiv(8, static_cast<int>(dpi == 0 ? 96 : dpi), 96);
        const bool left = point.x < border;
        const bool right = point.x >= size.cx - border;
        const bool top = point.y < border;
        const bool bottom = point.y >= size.cy - border;
        if (top && left) return HTTOPLEFT;
        if (top && right) return HTTOPRIGHT;
        if (bottom && left) return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (top) return HTTOP;
        if (bottom) return HTBOTTOM;
    }
    return HTCLIENT;
}

inline LRESULT FrameHitTest(HWND window, const LPARAM position,
    const bool client_control = false) noexcept {
    POINT point{GET_X_LPARAM(position), GET_Y_LPARAM(position)};
    RECT client{};
    if (!ScreenToClient(window, &point) || !GetClientRect(window, &client)) return HTCLIENT;
    return HitTestClient(point, {client.right - client.left, client.bottom - client.top},
        GetDpiForWindow(window), IsZoomed(window) != FALSE, client_control);
}

inline void ConstrainMaximizedToWorkArea(HWND window, MINMAXINFO& limits) noexcept {
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) return;
    limits.ptMaxPosition = {monitor.rcWork.left - monitor.rcMonitor.left,
        monitor.rcWork.top - monitor.rcMonitor.top};
    limits.ptMaxSize = {monitor.rcWork.right - monitor.rcWork.left,
        monitor.rcWork.bottom - monitor.rcWork.top};
}

} // namespace cabbird::launcher
