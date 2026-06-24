#include "runtime_handoff.h"

#include <debug.h>
#include <cstdlib>
#include <cstdio>
#include <cstring>

namespace N64_Runtime
{
	namespace
	{
		bool s_sdFsInitTried = false;
		bool s_sdFsReady = false;

		bool startsWith(const char* text, const char* prefix)
		{
			if (!text || !prefix)
			{
				return false;
			}

			size_t i = 0;
			while (prefix[i])
			{
				if (!text[i] || text[i] != prefix[i])
				{
					return false;
				}
				i++;
			}
			return true;
		}

		bool isSdPath(const char* path)
		{
			return startsWith(path, "sd:/");
		}

		void ensureSdFilesystemReady(const char* path)
		{
			if (!isSdPath(path) || s_sdFsInitTried)
			{
				return;
			}

			s_sdFsInitTried = true;
			s_sdFsReady = debug_init_sdfs("sd:/", -1);
			debugf("[handoff_io] sd filesystem init: %s\n", s_sdFsReady ? "ok" : "failed");
		}

		void copyString(char* dst, const char* src, size_t dstLen)
		{
			if (!dst || !dstLen)
			{
				return;
			}

			dst[0] = 0;
			if (!src)
			{
				return;
			}

			size_t i = 0;
			for (; i + 1 < dstLen && src[i]; i++)
			{
				dst[i] = src[i];
			}
			dst[i] = 0;
		}

		void trimLineEnd(char* line)
		{
			if (!line)
			{
				return;
			}

			size_t len = strlen(line);
			while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
			{
				line[len - 1] = 0;
				len--;
			}
		}
	}

	bool writeRuntimeHandoff(const char* path, const RuntimeHandoffRequest& request)
	{
		if (!path || !path[0])
		{
			debugf("[handoff_io] write failed: invalid path\n");
			return false;
		}

		ensureSdFilesystemReady(path);
		if (isSdPath(path) && s_sdFsInitTried && !s_sdFsReady)
		{
			debugf("[handoff_io] write blocked: sd filesystem unavailable\n");
			return false;
		}

		FILE* file = fopen(path, "w");
		if (!file)
		{
			debugf("[handoff_io] write failed: fopen(%s)\n", path);
			return false;
		}

		fprintf(file, "VERSION=%ld\n", (long)request.version);
		fprintf(file, "AGENT_INDEX=%ld\n", (long)request.agentIndex);
		fprintf(file, "MISSION_INDEX=%ld\n", (long)request.missionIndex);
		fprintf(file, "AGENT_NAME=%s\n", request.agentName);
		fprintf(file, "MISSION_CODE=%s\n", request.missionCode);

		fclose(file);
		debugf("[handoff_io] write ok: path=%s idx=%ld code=%s agentIndex=%ld\n",
			path,
			(long)request.missionIndex,
			request.missionCode[0] ? request.missionCode : "<empty>",
			(long)request.agentIndex);
		return true;
	}

	bool readRuntimeHandoff(const char* path, RuntimeHandoffRequest* outRequest)
	{
		if (!path || !path[0] || !outRequest)
		{
			debugf("[handoff_io] read failed: invalid args\n");
			return false;
		}

		ensureSdFilesystemReady(path);
		if (isSdPath(path) && s_sdFsInitTried && !s_sdFsReady)
		{
			debugf("[handoff_io] read blocked: sd filesystem unavailable\n");
			return false;
		}

		FILE* file = fopen(path, "r");
		if (!file)
		{
			debugf("[handoff_io] read failed: fopen(%s)\n", path);
			return false;
		}

		RuntimeHandoffRequest request = {};
		request.version = 1;
		request.agentIndex = 0;
		request.missionIndex = 1;

		char line[256];
		while (fgets(line, sizeof(line), file))
		{
			trimLineEnd(line);

			if (strncmp(line, "VERSION=", 8) == 0)
			{
				request.version = (s32)strtol(line + 8, nullptr, 10);
			}
			else if (strncmp(line, "AGENT_INDEX=", 12) == 0)
			{
				request.agentIndex = (s32)strtol(line + 12, nullptr, 10);
			}
			else if (strncmp(line, "MISSION_INDEX=", 14) == 0)
			{
				request.missionIndex = (s32)strtol(line + 14, nullptr, 10);
			}
			else if (strncmp(line, "AGENT_NAME=", 11) == 0)
			{
				copyString(request.agentName, line + 11, MAX_AGENT_NAME_LEN);
			}
			else if (strncmp(line, "MISSION_CODE=", 13) == 0)
			{
				copyString(request.missionCode, line + 13, MAX_MISSION_CODE_LEN);
			}
		}

		fclose(file);

		if (request.missionIndex < 1)
		{
			debugf("[handoff_io] read failed: invalid mission index=%ld path=%s\n", (long)request.missionIndex, path);
			return false;
		}

		*outRequest = request;
		debugf("[handoff_io] read ok: path=%s idx=%ld code=%s agentIndex=%ld\n",
			path,
			(long)request.missionIndex,
			request.missionCode[0] ? request.missionCode : "<empty>",
			(long)request.agentIndex);
		return true;
	}
}
