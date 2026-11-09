#pragma once
#include "../RHI.h"
#include <vulkan/vulkan.h>
#include <atomic>

// Реальная обёртка над VkBuffer + VkDeviceMemory.
// MVP-путь: HOST_VISIBLE | HOST_COHERENT, персистентный мап, memcpy + flush.
// Стиль именования по аналогии с CD3D11Buffer.
class CVKBuffer final : public IRHIBuffer
{
public:
	CVKBuffer(VkDevice device, VkPhysicalDevice physicalDevice,
			  const RHIBufferDesc& desc, const RHIBufferSubresource* data);
	~CVKBuffer() override;

	// IRHIBuffer
	void AddRef() override;
	u32  Release() override;
	u32  GetSize() const override;

	bool Map(ERHI_BUFFER_MAP MapType, u32 MapFlags, RHIMappedSubresource* out) override;
	void Unmap() override;
	void UpdateSubresource(const void* data, u32 size, u32 offset = 0) override;
	void SetVertexBuffer(u32 slot, u32 stride, u32 offset) override;
	void SetIndexBuffer(bool is32bit, u32 offset) override;

	// Для внутреннего использования (descriptor set, bind в Draw)
	VkBuffer        GetHandle()       const { return _buffer; }
	VkDeviceSize    GetDeviceSize()   const { return _size; }
	VkDescriptorBufferInfo GetDescriptorInfo() const;

	// Сохранённое состояние bind (используется в 2.3+)
	u32  GetVBSlot()   const { return _vbSlot; }
	u32  GetVBStride() const { return _vbStride; }
	u32  GetVBOffset() const { return _vbOffset; }
	bool GetIBIs32Bit() const { return _ibIs32Bit; }
	u32  GetIBOffset()  const { return _ibOffset; }

private:
	VkDevice        _device     = VK_NULL_HANDLE;
	VkBuffer        _buffer     = VK_NULL_HANDLE;
	VkDeviceMemory  _memory     = VK_NULL_HANDLE;
	void*           _mapped     = nullptr;
	VkDeviceSize    _size       = 0;
	RHIBufferDesc   _desc       = {};
	std::atomic<u32> _refs{ 1 };
	bool            _isCoherent = false;

	// bind state (пока для будущего)
	u32  _vbSlot    = 0;
	u32  _vbStride  = 0;
	u32  _vbOffset  = 0;
	bool _ibIs32Bit = false;
	u32  _ibOffset  = 0;
};

class CVKUnorderedAccessView final : public IRHIUnorderedAccessView
{
public:
	CVKUnorderedAccessView(CVKBuffer* buf, const RHIUAVDesc& desc);
	~CVKUnorderedAccessView() override;

	void* GetRaw() override { return this; }
	void  AddRef() override;
	u32   Release() override;

	const VkDescriptorBufferInfo& GetBufferInfo()     const { return _info; }
	VkDescriptorType              GetDescriptorType() const { return _type; }
	CVKBuffer*                    GetBuffer()         const { return _buffer; }

private:
	CVKBuffer* _buffer = nullptr;
	VkDescriptorBufferInfo _info{};
	VkDescriptorType _type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	std::atomic<u32> _refs{ 1 };
};