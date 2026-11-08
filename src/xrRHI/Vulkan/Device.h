#pragma once
#include "../RHI.h"
#include <vulkan/vulkan_core.h>
#include "RHIDevice.h"
#include "Shader.h"

struct LocalQueueFamilyIndices
{
	u32 Graphics = UINT32_MAX;
	u32 Present  = UINT32_MAX;

	bool IsComplete() const
	{
		return Graphics != UINT32_MAX && Present != UINT32_MAX;
	}
};

class InternalDeviceVK final : public IRHIDevice
{
public:
	struct PipelineKey
	{
		VkShaderModule vs = VK_NULL_HANDLE;
		VkShaderModule ps = VK_NULL_HANDLE;
		VkShaderModule gs = VK_NULL_HANDLE;
		VkShaderModule hs = VK_NULL_HANDLE;
		VkShaderModule ds = VK_NULL_HANDLE;
		ERHI_PRIMITIVE_TOPOLOGY topology = ERHI_PRIMITIVE_TOPOLOGY::TRIANGLE_LIST;
		VkFormat rtvFormats[8] = {};
		u32 rtvCount = 0;
	};
	
	class ContextLock
	{
	public:
		explicit ContextLock(const InternalDeviceVK& device);
		~ContextLock();
		ContextLock(const ContextLock&) = delete;
		ContextLock& operator=(const ContextLock&) = delete;

	private:
		const InternalDeviceVK* _device;
	};
	
	InternalDeviceVK();
	~InternalDeviceVK() override;
	
	// Drawing methods
	void SetPrimitiveTopology(ERHI_PRIMITIVE_TOPOLOGY topology) override;
	void Draw(u32 VertexCount, u32 StartVertex) override;
	void DrawIndexed(u32 IndexCount, u32 StartIndex, u32 BaseVertex) override;
	void DrawInstanced(u32 VertexCount, u32 InstanceCount, u32 StartVertex, u32 StartInstance) override;
	void DrawIndexedInstanced(u32 IndexCount, u32 InstanceCount, u32 StartIndex, u32 BaseVertex, u32 StartInstance) override;

	void ResizeBuffers(u32 Width, u32 Height) override;
	void ClearTarget(void* Target, ERTColor Transparent) override;
	void ClearTarget(void* Target, const float* Color) override;
	void ClearDepthStencil(IRHIDepthStencilView* View, ERHI_CLEAR_TARGET TargetFlags, float Depth, u8 Stencil) override;
	void GenerateMips(IRHIShaderResourceView* SRV) override;
	void Present() override;
	void CopySurface(IRHISurface* Dest, IRHISurface* Source) override;
	void CopySurface(IRHIRenderTargetView* Dest, IRHIRenderTargetView* Source) override;

	// Texture management
	IRHITextureFactory* GetTextureFactory() override;
	void SetViewport(RHIViewport& VP) override;

	// Buffer stuff
	IRHIBuffer* CreateBuffer(const RHIBufferDesc& desc = {}, const RHIBufferSubresource* pSubresource = nullptr) override;

	// Scissor rect
	void SetScissorRect(Irect* R) override;

	// Read pixels from a render target into a contiguous buffer (RGBA8 or 32-bit per pixel layout)
	// Dst: pointer to destination buffer; DstSize: size of the destination buffer in bytes
	// OutWidth/OutHeight: returned width/height of the captured region
	// OutRowPitch: number of bytes per row written into Dst (may be Width*4)
	bool ReadRenderTargetPixels(IRHIRenderTargetView* Rtv, void* Dst, u32 DstSize, u32& OutWidth, u32& OutHeight, u32& OutRowPitch) override;

	// Render Taget setup
	void SetRenderTargets(u32 NumViews, IRHIRenderTargetView* const* ppRenderTargetViews, IRHIUnorderedAccessView* const* ppRenderUAViews) override;
	void SetDSV(IRHIDepthStencilView* pDepthStencilView) override;


	void* GetContext() override;
	void* GetSwapchain() override;
	void BeginFrame() override;
	IRHIShaderResourceView* CreateShaderResourceView(IRHIBuffer* Buffer, const RHIShaderResourceViewDesc* desc) override;
	IRHIUnorderedAccessView* CreateUAV(IRHIBuffer* Buffer, const RHIUAVDesc& Desc) override;
	void SetConstants(const RHIConstantWrite* Writes, u32 Count) override;
	void ClearIndexBuffer() override;
	void SetShader(RHIObject* shader, ERHI_SHADER_TYPE Type) override;
	HRESULT LoadDDS(const void* data, size_t size, ERHI_USAGE usage, u32 bind_flags, ERHI_CPU_ACCESS_FLAG cpu_flags, int& lod, bool fallback, IRHISurface** out_surface) override;

	HRESULT CreateShader(const void* code, size_t size, ERHI_SHADER_TYPE type, RHIObject** out_shader) override;
	HRESULT ReplaceShader(RHIObject* shader, const void* code, size_t size) override;
	void Flush() override;
	HRESULT CreateInputLayout(const RHIInputElementDesc* desc, size_t count, const void* code, size_t size, RHIObject** out_layout) override;
	void SetInputLayout(RHIObject* layout) override;
	void Dispatch(u32 x, u32 y, u32 z) override;
	HRESULT CreateSamplerState(const RHISampleDesc& desc, RHIObject** out_state) override;
	void SetSamplers(RHIObject* const* States, u32 Count) override;
	void SetComputeResources(u32 start, u32 count, IRHIShaderResourceView* const* views) override;
	void SetComputeUAVs(u32 start, u32 count, IRHIUnorderedAccessView* const* views, const u32* initial_counts) override;
	HRESULT CreateOcclusionQuery(RHIObject** out_query) override;
	HRESULT GetQueryData(RHIObject* query, void* data, u32 size, u32 flags) override;
	void BeginQuery(RHIObject* query) override;
	void EndQuery(RHIObject* query) override;
	IRHISurface* CreateTexture1D(const RHITextureDesc& desc, const RHISubResource& data) override;
	void CopySwapchain(IRHISurface* dest) override;
	bool SupportsTextureSampling(ERHI_FORMAT format, u32& out_flags) override;
	void* GetState(const RHIRasterizerDesc& desc) override;
	void* GetState(const RHIDepthStencilDesc& desc) override;
	void* GetState(const RHIBlendDesc& desc) override;
	HRESULT CreateBlendState(const RHIBlendDesc& desc, RHIObject** out_state) override;
	void SetBlendState(RHIObject* state, const float* factor, u32 mask) override;
	void SetRawBlendState(void* state, const float* factor, u32 mask) override;
	bool SetDepthBounds(bool enable, float minimum, float maximum) override;
	
	xrCriticalSection& ContextMutex() const { return _contextMutex; }
	IRHIStateManager*             CreateStateManager();
	IRHIShaderResourceStateCache* CreateResourceCache();

	void EvictManagedResources() override {};
private:
	mutable xrCriticalSection _contextMutex;
	static constexpr u32 FrameCount = 3;

	struct Frame
	{
		VkCommandPool   CommandPool   = VK_NULL_HANDLE;
		VkCommandBuffer CommandBuffer = VK_NULL_HANDLE;
		VkFence         InFlight      = VK_NULL_HANDLE;
		VkSemaphore     ImageReady    = VK_NULL_HANDLE;
		VkSemaphore     RenderDone    = VK_NULL_HANDLE;
		u64             Serial        = 0;
	};

	Frame _frames[FrameCount];
	u32   _frame       = 0;     // индекс текущего кадра в _frames
	u32   _imageIndex  = 0;     // индекс текущего swapchain image
	u64   _frameSerial = 0;     // монотонный счётчик кадров
	bool  _isRecording = false; // открыт ли command buffer в этом кадре

	static bool IsValidationLayerAvailable();
	void CreateVulkanInstance();
	void CreateVulkanSurface();
	void PickPhysicalDevice();
	bool QueryQueueFamilies(VkPhysicalDevice device, LocalQueueFamilyIndices& out) const;
	void CreateLogicalDevice();
	void CreatePipelineCache();
	void CreateSwapchain();
	void CreateSwapchainImageViews();
	void CreateFrameResources();
	void CreateTriangleResources();

	VkInstance _vkInstance = VK_NULL_HANDLE;
	VkDevice _vkDevice = VK_NULL_HANDLE;
	VkPhysicalDevice _vkPhysicalDevice = VK_NULL_HANDLE;
	VkQueue _graphicsVkQueue = VK_NULL_HANDLE;
	VkQueue _presentVkQueue  = VK_NULL_HANDLE;
	VkSwapchainKHR _vkSwapchain	= VK_NULL_HANDLE;
	VkSurfaceKHR _vkSurface = VK_NULL_HANDLE;
	VkDebugUtilsMessengerEXT _vkDebugMessenger = VK_NULL_HANDLE;
	
	VkPipelineCache _vkPipelineCache = VK_NULL_HANDLE;

	VkFormat        _swapchainFormat     = VK_FORMAT_UNDEFINED;
	VkColorSpaceKHR _swapchainColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	VkExtent2D      _swapchainExtent     = {};
	bool			_swapchainOutOfDate    = false;

	xr_vector<VkImage>     _swapchainImages;
	xr_vector<VkImageView> _swapchainImageViews;
	
	VkBuffer         _triangleVB     = VK_NULL_HANDLE;
	VkDeviceMemory   _triangleVBMem  = VK_NULL_HANDLE;
	VkPipeline       _trianglePipeline = VK_NULL_HANDLE;
	VkPipelineLayout _triangleLayout = VK_NULL_HANDLE;
	
	
	
	CVKShader* _shaders[6] = {};
	RHIObject* _inputLayout = nullptr;    // сохраняем как есть, layout пока не нужен (UI = vertex pulling)
	IRHIRenderTargetView* _rtvs[8] = {};
	u32 _rtvCount = 0;
	IRHIDepthStencilView* _dsv = nullptr;
	ERHI_PRIMITIVE_TOPOLOGY _topology = ERHI_PRIMITIVE_TOPOLOGY::TRIANGLE_LIST;
	RHIViewport _viewport;
	Irect _scissor;
	bool _hasScissor = false;
	u32 _drawConstants[24] = {};   // CBDraw (slot 13 = DrawSlot)

	xr_hash_map<u64, VkPipeline> _pipelineCache;
	VkPipelineLayout _uiPipelineLayout = VK_NULL_HANDLE;   // layout с push-constants (для будущих UI-шейдеров)

	VkPipeline GetOrCreatePipeline();
	void       InvalidatePipelineCache();
	
	LocalQueueFamilyIndices _queueFamilies;
	
};
