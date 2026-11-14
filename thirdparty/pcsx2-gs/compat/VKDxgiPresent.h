// pcsx2gs (Kamite): Vulkan frames shown through a DXGI swap chain on Windows, so overlays and recorders that hook
// DXGI (the NVIDIA overlay, Game Bar, OBS game capture) see the game, and presentation gets the flip model.
//
// PCSX2's Vulkan device still draws everything (the game, ImGui, the port's overlay) into what it takes for swap chain
// images: here, Direct3D 11 textures shared with Vulkan. At each present a small D3D11 device on the same GPU waits for
// Vulkan's frame (a shared fence that Vulkan sees as a timeline semaphore), copies it into a flip-model swap chain's
// back buffer and presents. Vulkan waits for the copy (a second shared fence) before drawing into that texture again.
// No CPU waits. VKSwapChain (PCSX2) owns one of these instead of a VkSwapchainKHR when it starts; when it cannot, the
// game presents the native Vulkan way. claude/dxgi-present.md.
#pragma once

#ifdef _WIN32

#include "GS/Renderers/Vulkan/VKLoader.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>

struct IDXGISwapChain1;

namespace pcsx2gs
{
	// The bridge's dxgi_present option (PS2X_PCSX2_DXGI_PRESENT=0 turns it off): on by default.
	extern bool g_vkDxgiPresent;

	class VKDxgiPresenter
	{
	public:
		static constexpr uint32_t kImages = 3; // as many as PCSX2 has frames in flight

		// format: VK_FORMAT_B8G8R8A8_UNORM or (anything else) VK_FORMAT_R8G8B8A8_UNORM, the same in DXGI; the native
		// swap chain's format, so the present pipelines still fit if it takes over mid-run.
		// vsync as GSVSyncMode: 0 = no wait (tearing when the system allows it), 1 = FIFO, 2 = mailbox.
		// width / height 0: the window's client size. nullptr and *why filled when anything is missing or fails;
		// nothing is attached to the window then (the swap chain is made last).
		static std::unique_ptr<VKDxgiPresenter> Create(void* hwnd, VkFormat format, uint32_t width, uint32_t height,
			int vsync, std::string* why);
		~VKDxgiPresenter();

		VKDxgiPresenter(const VKDxgiPresenter&) = delete;
		VKDxgiPresenter& operator=(const VKDxgiPresenter&) = delete;

		// New size and / or vsync (its buffer count): the images are made again. The caller has made sure Vulkan
		// is idle and dropped everything that refers to the old images.
		bool Resize(uint32_t width, uint32_t height, int vsync, std::string* why);
		void SetVSync(int vsync) { m_vsync = vsync; }
		bool NeedsNewBuffers(int vsync) const { return BufferCount(vsync) != BufferCount(m_vsync); }

		VkFormat Format() const { return m_format; }
		uint32_t Width() const { return m_width; }
		uint32_t Height() const { return m_height; }
		VkImage Image(uint32_t i) const { return m_slots[i].image; }
		bool Failed() const { return m_failed; }
		std::string Describe() const; // for the log

		// Vulkan's side of a present. Before drawing into image i: wait for CopyDone() >= SlotFree(i). The submit
		// that finishes the frame signals RenderDone() = NextFrame(); Present() then shows it.
		const VkSemaphore* CopyDonePtr() const { return &m_copyDoneVk; }
		const VkSemaphore* RenderDonePtr() const { return &m_renderDoneVk; }
		VkSemaphore RenderDone() const { return m_renderDoneVk; }
		uint64_t SlotFree(uint32_t i) const { return m_slots[i].copied; }
		uint64_t NextFrame() const { return m_frame + 1; }

		// Queue family ownership round the shared images (the D3D11 device is VK_QUEUE_FAMILY_EXTERNAL): taken back
		// at the start of a frame (contents discarded, into new_layout), handed over at its end (old_layout -> GENERAL).
		void RecordAcquire(VkCommandBuffer cmd, uint32_t i, VkImageLayout new_layout) const;
		void RecordRelease(VkCommandBuffer cmd, uint32_t i, VkImageLayout old_layout) const;

		// After the submit that signals NextFrame(): the D3D11 device waits for it, copies image i and presents.
		// false = the D3D11 device is gone (Failed() from then on).
		bool Present(uint32_t i);

		// The live presenter's swap chain (PS2X_PRESENT_STATS reads its frame statistics), or nullptr.
		static IDXGISwapChain1* CurrentSwapChain();

	private:
		struct Impl;
		struct Slot
		{
			VkImage image = VK_NULL_HANDLE;
			VkDeviceMemory memory = VK_NULL_HANDLE;
			uint64_t copied = 0; // the CopyDone value of the last copy out of it
		};

		VKDxgiPresenter();
		static uint32_t BufferCount(int vsync) { return vsync == 2 ? 3u : 2u; } // as GSDevice11
		bool CreateImages(std::string* why);
		void DestroyImages();
		void WaitIdle();

		std::unique_ptr<Impl> m_impl; // the D3D11 / DXGI objects
		std::array<Slot, kImages> m_slots = {};
		VkSemaphore m_renderDoneVk = VK_NULL_HANDLE;
		VkSemaphore m_copyDoneVk = VK_NULL_HANDLE;
		uint64_t m_frame = 0;
		VkFormat m_format = VK_FORMAT_R8G8B8A8_UNORM;
		uint32_t m_width = 0;
		uint32_t m_height = 0;
		uint32_t m_graphicsFamily = 0;
		int m_vsync = 2;
		bool m_failed = false;
	};
} // namespace pcsx2gs

#endif // _WIN32
