#pragma once

#include <TFE_System/types.h>

namespace N64_MissionCatalog
{
	enum
	{
		MAX_MISSION_CATALOG_ENTRIES = 64,
		MAX_MISSION_CODE_LEN = 32,
	};

	bool loadFromDarkGob(const char* gobPath = "rom:/DARK.GOB");
	void unload();

	bool isLoaded();
	s32 getMissionCount();
	const char* getMissionCodeByIndex(s32 missionIndex); // 1-based
	s32 getMissionIndexByCode(const char* missionCode);  // 1-based, 0 if not found

	const char* getLastError();
}
