#include "Device.h"
#include "Shader.h"
#include "State.h"
#include "Utils.h"
#include "Buffer.h"
#include "VkStubs.h"

#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan_core.h>


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

static void DeleteShader(void* payload)
{
	// delete static_cast<CVKShader*>(payload);
	xr_delete(static_cast<CVKShader*>(payload));
}

static VKAPI_ATTR VkBool32 VKAPI_CALL VulkanDebugCallback(
	VkDebugUtilsMessageSeverityFlagBitsEXT severity,
	VkDebugUtilsMessageTypeFlagsEXT /*type*/,
	const VkDebugUtilsMessengerCallbackDataEXT* data,
	void* /*userData*/)
{
	if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
		Msg("! [VK-ERR] %s", data->pMessage);
	else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
		Msg("! [VK-WRN] %s", data->pMessage);
	return VK_FALSE;
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
	CreateTriangleResources();

	// --- IRHIDevice defaults ---
	FeatureLevel = RHI_FEATURE_LEVEL_11_1;
	VertexCache = 32;
	NoOverwriteSRV = true;

	Msg("* [Vulkan] Vulkan device initialized");
}

bool InternalDeviceVK::IsValidationLayerAvailable()
{
	u32 layerCount = 0;
	if (vkEnumerateInstanceLayerProperties(&layerCount, nullptr) != VK_SUCCESS || layerCount == 0)
		return false;

	xr_vector<VkLayerProperties> layers(layerCount);
	if (vkEnumerateInstanceLayerProperties(&layerCount, layers.data()) != VK_SUCCESS)
		return false;

	for (const auto& layer : layers)
	{
		if (strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0)
			return true;
	}
	return false;
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

    // was `const xr_vector<...>` - убираем const, чтобы можно было push_back
    xr_vector<const char*> extensions(sdlExtensions, sdlExtensions + extensionsCount);

	static constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

	const bool wantValidation = Core.ParamsData.test(ECoreParams::dxdebug);
	const bool hasValidation  = wantValidation && IsValidationLayerAvailable();
	const bool enableValidation = hasValidation; // = wantValidation && layerAvailable

	if (wantValidation && !hasValidation)
	{
		Msg("! [VK] Validation requested (-dxdebug) but '%s' is not installed. "
			"Install Vulkan SDK: https://vulkan.lunarg.com/sdk/home", kValidationLayer);
	}
	else if (hasValidation)
	{
		extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
		Msg("* [VK] Validation layers: enabled");
	}


    VkInstanceCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = static_cast<u32>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

    if (enableValidation)
    {
        createInfo.enabledLayerCount = 1;
        createInfo.ppEnabledLayerNames = &kValidationLayer;
    }

#if defined(IXR_APPLE_SERIES)
    createInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif

    VkCheck(vkCreateInstance(&createInfo, nullptr, &_vkInstance), "vkCreateInstance");

	if (enableValidation)
	{
		auto createMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
			vkGetInstanceProcAddr(_vkInstance, "vkCreateDebugUtilsMessengerEXT"));

		if (createMessenger)
		{
			VkDebugUtilsMessengerCreateInfoEXT messengerInfo = {};
			messengerInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
			messengerInfo.messageSeverity =
				VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
				VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
			messengerInfo.messageType =
				VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
				VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
				VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
			messengerInfo.pfnUserCallback = VulkanDebugCallback;

			VkCheck(createMessenger(_vkInstance, &messengerInfo, nullptr, &_vkDebugMessenger),
				"vkCreateDebugUtilsMessengerEXT");
		}
	}
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

void InternalDeviceVK::CreateTriangleResources()
{
    PROF_EVENT("Vulkan: CreateTriangleResources");

    // ---- Загрузка SPIR-V ----
	auto loadSpirv = [](const char* name) -> xr_vector<u8>
	{
		string_path path;
		FS.update_path(path, "$game_shaders$", (xr_string("vk\\") + name).c_str());

		Msg("* [VK] Loading SPIR-V: %s", path);   // для отладки — сразу видно, куда лезем

		IReader* reader = FS.r_open(path);
		if (!reader)
		{
			Msg("! [VK] Cannot open SPIR-V: %s", path);
			return {};
		}

		xr_vector<u8> data(reader->length());
		reader->r(data.data(), reader->length());
		FS.r_close(reader);
		return data;
	};

    auto vsData = loadSpirv("triangle_vs.spv");
    auto psData = loadSpirv("triangle_ps.spv");
    R_ASSERT3(!vsData.empty() && !psData.empty(),
        "Triangle SPIR-V not found. Put triangle_vs.spv/triangle_ps.spv into gamedata/shaders_vk/",
        "");
    R_ASSERT2((vsData.size() % 4 == 0) && (psData.size() % 4 == 0),
        "SPIR-V size must be a multiple of 4");

    VkShaderModuleCreateInfo vsInfo = {};
    vsInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    vsInfo.codeSize = vsData.size();
    vsInfo.pCode    = reinterpret_cast<const u32*>(vsData.data());

    VkShaderModule vsModule = VK_NULL_HANDLE;
    VkCheck(vkCreateShaderModule(_vkDevice, &vsInfo, nullptr, &vsModule),
        "vkCreateShaderModule (triangle VS)");

    VkShaderModuleCreateInfo psInfo = {};
    psInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    psInfo.codeSize = psData.size();
    psInfo.pCode    = reinterpret_cast<const u32*>(psData.data());

    VkShaderModule psModule = VK_NULL_HANDLE;
    VkCheck(vkCreateShaderModule(_vkDevice, &psInfo, nullptr, &psModule),
        "vkCreateShaderModule (triangle PS)");

    // ---- Pipeline layout (пустой) ----
    VkPipelineLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    VkCheck(vkCreatePipelineLayout(_vkDevice, &layoutInfo, nullptr, &_triangleLayout),
        "vkCreatePipelineLayout (triangle)");

    // ---- Vertex buffer (3 вершины) ----
    struct Vertex { float pos[3]; float color[3]; };
    const Vertex verts[3] = {
        { {  0.0f, -0.5f, 0.0f }, { 1.0f, 0.0f, 0.0f } }, // низ, красный
        { {  0.5f,  0.5f, 0.0f }, { 0.0f, 1.0f, 0.0f } }, // правый верх, зелёный
        { { -0.5f,  0.5f, 0.0f }, { 0.0f, 0.0f, 1.0f } }, // левый верх, синий
    };
    const VkDeviceSize vbSize = sizeof(verts);

    VkBufferCreateInfo vbInfo = {};
    vbInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    vbInfo.size        = vbSize;
    vbInfo.usage       = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    vbInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkCheck(vkCreateBuffer(_vkDevice, &vbInfo, nullptr, &_triangleVB),
        "vkCreateBuffer (triangle)");

    VkMemoryRequirements memReq = {};
    vkGetBufferMemoryRequirements(_vkDevice, _triangleVB, &memReq);

    VkPhysicalDeviceMemoryProperties memProps = {};
    vkGetPhysicalDeviceMemoryProperties(_vkPhysicalDevice, &memProps);

    const VkMemoryPropertyFlags required =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    u32 memTypeIdx = UINT32_MAX;
    for (u32 i = 0; i < memProps.memoryTypeCount; ++i)
    {
        if ((memReq.memoryTypeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & required) == required)
        {
            memTypeIdx = i;
            break;
        }
    }
    R_ASSERT2(memTypeIdx != UINT32_MAX, "No HOST_VISIBLE|HOST_COHERENT memory type for triangle VB");

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize  = memReq.size;
    allocInfo.memoryTypeIndex = memTypeIdx;
    VkCheck(vkAllocateMemory(_vkDevice, &allocInfo, nullptr, &_triangleVBMem),
        "vkAllocateMemory (triangle)");
    VkCheck(vkBindBufferMemory(_vkDevice, _triangleVB, _triangleVBMem, 0),
        "vkBindBufferMemory (triangle)");

    void* mapped = nullptr;
    VkCheck(vkMapMemory(_vkDevice, _triangleVBMem, 0, vbSize, 0, &mapped),
        "vkMapMemory (triangle)");
    memcpy(mapped, verts, vbSize);
    vkUnmapMemory(_vkDevice, _triangleVBMem); // HOST_COHERENT → flush не нужен

    // ---- Pipeline ----
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vsModule;
    stages[0].pName  = "main";

    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = psModule;
    stages[1].pName  = "main";

    VkVertexInputBindingDescription binding = {};
    binding.binding   = 0;
    binding.stride    = sizeof(Vertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[2] = {};
    attrs[0].location = 0;
    attrs[0].binding  = 0;
    attrs[0].format   = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[0].offset   = offsetof(Vertex, pos);

    attrs[1].location = 1;
    attrs[1].binding  = 0;
    attrs[1].format   = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[1].offset   = offsetof(Vertex, color);

    VkPipelineVertexInputStateCreateInfo vertexInput = {};
    vertexInput.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount   = 1;
    vertexInput.pVertexBindingDescriptions      = &binding;
    vertexInput.vertexAttributeDescriptionCount = 2;
    vertexInput.pVertexAttributeDescriptions    = attrs;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
    inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState = {};
    viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer = {};
    rasterizer.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode    = VK_CULL_MODE_NONE;
    rasterizer.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisampling = {};
    multisampling.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blendAttachment = {};
    blendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blendAttachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlend = {};
    colorBlend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments    = &blendAttachment;

    VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState = {};
    dynamicState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates    = dynamicStates;

    // Dynamic rendering: формат обязателен, layout НЕ указывается (он в VkRenderingAttachmentInfo)
    VkPipelineRenderingCreateInfo renderingInfo = {};
    renderingInfo.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingInfo.colorAttachmentCount    = 1;
    renderingInfo.pColorAttachmentFormats = &_swapchainFormat;
    renderingInfo.depthAttachmentFormat   = VK_FORMAT_UNDEFINED;
    renderingInfo.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;

    VkGraphicsPipelineCreateInfo pipelineInfo = {};
    pipelineInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.pNext               = &renderingInfo;
    pipelineInfo.stageCount          = 2;
    pipelineInfo.pStages             = stages;
    pipelineInfo.pVertexInputState   = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState      = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState   = &multisampling;
    pipelineInfo.pColorBlendState    = &colorBlend;
    pipelineInfo.pDynamicState       = &dynamicState;
    pipelineInfo.layout              = _triangleLayout;
    pipelineInfo.renderPass          = VK_NULL_HANDLE;
    pipelineInfo.subpass             = 0;

    VkCheck(vkCreateGraphicsPipelines(_vkDevice, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &_trianglePipeline),
        "vkCreateGraphicsPipelines (triangle)");

    vkDestroyShaderModule(_vkDevice, vsModule, nullptr);
    vkDestroyShaderModule(_vkDevice, psModule, nullptr);

    Msg("* [VK] Triangle resources created (format %d, %ux%u)",
        (int)_swapchainFormat, _swapchainExtent.width, _swapchainExtent.height);
}

static u64 HashPipelineKey(const InternalDeviceVK::PipelineKey& key)
{
    // FNV-1a по байтам
    const u8* p = reinterpret_cast<const u8*>(&key);
    u64 h = 14695981039346656037ull;
    for (size_t i = 0; i < sizeof(key); ++i)
    {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

VkPipeline InternalDeviceVK::GetOrCreatePipeline()
{
    // --- Проверки готовности ---
    CVKShader* vs = _shaders[(u32)ERHI_SHADER_TYPE::VS];
    CVKShader* ps = _shaders[(u32)ERHI_SHADER_TYPE::PS];
    if (!vs || !vs->isSpirV) return VK_NULL_HANDLE;
    if (!ps || !ps->isSpirV) return VK_NULL_HANDLE;
    if (_rtvCount == 0)      return VK_NULL_HANDLE;

    // --- Ключ ---
    PipelineKey key = {};
    key.vs = vs->module;
    key.ps = ps->module;
    if (_shaders[(u32)ERHI_SHADER_TYPE::GS]) key.gs = _shaders[(u32)ERHI_SHADER_TYPE::GS]->module;
    if (_shaders[(u32)ERHI_SHADER_TYPE::HS]) key.hs = _shaders[(u32)ERHI_SHADER_TYPE::HS]->module;
    if (_shaders[(u32)ERHI_SHADER_TYPE::DS]) key.ds = _shaders[(u32)ERHI_SHADER_TYPE::DS]->module;
    key.topology = _topology;
    key.rtvCount = _rtvCount;

    // Формат RTV: из swapchain-формата, если RTV == SwapChainRTV (мы сейчас не различаем RTV по объектам —
    // в 2.5 сделаем CVKRenderTargetView с реальным VkFormat, сейчас хак для MVP)
    for (u32 i = 0; i < _rtvCount; ++i)
        key.rtvFormats[i] = _swapchainFormat;

    const u64 hash = HashPipelineKey(key);
    auto it = _pipelineCache.find(hash);
    if (it != _pipelineCache.end())
        return it->second;

    // --- Создание layout (один на все UI pipeline) ---
    if (_uiPipelineLayout == VK_NULL_HANDLE)
    {
        VkPushConstantRange range = {};
        range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        range.offset = 0;
        range.size   = sizeof(_drawConstants); // 24 dword = 96 bytes

        VkPipelineLayoutCreateInfo layoutInfo = {};
        layoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges    = &range;

        VkResult r = vkCreatePipelineLayout(_vkDevice, &layoutInfo, nullptr, &_uiPipelineLayout);
        if (r != VK_SUCCESS)
        {
            Msg("! [VK] vkCreatePipelineLayout (UI) failed: %d", (int)r);
            return VK_NULL_HANDLE;
        }
    }

    // --- Shader stages ---
    xr_vector<VkPipelineShaderStageCreateInfo> stages;
    stages.reserve(5);

    auto addStage = [&](VkShaderModule mod, VkShaderStageFlagBits bit)
    {
        if (mod == VK_NULL_HANDLE) return;
        VkPipelineShaderStageCreateInfo s = {};
        s.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        s.stage  = bit;
        s.module = mod;
        s.pName  = "main";
        stages.push_back(s);
    };
    addStage(key.vs, VK_SHADER_STAGE_VERTEX_BIT);
    addStage(key.ps, VK_SHADER_STAGE_FRAGMENT_BIT);
    addStage(key.gs, VK_SHADER_STAGE_GEOMETRY_BIT);
    addStage(key.hs, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT);
    addStage(key.ds, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT);

    // --- Vertex input: пустой (UI = vertex pulling) ---
    VkPipelineVertexInputStateCreateInfo vertexInput = {};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    // --- Input assembly ---
    VkPrimitiveTopology vkTopo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    switch (_topology)
    {
    case ERHI_PRIMITIVE_TOPOLOGY::POINT_LIST:     vkTopo = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
    case ERHI_PRIMITIVE_TOPOLOGY::LINE_LIST:      vkTopo = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
    case ERHI_PRIMITIVE_TOPOLOGY::LINE_STRIP:     vkTopo = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
    case ERHI_PRIMITIVE_TOPOLOGY::TRIANGLE_STRIP: vkTopo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
    case ERHI_PRIMITIVE_TOPOLOGY::TRIANGLE_FAN:   vkTopo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; break;
    case ERHI_PRIMITIVE_TOPOLOGY::CONTROL_POINT_3_PATCH: vkTopo = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST; break;
    default: vkTopo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
    }

    VkPipelineInputAssemblyStateCreateInfo ia = {};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = vkTopo;

    // --- Viewport / scissor (dynamic) ---
    VkPipelineViewportStateCreateInfo vp = {};
    vp.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount  = 1;

    // --- Rasterizer ---
    VkPipelineRasterizationStateCreateInfo rast = {};
    rast.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rast.polygonMode = VK_POLYGON_MODE_FILL;
    rast.cullMode    = VK_CULL_MODE_NONE;
    rast.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rast.lineWidth   = 1.0f;

    // --- Multisample ---
    VkPipelineMultisampleStateCreateInfo ms = {};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // --- Blend: стандартный UI (SRC_ALPHA, ONE_MINUS_SRC_ALPHA) ---
    VkPipelineColorBlendAttachmentState blendAtt = {};
    blendAtt.blendEnable    = VK_TRUE;
    blendAtt.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blendAtt.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAtt.colorBlendOp   = VK_BLEND_OP_ADD;
    blendAtt.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAtt.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAtt.alphaBlendOp   = VK_BLEND_OP_ADD;
    blendAtt.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo blend = {};
    blend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments    = &blendAtt;

    // --- Dynamic states ---
    VkDynamicState dyn[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };
    VkPipelineDynamicStateCreateInfo dynState = {};
    dynState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynState.dynamicStateCount = 2;
    dynState.pDynamicStates    = dyn;

    // --- Rendering info (dynamic rendering) ---
    VkPipelineRenderingCreateInfo renderingInfo = {};
    renderingInfo.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingInfo.colorAttachmentCount    = _rtvCount;
    renderingInfo.pColorAttachmentFormats = key.rtvFormats;
    renderingInfo.depthAttachmentFormat   = VK_FORMAT_UNDEFINED;
    renderingInfo.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;

    // --- Pipeline ---
    VkGraphicsPipelineCreateInfo info = {};
    info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext               = &renderingInfo;
    info.stageCount          = (u32)stages.size();
    info.pStages             = stages.data();
    info.pVertexInputState   = &vertexInput;
    info.pInputAssemblyState = &ia;
    info.pViewportState      = &vp;
    info.pRasterizationState = &rast;
    info.pMultisampleState   = &ms;
    info.pColorBlendState    = &blend;
    info.pDynamicState       = &dynState;
    info.layout              = _uiPipelineLayout;
    info.renderPass          = VK_NULL_HANDLE;

    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult r = vkCreateGraphicsPipelines(_vkDevice, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
    if (r != VK_SUCCESS)
    {
        Msg("! [VK] vkCreateGraphicsPipelines (UI) failed: %d (vs=%p ps=%p topo=%d rtvCount=%u)",
            (int)r, (void*)key.vs, (void*)key.ps, (u32)key.topology, key.rtvCount);
        return VK_NULL_HANDLE;
    }

    _pipelineCache[hash] = pipeline;
    Msg("* [VK] UI pipeline created (vs=%p ps=%p topo=%d rtvCount=%u)",
        (void*)key.vs, (void*)key.ps, (u32)key.topology, key.rtvCount);
    return pipeline;
}

void InternalDeviceVK::InvalidatePipelineCache()
{
    for (auto& it : _pipelineCache)
        if (it.second != VK_NULL_HANDLE)
            vkDestroyPipeline(_vkDevice, it.second, nullptr);
    _pipelineCache.clear();
}

// =====================================================================
// Все draw / state / bind - no-op заглушки.
// Никаких Msg, чтобы не засорять лог в render-loop.
// =====================================================================

void InternalDeviceVK::SetPrimitiveTopology(ERHI_PRIMITIVE_TOPOLOGY topology)
{
	_topology = topology;
}

static u32 g_drawSkipped = 0;
static u32 g_drawCalls = 0;

void InternalDeviceVK::Draw(u32 VertexCount, u32 StartVertex)
{
    if (VertexCount == 0 || !_isRecording) return;
    VkPipeline pipe = GetOrCreatePipeline();
    if (pipe == VK_NULL_HANDLE) { ++g_drawSkipped; return; }

    VkCommandBuffer cmd = _frames[_frame].CommandBuffer;

    VkViewport vp = { _viewport.TopLeftX, _viewport.TopLeftY,
                      _viewport.Width, _viewport.Height,
                      _viewport.MinDepth, _viewport.MaxDepth };
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D sc = {};
    if (_hasScissor)
        sc = { { _scissor.x1, _scissor.y1 }, { (u32)(_scissor.x2 - _scissor.x1), (u32)(_scissor.y2 - _scissor.y1) } };
    else
        sc = { { 0, 0 }, _swapchainExtent };
    vkCmdSetScissor(cmd, 0, 1, &sc);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdPushConstants(cmd, _uiPipelineLayout,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        0, sizeof(_drawConstants), _drawConstants);
    vkCmdDraw(cmd, VertexCount, 1, StartVertex, 0);
    ++g_drawCalls;
}

void InternalDeviceVK::DrawInstanced(u32 VertexCount, u32 InstanceCount, u32 StartVertex, u32 StartInstance)
{
    if (VertexCount == 0 || InstanceCount == 0 || !_isRecording) return;
    VkPipeline pipe = GetOrCreatePipeline();
    if (pipe == VK_NULL_HANDLE) { ++g_drawSkipped; return; }

    VkCommandBuffer cmd = _frames[_frame].CommandBuffer;
    VkViewport vp = { _viewport.TopLeftX, _viewport.TopLeftY,
                      _viewport.Width, _viewport.Height,
                      _viewport.MinDepth, _viewport.MaxDepth };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc = _hasScissor
        ? VkRect2D{ { _scissor.x1, _scissor.y1 }, { (u32)(_scissor.x2 - _scissor.x1), (u32)(_scissor.y2 - _scissor.y1) } }
        : VkRect2D{ { 0, 0 }, _swapchainExtent };
    vkCmdSetScissor(cmd, 0, 1, &sc);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdPushConstants(cmd, _uiPipelineLayout,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        0, sizeof(_drawConstants), _drawConstants);
    vkCmdDraw(cmd, VertexCount, InstanceCount, StartVertex, StartInstance);
    ++g_drawCalls;
}

void InternalDeviceVK::DrawIndexed(u32 IndexCount, u32 StartIndex, u32 BaseVertex)
{
    if (IndexCount == 0 || !_isRecording) return;
    VkPipeline pipe = GetOrCreatePipeline();
    if (pipe == VK_NULL_HANDLE) { ++g_drawSkipped; return; }

    VkCommandBuffer cmd = _frames[_frame].CommandBuffer;
    VkViewport vp = { _viewport.TopLeftX, _viewport.TopLeftY,
                      _viewport.Width, _viewport.Height,
                      _viewport.MinDepth, _viewport.MaxDepth };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc = _hasScissor
        ? VkRect2D{ { _scissor.x1, _scissor.y1 }, { (u32)(_scissor.x2 - _scissor.x1), (u32)(_scissor.y2 - _scissor.y1) } }
        : VkRect2D{ { 0, 0 }, _swapchainExtent };
    vkCmdSetScissor(cmd, 0, 1, &sc);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdPushConstants(cmd, _uiPipelineLayout,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        0, sizeof(_drawConstants), _drawConstants);
    vkCmdDrawIndexed(cmd, IndexCount, 1, StartIndex, (s32)BaseVertex, 0);
    ++g_drawCalls;
}

void InternalDeviceVK::DrawIndexedInstanced(u32 IndexCount, u32 InstanceCount, u32 StartIndex, u32 BaseVertex, u32 StartInstance)
{
    if (IndexCount == 0 || InstanceCount == 0 || !_isRecording) return;
    VkPipeline pipe = GetOrCreatePipeline();
    if (pipe == VK_NULL_HANDLE) { ++g_drawSkipped; return; }

    VkCommandBuffer cmd = _frames[_frame].CommandBuffer;
    VkViewport vp = { _viewport.TopLeftX, _viewport.TopLeftY,
                      _viewport.Width, _viewport.Height,
                      _viewport.MinDepth, _viewport.MaxDepth };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc = _hasScissor
        ? VkRect2D{ { _scissor.x1, _scissor.y1 }, { (u32)(_scissor.x2 - _scissor.x1), (u32)(_scissor.y2 - _scissor.y1) } }
        : VkRect2D{ { 0, 0 }, _swapchainExtent };
    vkCmdSetScissor(cmd, 0, 1, &sc);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdPushConstants(cmd, _uiPipelineLayout,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        0, sizeof(_drawConstants), _drawConstants);
    vkCmdDrawIndexed(cmd, IndexCount, InstanceCount, StartIndex, (s32)BaseVertex, StartInstance);
    ++g_drawCalls;
}


void InternalDeviceVK::ClearTarget(void*, ERTColor) {}
void InternalDeviceVK::ClearTarget(void*, const float*) {}
void InternalDeviceVK::ClearDepthStencil(IRHIDepthStencilView*, ERHI_CLEAR_TARGET, float, u8) {}
void InternalDeviceVK::GenerateMips(IRHIShaderResourceView*) {}
void InternalDeviceVK::CopySurface(IRHISurface*, IRHISurface*) {}
void InternalDeviceVK::CopySurface(IRHIRenderTargetView*, IRHIRenderTargetView*) {}

void InternalDeviceVK::SetViewport(RHIViewport& VP)
{
	_viewport = VP;
}

void InternalDeviceVK::SetScissorRect(Irect* R)
{
	if (R)
	{
		_scissor = *R;
		_hasScissor = true;
	}
	else
		_hasScissor = false;
}

void InternalDeviceVK::SetRenderTargets(u32 NumViews, IRHIRenderTargetView* const* ppRenderTargetViews, IRHIUnorderedAccessView* const* /*ppRenderUAViews*/)
{
	_rtvCount = NumViews < 8 ? NumViews : 8;
	for (u32 i = 0; i < 8; ++i)
		_rtvs[i] = (i < _rtvCount && ppRenderTargetViews) ? ppRenderTargetViews[i] : nullptr;
}

void InternalDeviceVK::SetDSV(IRHIDepthStencilView* pDepthStencilView)
{
	_dsv = pDepthStencilView;
}

void InternalDeviceVK::SetConstants(const RHIConstantWrite* Writes, u32 Count)
{
	if (!Writes) return;
	for (u32 i = 0; i < Count; ++i)
	{
		const u32 slot = Writes[i].Slot;
		// Для UI нужен только DrawSlot = 13 (CBDraw: draw_flags, draw_vertex_base, ...).
		// Остальные слоты (CBView, CBFrame, ...) будем привязывать через UBO в шаге 2.6.
		if (slot == 13 && Writes[i].Data && Writes[i].Size <= sizeof(_drawConstants))
			memcpy(_drawConstants, Writes[i].Data, Writes[i].Size);
	}
}

void InternalDeviceVK::ClearIndexBuffer() {}

void InternalDeviceVK::SetShader(RHIObject* shader, ERHI_SHADER_TYPE type)
{
	auto* vkShader = shader ? static_cast<CVKShader*>(shader->resource) : nullptr;
	_shaders[(u32)type] = vkShader;
}

void InternalDeviceVK::SetInputLayout(RHIObject* layout)
{
	_inputLayout = layout;
}

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
	
	vkCmdEndRendering(f.CommandBuffer);

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
	
	if ((_frame % 60) == 0 && (g_drawCalls || g_drawSkipped))
	{
		Msg("* [VK] Draw stats (frame %u): draws=%u skipped=%u", _frame, g_drawCalls, g_drawSkipped);
		g_drawCalls = 0;
		g_drawSkipped = 0;
	}

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

IRHIBuffer* InternalDeviceVK::CreateBuffer(const RHIBufferDesc& desc, const RHIBufferSubresource* pSubresource)
{
	return new CVKBuffer(_vkDevice, _vkPhysicalDevice, desc, pSubresource);
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
	colorAttachment.clearValue.color = { { 0.0f, 0.3f, 0.4f, 1.0f } };

	VkRenderingInfo renderInfo{};
	renderInfo.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
	renderInfo.renderArea           = { { 0, 0 }, _swapchainExtent };
	renderInfo.layerCount           = 1;
	renderInfo.colorAttachmentCount = 1;
	renderInfo.pColorAttachments    = &colorAttachment;

	vkCmdBeginRendering(f.CommandBuffer, &renderInfo);
	
	// --- MVP: тестовый треугольник ---
	if (_trianglePipeline != VK_NULL_HANDLE)
	{
		vkCmdBindPipeline(f.CommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, _trianglePipeline);
		VkDeviceSize vbOffset = 0;
		vkCmdBindVertexBuffers(f.CommandBuffer, 0, 1, &_triangleVB, &vbOffset);

		// Треугольник 1 — левая половина
		VkViewport vp1 = { 0.0f, 0.0f,
			_swapchainExtent.width * 0.5f, (float)_swapchainExtent.height, 0.0f, 1.0f };
		VkRect2D sc1 = { { 0, 0 }, { _swapchainExtent.width / 2, _swapchainExtent.height } };
		vkCmdSetViewport(f.CommandBuffer, 0, 1, &vp1);
		vkCmdSetScissor (f.CommandBuffer, 0, 1, &sc1);
		vkCmdDraw(f.CommandBuffer, 3, 1, 0, 0);

		// Треугольник 2 — правая половина
		VkViewport vp2 = { (float)_swapchainExtent.width * 0.5f, 0.0f,
			_swapchainExtent.width * 0.5f, (float)_swapchainExtent.height, 0.0f, 1.0f };
		VkRect2D sc2 = { { (int)(_swapchainExtent.width / 2), 0 },
						 { _swapchainExtent.width / 2, _swapchainExtent.height } };
		vkCmdSetViewport(f.CommandBuffer, 0, 1, &vp2);
		vkCmdSetScissor (f.CommandBuffer, 0, 1, &sc2);
		vkCmdDraw(f.CommandBuffer, 3, 1, 0, 0);
	}
}

IRHIShaderResourceView* InternalDeviceVK::CreateShaderResourceView(IRHIBuffer*, const RHIShaderResourceViewDesc*)
{
	return new VkStubSRV();
}

IRHIUnorderedAccessView* InternalDeviceVK::CreateUAV(IRHIBuffer*, const RHIUAVDesc&)
{
	return new VkStubUAV();
}

HRESULT InternalDeviceVK::LoadDDS(const void* data, size_t size, ERHI_USAGE usage,
	u32 bind_flags, ERHI_CPU_ACCESS_FLAG cpu_flags, int& lod, bool fallback,
	IRHISurface** out_surface)
{
	*out_surface = nullptr;
	if (!data || !size)
	{
		return E_INVALIDARG;
	}

	RHITextureMetadata meta{};
	HRESULT hr = GRHI->GetDDSMetadata(data, size, meta);
	if (FAILED(hr))
	{
		return hr;
	}

	// Учёт lod для мип-цепочки (как в D3D11/D3D12 LoadDDS)
	if (!meta.cubemap && !meta.volumemap && lod)
	{
		while (meta.mipmap_count > 1 && meta.width > 4 && meta.height > 4 && lod)
		{
			meta.width  /= 2;
			meta.height /= 2;
			--meta.mipmap_count;
			--lod;
		}
		meta.width  = std::max<u32>(meta.width,  4);
		meta.height = std::max<u32>(meta.height, 4);
	}

	RHITextureDesc desc(meta.width, meta.height, (ERHI_FORMAT)meta.format);
	desc.MipLevels  = meta.mipmap_count ? meta.mipmap_count : 1;
	desc.ArraySize  = meta.cubemap ? 6 : (meta.volumemap ? 1 : 1);
	desc.Depth      = meta.volumemap ? std::max<u32>(meta.height, 1) : 1;  // поправь, если нужен реальный depth из метаданных
	desc.BindFlags  = (ERHI_BIND_FLAG)bind_flags;
	desc.Usage      = usage;
	desc.CPUAccessFlags = (u32)cpu_flags;
	desc.MiscFlags  = meta.cubemap ? 0x4 /* D3D11_RESOURCE_MISC_TEXTURECUBE */ : 0;

	*out_surface = new VkStubSurface(desc);
	return S_OK;
}

HRESULT InternalDeviceVK::CreateShader(const void* code, size_t size, ERHI_SHADER_TYPE type, RHIObject** out_shader)
{
	*out_shader = nullptr;

	if (!code || size < 4)
		return E_INVALIDARG;

	const u32 magic = *static_cast<const u32*>(code);
	const bool isSpirV = (magic == 0x07230203);

	if (!isSpirV)
	{
		// DXBC или что-то ещё — пока не умеем. Возвращаем stub, движок не упадёт.
		Msg("* [VK] CreateShader: non-SPIR-V for type=%d size=%u, returning stub",
			(u32)type, (u32)size);
		auto* shader = new CVKShader(_vkDevice, VK_NULL_HANDLE, type, false);
		*out_shader = new RHIObject(shader, DeleteShader);
		return S_OK;
	}

	VkShaderModuleCreateInfo info = {};
	info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	info.codeSize = size;
	info.pCode    = static_cast<const u32*>(code);

	VkShaderModule module = VK_NULL_HANDLE;
	VkResult r = vkCreateShaderModule(_vkDevice, &info, nullptr, &module);
	if (r != VK_SUCCESS)
	{
		Msg("! [VK] vkCreateShaderModule failed (type=%d, size=%u, r=%d)",
			(u32)type, (u32)size, (int)r);
		auto* shader = new CVKShader(_vkDevice, VK_NULL_HANDLE, type, false);
		*out_shader = new RHIObject(shader, DeleteShader);
		return E_FAIL;
	}

	auto* shader = new CVKShader(_vkDevice, module, type, true);
	*out_shader = new RHIObject(shader, DeleteShader);
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

	if (_trianglePipeline != VK_NULL_HANDLE) vkDestroyPipeline(_vkDevice, _trianglePipeline, nullptr);
	if (_triangleLayout   != VK_NULL_HANDLE) vkDestroyPipelineLayout(_vkDevice, _triangleLayout, nullptr);
	if (_triangleVB       != VK_NULL_HANDLE) vkDestroyBuffer(_vkDevice, _triangleVB, nullptr);
	if (_triangleVBMem    != VK_NULL_HANDLE) vkFreeMemory(_vkDevice, _triangleVBMem, nullptr);
	
	if (_uiPipelineLayout != VK_NULL_HANDLE)
	{
		vkDestroyPipelineLayout(_vkDevice, _uiPipelineLayout, nullptr);
		_uiPipelineLayout = VK_NULL_HANDLE;
	}
	InvalidatePipelineCache();
	
	vkDestroyDevice(_vkDevice, nullptr);
	_vkDevice = VK_NULL_HANDLE;

	// --- Validation messenger (до destroy instance!) ---
	if (_vkDebugMessenger != VK_NULL_HANDLE)
	{
		auto destroyMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
			vkGetInstanceProcAddr(_vkInstance, "vkDestroyDebugUtilsMessengerEXT"));

		if (destroyMessenger)
			destroyMessenger(_vkInstance, _vkDebugMessenger, nullptr);

		_vkDebugMessenger = VK_NULL_HANDLE;
	}

	if (_vkSurface  != VK_NULL_HANDLE) vkDestroySurfaceKHR(_vkInstance, _vkSurface, nullptr);
	if (_vkInstance != VK_NULL_HANDLE) vkDestroyInstance(_vkInstance, nullptr);
}