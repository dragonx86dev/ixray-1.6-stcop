#include "Device.h"

#include <SDL3/SDL_vulkan.h>

#include "State.h"
#include "Utils.h"
#include "VkStubs.h"

inline void VkCheck(VkResult result, const char* what)
{
	if (result != VK_SUCCESS)
	{
		string_path buf;
		xr_sprintf(buf, "%s failed, VkResult = %d", what, static_cast<int>(result));
		Msg("! Vulkan: %s", buf);
		xrLogger::FlushLog();
		R_ASSERT2(result == VK_SUCCESS, buf);
	}
}

// ---- Stub helpers ----
static void VkStubNoopRelease(void*) {}

static void DeleteRhiBlendDesc(void* p)
{
	xr_delete(static_cast<RHIBlendDesc*>(p));
}

// Постоянные stub-объекты (живут всю жизнь движка)
static VkStubTextureFactory g_VkStubTextureFactory;

InternalDeviceVK::ContextLock::ContextLock(const InternalDeviceVK& device)
	: _device(&device)
{
	device._contextMutex.Enter();
}

InternalDeviceVK::ContextLock::~ContextLock()
{
	_device->_contextMutex.Leave();
}


InternalDeviceVK::InternalDeviceVK()
{
	if (!Utils::CheckVulkanVersion())
	{
		Msg("! [Vulkan] Vulkan 1.3 is required but not supported by the loader.");
		return;
	}

	CreateVulkanInstance();
	CreateVulkanSurface();
	PickPhysicalDevice();
	CreateLogicalDevice();

	CreatePipelineCache();
	CreateSwapchain();
	CreateSwapchainImageViews();
	CreateFrameResources();

	// --- IRHIDevice defaults ---
	FeatureLevel = RHI_FEATURE_LEVEL_11_1;
	VertexCache = 32;
	NoOverwriteSRV = true;

	Msg("* [Vulkan] Vulkan device initialized");
}

void InternalDeviceVK::CreateVulkanInstance()
{
	PROF_EVENT("Vulkan: CreateInstance");

	VkApplicationInfo appInfo = {};
	appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	appInfo.pApplicationName = "IX-Ray Engine";
	appInfo.applicationVersion = VK_MAKE_API_VERSION(0, 1, 4, 0);
	appInfo.apiVersion = VK_API_VERSION_1_4;

	u32 extensionsCount = 0;
	const auto sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&extensionsCount);
	R_ASSERT2(sdlExtensions && extensionsCount > 0, "SDL_Vulkan_GetInstanceExtensions failed");

	const xr_vector<const char*> extensions(sdlExtensions, sdlExtensions + extensionsCount);

	VkInstanceCreateInfo createInfo = {};
	createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	createInfo.pApplicationInfo = &appInfo;
	createInfo.enabledExtensionCount = static_cast<u32>(extensions.size());
	createInfo.ppEnabledExtensionNames = extensions.data();

#if defined(IXR_APPLE_SERIES)
	createInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif

	VkCheck(vkCreateInstance(&createInfo, nullptr, &_vkInstance), "vkCreateInstance");
}

void InternalDeviceVK::CreateVulkanSurface()
{
	R_ASSERT2(g_AppInfo.Window != nullptr, "[Vulkan] surface requires an SDL window");

	if (!SDL_Vulkan_CreateSurface(g_AppInfo.Window, _vkInstance, nullptr, &_vkSurface))
	{
		Msg("! Vulkan: SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
		xrLogger::FlushLog();
		Msg("[Vulkan] Failed to create Vulkan surface.");
	}
}

void InternalDeviceVK::PickPhysicalDevice()
{
	u32 deviceCount = 0;
	vkEnumeratePhysicalDevices(_vkInstance, &deviceCount, nullptr);
	R_ASSERT2(deviceCount > 0, "[Vulkan] No Vulkan physical devices found");
	xr_vector<VkPhysicalDevice> devices(deviceCount);
	VkCheck(vkEnumeratePhysicalDevices(_vkInstance, &deviceCount, devices.data()),
	        "vkEnumeratePhysicalDevices");

	VkPhysicalDevice best = VK_NULL_HANDLE;
	LocalQueueFamilyIndices bestFamilies;
	int bestScore = -1;

	for (VkPhysicalDevice device : devices)
	{
		VkPhysicalDeviceProperties props{};
		vkGetPhysicalDeviceProperties(device, &props);

		if (props.apiVersion < VK_API_VERSION_1_4)
		{
			Msg("~ Vulkan device skipped (API too low): %s", props.deviceName);
			continue;
		}

		LocalQueueFamilyIndices families;
		if (!QueryQueueFamilies(device, families))
		{
			Msg("~ Vulkan device skipped (no graphics/present queue): %s", props.deviceName);
			continue;
		}

		int score = 0;
		switch (props.deviceType)
		{
			case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: score += 1000;
				break;
			case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: score += 500;
				break;
			case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: score += 300;
				break;
			case VK_PHYSICAL_DEVICE_TYPE_CPU: score += 100;
				break;
			default: score += 25;
				break;
		}

		if (score > bestScore)
		{
			bestScore = score;
			best = device;
			bestFamilies = families;
		}
	}

	R_ASSERT2(best != VK_NULL_HANDLE, "No Vulkan physical device supports graphics+present");
	_vkPhysicalDevice = best;
	_queueFamilies = bestFamilies;
}

bool InternalDeviceVK::QueryQueueFamilies(VkPhysicalDevice device, LocalQueueFamilyIndices& out) const
{
	out = {};

	u32 count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
	xr_vector<VkQueueFamilyProperties> families(count);
	vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());

	for (u32 i = 0; i < count; ++i)
	{
		if (out.Graphics == UINT32_MAX && families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
		{
			out.Graphics = i;
		}

		VkBool32 present = VK_FALSE;
		vkGetPhysicalDeviceSurfaceSupportKHR(device, i, _vkSurface, &present);
		if (out.Present == UINT32_MAX && present)
		{
			out.Present = i;
		}

		if (out.IsComplete())
			return true;
	}
	return false;
}

void InternalDeviceVK::CreateLogicalDevice()
{
	PROF_EVENT("Vulkan: CreateLogicalDevice");

	xr_vector<u32> uniqueFamilies;
	uniqueFamilies.push_back(_queueFamilies.Graphics);
	if (_queueFamilies.Present != _queueFamilies.Graphics)
	{
		uniqueFamilies.push_back(_queueFamilies.Present);
	}

	constexpr float kQueuePriority = 1.0f;
	xr_vector<VkDeviceQueueCreateInfo> queueInfos;
	queueInfos.reserve(uniqueFamilies.size());
	for (u32 family : uniqueFamilies)
	{
		VkDeviceQueueCreateInfo qi{};
		qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		qi.queueFamilyIndex = family;
		qi.queueCount = 1;
		qi.pQueuePriorities = &kQueuePriority;
		queueInfos.push_back(qi);
	}

	VkPhysicalDeviceVulkan14Features want14{};
	want14.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES;
	want14.pushDescriptor = VK_TRUE;
	want14.dynamicRenderingLocalRead = VK_TRUE;
	want14.maintenance5 = VK_TRUE;
	want14.maintenance6 = VK_TRUE;
	want14.hostImageCopy = VK_TRUE;
	want14.indexTypeUint8 = VK_TRUE;
	want14.pipelineProtectedAccess = VK_TRUE;

	VkPhysicalDeviceVulkan13Features want13{};
	want13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
	want13.pNext = &want14;
	want13.synchronization2 = VK_TRUE;
	want13.dynamicRendering = VK_TRUE;
	want13.maintenance4 = VK_TRUE;

	VkPhysicalDeviceVulkan12Features want12{};
	want12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
	want12.pNext = &want13;
	want12.timelineSemaphore = VK_TRUE;
	want12.bufferDeviceAddress = VK_TRUE;
	want12.scalarBlockLayout = VK_TRUE;
	want12.separateDepthStencilLayouts = VK_TRUE;
	want12.drawIndirectCount = VK_TRUE;

	VkPhysicalDeviceVulkan11Features want11{};
	want11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
	want11.pNext = &want12;
	want11.shaderDrawParameters = VK_TRUE;

	VkPhysicalDeviceFeatures2 wantCore{};
	wantCore.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	wantCore.pNext = &want11;
	wantCore.features.samplerAnisotropy = VK_TRUE;
	wantCore.features.fillModeNonSolid = VK_TRUE;
	wantCore.features.depthBounds = VK_TRUE;
	wantCore.features.independentBlend = VK_TRUE;
	wantCore.features.textureCompressionBC = VK_TRUE;
	wantCore.features.shaderClipDistance = VK_TRUE;
	wantCore.features.shaderCullDistance = VK_TRUE;
	wantCore.features.imageCubeArray = VK_TRUE;
	wantCore.features.fragmentStoresAndAtomics = VK_TRUE;
	wantCore.features.vertexPipelineStoresAndAtomics = VK_TRUE;

	vkGetPhysicalDeviceFeatures2(_vkPhysicalDevice, &wantCore);

	R_ASSERT2(want13.synchronization2 == VK_TRUE, "synchronization2 required");
	R_ASSERT2(want13.dynamicRendering == VK_TRUE, "dynamicRendering required");
	R_ASSERT2(want14.pushDescriptor == VK_TRUE, "pushDescriptor required");
	R_ASSERT2(want12.scalarBlockLayout == VK_TRUE, "scalarBlockLayout required");
	R_ASSERT2(want12.separateDepthStencilLayouts == VK_TRUE, "separateDepthStencilLayouts required");

	static constexpr const char* kDeviceExtensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

	VkDeviceCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	info.pNext = &wantCore;
	info.queueCreateInfoCount = static_cast<u32>(queueInfos.size());
	info.pQueueCreateInfos = queueInfos.data();
	info.enabledExtensionCount = static_cast<u32>(std::size(kDeviceExtensions));
	info.ppEnabledExtensionNames = kDeviceExtensions;
	info.pEnabledFeatures = nullptr;

	VkCheck(vkCreateDevice(_vkPhysicalDevice, &info, nullptr, &_vkDevice), "vkCreateDevice");

	vkGetDeviceQueue(_vkDevice, _queueFamilies.Graphics, 0, &_graphicsVkQueue);
	vkGetDeviceQueue(_vkDevice, _queueFamilies.Present, 0, &_presentVkQueue);
}

void InternalDeviceVK::CreatePipelineCache()
{
	PROF_EVENT("Vulkan: CreatePipelineCache");

	VkPipelineCacheCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
	info.flags = VK_PIPELINE_CACHE_CREATE_EXTERNALLY_SYNCHRONIZED_BIT;

	const VkResult r = vkCreatePipelineCache(_vkDevice, &info, nullptr, &_vkPipelineCache);
	VkCheck(r, "vkCreatePipelineCache");
}

void InternalDeviceVK::CreateSwapchain()
{
	PROF_EVENT("Vulkan: CreateSwapchain");

	VkSurfaceCapabilitiesKHR caps{};
	VkCheck(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(_vkPhysicalDevice, _vkSurface, &caps),
	        "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");

	u32 formatCount = 0;
	VkCheck(vkGetPhysicalDeviceSurfaceFormatsKHR(_vkPhysicalDevice, _vkSurface, &formatCount, nullptr),
	        "vkGetPhysicalDeviceSurfaceFormatsKHR (count)");
	xr_vector<VkSurfaceFormatKHR> formats(formatCount);
	VkCheck(vkGetPhysicalDeviceSurfaceFormatsKHR(_vkPhysicalDevice, _vkSurface, &formatCount, formats.data()),
	        "vkGetPhysicalDeviceSurfaceFormatsKHR");

	VkSurfaceFormatKHR chosenFormat = formats[0];
	for (const auto& fmt : formats)
	{
		if ((fmt.format == VK_FORMAT_B8G8R8A8_SRGB || fmt.format == VK_FORMAT_B8G8R8A8_UNORM) &&
		    fmt.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
		{
			chosenFormat = fmt;
			break;
		}
	}
	_swapchainFormat = chosenFormat.format;
	_swapchainColorSpace = chosenFormat.colorSpace;

	u32 presentCount = 0;
	VkCheck(vkGetPhysicalDeviceSurfacePresentModesKHR(_vkPhysicalDevice, _vkSurface, &presentCount, nullptr),
	        "vkGetPhysicalDeviceSurfacePresentModesKHR (count)");
	xr_vector<VkPresentModeKHR> presentModes(presentCount);
	VkCheck(vkGetPhysicalDeviceSurfacePresentModesKHR(_vkPhysicalDevice, _vkSurface, &presentCount, presentModes.data()),
	        "vkGetPhysicalDeviceSurfacePresentModesKHR");

	VkPresentModeKHR chosenPresent = VK_PRESENT_MODE_FIFO_KHR;
	if (!psDeviceFlags.test(rsVSync))
	{
		for (VkPresentModeKHR pm : presentModes)
		{
			if (pm == VK_PRESENT_MODE_MAILBOX_KHR)
			{
				chosenPresent = pm;
				break;
			}
		}
	}

	if (caps.currentExtent.width != UINT32_MAX && caps.currentExtent.height != UINT32_MAX)
	{
		_swapchainExtent = caps.currentExtent;
	}
	else
	{
		_swapchainExtent = {
			std::clamp<u32>(psCurrentVidMode[0], caps.minImageExtent.width, caps.maxImageExtent.width),
			std::clamp<u32>(psCurrentVidMode[1], caps.minImageExtent.height, caps.maxImageExtent.height)
		};
	}
	psCurrentVidMode[0] = _swapchainExtent.width;
	psCurrentVidMode[1] = _swapchainExtent.height;
	HalfTarget.x = (int)_swapchainExtent.width;
	HalfTarget.y = (int)_swapchainExtent.height;

	u32 imageCount = caps.minImageCount + 1;
	if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount)
	{
		imageCount = caps.maxImageCount;
	}

	const VkCompositeAlphaFlagBitsKHR composite =
		(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
			? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
			: VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;

	VkSwapchainCreateInfoKHR info{};
	info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	info.surface = _vkSurface;
	info.minImageCount = imageCount;
	info.imageFormat = chosenFormat.format;
	info.imageColorSpace = chosenFormat.colorSpace;
	info.imageExtent = _swapchainExtent;
	info.imageArrayLayers = 1;
	info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	info.preTransform = caps.currentTransform;
	info.compositeAlpha = composite;
	info.presentMode = chosenPresent;
	info.clipped = VK_TRUE;
	info.oldSwapchain = _vkSwapchain;

	const u32 families[2] = { _queueFamilies.Graphics, _queueFamilies.Present };
	if (_queueFamilies.Graphics != _queueFamilies.Present)
	{
		info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
		info.queueFamilyIndexCount = 2;
		info.pQueueFamilyIndices = families;
	}
	else
	{
		info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	}

	const VkResult r = vkCreateSwapchainKHR(_vkDevice, &info, nullptr, &_vkSwapchain);
	VkCheck(r, "vkCreateSwapchainKHR");

	if (info.oldSwapchain != VK_NULL_HANDLE)
	{
		vkDestroySwapchainKHR(_vkDevice, info.oldSwapchain, nullptr);
	}

	u32 swapImageCount = 0;
	VkCheck(vkGetSwapchainImagesKHR(_vkDevice, _vkSwapchain, &swapImageCount, nullptr),
	        "vkGetSwapchainImagesKHR (count)");
	_swapchainImages.resize(swapImageCount);
	VkCheck(vkGetSwapchainImagesKHR(_vkDevice, _vkSwapchain, &swapImageCount, _swapchainImages.data()),
	        "vkGetSwapchainImagesKHR");

	Msg("* Vulkan swapchain: %ux%u, %u images, format %d, present %d",
	    _swapchainExtent.width,
	    _swapchainExtent.height,
	    swapImageCount,
	    static_cast<int>(chosenFormat.format),
	    static_cast<int>(chosenPresent));
	
	RHITextureDesc depthDesc;
	depthDesc.Width           = _swapchainExtent.width;
	depthDesc.Height          = _swapchainExtent.height;
	depthDesc.Format          = ERHI_FORMAT::D24_UNORM_S8_UINT;
	depthDesc.BindFlags       = ERHI_BIND_FLAG::DEPTH_STENCIL;
	depthDesc.Usage           = ERHI_USAGE::USAGE_DEFAULT;
	depthDesc.SampleDescCount = 1;

	auto* depthSurface = new VkStubSurface(depthDesc);
	RenderDSV = new VkStubDSV(depthSurface);

	RHITextureDesc colorDesc;
	colorDesc.Width           = _swapchainExtent.width;
	colorDesc.Height          = _swapchainExtent.height;
	colorDesc.Format          = ERHI_FORMAT::R8G8B8A8_UNORM;
	colorDesc.BindFlags       = ERHI_BIND_FLAG::RENDER_TARGET | ERHI_BIND_FLAG::SHADER_RESOURCE;
	colorDesc.Usage           = ERHI_USAGE::USAGE_DEFAULT;
	colorDesc.SampleDescCount = 1;

	auto* colorSurface = new VkStubSurface(colorDesc);
	RenderRTV       = new VkStubRTV(colorSurface);
	RenderTexture   = RenderRTV;
	RenderSRV       = new VkStubSRV();
	SwapChainRTV    = new VkStubRTV(new VkStubSurface(colorDesc));
	
	Msg("* [VK] RenderDSV=%p RenderRTV=%p SwapChainRTV=%p",
	(void*)RenderDSV, (void*)RenderRTV, (void*)SwapChainRTV);

}

void InternalDeviceVK::CreateSwapchainImageViews()
{
	PROF_EVENT("Vulkan: CreateSwapchainImageViews");

	_swapchainImageViews.resize(_swapchainImages.size());

	for (size_t i = 0; i < _swapchainImages.size(); ++i)
	{
		VkImageViewCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		info.image = _swapchainImages[i];
		info.viewType = VK_IMAGE_VIEW_TYPE_2D;
		info.format = _swapchainFormat;
		info.components = {
			VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
			VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY
		};
		info.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

		const VkResult r = vkCreateImageView(_vkDevice, &info, nullptr, &_swapchainImageViews[i]);
		VkCheck(r, "vkCreateImageView (swapchain)");
	}
}

void InternalDeviceVK::CreateFrameResources()
{
	PROF_EVENT("Vulkan: CreateFrameResources");

	VkCommandPoolCreateInfo poolInfo{};
	poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	poolInfo.queueFamilyIndex = _queueFamilies.Graphics;
	poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT |
	                 VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;

	VkCommandBufferAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandBufferCount = 1;

	VkFenceCreateInfo fenceInfo{};
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

	VkSemaphoreCreateInfo semInfo{};
	semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

	for (u32 i = 0; i < FrameCount; ++i)
	{
		Frame& f = _frames[i];

		VkResult r = vkCreateCommandPool(_vkDevice, &poolInfo, nullptr, &f.CommandPool);
		VkCheck(r, "vkCreateCommandPool");

		allocInfo.commandPool = f.CommandPool;
		r = vkAllocateCommandBuffers(_vkDevice, &allocInfo, &f.CommandBuffer);
		VkCheck(r, "vkAllocateCommandBuffers");

		r = vkCreateFence(_vkDevice, &fenceInfo, nullptr, &f.InFlight);
		VkCheck(r, "vkCreateFence");

		r = vkCreateSemaphore(_vkDevice, &semInfo, nullptr, &f.ImageReady);
		VkCheck(r, "vkCreateSemaphore (ImageReady)");

		r = vkCreateSemaphore(_vkDevice, &semInfo, nullptr, &f.RenderDone);
		VkCheck(r, "vkCreateSemaphore (RenderDone)");

		f.Serial = 0;
	}
}

// =====================================================================
// Все draw / state / bind - no-op заглушки.
// Никаких Msg, чтобы не засорять лог в render-loop.
// =====================================================================

void InternalDeviceVK::SetPrimitiveTopology(ERHI_PRIMITIVE_TOPOLOGY) {}
void InternalDeviceVK::Draw(u32, u32) {}
void InternalDeviceVK::DrawIndexed(u32, u32, u32) {}
void InternalDeviceVK::DrawInstanced(u32, u32, u32, u32) {}
void InternalDeviceVK::DrawIndexedInstanced(u32, u32, u32, u32, u32) {}
void InternalDeviceVK::ClearTarget(void*, ERTColor) {}
void InternalDeviceVK::ClearTarget(void*, const float*) {}
void InternalDeviceVK::ClearDepthStencil(IRHIDepthStencilView*, ERHI_CLEAR_TARGET, float, u8) {}
void InternalDeviceVK::GenerateMips(IRHIShaderResourceView*) {}
void InternalDeviceVK::CopySurface(IRHISurface*, IRHISurface*) {}
void InternalDeviceVK::CopySurface(IRHIRenderTargetView*, IRHIRenderTargetView*) {}
void InternalDeviceVK::SetViewport(RHIViewport&) {}
void InternalDeviceVK::SetScissorRect(Irect*) {}
void InternalDeviceVK::SetRenderTargets(u32, IRHIRenderTargetView* const*, IRHIUnorderedAccessView* const*) {}
void InternalDeviceVK::SetDSV(IRHIDepthStencilView*) {}
void InternalDeviceVK::SetConstants(const RHIConstantWrite*, u32) {}
void InternalDeviceVK::ClearIndexBuffer() {}
void InternalDeviceVK::SetShader(RHIObject*, ERHI_SHADER_TYPE) {}
void InternalDeviceVK::SetInputLayout(RHIObject*) {}
void InternalDeviceVK::Dispatch(u32, u32, u32) {}
void InternalDeviceVK::SetSamplers(RHIObject* const*, u32) {}
void InternalDeviceVK::SetComputeResources(u32, u32, IRHIShaderResourceView* const*) {}
void InternalDeviceVK::SetComputeUAVs(u32, u32, IRHIUnorderedAccessView* const*, const u32*) {}
void InternalDeviceVK::BeginQuery(RHIObject*) {}
void InternalDeviceVK::EndQuery(RHIObject*) {}
void InternalDeviceVK::CopySwapchain(IRHISurface*) {}
void InternalDeviceVK::SetBlendState(RHIObject*, const float*, u32) {}
void InternalDeviceVK::SetRawBlendState(void*, const float*, u32) {}

void InternalDeviceVK::ResizeBuffers(u32 Width, u32 Height)
{
	PROF_EVENT("Vulkan: ResizeBuffers");
	ContextLock guard(*this);

	if (_vkDevice == VK_NULL_HANDLE || _vkSwapchain == VK_NULL_HANDLE)
	{
		return;
	}
	if (Width == 0 || Height == 0)
	{
		return;
	}

	vkDeviceWaitIdle(_vkDevice);

	psCurrentVidMode[0] = Width;
	psCurrentVidMode[1] = Height;
	HalfTarget.x = (int)Width;
	HalfTarget.y = (int)Height;

	for (VkImageView v : _swapchainImageViews)
	{
		if (v != VK_NULL_HANDLE) vkDestroyImageView(_vkDevice, v, nullptr);
	}
	_swapchainImageViews.clear();
	_swapchainImages.clear();

	CreateSwapchain();
	CreateSwapchainImageViews();

	RHITextureDesc depthDesc;
	depthDesc.Width           = _swapchainExtent.width;
	depthDesc.Height          = _swapchainExtent.height;
	depthDesc.Format          = ERHI_FORMAT::D24_UNORM_S8_UINT;
	depthDesc.BindFlags       = ERHI_BIND_FLAG::DEPTH_STENCIL;
	depthDesc.Usage           = ERHI_USAGE::USAGE_DEFAULT;
	depthDesc.SampleDescCount = 1;

	auto* depthSurface = new VkStubSurface(depthDesc);
	RenderDSV = new VkStubDSV(depthSurface);

	RHITextureDesc colorDesc;
	colorDesc.Width           = _swapchainExtent.width;
	colorDesc.Height          = _swapchainExtent.height;
	colorDesc.Format          = ERHI_FORMAT::R8G8B8A8_UNORM;
	colorDesc.BindFlags       = ERHI_BIND_FLAG::RENDER_TARGET | ERHI_BIND_FLAG::SHADER_RESOURCE;
	colorDesc.Usage           = ERHI_USAGE::USAGE_DEFAULT;
	colorDesc.SampleDescCount = 1;

	auto* colorSurface = new VkStubSurface(colorDesc);
	RenderRTV       = new VkStubRTV(colorSurface);
	RenderTexture   = RenderRTV;
	RenderSRV       = new VkStubSRV();
	SwapChainRTV    = new VkStubRTV(new VkStubSurface(colorDesc));

	Msg("* [VK] RenderDSV=%p RenderRTV=%p SwapChainRTV=%p",
		(void*)RenderDSV, (void*)RenderRTV, (void*)SwapChainRTV);

	_frame          = 0;
	_imageIndex     = 0;
	_frameSerial    = 0;
	_isRecording    = false;
	_swapchainOutOfDate = false;

	Msg("* [Vulkan] swapchain resized: %ux%u", Width, Height);
}

void InternalDeviceVK::Present()
{
	PROF_EVENT("Vulkan: Present");
	ContextLock guard(*this);

	if (_vkDevice == VK_NULL_HANDLE || _vkSwapchain == VK_NULL_HANDLE)
	{
		return;
	}
	if (!_isRecording)
	{
		return;
	}

	Frame& f = _frames[_frame];
	_isRecording = false;

	VkImageMemoryBarrier2 barrier{};
	barrier.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
	barrier.srcStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
	barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
	barrier.dstStageMask  = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
	barrier.dstAccessMask = VK_ACCESS_2_NONE;
	barrier.oldLayout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	barrier.newLayout     = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	barrier.image         = _swapchainImages[_imageIndex];
	barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

	VkDependencyInfo dep{};
	dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
	dep.imageMemoryBarrierCount = 1;
	dep.pImageMemoryBarriers    = &barrier;
	vkCmdPipelineBarrier2(f.CommandBuffer, &dep);

	VkCheck(vkEndCommandBuffer(f.CommandBuffer), "vkEndCommandBuffer");

	VkSemaphoreSubmitInfo waitInfo{};
	waitInfo.sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
	waitInfo.semaphore = f.ImageReady;
	waitInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

	VkSemaphoreSubmitInfo signalInfo{};
	signalInfo.sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
	signalInfo.semaphore = f.RenderDone;
	signalInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;

	VkCommandBufferSubmitInfo cmdInfo{};
	cmdInfo.sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
	cmdInfo.commandBuffer = f.CommandBuffer;

	VkSubmitInfo2 submit{};
	submit.sType                    = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
	submit.waitSemaphoreInfoCount   = 1;
	submit.pWaitSemaphoreInfos      = &waitInfo;
	submit.commandBufferInfoCount   = 1;
	submit.pCommandBufferInfos      = &cmdInfo;
	submit.signalSemaphoreInfoCount = 1;
	submit.pSignalSemaphoreInfos    = &signalInfo;

	VkCheck(vkQueueSubmit2(_graphicsVkQueue, 1, &submit, f.InFlight), "vkQueueSubmit2");

	VkPresentInfoKHR present{};
	present.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	present.waitSemaphoreCount = 1;
	present.pWaitSemaphores    = &f.RenderDone;
	present.swapchainCount     = 1;
	present.pSwapchains        = &_vkSwapchain;
	present.pImageIndices      = &_imageIndex;

	const VkResult r = vkQueuePresentKHR(_presentVkQueue, &present);
	if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
	{
		_swapchainOutOfDate = true;
	}
	else
	{
		VkCheck(r, "vkQueuePresentKHR");
	}

	_frame = (_frame + 1) % FrameCount;
	_frameSerial++;
	++ConstantEpoch;
}

// =====================================================================
// Stub implementations: возвращают валидные объекты.
// =====================================================================

IRHITextureFactory* InternalDeviceVK::GetTextureFactory()
{
	return &g_VkStubTextureFactory;
}

IRHIBuffer* InternalDeviceVK::CreateBuffer(const RHIBufferDesc& desc, const RHIBufferSubresource*)
{
	return new VkStubBuffer(desc);
}

bool InternalDeviceVK::ReadRenderTargetPixels(IRHIRenderTargetView*, void*, u32, u32&, u32&, u32&)
{
	return false;
}

void* InternalDeviceVK::GetContext()
{
	// Возвращаем command buffer текущего кадра — движок использует его
	// в некоторых вспомогательных местах как opaque handle.
	return _frames[_frame].CommandBuffer;
}

void* InternalDeviceVK::GetSwapchain()
{
	return _vkSwapchain;
}

void InternalDeviceVK::BeginFrame()
{
	PROF_EVENT("Vulkan: BeginFrame");
	ContextLock guard(*this);

	if (_vkDevice == VK_NULL_HANDLE || _vkSwapchain == VK_NULL_HANDLE)
	{
		return;
	}

	int windowW = 0, windowH = 0;
	SDL_GetWindowSizeInPixels(g_AppInfo.Window, &windowW, &windowH);
	if (windowW > 0 && windowH > 0 &&
	    (u32(windowW) != _swapchainExtent.width || u32(windowH) != _swapchainExtent.height))
	{
		ResizeBuffers(u32(windowW), u32(windowH));
	}

	Frame& f = _frames[_frame];

	VkCheck(vkWaitForFences(_vkDevice, 1, &f.InFlight, VK_TRUE, UINT64_MAX),
	        "vkWaitForFences");
	VkCheck(vkResetFences(_vkDevice, 1, &f.InFlight), "vkResetFences");

	const VkResult acq = vkAcquireNextImageKHR(
	    _vkDevice, _vkSwapchain, UINT64_MAX, f.ImageReady, VK_NULL_HANDLE, &_imageIndex);

	R_ASSERT2(acq == VK_SUCCESS || acq == VK_SUBOPTIMAL_KHR,
	          "[Vulkan] vkAcquireNextImageKHR failed");

	VkCheck(vkResetCommandBuffer(f.CommandBuffer, 0), "vkResetCommandBuffer");

	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	VkCheck(vkBeginCommandBuffer(f.CommandBuffer, &beginInfo), "vkBeginCommandBuffer");
	_isRecording = true;

	VkImageMemoryBarrier2 barrier{};
	barrier.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
	barrier.srcStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
	barrier.srcAccessMask = VK_ACCESS_2_NONE;
	barrier.dstStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
	barrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
	barrier.oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
	barrier.newLayout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	barrier.image         = _swapchainImages[_imageIndex];
	barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

	VkDependencyInfo dep{};
	dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
	dep.imageMemoryBarrierCount = 1;
	dep.pImageMemoryBarriers    = &barrier;
	vkCmdPipelineBarrier2(f.CommandBuffer, &dep);

	VkRenderingAttachmentInfo colorAttachment{};
	colorAttachment.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
	colorAttachment.imageView   = _swapchainImageViews[_imageIndex];
	colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	colorAttachment.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
	colorAttachment.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
	colorAttachment.clearValue.color = { { 0.2f, 0.3f, 0.4f, 1.0f } };

	VkRenderingInfo renderInfo{};
	renderInfo.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
	renderInfo.renderArea           = { { 0, 0 }, _swapchainExtent };
	renderInfo.layerCount           = 1;
	renderInfo.colorAttachmentCount = 1;
	renderInfo.pColorAttachments    = &colorAttachment;

	vkCmdBeginRendering(f.CommandBuffer, &renderInfo);
	vkCmdEndRendering(f.CommandBuffer);
}

IRHIShaderResourceView* InternalDeviceVK::CreateShaderResourceView(IRHIBuffer*, const RHIShaderResourceViewDesc*)
{
	return new VkStubSRV();
}

IRHIUnorderedAccessView* InternalDeviceVK::CreateUAV(IRHIBuffer*, const RHIUAVDesc&)
{
	return new VkStubUAV();
}

HRESULT InternalDeviceVK::LoadDDS(const void*, size_t, ERHI_USAGE, u32, ERHI_CPU_ACCESS_FLAG, int&, bool, IRHISurface** out_surface)
{
	if (out_surface) *out_surface = new VkStubSurface();
	return S_OK;
}

HRESULT InternalDeviceVK::CreateShader(const void*, size_t, ERHI_SHADER_TYPE, RHIObject** out_shader)
{
	if (out_shader) *out_shader = new RHIObject(nullptr, VkStubNoopRelease);
	return S_OK;
}

HRESULT InternalDeviceVK::ReplaceShader(RHIObject*, const void*, size_t)
{
	return S_OK;
}

void InternalDeviceVK::Flush()
{
	if (_vkDevice != VK_NULL_HANDLE)
	{
		vkDeviceWaitIdle(_vkDevice);
	}
}

HRESULT InternalDeviceVK::CreateInputLayout(const RHIInputElementDesc*, size_t, const void*, size_t, RHIObject** out_layout)
{
	if (out_layout) *out_layout = new RHIObject(nullptr, VkStubNoopRelease);
	return S_OK;
}

HRESULT InternalDeviceVK::CreateSamplerState(const RHISampleDesc&, RHIObject** out_state)
{
	if (out_state) *out_state = new RHIObject(nullptr, VkStubNoopRelease);
	return S_OK;
}

HRESULT InternalDeviceVK::CreateOcclusionQuery(RHIObject** out_query)
{
	if (out_query) *out_query = new RHIObject(nullptr, VkStubNoopRelease);
	return S_OK;
}

HRESULT InternalDeviceVK::GetQueryData(RHIObject*, void*, u32, u32)
{
	return S_OK;
}

IRHISurface* InternalDeviceVK::CreateTexture1D(const RHITextureDesc& desc, const RHISubResource&)
{
	return new VkStubSurface(desc);
}

bool InternalDeviceVK::SupportsTextureSampling(ERHI_FORMAT, u32& out_flags)
{
	out_flags = 0xFFFFFFFF;
	return true;
}

void* InternalDeviceVK::GetState(const RHIRasterizerDesc& desc)
{
	static RHIRasterizerDesc s_stub = desc;
	s_stub = desc;
	return &s_stub;
}

void* InternalDeviceVK::GetState(const RHIDepthStencilDesc& desc)
{
	static RHIDepthStencilDesc s_stub = desc;
	s_stub = desc;
	return &s_stub;
}

void* InternalDeviceVK::GetState(const RHIBlendDesc& desc)
{
	static RHIBlendDesc s_stub = desc;
	s_stub = desc;
	return &s_stub;
}

HRESULT InternalDeviceVK::CreateBlendState(const RHIBlendDesc& desc, RHIObject** out_state)
{
	if (out_state) *out_state = new RHIObject(new RHIBlendDesc(desc), DeleteRhiBlendDesc);
	return S_OK;
}

bool InternalDeviceVK::SetDepthBounds(bool, float, float)
{
	return false;
}

IRHIStateManager* InternalDeviceVK::CreateStateManager()
{
	return new VkStateManager();
}

IRHIShaderResourceStateCache* InternalDeviceVK::CreateResourceCache()
{
	return new VkResourceCache();
}

InternalDeviceVK::~InternalDeviceVK()
{
	if (_vkDevice == VK_NULL_HANDLE) return;

	vkDeviceWaitIdle(_vkDevice);

	for (u32 i = 0; i < FrameCount; ++i)
	{
		Frame& f = _frames[i];
		if (f.RenderDone  != VK_NULL_HANDLE) vkDestroySemaphore(_vkDevice, f.RenderDone,  nullptr);
		if (f.ImageReady  != VK_NULL_HANDLE) vkDestroySemaphore(_vkDevice, f.ImageReady,  nullptr);
		if (f.InFlight    != VK_NULL_HANDLE) vkDestroyFence    (_vkDevice, f.InFlight,    nullptr);
		if (f.CommandPool != VK_NULL_HANDLE) vkDestroyCommandPool(_vkDevice, f.CommandPool, nullptr);
	}

	for (VkImageView v : _swapchainImageViews)
	{
		if (v != VK_NULL_HANDLE) vkDestroyImageView(_vkDevice, v, nullptr);
	}
	_swapchainImageViews.clear();
	if (_vkSwapchain != VK_NULL_HANDLE) vkDestroySwapchainKHR(_vkDevice, _vkSwapchain, nullptr);

	if (_vkPipelineCache != VK_NULL_HANDLE) vkDestroyPipelineCache(_vkDevice, _vkPipelineCache, nullptr);

	vkDestroyDevice(_vkDevice, nullptr);
	_vkDevice = VK_NULL_HANDLE;

	if (_vkSurface  != VK_NULL_HANDLE) vkDestroySurfaceKHR(_vkInstance, _vkSurface, nullptr);
	if (_vkInstance != VK_NULL_HANDLE) vkDestroyInstance(_vkInstance, nullptr);
}