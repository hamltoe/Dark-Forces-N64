#include "mission_catalog.h"

#include <cctype>
#include <cstdio>
#include <cstring>

namespace N64_MissionCatalog
{
	namespace
	{
		struct GobHeaderRaw
		{
			char magic[4];
			unsigned char masterOffset[4];
		};

		struct GobIndexHeaderRaw
		{
			unsigned char fileCount[4];
		};

		struct GobEntryRaw
		{
			unsigned char offset[4];
			unsigned char length[4];
			char name[13];
		};

		bool s_loaded = false;
		s32 s_missionCount = 0;
		char s_missionCodes[MAX_MISSION_CATALOG_ENTRIES][MAX_MISSION_CODE_LEN] = { 0 };
		char s_lastError[96] = "not loaded";

		u32 decodeLeU32(const unsigned char bytes[4])
		{
			return (u32)bytes[0] | ((u32)bytes[1] << 8u) | ((u32)bytes[2] << 16u) | ((u32)bytes[3] << 24u);
		}

		void copyBounded(char* dst, size_t dstLen, const char* src)
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

		char toUpperAscii(char c)
		{
			if (c >= 'a' && c <= 'z')
			{
				return (char)(c - ('a' - 'A'));
			}
			return c;
		}

		bool equalsIgnoreCaseAscii(const char* a, const char* b)
		{
			if (!a || !b)
			{
				return false;
			}

			size_t i = 0;
			while (a[i] && b[i])
			{
				if (toUpperAscii(a[i]) != toUpperAscii(b[i]))
				{
					return false;
				}
				i++;
			}
			return a[i] == b[i];
		}

		void setLastError(const char* message)
		{
			if (!message)
			{
				s_lastError[0] = 0;
				return;
			}

			strncpy(s_lastError, message, sizeof(s_lastError) - 1);
			s_lastError[sizeof(s_lastError) - 1] = 0;
		}

		void trimAsciiInPlace(char* text)
		{
			if (!text)
			{
				return;
			}

			char* start = text;
			while (*start && isspace((unsigned char)*start))
			{
				start++;
			}

			char* end = start + strlen(start);
			while (end > start && isspace((unsigned char)*(end - 1)))
			{
				end--;
			}
			*end = 0;

			if (start != text)
			{
				memmove(text, start, (size_t)(end - start) + 1);
			}
		}

		bool startsWithIgnoreCaseAscii(const char* text, const char* prefix)
		{
			if (!text || !prefix)
			{
				return false;
			}

			size_t i = 0;
			while (prefix[i])
			{
				if (!text[i])
				{
					return false;
				}
				if (toUpperAscii(text[i]) != toUpperAscii(prefix[i]))
				{
					return false;
				}
				i++;
			}
			return true;
		}

		void sanitizeMissionCode(char* code)
		{
			if (!code)
			{
				return;
			}

			trimAsciiInPlace(code);
			size_t len = strlen(code);
			if (len >= 2 && ((code[0] == '"' && code[len - 1] == '"') || (code[0] == '\'' && code[len - 1] == '\'')))
			{
				memmove(code, code + 1, len - 2);
				code[len - 2] = 0;
				trimAsciiInPlace(code);
			}

			if ((code[0] == 'L' || code[0] == 'l') && code[1] == ':')
			{
				memmove(code, code + 2, strlen(code + 2) + 1);
			}
		}

		bool extractMissionCodeFromLine(const char* inputLine, char* outCode, size_t outCodeLen)
		{
			if (!inputLine || !outCode || outCodeLen == 0)
			{
				return false;
			}

			char line[256];
			copyBounded(line, sizeof(line), inputLine);

			char* cppComment = strstr(line, "//");
			if (cppComment)
			{
				*cppComment = 0;
			}
			char* hashComment = strchr(line, '#');
			if (hashComment)
			{
				*hashComment = 0;
			}

			trimAsciiInPlace(line);
			if (!line[0])
			{
				return false;
			}

			if (startsWithIgnoreCaseAscii(line, "LEVELS"))
			{
				return false;
			}

			char* comma1 = strchr(line, ',');
			if (comma1)
			{
				char* codeStart = comma1 + 1;
				while (*codeStart && isspace((unsigned char)*codeStart))
				{
					codeStart++;
				}

				char* comma2 = strchr(codeStart, ',');
				char saved = 0;
				if (comma2)
				{
					saved = *comma2;
					*comma2 = 0;
				}

				char code[MAX_MISSION_CODE_LEN];
				copyBounded(code, sizeof(code), codeStart);
				sanitizeMissionCode(code);

				if (comma2)
				{
					*comma2 = saved;
				}

				if (code[0])
				{
					copyBounded(outCode, outCodeLen, code);
					return true;
				}
			}

			char* tokens[32] = { 0 };
			s32 tokenCount = 0;
			char* cursor = line;
			while (*cursor && tokenCount < 32)
			{
				while (*cursor && isspace((unsigned char)*cursor))
				{
					cursor++;
				}
				if (!*cursor)
				{
					break;
				}

				tokens[tokenCount++] = cursor;
				while (*cursor && !isspace((unsigned char)*cursor))
				{
					cursor++;
				}
				if (*cursor)
				{
					*cursor = 0;
					cursor++;
				}
			}

			if (tokenCount >= 2)
			{
				s32 tokenOffset = 1;
				const char* lastToken = tokens[tokenCount - 1];
				if (lastToken && (lastToken[0] == 'L' || lastToken[0] == 'l') && lastToken[1] == ':')
				{
					tokenOffset = 2;
				}

				s32 nameCount = tokenCount - tokenOffset;
				if (nameCount >= 1 && nameCount < tokenCount)
				{
					char code[MAX_MISSION_CODE_LEN];
					copyBounded(code, sizeof(code), tokens[nameCount]);
					sanitizeMissionCode(code);
					if (code[0])
					{
						copyBounded(outCode, outCodeLen, code);
						return true;
					}
				}
			}

			return false;
		}

		void addMissionCode(const char* missionCode)
		{
			if (!missionCode || !missionCode[0] || s_missionCount >= MAX_MISSION_CATALOG_ENTRIES)
			{
				return;
			}

			for (s32 i = 0; i < s_missionCount; i++)
			{
				if (equalsIgnoreCaseAscii(s_missionCodes[i], missionCode))
				{
					return;
				}
			}

			copyBounded(s_missionCodes[s_missionCount], MAX_MISSION_CODE_LEN, missionCode);
			s_missionCount++;
		}
	}

	void unload()
	{
		s_loaded = false;
		s_missionCount = 0;
		memset(s_missionCodes, 0, sizeof(s_missionCodes));
		setLastError("not loaded");
	}

	bool loadFromDarkGob(const char* gobPath)
	{
		unload();

		if (!gobPath || !gobPath[0])
		{
			setLastError("invalid gob path");
			return false;
		}

		FILE* file = fopen(gobPath, "rb");
		if (!file)
		{
			setLastError("cannot open DARK.GOB");
			return false;
		}

		GobHeaderRaw header = {};
		if (fread(&header, 1, sizeof(header), file) != sizeof(header))
		{
			fclose(file);
			setLastError("cannot read GOB header");
			return false;
		}

		if (header.magic[0] != 'G' || header.magic[1] != 'O' || header.magic[2] != 'B' || header.magic[3] != '\n')
		{
			fclose(file);
			setLastError("invalid GOB signature");
			return false;
		}

		const u32 indexOffset = decodeLeU32(header.masterOffset);
		if (fseek(file, (long)indexOffset, SEEK_SET) != 0)
		{
			fclose(file);
			setLastError("invalid GOB index offset");
			return false;
		}

		GobIndexHeaderRaw indexHeader = {};
		if (fread(&indexHeader, 1, sizeof(indexHeader), file) != sizeof(indexHeader))
		{
			fclose(file);
			setLastError("cannot read GOB file count");
			return false;
		}

		const u32 fileCount = decodeLeU32(indexHeader.fileCount);
		if (fileCount == 0 || fileCount > 4096)
		{
			fclose(file);
			setLastError("invalid GOB file count");
			return false;
		}

		u32 jediOffset = 0;
		u32 jediLength = 0;
		for (u32 i = 0; i < fileCount; i++)
		{
			GobEntryRaw entry = {};
			if (fread(&entry, 1, sizeof(entry), file) != sizeof(entry))
			{
				fclose(file);
				setLastError("cannot read GOB directory entry");
				return false;
			}

			char entryName[14] = { 0 };
			memcpy(entryName, entry.name, sizeof(entry.name));
			entryName[13] = 0;

			if (equalsIgnoreCaseAscii(entryName, "JEDI.LVL"))
			{
				jediOffset = decodeLeU32(entry.offset);
				jediLength = decodeLeU32(entry.length);
				break;
			}
		}

		if (!jediOffset || !jediLength)
		{
			fclose(file);
			setLastError("JEDI.LVL missing in DARK.GOB");
			return false;
		}

		if (fseek(file, (long)jediOffset, SEEK_SET) != 0)
		{
			fclose(file);
			setLastError("invalid JEDI.LVL offset");
			return false;
		}

		char* text = new char[(size_t)jediLength + 1];
		if (fread(text, 1, jediLength, file) != jediLength)
		{
			delete[] text;
			fclose(file);
			setLastError("cannot read JEDI.LVL data");
			return false;
		}
		text[jediLength] = 0;
		fclose(file);

		char code[MAX_MISSION_CODE_LEN];
		const char* cursor = text;
		while (*cursor)
		{
			const char* lineStart = cursor;
			while (*cursor && *cursor != '\n' && *cursor != '\r')
			{
				cursor++;
			}

			char line[256];
			size_t lineLen = (size_t)(cursor - lineStart);
			if (lineLen >= sizeof(line))
			{
				lineLen = sizeof(line) - 1;
			}
			memcpy(line, lineStart, lineLen);
			line[lineLen] = 0;

			if (extractMissionCodeFromLine(line, code, sizeof(code)))
			{
				addMissionCode(code);
			}

			if (*cursor == '\r')
			{
				cursor++;
			}
			if (*cursor == '\n')
			{
				cursor++;
			}
		}

		delete[] text;

		if (s_missionCount == 0)
		{
			setLastError("no mission codes parsed from JEDI.LVL");
			return false;
		}

		s_loaded = true;
		setLastError("ok");
		return true;
	}

	bool isLoaded()
	{
		return s_loaded;
	}

	s32 getMissionCount()
	{
		return s_missionCount;
	}

	const char* getMissionCodeByIndex(s32 missionIndex)
	{
		if (missionIndex < 1 || missionIndex > s_missionCount)
		{
			return nullptr;
		}
		return s_missionCodes[missionIndex - 1];
	}

	s32 getMissionIndexByCode(const char* missionCode)
	{
		if (!missionCode || !missionCode[0])
		{
			return 0;
		}

		for (s32 i = 0; i < s_missionCount; i++)
		{
			if (equalsIgnoreCaseAscii(s_missionCodes[i], missionCode))
			{
				return i + 1;
			}
		}

		return 0;
	}

	const char* getLastError()
	{
		return s_lastError;
	}
}
