#include <debug.h>
#include <libdragon.h>

#include "mission_catalog.h"
#include "runtime_bridge.h"

#ifdef TICKS_PER_SECOND
#undef TICKS_PER_SECOND
#endif

#include <TFE_DarkForces/mission.h>
#include <TFE_DarkForces/agent.h>
#include <TFE_DarkForces/util.h>
#include <TFE_DarkForces/time.h>
#include <TFE_Jedi/InfSystem/infSystem.h>
#include <TFE_Jedi/Renderer/jediRenderer.h>
#include <TFE_Memory/chunkedArray.h>
#include <TFE_Memory/memoryRegion.h>
#include <TFE_Settings/settings.h>
#include <TFE_System/profiler.h>
#include <TFE_System/system.h>
#include <TFE_System/types.h>
#include <TFE_Jedi/Task/task.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

struct ChunkedArray
{
	u32 elemSize;
	std::vector<u8*> slots;
	std::vector<u8*> freeSlots;
};

namespace TFE_Console
{
	void registerCVarBool(const char* name, u32 flags, bool* var, const char* helpString)
	{
		(void)name;
		(void)flags;
		(void)var;
		(void)helpString;
	}
}

namespace TFE_Settings
{
	TFE_Settings_Game* getGameSettings()
	{
		static TFE_Settings_Game s_gameSettings = {};
		return &s_gameSettings;
	}
}

namespace TFE_System
{
	f64 getTime()
	{
		// The transitional task scheduler only needs monotonic progression.
		return (f64)TFE_DarkForces::s_curTick;
	}

	u64 getCurrentTimeInTicks()
	{
		return (u64)TFE_DarkForces::s_curTick;
	}
}

namespace TFE_Profiler
{
	u32 beginZone(const char* name, const char* func, u32 lineNumber)
	{
		(void)name;
		(void)func;
		(void)lineNumber;
		return 0;
	}

	void endZone(u32 id, u64 dt)
	{
		(void)id;
		(void)dt;
	}
}

namespace TFE_Input
{
	bool isReplaySystemLive()
	{
		return false;
	}

	bool isDemoPlayback()
	{
		return false;
	}

	bool isRecording()
	{
		return false;
	}

	void loadReplay()
	{
	}

	void startRecording()
	{
	}

	void clearAccumulatedMouseMove()
	{
	}
}

namespace TFE_DarkForces
{
	void startAgentMenuNoCutscenes() __attribute__((weak));
	Tick __attribute__((weak)) s_curTick = 0;
	fixed16_16 __attribute__((weak)) s_curTickFract = 0;
	Tick __attribute__((weak)) s_prevTick = 0;
	fixed16_16 __attribute__((weak)) s_prevTickFract = 0;
	fixed16_16 __attribute__((weak)) s_deltaTime = 0;
	fixed16_16 __attribute__((weak)) s_frameTicks[13] = { 0 };
	f64 __attribute__((weak)) s_timeAccum = 0.0;
}

namespace TFE_Memory
{
	ChunkedArray* createChunkedArray(u32 elemSize, u32 elemPerChunk, u32 initChunkCount, MemoryRegion* region)
	{
		(void)elemPerChunk;
		(void)initChunkCount;
		(void)region;

		ChunkedArray* arr = new ChunkedArray();
		arr->elemSize = elemSize;
		return arr;
	}

	void freeChunkedArray(ChunkedArray* arr)
	{
		if (!arr)
		{
			return;
		}

		for (u8* slot : arr->slots)
		{
			std::free(slot);
		}
		delete arr;
	}

	void chunkedArrayClear(ChunkedArray* arr)
	{
		if (!arr)
		{
			return;
		}

		for (u8* slot : arr->slots)
		{
			std::free(slot);
		}
		arr->slots.clear();
		arr->freeSlots.clear();
	}

	void* allocFromChunkedArray(ChunkedArray* arr)
	{
		if (!arr)
		{
			return nullptr;
		}

		u8* slot = nullptr;
		if (!arr->freeSlots.empty())
		{
			slot = arr->freeSlots.back();
			arr->freeSlots.pop_back();
		}
		else
		{
			slot = (u8*)std::calloc(1, arr->elemSize);
			if (!slot)
			{
				return nullptr;
			}
			arr->slots.push_back(slot);
		}

		std::memset(slot, 0, arr->elemSize);
		return slot;
	}

	void freeToChunkedArray(ChunkedArray* arr, void* ptr)
	{
		if (!arr || !ptr)
		{
			return;
		}

		u8* slot = (u8*)ptr;
		auto it = std::find(arr->freeSlots.begin(), arr->freeSlots.end(), slot);
		if (it == arr->freeSlots.end())
		{
			arr->freeSlots.push_back(slot);
		}
	}

	u32 chunkedArraySize(ChunkedArray* arr)
	{
		if (!arr)
		{
			return 0;
		}
		return (u32)arr->slots.size();
	}

	u32 chunkedArrayCount(ChunkedArray* arr)
	{
		if (!arr)
		{
			return 0;
		}
		return (u32)(arr->slots.size() - arr->freeSlots.size());
	}

	void* chunkedArrayGet(ChunkedArray* arr, u32 index)
	{
		if (!arr || index >= arr->slots.size())
		{
			return nullptr;
		}
		return arr->slots[index];
	}

	void serialize(ChunkedArray* arr, FileStream* file)
	{
		(void)arr;
		(void)file;
	}

	ChunkedArray* restore(FileStream* file, MemoryRegion* region)
	{
		(void)file;
		(void)region;
		return nullptr;
	}

	s32 getSlotIndex(ChunkedArray* arr, u8* ptr)
	{
		if (!arr || !ptr)
		{
			return -1;
		}

		for (u32 i = 0; i < arr->slots.size(); ++i)
		{
			if (arr->slots[i] == ptr)
			{
				return (s32)i;
			}
		}
		return -1;
	}
}

MemoryRegion* __attribute__((weak)) s_gameRegion = nullptr;
MemoryRegion* __attribute__((weak)) s_levelRegion = nullptr;

void reticle_enable(bool enable)
{
	debugf("[df_runtime_stubs] reticle_enable(%d)\n", enable ? 1 : 0);
}

namespace TFE_Memory
{
	void region_clear(MemoryRegion* region)
	{
		(void)region;
		debugf("[df_runtime_stubs] region_clear(level)\n");
	}
}

namespace TFE_DarkForces
{
	JBool __attribute__((weak)) s_gamePaused = JTRUE;
	GameMissionMode __attribute__((weak)) s_missionMode = MISSION_MODE_LOADING;
	TextureData* __attribute__((weak)) s_defaultLoadScreen = nullptr;
	TextureData* __attribute__((weak)) s_loadScreen = nullptr;
	u8 __attribute__((weak)) s_loadingScreenPal[768] = { 0 };
	u8 __attribute__((weak)) s_levelPalette[768] = { 0 };
	u8 __attribute__((weak)) s_basePalette[768] = { 0 };
	AgentData s_agentData[MAX_AGENT_COUNT] = {};
	s32 s_agentId = 0;
	JBool s_invalidLevelIndex = JFALSE;
	JBool s_levelComplete = JFALSE;

	static Task* s_runtimeMissionTask = nullptr;
	static Task* s_runtimeLoadMissionTask = nullptr;
	static JBool s_runtimeExitLevel = JFALSE;
	static JBool s_runtimeLoadingFromSave = JFALSE;
	static JBool s_runtimeFrontendActive = JFALSE;
	static JBool s_runtimeFrontendMissionLoop = JFALSE;
	static s32 s_runtimeFrontendMissionIndex = 1;
	static char s_runtimeFrontendStatus[96] = "Ready";

	static void ensureRuntimeAgentDefaults()
	{
		if (s_agentData[0].name[0])
		{
			return;
		}

		strncpy(s_agentData[0].name, "N64_AGENT", sizeof(s_agentData[0].name) - 1);
		s_agentData[0].difficulty = 1;
		s_agentData[0].selectedMission = 1;
		s_agentData[0].nextMission = 1;
	}

	static void setRuntimeFrontendStatus(const char* msg)
	{
		const char* status = msg ? msg : "";
		if (strcmp(s_runtimeFrontendStatus, status) == 0)
		{
			return;
		}

		strncpy(s_runtimeFrontendStatus, status, sizeof(s_runtimeFrontendStatus) - 1);
		s_runtimeFrontendStatus[sizeof(s_runtimeFrontendStatus) - 1] = 0;
		debugf("[df_runtime_stubs] frontend status: %s\n",
			s_runtimeFrontendStatus[0] ? s_runtimeFrontendStatus : "<empty>");
	}

	static s32 clampRuntimeFrontendMissionIndex(s32 missionIndex)
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

	static const char* runtimeFrontendMissionCode(s32 missionIndex)
	{
		const char* code = N64_MissionCatalog::getMissionCodeByIndex(missionIndex);
		return code ? code : "UNKNOWN";
	}

	void updateTime()
	{
		s_prevTick = s_curTick;
		s_prevTickFract = s_curTickFract;
		TFE_Jedi::task_updateTime();
		s_curTick++;
	}

	void __attribute__((weak)) agent_setNextLevelByIndex(s32 index)
	{
		ensureRuntimeAgentDefaults();
		s_agentData[s_agentId].nextMission = clampRuntimeFrontendMissionIndex(index);
		s_agentData[s_agentId].selectedMission = s_agentData[s_agentId].nextMission;
	}

	s32 __attribute__((weak)) agent_getLevelIndex()
	{
		ensureRuntimeAgentDefaults();
		return clampRuntimeFrontendMissionIndex(s_agentData[s_agentId].nextMission);
	}

	s32 __attribute__((weak)) agent_getLevelIndexFromName(const char* name)
	{
		if (!name || !name[0])
		{
			return 0;
		}

		const s32 idx = N64_MissionCatalog::getMissionIndexByCode(name);
		return idx > 0 ? idx : 0;
	}

	const char* __attribute__((weak)) agent_getLevelName()
	{
		return runtimeFrontendMissionCode(agent_getLevelIndex());
	}

	const char* __attribute__((weak)) agent_getLevelDisplayName()
	{
		return agent_getLevelName();
	}

	void __attribute__((weak)) agent_setLevelComplete(JBool complete)
	{
		s_levelComplete = complete ? JTRUE : JFALSE;
	}

	void __attribute__((weak)) agent_readSavedDataForLevel(s32 agentId, s32 levelIndex)
	{
		(void)agentId;
		agent_setNextLevelByIndex(levelIndex);
	}

	void __attribute__((weak)) agent_saveLevelCompletion(u8 diff, s32 levelIndex)
	{
		(void)diff;
		(void)levelIndex;
	}

	s32 __attribute__((weak)) agent_saveInventory(s32 agentId, s32 nextLevel)
	{
		(void)agentId;
		(void)nextLevel;
		return 0;
	}

	void __attribute__((weak)) agent_updateAgentSavedData()
	{
	}

	JBool __attribute__((weak)) cutscene_play(s32 cutscene)
	{
		(void)cutscene;
		return JFALSE;
	}

	JBool __attribute__((weak)) cutscene_update()
	{
		return JFALSE;
	}

	void __attribute__((weak)) missionBriefing_start(const char* archive, const char* bgAnim, const char* mission, const char* palette, s32 skill, LangHotkeys* langKeys)
	{
		(void)archive;
		(void)bgAnim;
		(void)mission;
		(void)palette;
		(void)skill;
		(void)langKeys;
	}

	JBool __attribute__((weak)) missionBriefing_update(s32* skill, JBool* abort)
	{
		if (skill)
		{
			*skill = (s32)s_agentData[s_agentId].difficulty;
		}
		if (abort)
		{
			*abort = JFALSE;
		}
		return JFALSE;
	}

	JBool __attribute__((weak)) agentMenu_update(s32* levelIndex)
	{
		ensureRuntimeAgentDefaults();
		const s32 missionCount = N64_MissionCatalog::getMissionCount();
		if (missionCount <= 0)
		{
			s_invalidLevelIndex = JTRUE;
			if (levelIndex)
			{
				*levelIndex = 1;
			}
			return JTRUE;
		}

		joypad_buttons_t pressed = joypad_get_buttons_pressed(JOYPAD_PORT_1);
		s32 missionIndex = clampRuntimeFrontendMissionIndex(s_agentData[s_agentId].nextMission);
		if (pressed.d_right)
		{
			missionIndex++;
			if (missionIndex > missionCount)
			{
				missionIndex = 1;
			}
		}
		if (pressed.d_left)
		{
			missionIndex--;
			if (missionIndex < 1)
			{
				missionIndex = missionCount;
			}
		}

		agent_setNextLevelByIndex(missionIndex);
		if (levelIndex)
		{
			*levelIndex = missionIndex;
		}

		console_clear();
		printf("Dark Forces N64\n");
		printf("Agent Menu Bridge\n\n");
		printf("Mission %02ld  Code %s\n",
			(long)missionIndex,
			runtimeFrontendMissionCode(missionIndex));
		printf("D-Pad Left/Right: mission select\n");
		printf("A/Start/Z: confirm mission\n");
		console_render();

		if (pressed.a || pressed.start || pressed.z)
		{
			s_invalidLevelIndex = JFALSE;
			return JFALSE;
		}

		return JTRUE;
	}

	void __attribute__((weak)) missionBriefing_cleanup()
	{
	}

	#if !(defined(TFE_N64_LINK_MISSION_PROBE) && TFE_N64_LINK_MISSION_PROBE)
	static void mission_runtimeTask(MessageType msg)
	{
		(void)msg;
		task_begin;
		{
			static JBool s_loggedRuntimeTaskStart = JFALSE;
			static Tick s_lastRuntimeHeartbeat = 0;
			if (!s_loggedRuntimeTaskStart)
			{
				s_loggedRuntimeTaskStart = JTRUE;
				s_lastRuntimeHeartbeat = s_curTick;
				debugf("[df_runtime_stubs] mission_runtimeTask active tick=%ld\n", (long)s_curTick);
			}

			while (!s_runtimeExitLevel)
			{
				if ((s_curTick - s_lastRuntimeHeartbeat) >= 145)
				{
					s_lastRuntimeHeartbeat = s_curTick;
					debugf("[df_runtime_stubs] mission_runtimeTask heartbeat tick=%ld tasks=%ld paused=%d\n",
						(long)s_curTick,
						(long)TFE_Jedi::task_getCount(),
						(int)s_gamePaused);
				}
				task_yield(TASK_NO_DELAY);
			}

			debugf("[df_runtime_stubs] mission_runtimeTask exiting tick=%ld\n", (long)s_curTick);
		}
		task_end;
	}

	void __attribute__((weak)) mission_setLoadingFromSave()
	{
		s_runtimeLoadingFromSave = JTRUE;
		s_runtimeExitLevel = JFALSE;
		s_runtimeMissionTask = nullptr;
		s_runtimeLoadMissionTask = nullptr;
		s_runtimeFrontendActive = JTRUE;
		s_runtimeFrontendMissionLoop = JTRUE;
		setRuntimeFrontendStatus("Mission runtime initializing");
		debugf("[df_runtime_stubs] mission_setLoadingFromSave()\n");
	}

	void __attribute__((weak)) mission_startTaskFunc(MessageType msg)
	{
		task_begin;
		{
			debugf("[df_runtime_stubs] mission_startTaskFunc(msg=%d) loadTask=%p taskCount=%ld\n",
				(int)msg,
				(void*)s_runtimeLoadMissionTask,
				(long)TFE_Jedi::task_getCount());

			s_missionMode = MISSION_MODE_MAIN;

			if (!s_runtimeMissionTask)
			{
				mission_setupTasks();
				debugf("[df_runtime_stubs] mission_startTaskFunc() bootstrapped mission runtime task\n");
			}
			else
			{
				debugf("[df_runtime_stubs] mission_startTaskFunc() runtime task already active (%p)\n", (void*)s_runtimeMissionTask);
			}
		}
		task_end;
	}

	void __attribute__((weak)) mission_setLoadMissionTask(Task* task)
	{
		s_runtimeLoadMissionTask = task;
		debugf("[df_runtime_stubs] mission_setLoadMissionTask(%p)\n", (void*)task);
	}

	void __attribute__((weak)) mission_exitLevel()
	{
		s_runtimeExitLevel = JTRUE;
		debugf("[df_runtime_stubs] mission_exitLevel()\n");
	}

	void __attribute__((weak)) mission_setExitLevel(JBool exitLevel)
	{
		s_runtimeExitLevel = exitLevel ? JTRUE : JFALSE;
		if (s_runtimeExitLevel)
		{
			s_runtimeFrontendMissionLoop = JFALSE;
			setRuntimeFrontendStatus("Mission loop exit requested");
		}
		debugf("[df_runtime_stubs] mission_setExitLevel(%d)\n", (int)s_runtimeExitLevel);
	}

	void __attribute__((weak)) mission_pause(JBool pause)
	{
		s_gamePaused = pause ? JTRUE : JFALSE;
		TFE_Jedi::task_pause(s_gamePaused);
		debugf("[df_runtime_stubs] mission_pause(%d)\n", (int)s_gamePaused);
	}

	void __attribute__((weak)) mission_setupTasks()
	{
		// Transitional runtime path: keep mission bootstrap non-empty by using
		// the real task and INF modules even before full mission.cpp is link-ready.
		if (!s_runtimeLoadingFromSave)
		{
			TFE_Jedi::inf_clearState();
		}

		s_missionMode = MISSION_MODE_MAIN;
		s_gamePaused = JFALSE;
		s_runtimeExitLevel = JFALSE;
		s_runtimeFrontendActive = JTRUE;
		s_runtimeFrontendMissionLoop = JTRUE;

		if (!s_runtimeMissionTask)
		{
			s_runtimeMissionTask = TFE_Jedi::createTask("n64 mission runtime", mission_runtimeTask, JFALSE);
		}

		if (s_runtimeLoadMissionTask)
		{
			TFE_Jedi::task_makeActive(s_runtimeLoadMissionTask);
		}

		s_runtimeLoadingFromSave = JFALSE;
		setRuntimeFrontendStatus("Mission runtime running");
		debugf("[df_runtime_stubs] mission_setupTasks() runtimeTask=%p loadTask=%p taskCount=%ld\n",
			(void*)s_runtimeMissionTask,
			(void*)s_runtimeLoadMissionTask,
			(long)TFE_Jedi::task_getCount());
	}
	#endif

	bool __attribute__((weak)) beginFrontendRuntimeNoVideos()
	{
		s_runtimeFrontendActive = JTRUE;
		s_runtimeFrontendMissionLoop = JFALSE;
		s_runtimeExitLevel = JFALSE;
		s_runtimeFrontendMissionIndex = clampRuntimeFrontendMissionIndex(s_runtimeFrontendMissionIndex);
		setRuntimeFrontendStatus("Runtime frontend active");

		if (startAgentMenuNoCutscenes)
		{
			startAgentMenuNoCutscenes();
			debugf("[df_runtime_stubs] beginFrontendRuntimeNoVideos -> startAgentMenuNoCutscenes()\n");
		}
		else
		{
			debugf("[df_runtime_stubs] beginFrontendRuntimeNoVideos -> menu symbol missing, using local frontend\n");
		}

		return true;
	}

	void __attribute__((weak)) mission_serialize(Stream* stream)
	{
		(void)stream;
		debugf("[df_runtime_stubs] mission_serialize()\n");
	}

	void __attribute__((weak)) mission_serializeColorMap(Stream* stream)
	{
		(void)stream;
		debugf("[df_runtime_stubs] mission_serializeColorMap()\n");
	}

	void __attribute__((weak)) setScreenFxLevels(s32 healthFx, s32 shieldFx, s32 flashFx)
	{
		(void)healthFx;
		(void)shieldFx;
		(void)flashFx;
	}

	void __attribute__((weak)) disableNightVisionInternal() {}
	void __attribute__((weak)) enableMask() {}
	void __attribute__((weak)) enableCleats() {}
	void __attribute__((weak)) enableNightVision() {}
	void __attribute__((weak)) enableHeadlamp() {}
	void __attribute__((weak)) disableMask() {}
	void __attribute__((weak)) disableCleats() {}
	void __attribute__((weak)) disableNightVision() {}

	void __attribute__((weak)) mission_render(s32 rendererIndex, bool forceTextureUpdate)
	{
		static JBool s_loggedMissionRenderStub = JFALSE;
		if (!s_loggedMissionRenderStub)
		{
			s_loggedMissionRenderStub = JTRUE;
			debugf("[df_runtime_stubs] mission_render stub active (no world rendering in this lane)\n");
		}
		(void)rendererIndex;
		(void)forceTextureUpdate;
	}

	void __attribute__((weak)) tickFrontendRuntimeFrame()
	{
		static Tick s_lastFrontendHeartbeat = 0;
		static JBool s_loggedTransitionalRenderNote = JFALSE;

		if (!s_runtimeFrontendActive)
		{
			if (TFE_Jedi::task_getCount() > 0)
			{
				s_runtimeFrontendActive = JTRUE;
				s_runtimeFrontendMissionLoop = JTRUE;
				setRuntimeFrontendStatus("Runtime takeover attached to mission loop");
			}
			else
			{
				return;
			}
		}

		joypad_buttons_t pressed = joypad_get_buttons_pressed(JOYPAD_PORT_1);

		if (s_runtimeFrontendMissionLoop)
		{
			if (!s_loggedTransitionalRenderNote)
			{
				s_loggedTransitionalRenderNote = JTRUE;
				debugf("[df_runtime_stubs] transitional mission loop active (task-driven, renderer still shimmed)\n");
			}

			if (pressed.b)
			{
				mission_setExitLevel(JTRUE);
				mission_pause(JTRUE);
				s_runtimeFrontendMissionLoop = JFALSE;
				setRuntimeFrontendStatus("Returned to mission selection");
			}

			if (TFE_Jedi::task_getCount() > 0)
			{
				TFE_Jedi::task_updateTime();
				s_curTick++;
				TFE_Jedi::task_run();

				if ((s_curTick - s_lastFrontendHeartbeat) >= 145)
				{
					s_lastFrontendHeartbeat = s_curTick;
					debugf("[df_runtime_stubs] frontend mission heartbeat tick=%ld tasks=%ld paused=%d exit=%d\n",
						(long)s_curTick,
						(long)TFE_Jedi::task_getCount(),
						(int)s_gamePaused,
						(int)s_runtimeExitLevel);
				}
			}
			else
			{
				s_runtimeFrontendMissionLoop = JFALSE;
				setRuntimeFrontendStatus("Mission loop completed");
			}

			console_clear();
			printf("Dark Forces N64\n");
			printf("Runtime Mission Loop Active\n\n");
			printf("Mission %02ld  Code %s\n",
				(long)s_runtimeFrontendMissionIndex,
				runtimeFrontendMissionCode(s_runtimeFrontendMissionIndex));
			printf("B: return to mission select\n");
			printf("Tick: %ld\n", (long)s_curTick);
			printf("Status: %s\n", s_runtimeFrontendStatus);
			console_render();
			return;
		}

		const s32 missionCount = N64_MissionCatalog::getMissionCount();
		if (pressed.d_right && missionCount > 0)
		{
			s_runtimeFrontendMissionIndex++;
			if (s_runtimeFrontendMissionIndex > missionCount)
			{
				s_runtimeFrontendMissionIndex = 1;
			}
			setRuntimeFrontendStatus("Mission advanced");
		}
		if (pressed.d_left && missionCount > 0)
		{
			s_runtimeFrontendMissionIndex--;
			if (s_runtimeFrontendMissionIndex < 1)
			{
				s_runtimeFrontendMissionIndex = missionCount;
			}
			setRuntimeFrontendStatus("Mission reversed");
		}

		if ((pressed.a || pressed.start || pressed.z) && missionCount > 0)
		{
			N64_Runtime::RuntimeHandoffRequest request = {};
			request.version = 1;
			request.agentIndex = 0;
			request.missionIndex = s_runtimeFrontendMissionIndex;
			strncpy(request.agentName, "N64_AGENT", N64_Runtime::MAX_AGENT_NAME_LEN - 1);
			strncpy(request.missionCode, runtimeFrontendMissionCode(s_runtimeFrontendMissionIndex), N64_Runtime::MAX_MISSION_CODE_LEN - 1);
			debugf("[df_runtime_stubs] runtime request missionIndex=%ld code=%s\n",
				(long)request.missionIndex,
				request.missionCode);

			const N64_Runtime::BeginRuntimeResult result = N64_Runtime::beginRuntimeFromHandoff(request);
			debugf("[df_runtime_stubs] runtime request result=%d\n", (int)result);
			if (result == N64_Runtime::BeginRuntimeResult::Started)
			{
				s_runtimeFrontendMissionLoop = JTRUE;
				setRuntimeFrontendStatus("Mission runtime started");
			}
			else if (result == N64_Runtime::BeginRuntimeResult::SymbolMissing)
			{
				setRuntimeFrontendStatus("Runtime symbol missing");
			}
			else
			{
				setRuntimeFrontendStatus("Runtime request invalid");
			}
		}

		console_clear();
		printf("Dark Forces N64\n");
		printf("Runtime Frontend (Transitional)\n\n");
		printf("Mission %02ld  Code %s\n",
			(long)s_runtimeFrontendMissionIndex,
			runtimeFrontendMissionCode(s_runtimeFrontendMissionIndex));
		printf("D-Pad Left/Right: mission select\n");
		printf("A/Start/Z: start mission runtime\n");
		printf("Status: %s\n", s_runtimeFrontendStatus);
		console_render();
	}

	void __attribute__((weak)) cheat_revealMap() {}
	void __attribute__((weak)) cheat_supercharge() {}
	void __attribute__((weak)) cheat_toggleData() {}
	void __attribute__((weak)) cheat_toggleFullBright() {}
	void __attribute__((weak)) cheat_levelSkip() {}

	void __attribute__((weak)) pda_cleanup()
	{
		debugf("[df_runtime_stubs] pda_cleanup()\n");
	}

	void __attribute__((weak)) lmusic_reset()
	{
		debugf("[df_runtime_stubs] lmusic_reset()\n");
	}

	void __attribute__((weak)) gameMusic_stop()
	{
		debugf("[df_runtime_stubs] gameMusic_stop()\n");
	}

	void __attribute__((weak)) gameMusic_start(s32 level)
	{
		debugf("[df_runtime_stubs] gameMusic_start(%ld)\n", (long)level);
	}

	void __attribute__((weak)) sound_levelStop()
	{
		debugf("[df_runtime_stubs] sound_levelStop()\n");
	}

	void __attribute__((weak)) actor_clearState()
	{
		debugf("[df_runtime_stubs] actor_clearState()\n");
	}

	void __attribute__((weak)) sound_levelStart()
	{
		debugf("[df_runtime_stubs] sound_levelStart()\n");
	}

	void __attribute__((weak)) agent_levelEndTask()
	{
		debugf("[df_runtime_stubs] agent_levelEndTask()\n");
	}
}

namespace TFE_A11Y
{
	void clearActiveCaptions()
	{
	}

	Vec2f drawCaptions()
	{
		return { 0.0f, 0.0f };
	}

	bool cutsceneCaptionsEnabled()
	{
		return false;
	}

	bool gameplayCaptionsEnabled()
	{
		return false;
	}
}

namespace TFE_ScriptInterface
{
	void reset()
	{
	}
}

namespace TFE_Jedi
{
	void __attribute__((weak)) renderer_setType(RendererType type)
	{
		(void)type;
	}

	JBool __attribute__((weak)) render_setResolution(bool forceUpdate)
	{
		(void)forceUpdate;
		return JTRUE;
	}

	void __attribute__((weak)) renderer_setLimits()
	{
	}

	void __attribute__((weak)) renderer_reset()
	{
		debugf("[df_runtime_stubs] renderer_reset()\n");
	}

	void __attribute__((weak)) bitmap_setAllocator(MemoryRegion* allocator)
	{
		(void)allocator;
		debugf("[df_runtime_stubs] bitmap_setAllocator(level)\n");
	}

	void __attribute__((weak)) level_freeAllAssets()
	{
		debugf("[df_runtime_stubs] level_freeAllAssets()\n");
	}

	void __attribute__((weak)) bitmap_clearLevelData()
	{
		debugf("[df_runtime_stubs] bitmap_clearLevelData()\n");
	}
}
