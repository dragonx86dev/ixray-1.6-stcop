#pragma once
#include "../RHI.h"

// ---------------------------------------------------------------------------
// VkStateManager — минимальная реализация IRHIStateManager.
//
// Что делает: накапливает rasterizer/depth/blend/stencil-ref, которые
// выставляет CRHI через IRHIStateManager::Set*. Ничего не передаёт в GPU —
// это произойдёт в PrepareDraw (Milestone 4), когда PSO начнут реально
// собираться. Пока служит только копилкой для будущей логики.
//
// Семантика Reset/Set повторяет D3D12StateManager:
//  - Set(nullptr) сбрасывает соответствующее состояние в дефолт;
//  - Apply() — no-op до Milestone 4.
// ---------------------------------------------------------------------------
class VkStateManager final : public IRHIStateManager
{
public:
    void Apply() override
    {
        // Реальная работа: применить к текущему PSO/rasterizer state.
        // Появится в Milestone 4 вместе с PrepareDraw.
    }

    void Reset() override
    {
        _rasterizer   = {};
        _depthStencil = {};
        _blend        = {};
        _stencilRef   = 0;
    }

    void SetRasterizerState(void* state) override
    {
        _rasterizer = state ? *static_cast<RHIRasterizerDesc*>(state) : RHIRasterizerDesc{};
    }

    void SetDepthStencilState(void* state) override
    {
        _depthStencil = state ? *static_cast<RHIDepthStencilDesc*>(state) : RHIDepthStencilDesc{};
    }

    void SetBlendState(void* state) override
    {
        _blend = state ? *static_cast<RHIBlendDesc*>(state) : RHIBlendDesc{};
    }

    void SetStencilRef(u32 NewStencilRef) override
    {
        _stencilRef = NewStencilRef;
    }

    // Аксессоры для PrepareDraw / сборки PSO (Milestone 4).
    const RHIRasterizerDesc&   GetRasterizerState()   const { return _rasterizer;   }
    const RHIDepthStencilDesc& GetDepthStencilState() const { return _depthStencil; }
    const RHIBlendDesc&        GetBlendState()        const { return _blend;        }
    u32                        GetStencilRef()        const { return _stencilRef;   }

private:
    RHIRasterizerDesc   _rasterizer   = {};
    RHIDepthStencilDesc _depthStencil = {};
    RHIBlendDesc        _blend        = {};
    u32                 _stencilRef   = 0;
};

// ---------------------------------------------------------------------------
// VkResourceCache — минимальная реализация IRHIShaderResourceStateCache.
//
// В D3D11 этот класс копит SRV per-slot-per-stage и одним махом применяет
// через PSSetShaderResources/VSSetShaderResources/… В Vulkan аналога нет:
// привязка идёт через descriptor sets, а не через «слоты» — поэтому Apply()
// здесь no-op. Копилка нужна, потому что CRHI всё равно вызывает
// SetPSResource/SetVSResource/… из шейдерного кода, и эти вызовы
// должны куда-то деться.
//
// Значения читаются в PrepareDraw (Milestone 4), когда мы будем собирать
// descriptor set для каждого draw из массива [stage][slot].
// ---------------------------------------------------------------------------
class VkResourceCache final : public IRHIShaderResourceStateCache
{
public:
    void ResetDeviceState() override
    {
        for (auto& stage : _views)
        {
            for (auto& v : stage) v = nullptr;
        }
    }

    void Apply() override
    {
        // No-op в Vulkan. Реальная привязка — в PrepareDraw через descriptor set.
    }

    void SetPSResource(u32 slot, IRHIShaderResourceView* res) override { Store(0, slot, res); }
    void SetVSResource(u32 slot, IRHIShaderResourceView* res) override { Store(1, slot, res); }
    void SetGSResource(u32 slot, IRHIShaderResourceView* res) override { Store(2, slot, res); }
    void SetHSResource(u32 slot, IRHIShaderResourceView* res) override { Store(3, slot, res); }
    void SetDSResource(u32 slot, IRHIShaderResourceView* res) override { Store(4, slot, res); }
    void SetCSResource(u32 slot, IRHIShaderResourceView* res) override { Store(5, slot, res); }

    // Аксессоры для PrepareDraw (Milestone 4).
    IRHIShaderResourceView* Get(u32 stage, u32 slot) const
    {
        if (stage >= kStages || slot >= kSlots) return nullptr;
        return _views[stage][slot];
    }

private:
    static constexpr u32 kStages = 6; // PS, VS, GS, HS, DS, CS — как в RHI_SHADERS_TYPE_SIZE
    static constexpr u32 kSlots  = u32(ERHI_MAX_TEXTURE::PixelShaderTextures); // 64

    void Store(u32 stage, u32 slot, IRHIShaderResourceView* res)
    {
        if (stage < kStages && slot < kSlots)
        {
            _views[stage][slot] = res;
        }
    }

    IRHIShaderResourceView* _views[kStages][kSlots] = {};
};