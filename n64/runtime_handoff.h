#pragma once

#include <TFE_System/types.h>

namespace N64_Runtime
{
	enum
	{
		MAX_AGENT_NAME_LEN = 32,
		MAX_MISSION_CODE_LEN = 32,
	};

	struct RuntimeHandoffRequest
	{
		s32 version = 1;
		s32 agentIndex = 0;
		s32 missionIndex = 1; // 1-based mission index used by Dark Forces runtime.
		char agentName[MAX_AGENT_NAME_LEN] = { 0 };
		char missionCode[MAX_MISSION_CODE_LEN] = { 0 };
	};

	bool writeRuntimeHandoff(const char* path, const RuntimeHandoffRequest& request);
	bool readRuntimeHandoff(const char* path, RuntimeHandoffRequest* outRequest);
}
