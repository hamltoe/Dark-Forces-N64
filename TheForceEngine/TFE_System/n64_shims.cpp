//////////////////////////////////////////////////////////////////////
// N64-only engine shims.
//
// Provides host/game symbols referenced by the linked fixed-point render
// path whose real implementations pull non-portable deps (SDL/ImGui/audio/
// script) or aren't brought up yet. Most are no-ops/null; a few have real
// minimal logic. Grown iteratively as the engine link is brought up.
//////////////////////////////////////////////////////////////////////
#include <libdragon.h>

#include <TFE_System/types.h>
#include <TFE_System/system.h>
#include <TFE_System/profiler.h>
#include <TFE_Memory/memoryRegion.h>
#include <TFE_Settings/settings.h>
#include <TFE_FileSystem/fileutil.h>
#include <TFE_FileSystem/paths.h>
#include <TFE_Archive/gobArchive.h>            // GobArchive, INVALID_FILE
#include <TFE_FrontEndUI/console.h>
#include <TFE_Jedi/Math/core_math.h>
#include <TFE_Jedi/Math/fixedPoint.h>            // FIXED, intToFixed16
#include <TFE_Jedi/InfSystem/message.h>          // MessageType, RSector, SecObject, message_*
#include <TFE_Jedi/InfSystem/infPublicTypes.h>   // InfSpecialElevator
#include <TFE_Jedi/Task/task.h>                   // Task, TaskFunc, ctx*, itask_yield, createSubTask
#include <TFE_Jedi/Renderer/jediRenderer.h>      // render_setResolution, drawWorld, camera transform
#include <TFE_Jedi/Level/levelData.h>            // s_levelState (sectors, palette name)
#include <TFE_Jedi/Level/robjData.h>             // ETFLAG_PLAYER (object SEQ parse)
#include <TFE_System/parser.h>                    // TFE_Parser::readLine (object SEQ parse)
#include <TFE_RenderBackend/renderBackend.h>     // setPalette
#include <TFE_FileSystem/filestream.h>           // FileStream (level palette load)
#include <TFE_Jedi/Level/rwall.h>                 // RWall (inf wall stubs)

// Actor-AI host stubs: the real player/sound/hitEffect/pickup/item/weapon/
// generator/vueLogic/updateLogic/gameMusic/ExternalData sources are not linked
// on N64 yet. Pull their headers so the stub signatures (and mangled names)
// match the linked actor code exactly.
#include <TFE_DarkForces/player.h>
#include <TFE_DarkForces/sound.h>
#include <TFE_DarkForces/hitEffect.h>
#include <TFE_DarkForces/pickup.h>
#include <TFE_DarkForces/item.h>
#include <TFE_DarkForces/weapon.h>
#include <TFE_DarkForces/generator.h>
#include <TFE_DarkForces/vueLogic.h>
#include <TFE_DarkForces/updateLogic.h>
#include <TFE_DarkForces/gameMusic.h>
#include <TFE_DarkForces/projectile.h>          // ProjectileLogic, PROJ_COUNT
#include <TFE_DarkForces/animLogic.h>            // setSpriteAnimation (sprite-anim task reset)
#include <TFE_DarkForces/Actor/actor.h>          // actor_createTask / clearState / physics list
#include <TFE_DarkForces/time.h>                  // updateTime (drives s_curTick + s_frameTicks)
#include <TFE_Jedi/Level/rsector.h>               // sector_addObject (player-object relink)
#include <TFE_ExternalData/dfLogics.h>
#include <TFE_ExternalData/weaponExternal.h>

#include <cstring>
#include <cstddef>
#include <cstdlib>
#include <vector>
#include <string>

// ---------------------------------------------------------------------
// Memory regions backing game_alloc / level_alloc (see TFE_Game/igame.h).
// ---------------------------------------------------------------------
MemoryRegion* s_gameRegion  = nullptr;
MemoryRegion* s_levelRegion = nullptr;

namespace TFE_Jedi { void bitmap_setAllocator(MemoryRegion* allocator); }

namespace TFE_N64
{
	void initEngineRegions()
	{
		// Geometry-only path: level data + textures both come from the level region.
		// The game region is barely touched here, so keep it small to fit 4 MiB RDRAM.
		// (region_alloc fails any single alloc > blockSize, so the level block stays 1 MiB.)
		if (!s_gameRegion)  { s_gameRegion  = TFE_Memory::region_create("game",   256u * 1024u); }
		if (!s_levelRegion) { s_levelRegion = TFE_Memory::region_create("level", 1024u * 1024u); }

		// bitmap_load() allocates TextureData from s_texState.memoryRegion. The DarkForces
		// boot normally sets this; we call level_loadGeometry directly, so wire it here.
		TFE_Jedi::bitmap_setAllocator(s_levelRegion);
	}
}

// ---------------------------------------------------------------------
// TFE_System
// ---------------------------------------------------------------------
namespace TFE_System
{
	u64 getCurrentTimeInTicks()
	{
		return (u64)get_ticks();
	}

	// Wall-clock seconds since boot. Used by the task system's optional time
	// limiter (disabled on N64 since we run one task_run() per frame).
	f64 getTime()
	{
		// libdragon's hardware counter runs at TICKS_PER_SECOND (46.875 MHz).
		return (f64)get_ticks() / (f64)TICKS_PER_SECOND;
	}

	// Per-frame delta in seconds, consumed by TFE_DarkForces::updateTime() to
	// advance s_curTick / s_deltaTime / s_frameTicks (drives real actor timing).
	f64 getDeltaTime()
	{
		static u64 s_prevTicks = 0;
		const u64 now = (u64)get_ticks();
		if (s_prevTicks == 0) { s_prevTicks = now; }
		u64 delta = now - s_prevTicks;
		s_prevTicks = now;
		f64 dt = (f64)delta / (f64)TICKS_PER_SECOND;
		// Clamp to a sane range so a long first frame / stall can't explode movement.
		if (dt <= 0.0)   { dt = 1.0 / 60.0; }
		if (dt > 0.1)    { dt = 0.1; }
		return dt;
	}
}

// ---------------------------------------------------------------------
// TFE_DarkForces game globals (time.cpp + random.cpp now linked, so
// s_curTick / time_frameRateToDelay come from the real engine).
// ---------------------------------------------------------------------
namespace TFE_DarkForces
{
	// Player/level globals referenced by the render + level-load path.
	u8  s_levelPalette[768] = { 0 };
	u8  s_basePalette[768]  = { 0 };
	s32 s_palModified  = 0;   // JBool
	u32 s_playerDying  = 0;   // matches pickup.h
	s32 s_playerYPos   = 0;   // fixed16_16

	// ---- Actor-AI host globals (player/sound/effects not linked on N64) ----
	SecObject*    s_playerObject       = nullptr;  // actor chase target (set at level load)
	TFE_Jedi::vec3_fixed s_eyePos      = { 0, 0, 0 };
	JBool         s_aiActive           = JTRUE;    // ACTOR_YIELD spins until this is set
	JBool         s_headlampActive     = JFALSE;
	Tick          s_reviveTick         = 0;
	fixed16_16    s_gravityAccel       = FIXED(150);
	s32           s_baseAtten          = 0;
	s32           s_lastMaintainVolume = 0;
	EffectData*   s_curEffectData      = nullptr;
	SoundSourceId s_pistolSndSrc       = 0;
	SoundSourceId s_rifleSndSrc        = 0;
	SoundSourceId s_concussion5SndSrc  = 0;
	SoundSourceId s_plasma4SndSrc      = 0;
	SoundSourceId s_missile1SndSrc     = 0;

	// ---- Player queries used by actor targeting / collision ----
	void player_setupObject(SecObject* obj)  { s_playerObject = obj; }
	void player_setupEyeObject(SecObject*)   {}
	void player_getVelocity(TFE_Jedi::vec3_fixed* vel) { if (vel) { vel->x = 0; vel->y = 0; vel->z = 0; } }
	void player_applyDamage(fixed16_16, fixed16_16, JBool) {}
	fixed16_16 player_getSquaredDistance(SecObject* obj)
	{
		if (!obj || !s_playerObject) { return FIXED(32767); }
		fixed16_16 dx = obj->posWS.x - s_playerObject->posWS.x;
		fixed16_16 dy = obj->posWS.y - s_playerObject->posWS.y;
		fixed16_16 dz = obj->posWS.z - s_playerObject->posWS.z;
		return mul16(dx, dx) + mul16(dy, dy) + mul16(dz, dz);
	}

	// ---- Sound (audio system not linked yet) ----
	SoundEffectId sound_playCued(SoundSourceId, TFE_Jedi::vec3_fixed) { return 0; }
	void sound_stop(SoundEffectId) {}
	void sound_setVolume(SoundEffectId, s32) {}
	void sound_adjustCued(SoundEffectId, TFE_Jedi::vec3_fixed) {}

	// ---- Hit effects / explosions (no particle/effect system yet) ----
	void spawnHitEffect(HitEffectID, RSector*, TFE_Jedi::vec3_fixed, SecObject*) {}
	void computeExplosionPushDir(TFE_Jedi::vec3_fixed*, TFE_Jedi::vec3_fixed* pushDir)   { if (pushDir) { pushDir->x = 0; pushDir->y = 0; pushDir->z = 0; } }
	void computeDamagePushVelocity(ProjectileLogic*, TFE_Jedi::vec3_fixed* vel)          { if (vel) { vel->x = 0; vel->y = 0; vel->z = 0; } }

	// ---- Pickups / items (inventory system not linked) ----
	ItemId     getPickupItemId(const char*)         { return (ItemId)0; }
	SecObject* item_create(ItemId)                  { return nullptr; }
	Logic*     obj_createPickup(SecObject*, ItemId)  { return nullptr; }

	// ---- Logic types not used by basic enemy AI ----
	Logic* obj_createGenerator(SecObject*, LogicSetupFunc*, KEYWORD, const char*) { return nullptr; }
	Logic* obj_createVueLogic(SecObject*, LogicSetupFunc*)                        { return nullptr; }
	Logic* obj_setUpdate(SecObject*, LogicSetupFunc*)                             { return nullptr; }

	// ---- Weapon / music ----
	void weapon_computeMatrix(fixed16_16*, angle14_32, angle14_32) {}
	void gameMusic_startFight()   {}
	void gameMusic_sustainFight() {}
}

// ---------------------------------------------------------------------
// TFE_Console (debug console: no-op on N64)
// ---------------------------------------------------------------------
namespace TFE_Console
{
	void addToHistory(const char*) {}
	void registerCVarInt(const char*, u32, s32*, const char*) {}
	void registerCVarBool(const char*, u32, bool*, const char*) {}
	void registerCommand(const char*, ConsoleFunc, u32, const char*, bool) {}
}

// ---------------------------------------------------------------------
// TFE_Profiler (no-op on N64)
// ---------------------------------------------------------------------
namespace TFE_Profiler
{
	u32  beginZone(const char*, const char*, u32) { return 0; }
	void endZone(u32, u64) {}
	void addCounter(const char*, s32*) {}
}

// ---------------------------------------------------------------------
// TFE_Settings (return safe N64 defaults; no INI/console)
// ---------------------------------------------------------------------
namespace TFE_Settings
{
	static TFE_Settings_Graphics s_n64Graphics = {};
	TFE_Settings_Graphics* getGraphicsSettings() { return &s_n64Graphics; }
	void setLevelName(const char*) {}
	bool isHdAssetValid(const char*, HdAssetType) { return false; }
	bool extendAdjoinLimits() { return false; }
	bool ignore3doLimits() { return s_n64Graphics.ignore3doLimits; }
	bool normalFix3do() { return s_n64Graphics.fix3doNormalOverflow; }

	static TFE_Settings_Game s_n64Game = {};
	TFE_Settings_Game* getGameSettings() { return &s_n64Game; }
	bool jsonAiLogics()     { return false; }   // no external JSON AI on N64
	bool stepSecondAlt()    { return false; }
	bool enableUnusedItem() { return false; }
}

// ---------------------------------------------------------------------
// FileUtil
// ---------------------------------------------------------------------
namespace FileUtil
{
	void replaceExtension(const char* srcPath, const char* newExt, char* outPath)
	{
		strcpy(outPath, srcPath);
		char* dot = strrchr(outPath, '.');
		if (dot) { strcpy(dot + 1, newExt); }
		else     { strcat(outPath, "."); strcat(outPath, newExt); }
	}
}

// ---------------------------------------------------------------------
// TFE_Paths: resolve a game file by searching the staged archives. DF
// levels/objects/INF live in DARK.GOB; wall/floor art in TEXTURES.GOB.
// FileStream::open(FilePath*) reads straight from archive->openFile(index).
// ---------------------------------------------------------------------
namespace TFE_Paths
{
	static GobArchive* openStagedGob(const char* romPath)
	{
		GobArchive* gob = new GobArchive();
		if (!gob->open(romPath)) { delete gob; return nullptr; }
		return gob;
	}

	bool getFilePath(const char* fileName, FilePath* path)
	{
		static GobArchive* s_archives[3] =
		{
			openStagedGob("rom:/DARK.GOB"),
			openStagedGob("rom:/TEXTURES.GOB"),
			openStagedGob("rom:/SPRITES.GOB"),
		};

		path->archive = nullptr;
		path->index   = INVALID_FILE;
		strncpy(path->path, fileName, TFE_MAX_PATH - 1);
		path->path[TFE_MAX_PATH - 1] = 0;

		for (s32 i = 0; i < 3; i++)
		{
			if (!s_archives[i]) { continue; }
			u32 index = s_archives[i]->getFileIndex(fileName);
			if (index != INVALID_FILE)
			{
				path->archive = s_archives[i];
				path->index   = index;
				return true;
			}
		}
		return false;
	}
}

// ---------------------------------------------------------------------
// SecObject, RSector, Task, MessageType, AssetPool, InfSpecialElevator are
// provided by the engine headers above. The remaining opaque pointer types
// only need a forward declaration (avoids pulling SDL-tainted asset headers).
// ---------------------------------------------------------------------
struct JediModel;
struct Wax;
struct WaxFrame;
class  TFE_Parser;

namespace TFE_ForceScript { struct ScriptArg; }

// ---------------------------------------------------------------------
// TFE_DarkForces audio + object parse (no audio / no object logic yet)
// ---------------------------------------------------------------------
namespace TFE_DarkForces
{
	s64 sound_load(const char*, u32) { return 0; }                          // SoundSourceId
	s64 sound_maintain(s64, s64, TFE_Jedi::vec3_fixed) { return 0; }        // SoundEffectId
}

// ---------------------------------------------------------------------
// Script system remains stubbed on N64 bring-up.
// ---------------------------------------------------------------------
namespace TFE_ForceScript
{
	void* createModule(const char*, const char*, bool, u32) { return nullptr; }
	void* findScriptFuncByDecl(void*, const char*)          { return nullptr; }
	void* findScriptFuncByNameNoCase(void*, const char*)    { return nullptr; }
	s32   execFunc(void*, s32, const ScriptArg*)            { return 0; }
	void* getModule(const char*)                            { return nullptr; }
}

// ---------------------------------------------------------------------
// TFE_ExternalData: external JSON logic/projectile tables are disabled on N64
// (jsonAiLogics() returns false). Return valid empty/default tables so the
// linked actor + projectile code stays in-bounds.
// ---------------------------------------------------------------------
namespace TFE_ExternalData
{
	ExternalLogics* getExternalLogics()
	{
		static ExternalLogics s_empty;   // actorLogics vector is empty
		return &s_empty;
	}
	ExternalProjectile* getExternalProjectiles()
	{
		// projectile_startup() iterates [0, PROJ_COUNT); default-construct the
		// whole table so member initializers (assetType = "") apply.
		static ExternalProjectile s_projectiles[TFE_DarkForces::PROJ_COUNT];
		return s_projectiles;
	}
}

// ---------------------------------------------------------------------
// TFE_Jedi: task/INF/message/3d-object stubs (geometry-only render).
// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
// TFE_Jedi: INF/message/3d-object stubs (geometry-only render).
// The task system (itask_yield/createSubTask/ctx*/createTask/task_run) is now
// provided by the real TFE_Jedi/Task/task.cpp, so those stubs are removed.
// ---------------------------------------------------------------------
namespace TFE_Jedi
{
	void* inf_load(const char*) { return nullptr; }
	void* inf_allocateSpecialElevator(RSector*, InfSpecialElevator) { return nullptr; }

	// INF elevator/trigger routing is not linked on N64 (no INF system yet).
	// message_sendToSector() calls these; actors never route sector messages
	// during basic movement, so no-op them to satisfy the linker.
	void inf_sendSectorMessage(RSector*, MessageType) {}
	void inf_sendLinkMessages(Allocator*, SecObject*, u32, MessageType) {}
	void inf_triggerWallEvent(RWall*, SecObject*, u32) {}
	void inf_wallAndMirrorSendMessageAtPos(RWall*, SecObject*, u32, fixed16_16, fixed16_16) {}

	// projectileTaskFunc() queries a projectile's host elevator velocity; with no INF
	// system there is no moving elevator, so report zero motion / not-on-moving-floor.
	struct InfElevator;
	void inf_getMovingElevatorVelocity(InfElevator*, vec3_fixed* vel, fixed16_16* speed)
	{
		if (vel) { vel->x = 0; vel->y = 0; vel->z = 0; }
		if (speed) { *speed = 0; }
	}
	JBool inf_isOnMovingFloor(SecObject*, InfElevator*, RSector*) { return JFALSE; }
}

// ---------------------------------------------------------------------
// TFE_Input: replay system is host-only; report "not live" so the task
// system's time limiter and time.cpp's record/playback paths stay inert.
// ---------------------------------------------------------------------
namespace TFE_Input
{
	bool isReplaySystemLive() { return false; }
	bool isRecording()        { return false; }
	bool isDemoPlayback()     { return false; }
	void saveTick()           {}
	void loadTick()           {}
}

// ---------------------------------------------------------------------
// N64 first-frame 3D bring-up.
//
// Sets up the fixed-point software renderer and draws the loaded level
// geometry from a camera placed inside a sector. Lighting uses a fullbright
// identity colormap for now (no distance shading), so the geometry is fully
// visible while the rest of the engine is still stubbed.
// ---------------------------------------------------------------------
namespace TFE_N64
{
	struct SpriteAnimState
	{
		SecObject* obj;
		u32 nextTick;
		u32 delay;
		u32 seenFrame;
	};

	static u8  s_colorMapData[256 * 32];         // level colormap (.CMP) or fullbright fallback
	static u8  s_lightRamp[128];                 // light source ramp (LIGHT_SOURCE_LEVELS)
	static u32 s_levelPaletteRGBA[256];
	static RSector*   s_camSector = nullptr;
	static fixed16_16 s_camX = 0, s_camY = 0, s_camZ = 0;
	static angle14_32 s_camYaw = 0, s_camPitch = 0;
	static std::vector<SpriteAnimState> s_spriteAnimState;
	static u32 s_behaviorTick = 0;
	static u32 s_behaviorFrame = 0;
	static bool s_behaviorLogged = false;
	static bool s_renderReady = false;

	static SpriteAnimState* findSpriteAnimState(SecObject* obj)
	{
		for (size_t i = 0; i < s_spriteAnimState.size(); i++)
		{
			if (s_spriteAnimState[i].obj == obj)
			{
				return &s_spriteAnimState[i];
			}
		}
		return nullptr;
	}

	// Load the level's palette (e.g. SECBASE.PAL) from the GOB and push it to the backend.
	static void loadLevelPalette()
	{
		FilePath palPath;
		if (!TFE_Paths::getFilePath(TFE_Jedi::s_levelState.levelPaletteName, &palPath)) { return; }
		FileStream pal;
		if (!pal.open(&palPath, Stream::MODE_READ)) { return; }
		u8 raw[768] = { 0 };
		pal.readBuffer(raw, 768);
		pal.close();
		for (s32 i = 0; i < 256; i++)
		{
			const u8 r = (u8)((raw[i * 3 + 0] << 2) | (raw[i * 3 + 0] >> 4));   // 6-bit VGA -> 8-bit
			const u8 g = (u8)((raw[i * 3 + 1] << 2) | (raw[i * 3 + 1] >> 4));
			const u8 b = (u8)((raw[i * 3 + 2] << 2) | (raw[i * 3 + 2] >> 4));
			s_levelPaletteRGBA[i] = (u32)r | ((u32)g << 8) | ((u32)b << 16) | 0xff000000u;
		}
		TFE_RenderBackend::setPalette(s_levelPaletteRGBA);
	}

	// Load the level colormap (e.g. SECBASE.CMP) for distance/ambient shading.
	// .CMP layout: 8192 bytes (256 colors x 32 light levels) + 128-byte light source ramp.
	// Falls back to a fullbright identity map if the file is missing.
	static void loadColorMap()
	{
		for (s32 light = 0; light < 32; light++)
		{
			for (s32 c = 0; c < 256; c++) { s_colorMapData[(light << 8) + c] = (u8)c; }
		}
		for (s32 i = 0; i < 128; i++) { s_lightRamp[i] = 0; }

		FilePath cmpPath;
		if (!TFE_Paths::getFilePath("SECBASE.CMP", &cmpPath)) { return; }
		FileStream cmp;
		if (!cmp.open(&cmpPath, Stream::MODE_READ)) { return; }
		cmp.readBuffer(s_colorMapData, 256 * 32);
		cmp.readBuffer(s_lightRamp, 128);
		cmp.close();
		debugf("[engine] loaded colormap SECBASE.CMP\n");
	}

	// Parse the level objects file (SECBASE.O, ASCII) for the object whose SEQ contains
	// LOGIC: PLAYER, and seat the camera there (position + facing). False if not found.
	static bool findPlayerStart()
	{
		using namespace TFE_Jedi;
		FilePath objPath;
		if (!TFE_Paths::getFilePath("SECBASE.O", &objPath)) { return false; }
		FileStream obj;
		if (!obj.open(&objPath, Stream::MODE_READ)) { return false; }
		const size_t len = obj.getSize();
		char* buf = (char*)malloc(len + 1);
		if (!buf) { obj.close(); return false; }
		obj.readBuffer(buf, (u32)len);
		obj.close();
		buf[len] = 0;

		float lastX = 0, lastY = 0, lastZ = 0, lastYaw = 0;
		bool haveObj = false, found = false;
		char* line = buf;
		while (line < buf + len)
		{
			char* nl = strchr(line, '\n');
			if (nl) { *nl = 0; }

			char cls[32];
			s32 data, diff;
			float x, y, z, pch, yaw = 0, rol = 0;
			if (sscanf(line, " CLASS: %31s DATA: %d X: %f Y: %f Z: %f PCH: %f YAW: %f ROL: %f DIFF: %d",
				cls, &data, &x, &y, &z, &pch, &yaw, &rol, &diff) > 5)
			{
				lastX = x; lastY = y; lastZ = z; lastYaw = yaw;
				haveObj = true;
			}
			else if (haveObj && strstr(line, "LOGIC") && strstr(line, "PLAYER"))
			{
				const fixed16_16 px   = floatToFixed16(lastX);
				const fixed16_16 pz   = floatToFixed16(lastZ);
				const fixed16_16 eyeY = floatToFixed16(lastY) - FIXED(6);
				RSector* sec = sector_which3D(px, eyeY, pz);
				if (sec)
				{
					s_camX = px;
					s_camZ = pz;
					s_camYaw = (angle14_32)floatDegreesToFixed(lastYaw) & ANGLE_MASK;
					s_camSector = sec;
					s_camY = sec->floorHeight - FIXED(6);
					found = true;
				}
				break;
			}

			if (!nl) { break; }
			line = nl + 1;
		}
		free(buf);
		return found;
	}

	// Per-frame framebreak boundary task. task_run() keeps processing tasks until a
	// framebreak task completes; now that real AI sub-tasks exist, without one it would
	// spin forever. All actor sub-tasks run before this each frame (see createSubTask).
	static Task* s_n64LoopTask = nullptr;
	void n64LoopTaskFunc(MessageType msg)
	{
		task_begin;
		while (msg != MSG_FREE_TASK)
		{
			task_yield(TASK_NO_DELAY);
		}
		task_end;
	}

	// Bring up the real Dark Forces actor AI so enemies think, animate, and move.
	// Must run AFTER geometry load + camera setup and BEFORE level_loadObjects: enemy
	// SEQ parsing allocates ActorDispatch items from the list created here, and the
	// actor sub-tasks must parent into the live task graph.
	void startActorSystem()
	{
		using namespace TFE_DarkForces;
		using namespace TFE_Jedi;

		// Reset the sprite-animation task/list (obj_setSpriteAnim recreates lazily).
		setSpriteAnimation(nullptr, nullptr);

		// Framebreak boundary first so task_run() always terminates each frame.
		s_n64LoopTask = createTask("n64loop", n64LoopTaskFunc, JTRUE);

		// Projectiles must exist before enemies attack: defaultAttackFunc() fires via
		// createProjectile(), which returns null (-> NULL deref crash) if s_projectiles
		// was never allocated.
		projectile_createTask();

		// Actor system: physics list, clean state, then the logic + physics tasks.
		actor_allocatePhysicsActorList();
		actor_clearState();
		actor_createTask();
	}

	void setupLevelRenderer()
	{
		using namespace TFE_Jedi;
		s_spriteAnimState.clear();
		s_behaviorTick = 0;
		s_behaviorFrame = 0;
		s_behaviorLogged = false;

		// Bring up the fixed-point software renderer: projection tables + sector renderer.
		render_setResolution(true);
		setupInitCameraAndLights();

		loadColorMap();
		loadLevelPalette();

		// Seat the camera at the real player start; fall back to sector 0's centroid.
		bool haveCam = findPlayerStart();
		if (!haveCam && s_levelState.sectorCount > 0 && s_levelState.sectors)
		{
			RSector* sector = &s_levelState.sectors[0];
			s64 sumX = 0, sumZ = 0;
			for (s32 v = 0; v < sector->vertexCount; v++)
			{
				sumX += sector->verticesWS[v].x;
				sumZ += sector->verticesWS[v].z;
			}
			const s32 vc = sector->vertexCount > 0 ? sector->vertexCount : 1;
			s_camX = (fixed16_16)(sumX / vc);
			s_camZ = (fixed16_16)(sumZ / vc);
			s_camY = sector->floorHeight - FIXED(6);
			if (s_camY < sector->ceilingHeight) { s_camY = (sector->floorHeight + sector->ceilingHeight) >> 1; }
			s_camSector = sector;
			haveCam = true;
		}

		if (haveCam)
		{
			s_renderReady = true;
			debugf("[engine] camera ready sector=%ld camX=%ld camY=%ld camZ=%ld yaw=%ld\n",
				(long)(s_camSector ? s_camSector->id : -1), (long)s_camX, (long)s_camY, (long)s_camZ, (long)s_camYaw);
		}
		else
		{
			debugf("[engine] setupLevelRenderer: no camera\n");
		}
	}

	// Read the controller and walk/look around the level. Movement only commits when
	// the new position resolves to a real sector, so the camera can't leave the level.
	void updateCameraN64()
	{
		using namespace TFE_Jedi;
		if (!s_renderReady) { return; }

		const joypad_inputs_t  in   = joypad_get_inputs(JOYPAD_PORT_1);
		const joypad_buttons_t held = joypad_get_buttons_held(JOYPAD_PORT_1);

		// Analog stick with a small deadzone (neutral can drift on real sticks).
		s32 sx = in.stick_x; if (sx > -8 && sx < 8) { sx = 0; }
		s32 sy = in.stick_y; if (sy > -8 && sy < 8) { sy = 0; }

		// Look: stick X turns (yaw); C-Up/Down tilt (pitch, clamped to ~+/-60 degrees).
		s_camYaw = (s_camYaw + sx * 3) & ANGLE_MASK;
		if (held.c_up)   { s_camPitch += 96; }
		if (held.c_down) { s_camPitch -= 96; }
		if (s_camPitch >  2730) { s_camPitch =  2730; }
		if (s_camPitch < -2730) { s_camPitch = -2730; }

		// Move: stick Y walks forward/back along the facing; C-Left/Right strafe.
		fixed16_16 sinYaw, cosYaw;
		sinCosFixed(s_camYaw, &sinYaw, &cosYaw);

		fixed16_16 newX = s_camX;
		fixed16_16 newZ = s_camZ;
		const fixed16_16 fwd = sy * 384;
		newX += mul16(fwd, sinYaw);
		newZ += mul16(fwd, cosYaw);
		if (held.c_right) { newX += mul16(0xC000, cosYaw); newZ -= mul16(0xC000, sinYaw); }
		if (held.c_left)  { newX -= mul16(0xC000, cosYaw); newZ += mul16(0xC000, sinYaw); }

		// Commit only if inside a real sector; keep the eye 6 units above that sector's floor.
		RSector* next = sector_which3D(newX, s_camY, newZ);
		if (next)
		{
			s_camX = newX;
			s_camZ = newZ;
			s_camSector = next;
			s_camY = next->floorHeight - FIXED(6);
		}

		// Glue the AI chase target (the player object created by level load) to the camera
		// so enemies pursue the viewpoint. Relink sectors when we cross a boundary.
		SecObject* pobj = TFE_DarkForces::s_playerObject;
		if (pobj)
		{
			pobj->posWS.x = s_camX;
			pobj->posWS.y = s_camY + FIXED(6);   // camera is the eye; object origin is the feet
			pobj->posWS.z = s_camZ;
			if (s_camSector && pobj->sector != s_camSector)
			{
				sector_addObject(s_camSector, pobj);
			}
			TFE_DarkForces::s_eyePos.x = s_camX;
			TFE_DarkForces::s_eyePos.y = s_camY;
			TFE_DarkForces::s_eyePos.z = s_camZ;
		}
	}

	// Minimal non-AI object behavior pass.
	// Advances WAX sprite frames using original frameRate timing without pulling in
	// actor/task logic stacks. Tick cadence uses +5 per rendered frame (~150 ticks/s
	// at ~30fps), close to the DOS 145.5 tick basis used by time_frameRateToDelay().
	void updateObjectBehaviorN64()
	{
		if (!s_renderReady || !TFE_Jedi::s_levelState.sectors || !TFE_Jedi::s_levelState.sectorCount)
		{
			return;
		}

		s_behaviorFrame++;
		s_behaviorTick += 5;
		TFE_DarkForces::s_curTick = s_behaviorTick;

		u32 objectCount = 0;
		u32 animatedSpriteCount = 0;

		RSector* sector = TFE_Jedi::s_levelState.sectors;
		for (u32 s = 0; s < TFE_Jedi::s_levelState.sectorCount; s++, sector++)
		{
			if (!sector->objectList || sector->objectCount <= 0 || sector->objectCapacity <= 0)
			{
				continue;
			}

			for (s32 i = 0; i < sector->objectCapacity; i++)
			{
				SecObject* obj = sector->objectList[i];
				if (!obj) { continue; }
				objectCount++;

				if (obj->type != OBJ_TYPE_SPRITE || !obj->wax) { continue; }

				WaxAnim* anim = WAX_AnimPtr(obj->wax, obj->anim & 0x1f);
				if (!anim || anim->frameCount <= 1 || anim->frameRate <= 0) { continue; }

				u32 delay = TFE_DarkForces::time_frameRateToDelay((u32)anim->frameRate);
				if (!delay) { delay = 1; }

				SpriteAnimState* state = findSpriteAnimState(obj);
				if (!state)
				{
					s_spriteAnimState.push_back({ obj, s_behaviorTick + delay, delay, s_behaviorFrame });
					state = &s_spriteAnimState.back();
				}
				else
				{
					state->seenFrame = s_behaviorFrame;
					if (state->delay != delay)
					{
						state->delay = delay;
						if (state->nextTick <= s_behaviorTick)
						{
							state->nextTick = s_behaviorTick + delay;
						}
					}
				}

				if (s_behaviorTick >= state->nextTick)
				{
					const s32 frameCount = anim->frameCount > 0 ? anim->frameCount : 1;
					s32 nextFrame = obj->frame + 1;
					if (nextFrame >= frameCount || nextFrame < 0)
					{
						nextFrame = 0;
					}
					obj->frame = nextFrame;

					do { state->nextTick += delay; } while (state->nextTick <= s_behaviorTick);
				}

				animatedSpriteCount++;
			}
		}

		if (!s_spriteAnimState.empty())
		{
			size_t writeIndex = 0;
			for (size_t i = 0; i < s_spriteAnimState.size(); i++)
			{
				if (s_spriteAnimState[i].seenFrame == s_behaviorFrame)
				{
					if (writeIndex != i)
					{
						s_spriteAnimState[writeIndex] = s_spriteAnimState[i];
					}
					writeIndex++;
				}
			}
			s_spriteAnimState.resize(writeIndex);
		}

		if (!s_behaviorLogged)
		{
			debugf("[engine] behavior active: objects=%lu animatedSprites=%lu\n", (unsigned long)objectCount, (unsigned long)animatedSpriteCount);
			s_behaviorLogged = true;
		}
	}

	void renderLevelFrame(u8* display)
	{
		if (!s_renderReady) { return; }
		memset(display, 0, 320 * 200);
		TFE_Jedi::renderer_computeCameraTransform(s_camSector, s_camPitch, s_camYaw, s_camX, s_camY, s_camZ);
		TFE_Jedi::drawWorld(display, s_camSector, s_colorMapData, s_lightRamp);
	}
}
