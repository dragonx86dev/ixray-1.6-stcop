#pragma once

#include "../RHI.h"
#include <vulkan/vulkan.h>
#include <atomic>

// ---------------------------------------------------------------------------
// CVKTexture — обёртка над VkImage + VkImageView + VkDeviceMemory.
// Поддерживает 2D / 3D / Cubemap (по desc.Depth и desc.ArraySize).
// Загрузка данных идёт через staging buffer + vkCmdCopyBufferToImage
// (вызывается из CVKTextureFactory::UploadSubresource).
// ---------------------------------------------------------------------------
class CVKTexture final : public IRHISurface
{
public:
	CVKTexture(VkDevice device, VkPhysicalDevice physicalDevice,
	           VkCommandPool cmdPool, VkQueue queue,
	           const RHITextureDesc& desc);
	~CVKTexture() override;

	void AddRef() override;
	u32  Release() override;

	void* GetRawTexture() override { return _image; }
	VkDevice GetDevice() const { return _device; }
	VkFormat GetVkFormat() const { return _vkFormat; }

	u32 GetWidth()       const override { return _desc.Width; }
	u32 GetHeight()      const override { return _desc.Height; }
	u32 GetDepth()       const override { return _desc.Depth; }
	u32 GetMipLevels()   const override { return _desc.MipLevels; }
	u32 GetMiscFlags()   const override { return _desc.MiscFlags; }
	u32 GetSampleDescCount() const override { return _desc.SampleDescCount; }
	u32 GetArraySize()   const override { return _desc.ArraySize; }
	ERHI_FORMAT GetFormat() const override { return _desc.Format; }
	ERHI_USAGE  GetUsage()  const override { return _desc.Usage; }
	ERHI_RESOURCE_DIMENSION GetTextureType() const override
	{
		return _desc.Depth > 1 ? ERHI_RESOURCE_DIMENSION::TEXTURE3D
		                       : ERHI_RESOURCE_DIMENSION::TEXTURE2D;
	}

	bool UpdateData(u32 mipLevel, u32 arrayLayer, const RHISubResource* subRes, const RHIBox& box) override;

	// Не поддерживаем lock/unlock для MVP — только через UpdateData
	void* Lock(u32 /*mip*/, u32* /*pitch*/) override { return nullptr; }
	void  Unlock() override {}

	VkImage     GetImage()  const { return _image; }
	VkImageView GetView()   const { return _view; }
	const RHITextureDesc& GetDesc() const { return _desc; }
	bool IsReady() const { return _image != VK_NULL_HANDLE && _view != VK_NULL_HANDLE; }

private:
	void CreateImageAndView();
	void Destroy();
	bool UploadSubresource(u32 mipLevel, u32 arrayLayer, const RHISubResource& subRes, const RHIBox* box);

	VkDevice         _device = VK_NULL_HANDLE;
	VkPhysicalDevice _physical = VK_NULL_HANDLE;
	VkCommandPool    _cmdPool = VK_NULL_HANDLE;
	VkQueue          _queue = VK_NULL_HANDLE;
	VkFormat		 _vkFormat = VK_FORMAT_UNDEFINED; 

	VkImage          _image = VK_NULL_HANDLE;
	VkImageView      _view = VK_NULL_HANDLE;
	VkDeviceMemory   _memory = VK_NULL_HANDLE;
	RHITextureDesc   _desc{};
	bool             _ready = false;
	std::atomic<u32> _refs{ 1 };
};

// ---------------------------------------------------------------------------
// CVKShaderResourceView — обёртка над VkImageView для SampledImage.
// (уже есть в Buffer.h — добавляем ещё один конструктор для IRHISurface)
// ---------------------------------------------------------------------------
class CVKShaderResourceViewTexture final : public IRHIShaderResourceView
{
public:
	CVKShaderResourceViewTexture(CVKTexture* tex, const RHIShaderResourceViewDesc* desc);
	~CVKShaderResourceViewTexture() override;

	void* GetRawSRV() override { return this; }
	void  AddRef() override;
	u32   Release() override;

	VkImageView                   GetView()  const { return _info.imageView; }
	const VkDescriptorImageInfo&  GetImageInfo() const { return _info; }

private:
	CVKTexture* _texture = nullptr;
	VkDescriptorImageInfo _info{};
	std::atomic<u32> _refs{ 1 };
};

class CVKRenderTargetView final : public IRHIRenderTargetView
{
public:
	CVKRenderTargetView(CVKTexture* tex, VkImageView view);
	~CVKRenderTargetView() override;

	void* GetRawRTV() override { return _view; }
	IRHISurface* GetSurface() override { return _texture; }
	void  AddRef() override;
	u32   Release() override;

private:
	CVKTexture*      _texture = nullptr;
	VkImageView      _view    = VK_NULL_HANDLE;
	std::atomic<u32> _refs{ 1 };
};

class CVKDepthStencilView final : public IRHIDepthStencilView
{
public:
	CVKDepthStencilView(CVKTexture* tex, VkImageView view, ERHI_DSV_DIMENSION dim);
	~CVKDepthStencilView() override;

	void* GetRawDSV() override { return _view; }
	IRHISurface* GetSurface() override { return _texture; }
	void  AddRef() override;
	u32   Release() override;
	ERHI_DSV_DIMENSION GetDimension() const override { return _dimension; }

private:
	CVKTexture*      _texture = nullptr;
	VkImageView      _view    = VK_NULL_HANDLE;
	ERHI_DSV_DIMENSION _dimension;
	std::atomic<u32> _refs{ 1 };
};

class CVKUnorderedAccessViewTexture final : public IRHIUnorderedAccessView
{
public:
	CVKUnorderedAccessViewTexture(CVKTexture* tex, const RHIUAVDesc& desc);
	~CVKUnorderedAccessViewTexture() override;

	void* GetRaw() override { return this; }
	void  AddRef() override;
	u32   Release() override;

	VkImageView GetView() const { return _view; }

private:
	CVKTexture*    _texture = nullptr;
	VkImageView    _view    = VK_NULL_HANDLE;   // отдельный view для конкретного mip
	std::atomic<u32> _refs{ 1 };
};

// ---------------------------------------------------------------------------
// CVKTextureFactory — заменяет VkStubTextureFactory
// ---------------------------------------------------------------------------
class CVKTextureFactory final : public IRHITextureFactory
{
public:
	CVKTextureFactory(VkDevice device, VkPhysicalDevice physical,
	                  VkCommandPool cmdPool, VkQueue queue);
	~CVKTextureFactory() override = default;

	IRHISurface* CreateTexture2D(const RHITextureDesc& desc, const RHISubResource* data) override;
	IRHISurface* CreateTexture3D(const RHITextureDesc& desc, const RHISubResource* data) override;
	IRHISurface* CreateTextureFromMemory(const void* data, u32 size, const RHITextureDesc& desc) override;
	IRHISurface* CreateRenderTarget(const RHITextureDesc& desc) override;

	IRHIShaderResourceView* CreateShaderResourceView(IRHISurface* surface, const RHIShaderResourceViewDesc* desc) override;
	IRHIRenderTargetView*   CreateRenderTargetView(IRHISurface* surface, const RHIRenderTargetViewDesc& desc) override;
	IRHIDepthStencilView*   CreateDepthStencilView(IRHISurface* surface, const RHIDepthStencilViewDesc& desc) override;
	IRHIUnorderedAccessView* CreateUAV(IRHISurface* surface, const RHIUAVDesc& desc) override;

private:
	VkDevice         _device = VK_NULL_HANDLE;
	VkPhysicalDevice _physical = VK_NULL_HANDLE;
	VkCommandPool    _cmdPool = VK_NULL_HANDLE;
	VkQueue          _queue = VK_NULL_HANDLE;
};