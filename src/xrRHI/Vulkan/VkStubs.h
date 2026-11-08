#pragma once
#include "../RHI.h"
#include <vulkan/vulkan.h>

// ---- Пустая поверхность ----
class VkStubSurface final : public IRHISurface
{
public:
	explicit VkStubSurface(const RHITextureDesc& d = {}) : _desc(d) {}

	void  AddRef() override {}
	u32   Release() override { return 1; }
	void* GetRawTexture() override { return this; }

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
		const u32 w = std::max<u32>(1, GetWidth() >> mipLevel);
		const u32 h = std::max<u32>(1, GetHeight() >> mipLevel);
		const u32 rowPitch = w * 4;
		_lockedData.resize(size_t(rowPitch) * h);
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