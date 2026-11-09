#pragma once
#include <cstdint>

class СVKBindings
{
public:
	static constexpr uint32_t CBBase        = 0;
	static constexpr uint32_t CBCount       = 14;
	static constexpr uint32_t SrvBase       = 14;
	static constexpr uint32_t SrvCount      = 57;
	static constexpr uint32_t SsboBase      = 71;
	static constexpr uint32_t SsboCount     = 7;
	static constexpr uint32_t SamplerBase   = 78;
	static constexpr uint32_t SamplerCount  = 16;
	static constexpr uint32_t UavBase       = 94;
	static constexpr uint32_t UavCount      = 8;
	static constexpr uint32_t UserSsboBase  = 102;
	static constexpr uint32_t UserSsboCount = 8;
	static constexpr uint32_t TotalBindings = 110;
};
