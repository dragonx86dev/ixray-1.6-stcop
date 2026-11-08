#include "Utils.h"

#include <vulkan/vulkan_core.h>

bool Utils::CheckVulkanVersion()
{
	uint32_t vulkanApiVersion = 0;
	if (vkEnumerateInstanceVersion(&vulkanApiVersion) == VK_SUCCESS 
		&& vulkanApiVersion < VK_API_VERSION_1_4)
	{
		return false;
	}
	
	return true;
}



