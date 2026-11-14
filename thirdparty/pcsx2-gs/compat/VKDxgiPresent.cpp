// pcsx2gs (Kamite): Vulkan frames shown through a DXGI swap chain (VKDxgiPresent.h, claude/dxgi-present.md).
// References for the interop calls: krOoze/Hello_Triangle (dxgi_interop branch) and malstraem/vulkan-interop-directx,
// both public domain; the swap chain's settings follow PCSX2's GSDevice11 (the DX11 renderer that records fine).
#include "VKDxgiPresent.h"

#ifdef _WIN32

#include "GS/Renderers/Vulkan/GSDeviceVK.h"

#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>

namespace pcsx2gs
{
	bool g_vkDxgiPresent = true;

	namespace
	{
		using Microsoft::WRL::ComPtr;

		constexpr VkImageUsageFlags kUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		constexpr VkPipelineStageFlags kStages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
		IDXGISwapChain1* s_current = nullptr;

		std::string Hex(HRESULT hr)
		{
			char text[16];
			std::snprintf(text, sizeof(text), "0x%08X", static_cast<unsigned>(hr));
			return text;
		}

		std::string VkText(VkResult res) { return "VkResult " + std::to_string(static_cast<int>(res)); }

		void ClientSize(HWND hwnd, uint32_t* width, uint32_t* height)
		{
			RECT rect{};
			GetClientRect(hwnd, &rect);
			*width = static_cast<uint32_t>(std::max<LONG>(1, rect.right - rect.left));
			*height = static_cast<uint32_t>(std::max<LONG>(1, rect.bottom - rect.top));
		}
	} // namespace

	struct VKDxgiPresenter::Impl
	{
		HWND hwnd = nullptr;
		ComPtr<IDXGIFactory4> factory;
		ComPtr<ID3D11Device5> device;
		ComPtr<ID3D11DeviceContext4> context;
		ComPtr<ID3D11Fence> renderDone; // signalled by Vulkan when a frame is drawn
		ComPtr<ID3D11Fence> copyDone; // signalled here when it has been copied out
		ComPtr<IDXGISwapChain1> swapChain;
		std::array<ComPtr<ID3D11Texture2D>, kImages> textures;
		DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
		HANDLE event = nullptr;
		bool tearing = false;
	};

	VKDxgiPresenter::VKDxgiPresenter()
		: m_impl(std::make_unique<Impl>())
	{
	}

	std::unique_ptr<VKDxgiPresenter> VKDxgiPresenter::Create(void* hwnd, VkFormat format, uint32_t width, uint32_t height,
		int vsync, std::string* why)
	{
		std::string reason;
		const bool bgra = format == VK_FORMAT_B8G8R8A8_UNORM;
		format = bgra ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
		std::unique_ptr<VKDxgiPresenter> presenter = [&]() -> std::unique_ptr<VKDxgiPresenter> {
			GSDeviceVK* const vk = GSDeviceVK::GetInstance();
			const GSDeviceVK::OptionalExtensions& ext = vk->GetOptionalExtensions();
			if (!ext.vk_khr_external_memory_win32 || !ext.vk_khr_external_semaphore_win32 || !ext.vk_khr_timeline_semaphore)
			{
				reason = "the Vulkan driver lacks VK_KHR_external_memory_win32, VK_KHR_external_semaphore_win32 or "
						 "timeline semaphores";
				return nullptr;
			}
			if (!vkGetMemoryWin32HandlePropertiesKHR || !vkImportSemaphoreWin32HandleKHR ||
				!vkGetPhysicalDeviceImageFormatProperties2 || !vkGetPhysicalDeviceExternalSemaphoreProperties)
			{
				reason = "the Vulkan driver lacks the interop entry points";
				return nullptr;
			}
			const VkPhysicalDevice physical = vk->GetPhysicalDevice();
			const VkDevice device = vk->GetDevice();

			// Direct3D 11 textures and fences must be importable.
			const VkPhysicalDeviceExternalImageFormatInfo external_image = {
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO, nullptr,
				VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT};
			const VkPhysicalDeviceImageFormatInfo2 image_info = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
				&external_image, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, kUsage, 0};
			VkExternalImageFormatProperties external_props = {VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
			VkImageFormatProperties2 image_props = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &external_props};
			VkResult res = vkGetPhysicalDeviceImageFormatProperties2(physical, &image_info, &image_props);
			if (res != VK_SUCCESS || !(external_props.externalMemoryProperties.externalMemoryFeatures &
										 VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT))
			{
				reason = "the Vulkan driver cannot import Direct3D 11 textures (" + VkText(res) + ")";
				return nullptr;
			}
			const VkSemaphoreTypeCreateInfo timeline = {
				VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, nullptr, VK_SEMAPHORE_TYPE_TIMELINE, 0};
			const VkPhysicalDeviceExternalSemaphoreInfo semaphore_info = {
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO, &timeline,
				VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT};
			VkExternalSemaphoreProperties semaphore_props = {VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
			vkGetPhysicalDeviceExternalSemaphoreProperties(physical, &semaphore_info, &semaphore_props);
			if (!(semaphore_props.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT))
			{
				reason = "the Vulkan driver cannot import Direct3D fences";
				return nullptr;
			}

			// The D3D11 device on the same GPU (the adapter's LUID).
			VkPhysicalDeviceIDProperties id_props = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
			VkPhysicalDeviceProperties2 props2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id_props};
			vkGetPhysicalDeviceProperties2(physical, &props2);
			if (!id_props.deviceLUIDValid)
			{
				reason = "the Vulkan device has no adapter LUID";
				return nullptr;
			}
			LUID luid;
			static_assert(sizeof(luid) == VK_LUID_SIZE);
			std::memcpy(&luid, id_props.deviceLUID, sizeof(luid));

			std::unique_ptr<VKDxgiPresenter> p(new VKDxgiPresenter());
			Impl& d = *p->m_impl;
			d.hwnd = static_cast<HWND>(hwnd);
			p->m_graphicsFamily = vk->GetGraphicsQueueFamilyIndex();
			p->m_vsync = vsync;
			p->m_format = format;
			d.format = bgra ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
			HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(d.factory.GetAddressOf()));
			if (FAILED(hr))
			{
				reason = "CreateDXGIFactory2 failed (" + Hex(hr) + ")";
				return nullptr;
			}
			ComPtr<IDXGIAdapter1> adapter;
			hr = d.factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(adapter.GetAddressOf()));
			if (FAILED(hr))
			{
				reason = "no DXGI adapter matches the Vulkan device (" + Hex(hr) + ")";
				return nullptr;
			}
			static constexpr D3D_FEATURE_LEVEL kLevels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
			ComPtr<ID3D11Device> device11;
			ComPtr<ID3D11DeviceContext> context11;
			hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, kLevels,
				static_cast<UINT>(std::size(kLevels)), D3D11_SDK_VERSION, device11.GetAddressOf(), nullptr,
				context11.GetAddressOf());
			if (FAILED(hr))
			{
				reason = "D3D11CreateDevice failed (" + Hex(hr) + ")";
				return nullptr;
			}
			if (FAILED(device11.As(&d.device)) || FAILED(context11.As(&d.context)))
			{
				reason = "Direct3D 11.4 is missing (Windows 10 1703 or newer)";
				return nullptr;
			}
			ComPtr<IDXGIFactory5> factory5;
			BOOL tearing = FALSE;
			if (SUCCEEDED(d.factory.As(&factory5)) &&
				FAILED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing))))
				tearing = FALSE;
			d.tearing = tearing != FALSE;
			d.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			if (!d.event)
			{
				reason = "CreateEvent failed";
				return nullptr;
			}

			// The two shared fences, seen by Vulkan as timeline semaphores.
			const auto make_fence = [&](ComPtr<ID3D11Fence>& fence, VkSemaphore& semaphore, const char* name) {
				HRESULT r = d.device->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(fence.GetAddressOf()));
				HANDLE handle = nullptr;
				if (SUCCEEDED(r))
					r = fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &handle);
				if (FAILED(r))
				{
					reason = std::string("the ") + name + " fence could not be shared (" + Hex(r) + ")";
					return false;
				}
				const VkSemaphoreCreateInfo create = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &timeline, 0};
				VkResult vr = vkCreateSemaphore(device, &create, nullptr, &semaphore);
				if (vr == VK_SUCCESS)
				{
					const VkImportSemaphoreWin32HandleInfoKHR import = {
						VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR, nullptr, semaphore, 0,
						VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT, handle, nullptr};
					vr = vkImportSemaphoreWin32HandleKHR(device, &import);
				}
				CloseHandle(handle); // importing an NT handle does not take it over
				if (vr != VK_SUCCESS)
				{
					reason = std::string("Vulkan could not import the ") + name + " fence (" + VkText(vr) + ")";
					return false;
				}
				return true;
			};
			if (!make_fence(d.renderDone, p->m_renderDoneVk, "frame") || !make_fence(d.copyDone, p->m_copyDoneVk, "copy"))
				return nullptr;

			if (width == 0 || height == 0)
				ClientSize(d.hwnd, &width, &height);
			p->m_width = width;
			p->m_height = height;
			if (!p->CreateImages(&reason))
				return nullptr;

			// Last, so nothing is attached to the window unless everything else worked (the native swap chain
			// can still take it then).
			DXGI_SWAP_CHAIN_DESC1 desc = {};
			desc.Width = width;
			desc.Height = height;
			desc.Format = d.format;
			desc.SampleDesc.Count = 1;
			desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
			desc.BufferCount = BufferCount(vsync);
			desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
			desc.Flags = d.tearing ? static_cast<UINT>(DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) : 0u;
			hr = d.factory->CreateSwapChainForHwnd(d.device.Get(), d.hwnd, &desc, nullptr, nullptr, d.swapChain.GetAddressOf());
			if (FAILED(hr))
			{
				reason = "CreateSwapChainForHwnd failed (" + Hex(hr) + ")";
				return nullptr;
			}
			// The port's window handles Alt+Enter and its own size (as GSDevice11).
			d.factory->MakeWindowAssociation(d.hwnd, DXGI_MWA_NO_WINDOW_CHANGES);
			s_current = d.swapChain.Get();
			return p;
		}();
		if (!presenter && why)
			*why = reason;
		return presenter;
	}

	VKDxgiPresenter::~VKDxgiPresenter()
	{
		Impl& d = *m_impl;
		if (d.swapChain && d.swapChain.Get() == s_current)
			s_current = nullptr;
		WaitIdle();
		DestroyImages();
		const VkDevice device = GSDeviceVK::GetInstance()->GetDevice();
		if (m_renderDoneVk != VK_NULL_HANDLE)
			vkDestroySemaphore(device, m_renderDoneVk, nullptr);
		if (m_copyDoneVk != VK_NULL_HANDLE)
			vkDestroySemaphore(device, m_copyDoneVk, nullptr);
		if (d.context)
		{
			d.context->ClearState();
			d.context->Flush();
		}
		if (d.event)
			CloseHandle(d.event);
	}

	bool VKDxgiPresenter::CreateImages(std::string* why)
	{
		Impl& d = *m_impl;
		GSDeviceVK* const vk = GSDeviceVK::GetInstance();
		const VkDevice device = vk->GetDevice();
		VkPhysicalDeviceMemoryProperties memory_props;
		vkGetPhysicalDeviceMemoryProperties(vk->GetPhysicalDevice(), &memory_props);
		for (uint32_t i = 0; i < kImages; i++)
		{
			D3D11_TEXTURE2D_DESC desc = {};
			desc.Width = m_width;
			desc.Height = m_height;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = d.format;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
			desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
			HRESULT hr = d.device->CreateTexture2D(&desc, nullptr, d.textures[i].GetAddressOf());
			ComPtr<IDXGIResource1> resource;
			HANDLE handle = nullptr;
			if (SUCCEEDED(hr))
				hr = d.textures[i].As(&resource);
			if (SUCCEEDED(hr))
				hr = resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle);
			if (FAILED(hr))
			{
				*why = "a shared texture could not be made (" + Hex(hr) + ")";
				return false;
			}

			Slot& slot = m_slots[i];
			slot.copied = 0;
			const VkExternalMemoryImageCreateInfo external = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, nullptr,
				VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT};
			const VkImageCreateInfo image_info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &external, 0, VK_IMAGE_TYPE_2D,
				m_format, {m_width, m_height, 1u}, 1u, 1u, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, kUsage,
				VK_SHARING_MODE_EXCLUSIVE, 0u, nullptr, VK_IMAGE_LAYOUT_UNDEFINED};
			VkResult res = vkCreateImage(device, &image_info, nullptr, &slot.image);
			uint32_t type = UINT32_MAX;
			VkMemoryRequirements req = {};
			if (res == VK_SUCCESS)
			{
				vkGetImageMemoryRequirements(device, slot.image, &req);
				uint32_t bits = req.memoryTypeBits;
				VkMemoryWin32HandlePropertiesKHR handle_props = {VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
				if (vkGetMemoryWin32HandlePropertiesKHR(device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT, handle,
						&handle_props) == VK_SUCCESS &&
					(bits & handle_props.memoryTypeBits) != 0)
					bits &= handle_props.memoryTypeBits;
				for (uint32_t t = 0; t < memory_props.memoryTypeCount; t++)
				{
					if (!(bits & (1u << t)))
						continue;
					if (type == UINT32_MAX)
						type = t;
					if (memory_props.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
					{
						type = t;
						break;
					}
				}
				if (type == UINT32_MAX)
					res = VK_ERROR_FEATURE_NOT_PRESENT;
			}
			if (res == VK_SUCCESS)
			{
				const VkMemoryDedicatedAllocateInfo dedicated = {
					VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, nullptr, slot.image, VK_NULL_HANDLE};
				const VkImportMemoryWin32HandleInfoKHR import = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR,
					&dedicated, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT, handle, nullptr};
				const VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &import, req.size, type};
				res = vkAllocateMemory(device, &alloc, nullptr, &slot.memory);
			}
			if (res == VK_SUCCESS)
				res = vkBindImageMemory(device, slot.image, slot.memory, 0);
			CloseHandle(handle); // importing an NT handle does not take it over
			if (res != VK_SUCCESS)
			{
				*why = "Vulkan could not import a shared texture (" + VkText(res) + ")";
				return false;
			}
		}
		return true;
	}

	void VKDxgiPresenter::DestroyImages()
	{
		const VkDevice device = GSDeviceVK::GetInstance()->GetDevice();
		for (Slot& slot : m_slots)
		{
			if (slot.image != VK_NULL_HANDLE)
				vkDestroyImage(device, slot.image, nullptr);
			if (slot.memory != VK_NULL_HANDLE)
				vkFreeMemory(device, slot.memory, nullptr);
			slot = {};
		}
		for (ComPtr<ID3D11Texture2D>& texture : m_impl->textures)
			texture.Reset();
	}

	// The D3D11 device done with every copy (Vulkan's side is the caller's: WaitForGPUIdle).
	void VKDxgiPresenter::WaitIdle()
	{
		Impl& d = *m_impl;
		if (!d.context || !d.copyDone || !d.event)
			return;
		d.context->Flush();
		if (d.copyDone->GetCompletedValue() < m_frame && SUCCEEDED(d.copyDone->SetEventOnCompletion(m_frame, d.event)))
			WaitForSingleObject(d.event, 2000); // bounded: a lost device never signals
	}

	bool VKDxgiPresenter::Resize(uint32_t width, uint32_t height, int vsync, std::string* why)
	{
		Impl& d = *m_impl;
		WaitIdle();
		DestroyImages();
		if (width == 0 || height == 0)
			ClientSize(d.hwnd, &width, &height);
		m_width = width;
		m_height = height;
		m_vsync = vsync;
		d.context->ClearState();
		d.context->Flush();
		const HRESULT hr = d.swapChain->ResizeBuffers(
			BufferCount(vsync), width, height, DXGI_FORMAT_UNKNOWN,
			d.tearing ? static_cast<UINT>(DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) : 0u);
		if (FAILED(hr))
		{
			*why = "ResizeBuffers failed (" + Hex(hr) + ")";
			m_failed = true;
			return false;
		}
		if (!CreateImages(why))
		{
			m_failed = true;
			return false;
		}
		return true;
	}

	std::string VKDxgiPresenter::Describe() const
	{
		char text[160];
		std::snprintf(text, sizeof(text), "Direct3D 11 flip model, %s %ux%u, %u buffers, tearing %s",
			m_format == VK_FORMAT_B8G8R8A8_UNORM ? "BGRA8" : "RGBA8", m_width, m_height, BufferCount(m_vsync),
			m_impl->tearing ? "allowed with vsync off" : "unsupported");
		return text;
	}

	void VKDxgiPresenter::RecordAcquire(VkCommandBuffer cmd, uint32_t i, VkImageLayout new_layout) const
	{
		const VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, 0,
			VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
			VK_IMAGE_LAYOUT_UNDEFINED, new_layout, VK_QUEUE_FAMILY_EXTERNAL, m_graphicsFamily, m_slots[i].image,
			{VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u}};
		// Source stages = the stages the submit's wait on CopyDone holds back, so the transition comes after it.
		vkCmdPipelineBarrier(cmd, kStages, kStages, 0, 0, nullptr, 0, nullptr, 1, &barrier);
	}

	void VKDxgiPresenter::RecordRelease(VkCommandBuffer cmd, uint32_t i, VkImageLayout old_layout) const
	{
		const VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr,
			VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, 0, old_layout, VK_IMAGE_LAYOUT_GENERAL,
			m_graphicsFamily, VK_QUEUE_FAMILY_EXTERNAL, m_slots[i].image, {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u}};
		vkCmdPipelineBarrier(cmd, kStages, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
	}

	bool VKDxgiPresenter::Present(uint32_t i)
	{
		Impl& d = *m_impl;
		if (m_failed)
			return false;
		m_frame++;
		d.context->Wait(d.renderDone.Get(), m_frame);
		ComPtr<ID3D11Texture2D> back;
		HRESULT hr = d.swapChain->GetBuffer(0, IID_PPV_ARGS(back.GetAddressOf()));
		if (SUCCEEDED(hr))
			d.context->CopyResource(back.Get(), d.textures[i].Get());
		// Signalled even without a copy, so Vulkan never waits for one that will not come.
		d.context->Signal(d.copyDone.Get(), m_frame);
		m_slots[i].copied = m_frame;
		back.Reset();
		if (SUCCEEDED(hr))
		{
			const UINT sync = m_vsync == 1 ? 1u : 0u;
			const UINT flags = (m_vsync == 0 && d.tearing) ? DXGI_PRESENT_ALLOW_TEARING : 0u;
			hr = d.swapChain->Present(sync, flags);
		}
		if (FAILED(hr))
		{
			std::fprintf(stderr, "[pcsx2-gs] DXGI present failed (%s): presenting the native Vulkan way from now on\n",
				Hex(hr).c_str());
			m_failed = true;
			return false;
		}
		return true;
	}

	IDXGISwapChain1* VKDxgiPresenter::CurrentSwapChain()
	{
		return s_current;
	}
} // namespace pcsx2gs

#endif // _WIN32
