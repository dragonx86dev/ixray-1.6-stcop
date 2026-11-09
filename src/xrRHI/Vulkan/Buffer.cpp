#include "Buffer.h"

CVKBuffer::CVKBuffer(VkDevice device, VkPhysicalDevice physicalDevice,
                     const RHIBufferDesc& desc, const RHIBufferSubresource* data)
    : _device(device), _desc(desc)
{
    _size = desc.Size ? desc.Size : 1;

    VkBufferCreateInfo info = {};
    info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size        = _size;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    // usage по типу (аналог CD3D11Buffer::GetD3D11BindFlags)
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    switch (desc.Type)
    {
    case ERHI_BUFFER_TYPE::VERTEX:      info.usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;   break;
    case ERHI_BUFFER_TYPE::INDEX:       info.usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;    break;
    case ERHI_BUFFER_TYPE::CONSTANT:    info.usage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;  break;
    case ERHI_BUFFER_TYPE::STRUCTURED:
    case ERHI_BUFFER_TYPE::STRUCTURED_RW:
        info.usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        break;
    }

    VkResult r = vkCreateBuffer(_device, &info, nullptr, &_buffer);
    R_ASSERT2(r == VK_SUCCESS, "vkCreateBuffer (CVKBuffer) failed");

    VkMemoryRequirements memReq = {};
    vkGetBufferMemoryRequirements(_device, _buffer, &memReq);

    VkPhysicalDeviceMemoryProperties memProps = {};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);

    const VkMemoryPropertyFlags wantFlags[] = {
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
    };

    u32 memType = UINT32_MAX;
    for (VkMemoryPropertyFlags want : wantFlags)
    {
        for (u32 i = 0; i < memProps.memoryTypeCount; ++i)
        {
            if ((memReq.memoryTypeBits & (1u << i)) &&
                (memProps.memoryTypes[i].propertyFlags & want) == want)
            {
                memType = i;
                break;
            }
        }
        if (memType != UINT32_MAX)
            break;
    }
    R_ASSERT2(memType != UINT32_MAX, "No suitable memory type for CVKBuffer");

    VkMemoryAllocateInfo alloc = {};
    alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize  = memReq.size;
    alloc.memoryTypeIndex = memType;

    r = vkAllocateMemory(_device, &alloc, nullptr, &_memory);
    R_ASSERT2(r == VK_SUCCESS, "vkAllocateMemory (CVKBuffer) failed");

    r = vkBindBufferMemory(_device, _buffer, _memory, 0);
    R_ASSERT2(r == VK_SUCCESS, "vkBindBufferMemory (CVKBuffer) failed");

    r = vkMapMemory(_device, _memory, 0, VK_WHOLE_SIZE, 0, &_mapped);
    R_ASSERT2(r == VK_SUCCESS, "vkMapMemory (CVKBuffer) failed");

    _isCoherent = (memProps.memoryTypes[memType].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;

    if (data && data->pSysMem)
    {
        VkDeviceSize copySize = _size;
        if (data->SysMemSize && data->SysMemSize < copySize)
            copySize = data->SysMemSize;

        memcpy(_mapped, data->pSysMem, copySize);
        if (!_isCoherent)
        {
            VkMappedMemoryRange range = {};
            range.sType  = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
            range.memory = _memory;
            range.offset = 0;
            range.size   = VK_WHOLE_SIZE;
            vkFlushMappedMemoryRanges(_device, 1, &range);
        }
    }

    Msg("* [VK] CVKBuffer: size=%u, type=%u, coherent=%d",
        (u32)_size, (u32)desc.Type, _isCoherent ? 1 : 0);
}

CVKBuffer::~CVKBuffer()
{
    if (_device == VK_NULL_HANDLE)
        return;
    if (_mapped)
    {
        vkUnmapMemory(_device, _memory);
        _mapped = nullptr;
    }
    if (_buffer != VK_NULL_HANDLE) vkDestroyBuffer(_device, _buffer, nullptr);
    if (_memory != VK_NULL_HANDLE) vkFreeMemory(_device, _memory, nullptr);
}

void CVKBuffer::AddRef() { ++_refs; }

u32 CVKBuffer::Release()
{
    const u32 remaining = --_refs;
    if (!remaining)
    {
        auto self = this;
        xr_delete(self);
    }
    return remaining;
}

u32 CVKBuffer::GetSize() const { return (u32)_size; }

bool CVKBuffer::Map(ERHI_BUFFER_MAP /*MapType*/, u32 /*MapFlags*/, RHIMappedSubresource* out)
{
    if (!out || !_mapped)
        return false;
    out->pData      = _mapped;
    out->RowPitch   = (u32)_size;
    out->DepthPitch = (u32)_size;
    return true;
}

void CVKBuffer::Unmap()
{
    if (!_isCoherent && _mapped)
    {
        VkMappedMemoryRange range = {};
        range.sType  = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = _memory;
        range.offset = 0;
        range.size   = VK_WHOLE_SIZE;
        vkFlushMappedMemoryRanges(_device, 1, &range);
    }
}

void CVKBuffer::UpdateSubresource(const void* data, u32 size, u32 offset)
{
    if (!data || !_mapped || size == 0)
        return;

    if (VkDeviceSize(offset) + size > _size)
    {
        Msg("! [VK] CVKBuffer::UpdateSubresource out of bounds: offset=%u size=%u total=%u",
            offset, size, (u32)_size);
        return;
    }

    memcpy(static_cast<u8*>(_mapped) + offset, data, size);

    if (!_isCoherent)
    {
        VkMappedMemoryRange range = {};
        range.sType  = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = _memory;
        range.offset = offset;
        range.size   = VkDeviceSize((size + 63) & ~63u);
        vkFlushMappedMemoryRanges(_device, 1, &range);
    }
}

void CVKBuffer::SetVertexBuffer(u32 slot, u32 stride, u32 offset)
{
    _vbSlot   = slot;
    _vbStride = stride;
    _vbOffset = offset;
}

void CVKBuffer::SetIndexBuffer(bool is32bit, u32 offset)
{
    _ibIs32Bit = is32bit;
    _ibOffset  = offset;
}

VkDescriptorBufferInfo CVKBuffer::GetDescriptorInfo() const
{
	VkDescriptorBufferInfo info = {};
	info.buffer = _buffer;
	info.offset = 0;
	info.range  = _size;
	return info;
}

CVKUnorderedAccessView::CVKUnorderedAccessView(CVKBuffer* buf, const RHIUAVDesc& /*desc*/)
	: _buffer(buf)
{
	R_ASSERT2(buf, "[VK] CVKUnorderedAccessView: null buffer");
	_buffer->AddRef();

	_info = _buffer->GetDescriptorInfo();
	_type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;

	Msg("* [VK] CVKUnorderedAccessView: buffer=%p size=%llu",
		(void*)_info.buffer, (unsigned long long)_info.range);
}

CVKUnorderedAccessView::~CVKUnorderedAccessView()
{
	if (_buffer) _buffer->Release();
}

void CVKUnorderedAccessView::AddRef() { ++_refs; }

u32 CVKUnorderedAccessView::Release()
{
	const u32 remaining = --_refs;
	if (!remaining)
	{
		auto self = this;
		xr_delete(self);
	}
	return remaining;
}