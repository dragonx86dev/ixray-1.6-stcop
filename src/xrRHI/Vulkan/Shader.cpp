#include "Shader.h"

// ---------------------------------------------------------------------------
// CVKShaderResourceView / CVKUnorderedAccessView — Шаг 1.
// ---------------------------------------------------------------------------
CVKShaderResourceView::CVKShaderResourceView(CVKBuffer* buf, const RHIShaderResourceViewDesc* desc)
	: _buffer(buf)
{
	R_ASSERT2(buf, "[VK] CVKShaderResourceView: null buffer");
	_buffer->AddRef();

	_info = _buffer->GetDescriptorInfo();      // { VkBuffer, 0, size }
	_type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; // StructuredBuffer / ByteAddressBuffer

	const u32 fmt = desc ? u32(desc->Format) : 0;
	Msg("* [VK] CVKShaderResourceView: buffer=%p size=%llu format=%u type=STORAGE_BUFFER",
		(void*)_info.buffer, (unsigned long long)_info.range, fmt);
}

CVKShaderResourceView::~CVKShaderResourceView()
{
	if (_buffer) _buffer->Release();
}

void CVKShaderResourceView::AddRef() { ++_refs; }

u32 CVKShaderResourceView::Release()
{
	const u32 remaining = --_refs;
	if (!remaining)
	{
		auto self = this;
		xr_delete(self);
	}
	return remaining;
}