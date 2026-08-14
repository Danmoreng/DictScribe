#include "platform/win/win_d3d_renderer.hpp"

#include <array>
#include <cstdint>
#include <sstream>

#include <d3d12.h>
#include <dcomp.h>
#include <dxgi1_6.h>

#pragma warning(push)
#pragma warning(disable: 4244 4267)
#include "include/core/SkColorSpace.h"
#include "include/core/SkColorType.h"
#include "include/core/SkSurface.h"
#include "include/gpu/ganesh/GrBackendSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/GrTypes.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/d3d/GrD3DBackendContext.h"
#include "include/gpu/ganesh/d3d/GrD3DBackendSurface.h"
#include "include/gpu/ganesh/d3d/GrD3DDirectContext.h"
#pragma warning(pop)

namespace dictscribe::win {

namespace {

constexpr UINT kBufferCount = 3;
constexpr DXGI_FORMAT kSwapChainFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
constexpr UINT kSwapChainFlags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

std::string HResultMessage(const char* operation, HRESULT result) {
    std::ostringstream message;
    message << operation << " failed with HRESULT 0x" << std::hex
            << static_cast<unsigned long>(result);
    return message.str();
}

template <typename Function>
bool CheckHResult(Function&& operation, const char* label, std::string& error) {
    const HRESULT result = operation();
    if (SUCCEEDED(result)) return true;
    error = HResultMessage(label, result);
    return false;
}

} // namespace

struct WinD3DRenderer::Impl {
    HWND window = nullptr;
    int width = 0;
    int height = 0;
    UINT buffer_index = 0;
    std::uint64_t next_fence_value = 1;
    HANDLE fence_event = nullptr;
    HANDLE frame_latency_waitable = nullptr;

    gr_cp<IDXGIFactory4> factory;
    gr_cp<IDXGIAdapter1> adapter;
    gr_cp<ID3D12Device> device;
    gr_cp<ID3D12CommandQueue> queue;
    sk_sp<GrDirectContext> context;
    gr_cp<IDXGISwapChain3> swap_chain;
    gr_cp<ID3D12Fence> fence;
    std::array<gr_cp<ID3D12Resource>, kBufferCount> buffers;
    std::array<sk_sp<SkSurface>, kBufferCount> surfaces;
    std::array<std::uint64_t, kBufferCount> buffer_fence_values{};

    gr_cp<IDCompositionDevice> composition_device;
    gr_cp<IDCompositionTarget> composition_target;
    gr_cp<IDCompositionVisual> composition_visual;

    bool choose_adapter(std::string& error) {
        for (UINT index = 0;; ++index) {
            gr_cp<IDXGIAdapter1> candidate;
            const HRESULT result = factory->EnumAdapters1(index, &candidate);
            if (result == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(result)) {
                error = HResultMessage("IDXGIFactory4::EnumAdapters1", result);
                return false;
            }
            DXGI_ADAPTER_DESC1 description{};
            if (FAILED(candidate->GetDesc1(&description)) ||
                (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
                continue;
            }
            if (SUCCEEDED(D3D12CreateDevice(
                    candidate.get(),
                    D3D_FEATURE_LEVEL_11_0,
                    __uuidof(ID3D12Device),
                    nullptr))) {
                adapter = std::move(candidate);
                return true;
            }
        }
        error = "No hardware adapter with Direct3D 12 support was found.";
        return false;
    }

    bool create_device(std::string& error) {
        if (!CheckHResult(
                [&] { return CreateDXGIFactory1(IID_PPV_ARGS(&factory)); },
                "CreateDXGIFactory1",
                error) ||
            !choose_adapter(error) ||
            !CheckHResult(
                [&] {
                    return D3D12CreateDevice(
                        adapter.get(),
                        D3D_FEATURE_LEVEL_11_0,
                        IID_PPV_ARGS(&device));
                },
                "D3D12CreateDevice",
                error)) {
            return false;
        }

        D3D12_COMMAND_QUEUE_DESC queue_description{};
        queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        queue_description.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        queue_description.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        if (!CheckHResult(
                [&] {
                    return device->CreateCommandQueue(
                        &queue_description, IID_PPV_ARGS(&queue));
                },
                "ID3D12Device::CreateCommandQueue",
                error)) {
            return false;
        }

        GrD3DBackendContext backend_context;
        backend_context.fAdapter = adapter;
        backend_context.fDevice = device;
        backend_context.fQueue = queue;
        context = GrDirectContexts::MakeD3D(backend_context);
        if (!context) {
            error = "Skia could not create a Ganesh Direct3D context.";
            return false;
        }

        if (!CheckHResult(
                [&] {
                    return device->CreateFence(
                        0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
                },
                "ID3D12Device::CreateFence",
                error)) {
            return false;
        }
        fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!fence_event) {
            error = "Could not create the Direct3D fence event.";
            return false;
        }
        return true;
    }

    bool create_swap_chain(std::string& error) {
        DXGI_SWAP_CHAIN_DESC1 description{};
        description.Width = static_cast<UINT>(width);
        description.Height = static_cast<UINT>(height);
        description.Format = kSwapChainFormat;
        description.Stereo = FALSE;
        description.SampleDesc.Count = 1;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = kBufferCount;
        description.Scaling = DXGI_SCALING_STRETCH;
        description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        description.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        description.Flags = kSwapChainFlags;

        gr_cp<IDXGISwapChain1> initial_swap_chain;
        if (!CheckHResult(
                [&] {
                    return factory->CreateSwapChainForComposition(
                        queue.get(), &description, nullptr, &initial_swap_chain);
                },
                "IDXGIFactory4::CreateSwapChainForComposition",
                error) ||
            !CheckHResult(
                [&] {
                    return initial_swap_chain->QueryInterface(IID_PPV_ARGS(&swap_chain));
                },
                "IDXGISwapChain1::QueryInterface",
                error)) {
            return false;
        }

        gr_cp<IDXGISwapChain2> swap_chain_2;
        if (SUCCEEDED(swap_chain->QueryInterface(IID_PPV_ARGS(&swap_chain_2)))) {
            swap_chain_2->SetMaximumFrameLatency(1);
            frame_latency_waitable = swap_chain_2->GetFrameLatencyWaitableObject();
        }

        if (!CheckHResult(
                [&] {
                    return DCompositionCreateDevice(
                        nullptr, IID_PPV_ARGS(&composition_device));
                },
                "DCompositionCreateDevice",
                error) ||
            !CheckHResult(
                [&] {
                    return composition_device->CreateTargetForHwnd(
                        window, TRUE, &composition_target);
                },
                "IDCompositionDevice::CreateTargetForHwnd",
                error) ||
            !CheckHResult(
                [&] { return composition_device->CreateVisual(&composition_visual); },
                "IDCompositionDevice::CreateVisual",
                error) ||
            !CheckHResult(
                [&] { return composition_visual->SetContent(swap_chain.get()); },
                "IDCompositionVisual::SetContent",
                error) ||
            !CheckHResult(
                [&] { return composition_target->SetRoot(composition_visual.get()); },
                "IDCompositionTarget::SetRoot",
                error) ||
            !CheckHResult(
                [&] { return composition_device->Commit(); },
                "IDCompositionDevice::Commit",
                error)) {
            return false;
        }
        return create_surfaces(error);
    }

    bool create_surfaces(std::string& error) {
        for (UINT index = 0; index < kBufferCount; ++index) {
            if (!CheckHResult(
                    [&] {
                        return swap_chain->GetBuffer(index, IID_PPV_ARGS(&buffers[index]));
                    },
                    "IDXGISwapChain3::GetBuffer",
                    error)) {
                return false;
            }
            GrD3DTextureResourceInfo resource_info(
                buffers[index].get(),
                nullptr,
                D3D12_RESOURCE_STATE_PRESENT,
                kSwapChainFormat,
                1,
                1,
                0);
            const GrBackendRenderTarget render_target =
                GrBackendRenderTargets::MakeD3D(width, height, resource_info);
            surfaces[index] = SkSurfaces::WrapBackendRenderTarget(
                context.get(),
                render_target,
                kTopLeft_GrSurfaceOrigin,
                kBGRA_8888_SkColorType,
                nullptr,
                nullptr);
            if (!surfaces[index]) {
                error = "Skia could not wrap a Direct3D swap-chain buffer.";
                return false;
            }
        }
        buffer_index = swap_chain->GetCurrentBackBufferIndex();
        return true;
    }

    bool wait_for_fence(std::uint64_t value) {
        if (value == 0 || fence->GetCompletedValue() >= value) return true;
        if (FAILED(fence->SetEventOnCompletion(value, fence_event))) return false;
        return WaitForSingleObject(fence_event, 2000) == WAIT_OBJECT_0;
    }

    bool wait_for_idle() {
        const std::uint64_t value = next_fence_value++;
        if (FAILED(queue->Signal(fence.get(), value))) return false;
        return wait_for_fence(value);
    }

    void release_surfaces() {
        for (auto& surface : surfaces) surface.reset();
        for (auto& buffer : buffers) buffer.reset();
    }

    bool resize(int new_width, int new_height, std::string& error) {
        if (new_width == width && new_height == height) return true;
        if (new_width <= 0 || new_height <= 0 || !wait_for_idle()) {
            error = "Direct3D could not synchronize before resizing.";
            return false;
        }
        context->flush();
        context->submit(GrSyncCpu::kYes);
        release_surfaces();
        width = new_width;
        height = new_height;
        buffer_fence_values.fill(0);
        if (!CheckHResult(
                [&] {
                    return swap_chain->ResizeBuffers(
                        kBufferCount,
                        static_cast<UINT>(width),
                        static_cast<UINT>(height),
                        kSwapChainFormat,
                        kSwapChainFlags);
                },
                "IDXGISwapChain3::ResizeBuffers",
                error)) {
            return false;
        }
        return create_surfaces(error);
    }

    void shutdown() {
        if (context && queue && fence) wait_for_idle();
        if (composition_target) composition_target->SetRoot(nullptr);
        if (composition_device) composition_device->Commit();
        release_surfaces();
        if (context) {
            context->flush();
            context->submit(GrSyncCpu::kYes);
            context->abandonContext();
        }
        composition_visual.reset();
        composition_target.reset();
        composition_device.reset();
        if (frame_latency_waitable) CloseHandle(frame_latency_waitable);
        frame_latency_waitable = nullptr;
        swap_chain.reset();
        context.reset();
        fence.reset();
        queue.reset();
        device.reset();
        adapter.reset();
        factory.reset();
        if (fence_event) CloseHandle(fence_event);
        fence_event = nullptr;
        width = 0;
        height = 0;
        window = nullptr;
    }
};

WinD3DRenderer::WinD3DRenderer() : impl_(std::make_unique<Impl>()) {}

WinD3DRenderer::~WinD3DRenderer() { shutdown(); }

bool WinD3DRenderer::initialize(
    HWND window,
    int width,
    int height,
    std::string& error) {
    shutdown();
    impl_->window = window;
    impl_->width = width;
    impl_->height = height;
    if (!impl_->create_device(error) || !impl_->create_swap_chain(error)) {
        shutdown();
        return false;
    }
    return true;
}

void WinD3DRenderer::shutdown() {
    if (impl_) impl_->shutdown();
}

bool WinD3DRenderer::valid() const {
    return impl_ && impl_->context && impl_->swap_chain;
}

SkSurface* WinD3DRenderer::begin_frame(int width, int height) {
    if (!valid()) return nullptr;
    std::string error;
    if (!impl_->resize(width, height, error)) return nullptr;
    if (impl_->frame_latency_waitable) {
        WaitForSingleObject(impl_->frame_latency_waitable, 16);
    }
    impl_->buffer_index = impl_->swap_chain->GetCurrentBackBufferIndex();
    if (!impl_->wait_for_fence(
            impl_->buffer_fence_values[impl_->buffer_index])) {
        return nullptr;
    }
    return impl_->surfaces[impl_->buffer_index].get();
}

bool WinD3DRenderer::present() {
    if (!valid() || !impl_->surfaces[impl_->buffer_index]) return false;
    GrFlushInfo flush_info;
    impl_->context->flush(
        impl_->surfaces[impl_->buffer_index].get(),
        SkSurfaces::BackendSurfaceAccess::kPresent,
        flush_info);
    if (!impl_->context->submit()) return false;
    const HRESULT present_result = impl_->swap_chain->Present(1, 0);
    if (FAILED(present_result)) return false;
    const std::uint64_t fence_value = impl_->next_fence_value++;
    if (FAILED(impl_->queue->Signal(impl_->fence.get(), fence_value))) return false;
    impl_->buffer_fence_values[impl_->buffer_index] = fence_value;
    return true;
}

} // namespace dictscribe::win
