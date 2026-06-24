#pragma once

#include "runtime_handoff.h"

namespace N64_Runtime
{
	enum class BeginRuntimeResult
	{
		Started = 0,
		SymbolMissing,
		InvalidRequest,
	};

	bool isRuntimeEntryAvailable();
	bool isRuntimeMenuEntryAvailable();
	bool isRuntimeFrontendFrameTickAvailable();
	bool isAdvancedFrontendRuntimeAvailable();
	BeginRuntimeResult beginFrontendNoVideos();
	bool tickFrontendFrame();
	BeginRuntimeResult beginRuntimeFromHandoff(const RuntimeHandoffRequest& request);
}
