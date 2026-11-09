#include "Device.h"
#include "Shader.h"
#include "State.h"
#include "Utils.h"
#include "Texture.h"
#include "VkStubs.h"
#include <DirectXTex.h>

#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan_core.h>

#include "Bindings.h"


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
	CreateDescriptorSetLayout();
	CreateDummyResources();
	InitPushDescriptorWrites();
	CreateDescriptorPool();
	UpdatePipelineLayout();
	CreateSwapchain();
	CreateSwapchainImageViews();
	CreateFrameResources();
	
	_textureFactory = new CVKTextureFactory(
		_vkDevice, _vkPhysicalDevice, _loadCmdPool, _graphicsVkQueue
	);

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
	want14.pushDescriptor = VK_FALSE;
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
	
	VkCommandPoolCreateInfo loadPoolInfo{};
	loadPoolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	loadPoolInfo.queueFamilyIndex = _queueFamilies.Graphics;
	loadPoolInfo.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
									VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	VkCheck(vkCreateCommandPool(_vkDevice, &loadPoolInfo, nullptr, &_loadCmdPool), "vkCreateCommandPool (load)");
}

void InternalDeviceVK::CreateDescriptorSetLayout()
{
	PROF_EVENT("Vulkan: CreateDescriptorSetLayout");

	xr_vector<VkDescriptorSetLayoutBinding> bindings;
	bindings.reserve(СVKBindings::TotalBindings);

	auto add = [&](u32 binding, VkDescriptorType type)
	{
		VkDescriptorSetLayoutBinding b{};
		b.binding         = binding;
		b.descriptorType  = type;
		b.descriptorCount = 1;
		b.stageFlags      = VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT;
		bindings.push_back(b);
	};

	for (u32 i = 0; i < СVKBindings::CBCount;      ++i) add(СVKBindings::CBBase       + i, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
	for (u32 i = 0; i < СVKBindings::SrvCount;     ++i) add(СVKBindings::SrvBase      + i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
	for (u32 i = 0; i < СVKBindings::SsboCount;    ++i) add(СVKBindings::SsboBase     + i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	for (u32 i = 0; i < СVKBindings::SamplerCount; ++i) add(СVKBindings::SamplerBase  + i, VK_DESCRIPTOR_TYPE_SAMPLER);
	for (u32 i = 0; i < СVKBindings::UavCount;     ++i) add(СVKBindings::UavBase      + i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
	for (u32 i = 0; i < СVKBindings::UserSsboCount;++i) add(СVKBindings::UserSsboBase + i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

	VkDescriptorSetLayoutCreateInfo info{};
	info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	info.bindingCount = static_cast<u32>(bindings.size());
	info.pBindings    = bindings.data();

	VkCheck(vkCreateDescriptorSetLayout(_vkDevice, &info, nullptr, &_setLayout),
			"vkCreateDescriptorSetLayout");

	Msg("* [VK] Descriptor set layout: %u bindings", static_cast<u32>(bindings.size()));
}

void InternalDeviceVK::CreateDescriptorPool()
{
	PROF_EVENT("Vulkan: CreateDescriptorPool");

	// Per-draw descriptor sets: до 2048 draw / кадр. Реально ~1500 макс.
	constexpr u32 kMaxSets = 2048;

	VkDescriptorPoolSize sizes[] = {
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,  СVKBindings::CBCount        * kMaxSets },
		{ VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,   СVKBindings::SrvCount       * kMaxSets },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, (СVKBindings::SsboCount + СVKBindings::UserSsboCount) * kMaxSets },
		{ VK_DESCRIPTOR_TYPE_SAMPLER,         СVKBindings::SamplerCount   * kMaxSets },
		{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,   СVKBindings::UavCount       * kMaxSets },
	};

	for (u32 f = 0; f < FrameCount; ++f)
	{
		VkDescriptorPoolCreateInfo info{};
		info.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		info.maxSets       = kMaxSets;
		info.poolSizeCount = static_cast<u32>(std::size(sizes));
		info.pPoolSizes    = sizes;
		info.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
		VkCheck(vkCreateDescriptorPool(_vkDevice, &info, nullptr, &_frameDescriptorPools[f]),
				"vkCreateDescriptorPool (per-frame)");
		_frameDescriptorSetUsed[f] = 0;
	}

	Msg("* [VK] Descriptor pools: %u per-frame, maxSets=%u", FrameCount, kMaxSets);
}

void InternalDeviceVK::CreateDummyResources()
{
    PROF_EVENT("Vulkan: CreateDummyResources");

    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(_vkPhysicalDevice, &memProps);

    auto pickMemory = [&](u32 typeBits, VkMemoryPropertyFlags want) -> u32
    {
        for (u32 i = 0; i < memProps.memoryTypeCount; ++i)
        {
            if ((typeBits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & want) == want)
                return i;
        }
        return UINT32_MAX;
    };

    // --- 1x1 RGBA8 dummy image: используется и как SampledImage (t*), и как StorageImage (u*) ---
    {
        VkImageCreateInfo ii{};
        ii.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ii.imageType     = VK_IMAGE_TYPE_2D;
        ii.format        = VK_FORMAT_R8G8B8A8_UNORM;
        ii.extent        = { 1, 1, 1 };
        ii.mipLevels     = 1;
        ii.arrayLayers   = 1;
        ii.samples       = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling        = VK_IMAGE_TILING_OPTIMAL;
        ii.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ii.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkCheck(vkCreateImage(_vkDevice, &ii, nullptr, &_dummyImage), "vkCreateImage (dummy)");

        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(_vkDevice, _dummyImage, &req);
        const u32 typeIdx = pickMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        R_ASSERT2(typeIdx != UINT32_MAX, "No device-local memory for dummy image");

        VkMemoryAllocateInfo ai{};
        ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize  = req.size;
        ai.memoryTypeIndex = typeIdx;
        VkCheck(vkAllocateMemory(_vkDevice, &ai, nullptr, &_dummyImageMem), "vkAllocateMemory (dummy image)");
        VkCheck(vkBindImageMemory(_vkDevice, _dummyImage, _dummyImageMem, 0), "vkBindImageMemory (dummy)");

        // One-shot upload:
        //   UNDEFINED -> TRANSFER_DST_OPTIMAL -> clear white -> GENERAL
        // Final layout is GENERAL because the same image is bound both as SampledImage (t*)
        // and StorageImage (u*) writes it with VK_IMAGE_LAYOUT_GENERAL.
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;

        VkCommandPoolCreateInfo pci{};
        pci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.queueFamilyIndex = _queueFamilies.Graphics;
        VkCheck(vkCreateCommandPool(_vkDevice, &pci, nullptr, &pool), "vkCreateCommandPool (dummy)");

        VkCommandBufferAllocateInfo cbi{};
        cbi.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbi.commandPool        = pool;
        cbi.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbi.commandBufferCount = 1;
        VkCheck(vkAllocateCommandBuffers(_vkDevice, &cbi, &cmd), "vkAllocateCommandBuffers (dummy)");

        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VkCheck(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer (dummy)");

        const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

        // 1) UNDEFINED -> TRANSFER_DST_OPTIMAL
        VkImageMemoryBarrier2 b2{};
        b2.sType             = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b2.oldLayout         = VK_IMAGE_LAYOUT_UNDEFINED;
        b2.newLayout         = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b2.srcStageMask      = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        b2.srcAccessMask     = VK_ACCESS_2_NONE;
        b2.dstStageMask      = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        b2.dstAccessMask     = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b2.image             = _dummyImage;
        b2.subresourceRange  = range;

        VkDependencyInfo di{};
        di.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        di.imageMemoryBarrierCount = 1;
        di.pImageMemoryBarriers    = &b2;
        vkCmdPipelineBarrier2(cmd, &di);

        // 2) Clear in TRANSFER_DST_OPTIMAL
        VkClearColorValue white{};
        white.float32[0] = white.float32[1] = white.float32[2] = white.float32[3] = 1.f;
        vkCmdClearColorImage(cmd, _dummyImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1, &range);

        // 3) TRANSFER_DST_OPTIMAL -> GENERAL
        b2 = {};
        b2.sType             = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b2.oldLayout         = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b2.newLayout         = VK_IMAGE_LAYOUT_GENERAL;
        b2.srcStageMask      = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        b2.srcAccessMask     = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b2.dstStageMask      = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        b2.dstAccessMask     = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
        b2.image             = _dummyImage;
        b2.subresourceRange  = range;

        di = {};
        di.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        di.imageMemoryBarrierCount = 1;
        di.pImageMemoryBarriers    = &b2;
        vkCmdPipelineBarrier2(cmd, &di);

        VkCheck(vkEndCommandBuffer(cmd), "vkEndCommandBuffer (dummy)");

        VkCommandBufferSubmitInfo cbsi{};
        cbsi.sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cbsi.commandBuffer = cmd;

        VkSubmitInfo2 si{};
        si.sType                  = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        si.commandBufferInfoCount = 1;
        si.pCommandBufferInfos    = &cbsi;
        VkCheck(vkQueueSubmit2(_graphicsVkQueue, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit2 (dummy)");
        vkDeviceWaitIdle(_vkDevice);

        vkDestroyCommandPool(_vkDevice, pool, nullptr);

        VkImageViewCreateInfo vi{};
        vi.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image    = _dummyImage;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format   = VK_FORMAT_R8G8B8A8_UNORM;
        vi.subresourceRange = range;
        VkCheck(vkCreateImageView(_vkDevice, &vi, nullptr, &_dummyImageView), "vkCreateImageView (dummy)");
    }

    // --- Dummy UBO (4096 байт) ---
    {
        VkBufferCreateInfo bi{};
        bi.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size        = 4096;
        bi.usage       = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkCheck(vkCreateBuffer(_vkDevice, &bi, nullptr, &_dummyUBO), "vkCreateBuffer (dummy UBO)");

        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(_vkDevice, _dummyUBO, &req);
        const u32 typeIdx = pickMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        R_ASSERT2(typeIdx != UINT32_MAX, "No host-visible memory for dummy UBO");

        VkMemoryAllocateInfo ai{};
        ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize  = req.size;
        ai.memoryTypeIndex = typeIdx;
        VkCheck(vkAllocateMemory(_vkDevice, &ai, nullptr, &_dummyUBOMem), "vkAllocateMemory (dummy UBO)");
        VkCheck(vkBindBufferMemory(_vkDevice, _dummyUBO, _dummyUBOMem, 0), "vkBindBufferMemory (dummy UBO)");

        void* mapped = nullptr;
        VkCheck(vkMapMemory(_vkDevice, _dummyUBOMem, 0, VK_WHOLE_SIZE, 0, &mapped), "vkMapMemory (dummy UBO)");
        memset(mapped, 0, 4096);
        vkUnmapMemory(_vkDevice, _dummyUBOMem);
    }

    // --- Dummy SSBO (256 байт) — используется и для geometry (t57..t63), и для user-structured (t64..t71) ---
    {
        VkBufferCreateInfo bi{};
        bi.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size        = 256;
        bi.usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkCheck(vkCreateBuffer(_vkDevice, &bi, nullptr, &_dummySSBO), "vkCreateBuffer (dummy SSBO)");

        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(_vkDevice, _dummySSBO, &req);
        const u32 typeIdx = pickMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        R_ASSERT2(typeIdx != UINT32_MAX, "No host-visible memory for dummy SSBO");

        VkMemoryAllocateInfo ai{};
        ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize  = req.size;
        ai.memoryTypeIndex = typeIdx;
        VkCheck(vkAllocateMemory(_vkDevice, &ai, nullptr, &_dummySSBOMem), "vkAllocateMemory (dummy SSBO)");
        VkCheck(vkBindBufferMemory(_vkDevice, _dummySSBO, _dummySSBOMem, 0), "vkBindBufferMemory (dummy SSBO)");

        void* mapped = nullptr;
        VkCheck(vkMapMemory(_vkDevice, _dummySSBOMem, 0, VK_WHOLE_SIZE, 0, &mapped), "vkMapMemory (dummy SSBO)");
        memset(mapped, 0, 256);
        vkUnmapMemory(_vkDevice, _dummySSBOMem);
    }

    // --- Dummy sampler ---
    {
        VkSamplerCreateInfo si{};
        si.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter    = VK_FILTER_LINEAR;
        si.minFilter    = VK_FILTER_LINEAR;
        si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod       = VK_LOD_CLAMP_NONE;
        VkCheck(vkCreateSampler(_vkDevice, &si, nullptr, &_dummySampler), "vkCreateSampler (dummy)");
    }
	
	// --- _cbDrawUBO (96 байт, CBDraw slot 13) ---
	{
    	VkBufferCreateInfo bi{};
    	bi.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    	bi.size        = 256;   // выровнено
    	bi.usage       = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    	bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    	VkCheck(vkCreateBuffer(_vkDevice, &bi, nullptr, &_cbDrawUBO), "vkCreateBuffer (_cbDrawUBO)");

    	VkMemoryRequirements req{};
    	vkGetBufferMemoryRequirements(_vkDevice, _cbDrawUBO, &req);
    	const u32 typeIdx = pickMemory(req.memoryTypeBits,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    	R_ASSERT2(typeIdx != UINT32_MAX, "No host-visible memory for _cbDrawUBO");

    	VkMemoryAllocateInfo ai{};
    	ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    	ai.allocationSize  = req.size;
    	ai.memoryTypeIndex = typeIdx;
    	VkCheck(vkAllocateMemory(_vkDevice, &ai, nullptr, &_cbDrawUBOMem), "vkAllocateMemory (_cbDrawUBO)");
    	VkCheck(vkBindBufferMemory(_vkDevice, _cbDrawUBO, _cbDrawUBOMem, 0), "vkBindBufferMemory (_cbDrawUBO)");

    	VkCheck(vkMapMemory(_vkDevice, _cbDrawUBOMem, 0, VK_WHOLE_SIZE, 0, &_cbDrawMapped),
			"vkMapMemory (_cbDrawUBO)");
    	memset(_cbDrawMapped, 0, 256);
    	_cbDrawDirty = true;
	}
	
		// --- Dummy 3D image (s_blue_noise / прочие Texture3D) ---
	{
		VkImageCreateInfo ii{};
		ii.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		ii.imageType     = VK_IMAGE_TYPE_3D;
		ii.format        = VK_FORMAT_R8G8B8A8_UNORM;
		ii.extent        = { 1, 1, 1 };
		ii.mipLevels     = 1;
		ii.arrayLayers   = 1;
		ii.samples       = VK_SAMPLE_COUNT_1_BIT;
		ii.tiling        = VK_IMAGE_TILING_OPTIMAL;
		ii.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		ii.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
		ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		VkCheck(vkCreateImage(_vkDevice, &ii, nullptr, &_dummyImage3D), "vkCreateImage (dummy 3D)");

		VkMemoryRequirements req{};
		vkGetImageMemoryRequirements(_vkDevice, _dummyImage3D, &req);
		const u32 typeIdx = pickMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		R_ASSERT2(typeIdx != UINT32_MAX, "No device-local memory for dummy 3D image");

		VkMemoryAllocateInfo ai{};
		ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		ai.allocationSize  = req.size;
		ai.memoryTypeIndex = typeIdx;
		VkCheck(vkAllocateMemory(_vkDevice, &ai, nullptr, &_dummyImage3DMem), "vkAllocateMemory (dummy 3D)");
		VkCheck(vkBindImageMemory(_vkDevice, _dummyImage3D, _dummyImage3DMem, 0), "vkBindImageMemory (dummy 3D)");

		// one-shot: UNDEFINED -> TRANSFER_DST -> clear -> SHADER_READ_ONLY_OPTIMAL
		VkCommandPool pool = VK_NULL_HANDLE;
		VkCommandBuffer cmd = VK_NULL_HANDLE;
		VkCommandPoolCreateInfo pci{};
		pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		pci.queueFamilyIndex = _queueFamilies.Graphics;
		vkCreateCommandPool(_vkDevice, &pci, nullptr, &pool);
		VkCommandBufferAllocateInfo cbi{};
		cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		cbi.commandPool = pool;
		cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cbi.commandBufferCount = 1;
		vkAllocateCommandBuffers(_vkDevice, &cbi, &cmd);
		VkCommandBufferBeginInfo bi{};
		bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer(cmd, &bi);

		const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		VkImageMemoryBarrier2 b2{};
		b2.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
		b2.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		b2.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		b2.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
		b2.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
		b2.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
		b2.image = _dummyImage3D;
		b2.subresourceRange = range;
		VkDependencyInfo di{};
		di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
		di.imageMemoryBarrierCount = 1;
		di.pImageMemoryBarriers = &b2;
		vkCmdPipelineBarrier2(cmd, &di);

		VkClearColorValue white{};
		white.float32[0] = white.float32[1] = white.float32[2] = white.float32[3] = 1.f;
		vkCmdClearColorImage(cmd, _dummyImage3D, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1, &range);

		b2 = {};
		b2.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
		b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		b2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		b2.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
		b2.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
		b2.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
		b2.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
		b2.image = _dummyImage3D;
		b2.subresourceRange = range;
		di = {};
		di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
		di.imageMemoryBarrierCount = 1;
		di.pImageMemoryBarriers = &b2;
		vkCmdPipelineBarrier2(cmd, &di);

		vkEndCommandBuffer(cmd);
		VkCommandBufferSubmitInfo cbsi{};
		cbsi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
		cbsi.commandBuffer = cmd;
		VkSubmitInfo2 si{};
		si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
		si.commandBufferInfoCount = 1;
		si.pCommandBufferInfos = &cbsi;
		vkQueueSubmit2(_graphicsVkQueue, 1, &si, VK_NULL_HANDLE);
		vkDeviceWaitIdle(_vkDevice);
		vkDestroyCommandPool(_vkDevice, pool, nullptr);

		VkImageViewCreateInfo vi{};
		vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		vi.image = _dummyImage3D;
		vi.viewType = VK_IMAGE_VIEW_TYPE_3D;
		vi.format = VK_FORMAT_R8G8B8A8_UNORM;
		vi.subresourceRange = range;
		VkCheck(vkCreateImageView(_vkDevice, &vi, nullptr, &_dummyImageView3D), "vkCreateImageView (dummy 3D)");
	}
	
	// --- Per-slot UBO для CBView / CBFrame / CBModel / ... (slots 0..12) ---
	for (u32 slot = 0; slot < 13; ++slot)
	{
		VkBufferCreateInfo bi{};
		bi.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bi.size        = kCbSlotSize;
		bi.usage       = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
		bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		VkCheck(vkCreateBuffer(_vkDevice, &bi, nullptr, &_cbUBO[slot]), "vkCreateBuffer (_cbUBO)");

		VkMemoryRequirements req{};
		vkGetBufferMemoryRequirements(_vkDevice, _cbUBO[slot], &req);
		const u32 typeIdx = pickMemory(req.memoryTypeBits,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		R_ASSERT2(typeIdx != UINT32_MAX, "No host-visible memory for _cbUBO");

		VkMemoryAllocateInfo ai{};
		ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		ai.allocationSize  = req.size;
		ai.memoryTypeIndex = typeIdx;
		VkCheck(vkAllocateMemory(_vkDevice, &ai, nullptr, &_cbUBOMem[slot]), "vkAllocateMemory (_cbUBO)");
		VkCheck(vkBindBufferMemory(_vkDevice, _cbUBO[slot], _cbUBOMem[slot], 0), "vkBindBufferMemory (_cbUBO)");
		VkCheck(vkMapMemory(_vkDevice, _cbUBOMem[slot], 0, VK_WHOLE_SIZE, 0, &_cbUBOMapped[slot]),
				"vkMapMemory (_cbUBO)");
		memset(_cbUBOMapped[slot], 0, kCbSlotSize);
		_cbUBOSize[slot] = 0;
	}

    Msg("* [VK] Dummy resources created");
}

void InternalDeviceVK::InitPushDescriptorWrites()
{
	_pushCbInfo.assign(СVKBindings::CBCount,        { _dummyUBO, 0, VK_WHOLE_SIZE });
	_pushSrvInfo.assign(СVKBindings::SrvCount,      { VK_NULL_HANDLE, _dummyImageView, VK_IMAGE_LAYOUT_GENERAL });
	_pushSsboInfo.assign(СVKBindings::SsboCount,    { _dummySSBO, 0, VK_WHOLE_SIZE });
	_pushSamplerInfo.assign(СVKBindings::SamplerCount, { _dummySampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED });
	_pushUavInfo.assign(СVKBindings::UavCount,      { VK_NULL_HANDLE, _dummyImageView, VK_IMAGE_LAYOUT_GENERAL });
	_pushUserSsboInfo.assign(СVKBindings::UserSsboCount, { _dummySSBO, 0, VK_WHOLE_SIZE });

	_pushWrites.clear();
	_pushWrites.reserve(СVKBindings::TotalBindings);

	auto addWrite = [&](u32 binding, VkDescriptorType type,
	                    const VkDescriptorBufferInfo* buf,
	                    const VkDescriptorImageInfo* img)
	{
		VkWriteDescriptorSet w{};
		w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		w.dstSet          = VK_NULL_HANDLE; // игнорируется в push
		w.dstBinding      = binding;
		w.dstArrayElement = 0;
		w.descriptorCount = 1;
		w.descriptorType  = type;
		w.pBufferInfo     = buf;
		w.pImageInfo      = img;
		_pushWrites.push_back(w);
	};

	for (u32 i = 0; i < СVKBindings::CBCount;       ++i) addWrite(СVKBindings::CBBase       + i, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,  &_pushCbInfo[i],       nullptr);
	for (u32 i = 0; i < СVKBindings::SrvCount;      ++i) addWrite(СVKBindings::SrvBase      + i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,   nullptr, &_pushSrvInfo[i]);
	for (u32 i = 0; i < СVKBindings::SsboCount;     ++i) addWrite(СVKBindings::SsboBase     + i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,  &_pushSsboInfo[i],     nullptr);
	for (u32 i = 0; i < СVKBindings::SamplerCount;  ++i) addWrite(СVKBindings::SamplerBase  + i, VK_DESCRIPTOR_TYPE_SAMPLER,         nullptr, &_pushSamplerInfo[i]);
	for (u32 i = 0; i < СVKBindings::UavCount;      ++i) addWrite(СVKBindings::UavBase      + i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,   nullptr, &_pushUavInfo[i]);
	for (u32 i = 0; i < СVKBindings::UserSsboCount; ++i) addWrite(СVKBindings::UserSsboBase + i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,  &_pushUserSsboInfo[i], nullptr);

	// --- s_blue_noise (t36) — 3D-текстура ---
	// 2D dummy по умолчанию лежит в GENERAL, 3D dummy после clear'а —
	// в SHADER_READ_ONLY_OPTIMAL. Layout должен совпадать, иначе VUID-vkCmdDraw-imageLayout.
	_pushSrvInfo[36].imageView   = _dummyImageView3D;
	_pushSrvInfo[36].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	_pushSrvInfo[36].sampler     = VK_NULL_HANDLE;
	
	Msg("* [VK] Push descriptor writes: %u", (u32)_pushWrites.size());
}

void InternalDeviceVK::UpdatePipelineLayout()
{
	PROF_EVENT("Vulkan: UpdatePipelineLayout");
	if (_uiPipelineLayout != VK_NULL_HANDLE)
	{
		vkDestroyPipelineLayout(_vkDevice, _uiPipelineLayout, nullptr);
		_uiPipelineLayout = VK_NULL_HANDLE;
	}
	InvalidatePipelineCache();

	VkPipelineLayoutCreateInfo info{};
	info.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	info.setLayoutCount = 1;
	info.pSetLayouts    = &_setLayout;
	VkCheck(vkCreatePipelineLayout(_vkDevice, &info, nullptr, &_uiPipelineLayout),
			"vkCreatePipelineLayout (UI)");
}

void InternalDeviceVK::DestroyDescriptorObjects()
{
	if (_vkDevice == VK_NULL_HANDLE) return;

	// 1) Per-frame пулы (Шаг 2b) — до любых set'ов/layout'ов
	for (auto& pool : _frameDescriptorPools)
	{
		if (pool != VK_NULL_HANDLE)
		{
			vkDestroyDescriptorPool(_vkDevice, pool, nullptr);
			pool = VK_NULL_HANDLE;
		}
	}

	// 2) Остальные dummy-объекты
	if (_dummySampler    != VK_NULL_HANDLE) vkDestroySampler   (_vkDevice, _dummySampler,    nullptr);
	if (_dummySSBO       != VK_NULL_HANDLE) vkDestroyBuffer    (_vkDevice, _dummySSBO,       nullptr);
	if (_dummySSBOMem    != VK_NULL_HANDLE) vkFreeMemory       (_vkDevice, _dummySSBOMem,    nullptr);
	if (_dummyUBO        != VK_NULL_HANDLE) vkDestroyBuffer    (_vkDevice, _dummyUBO,        nullptr);
	if (_dummyUBOMem     != VK_NULL_HANDLE) vkFreeMemory       (_vkDevice, _dummyUBOMem,     nullptr);
	if (_dummyImageView  != VK_NULL_HANDLE) vkDestroyImageView (_vkDevice, _dummyImageView,  nullptr);
	if (_dummyImage      != VK_NULL_HANDLE) vkDestroyImage     (_vkDevice, _dummyImage,      nullptr);
	if (_dummyImageMem   != VK_NULL_HANDLE) vkFreeMemory       (_vkDevice, _dummyImageMem,   nullptr);

	// 3) Старый pool (после перехода на per-frame он = VK_NULL_HANDLE,
	//    проверка страхует на случай отката)
	if (_descriptorPool  != VK_NULL_HANDLE) vkDestroyDescriptorPool     (_vkDevice, _descriptorPool, nullptr);

	// 4) Layout — последним, после всех set'ов/пулов
	if (_setLayout       != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(_vkDevice, _setLayout,      nullptr);
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
	key.inputLayoutId = reinterpret_cast<u64>(_inputLayout);

    // Формат RTV: из swapchain-формата, если RTV == SwapChainRTV (мы сейчас не различаем RTV по объектам —
    // в 2.5 сделаем CVKRenderTargetView с реальным VkFormat, сейчас хак для MVP)
    for (u32 i = 0; i < _rtvCount; ++i)
        key.rtvFormats[i] = _swapchainFormat;

    const u64 hash = HashPipelineKey(key);
    auto it = _pipelineCache.find(hash);
    if (it != _pipelineCache.end())
        return it->second;

    // --- Создание layout (один на все UI pipeline) ---
	R_ASSERT2(_uiPipelineLayout != VK_NULL_HANDLE, "UI pipeline layout not created");

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
	
	VkPipelineDepthStencilStateCreateInfo ds = {};
	ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	ds.depthTestEnable  = VK_FALSE;
	ds.depthWriteEnable = VK_FALSE;
	ds.depthCompareOp   = VK_COMPARE_OP_ALWAYS;
	ds.stencilTestEnable = VK_FALSE;
	
	VkPipelineTessellationStateCreateInfo tess = {};
	tess.sType             = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO;
	tess.patchControlPoints = 3;

	info.pTessellationState = key.hs || key.ds ? &tess : nullptr;
	info.pDepthStencilState = &ds;

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
	VkRect2D sc = _hasScissor
		? VkRect2D{ { _scissor.x1, _scissor.y1 }, { (u32)(_scissor.x2 - _scissor.x1), (u32)(_scissor.y2 - _scissor.y1) } }
	: VkRect2D{ { 0, 0 }, _swapchainExtent };
	vkCmdSetScissor(cmd, 0, 1, &sc);

	FlushCbDraw();
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	R_ASSERT2(_frameDescriptorSetUsed[_frame] < 2048,
			  "Descriptor pool exhausted: больше 2048 draw / кадр");
	VkDescriptorSetAllocateInfo alloc{};
	alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	alloc.descriptorPool     = _frameDescriptorPools[_frame];
	alloc.descriptorSetCount = 1;
	alloc.pSetLayouts        = &_setLayout;
	VkDescriptorSet set = VK_NULL_HANDLE;
	VkCheck(vkAllocateDescriptorSets(_vkDevice, &alloc, &set), "vkAllocateDescriptorSets");
	_frameDescriptorSetUsed[_frame]++;

	FillDescriptorSet(set);

	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
							_uiPipelineLayout, 0, 1, &set, 0, nullptr);

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

	FlushCbDraw();
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	R_ASSERT2(_frameDescriptorSetUsed[_frame] < 2048,
			  "Descriptor pool exhausted: больше 2048 draw / кадр");
	VkDescriptorSetAllocateInfo alloc{};
	alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	alloc.descriptorPool     = _frameDescriptorPools[_frame];
	alloc.descriptorSetCount = 1;
	alloc.pSetLayouts        = &_setLayout;
	VkDescriptorSet set = VK_NULL_HANDLE;
	VkCheck(vkAllocateDescriptorSets(_vkDevice, &alloc, &set), "vkAllocateDescriptorSets");
	_frameDescriptorSetUsed[_frame]++;

	FillDescriptorSet(set);

	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
							_uiPipelineLayout, 0, 1, &set, 0, nullptr);

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

	FlushCbDraw();
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	R_ASSERT2(_frameDescriptorSetUsed[_frame] < 2048,
			  "Descriptor pool exhausted: больше 2048 draw / кадр");
	VkDescriptorSetAllocateInfo alloc{};
	alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	alloc.descriptorPool     = _frameDescriptorPools[_frame];
	alloc.descriptorSetCount = 1;
	alloc.pSetLayouts        = &_setLayout;
	VkDescriptorSet set = VK_NULL_HANDLE;
	VkCheck(vkAllocateDescriptorSets(_vkDevice, &alloc, &set), "vkAllocateDescriptorSets");
	_frameDescriptorSetUsed[_frame]++;

	FillDescriptorSet(set);

	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
							_uiPipelineLayout, 0, 1, &set, 0, nullptr);

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

	FlushCbDraw();
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	R_ASSERT2(_frameDescriptorSetUsed[_frame] < 2048,
			  "Descriptor pool exhausted: больше 2048 draw / кадр");
	VkDescriptorSetAllocateInfo alloc{};
	alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	alloc.descriptorPool     = _frameDescriptorPools[_frame];
	alloc.descriptorSetCount = 1;
	alloc.pSetLayouts        = &_setLayout;
	VkDescriptorSet set = VK_NULL_HANDLE;
	VkCheck(vkAllocateDescriptorSets(_vkDevice, &alloc, &set), "vkAllocateDescriptorSets");
	_frameDescriptorSetUsed[_frame]++;

	FillDescriptorSet(set);

	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
							_uiPipelineLayout, 0, 1, &set, 0, nullptr);

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
	_rtvCount = 0;
	for (u32 i = 0; i < NumViews && i < 8; ++i)
	{
		_rtvs[i] = ppRenderTargetViews ? ppRenderTargetViews[i] : nullptr;
		if (_rtvs[i]) {
			_rtvCount = i + 1;
		}
	}
	for (u32 i = _rtvCount; i < 8; ++i) {
		_rtvs[i] = nullptr;
	}
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
		if (!Writes[i].Data || !Writes[i].Size) continue;

		if (slot == 13)
		{
			// CBDraw — per-draw, обновляем _cbDrawUBO
			if (Writes[i].Size <= sizeof(_drawConstants))
			{
				memcpy(_drawConstants, Writes[i].Data, Writes[i].Size);
				_cbDrawDirty = true;
			}
		}
		else if (slot < 13 && Writes[i].Size <= kCbSlotSize)
		{
			// CBView / CBFrame / CBModel / ... — per-frame, пишем в свой UBO
			memcpy(_cbUBOMapped[slot], Writes[i].Data, Writes[i].Size);
			_cbUBOSize[slot] = Writes[i].Size;
		}
	}
}

void InternalDeviceVK::FillDescriptorSet(VkDescriptorSet set)
{
	auto* cache = static_cast<VkResourceCache*>(GRHI->ShaderResourceCache);
	if (!cache) return;

	// CB0..12 = CBView / CBFrame / CBModel / ... — свои UBO
	for (u32 slot = 0; slot < 13; ++slot)
	{
		_pushCbInfo[slot] = { _cbUBO[slot], 0, VK_WHOLE_SIZE };
	}
	// CB13 = CBDraw — per-draw UBO
	_pushCbInfo[13] = { _cbDrawUBO, 0, VK_WHOLE_SIZE };

	// SSBO (t57..t63 → VK bindings 71..77): из cache по slot = 57 + i
	for (u32 i = 0; i < СVKBindings::SsboCount; ++i)
	{
		_pushSsboInfo[i] = { _dummySSBO, 0, VK_WHOLE_SIZE };  // reset к dummy
		const u32 cacheSlot = СVKBindings::SrvCount + i;       // 57..63
		for (u32 stg = 0; stg < 6; ++stg)
		{
			auto* res = cache->Get(stg, cacheSlot);
			if (!res) continue;
			auto* vkSrv = dynamic_cast<CVKShaderResourceView*>(res);
			if (vkSrv)
			{
				_pushSsboInfo[i] = vkSrv->GetBufferInfo();
				break;
			}
		}
	}
	
	for (u32 i = 0; i < СVKBindings::SrvCount; ++i)
	{
		_pushSrvInfo[i].sampler     = VK_NULL_HANDLE;
		_pushSrvInfo[i].imageView   = _dummyImageView;
		_pushSrvInfo[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

		// Ищем по всем стадиям
		for (u32 stg = 0; stg < 6; ++stg)
		{
			auto* res = cache->Get(stg, i);
			if (!res) continue;
			if (auto* texSrv = dynamic_cast<CVKShaderResourceViewTexture*>(res))
			{
				_pushSrvInfo[i] = texSrv->GetImageInfo();
				break;
			}
		}
	}
	// Оставляем t36 как 3D (s_blue_noise) — но если в cache есть реальная 3D, она перезапишет
	if (_dummyImageView3D != VK_NULL_HANDLE && _pushSrvInfo[36].imageView == _dummyImageView)
	{
		_pushSrvInfo[36].imageView   = _dummyImageView3D;
		_pushSrvInfo[36].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	}

	// Собираем write'ы (используем _pushWrites из InitPushDescriptorWrites,
	// но dstSet будет наш set)
	for (auto& w : _pushWrites)
	{
		w.dstSet = set;
	}

	vkUpdateDescriptorSets(_vkDevice,
		static_cast<u32>(_pushWrites.size()), _pushWrites.data(),
		0, nullptr);
}

void InternalDeviceVK::FlushCbDraw()
{
	if (!_cbDrawDirty || !_cbDrawMapped) return;
	memcpy(_cbDrawMapped, _drawConstants, sizeof(_drawConstants));
	_cbDrawDirty = false;
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
	
	if (Width == _swapchainExtent.width &&
		Height == _swapchainExtent.height &&
		_vkSwapchain != VK_NULL_HANDLE)
	{
		// psCurrentVidMode/HalfTarget уже корректны из конструктора
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
	
	// if ((_frame % 60) == 0 && (g_drawCalls || g_drawSkipped))
	if ((_frameSerial % 60) == 0 && (g_drawCalls || g_drawSkipped))
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

IRHITextureFactory* InternalDeviceVK::GetTextureFactory()
{
	return _textureFactory;
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
	
	// СНАЧАЛА ждём fence — гарантируем, что GPU закончил с предыдущим использованием кадра
	VkCheck(vkWaitForFences(_vkDevice, 1, &f.InFlight, VK_TRUE, UINT64_MAX), "vkWaitForFences");
	VkCheck(vkResetFences(_vkDevice, 1, &f.InFlight), "vkResetFences");

	// ТЕПЕРЬ безопасно сбросить пул
	VkCheck(vkResetDescriptorPool(_vkDevice, _frameDescriptorPools[_frame], 0), "vkResetDescriptorPool");
	_frameDescriptorSetUsed[_frame] = 0;

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
	colorAttachment.clearValue.color = { { 0.0f, 0.0f, 0.0f, 1.0f } };

	VkRenderingInfo renderInfo{};
	renderInfo.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
	renderInfo.renderArea           = { { 0, 0 }, _swapchainExtent };
	renderInfo.layerCount           = 1;
	renderInfo.colorAttachmentCount = 1;
	renderInfo.pColorAttachments    = &colorAttachment;

	vkCmdBeginRendering(f.CommandBuffer, &renderInfo);
}

IRHIShaderResourceView* InternalDeviceVK::CreateShaderResourceView(IRHIBuffer* Buffer, const RHIShaderResourceViewDesc* desc)
{
	if (!Buffer)
	{
		Msg("! [VK] CreateShaderResourceView(IRHIBuffer*): null buffer, returning stub");
		return new VkStubSRV();
	}
	auto* vkBuf = static_cast<CVKBuffer*>(Buffer);
	return new CVKShaderResourceView(vkBuf, desc);
}

IRHIUnorderedAccessView* InternalDeviceVK::CreateUAV(IRHIBuffer* Buffer, const RHIUAVDesc& Desc)
{
	if (!Buffer)
	{
		Msg("! [VK] CreateUAV(IRHIBuffer*): null buffer, returning stub");
		return new VkStubUAV();
	}
	auto* vkBuf = static_cast<CVKBuffer*>(Buffer);
	return new CVKUnorderedAccessView(vkBuf, Desc);
}

HRESULT InternalDeviceVK::LoadDDS(const void* data, size_t size,
	ERHI_USAGE usage, u32 bind_flags, ERHI_CPU_ACCESS_FLAG cpu_flags,
	int& lod, bool fallback, IRHISurface** out_surface)
{
	PROF_EVENT("Vulkan: LoadDDS");
	*out_surface = nullptr;

	if (!data || !size)
	{
		return E_INVALIDARG;
	}
	
	xrCriticalSectionGuard guard(_loadMutex);

	DirectX::ScratchImage image;
	DirectX::TexMetadata metadata = {};
	HRESULT result = DirectX::LoadFromDDSMemory(
		data, size,
		fallback ? DirectX::DDS_FLAGS_NO_16BPP : DirectX::DDS_FLAGS_NONE,
		&metadata, image);
	if (FAILED(result))
	{
		return result;
	}

	// --- Mip-skip (тот же алгоритм, что в D3D12) ---
	size_t firstMip = 0;
	if (!metadata.IsCubemap() && !metadata.IsVolumemap())
	{
		while (lod > 0 && metadata.mipLevels - firstMip > 1 &&
		       (metadata.width >> firstMip) > 4 &&
		       (metadata.height >> firstMip) > 4)
		{
			++firstMip;
			--lod;
		}
	}

	if (metadata.width  > UINT32_MAX || metadata.height > UINT32_MAX ||
	    metadata.depth  > UINT16_MAX || metadata.arraySize > UINT16_MAX)
	{
		return E_INVALIDARG;
	}

	RHITextureDesc desc(
		(u32)(metadata.width  >> firstMip),
		(u32)(metadata.height >> firstMip),
		(ERHI_FORMAT)metadata.format);
	desc.Depth       = (u32)metadata.depth;
	desc.ArraySize   = (u32)metadata.arraySize;
	desc.MipLevels   = (u32)(metadata.mipLevels - firstMip);
	desc.Usage       = usage;
	desc.BindFlags   = (ERHI_BIND_FLAG)bind_flags;
	desc.CPUAccessFlags = (u32)cpu_flags;
	desc.MiscFlags   = (u32)metadata.miscFlags;   // TEX_MISC_TEXTURECUBE == 0x4 == D3D11_RESOURCE_MISC_TEXTURECUBE

	auto* surface = new CVKTexture(_vkDevice, _vkPhysicalDevice,
							   _loadCmdPool, _graphicsVkQueue, desc);

	const bool is3D = metadata.dimension == DirectX::TEX_DIMENSION_TEXTURE3D;
	
	if (!surface->IsReady())
	{
		Msg("! [VK] LoadDDS: CVKTexture creation failed (fmt=%d %ux%u mips=%u layers=%u)",
			(int)desc.Format, desc.Width, desc.Height, desc.MipLevels, desc.ArraySize);
		surface->Release();
		return E_FAIL;
	}
	
	Msg("* [VK] LoadDDS: %ux%u, mips=%u, layers=%u, depth=%u, fmt=%d",
		desc.Width, desc.Height, desc.MipLevels, desc.ArraySize, desc.Depth, (int)desc.Format);

	// Для 3D layer_idx всегда 0 (arraySize == 1), разворачиваем глубину
	// через slicePitch * depth.
	for (u32 layer_idx = 0; layer_idx < desc.ArraySize; ++layer_idx)
	{
		for (u32 mip_idx = 0; mip_idx < desc.MipLevels; ++mip_idx)
		{
			static u32 s_loadCount = 0;
			if (((++s_loadCount) % 32) == 0)
			{
				Msg("~ [VK] LoadDDS progress: %u mip uploads", s_loadCount);
			}
			
			const auto source = image.GetImage(mip_idx + firstMip, layer_idx, 0);
			if (!source || source->rowPitch   > UINT32_MAX ||
			                source->slicePitch > UINT32_MAX)
			{
				surface->Release();
				return E_INVALIDARG;
			}

			const u32 depth = is3D
				? std::max<u32>(1, desc.Depth >> mip_idx)
				: 1;

			RHISubResource subresource;
			subresource.Data       = source->pixels;
			subresource.RowPitch   = (u32)source->rowPitch;
			subresource.DepthPitch = (u32)source->slicePitch;
			// DataSize = полный размер для 3D-стека (или одного слоя для 2D/cube)
			subresource.DataSize   = (u32)(source->slicePitch) * depth;

			if (!surface->UpdateData(mip_idx, layer_idx, &subresource, RHIBox{}))
			{
				Msg("! [VK] LoadDDS: UpdateData failed at mip=%u layer=%u",
				    mip_idx, layer_idx);
				surface->Release();
				return E_FAIL;
			}
		}
	}

	*out_surface = surface;
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

HRESULT InternalDeviceVK::CreateSamplerState(const RHISampleDesc& desc, RHIObject** out_state)
{
	if (!out_state) return E_POINTER;
	*out_state = nullptr;

	VkSamplerCreateInfo si{};
	si.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	si.magFilter    = (desc.Filter & ERHI_FILTER::MIN_MAG_MIP_POINT) == ERHI_FILTER::MIN_MAG_MIP_POINT
		? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
	si.minFilter    = si.magFilter;
	si.mipmapMode   = (desc.Filter & ERHI_FILTER::MIN_MAG_MIP_POINT) == ERHI_FILTER::MIN_MAG_MIP_POINT
		? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
	si.addressModeU = (VkSamplerAddressMode)desc.AddressU;
	si.addressModeV = (VkSamplerAddressMode)desc.AddressV;
	si.addressModeW = (VkSamplerAddressMode)desc.AddressW;
	si.mipLodBias   = desc.MipLODBias;
	si.maxAnisotropy = desc.MaxAnisotropy ? desc.MaxAnisotropy : 1.0f;
	si.anisotropyEnable = desc.MaxAnisotropy > 1;
	si.compareEnable = false;
	si.minLod       = desc.MinLOD;
	si.maxLod       = desc.MaxLOD;

	VkSampler sampler = VK_NULL_HANDLE;
	VkResult r = vkCreateSampler(_vkDevice, &si, nullptr, &sampler);
	if (r != VK_SUCCESS) { Msg("! [VK] vkCreateSampler failed: %d", (int)r); return E_FAIL; }

	// Сохраняем в RHIObject как void*, Release — vkDestroySampler
	struct SamplerHolder { VkDevice dev; VkSampler s; };
	auto* holder = new SamplerHolder{ _vkDevice, sampler };
	auto release = [](void* payload)
	{
		auto* h = static_cast<SamplerHolder*>(payload);
		if (h && h->s != VK_NULL_HANDLE) vkDestroySampler(h->dev, h->s, nullptr);
		xr_delete(h);
	};
	*out_state = new RHIObject(holder, release);
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
	
	if (_loadCmdPool != VK_NULL_HANDLE) vkDestroyCommandPool(_vkDevice, _loadCmdPool, nullptr);

	for (VkImageView v : _swapchainImageViews)
	{
		if (v != VK_NULL_HANDLE) vkDestroyImageView(_vkDevice, v, nullptr);
	}
	_swapchainImageViews.clear();
	if (_vkSwapchain != VK_NULL_HANDLE) vkDestroySwapchainKHR(_vkDevice, _vkSwapchain, nullptr);

	if (_vkPipelineCache != VK_NULL_HANDLE) vkDestroyPipelineCache(_vkDevice, _vkPipelineCache, nullptr);
	
	if (_uiPipelineLayout != VK_NULL_HANDLE)
	{
		vkDestroyPipelineLayout(_vkDevice, _uiPipelineLayout, nullptr);
		_uiPipelineLayout = VK_NULL_HANDLE;
	}
	InvalidatePipelineCache();
	
	if (_cbDrawUBO != VK_NULL_HANDLE)
	{
		vkUnmapMemory(_vkDevice, _cbDrawUBOMem);
		vkDestroyBuffer(_vkDevice, _cbDrawUBO, nullptr);
		vkFreeMemory(_vkDevice, _cbDrawUBOMem, nullptr);
	}
	
	for (u32 slot = 0; slot < 13; ++slot)
	{
		if (_cbUBOMapped[slot]) vkUnmapMemory(_vkDevice, _cbUBOMem[slot]);
		if (_cbUBO[slot]      != VK_NULL_HANDLE) vkDestroyBuffer(_vkDevice, _cbUBO[slot], nullptr);
		if (_cbUBOMem[slot]   != VK_NULL_HANDLE) vkFreeMemory   (_vkDevice, _cbUBOMem[slot], nullptr);
	}
	
	if (_dummyImageView3D != VK_NULL_HANDLE) vkDestroyImageView(_vkDevice, _dummyImageView3D, nullptr);
	if (_dummyImage3D     != VK_NULL_HANDLE) vkDestroyImage    (_vkDevice, _dummyImage3D, nullptr);
	if (_dummyImage3DMem  != VK_NULL_HANDLE) vkFreeMemory      (_vkDevice, _dummyImage3DMem, nullptr);
	
	xr_delete(_textureFactory);
	DestroyDescriptorObjects();

	if (_vkDevice != VK_NULL_HANDLE)
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