#include "runtime_bridge.h"

#include <debug.h>
#include <TFE_System/types.h>

namespace TFE_DarkForces
{
	void startMissionFromSave(s32 levelIndex) __attribute__((weak));
	void enableCutscenes(JBool enable) __attribute__((weak));
	void startAgentMenuNoCutscenes() __attribute__((weak));
	bool beginFrontendRuntimeNoVideos() __attribute__((weak));
	void tickFrontendRuntimeFrame() __attribute__((weak));
}

namespace N64_Runtime
{
	bool isRuntimeFrontendFrameTickAvailable()
	{
		return TFE_DarkForces::tickFrontendRuntimeFrame != nullptr;
	}

	bool isRuntimeMenuEntryAvailable()
	{
		return TFE_DarkForces::startAgentMenuNoCutscenes != nullptr;
	}

	bool isAdvancedFrontendRuntimeAvailable()
	{
		return TFE_DarkForces::beginFrontendRuntimeNoVideos != nullptr &&
			TFE_DarkForces::tickFrontendRuntimeFrame != nullptr;
	}

	BeginRuntimeResult beginFrontendNoVideos()
	{
		if (!TFE_DarkForces::startAgentMenuNoCutscenes)
		{
			debugf("[runtime_bridge] symbol missing: TFE_DarkForces::startAgentMenuNoCutscenes\n");
			return BeginRuntimeResult::SymbolMissing;
		}

		if (TFE_DarkForces::beginFrontendRuntimeNoVideos)
		{
			debugf("[runtime_bridge] invoking beginFrontendRuntimeNoVideos()\n");
			const bool started = TFE_DarkForces::beginFrontendRuntimeNoVideos();
			debugf("[runtime_bridge] beginFrontendRuntimeNoVideos returned %d\n", started ? 1 : 0);
			if (!started)
			{
				return BeginRuntimeResult::InvalidRequest;
			}
			return BeginRuntimeResult::Started;
		}

		debugf("[runtime_bridge] invoking startAgentMenuNoCutscenes()\n");
		TFE_DarkForces::startAgentMenuNoCutscenes();
		debugf("[runtime_bridge] startAgentMenuNoCutscenes returned\n");
		return BeginRuntimeResult::Started;
	}

	bool tickFrontendFrame()
	{
		if (!TFE_DarkForces::tickFrontendRuntimeFrame)
		{
			debugf("[runtime_bridge] symbol missing: TFE_DarkForces::tickFrontendRuntimeFrame\n");
			return false;
		}

		TFE_DarkForces::tickFrontendRuntimeFrame();
		return true;
	}

	bool isRuntimeEntryAvailable()
	{
		return TFE_DarkForces::startMissionFromSave != nullptr;
	}

	BeginRuntimeResult beginRuntimeFromHandoff(const RuntimeHandoffRequest& request)
	{
		debugf("[runtime_bridge] begin: missionIndex=%ld missionCode=%s agentIndex=%ld\n",
			(long)request.missionIndex,
			request.missionCode[0] ? request.missionCode : "<empty>",
			(long)request.agentIndex);

		if (request.missionIndex < 1)
		{
			debugf("[runtime_bridge] invalid request: missionIndex=%ld\n", (long)request.missionIndex);
			return BeginRuntimeResult::InvalidRequest;
		}

		if (!TFE_DarkForces::startMissionFromSave)
		{
			debugf("[runtime_bridge] symbol missing: TFE_DarkForces::startMissionFromSave\n");
			return BeginRuntimeResult::SymbolMissing;
		}

		if (TFE_DarkForces::enableCutscenes)
		{
			TFE_DarkForces::enableCutscenes(JFALSE);
			debugf("[runtime_bridge] cutscene playback disabled for N64 runtime\n");
		}
		else
		{
			debugf("[runtime_bridge] optional symbol missing: TFE_DarkForces::enableCutscenes\n");
		}

		debugf("[runtime_bridge] invoking startMissionFromSave(%ld)\n", (long)request.missionIndex);
		TFE_DarkForces::startMissionFromSave(request.missionIndex);
		debugf("[runtime_bridge] startMissionFromSave returned\n");
		return BeginRuntimeResult::Started;
	}
}
