#pragma once
#include "../RHI.h"

// ---- Пустая поверхность ----
class VkStubSurface final : public IRHISurface
{
public:
	explicit VkStubSurface(const RHITextureDesc& d = {}) : _desc(d) {}

	void  AddRef() override {}
	u32   Release() override { return 1; }
	void* GetRawTexture() override { return nullptr; }

	u32   GetWidth()  const override { return _desc.Width  ? _desc.Width  : 1; }
	u32   GetHeight() const override { return _desc.Height ? _desc.Height : 1; }
	u32   GetDepth()  const override { return 1; }
	u32   GetMipLevels() const override { return 1; }
	ERHI_RESOURCE_DIMENSION GetTextureType() const override { return ERHI_RESOURCE_DIMENSION::TEXTURE2D; }
	u32   GetMiscFlags() const override { return 0; }
	u32   GetSampleDescCount() const override { return 1; }
	u32   GetArraySize() const override { return 1; }
	ERHI_FORMAT GetFormat() const override { return _desc.Format; }
	ERHI_USAGE  GetUsage() const override { return _desc.Usage; }

	bool UpdateData(u32, u32, const RHISubResource*, const RHIBox&) override { return true; }

	void* Lock(u32 mipLevel = 0, u32* pitch = nullptr) override
	{
		// Theora, кубики, скайбоксы и др. делают Lock → memcpy → Unlock.
		// Без реального VkImage возвращаем CPU-буфер, чтобы данные было куда писать.
		const u32 w = GetWidth();
		const u32 h = GetHeight();
		const u32 rowPitch = w * 4;   // RGBA8 по умолчанию
		if (_lockedData.empty())
		{
			_lockedData.resize(size_t(rowPitch) * h);
		}
		if (pitch) *pitch = rowPitch;
		return _lockedData.data();
	}

	void Unlock() override { _lockedData.clear(); }

private:
	RHITextureDesc _desc;
	xr_vector<u8>  _lockedData;   // ← CPU-память под Lock/Unlock
};

// ---- Пустой SRV ----
class VkStubSRV final : public IRHIShaderResourceView
{
public:
    void* GetRawSRV() override { return this; }
    void  AddRef() override {}
    u32   Release() override { return 1; }
};

// ---- Пустой RTV ----
class VkStubRTV final : public IRHIRenderTargetView
{
public:
    explicit VkStubRTV(IRHISurface* s = nullptr) : _s(s ? s : new VkStubSurface()) {}
    void* GetRawRTV() override { return this; }
    IRHISurface* GetSurface() override { return _s; }
    void  AddRef() override {}
    u32   Release() override { return 1; }
private:
    IRHISurface* _s;
};

// ---- Пустой DSV ----
class VkStubDSV final : public IRHIDepthStencilView
{
public:
    explicit VkStubDSV(IRHISurface* s = nullptr) : _s(s ? s : new VkStubSurface()) {}
    void* GetRawDSV() override { return this; }
    IRHISurface* GetSurface() override { return _s; }
    void  AddRef() override {}
    u32   Release() override { return 1; }
    ERHI_DSV_DIMENSION GetDimension() const override { return ERHI_DSV_DIMENSION::TEXTURE2D; }
private:
    IRHISurface* _s;
};

// ---- Пустой UAV ----
class VkStubUAV final : public IRHIUnorderedAccessView
{
public:
    void* GetRaw() override { return this; }
    void  AddRef() override {}
    u32   Release() override { return 1; }
};

// ---- Пустой буфер ----
class VkStubBuffer final : public IRHIBuffer
{
public:
	explicit VkStubBuffer(const RHIBufferDesc& d = {})
		: _desc(d)
		, _data(d.Size ? d.Size : 1)   // ← реальная память CPU
	{}

	void  AddRef() override {}
	u32   Release() override { return 1; }
	u32   GetSize() const override { return _desc.Size ? _desc.Size : 1; }

	bool  Map(ERHI_BUFFER_MAP, u32, RHIMappedSubresource* out) override
	{
		if (out)
		{
			out->pData      = _data.data();   // ← валидный указатель
			out->RowPitch   = _desc.Size;
			out->DepthPitch = _desc.Size;
		}
		return true;
	}

	void  Unmap() override {}
	void  UpdateSubresource(const void* data, u32 size, u32 offset = 0) override
	{
		if (data && size && offset + size <= _data.size())
			memcpy(_data.data() + offset, data, size);
	}
	void  SetVertexBuffer(u32, u32, u32) override {}
	void  SetIndexBuffer(bool, u32) override {}

private:
	RHIBufferDesc _desc;
	xr_vector<u8> _data;   // ← CPU-память, чтобы Map возвращал валидный pData
};

// ---- Пустая фабрика ----
class VkStubTextureFactory final : public IRHITextureFactory
{
public:
    IRHISurface* CreateTexture2D(const RHITextureDesc& d, const RHISubResource*) override { return new VkStubSurface(d); }
    IRHISurface* CreateTexture3D(const RHITextureDesc& d, const RHISubResource*) override { return new VkStubSurface(d); }
    IRHISurface* CreateTextureFromMemory(const void*, u32, const RHITextureDesc& d) override { return new VkStubSurface(d); }
    IRHISurface* CreateRenderTarget(const RHITextureDesc& d) override { return new VkStubSurface(d); }
    IRHIShaderResourceView* CreateShaderResourceView(IRHISurface*, const RHIShaderResourceViewDesc*) override { return new VkStubSRV(); }
    IRHIRenderTargetView*   CreateRenderTargetView(IRHISurface* s, const RHIRenderTargetViewDesc& = {}) override { return new VkStubRTV(s); }
    IRHIDepthStencilView*   CreateDepthStencilView(IRHISurface* s, const RHIDepthStencilViewDesc& = {}) override { return new VkStubDSV(s); }
    IRHIUnorderedAccessView* CreateUAV(IRHISurface*, const RHIUAVDesc&) override { return new VkStubUAV(); }
};