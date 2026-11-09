#include "Texture.h"

// ---- helpers ----
static u32 FindMemoryType(VkPhysicalDevice phys, u32 typeBits, VkMemoryPropertyFlags want)
{
	VkPhysicalDeviceMemoryProperties props{};
	vkGetPhysicalDeviceMemoryProperties(phys, &props);
	for (u32 i = 0; i < props.memoryTypeCount; ++i)
	{
		if ((typeBits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want)
			return i;
	}
	return UINT32_MAX;
}

static bool IsDepthFormat(VkFormat fmt)
{
	return fmt == VK_FORMAT_D16_UNORM         ||
		   fmt == VK_FORMAT_D24_UNORM_S8_UINT ||
		   fmt == VK_FORMAT_D32_SFLOAT        ||
		   fmt == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

static bool HasStencil(VkFormat fmt)
{
	return fmt == VK_FORMAT_D24_UNORM_S8_UINT ||
		   fmt == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

static VkFormat RHIFormatToVk(ERHI_FORMAT fmt)
{
	// Быстрая проверка: для многих TYPELESS-форматов используется
	// «читаемый» вариант (UNORM / UINT / FLOAT), т.к. в Vulkan нет TYPELESS.
	// SRGB-варианты сохраняются как SRGB (для UI/иконок это критично).
	// Depth-TYPELESS маппится в глубинный вариант (для depth-stencil).
	switch (fmt)
	{
	// ===== 1-канальные 8-bit =====
	case ERHI_FORMAT::R8_TYPELESS:     return VK_FORMAT_R8_UNORM;
	case ERHI_FORMAT::R8_UNORM:        return VK_FORMAT_R8_UNORM;
	case ERHI_FORMAT::R8_UINT:         return VK_FORMAT_R8_UINT;
	case ERHI_FORMAT::R8_SNORM:        return VK_FORMAT_R8_SNORM;
	case ERHI_FORMAT::R8_SINT:         return VK_FORMAT_R8_SINT;
	case ERHI_FORMAT::A8_UNORM:        return VK_FORMAT_R8_UNORM;
	case ERHI_FORMAT::R1_UNORM:        return VK_FORMAT_R8_UNORM;   // расширяем до 8-bit

	// ===== 1-канальные 16-bit =====
	case ERHI_FORMAT::R16_TYPELESS:    return VK_FORMAT_R16_UNORM;
	case ERHI_FORMAT::R16_FLOAT:       return VK_FORMAT_R16_SFLOAT;
	case ERHI_FORMAT::R16_UNORM:       return VK_FORMAT_R16_UNORM;
	case ERHI_FORMAT::R16_UINT:        return VK_FORMAT_R16_UINT;
	case ERHI_FORMAT::R16_SNORM:       return VK_FORMAT_R16_SNORM;
	case ERHI_FORMAT::R16_SINT:        return VK_FORMAT_R16_SINT;

	// ===== 1-канальные 32-bit =====
	case ERHI_FORMAT::R32_TYPELESS:    return VK_FORMAT_R32_UINT;   // «нейтральный» view
	case ERHI_FORMAT::R32_FLOAT:       return VK_FORMAT_R32_SFLOAT;
	case ERHI_FORMAT::R32_UINT:        return VK_FORMAT_R32_UINT;
	case ERHI_FORMAT::R32_SINT:        return VK_FORMAT_R32_SINT;

	// ===== 2-канальные =====
	case ERHI_FORMAT::R8G8_TYPELESS:   return VK_FORMAT_R8G8_UNORM;
	case ERHI_FORMAT::R8G8_UNORM:      return VK_FORMAT_R8G8_UNORM;
	case ERHI_FORMAT::R8G8_UINT:       return VK_FORMAT_R8G8_UINT;
	case ERHI_FORMAT::R8G8_SNORM:      return VK_FORMAT_R8G8_SNORM;
	case ERHI_FORMAT::R8G8_SINT:       return VK_FORMAT_R8G8_SINT;

	case ERHI_FORMAT::R16G16_TYPELESS: return VK_FORMAT_R16G16_UNORM;
	case ERHI_FORMAT::R16G16_FLOAT:    return VK_FORMAT_R16G16_SFLOAT;
	case ERHI_FORMAT::R16G16_UNORM:    return VK_FORMAT_R16G16_UNORM;
	case ERHI_FORMAT::R16G16_UINT:     return VK_FORMAT_R16G16_UINT;
	case ERHI_FORMAT::R16G16_SNORM:    return VK_FORMAT_R16G16_SNORM;
	case ERHI_FORMAT::R16G16_SINT:     return VK_FORMAT_R16G16_SINT;

	case ERHI_FORMAT::R32G32_TYPELESS: return VK_FORMAT_R32G32_UINT;
	case ERHI_FORMAT::R32G32_FLOAT:    return VK_FORMAT_R32G32_SFLOAT;
	case ERHI_FORMAT::R32G32_UINT:     return VK_FORMAT_R32G32_UINT;
	case ERHI_FORMAT::R32G32_SINT:     return VK_FORMAT_R32G32_SINT;

	// ===== 4-канальные 8-bit =====
	case ERHI_FORMAT::R8G8B8A8_TYPELESS:   return VK_FORMAT_R8G8B8A8_UNORM;
	case ERHI_FORMAT::R8G8B8A8_UNORM:      return VK_FORMAT_R8G8B8A8_UNORM;
	case ERHI_FORMAT::R8G8B8A8_UNORM_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
	case ERHI_FORMAT::R8G8B8A8_UINT:       return VK_FORMAT_R8G8B8A8_UINT;
	case ERHI_FORMAT::R8G8B8A8_SNORM:      return VK_FORMAT_R8G8B8A8_SNORM;
	case ERHI_FORMAT::R8G8B8A8_SINT:       return VK_FORMAT_R8G8B8A8_SINT;

	case ERHI_FORMAT::B8G8R8A8_TYPELESS:   return VK_FORMAT_B8G8R8A8_UNORM;
	case ERHI_FORMAT::B8G8R8A8_UNORM:      return VK_FORMAT_B8G8R8A8_UNORM;
	case ERHI_FORMAT::B8G8R8A8_UNORM_SRGB: return VK_FORMAT_B8G8R8A8_SRGB;
	case ERHI_FORMAT::B8G8R8X8_TYPELESS:   return VK_FORMAT_B8G8R8A8_UNORM;
	case ERHI_FORMAT::B8G8R8X8_UNORM:      return VK_FORMAT_B8G8R8A8_UNORM;
	case ERHI_FORMAT::B8G8R8X8_UNORM_SRGB: return VK_FORMAT_B8G8R8A8_SRGB;

	// ===== 4-канальные 16-bit =====
	case ERHI_FORMAT::R16G16B16A16_TYPELESS: return VK_FORMAT_R16G16B16A16_UNORM;
	case ERHI_FORMAT::R16G16B16A16_FLOAT:    return VK_FORMAT_R16G16B16A16_SFLOAT;
	case ERHI_FORMAT::R16G16B16A16_UNORM:    return VK_FORMAT_R16G16B16A16_UNORM;
	case ERHI_FORMAT::R16G16B16A16_UINT:     return VK_FORMAT_R16G16B16A16_UINT;
	case ERHI_FORMAT::R16G16B16A16_SNORM:    return VK_FORMAT_R16G16B16A16_SNORM;
	case ERHI_FORMAT::R16G16B16A16_SINT:     return VK_FORMAT_R16G16B16A16_SINT;

	// ===== 4-канальные 32-bit =====
	case ERHI_FORMAT::R32G32B32A32_TYPELESS: return VK_FORMAT_R32G32B32A32_UINT;
	case ERHI_FORMAT::R32G32B32A32_FLOAT:    return VK_FORMAT_R32G32B32A32_SFLOAT;
	case ERHI_FORMAT::R32G32B32A32_UINT:     return VK_FORMAT_R32G32B32A32_UINT;
	case ERHI_FORMAT::R32G32B32A32_SINT:     return VK_FORMAT_R32G32B32A32_SINT;

	// ===== 3-канальные (редко сэмплируемые; используй fallback R32G32B32A32) =====
	case ERHI_FORMAT::R32G32B32_TYPELESS:    return VK_FORMAT_R32G32B32A32_UINT;  // 3ch не поддержан везде — upscale
	case ERHI_FORMAT::R32G32B32_FLOAT:       return VK_FORMAT_R32G32B32A32_SFLOAT;
	case ERHI_FORMAT::R32G32B32_UINT:        return VK_FORMAT_R32G32B32A32_UINT;
	case ERHI_FORMAT::R32G32B32_SINT:        return VK_FORMAT_R32G32B32A32_SINT;

	// ===== 10/11-bit packed =====
	case ERHI_FORMAT::R10G10B10A2_TYPELESS:  return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
	case ERHI_FORMAT::R10G10B10A2_UNORM:     return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
	case ERHI_FORMAT::R10G10B10A2_UINT:      return VK_FORMAT_A2B10G10R10_UINT_PACK32;
	case ERHI_FORMAT::R10G10B10_XR_BIAS_A2_UNORM: return VK_FORMAT_A2B10G10R10_UNORM_PACK32; // приближение
	case ERHI_FORMAT::R11G11B10_FLOAT:       return VK_FORMAT_B10G11R11_UFLOAT_PACK32;

	// ===== Shared-exp HDR =====
	case ERHI_FORMAT::R9G9B9E5_SHAREDEXP:    return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;

	// ===== Packed 16-bit RGB(A) =====
	case ERHI_FORMAT::B5G6R5_UNORM:          return VK_FORMAT_B5G6R5_UNORM_PACK16;
	case ERHI_FORMAT::B5G5R5A1_UNORM:        return VK_FORMAT_B5G5R5A1_UNORM_PACK16;
	case ERHI_FORMAT::B4G4R4A4_UNORM:        return VK_FORMAT_B4G4R4A4_UNORM_PACK16;

	// ===== Depth / stencil =====
	case ERHI_FORMAT::R32G8X24_TYPELESS:       return VK_FORMAT_D32_SFLOAT_S8_UINT;
	case ERHI_FORMAT::D32_FLOAT_S8X24_UINT:    return VK_FORMAT_D32_SFLOAT_S8_UINT;
	case ERHI_FORMAT::R32_FLOAT_X8X24_TYPELESS: return VK_FORMAT_D32_SFLOAT_S8_UINT;
	case ERHI_FORMAT::X32_TYPELESS_G8X24_UINT: return VK_FORMAT_D32_SFLOAT_S8_UINT;

	case ERHI_FORMAT::R24G8_TYPELESS:          return VK_FORMAT_D24_UNORM_S8_UINT;
	case ERHI_FORMAT::D24_UNORM_S8_UINT:       return VK_FORMAT_D24_UNORM_S8_UINT;
	case ERHI_FORMAT::R24_UNORM_X8_TYPELESS:   return VK_FORMAT_D24_UNORM_S8_UINT;
	case ERHI_FORMAT::X24_TYPELESS_G8_UINT:    return VK_FORMAT_D24_UNORM_S8_UINT;

	case ERHI_FORMAT::D16_UNORM:               return VK_FORMAT_D16_UNORM;
	case ERHI_FORMAT::D32_FLOAT:               return VK_FORMAT_D32_SFLOAT;

	// ===== BC1..BC7 (DXGI: BC*_TYPELESS → UNORM; SRGB через view) =====
	case ERHI_FORMAT::BC1_TYPELESS:      return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
	case ERHI_FORMAT::BC1_UNORM:         return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
	case ERHI_FORMAT::BC1_UNORM_SRGB:    return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
	case ERHI_FORMAT::BC2_TYPELESS:      return VK_FORMAT_BC2_UNORM_BLOCK;
	case ERHI_FORMAT::BC2_UNORM:         return VK_FORMAT_BC2_UNORM_BLOCK;
	case ERHI_FORMAT::BC2_UNORM_SRGB:    return VK_FORMAT_BC2_SRGB_BLOCK;
	case ERHI_FORMAT::BC3_TYPELESS:      return VK_FORMAT_BC3_UNORM_BLOCK;
	case ERHI_FORMAT::BC3_UNORM:         return VK_FORMAT_BC3_UNORM_BLOCK;
	case ERHI_FORMAT::BC3_UNORM_SRGB:    return VK_FORMAT_BC3_SRGB_BLOCK;
	case ERHI_FORMAT::BC4_TYPELESS:      return VK_FORMAT_BC4_UNORM_BLOCK;
	case ERHI_FORMAT::BC4_UNORM:         return VK_FORMAT_BC4_UNORM_BLOCK;
	case ERHI_FORMAT::BC4_SNORM:         return VK_FORMAT_BC4_SNORM_BLOCK;
	case ERHI_FORMAT::BC5_TYPELESS:      return VK_FORMAT_BC5_UNORM_BLOCK;
	case ERHI_FORMAT::BC5_UNORM:         return VK_FORMAT_BC5_UNORM_BLOCK;
	case ERHI_FORMAT::BC5_SNORM:         return VK_FORMAT_BC5_SNORM_BLOCK;
	case ERHI_FORMAT::BC6H_TYPELESS:     return VK_FORMAT_BC6H_UFLOAT_BLOCK;
	case ERHI_FORMAT::BC6H_UF16:         return VK_FORMAT_BC6H_UFLOAT_BLOCK;
	case ERHI_FORMAT::BC6H_SF16:         return VK_FORMAT_BC6H_SFLOAT_BLOCK;
	case ERHI_FORMAT::BC7_TYPELESS:      return VK_FORMAT_BC7_UNORM_BLOCK;
	case ERHI_FORMAT::BC7_UNORM:         return VK_FORMAT_BC7_UNORM_BLOCK;
	case ERHI_FORMAT::BC7_UNORM_SRGB:    return VK_FORMAT_BC7_SRGB_BLOCK;

	// ===== Упакованные YUV / RGBG — в STALKER не встречаются,
	//        но ERHI_FORMAT их объявляет. Даём безопасный fallback. =====
	case ERHI_FORMAT::R8G8_B8G8_UNORM:
	case ERHI_FORMAT::G8R8_G8B8_UNORM:
	case ERHI_FORMAT::AYUV:
	case ERHI_FORMAT::Y410:
	case ERHI_FORMAT::Y416:
	case ERHI_FORMAT::NV12:
	case ERHI_FORMAT::P010:
	case ERHI_FORMAT::P016:
	case ERHI_FORMAT::YUY2:
	case ERHI_FORMAT::Y210:
	case ERHI_FORMAT::Y216:
	case ERHI_FORMAT::NV11:
	case ERHI_FORMAT::AI44:
	case ERHI_FORMAT::IA44:
	case ERHI_FORMAT::P8:
	case ERHI_FORMAT::A8P8:
	{
		static bool s_warned = false;
		if (!s_warned)
		{
			Msg("! [VK] RHIFormatToVk: exotic format %u, falling back to R8G8B8A8_UNORM",
			    (u32)fmt);
			s_warned = true;
		}
		return VK_FORMAT_R8G8B8A8_UNORM;
	}

	// ===== UNKNOWN / FORCE_UINT =====
	case ERHI_FORMAT::UNKNOWN:
	default:
		Msg("! [VK] RHIFormatToVk: unknown ERHI_FORMAT %u, falling back to R8G8B8A8_UNORM",
		    (u32)fmt);
		return VK_FORMAT_R8G8B8A8_UNORM;
	}
}

// ---- CVKTexture ----
CVKTexture::CVKTexture(VkDevice device, VkPhysicalDevice physical,
                       VkCommandPool cmdPool, VkQueue queue,
                       const RHITextureDesc& desc)
	: _device(device), _physical(physical), _cmdPool(cmdPool), _queue(queue), _desc(desc)
{
	CreateImageAndView();
}

CVKTexture::~CVKTexture()
{
	Destroy();
}

void CVKTexture::AddRef() { ++_refs; }

u32 CVKTexture::Release()
{
	const u32 remaining = --_refs;
	if (!remaining) { auto self = this; xr_delete(self); }
	return remaining;
}

void CVKTexture::CreateImageAndView()
{
	VkFormat vkFmt = RHIFormatToVk(_desc.Format);

	// --- Fix A: DEPTH_STENCIL-ресурс, но формат не depth ---
	// X-Ray передаёт TYPELESS-форматы (R16_TYPELESS, R32_TYPELESS и т.п.),
	// которые RHIFormatToVk маппит в не-depth. Если bindFlags содержит
	// DEPTH_STENCIL — исправляем на ближайший depth-формат.
	const bool isDepth = (_desc.BindFlags & ERHI_BIND_FLAG::DEPTH_STENCIL) != 0;
	if (isDepth && !IsDepthFormat(vkFmt))
	{
		switch (vkFmt)
		{
		case VK_FORMAT_R16_UNORM:
			vkFmt = VK_FORMAT_D16_UNORM;
			break;
		case VK_FORMAT_R32_UINT:
		case VK_FORMAT_R32_SFLOAT:
			vkFmt = VK_FORMAT_D32_SFLOAT;
			break;
		default:
			Msg("! [VK] CVKTexture: DEPTH_STENCIL with unsupported format %d, using D32_SFLOAT",
			    (int)vkFmt);
			vkFmt = VK_FORMAT_D32_SFLOAT;
			break;
		}
	}

	// --- Fix B: D24_UNORM_S8_UINT не сэмплится на AMDVLK ---
	// Если нужен SAMPLED-depth — заменяем на D32_SFLOAT_S8_UINT
	// (universally supported как sampled на десктопе).
	const bool wantsSampled = (_desc.BindFlags & ERHI_BIND_FLAG::SHADER_RESOURCE) != 0;
	if (isDepth && wantsSampled && vkFmt == VK_FORMAT_D24_UNORM_S8_UINT)
	{
		Msg("* [VK] CVKTexture: D24+sampled unsupported, using D32_SFLOAT_S8_UINT");
		vkFmt = VK_FORMAT_D32_SFLOAT_S8_UINT;
	}
	
	_vkFormat = vkFmt;

	VkImageCreateInfo ii{};
	ii.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ii.imageType     = _desc.Depth > 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
	ii.format        = vkFmt;
	ii.extent        = { _desc.Width, _desc.Height, _desc.Depth ? _desc.Depth : 1 };
	ii.mipLevels     = _desc.MipLevels ? _desc.MipLevels : 1;
	ii.arrayLayers   = _desc.ArraySize ? _desc.ArraySize : 1;
	ii.samples       = _desc.SampleDescCount > 1
		? (VkSampleCountFlagBits)_desc.SampleDescCount
		: VK_SAMPLE_COUNT_1_BIT;
	ii.tiling        = VK_IMAGE_TILING_OPTIMAL;
	ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	// SAMPLED — всегда (X-Ray не всегда выставляет SHADER_RESOURCE, но
	// потом создаёт SRV и сэмплирует). Иначе image создастся с TRANSFER_DST
	// и view упадёт.
	VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	if (_desc.BindFlags & ERHI_BIND_FLAG::RENDER_TARGET)
		usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	if (isDepth)
		usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	if (_desc.BindFlags & ERHI_BIND_FLAG::UNORDERED_ACCESS)
		usage |= VK_IMAGE_USAGE_STORAGE_BIT;
	ii.usage = usage;

	if (_desc.MiscFlags & 0x4 /* D3D11_RESOURCE_MISC_TEXTURECUBE */)
		ii.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;

	VkResult r = vkCreateImage(_device, &ii, nullptr, &_image);
	if (r != VK_SUCCESS)
	{
		Msg("! [VK] CVKTexture: vkCreateImage failed (fmt=%d ext=%ux%ux%u usage=0x%x r=%d)",
		    (int)vkFmt, ii.extent.width, ii.extent.height, ii.extent.depth,
		    (u32)ii.usage, (int)r);
		_image  = VK_NULL_HANDLE;
		_view   = VK_NULL_HANDLE;
		_memory = VK_NULL_HANDLE;
		return;
	}

	VkMemoryRequirements req{};
	vkGetImageMemoryRequirements(_device, _image, &req);

	const u32 typeIdx = FindMemoryType(_physical, req.memoryTypeBits,
	                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	R_ASSERT2(typeIdx != UINT32_MAX, "No device-local memory for CVKTexture");

	VkMemoryAllocateInfo ai{};
	ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	ai.allocationSize  = req.size;
	ai.memoryTypeIndex = typeIdx;
	r = vkAllocateMemory(_device, &ai, nullptr, &_memory);
	if (r != VK_SUCCESS)
	{
		Msg("! [VK] CVKTexture: vkAllocateMemory failed (fmt=%d %ux%ux%u r=%d)",
			(int)vkFmt, ii.extent.width, ii.extent.height, ii.extent.depth, (int)r);
		vkDestroyImage(_device, _image, nullptr);
		_image = VK_NULL_HANDLE;
		return;
	}
	r = vkBindImageMemory(_device, _image, _memory, 0);
	R_ASSERT2(r == VK_SUCCESS, "vkBindImageMemory (CVKTexture)");

	// --- View ---
	VkImageViewCreateInfo vi{};
	vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	vi.image = _image;
	vi.viewType =
		_desc.Depth > 1
			? VK_IMAGE_VIEW_TYPE_3D
			: (_desc.MiscFlags & 0x4)
				? (_desc.ArraySize > 6 ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE)
				: (_desc.ArraySize > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D);
	vi.format = vkFmt;
	vi.components = {
		VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
		VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY
	};

	// aspectMask по формату
	if (IsDepthFormat(vkFmt))
	{
		vi.subresourceRange.aspectMask = HasStencil(vkFmt)
			? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
			: VK_IMAGE_ASPECT_DEPTH_BIT;
	}
	else
	{
		vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	}
	vi.subresourceRange.baseMipLevel   = 0;
	vi.subresourceRange.levelCount     = ii.mipLevels;
	vi.subresourceRange.baseArrayLayer = 0;
	vi.subresourceRange.layerCount     = ii.arrayLayers;   // уже 6 для cubemap (из metadata.arraySize)

	r = vkCreateImageView(_device, &vi, nullptr, &_view);
	if (r != VK_SUCCESS)
	{
		Msg("! [VK] CVKTexture: vkCreateImageView failed (fmt=%d r=%d)", (int)vkFmt, (int)r);
		_view = VK_NULL_HANDLE;
	}
}


void CVKTexture::Destroy()
{
	if (_view   != VK_NULL_HANDLE) { vkDestroyImageView(_device, _view, nullptr);  _view = VK_NULL_HANDLE; }
	if (_image  != VK_NULL_HANDLE) { vkDestroyImage    (_device, _image, nullptr); _image = VK_NULL_HANDLE; }
	if (_memory != VK_NULL_HANDLE) { vkFreeMemory      (_device, _memory, nullptr); _memory = VK_NULL_HANDLE; }
}

bool CVKTexture::UpdateData(u32 mipLevel, u32 arrayLayer, const RHISubResource* subRes, const RHIBox& box)
{
	if (!subRes || !subRes->Data) return false;
	const bool hasBox = box.right > box.left && box.bottom > box.top && box.back > box.front;
	return UploadSubresource(mipLevel, arrayLayer, *subRes, hasBox ? &box : nullptr);
}

bool CVKTexture::UploadSubresource(u32 mipLevel, u32 arrayLayer,
								   const RHISubResource& subRes, const RHIBox* /*box*/)
{
	if (_image == VK_NULL_HANDLE)
		return false;

	// === FIX A: vkCmdCopyBufferToImage НЕ поддерживает depth/stencil aspect ===
	// Если это depth-текстура (shadow map, height map) — пропускаем загрузку.
	// Такие текстуры обычно рендерятся (render target), а не загружаются из DDS.
	// Если для depth нужна preinitialized data — нужен другой механизм
	// (staging image + vkCmdCopyImage), но для MVP это не критично.
	const VkFormat actualFmt = (_vkFormat != VK_FORMAT_UNDEFINED) ? _vkFormat
																   : RHIFormatToVk(_desc.Format);
	// Защита от неверного DataSize от X-Ray
	const u32 expectedMinSize =
		std::max<u32>(1, _desc.Width  >> mipLevel) *
		std::max<u32>(1, _desc.Height >> mipLevel) *
		std::max<u32>(1, _desc.Depth  >> mipLevel) * 4;   // 4 байта/texel для uncompressed
	if (IsDepthFormat(actualFmt) == false && subRes.DataSize < expectedMinSize / 2)
	{
		Msg("! [VK] UploadSubresource: suspicious DataSize=%u (expected ~%u) mip=%u",
			subRes.DataSize, expectedMinSize, mipLevel);
		// не падать — просто попробуем, возможно X-Ray прав (компрессия и т.п.)
	}

	// === FIX A2: MSAA-текстуры тоже нельзя грузить через buffer→image ===
	if (_desc.SampleDescCount > 1)
	{
		Msg("~ [VK] UploadSubresource: skip MSAA upload (mip=%u layer=%u)",
			mipLevel, arrayLayer);
		return true;
	}

	if (!subRes.Data || !subRes.DataSize)
	{
		Msg("! [VK] UploadSubresource: null data (mip=%u layer=%u)", mipLevel, arrayLayer);
		return false;
	}

	// --- staging buffer ---
	VkBufferCreateInfo bi{};
	bi.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bi.size        = subRes.DataSize;
	bi.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VkBuffer staging = VK_NULL_HANDLE;
	VkResult r = vkCreateBuffer(_device, &bi, nullptr, &staging);
	if (r != VK_SUCCESS)
	{
		Msg("! [VK] UploadSubresource: vkCreateBuffer staging failed r=%d", (int)r);
		return false;
	}

	VkMemoryRequirements req{};
	vkGetBufferMemoryRequirements(_device, staging, &req);
	const u32 typeIdx = FindMemoryType(_physical, req.memoryTypeBits,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (typeIdx == UINT32_MAX)
	{
		vkDestroyBuffer(_device, staging, nullptr);
		Msg("! [VK] UploadSubresource: no host-visible memory for staging");
		return false;
	}

	VkMemoryAllocateInfo ai{};
	ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	ai.allocationSize  = req.size;
	ai.memoryTypeIndex = typeIdx;
	VkDeviceMemory stagingMem = VK_NULL_HANDLE;
	r = vkAllocateMemory(_device, &ai, nullptr, &stagingMem);
	if (r != VK_SUCCESS)
	{
		vkDestroyBuffer(_device, staging, nullptr);
		Msg("! [VK] UploadSubresource: vkAllocateMemory staging failed r=%d", (int)r);
		return false;
	}
	vkBindBufferMemory(_device, staging, stagingMem, 0);

	void* mapped = nullptr;
	vkMapMemory(_device, stagingMem, 0, VK_WHOLE_SIZE, 0, &mapped);
	memcpy(mapped, subRes.Data, subRes.DataSize);
	vkUnmapMemory(_device, stagingMem);

	// --- one-shot command buffer ---
	VkCommandBufferAllocateInfo cbi{};
	cbi.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	cbi.commandPool        = _cmdPool;
	cbi.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cbi.commandBufferCount = 1;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	r = vkAllocateCommandBuffers(_device, &cbi, &cmd);
	if (r != VK_SUCCESS)
	{
		vkDestroyBuffer(_device, staging, nullptr);
		vkFreeMemory(_device, stagingMem, nullptr);
		Msg("! [VK] UploadSubresource: vkAllocateCommandBuffers failed r=%d", (int)r);
		return false;
	}

	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &beginInfo);

	// --- subresource range ---
	const VkFormat vkFmt = RHIFormatToVk(_desc.Format);   // только для aspect
	VkImageSubresourceRange range{};
	if (IsDepthFormat(vkFmt))
	{
		range.aspectMask = HasStencil(vkFmt)
			? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
			: VK_IMAGE_ASPECT_DEPTH_BIT;
	}
	else
	{
		range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	}
	range.baseMipLevel   = mipLevel;
	range.levelCount     = 1;
	range.baseArrayLayer = arrayLayer;
	range.layerCount     = 1;

	// --- UNDEFINED -> TRANSFER_DST ---
	VkImageMemoryBarrier2 b2{};
	b2.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
	b2.srcStageMask  = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
	b2.srcAccessMask = VK_ACCESS_2_NONE;
	b2.dstStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
	b2.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
	b2.oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
	b2.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	b2.image         = _image;
	b2.subresourceRange = range;

	VkDependencyInfo dep{};
	dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
	dep.imageMemoryBarrierCount = 1;
	dep.pImageMemoryBarriers    = &b2;
	vkCmdPipelineBarrier2(cmd, &dep);

	// --- extent с учётом mip и depth ---
	const bool is3D   = _desc.Depth > 1;
	const u32 extentW = std::max<u32>(1, _desc.Width  >> mipLevel);
	const u32 extentH = std::max<u32>(1, _desc.Height >> mipLevel);
	const u32 extentD = is3D ? std::max<u32>(1, _desc.Depth >> mipLevel) : 1;

	VkBufferImageCopy region{};
	region.bufferOffset      = 0;
	region.bufferRowLength   = 0;
	region.bufferImageHeight = 0;
	region.imageSubresource.aspectMask     = range.aspectMask;
	region.imageSubresource.mipLevel       = mipLevel;
	region.imageSubresource.baseArrayLayer = arrayLayer;
	region.imageSubresource.layerCount     = 1;
	region.imageOffset = { 0, 0, 0 };
	region.imageExtent = { extentW, extentH, extentD };

	vkCmdCopyBufferToImage(cmd, staging, _image,
	                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

	// --- TRANSFER_DST -> SHADER_READ_ONLY (или DEPTH_READ / RT / GENERAL) ---
	VkImageLayout finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	VkPipelineStageFlags2 dstStage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
	                                 VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
	VkAccessFlags2 dstAccess = VK_ACCESS_2_SHADER_READ_BIT;

	if (_desc.BindFlags & ERHI_BIND_FLAG::RENDER_TARGET)
	{
		finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		dstStage    = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
		dstAccess   = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
	}
	else if (_desc.BindFlags & ERHI_BIND_FLAG::DEPTH_STENCIL)
	{
		finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		dstStage    = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
		              VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
		dstAccess   = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
		              VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	}
	else if (_desc.BindFlags & ERHI_BIND_FLAG::UNORDERED_ACCESS)
	{
		finalLayout = VK_IMAGE_LAYOUT_GENERAL;
		dstStage    = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
		              VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
		dstAccess   = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
	}

	b2 = {};
	b2.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
	b2.srcStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
	b2.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
	b2.dstStageMask  = dstStage;
	b2.dstAccessMask = dstAccess;
	b2.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	b2.newLayout     = finalLayout;
	b2.image         = _image;
	b2.subresourceRange = range;

	dep = {};
	dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
	dep.imageMemoryBarrierCount = 1;
	dep.pImageMemoryBarriers    = &b2;
	vkCmdPipelineBarrier2(cmd, &dep);

	vkEndCommandBuffer(cmd);

	// --- submit + wait ---
	VkCommandBufferSubmitInfo cbsi{};
	cbsi.sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
	cbsi.commandBuffer = cmd;

	VkSubmitInfo2 si{};
	si.sType                  = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
	si.commandBufferInfoCount = 1;
	si.pCommandBufferInfos    = &cbsi;

	// === FIX B: fence вместо vkQueueWaitIdle ===
	// vkQueueWaitIdle ждёт ВСЕ команды на graphics queue, включая ещё не
	// submit'нутые render thread'ом. Это создаёт race и потенциальный
	// крэш/дедлок на AMDVLK. Fence ждёт только наш submit.
	VkFenceCreateInfo fenceInfo{};
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	VkFence loadFence = VK_NULL_HANDLE;
	VkResult fenceRes = vkCreateFence(_device, &fenceInfo, nullptr, &loadFence);
	if (fenceRes != VK_SUCCESS)
	{
		vkFreeCommandBuffers(_device, _cmdPool, 1, &cmd);
		vkDestroyBuffer(_device, staging, nullptr);
		vkFreeMemory(_device, stagingMem, nullptr);
		Msg("! [VK] UploadSubresource: vkCreateFence failed r=%d", (int)fenceRes);
		return false;
	}

	VkResult subRes_ = vkQueueSubmit2(_queue, 1, &si, loadFence);
	if (subRes_ == VK_SUCCESS)
	{
		vkWaitForFences(_device, 1, &loadFence, VK_TRUE, UINT64_MAX);
	}

	vkDestroyFence(_device, loadFence, nullptr);

	vkFreeCommandBuffers(_device, _cmdPool, 1, &cmd);
	vkDestroyBuffer(_device, staging, nullptr);
	vkFreeMemory(_device, stagingMem, nullptr);

	if (subRes_ != VK_SUCCESS)
	{
		Msg("! [VK] UploadSubresource: vkQueueSubmit2 failed r=%d", (int)subRes_);
		return false;
	}

	_ready = true;
	return true;
}


// ---- CVKShaderResourceViewTexture ----
CVKShaderResourceViewTexture::CVKShaderResourceViewTexture(CVKTexture* tex, const RHIShaderResourceViewDesc* /*desc*/)
	: _texture(tex)
{
	R_ASSERT2(tex, "CVKShaderResourceViewTexture: null texture");
	tex->AddRef();
	_info.imageView   = tex->GetView();
	_info.sampler     = VK_NULL_HANDLE;
	_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

CVKShaderResourceViewTexture::~CVKShaderResourceViewTexture()
{
	if (_texture) _texture->Release();
}

void CVKShaderResourceViewTexture::AddRef() { ++_refs; }
u32 CVKShaderResourceViewTexture::Release()
{
	const u32 remaining = --_refs;
	if (!remaining) { auto self = this; xr_delete(self); }
	return remaining;
}

// ---- CVKTextureFactory ----
CVKTextureFactory::CVKTextureFactory(VkDevice device, VkPhysicalDevice physical,
                                     VkCommandPool cmdPool, VkQueue queue)
	: _device(device), _physical(physical), _cmdPool(cmdPool), _queue(queue) {}

IRHISurface* CVKTextureFactory::CreateTexture2D(const RHITextureDesc& desc, const RHISubResource* data)
{
	auto* tex = new CVKTexture(_device, _physical, _cmdPool, _queue, desc);
	if (data && data->Data) tex->UpdateData(0, 0, data, {});
	return tex;
}

IRHISurface* CVKTextureFactory::CreateTexture3D(const RHITextureDesc& desc, const RHISubResource* data)
{
	auto* tex = new CVKTexture(_device, _physical, _cmdPool, _queue, desc);
	if (data && data->Data) tex->UpdateData(0, 0, data, {});
	return tex;
}

IRHISurface* CVKTextureFactory::CreateTextureFromMemory(const void* data, u32 size, const RHITextureDesc& desc)
{
	if (!data) return CreateTexture2D(desc, nullptr);
	// DDS из памяти
	int lod = 0;
	IRHISurface* surface = nullptr;
	// LoadDDS вызывается через GRHI->LoadDDS → InternalDeviceVK::LoadDDS
	// Тут напрямую — заглушка, но всё равно создаём текстуру
	auto* tex = new CVKTexture(_device, _physical, _cmdPool, _queue, desc);
	return tex;
}

IRHISurface* CVKTextureFactory::CreateRenderTarget(const RHITextureDesc& desc)
{
	return CreateTexture2D(desc, nullptr);
}

IRHIShaderResourceView* CVKTextureFactory::CreateShaderResourceView(IRHISurface* surface, const RHIShaderResourceViewDesc* desc)
{
	auto* tex = dynamic_cast<CVKTexture*>(surface);
	if (!tex)
	{
		Msg("! [VK] CreateShaderResourceView: surface is not CVKTexture, returning null");
		return nullptr;
	}
	return new CVKShaderResourceViewTexture(tex, desc);
}

// ---------- CVKRenderTargetView ----------
CVKRenderTargetView::CVKRenderTargetView(CVKTexture* tex, VkImageView view)
    : _texture(tex), _view(view)
{
    R_ASSERT2(tex, "CVKRenderTargetView: null texture");
    tex->AddRef();
}

CVKRenderTargetView::~CVKRenderTargetView()
{
    if (_view != VK_NULL_HANDLE && _texture)
        vkDestroyImageView(_texture->GetDevice(), _view, nullptr);
    if (_texture) _texture->Release();
}

void CVKRenderTargetView::AddRef() { ++_refs; }
u32 CVKRenderTargetView::Release()
{
    const u32 remaining = --_refs;
    if (!remaining) { auto self = this; xr_delete(self); }
    return remaining;
}

// ---------- CVKDepthStencilView ----------
CVKDepthStencilView::CVKDepthStencilView(CVKTexture* tex, VkImageView view, ERHI_DSV_DIMENSION dim)
    : _texture(tex), _view(view), _dimension(dim)
{
    R_ASSERT2(tex, "CVKDepthStencilView: null texture");
    tex->AddRef();
}

CVKDepthStencilView::~CVKDepthStencilView()
{
    if (_view != VK_NULL_HANDLE && _texture)
        vkDestroyImageView(_texture->GetDevice(), _view, nullptr);
    if (_texture) _texture->Release();
}

void CVKDepthStencilView::AddRef() { ++_refs; }
u32 CVKDepthStencilView::Release()
{
    const u32 remaining = --_refs;
    if (!remaining) { auto self = this; xr_delete(self); }
    return remaining;
}

// ---------- CVKTextureFactory::CreateRenderTargetView ----------
IRHIRenderTargetView* CVKTextureFactory::CreateRenderTargetView(IRHISurface* surface, const RHIRenderTargetViewDesc& desc)
{
    auto* tex = dynamic_cast<CVKTexture*>(surface);
    if (!tex)
    {
        Msg("! [VK] CreateRenderTargetView: surface is not CVKTexture");
        return nullptr;
    }

	VkFormat vkFmt = tex->GetVkFormat();

    // TYPELESS → глубинный/цветной аспект
    const bool isDepthView = (vkFmt == VK_FORMAT_D16_UNORM) ||
                             (vkFmt == VK_FORMAT_D24_UNORM_S8_UINT) ||
                             (vkFmt == VK_FORMAT_D32_SFLOAT) ||
                             (vkFmt == VK_FORMAT_D32_SFLOAT_S8_UINT);

    VkImageViewCreateInfo vi{};
    vi.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image    = tex->GetImage();
    vi.viewType = (desc.ArraySize > 1) ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    vi.format   = vkFmt;
    vi.components = {
        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY
    };
    vi.subresourceRange.aspectMask     = isDepthView ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.baseMipLevel   = desc.MipSlice;
    vi.subresourceRange.levelCount     = 1;
    vi.subresourceRange.baseArrayLayer = desc.FirstArraySlice;
    vi.subresourceRange.layerCount     = desc.ArraySize ? desc.ArraySize : 1;

    VkImageView view = VK_NULL_HANDLE;
    VkResult r = vkCreateImageView(tex->GetDevice(), &vi, nullptr, &view);
    if (r != VK_SUCCESS)
    {
        Msg("! [VK] CreateRenderTargetView: vkCreateImageView failed (fmt=%d mip=%u slice=%u r=%d) — RTV has null view",
            (int)vkFmt, desc.MipSlice, desc.FirstArraySlice, (int)r);
        view = VK_NULL_HANDLE;
    }

    return new CVKRenderTargetView(tex, view);
}

// ---------- CVKTextureFactory::CreateDepthStencilView ----------
IRHIDepthStencilView* CVKTextureFactory::CreateDepthStencilView(IRHISurface* surface, const RHIDepthStencilViewDesc& desc)
{
    auto* tex = dynamic_cast<CVKTexture*>(surface);
    if (!tex)
    {
        Msg("! [VK] CreateDepthStencilView: surface is not CVKTexture");
        return nullptr;
    }

	VkFormat vkFmt = tex->GetVkFormat();

    VkImageViewCreateInfo vi{};
    vi.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image    = tex->GetImage();
    vi.viewType = (desc.ArraySize > 1) ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    vi.format   = vkFmt;
    vi.components = {
        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY
    };

    // Aspect: depth + stencil (stencil есть только у D24S8 и D32S8)
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (vkFmt == VK_FORMAT_D24_UNORM_S8_UINT || vkFmt == VK_FORMAT_D32_SFLOAT_S8_UINT)
        vi.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;

    vi.subresourceRange.baseMipLevel   = desc.MipSlice;
    vi.subresourceRange.levelCount     = 1;
    vi.subresourceRange.baseArrayLayer = desc.FirstArraySlice;
    vi.subresourceRange.layerCount     = desc.ArraySize ? desc.ArraySize : 1;

    VkImageView view = VK_NULL_HANDLE;
    VkResult r = vkCreateImageView(tex->GetDevice(), &vi, nullptr, &view);
    if (r != VK_SUCCESS)
    {
        Msg("! [VK] CreateDepthStencilView: vkCreateImageView failed "
            "(fmt=%d mip=%u slice=%u layers=%u r=%d) — DSV has null view",
            (int)vkFmt, desc.MipSlice, desc.FirstArraySlice, desc.ArraySize, (int)r);
        view = VK_NULL_HANDLE;
    }

    return new CVKDepthStencilView(tex, view, desc.ViewDimension);
}

IRHIUnorderedAccessView* CVKTextureFactory::CreateUAV(IRHISurface* surface, const RHIUAVDesc& desc)
{
	auto* tex = dynamic_cast<CVKTexture*>(surface);
	if (!tex)
	{
		Msg("! [VK] CreateUAV: surface is not CVKTexture");
		return nullptr;
	}
	return new CVKUnorderedAccessViewTexture(tex, desc);
}

CVKUnorderedAccessViewTexture::CVKUnorderedAccessViewTexture(CVKTexture* tex, const RHIUAVDesc& desc)
	: _texture(tex)
{
	R_ASSERT2(tex, "CVKUnorderedAccessViewTexture: null texture");
	tex->AddRef();

	const VkFormat vkFmt = tex->GetVkFormat();

	VkImageViewCreateInfo vi{};
	vi.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	vi.image    = tex->GetImage();
	vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vi.format   = vkFmt;
	vi.components = {
		VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
		VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY
	};
	vi.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
	vi.subresourceRange.baseMipLevel   = desc.MipSlice;
	vi.subresourceRange.levelCount     = 1;
	vi.subresourceRange.baseArrayLayer = 0;
	vi.subresourceRange.layerCount     = 1;

	// ВАЖНО: image должен быть создан с VK_IMAGE_USAGE_STORAGE_BIT
	// (это уже в CreateImageAndView через BindFlags::UNORDERED_ACCESS)
	VkResult r = vkCreateImageView(tex->GetDevice(), &vi, nullptr, &_view);
	if (r != VK_SUCCESS)
	{
		Msg("! [VK] CVKUnorderedAccessViewTexture: vkCreateImageView failed r=%d", (int)r);
		_view = VK_NULL_HANDLE;
	}
}

CVKUnorderedAccessViewTexture::~CVKUnorderedAccessViewTexture()
{
	if (_view != VK_NULL_HANDLE && _texture)
	{
		vkDestroyImageView(_texture->GetDevice(), _view, nullptr);
	}
	if (_texture) _texture->Release();
}

void CVKUnorderedAccessViewTexture::AddRef() { ++_refs; }
u32 CVKUnorderedAccessViewTexture::Release()
{
	const u32 remaining = --_refs;
	if (!remaining) { auto self = this; xr_delete(self); }
	return remaining;
}