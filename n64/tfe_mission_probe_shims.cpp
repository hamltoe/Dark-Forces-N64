#include <TFE_Input/input.h>
#include <TFE_Input/inputMapping.h>
#include <TFE_Input/replay.h>

#include <TFE_FrontEndUI/console.h>
#include <TFE_FrontEndUI/frontEndUi.h>

#include <TFE_Settings/settings.h>

#include <TFE_Audio/audioSystem.h>
#include <TFE_Audio/midiPlayer.h>

#include <TFE_System/system.h>
#include <TFE_System/tfeMessage.h>

#include <TFE_ForceScript/forceScript.h>

#include <TFE_RenderBackend/renderBackend.h>

#include <TFE_Jedi/Renderer/jediRenderer.h>
#include <TFE_Jedi/Renderer/rcommon.h>
#include <TFE_Jedi/Renderer/screenDraw.h>
#include <TFE_Jedi/Renderer/virtualFramebuffer.h>
#include <TFE_Jedi/Renderer/RClassic_Fixed/rclassicFixed.h>
#include <TFE_Jedi/Renderer/RClassic_Fixed/rclassicFixedSharedState.h>
#include <TFE_Jedi/Renderer/RClassic_Fixed/rsectorFixed.h>
#include <TFE_Jedi/Level/level.h>
#include <TFE_Jedi/Level/levelData.h>
#include <TFE_Jedi/Level/rsector.h>
#include <TFE_Jedi/Level/robject.h>
#include <TFE_Jedi/InfSystem/infSystem.h>
#include <TFE_Jedi/InfSystem/message.h>
#include <TFE_Jedi/Level/rtexture.h>
#include <TFE_Jedi/Math/core_math.h>
#include <TFE_Jedi/Math/fixedPoint.h>

#include <TFE_System/parser.h>
#include <TFE_Asset/dfKeywords.h>

#include <TFE_RenderShared/texturePacker.h>

#include <TFE_FileSystem/filestream.h>
#include <TFE_FileSystem/paths.h>
#include <TFE_Archive/archive.h>

#include <TFE_Memory/memoryRegion.h>

#include <TFE_DarkForces/agent.h>
#include <TFE_DarkForces/animLogic.h>
#include <TFE_DarkForces/automap.h>
#include <TFE_DarkForces/cheats.h>
#include <TFE_DarkForces/config.h>
#include <TFE_DarkForces/hitEffect.h>
#include <TFE_DarkForces/hud.h>
#include <TFE_DarkForces/logic.h>
#include <TFE_DarkForces/pickup.h>
#include <TFE_DarkForces/player.h>
#include <TFE_DarkForces/projectile.h>
#include <TFE_DarkForces/time.h>
#include <TFE_DarkForces/updateLogic.h>
#include <TFE_DarkForces/weapon.h>
#include <TFE_DarkForces/GameUI/escapeMenu.h>
#include <TFE_DarkForces/GameUI/pda.h>

#include <libdragon.h>

#include <cstdarg>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
	TFE_Settings_Graphics s_probeGraphics = {};
	TFE_Settings_Temp s_probeTemp = {};
	TFE_Settings_System s_probeSystem = {};
	TFE_Settings_A11y s_probeA11y = {};
	u8 s_probeFramebuffer[320 * 200] = { 0 };
	ScreenRect s_probeScreenRect = { 0, 0, 319, 199 };
	u32 s_probePalette[256] = { 0 };
	u16 s_probePalette16[256] = { 0 };
	u32 s_probeWidth = 320;
	u32 s_probeHeight = 200;
	u32 s_probePresentCount = 0;
	u32 s_probePaletteUpdateCount = 0;
	JBool s_probePaletteFlat = JTRUE;
	u32 s_probeWorkBufferU32[1024] = { 0 };
	char s_probeWorkBufferChar[32768] = { 0 };
	TFE_Jedi::TFE_Sectors_Fixed s_probeFixedSectorRenderer = {};
	JBool s_probeFixedRendererInit = JFALSE;
	u32 s_probeFixedRendererWidth = 0;
	u32 s_probeFixedRendererHeight = 0;

	#pragma pack(push, 1)
	struct ProbeGobHeader
	{
		char magic[4];
		u32 masterOffset;
	};

	struct ProbeGobEntry
	{
		u32 offset;
		u32 length;
		char name[13];
	};
	#pragma pack(pop)

	inline u32 probe_readU32LE(const void* src)
	{
		const u8* b = (const u8*)src;
		return (u32)b[0] | ((u32)b[1] << 8u) | ((u32)b[2] << 16u) | ((u32)b[3] << 24u);
	}

	inline bool probe_isGobTableRangeValid(u32 masterOffset, u32 fileCount, u32 fileSize)
	{
		const u64 tableStart = (u64)masterOffset + 4ull;
		const u64 tableBytes = (u64)fileCount * (u64)sizeof(ProbeGobEntry);
		return masterOffset <= fileSize && tableStart <= fileSize && (tableStart + tableBytes) <= fileSize;
	}

	s32 probe_stricmp(const char* lhs, const char* rhs)
	{
		if (!lhs && !rhs) { return 0; }
		if (!lhs) { return -1; }
		if (!rhs) { return 1; }

		while (*lhs && *rhs)
		{
			const s32 cl = tolower((unsigned char)*lhs);
			const s32 cr = tolower((unsigned char)*rhs);
			if (cl != cr)
			{
				return cl - cr;
			}
			++lhs;
			++rhs;
		}
		return (s32)(unsigned char)*lhs - (s32)(unsigned char)*rhs;
	}

	bool probe_tryResolveReadablePath(const char* requested, std::string* resolvedPath)
	{
		if (!requested || !requested[0])
		{
			return false;
		}

		static const char* c_prefixes[] =
		{
			"",
			"rom:/",
			"sd:/",
			"filesystem/",
			"rom:/filesystem/",
			"sd:/filesystem/",
		};

		for (u32 i = 0; i < (u32)(sizeof(c_prefixes) / sizeof(c_prefixes[0])); ++i)
		{
			const char* prefix = c_prefixes[i];
			std::string candidate = prefix[0] ? (std::string(prefix) + requested) : std::string(requested);
			FILE* fp = fopen(candidate.c_str(), "rb");
			if (fp)
			{
				fclose(fp);
				if (resolvedPath)
				{
					*resolvedPath = candidate;
				}
				return true;
			}
		}
		return false;
	}

	void probe_ensureFixedRendererReady()
	{
		if (!s_probeFixedRendererInit)
		{
			TFE_Jedi::RClassic_Fixed::resetState();
			// Real jediRenderer is linked: renderer_init() creates the global
			// s_sectorRenderer and seeds the wall/depth/segment limits so beginRender()
			// never has to lazy-init (which would reset the per-frame camera).
			TFE_Jedi::renderer_init();
			// Safety net in case renderer_init() limits ever change.
			s_maxWallCount = 0xffff;
			s_maxDepthCount = 0xffff;
			s_maxSegCount = MAX_SEG;
			s_maxAdjoinSegCount = MAX_ADJOIN_SEG;
			s_maxAdjoinDepthRecursion = MAX_ADJOIN_DEPTH;
			TFE_Jedi::RClassic_Fixed::setupInitCameraAndLights();
			s_probeFixedRendererInit = JTRUE;
		}

		if (s_probeFixedRendererWidth != s_probeWidth || s_probeFixedRendererHeight != s_probeHeight)
		{
			TFE_Jedi::RClassic_Fixed::changeResolution((s32)s_probeWidth, (s32)s_probeHeight);
			s_probeFixedRendererWidth = s_probeWidth;
			s_probeFixedRendererHeight = s_probeHeight;
		}
	}

	void probe_drawCheckerFallback(u8* output)
	{
		u8* const out = output ? output : s_probeFramebuffer;
		const u32 width = s_probeWidth;
		const u32 height = s_probeHeight;
		const u8 base0 = 24;
		const u8 base1 = 48;
		const u8 pulse = (s_probePresentCount & 16u) ? 10 : 0;

		for (u32 y = 0; y < height; ++y)
		{
			u8* const row = out + y * width;
			const u32 yTile = (y >> 4u);
			for (u32 x = 0; x < width; ++x)
			{
				const u32 xTile = (x >> 4u);
				const bool checker = ((xTile ^ yTile) & 1u) != 0u;
				row[x] = (u8)((checker ? base1 : base0) + pulse);
			}
		}
	}

	class ProbeGobArchive final : public Archive
	{
	public:
		explicit ProbeGobArchive(const char* archiveName)
			: Archive(ARCHIVE_GOB)
			, m_stream(nullptr)
			, m_curFileIndex(-1)
			, m_isOpen(false)
		{
			m_name[0] = 0;
			m_archivePath[0] = 0;
			m_fileOffset = 0;
			if (archiveName)
			{
				setName(archiveName);
			}
		}

		~ProbeGobArchive() override
		{
			close();
		}

		bool create(const char* archivePath) override
		{
			(void)archivePath;
			return false;
		}

		bool open(const char* archivePath) override
		{
			close();

			std::string resolvedPath;
			if (!probe_tryResolveReadablePath(archivePath, &resolvedPath))
			{
				return false;
			}

			FILE* fp = fopen(resolvedPath.c_str(), "rb");
			if (!fp)
			{
				return false;
			}

			if (fseek(fp, 0, SEEK_END) != 0)
			{
				fclose(fp);
				return false;
			}

			const long endPos = ftell(fp);
			if (endPos < 0)
			{
				fclose(fp);
				return false;
			}

			const u32 fileSize = (u32)endPos;
			if (fseek(fp, 0, SEEK_SET) != 0)
			{
				fclose(fp);
				return false;
			}

			ProbeGobHeader header = {};
			if (fread(&header, sizeof(header), 1, fp) != 1 || memcmp(header.magic, "GOB\n", 4) != 0)
			{
				fclose(fp);
				return false;
			}

			const u32 masterOffset = probe_readU32LE(&header.masterOffset);

			if (masterOffset > fileSize || fseek(fp, (long)masterOffset, SEEK_SET) != 0)
			{
				fclose(fp);
				return false;
			}

			u32 fileCountLE = 0;
			if (fread(&fileCountLE, sizeof(fileCountLE), 1, fp) != 1)
			{
				fclose(fp);
				return false;
			}

			const u32 fileCount = probe_readU32LE(&fileCountLE);
			if (fileCount > 65536u || !probe_isGobTableRangeValid(masterOffset, fileCount, fileSize))
			{
				fclose(fp);
				return false;
			}

			try
			{
				m_entries.resize(fileCount);
			}
			catch (...)
			{
				fclose(fp);
				m_entries.clear();
				return false;
			}

			for (u32 i = 0; i < fileCount; ++i)
			{
				ProbeGobEntry rawEntry = {};
				if (fread(&rawEntry, sizeof(rawEntry), 1, fp) != 1)
				{
					fclose(fp);
					m_entries.clear();
					return false;
				}

				ProbeGobEntry& outEntry = m_entries[i];
				outEntry.offset = probe_readU32LE(&rawEntry.offset);
				outEntry.length = probe_readU32LE(&rawEntry.length);
				memcpy(outEntry.name, rawEntry.name, sizeof(outEntry.name));
				outEntry.name[sizeof(outEntry.name) - 1] = 0;

				const u64 endOffset = (u64)outEntry.offset + (u64)outEntry.length;
				if (endOffset > fileSize)
				{
					fclose(fp);
					m_entries.clear();
					return false;
				}
			}

			fclose(fp);
			m_resolvedPath = resolvedPath;
			strncpy(m_archivePath, resolvedPath.c_str(), TFE_MAX_PATH - 1);
			m_archivePath[TFE_MAX_PATH - 1] = 0;
			m_isOpen = true;
			m_curFileIndex = -1;
			m_fileOffset = 0;
			return true;
		}

		void close() override
		{
			closeFile();
			m_entries.clear();
			m_resolvedPath.clear();
			m_isOpen = false;
		}

		bool openFile(const char* file) override
		{
			u32 index = getFileIndex(file);
			if (index == INVALID_FILE)
			{
				return false;
			}
			return openFile(index);
		}

		bool openFile(u32 index) override
		{
			if (!m_isOpen || index >= m_entries.size())
			{
				return false;
			}

			closeFile();
			m_stream = fopen(m_resolvedPath.c_str(), "rb");
			if (!m_stream)
			{
				return false;
			}

			m_curFileIndex = (s32)index;
			m_fileOffset = 0;
			if (fseek(m_stream, (long)m_entries[index].offset, SEEK_SET) != 0)
			{
				closeFile();
				return false;
			}
			return true;
		}

		void closeFile() override
		{
			if (m_stream)
			{
				fclose(m_stream);
				m_stream = nullptr;
			}
			m_curFileIndex = -1;
			m_fileOffset = 0;
		}

		bool fileExists(const char* file) override
		{
			return getFileIndex(file) != INVALID_FILE;
		}

		bool fileExists(u32 index) override
		{
			return m_isOpen && index < m_entries.size();
		}

		u32 getFileIndex(const char* file) override
		{
			if (!m_isOpen || !file)
			{
				return INVALID_FILE;
			}

			for (u32 i = 0; i < (u32)m_entries.size(); ++i)
			{
				if (probe_stricmp(m_entries[i].name, file) == 0)
				{
					return i;
				}
			}
			return INVALID_FILE;
		}

		size_t getFileLength() override
		{
			if (m_curFileIndex < 0)
			{
				return 0;
			}
			return m_entries[(u32)m_curFileIndex].length;
		}

		size_t readFile(void* data, size_t size) override
		{
			if (!m_stream || !data || m_curFileIndex < 0)
			{
				return 0;
			}

			const size_t fileLength = m_entries[(u32)m_curFileIndex].length;
			const size_t remaining = m_fileOffset >= (s32)fileLength ? 0 : fileLength - (size_t)m_fileOffset;
			const size_t readSize = size == 0 ? remaining : (size < remaining ? size : remaining);
			const size_t bytesRead = fread(data, 1, readSize, m_stream);
			m_fileOffset += (s32)bytesRead;
			return bytesRead;
		}

		bool seekFile(s32 offset, s32 origin = SEEK_SET) override
		{
			if (!m_stream || m_curFileIndex < 0)
			{
				return false;
			}

			const size_t fileLength = m_entries[(u32)m_curFileIndex].length;
			s32 target = 0;
			switch (origin)
			{
				case SEEK_SET: target = offset; break;
				case SEEK_CUR: target = m_fileOffset + offset; break;
				case SEEK_END: target = (s32)fileLength - offset; break;
				default: return false;
			}

			if (target < 0)
			{
				target = 0;
			}
			if ((size_t)target > fileLength)
			{
				target = (s32)fileLength;
			}

			if (fseek(m_stream, (long)(m_entries[(u32)m_curFileIndex].offset + (u32)target), SEEK_SET) != 0)
			{
				return false;
			}
			m_fileOffset = target;
			return true;
		}

		size_t getLocInFile() override
		{
			return m_curFileIndex >= 0 ? (size_t)m_fileOffset : 0;
		}

		u32 getFileCount() override
		{
			return m_isOpen ? (u32)m_entries.size() : 0;
		}

		const char* getFileName(u32 index) override
		{
			if (!m_isOpen || index >= m_entries.size())
			{
				return nullptr;
			}
			return m_entries[index].name;
		}

		size_t getFileLength(u32 index) override
		{
			if (!m_isOpen || index >= m_entries.size())
			{
				return 0;
			}
			return m_entries[index].length;
		}

		void addFile(const char* fileName, const char* filePath) override
		{
			(void)fileName;
			(void)filePath;
		}

	private:
		std::string m_resolvedPath;
		std::vector<ProbeGobEntry> m_entries;
		FILE* m_stream;
		s32 m_curFileIndex;
		bool m_isOpen;
	};

	ProbeGobArchive s_probeDarkGob("DARK.GOB");
	ProbeGobArchive s_probeSoundsGob("SOUNDS.GOB");
	ProbeGobArchive s_probeTexturesGob("TEXTURES.GOB");
	ProbeGobArchive s_probeSpritesGob("SPRITES.GOB");
	ProbeGobArchive s_probeEnhancedGob("enhanced.gob");
	Archive* s_probeArchives[] =
	{
		&s_probeDarkGob,
		&s_probeSoundsGob,
		&s_probeTexturesGob,
		&s_probeSpritesGob,
		&s_probeEnhancedGob,
	};
	JBool s_probeArchivesInit = JFALSE;

	void probe_initGobArchives()
	{
		if (s_probeArchivesInit)
		{
			return;
		}
		s_probeArchivesInit = JTRUE;

		for (u32 i = 0; i < (u32)(sizeof(s_probeArchives) / sizeof(s_probeArchives[0])); ++i)
		{
			Archive* archive = s_probeArchives[i];
			if (!archive)
			{
				continue;
			}

			if (archive->open(archive->getName()))
			{
				debugf("[mission_probe] archive mounted: %s\n", archive->getPath());
			}
		}
	}

	inline u32 clampToFramebuffer(u32 value, u32 maxValue)
	{
		return value > maxValue ? maxValue : value;
	}

	void probe_updatePalette(const u32* palette)
	{
		if (!palette)
		{
			memset(s_probePalette, 0, sizeof(s_probePalette));
			memset(s_probePalette16, 0, sizeof(s_probePalette16));
			s_probePaletteFlat = JTRUE;
			return;
		}

		memcpy(s_probePalette, palette, sizeof(s_probePalette));
		u16 firstColor = 0;
		for (u32 i = 0; i < 256; ++i)
		{
			const u32 color = s_probePalette[i];
			const color_t rgba = RGBA32(
				(u8)(color & 0xffu),
				(u8)((color >> 8u) & 0xffu),
				(u8)((color >> 16u) & 0xffu),
				(u8)((color >> 24u) & 0xffu));
			s_probePalette16[i] = color_to_packed16(rgba);
			if (i == 0)
			{
				firstColor = s_probePalette16[i];
				s_probePaletteFlat = JTRUE;
			}
			else if (s_probePalette16[i] != firstColor)
			{
				s_probePaletteFlat = JFALSE;
			}
		}

		if (s_probePaletteUpdateCount == 0)
		{
			debugf("[mission_probe] first palette update flat=%d c0=0x%04x c255=0x%04x\n",
				s_probePaletteFlat ? 1 : 0,
				(unsigned int)s_probePalette16[0],
				(unsigned int)s_probePalette16[255]);
		}
		else if ((s_probePaletteUpdateCount % 120u) == 0)
		{
			debugf("[mission_probe] palette heartbeat updates=%lu flat=%d c0=0x%04x c255=0x%04x\n",
				(unsigned long)s_probePaletteUpdateCount,
				s_probePaletteFlat ? 1 : 0,
				(unsigned int)s_probePalette16[0],
				(unsigned int)s_probePalette16[255]);
		}
		s_probePaletteUpdateCount++;
	}

	void probe_presentFramebuffer()
	{
		surface_t* display = display_get();
		if (!display || !display->buffer)
		{
			return;
		}

		const u32 displayBpp = display_get_bitdepth();
		if (displayBpp != 2)
		{
			display_show(display);
			return;
		}

		u16* const dstPixels = (u16*)display->buffer;
		const u32 dstWidth = display->width;
		const u32 dstHeight = display->height;
		const u32 dstStridePixels = display->stride / (u32)sizeof(u16);

		const u32 drawWidth = dstWidth >= 640 ? 640 : dstWidth;
		const u32 drawHeight = dstHeight >= 240 ? 240 : dstHeight;
		const u32 offsetX = (dstWidth - drawWidth) / 2;
		const u32 offsetY = (dstHeight - drawHeight) / 2;
		const u16 clearColor = color_to_packed16(RGBA32(0, 0, 0, 255));
		const bool paletteFallback = s_probePaletteFlat == JTRUE;

		if (paletteFallback && (s_probePresentCount % 120u) == 0)
		{
			debugf("[mission_probe] palette fallback active (grayscale) frame=%lu\n",
				(unsigned long)s_probePresentCount);
		}

		for (u32 y = 0; y < dstHeight; ++y)
		{
			u16* row = dstPixels + y * dstStridePixels;
			for (u32 x = 0; x < dstWidth; ++x)
			{
				row[x] = clearColor;
			}
		}

		for (u32 y = 0; y < drawHeight; ++y)
		{
			const u32 srcY = (y * s_probeHeight) / drawHeight;
			const u8* const srcRow = s_probeFramebuffer + srcY * s_probeWidth;
			u16* const dstRow = dstPixels + (offsetY + y) * dstStridePixels + offsetX;

			for (u32 x = 0; x < drawWidth; ++x)
			{
				const u32 srcX = (x * s_probeWidth) / drawWidth;
				const u8 index = srcRow[srcX];
				if (paletteFallback)
				{
					dstRow[x] = color_to_packed16(RGBA32(index, index, index, 255));
				}
				else
				{
					dstRow[x] = s_probePalette16[index];
				}
			}
		}

		display_show(display);
	}
}

namespace TFE_Audio
{
	void __attribute__((weak)) resume()
	{
	}
}

namespace TFE_MidiPlayer
{
	void __attribute__((weak)) resume()
	{
	}
}

namespace TFE_Input
{
	void __attribute__((weak)) enableRelativeMode(bool enable)
	{
		(void)enable;
	}

	const char* __attribute__((weak)) getBufferedText()
	{
		return "";
	}

	ActionState __attribute__((weak)) inputMapping_getActionState(InputAction action)
	{
		(void)action;
		return STATE_UP;
	}

	void __attribute__((weak)) inputMapping_removeState(InputAction action)
	{
		(void)action;
	}

	void __attribute__((weak)) endRecording()
	{
	}
}

namespace TFE_FrontEndUI
{
	bool __attribute__((weak)) toggleConsole()
	{
		return false;
	}

	bool __attribute__((weak)) isConsoleOpen()
	{
		return false;
	}

	void __attribute__((weak)) setMenuReturnState(AppState state)
	{
		(void)state;
	}

	bool __attribute__((weak)) toggleEnhancements()
	{
		return false;
	}

	void __attribute__((weak)) exitToMenu()
	{
	}
}

namespace TFE_Settings
{
	TFE_Settings_Graphics* __attribute__((weak)) getGraphicsSettings()
	{
		return &s_probeGraphics;
	}

	TFE_Settings_Temp* __attribute__((weak)) getTempSettings()
	{
		return &s_probeTemp;
	}

	TFE_Settings_System* __attribute__((weak)) getSystemSettings()
	{
		return &s_probeSystem;
	}

	TFE_Settings_A11y* __attribute__((weak)) getA11ySettings()
	{
		return &s_probeA11y;
	}

	ModSettingLevelOverride* __attribute__((weak)) getLevelOverrides(string levelName)
	{
		(void)levelName;
		return nullptr;
	}
}

namespace TFE_System
{
	const char* __attribute__((weak)) getMessage(TFE_Message msg)
	{
		(void)msg;
		return nullptr;
	}

	void __attribute__((weak)) postSystemUiRequest()
	{
	}

	void __attribute__((weak)) postQuitMessage()
	{
	}

	void __attribute__((weak)) logWrite(LogWriteType type, const char* tag, const char* str, ...)
	{
		(void)type;
		(void)tag;
		(void)str;
	}
}

namespace TFE_Console
{
	void __attribute__((weak)) registerCommand(const char* name, ConsoleFunc func, u32 argCount, const char* helpString, bool repeat)
	{
		(void)name;
		(void)func;
		(void)argCount;
		(void)helpString;
		(void)repeat;
	}
}

namespace TFE_RenderBackend
{
	void __attribute__((weak)) getDisplayInfo(DisplayInfo* displayInfo)
	{
		if (displayInfo)
		{
			displayInfo->width = 320;
			displayInfo->height = 240;
			displayInfo->refreshRate = 60.0f;
		}
	}

	void __attribute__((weak)) setPalette(const u32* palette)
	{
		probe_updatePalette(palette);
	}
}

namespace TFE_Jedi
{
	JBool __attribute__((weak)) s_fullBright = JFALSE;
	JBool __attribute__((weak)) s_flatLighting = JFALSE;
	s32 __attribute__((weak)) s_flatAmbient = 0;
	LevelState __attribute__((weak)) s_levelState = {};

	JBool __attribute__((weak)) vfb_setResolution(u32 width, u32 height)
	{
		const u32 clampedW = clampToFramebuffer(width ? width : 320, 320);
		const u32 clampedH = clampToFramebuffer(height ? height : 200, 200);
		s_probeWidth = clampedW;
		s_probeHeight = clampedH;
		s_probeScreenRect.left = 0;
		s_probeScreenRect.top = 0;
		s_probeScreenRect.right = (s32)s_probeWidth - 1;
		s_probeScreenRect.bot = (s32)s_probeHeight - 1;
		memset(s_probeFramebuffer, 0, sizeof(s_probeFramebuffer));

		if ((width > 320 || height > 200) && s_probePresentCount == 0)
		{
			debugf("[mission_probe] vfb_setResolution clamped %lux%lu -> %lux%lu\n",
				(unsigned long)width,
				(unsigned long)height,
				(unsigned long)s_probeWidth,
				(unsigned long)s_probeHeight);
		}

		return JTRUE;
	}

	void __attribute__((weak)) vfb_setPalette(const u32* palette)
	{
		probe_updatePalette(palette);
	}

	u8* __attribute__((weak)) vfb_getCpuBuffer()
	{
		return s_probeFramebuffer;
	}

	ScreenRect* __attribute__((weak)) vfb_getScreenRect(ScreenRectType type)
	{
		(void)type;
		return &s_probeScreenRect;
	}

	void __attribute__((weak)) vfb_swap()
	{
		if (s_probePresentCount == 0)
		{
			debugf("[mission_probe] first vfb_swap present src=%lux%lu\n",
				(unsigned long)s_probeWidth,
				(unsigned long)s_probeHeight);
		}
		else if ((s_probePresentCount % 120u) == 0)
		{
			debugf("[mission_probe] vfb_swap heartbeat frame=%lu\n",
				(unsigned long)s_probePresentCount);
		}

		probe_presentFramebuffer();
		s_probePresentCount++;
	}

	JBool __attribute__((weak)) setSubRenderer(TFE_SubRenderer subRenderer)
	{
		(void)subRenderer;
		return JTRUE;
	}

	void __attribute__((weak)) renderer_setSourcePalette(const u32* srcPalette)
	{
		(void)srcPalette;
	}

	void __attribute__((weak)) render_clearCachedTextures()
	{
	}

	void __attribute__((weak)) beginRender()
	{
	}

	void __attribute__((weak)) endRender()
	{
	}

	void __attribute__((weak)) renderer_setupCameraLight(JBool flatShading, JBool headlamp)
	{
		(void)flatShading;
		(void)headlamp;
	}

	void __attribute__((weak)) renderer_setPalFx(const Vec3f* lumMask, const Vec3f* palFx)
	{
		(void)lumMask;
		(void)palFx;
	}

	void __attribute__((weak)) renderer_setWorldAmbient(s32 value)
	{
		s_worldAmbient = MAX_LIGHT_LEVEL - value;
	}

	void __attribute__((weak)) renderer_computeCameraTransform(RSector* sector, angle14_32 pitch, angle14_32 yaw, fixed16_16 camX, fixed16_16 camY, fixed16_16 camZ)
	{
		probe_ensureFixedRendererReady();
		TFE_Jedi::RClassic_Fixed::computeCameraTransform(sector, pitch, yaw, camX, camY, camZ);
	}

	void __attribute__((weak)) drawWorld(u8* display, RSector* sector, const u8* colormap, const u8* lightSourceRamp)
	{
		u8* const out = display ? display : s_probeFramebuffer;

		if (!sector)
		{
			if (s_probePresentCount == 0)
			{
				debugf("[mission_probe] drawWorld no sector; checker fallback\n");
			}
			probe_drawCheckerFallback(out);
			return;
		}

		probe_ensureFixedRendererReady();

		if (s_width > 0 && s_height > 0)
		{
			memset(out, 0, (size_t)s_width);
			memset(out + ((size_t)(s_height - 1) * (size_t)s_width), 0, (size_t)s_width);
		}

		s_drawFrame++;
		TFE_Jedi::RClassic_Fixed::computeSkyOffsets();

		s_display = out;
		s_colorMap = colormap;
		s_lightSourceRamp = lightSourceRamp;

		if (s_rcfState.depth1d_all && s_width > 0)
		{
			memset(s_rcfState.depth1d_all, 0, (size_t)s_width * sizeof(s32));
			s_rcfState.windowMinZ = 0;
		}

		s_windowMinX_Pixels = s_minScreenX_Pixels;
		s_windowMaxX_Pixels = s_maxScreenX_Pixels;
		s_windowMinY_Pixels = 1;
		s_windowMaxY_Pixels = s_height - 1;
		s_windowMaxCeil = s_minScreenY;
		s_windowMinFloor = s_maxScreenY;
		s_flatCount = 0;
		s_nextWall = 0;
		s_curWallSeg = 0;
		s_drawnObjCount = 0;

		s_prevSector = nullptr;
		s_sectorIndex = 0;
		s_maxAdjoinIndex = 0;
		s_adjoinSegCount = 1;
		s_adjoinIndex = 0;
		s_adjoinDepth = 1;
		s_maxAdjoinDepth = 1;

		if (s_columnTop && s_columnBot && s_windowTop_all && s_windowBot_all && s_width > 0)
		{
			for (s32 i = 0; i < s_width; ++i)
			{
				s_columnTop[i] = s_minScreenY;
				s_columnBot[i] = s_maxScreenY;
				s_windowTop_all[i] = s_minScreenY;
				s_windowBot_all[i] = s_maxScreenY;
			}
		}

		s_probeFixedSectorRenderer.prepare();
		s_probeFixedSectorRenderer.draw(sector);

		// --- One-shot camera/projection state dump (correlate with mergeSort cull diag). ---
		static u32 s_probeCamDumpCount = 0;
		if (s_probeCamDumpCount < 2)
		{
			debugf("[mission_probe] rcfState focal=%ld halfW=%ld winMinZ=%ld depth1d=%s cosYaw=%ld sinYaw=%ld negSin=%ld camTrans=(%ld,%ld) wallCount=%ld vtxCount=%ld\n",
				(long)s_rcfState.focalLength, (long)s_rcfState.halfWidth, (long)s_rcfState.windowMinZ,
				s_rcfState.depth1d_all ? "ok" : "NULL",
				(long)s_rcfState.cosYaw, (long)s_rcfState.sinYaw, (long)s_rcfState.negSinYaw,
				(long)s_rcfState.cameraTrans.x, (long)s_rcfState.cameraTrans.z,
				(long)sector->wallCount, (long)sector->vertexCount);
			s_probeCamDumpCount++;
		}

		// One-shot per-wall visibility gate dump from wallSegListSrc.
		// Mirrors wall_mergeSort cull predicate so we can see why queued walls vanish.
		static u32 s_probeWallGateDumpCount = 0;
		if (s_probeWallGateDumpCount < 2)
		{
			const s32 wallCount = s_nextWall;
			for (s32 wi = 0; wi < wallCount && wi < 12; ++wi)
			{
				const RWallSegmentFixed* seg = &s_rcfState.wallSegListSrc[wi];
				const RWall* srcWall = seg->srcWall;
				const JBool processed = (srcWall && s_drawFrame == srcWall->drawFrame) ? JTRUE : JFALSE;
				const JBool insideWindow =
					((seg->z0 >= s_rcfState.windowMinZ || seg->z1 >= s_rcfState.windowMinZ) &&
					 seg->wallX0 <= s_windowMaxX_Pixels && seg->wallX1 >= s_windowMinX_Pixels) ? JTRUE : JFALSE;

				debugf("[mission_probe] wallGate i=%ld proc=%d inWin=%d z0=%ld z1=%ld winMinZ=%ld x0=%ld x1=%ld winX=[%ld..%ld] drawF=%ld wallDF=%ld raw=[%ld..%ld]\n",
					(long)wi,
					(int)processed,
					(int)insideWindow,
					(long)seg->z0,
					(long)seg->z1,
					(long)s_rcfState.windowMinZ,
					(long)seg->wallX0,
					(long)seg->wallX1,
					(long)s_windowMinX_Pixels,
					(long)s_windowMaxX_Pixels,
					(long)s_drawFrame,
					srcWall ? (long)srcWall->drawFrame : (long)-1,
					(long)seg->wallX0_raw,
					(long)seg->wallX1_raw);
			}
			s_probeWallGateDumpCount++;
		}

		// --- Diagnostic: did the real fixed renderer actually rasterize anything? ---
		// "Black screen + top-left marker only" means the framebuffer stays all-zero, so we
		// log the render-window bounds, sector-traversal counters, and a non-zero pixel count.
		static u32 s_probeDrawDiagCount = 0;
		if (s_probeDrawDiagCount == 0 || (s_probeDrawDiagCount % 120u) == 0)
		{
			u32 nonZeroPx = 0;
			const u32 totalPx = (s_width > 0 && s_height > 0) ? (u32)s_width * (u32)s_height : 0u;
			for (u32 i = 0; i < totalPx; ++i)
			{
				if (out[i] != 0) { nonZeroPx++; }
			}
			debugf("[mission_probe] drawWorld diag frame=%lu sectorId=%ld dims=%ldx%ld scrX[%ld..%ld] scrY[%ld..%ld] win[%ld..%ld] sectors=%ld flats=%ld wallSegs=%ld nextWall=%ld objs=%ld nonZeroPx=%lu/%lu\n",
				(unsigned long)s_probeDrawDiagCount,
				(long)sector->id,
				(long)s_width, (long)s_height,
				(long)s_minScreenX_Pixels, (long)s_maxScreenX_Pixels,
				(long)s_minScreenY, (long)s_maxScreenY,
				(long)s_windowMinY_Pixels, (long)s_windowMaxY_Pixels,
				(long)s_sectorIndex, (long)s_flatCount, (long)s_curWallSeg,
				(long)s_nextWall, (long)s_drawnObjCount,
				(unsigned long)nonZeroPx, (unsigned long)totalPx);
		}
		s_probeDrawDiagCount++;
	}

	void __attribute__((weak)) blitTextureToScreen(TextureData* texture, DrawRect* rect, s32 x0, s32 y0, u8* output, JBool forceTransparency, JBool forceOpaque)
	{
		if (!texture || !texture->image || !rect || !output)
		{
			return;
		}

		const s32 texW = (s32)texture->width;
		const s32 texH = (s32)texture->height;
		if (texW <= 0 || texH <= 0)
		{
			return;
		}

		const s32 clipLeft = rect->x0;
		const s32 clipTop = rect->y0;
		const s32 clipRight = rect->x1;
		const s32 clipBottom = rect->y1;
		const s32 outW = (s32)s_probeWidth;
		const s32 outH = (s32)s_probeHeight;

		for (s32 srcY = 0; srcY < texH; ++srcY)
		{
			const s32 dstY = y0 + srcY;
			if (dstY < clipTop || dstY > clipBottom || dstY < 0 || dstY >= outH)
			{
				continue;
			}

			const u8* const srcRow = texture->image + srcY * texW;
			u8* const dstRow = output + dstY * outW;

			for (s32 srcX = 0; srcX < texW; ++srcX)
			{
				const s32 dstX = x0 + srcX;
				if (dstX < clipLeft || dstX > clipRight || dstX < 0 || dstX >= outW)
				{
					continue;
				}

				const u8 texel = srcRow[srcX];
				if (!forceOpaque && texel == 0)
				{
					continue;
				}
				dstRow[dstX] = texel;
			}
		}

		(void)forceTransparency;
	}

	void __attribute__((weak)) texturepacker_setConversionPalette(s32 index, s32 bpp, const u8* input)
	{
		(void)index;
		(void)bpp;
		(void)input;
	}

	void __attribute__((weak)) bitmap_setupAnimationTask()
	{
	}

	void __attribute__((weak)) inf_createElevatorTask()
	{
	}

	void __attribute__((weak)) inf_createTeleportTask()
	{
	}

	void __attribute__((weak)) inf_createTriggerTask()
	{
	}

	JBool __attribute__((weak)) level_load(const char* levelName, u8 difficulty)
	{
		(void)levelName;
		(void)difficulty;
		return JTRUE;
	}

	void __attribute__((weak)) level_clearData()
	{
	}

	void __attribute__((weak)) setSkyParallax(fixed16_16 parallax0, fixed16_16 parallax1)
	{
		(void)parallax0;
		(void)parallax1;
	}

	void __attribute__((weak)) updateLevelScript(f32 dt)
	{
		(void)dt;
	}

	void __attribute__((weak)) sector_changeGlobalLightLevel()
	{
	}
}

namespace TFE_Paths
{
	bool __attribute__((weak)) getFilePath(const char* fileName, FilePath* path)
	{
		if (!path)
		{
			return false;
		}
		path->archive = nullptr;
		path->index = INVALID_FILE;
		path->path[0] = 0;

		std::string resolvedPath;
		if (probe_tryResolveReadablePath(fileName, &resolvedPath))
		{
			strncpy(path->path, resolvedPath.c_str(), TFE_MAX_PATH - 1);
			path->path[TFE_MAX_PATH - 1] = 0;
			return true;
		}

		probe_initGobArchives();
		for (u32 i = 0; i < (u32)(sizeof(s_probeArchives) / sizeof(s_probeArchives[0])); ++i)
		{
			Archive* archive = s_probeArchives[i];
			if (!archive)
			{
				continue;
			}

			u32 index = archive->getFileIndex(fileName);
			if (index != INVALID_FILE)
			{
				path->archive = archive;
				path->index = index;
				return true;
			}
		}

		if (fileName)
		{
			strncpy(path->path, fileName, TFE_MAX_PATH - 1);
			path->path[TFE_MAX_PATH - 1] = 0;
		}
		return false;
	}
}

namespace TFE_Memory
{
	void* __attribute__((weak)) region_alloc(MemoryRegion* region, u64 size)
	{
		(void)region;
		return size ? malloc((size_t)size) : nullptr;
	}

	void* __attribute__((weak)) region_realloc(MemoryRegion* region, void* ptr, u64 size)
	{
		(void)region;
		return size ? realloc(ptr, (size_t)size) : nullptr;
	}

	void __attribute__((weak)) region_free(MemoryRegion* region, void* ptr)
	{
		(void)region;
		free(ptr);
	}
}

FileStream::FileStream() : Stream()
{
	m_file = nullptr;
	m_archive = nullptr;
	m_mode = MODE_INVALID;
}

FileStream::~FileStream()
{
	close();
}

bool FileStream::exists(const char* filename)
{
	bool result = open(filename, MODE_READ);
	close();
	return result;
}

bool FileStream::open(const char* filename, AccessMode mode)
{
	if (!filename)
	{
		return false;
	}

	const char* modeStrings[] = { "rb", "wb", "rb+", "ab" };
	m_archive = nullptr;
	m_file = nullptr;

	if (mode == MODE_READ)
	{
		std::string resolvedPath;
		if (probe_tryResolveReadablePath(filename, &resolvedPath))
		{
			m_file = fopen(resolvedPath.c_str(), modeStrings[mode]);
		}
	}
	else
	{
		m_file = fopen(filename, modeStrings[mode]);
	}

	m_mode = mode;
	return m_file != nullptr;
}

bool FileStream::open(const FilePath* filePath, AccessMode mode)
{
	if (!filePath)
	{
		return false;
	}

	if (filePath->archive)
	{
		assert(mode == Stream::MODE_READ);
		if (filePath->index == INVALID_FILE)
		{
			return false;
		}
		m_mode = mode;
		m_file = nullptr;
		m_archive = filePath->archive;
		return m_archive->openFile(filePath->index);
	}

	return open(filePath->path, mode);
}

void FileStream::close()
{
	if (m_file)
	{
		if (m_mode == MODE_WRITE || m_mode == MODE_READWRITE || m_mode == MODE_APPEND)
		{
			fflush(m_file);
		}
		fclose(m_file);
		m_file = nullptr;
	}
	else if (m_archive)
	{
		m_archive->closeFile();
		m_archive = nullptr;
	}
	m_mode = MODE_INVALID;
}

u32 FileStream::readContents(const char* filePath, void** output)
{
	assert(output);
	u32 size = 0;
	FileStream file;
	if (file.open(filePath, MODE_READ))
	{
		size = (u32)file.getSize();
		*output = realloc(*output, (size_t)size + 1);
		if (*output)
		{
			file.readBuffer(*output, size);
		}
		file.close();
	}
	return size;
}

u32 FileStream::readContents(const char* filePath, void* output, size_t size)
{
	assert(output);
	FileStream file;
	if (file.open(filePath, MODE_READ))
	{
		size_t fileSize = file.getSize();
		size = size <= fileSize ? size : fileSize;
		file.readBuffer(output, (u32)size);
		file.close();
		return (u32)size;
	}
	return 0;
}

u32 FileStream::readContents(const FilePath* filePath, void** output)
{
	assert(output);
	u32 size = 0;
	FileStream file;
	if (file.open(filePath, MODE_READ))
	{
		size = (u32)file.getSize();
		*output = realloc(*output, (size_t)size + 1);
		if (*output)
		{
			file.readBuffer(*output, size);
		}
		file.close();
	}
	return size;
}

u32 FileStream::readContents(const FilePath* filePath, void* output, size_t size)
{
	assert(output);
	FileStream file;
	if (file.open(filePath, MODE_READ))
	{
		size_t fileSize = file.getSize();
		size = size <= fileSize ? size : fileSize;
		file.readBuffer(output, (u32)size);
		file.close();
		return (u32)size;
	}
	return 0;
}

bool FileStream::seek(s32 offset, Origin origin)
{
	const s32 seekOrigin[] = { SEEK_SET, SEEK_END, SEEK_CUR };
	if (m_file)
	{
		return fseek(m_file, offset, seekOrigin[origin]) == 0;
	}
	if (m_archive)
	{
		return m_archive->seekFile(offset, seekOrigin[origin]);
	}
	return false;
}

size_t FileStream::getLoc()
{
	if (m_file)
	{
		return (size_t)ftell(m_file);
	}
	if (m_archive)
	{
		return m_archive->getLocInFile();
	}
	return 0;
}

size_t FileStream::getSize()
{
	if (m_file)
	{
		seek(0, ORIGIN_END);
		size_t size = getLoc();
		seek(0, ORIGIN_START);
		return size;
	}
	if (m_archive)
	{
		return m_archive->getFileLength();
	}
	return 0;
}

bool FileStream::isOpen() const
{
	return m_file != nullptr || m_archive != nullptr;
}

u32 FileStream::readBuffer(void* ptr, u32 size, u32 count)
{
	assert(m_mode == MODE_READ || m_mode == MODE_READWRITE || m_mode == MODE_APPEND);
	if (m_file)
	{
		return (u32)fread(ptr, size, count, m_file) * size;
	}
	if (m_archive)
	{
		return (u32)m_archive->readFile(ptr, (size_t)size * (size_t)count);
	}
	return 0;
}

void FileStream::writeBuffer(const void* ptr, u32 size, u32 count)
{
	assert(m_mode == MODE_WRITE || m_mode == MODE_READWRITE || m_mode == MODE_APPEND);
	if (m_file)
	{
		fwrite(ptr, size, count, m_file);
	}
}

void FileStream::writeString(const char* fmt, ...)
{
	assert(m_mode == MODE_WRITE || m_mode == MODE_READWRITE || m_mode == MODE_APPEND);
	if (!m_file)
	{
		return;
	}

	char tmpStr[4096] = { 0 };
	va_list arg;
	va_start(arg, fmt);
	vsnprintf(tmpStr, sizeof(tmpStr), fmt, arg);
	va_end(arg);

	const size_t len = strlen(tmpStr);
	fwrite(tmpStr, len, 1, m_file);
}

void FileStream::flush()
{
	if (m_file)
	{
		fflush(m_file);
	}
}

void FileStream::readString(std::string* ptr, u32 count)
{
	assert(m_mode == MODE_READ || m_mode == MODE_READWRITE || m_mode == MODE_APPEND);
	assert(count <= 256);

	readBuffer(s_probeWorkBufferU32, sizeof(u32), count);
	for (u32 s = 0; s < count; ++s)
	{
		assert(s_probeWorkBufferU32[s] <= sizeof(s_probeWorkBufferChar) - 1);
		readBuffer(s_probeWorkBufferChar, 1, s_probeWorkBufferU32[s]);
		s_probeWorkBufferChar[s_probeWorkBufferU32[s]] = 0;
		ptr[s] = s_probeWorkBufferChar;
	}
}

void FileStream::writeString(const std::string* ptr, u32 count)
{
	assert(m_mode == MODE_WRITE || m_mode == MODE_READWRITE || m_mode == MODE_APPEND);
	assert(m_file);
	assert(count <= 256);

	for (u32 s = 0; s < count; ++s)
	{
		s_probeWorkBufferU32[s] = (u32)ptr[s].length();
	}
	fwrite(s_probeWorkBufferU32, sizeof(u32), count, m_file);

	for (u32 s = 0; s < count; ++s)
	{
		fwrite(ptr[s].data(), 1, s_probeWorkBufferU32[s], m_file);
	}
}

namespace TFE_Jedi
{
void __attribute__((weak)) message_addAddress(const char* name, s32 param0, s32 param1, RSector* sector)
{
	(void)name;
	(void)param0;
	(void)param1;
	(void)sector;
}

MessageAddress* __attribute__((weak)) message_getAddress(const char* name)
{
	(void)name;
	return nullptr;
}

void __attribute__((weak)) message_free() {}

void __attribute__((weak)) level_serializeMessageAddresses(Stream* stream)
{
	(void)stream;
}

InfElevator* __attribute__((weak)) inf_allocateSpecialElevator(RSector* sector, InfSpecialElevator type)
{
	(void)sector;
	(void)type;
	return nullptr;
}

InfElevator* __attribute__((weak)) inf_allocateElevItem(RSector* sector, InfElevatorType type)
{
	(void)sector;
	(void)type;
	return nullptr;
}

JBool __attribute__((weak)) inf_load(const char* levelName)
{
	(void)levelName;
	return JTRUE;
}

void __attribute__((weak)) inf_loadSounds() {}
void __attribute__((weak)) inf_loadScripts() {}

void __attribute__((weak)) inf_sendSectorMessage(RSector* sector, MessageType msgType)
{
	(void)sector;
	(void)msgType;
}

void __attribute__((weak)) inf_sendLinkMessages(Allocator* infLink, SecObject* entity, u32 evt, MessageType msgType)
{
	(void)infLink;
	(void)entity;
	(void)evt;
	(void)msgType;
}

// rsector.o (sector_addObject/sector_removeObject) references this. The real body lives in
// message.cpp and pulls in the whole INF system; for static geometry render it is a no-op.
void __attribute__((weak)) message_sendToSector(RSector* sector, SecObject* entity, u32 evt, MessageType msgType)
{
	(void)sector;
	(void)entity;
	(void)evt;
	(void)msgType;
}
}

namespace TFE_Sprite_Jedi
{
	void __attribute__((weak)) freeLevelData() {}
}

namespace TFE_Model_Jedi
{
	void __attribute__((weak)) freeLevelData() {}
}

namespace TFE_ForceScript
{
	s32 __attribute__((weak)) execFunc(FunctionHandle funcHandle, s32 argCount, const ScriptArg* arg)
	{
		(void)funcHandle;
		(void)argCount;
		(void)arg;
		return 0;
	}
}

namespace TFE_Settings
{
	bool __attribute__((weak)) isHdAssetValid(const char* assetName, HdAssetType type)
	{
		(void)assetName;
		(void)type;
		return false;
	}
}

namespace FileUtil
{
	void __attribute__((weak)) replaceExtension(const char* srcPath, const char* newExt, char* outPath)
	{
		if (!outPath)
		{
			return;
		}
		if (!srcPath)
		{
			outPath[0] = 0;
			return;
		}

		strcpy(outPath, srcPath);
		if (!newExt)
		{
			return;
		}

		char* dot = strrchr(outPath, '.');
		if (!dot)
		{
			dot = outPath + strlen(outPath);
			*dot++ = '.';
			*dot = 0;
		}
		else
		{
			dot++;
		}
		strcpy(dot, newExt);
	}
}

namespace TFE_DarkForces
{
	u32 __attribute__((weak)) s_playerDying = 0;
	Task* __attribute__((weak)) s_gasmaskTask = nullptr;
	JBool __attribute__((weak)) s_drawAutomap = JFALSE;
	JBool __attribute__((weak)) s_automapLocked = JFALSE;
	PlayerInfo __attribute__((weak)) s_playerInfo = {};
	fixed16_16 __attribute__((weak)) s_batteryPower = 0;
	Tick __attribute__((weak)) s_playerTick = 0;
	fixed16_16 __attribute__((weak)) s_playerYPos = 0;
	SecObject* __attribute__((weak)) s_playerEye = nullptr;
	JBool __attribute__((weak)) s_externalCameraMode = JFALSE;
	JBool __attribute__((weak)) s_wearingCleats = JFALSE;
	JBool __attribute__((weak)) s_wearingGasmask = JFALSE;
	JBool __attribute__((weak)) s_nightVisionActive = JFALSE;
	JBool __attribute__((weak)) s_headlampActive = JFALSE;
	SoundSourceId __attribute__((weak)) s_nightVisionActiveSoundSource = NULL_SOUND;
	SoundSourceId __attribute__((weak)) s_nightVisionDeactiveSoundSource = NULL_SOUND;
	GameConfig __attribute__((weak)) s_config = { JFALSE, JFALSE, JFALSE, JFALSE, JTRUE };
	char __attribute__((weak)) s_cheatString[32] = { 0 };
	s32 __attribute__((weak)) s_cheatCharIndex = 0;
	s32 __attribute__((weak)) s_cheatInputCount = 0;

	SoundEffectId __attribute__((weak)) sound_play(SoundSourceId sourceId)
	{
		(void)sourceId;
		return NULL_SOUND;
	}

	void __attribute__((weak)) time_pause(JBool pause)
	{
		(void)pause;
	}

	Tick __attribute__((weak)) time_frameRateToDelay(u32 frameRate)
	{
		return frameRate > 0 ? 1u : 0u;
	}

	Tick __attribute__((weak)) time_frameRateToDelay(s32 frameRate)
	{
		return frameRate > 0 ? 1u : 0u;
	}

	Tick __attribute__((weak)) time_frameRateToDelay(f32 frameRate)
	{
		return frameRate > 0.0f ? 1u : 0u;
	}

	CheatID __attribute__((weak)) cheat_getID()
	{
		return CHEAT_NONE;
	}

	CheatID __attribute__((weak)) cheat_getIDFromString(const char* cheatStr)
	{
		(void)cheatStr;
		return CHEAT_NONE;
	}

	const char* __attribute__((weak)) cheat_getStringFromID(CheatID id)
	{
		(void)id;
		return "";
	}

	void __attribute__((weak)) cheat_toggleHeightCheck() {}
	void __attribute__((weak)) cheat_teleport() {}
	void __attribute__((weak)) cheat_bugMode() {}
	void __attribute__((weak)) cheat_pauseAI() {}
	void __attribute__((weak)) cheat_postal() {}
	void __attribute__((weak)) cheat_godMode() {}
	void __attribute__((weak)) cheat_fullAmmo() {}
	void __attribute__((weak)) cheat_unlock() {}
	void __attribute__((weak)) cheat_maxout() {}
	void __attribute__((weak)) cheat_fly() {}
	void __attribute__((weak)) cheat_noclip() {}
	void __attribute__((weak)) cheat_tester() {}
	void __attribute__((weak)) cheat_addLife() {}
	void __attribute__((weak)) cheat_subLife() {}
	void __attribute__((weak)) cheat_maxLives() {}
	void __attribute__((weak)) cheat_die() {}
	void __attribute__((weak)) cheat_oneHitKill() {}
	void __attribute__((weak)) cheat_instaDeath() {}

	void __attribute__((weak)) agent_levelComplete() {}

	void __attribute__((weak)) hud_sendTextMessage(s32 msgId)
	{
		(void)msgId;
	}

	void __attribute__((weak)) hud_sendTextMessage(const char* msg, s32 priority, bool skipPriority)
	{
		(void)msg;
		(void)priority;
		(void)skipPriority;
	}

	void __attribute__((weak)) hud_clearMessage() {}
	void __attribute__((weak)) hud_startup(JBool fromSave)
	{
		(void)fromSave;
	}

	void __attribute__((weak)) hud_toggleDataDisplay() {}

	JBool __attribute__((weak)) hud_setupToggleAnim1(JBool enable)
	{
		(void)enable;
		return JTRUE;
	}

	void __attribute__((weak)) hud_drawMessage(u8* framebuffer)
	{
		(void)framebuffer;
	}

	void __attribute__((weak)) hud_drawAndUpdate(u8* framebuffer)
	{
		(void)framebuffer;
	}

	void __attribute__((weak)) automap_computeScreenBounds() {}

	void __attribute__((weak)) automap_updateMapData(MapUpdateID id)
	{
		(void)id;
	}

	void __attribute__((weak)) automap_resetScale() {}

	void __attribute__((weak)) automap_draw(u8* framebuffer)
	{
		(void)framebuffer;
	}

	s32 __attribute__((weak)) automap_getLayer()
	{
		return 0;
	}

	void __attribute__((weak)) automap_disableLock() {}
	void __attribute__((weak)) automap_enableLock() {}

	void __attribute__((weak)) weapon_clearFireRate() {}
	void __attribute__((weak)) weapon_createPlayerWeaponTask() {}

	void __attribute__((weak)) weapon_enableAutomount(JBool enable)
	{
		(void)enable;
	}

	void __attribute__((weak)) weapon_holster() {}

	void __attribute__((weak)) weapon_draw(u8* display, DrawRect* rect)
	{
		(void)display;
		(void)rect;
	}

	void __attribute__((weak)) player_cycleWeapons(s32 change)
	{
		(void)change;
	}

	void __attribute__((weak)) player_createController(JBool clearData)
	{
		(void)clearData;
	}

	void __attribute__((weak)) player_clearEyeObject() {}

	// N64 free-look camera: until player.cpp / input mapping are de-shimmed, drive the
	// player eye object directly from the controller so the real jediRenderer produces a
	// live, navigable view. joypad is polled each frame by the platform event pump.
	static void probe_updateFreeLookCameraN64(SecObject* eye)
	{
		using namespace TFE_Jedi;
		if (!eye) { return; }

		const joypad_inputs_t in = joypad_get_inputs(JOYPAD_PORT_1);
		const joypad_buttons_t btn = joypad_get_buttons_held(JOYPAD_PORT_1);

		s32 sx = in.stick_x;
		s32 sy = in.stick_y;
		if (sx > -8 && sx < 8) { sx = 0; }
		if (sy > -8 && sy < 8) { sy = 0; }

		// Turn (yaw) from the analog stick X axis (tunable scale).
		eye->yaw = (eye->yaw + sx * 3) & ANGLE_MASK;

		// Pitch from C-Up / C-Down, clamped to the renderer's +/-60 degree range.
		if (btn.c_up)   { eye->pitch += 96; }
		if (btn.c_down) { eye->pitch -= 96; }
		if (eye->pitch >  2730) { eye->pitch =  2730; }
		if (eye->pitch < -2730) { eye->pitch = -2730; }

		const fixed16_16 fwdX = sinFixed(eye->yaw);
		const fixed16_16 fwdZ = cosFixed(eye->yaw);

		// Forward/back from stick Y, strafe from C-Left/C-Right, vertical fly from L/R.
		const fixed16_16 fwd    = (fixed16_16)(sy * 384);	// tunable move speed
		const fixed16_16 strafe = 0xC000;					// ~0.75 units/frame (tunable)

		vec3_fixed pos = eye->posWS;
		pos.x += mul16(fwd, fwdX);
		pos.z += mul16(fwd, fwdZ);
		if (btn.c_right) { pos.x += mul16(strafe, fwdZ); pos.z -= mul16(strafe, fwdX); }
		if (btn.c_left)  { pos.x -= mul16(strafe, fwdZ); pos.z += mul16(strafe, fwdX); }
		if (btn.r) { pos.y -= 0x10000; }	// up   (Y increases downward in DF)
		if (btn.l) { pos.y += 0x10000; }	// down

		// Commit only if the new position is inside a valid sector, so the real renderer is
		// never handed a null start sector (which would dereference null and crash).
		RSector* ns = sector_which3D(pos.x, pos.y, pos.z);
		if (ns)
		{
			eye->posWS = pos;
			eye->sector = ns;
		}
	}

	void __attribute__((weak)) player_setupCamera()
	{
		if (!s_playerEye) { return; }

		// Real jediRenderer is now linked; seed the fixed renderer (sector renderer,
		// resolution, limits, camera/lights) before the strong renderer_computeCameraTransform runs.
		probe_ensureFixedRendererReady();

		// Drive the eye from the controller (free-look) so the scene is navigable.
		probe_updateFreeLookCameraN64(s_playerEye);

		// Eye position sits at the top of the player capsule (feet pos minus world height).
		const fixed16_16 camX = s_playerEye->posWS.x;
		const fixed16_16 camY = s_playerEye->posWS.y - s_playerEye->worldHeight;
		const fixed16_16 camZ = s_playerEye->posWS.z;
		const angle14_32 pitch = s_playerEye->pitch;
		const angle14_32 yaw   = s_playerEye->yaw;

		if (s_playerEye->sector)
		{
			TFE_Jedi::renderer_computeCameraTransform(s_playerEye->sector, pitch, yaw, camX, camY, camZ);
		}
		// Bright baseline ambient until headlamp/light simulation is de-shimmed.
		TFE_Jedi::renderer_setWorldAmbient(0);

		static u32 s_playerCamLogCount = 0;
		if (s_playerCamLogCount == 0 || (s_playerCamLogCount % 120u) == 0)
		{
			debugf("[mission_probe] player_setupCamera sectorId=%ld camX=%ld camY=%ld camZ=%ld worldH=%ld pitch=%ld yaw=%ld\n",
				s_playerEye->sector ? (long)s_playerEye->sector->id : (long)-1,
				(long)camX, (long)camY, (long)camZ,
				(long)s_playerEye->worldHeight, (long)pitch, (long)yaw);
		}
		s_playerCamLogCount++;
	}

	void __attribute__((weak)) logic_clearScriptCalls() {}

	JBool __attribute__((weak)) object_parseSeq(SecObject* obj, TFE_Parser* parser, size_t* bufferPos)
	{
		(void)obj;
		(void)parser;
		(void)bufferPos;
		return JFALSE;
	}

	SoundSourceId __attribute__((weak)) sound_load(const char* sound, u32 priority)
	{
		(void)sound;
		(void)priority;
		return NULL_SOUND;
	}

	SoundEffectId __attribute__((weak)) sound_maintain(SoundEffectId idInstance, SoundSourceId idSound, vec3_fixed pos)
	{
		(void)idInstance;
		(void)idSound;
		(void)pos;
		return NULL_SOUND;
	}

	SecObject* __attribute__((weak)) logic_spawnEnemy(const char* waxName, const char* typeName)
	{
		(void)waxName;
		(void)typeName;
		return nullptr;
	}

	void __attribute__((weak)) pickup_createTask() {}
	void __attribute__((weak)) pickupSupercharge() {}
	void __attribute__((weak)) gasmaskTaskFunc(MessageType msg)
	{
		(void)msg;
	}
	void __attribute__((weak)) projectile_createTask() {}
	void __attribute__((weak)) actor_createTask() {}
	void __attribute__((weak)) hitEffect_createTask() {}
	void __attribute__((weak)) updateLogic_clearTask() {}

	void __attribute__((weak)) setSpriteAnimation(Task* spriteAnimTask, Allocator* spriteAnimAlloc)
	{
		(void)spriteAnimTask;
		(void)spriteAnimAlloc;
	}

	JBool __attribute__((weak)) escapeMenu_isOpen()
	{
		return JFALSE;
	}

	EscapeMenuAction __attribute__((weak)) escapeMenu_update()
	{
		return ESC_RETURN;
	}

	void __attribute__((weak)) escapeMenu_open(u8* framebuffer, u8* palette)
	{
		(void)framebuffer;
		(void)palette;
	}

	void __attribute__((weak)) escapeMenu_resetLevel() {}

	void __attribute__((weak)) pda_start(const char* levelName)
	{
		(void)levelName;
	}

	JBool __attribute__((weak)) pda_isOpen()
	{
		return JFALSE;
	}

	void __attribute__((weak)) pda_update() {}
}

// ---------------------------------------------------------------------------
// N64 direct-render: minimal player eye + camera bootstrap.
//
// The geometry-only mission load path skips the full object/logic stack, so
// TFE_DarkForces::s_playerEye stays null and the renderer falls back to a
// checker pattern. This routine parses just the level's .O player start
// (CLASS line + "LOGIC: PLAYER" sequence), allocates a lightweight eye object
// in the correct sector, and primes the camera transform so the real software
// renderer draws the level from the player's viewpoint.
// ---------------------------------------------------------------------------
void n64_mission_setupPlayerEye(const char* levelName, u8 difficulty)
{
	using namespace TFE_Jedi;
	(void)difficulty;

	TFE_DarkForces::s_playerEye = nullptr;
	if (!levelName) { return; }

	char levelPath[TFE_MAX_PATH];
	strcpy(levelPath, levelName);
	strcat(levelPath, ".O");

	FilePath filePath;
	if (!TFE_Paths::getFilePath(levelPath, &filePath))
	{
		TFE_System::logWrite(LOG_WARNING, "n64_mission", "No object file '%s' for player start.", levelPath);
		return;
	}

	FileStream file;
	if (!file.open(&filePath, Stream::MODE_READ))
	{
		TFE_System::logWrite(LOG_WARNING, "n64_mission", "Cannot open object file '%s'.", levelPath);
		return;
	}

	const size_t len = file.getSize();
	std::vector<char> buffer(len + 1, 0);
	file.readBuffer(buffer.data(), (u32)len);
	file.close();

	TFE_Parser parser;
	size_t bufferPos = 0;
	parser.init(buffer.data(), len);
	parser.enableBlockComments();
	parser.addCommentString("//");
	parser.addCommentString("#");
	parser.convertToUpperCase(true);

	bool haveObj = false;
	bool foundPlayer = false;
	f32 px = 0.0f, py = 0.0f, pz = 0.0f, ppch = 0.0f, pyaw = 0.0f, prol = 0.0f;
	f32 lx = 0.0f, ly = 0.0f, lz = 0.0f, lpch = 0.0f, lyaw = 0.0f, lrol = 0.0f;

	const char* line;
	while ((line = parser.readLine(bufferPos)) != nullptr)
	{
		char objClass[32];
		s32 dataIndex = 0, objDiff = 0;
		f32 x = 0, y = 0, z = 0, pch = 0, yaw = 0, rol = 0;
		if (sscanf(line, " CLASS: %31s DATA: %d X: %f Y: %f Z: %f PCH: %f YAW: %f ROL: %f DIFF: %d",
			objClass, &dataIndex, &x, &y, &z, &pch, &yaw, &rol, &objDiff) > 5)
		{
			lx = x; ly = y; lz = z; lpch = pch; lyaw = yaw; lrol = rol;
			haveObj = true;
			continue;
		}

		char arg0[32], arg1[32];
		if (haveObj && sscanf(line, " %31s %31s", arg0, arg1) == 2)
		{
			const KEYWORD key = getKeywordIndex(arg0);
			if ((key == KW_LOGIC || key == KW_TYPE) && getKeywordIndex(arg1) == KW_PLAYER)
			{
				px = lx; py = ly; pz = lz; ppch = lpch; pyaw = lyaw; prol = lrol;
				foundPlayer = true;
				break;
			}
		}
	}

	if (!foundPlayer)
	{
		TFE_System::logWrite(LOG_WARNING, "n64_mission", "No PLAYER logic in '%s'; renderer keeps probe fallback.", levelPath);
		return;
	}

	vec3_fixed posWS;
	posWS.x = floatToFixed16(px);
	posWS.y = floatToFixed16(py);
	posWS.z = floatToFixed16(pz);

	RSector* sector = sector_which3D(posWS.x, posWS.y, posWS.z);
	if (!sector && s_levelState.sectorCount > 0)
	{
		sector = &s_levelState.sectors[0];
	}
	if (!sector)
	{
		TFE_System::logWrite(LOG_ERROR, "n64_mission", "Player start sector not found in '%s'.", levelPath);
		return;
	}

	SecObject* eye = allocateObject();
	if (!eye) { return; }
	eye->posWS = posWS;
	eye->pitch = floatDegreesToFixed(ppch);
	eye->yaw   = floatDegreesToFixed(pyaw);
	eye->roll  = floatDegreesToFixed(prol);
	eye->worldWidth  = 0x1cccc;	// PLAYER_WIDTH  (1.8 units)
	eye->worldHeight = 0x5cccc;	// PLAYER_HEIGHT (5.8 units)
	eye->entityFlags |= ETFLAG_PLAYER;
	sector_addObject(sector, eye);

	TFE_DarkForces::s_playerEye = eye;
	TFE_DarkForces::s_playerYPos = eye->posWS.y;
	TFE_DarkForces::player_setupCamera();

	TFE_System::logWrite(LOG_MSG, "n64_mission", "Player eye ready: sector=%ld yaw=%ld", (long)sector->id, (long)eye->yaw);
	debugf("[df_mission_real] player eye setup sector=%d yaw=%d\n", (int)sector->id, (int)eye->yaw);
}