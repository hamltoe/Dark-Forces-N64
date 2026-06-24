#include <libdragon.h>

#include <cstring>

#include <TFE_System/platformMain.h>

#include "runtime_handoff.h"
#include "runtime_bridge.h"
#include "mission_catalog.h"

namespace
{
	static constexpr const char* c_handoffPath = "sd:/TFEBEGIN.CFG";

	bool s_running = true;
	s32 s_selectedMission = 1;
	char s_lastStatus[96] = "Ready";
	bool s_attemptedAutoMenuLaunch = false;
	bool s_runtimeFrontendTakeover = false;

	bool runtimeLinkBuildEnabled()
	{
	#if defined(TFE_N64_LINK_DF_RUNTIME) && TFE_N64_LINK_DF_RUNTIME
		return true;
	#else
		return false;
	#endif
	}

	bool missionProbeBuildEnabled()
	{
	#if defined(TFE_N64_LINK_MISSION_PROBE) && TFE_N64_LINK_MISSION_PROBE
		return true;
	#else
		return false;
	#endif
	}

	void setStatus(const char* msg);

	N64_Runtime::BeginRuntimeResult requestMenuLaunchNoVideos(const char* triggerLabel, bool autoLaunch)
	{
		const char* trigger = triggerLabel ? triggerLabel : "menu request";
		debugf("[runtime] %s -> launch main menu without videos\n", trigger);

		const N64_Runtime::BeginRuntimeResult menuResult = N64_Runtime::beginFrontendNoVideos();
		debugf("[runtime] menu begin result=%d\n", (int)menuResult);

		if (menuResult == N64_Runtime::BeginRuntimeResult::Started)
		{
			if (N64_Runtime::isAdvancedFrontendRuntimeAvailable())
			{
				s_runtimeFrontendTakeover = true;
				debugf("[runtime] frontend takeover enabled (launcher loop paused)\n");
				setStatus(autoLaunch ? "Auto menu launch requested (videos skipped)" : "Menu launch requested (videos skipped)");
			}
			else
			{
				debugf("[runtime] advanced frontend runtime unavailable; launcher loop remains active\n");
				setStatus(autoLaunch ? "Auto menu requested (advanced runtime missing)" : "Menu requested (advanced runtime missing)");
			}
		}
		else if (menuResult == N64_Runtime::BeginRuntimeResult::SymbolMissing)
		{
			s_runtimeFrontendTakeover = false;
			setStatus("Menu symbol missing (runtime link)");
		}
		else
		{
			s_runtimeFrontendTakeover = false;
			setStatus("Menu launch blocked");
		}

		return menuResult;
	}

	void setStatus(const char* msg)
	{
		const char* status = msg ? msg : "";
		if (strcmp(s_lastStatus, status) == 0)
		{
			return;
		}

		strncpy(s_lastStatus, status, sizeof(s_lastStatus) - 1);
		s_lastStatus[sizeof(s_lastStatus) - 1] = 0;
		debugf("n64 status: %s\n", s_lastStatus[0] ? s_lastStatus : "<empty>");
	}

	const char* missionCodeFromIndex(s32 missionIndex)
	{
		const char* code = N64_MissionCatalog::getMissionCodeByIndex(missionIndex);
		if (!code)
		{
			return "UNKNOWN";
		}
		return code;
	}

	s32 missionIndexFromCode(const char* missionCode)
	{
		return N64_MissionCatalog::getMissionIndexByCode(missionCode);
	}

	s32 clampMissionIndex(s32 missionIndex)
	{
		const s32 missionCount = N64_MissionCatalog::getMissionCount();
		if (missionCount <= 0)
		{
			return 1;
		}
		if (missionIndex < 1) { return 1; }
		if (missionIndex > missionCount) { return missionCount; }
		return missionIndex;
	}

	bool normalizeAndValidateRequest(N64_Runtime::RuntimeHandoffRequest* request, bool* outCorrectedFromCode)
	{
		if (!request)
		{
			debugf("[handoff] normalize skipped: null request\n");
			return false;
		}

		debugf("[handoff] normalize in: idx=%ld code=%s\n",
			(long)request->missionIndex,
			request->missionCode[0] ? request->missionCode : "<empty>");

		if (outCorrectedFromCode)
		{
			*outCorrectedFromCode = false;
		}

		s32 normalizedIndex = clampMissionIndex(request->missionIndex);
		const s32 codeIndex = missionIndexFromCode(request->missionCode);
		debugf("[handoff] normalize lookup: codeIndex=%ld missionCount=%ld\n",
			(long)codeIndex,
			(long)N64_MissionCatalog::getMissionCount());

		if (request->missionCode[0] && codeIndex == 0)
		{
			debugf("[handoff] normalize failed: unknown mission code '%s'\n", request->missionCode);
			return false;
		}

		if (codeIndex > 0)
		{
			if (normalizedIndex != codeIndex && outCorrectedFromCode)
			{
				*outCorrectedFromCode = true;
				debugf("[handoff] normalize corrected index from %ld to %ld via code\n",
					(long)normalizedIndex,
					(long)codeIndex);
			}
			normalizedIndex = codeIndex;
		}

		request->missionIndex = normalizedIndex;
		strncpy(request->missionCode, missionCodeFromIndex(normalizedIndex), N64_Runtime::MAX_MISSION_CODE_LEN - 1);
		request->missionCode[N64_Runtime::MAX_MISSION_CODE_LEN - 1] = 0;
		debugf("[handoff] normalize out: idx=%ld code=%s\n",
			(long)request->missionIndex,
			request->missionCode[0] ? request->missionCode : "<empty>");
		return true;
	}

	N64_Runtime::RuntimeHandoffRequest makeRequest()
	{
		N64_Runtime::RuntimeHandoffRequest request = {};
		request.version = 1;
		request.agentIndex = 0;
		request.missionIndex = s_selectedMission;
		strncpy(request.agentName, "N64_AGENT", N64_Runtime::MAX_AGENT_NAME_LEN - 1);
		strncpy(request.missionCode, missionCodeFromIndex(s_selectedMission), N64_Runtime::MAX_MISSION_CODE_LEN - 1);
		return request;
	}

	void handlePlatformEvent(const void* eventData, void* userData)
	{
		(void)userData;
		if (TFE_Platform::dispatchPlatformEvent(eventData))
		{
			s_running = false;
		}
	}
}

int main(void)
{
	console_init();
	console_set_render_mode(RENDER_MANUAL);
	console_set_debug(false);

	if (!TFE_Platform::initDesktopRuntime())
	{
		debugf("n64 lane: platform init failed\n");
		return 1;
	}
	debugf("n64 lane: platform init ok\n");

	if (!N64_MissionCatalog::loadFromDarkGob())
	{
		setStatus("Mission catalog load failed");
		debugf("n64 lane: mission catalog load failed: %s\n", N64_MissionCatalog::getLastError());
	}
	else
	{
		s_selectedMission = clampMissionIndex(s_selectedMission);
		const s32 missionCount = N64_MissionCatalog::getMissionCount();
		debugf("n64 lane: mission catalog loaded (%ld missions)\n", (long)missionCount);
		if (missionCount > 0)
		{
			debugf("n64 lane: mission range first=%s last=%s\n",
				missionCodeFromIndex(1),
				missionCodeFromIndex(missionCount));
		}
	}

	debugf("n64 lane: platform runtime initialized\n");
	debugf("n64 lane: runtime link build is %s\n", runtimeLinkBuildEnabled() ? "ENABLED" : "DISABLED");
	debugf("n64 lane: mission probe build is %s\n", missionProbeBuildEnabled() ? "ENABLED" : "DISABLED");
	const bool runtimeEntryAvailable = N64_Runtime::isRuntimeEntryAvailable();
	const bool runtimeMenuEntryAvailable = N64_Runtime::isRuntimeMenuEntryAvailable();
	debugf("n64 lane: runtime entry symbol is %s\n", runtimeEntryAvailable ? "AVAILABLE" : "MISSING");
	debugf("n64 lane: runtime menu symbol is %s\n", runtimeMenuEntryAvailable ? "AVAILABLE" : "MISSING");
	if (!runtimeEntryAvailable)
	{
		debugf("n64 lane: missing provider TFE_DarkForces::startMissionFromSave (expected from Dark Forces runtime link)\n");
	}
	if (!runtimeMenuEntryAvailable)
	{
		debugf("n64 lane: missing provider TFE_DarkForces::startAgentMenuNoCutscenes (expected from Dark Forces runtime link)\n");
		if (!runtimeLinkBuildEnabled())
		{
			debugf("n64 lane: menu launch disabled in build lane (set TFE_N64_LINK_DF_RUNTIME=1)\n");
			setStatus("Menu disabled in build lane");
		}
	}
	else
	{
		s_attemptedAutoMenuLaunch = true;
		requestMenuLaunchNoVideos("auto boot", true);
	}

	while (s_running)
	{
		TFE_Platform::pumpEvents(handlePlatformEvent, nullptr);
		if (s_runtimeFrontendTakeover)
		{
			if (!N64_Runtime::tickFrontendFrame())
			{
				s_runtimeFrontendTakeover = false;
				setStatus("Runtime tick unavailable");
				debugf("[runtime] frontend takeover disabled: runtime tick missing\n");
			}
			continue;
		}

		joypad_buttons_t pressed = joypad_get_buttons_pressed(JOYPAD_PORT_1);

		if (pressed.d_right)
		{
			const s32 missionCount = N64_MissionCatalog::getMissionCount();
			if (missionCount <= 0)
			{
				setStatus("No mission catalog loaded");
			}
			else
			{
				s_selectedMission++;
				if (s_selectedMission > missionCount)
				{
					s_selectedMission = 1;
				}
				setStatus("Mission advanced");
				debugf("[input] mission advanced -> idx=%ld code=%s\n",
					(long)s_selectedMission,
					missionCodeFromIndex(s_selectedMission));
			}
		}
		if (pressed.d_left)
		{
			const s32 missionCount = N64_MissionCatalog::getMissionCount();
			if (missionCount <= 0)
			{
				setStatus("No mission catalog loaded");
			}
			else
			{
				s_selectedMission--;
				if (s_selectedMission < 1)
				{
					s_selectedMission = missionCount;
				}
				setStatus("Mission reversed");
				debugf("[input] mission reversed -> idx=%ld code=%s\n",
					(long)s_selectedMission,
					missionCodeFromIndex(s_selectedMission));
			}
		}

		if (pressed.a)
		{
			const N64_Runtime::RuntimeHandoffRequest request = makeRequest();
			debugf("[handoff] write request: idx=%ld code=%s agentIndex=%ld agentName=%s\n",
				(long)request.missionIndex,
				request.missionCode[0] ? request.missionCode : "<empty>",
				(long)request.agentIndex,
				request.agentName[0] ? request.agentName : "<empty>");
			if (N64_Runtime::writeRuntimeHandoff(c_handoffPath, request))
			{
				setStatus("Handoff written to sd:/TFEBEGIN.CFG");
				debugf("[handoff] write success: path=%s\n", c_handoffPath);
			}
			else
			{
				setStatus("Write failed (check SD/debug_init_sdfs)");
				debugf("[handoff] write failed: path=%s\n", c_handoffPath);
			}
		}

		if (pressed.b)
		{
			N64_Runtime::RuntimeHandoffRequest loaded = {};
			if (N64_Runtime::readRuntimeHandoff(c_handoffPath, &loaded))
			{
				debugf("[handoff] read raw: idx=%ld code=%s agentIndex=%ld agentName=%s\n",
					(long)loaded.missionIndex,
					loaded.missionCode[0] ? loaded.missionCode : "<empty>",
					(long)loaded.agentIndex,
					loaded.agentName[0] ? loaded.agentName : "<empty>");
				bool correctedFromCode = false;
				if (!normalizeAndValidateRequest(&loaded, &correctedFromCode))
				{
					setStatus("Handoff invalid mission code");
					debugf("[handoff] read rejected: invalid mission code\n");
				}
				else
				{
					debugf("[handoff] read normalized: idx=%ld code=%s corrected=%d\n",
						(long)loaded.missionIndex,
						loaded.missionCode[0] ? loaded.missionCode : "<empty>",
						correctedFromCode ? 1 : 0);
					s_selectedMission = loaded.missionIndex;
					if (correctedFromCode)
					{
						setStatus("Handoff loaded (code corrected index)");
					}
					else
					{
						setStatus("Handoff loaded");
					}
				}
			}
			else
			{
				setStatus("Read failed (no handoff file yet)");
				debugf("[handoff] read failed: path=%s\n", c_handoffPath);
			}
		}

		if (pressed.start)
		{
			requestMenuLaunchNoVideos("start pressed", false);
		}

		if (pressed.z)
		{
			debugf("[runtime] z pressed -> direct mission launch\n");
			N64_Runtime::RuntimeHandoffRequest request = {};
			const bool loadedFromFile = N64_Runtime::readRuntimeHandoff(c_handoffPath, &request);
			if (!loadedFromFile)
			{
				request = makeRequest();
			}
			debugf("[runtime] source=%s initial idx=%ld code=%s\n",
				loadedFromFile ? "handoff-file" : "live-fallback",
				(long)request.missionIndex,
				request.missionCode[0] ? request.missionCode : "<empty>");

			bool correctedFromCode = false;
			if (!normalizeAndValidateRequest(&request, &correctedFromCode))
			{
				setStatus("Runtime blocked (invalid mission code)");
				debugf("[runtime] blocked: invalid mission request\n");
			}
			else
			{
				s_selectedMission = request.missionIndex;
				debugf("[runtime] begin request idx=%ld code=%s corrected=%d source=%s\n",
					(long)request.missionIndex,
					request.missionCode[0] ? request.missionCode : "<empty>",
					correctedFromCode ? 1 : 0,
					loadedFromFile ? "handoff-file" : "live-fallback");

				const N64_Runtime::BeginRuntimeResult result = N64_Runtime::beginRuntimeFromHandoff(request);
				debugf("[runtime] begin result=%d\n", (int)result);
				if (result == N64_Runtime::BeginRuntimeResult::Started)
				{
					if (N64_Runtime::isRuntimeFrontendFrameTickAvailable())
					{
						s_runtimeFrontendTakeover = true;
						setStatus("Runtime started; mission loop takeover active");
						debugf("[runtime] mission takeover enabled via tickFrontendRuntimeFrame\n");
					}
					else if (loadedFromFile)
					{
						if (correctedFromCode)
						{
							setStatus("Runtime started (file code->index)");
						}
						else
						{
							setStatus("Runtime started (handoff file)");
						}
					}
					else
					{
						setStatus("Runtime started (live fallback)");
					}
				}
				else if (result == N64_Runtime::BeginRuntimeResult::SymbolMissing)
				{
					if (loadedFromFile)
					{
						if (correctedFromCode)
						{
							setStatus("Runtime symbol missing (file code->index)");
						}
						else
						{
							setStatus("Runtime symbol missing (file request)");
						}
					}
					else
					{
						setStatus("Runtime symbol missing (live fallback)");
					}
				}
				else
				{
					setStatus("Runtime request invalid after verify");
				}
			}
		}

		console_clear();
		printf("Dark Forces N64\n");
		printf("Step 3 runtime handoff bootstrap\n\n");
		printf("Platform seam active\n");
		printf("Input path: libdragon -> TFE_Platform\n");
		printf("Runtime takeover: %s\n", s_runtimeFrontendTakeover ? "active" : "inactive");
		if (s_attemptedAutoMenuLaunch)
		{
			printf("Auto menu launch: attempted\n");
		}
		else
		{
			printf("Auto menu launch: %s\n", runtimeLinkBuildEnabled() ? "pending (symbol missing)" : "disabled (build lane)" );
		}
		printf("Mission catalog: %ld entries\n", (long)N64_MissionCatalog::getMissionCount());
		printf("Mission %02ld  Code %s\n", (long)s_selectedMission, missionCodeFromIndex(s_selectedMission));
		printf("A: write handoff  B: read handoff\n");
		printf("Start: retry menu launch (videos skipped)\n");
		printf("Z: verify + begin mission bridge\n");
		printf("Status: %s\n", s_lastStatus);
		printf("Press reset to exit\n");
		console_render();
	}

	N64_MissionCatalog::unload();
	TFE_Platform::shutdownDesktopRuntime();
	return 0;
}
