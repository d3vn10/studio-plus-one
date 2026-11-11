#pragma once
// Ported from ReSkate (GPL-3.0) Launcher/gui_renderer.h, commit 3d259667d706. Studio+ adds resize()
// for its resizable window and reload_fonts() for a DPI change; logging goes to OutputDebugString.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <imgui.h>

#include <array>
#include <string>
#include <vector>

namespace studio::gui
{

    using Microsoft::WRL::ComPtr;

    constexpr UINT frame_count = 2;

    class Renderer
    {
      public:
        bool init(HWND window);
        void render();
        // Resizes the swapchain to the window's client area. Waits for the GPU first.
        void resize(UINT width, UINT height);
        // Rebuilds the font texture after the atlas changed (a DPI change).
        bool reload_fonts();
        // Uploads RGBA pixels into a free SRV slot (slot 0 is the font) and
        // returns its ImGui texture id; empty when the slots are used up.
        ImTextureID upload_texture(const std::vector<unsigned char> &pixels, UINT width, UINT height);
        // Frees a texture's slot once the GPU is done with it.
        void release_texture(ImTextureID id);
        void shutdown();
        // Which card draws the window, for the Home page.
        const std::wstring &adapter() const
        {
            return adapter_;
        }

      private:
        struct Frame
        {
            ComPtr<ID3D12CommandAllocator> allocator;
            UINT64 fence_value{};
        };
        struct Target
        {
            ComPtr<ID3D12Resource> resource;
            D3D12_CPU_DESCRIPTOR_HANDLE handle{};
        };
        ComPtr<ID3D12Device> device_;
        static constexpr UINT max_textures = 64;
        std::vector<ComPtr<ID3D12Resource>> textures_; // index + 1 = SRV slot; null when free
        ComPtr<ID3D12DescriptorHeap> rtv_heap_, srv_heap_;
        ComPtr<ID3D12CommandQueue> queue_;
        ComPtr<ID3D12GraphicsCommandList> list_;
        ComPtr<ID3D12Fence> fence_;
        ComPtr<IDXGISwapChain3> swap_;
        std::array<Frame, frame_count> frames_;
        std::array<Target, frame_count> targets_;
        HANDLE fence_event_{};
        HANDLE waitable_{};
        UINT64 fence_value_{};
        UINT64 frame_index_{};
        std::wstring adapter_;

        void wait(UINT64 value);
        void flush();
        bool create_targets();
    };

    // An embedded RCDATA resource; empty when it is missing.
    std::vector<unsigned char> resource_bytes(const wchar_t *name);

} // namespace studio::gui
