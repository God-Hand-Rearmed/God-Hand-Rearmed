// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "Config.h"
#include "GS/Renderers/Vulkan/GSTextureVK.h"

#include "common/WindowInfo.h"

#include "VKDxgiPresent.h" // pcsx2gs (Kamite)

#include <array>
#include <memory>
#include <optional>
#include <vector>

class VKSwapChain
{
public:
	// We don't actually need +1 semaphores, or, more than one really.
	// But, the validation layer gets cranky if we don't fence wait before the next image acquire.
	// So, add an additional semaphore to ensure that we're never acquiring before fence waiting.
	static constexpr u32 NUM_SEMAPHORES = 4; // Should be command buffers + 1

	~VKSwapChain();

	// Creates a vulkan-renderable surface for the specified window handle.
	static VkSurfaceKHR CreateVulkanSurface(VkInstance instance, VkPhysicalDevice physical_device, WindowInfo* wi);

	// Destroys a previously-created surface.
	static void DestroyVulkanSurface(VkInstance instance, WindowInfo* wi, VkSurfaceKHR surface);

	// Create a new swap chain from a pre-existing surface.
	static std::unique_ptr<VKSwapChain> Create(const WindowInfo& wi, VkSurfaceKHR surface, VkPresentModeKHR present_mode,
		std::optional<bool> exclusive_fullscreen_control);

	/// Returns the Vulkan present mode for a given vsync mode that is compatible with this device.
	static bool SelectPresentMode(VkSurfaceKHR surface, GSVSyncMode* vsync_mode, VkPresentModeKHR* present_mode);

	__fi VkSurfaceKHR GetSurface() const { return m_surface; }
	__fi VkSwapchainKHR GetSwapChain() const { return m_swap_chain; }
	__fi const VkSwapchainKHR* GetSwapChainPtr() const { return &m_swap_chain; }
	__fi const WindowInfo& GetWindowInfo() const { return m_window_info; }
	__fi u32 GetWidth() const { return m_window_info.surface_width; }
	__fi u32 GetHeight() const { return m_window_info.surface_height; }
	__fi float GetScale() const { return m_window_info.surface_scale; }
	__fi u32 GetCurrentImageIndex() const { return m_current_image; }
	__fi const u32* GetCurrentImageIndexPtr() const { return &m_current_image; }
	__fi u32 GetImageCount() const { return static_cast<u32>(m_images.size()); }
	__fi const GSTextureVK* GetCurrentTexture() const { return m_images[m_current_image].get(); }
	__fi GSTextureVK* GetCurrentTexture() { return m_images[m_current_image].get(); }
	__fi VkSemaphore GetImageAvailableSemaphore() const
	{
		return *GetImageAvailableSemaphorePtr();
	}
	// pcsx2gs (Kamite): presenting through DXGI, these are timeline semaphores (GetDxgiWaitValue / SignalValue).
	__fi const VkSemaphore* GetImageAvailableSemaphorePtr() const
	{
#ifdef _WIN32
		if (m_dxgi)
			return m_dxgi->CopyDonePtr();
#endif
		return &m_semaphores[m_current_semaphore].available_semaphore;
	}
	__fi VkSemaphore GetRenderingFinishedSemaphore() const
	{
		return *GetRenderingFinishedSemaphorePtr();
	}
	__fi const VkSemaphore* GetRenderingFinishedSemaphorePtr() const
	{
#ifdef _WIN32
		if (m_dxgi)
			return m_dxgi->RenderDonePtr();
#endif
		return &m_semaphores[m_current_semaphore].rendering_finished_semaphore;
	}

	// pcsx2gs (Kamite): the frames shown through a DXGI swap chain instead of a VkSwapchainKHR (VKDxgiPresent.h).
#ifdef _WIN32
	__fi bool IsDxgi() const { return m_dxgi != nullptr; }
	__fi u64 GetDxgiWaitValue() const { return m_dxgi->SlotFree(m_current_image); }
	__fi u64 GetDxgiSignalValue() const { return m_dxgi->NextFrame(); }
	void PresentDxgi();
#else
	__fi bool IsDxgi() const { return false; }
	__fi u64 GetDxgiWaitValue() const { return 0; }
	__fi u64 GetDxgiSignalValue() const { return 0; }
	void PresentDxgi() {}
#endif
	// The acquired image made ready to draw into (contents discarded) / handed over to be shown at the end.
	void BeginImage(VkCommandBuffer cmd, GSTextureVK::Layout layout);
	void EndImage(VkCommandBuffer cmd);

	VkFormat GetTextureFormat() const;
	VkResult AcquireNextImage();
	void ReleaseCurrentImage();
	void ResetImageAcquireResult();

	bool RecreateSurface(const WindowInfo& new_wi);
	bool ResizeSwapChain(u32 new_width = 0, u32 new_height = 0, float new_scale = 1.0f);

	// Change vsync enabled state. This may fail as it causes a swapchain recreation.
	bool SetPresentMode(VkPresentModeKHR present_mode);

private:
	VKSwapChain(const WindowInfo& wi, VkSurfaceKHR surface, VkPresentModeKHR present_mode,
		std::optional<bool> exclusive_fullscreen_control);

	static std::optional<VkSurfaceFormatKHR> SelectSurfaceFormat(VkSurfaceKHR surface);

	bool CreateSwapChain();
	bool CreateDxgiSwapChain(); // pcsx2gs (Kamite)
	bool AdoptDxgiImages(); // pcsx2gs (Kamite)
	void DestroySwapChain();
	void DestroySwapChainImages();

	void DestroySurface();

	struct ImageSemaphores
	{
		VkSemaphore available_semaphore;
		VkSemaphore rendering_finished_semaphore;
	};

	WindowInfo m_window_info;

	VkSurfaceKHR m_surface = VK_NULL_HANDLE;
	VkSwapchainKHR m_swap_chain = VK_NULL_HANDLE;

	std::vector<std::unique_ptr<GSTextureVK>> m_images;
	std::array<ImageSemaphores, NUM_SEMAPHORES> m_semaphores = {};

	u32 m_current_image = 0;
	u32 m_current_semaphore = 0;

	VkPresentModeKHR m_present_mode = VK_PRESENT_MODE_IMMEDIATE_KHR;

	std::optional<VkResult> m_image_acquire_result;
	std::optional<bool> m_exclusive_fullscreen_control;

#ifdef _WIN32
	std::unique_ptr<pcsx2gs::VKDxgiPresenter> m_dxgi; // pcsx2gs (Kamite)
#endif
};
