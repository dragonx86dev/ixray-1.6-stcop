#pragma once
#include "../RHI.h"
#include <vulkan/vulkan.h>

// Обёртка над VkShaderModule (или DXBC-проверкой).
// isSpirV = false → шейдер не готов, pipeline не создаётся, Draw скипается.
struct CVKShader
{
	VkDevice         device  = VK_NULL_HANDLE;
	VkShaderModule   module  = VK_NULL_HANDLE;
	ERHI_SHADER_TYPE type    = ERHI_SHADER_TYPE::VS;
	bool             isSpirV = false;

	CVKShader(VkDevice dev, VkShaderModule mod, ERHI_SHADER_TYPE t, bool spirv)
		: device(dev), module(mod), type(t), isSpirV(spirv) {}

	~CVKShader()
	{
		if (module != VK_NULL_HANDLE && device != VK_NULL_HANDLE)
			vkDestroyShaderModule(device, module, nullptr);
	}
};