#pragma once

// camy
#include "camy_base.hpp"
#include "gpu_backend.hpp"

namespace camy
{
	namespace hidden
	{
		extern GPUBackend gpu;
	}

	bool init(u32 adapter_index = 0);
	
	void shutdown();
}