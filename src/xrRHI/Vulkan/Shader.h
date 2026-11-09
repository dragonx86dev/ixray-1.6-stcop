#pragma once
#include "../RHI.h"
#include <vulkan/vulkan.h>

#include "Buffer.h"

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

// ---------------------------------------------------------------------------
// SRV/UAV для VkBuffer (Шаг 1).
//
// Формируют VkDescriptorBufferInfo, пригодный для vkCmdPushDescriptorSet
// (Шаг 2) или vkUpdateDescriptorSets. Формат пока игнорируется: для буферов
// всегда STORAGE_BUFFER (StructuredBuffer / ByteAddressBuffer). Typed buffer
// (Buffer<float4>) — отдельная ветка, добавим когда/если валидатор попросит.
// ---------------------------------------------------------------------------
class CVKShaderResourceView final : public IRHIShaderResourceView
{
public:
	CVKShaderResourceView(CVKBuffer* buf, const RHIShaderResourceViewDesc* desc);
	~CVKShaderResourceView() override;

	void* GetRawSRV() override { return this; }
	void  AddRef() override;
	u32   Release() override;

	// Для Шага 2 (vkCmdPushDescriptorSet / vkUpdateDescriptorSets)
	const VkDescriptorBufferInfo& GetBufferInfo()     const { return _info; }
	VkDescriptorType              GetDescriptorType() const { return _type; }
	CVKBuffer*                    GetBuffer()         const { return _buffer; }

private:
	CVKBuffer* _buffer = nullptr;
	VkDescriptorBufferInfo _info{};
	VkDescriptorType _type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	std::atomic<u32> _refs{ 1 };
};