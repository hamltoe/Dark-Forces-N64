//////////////////////////////////////////////////////////////////////
// N64-only engine shims.
//
// Provides host/game symbols referenced by the linked fixed-point render
// path whose real implementations pull non-portable deps (SDL/ImGui/audio/
// script) or aren't brought up yet. Most are no-ops/null; a few have real
// minimal logic. Grown iteratively as the engine link is brought up.
//////////////////////////////////////////////////////////////////////
#include <libdragon.h>

// libdragon's hardware counter rate (COP0 count register = CPU clock / 2 =
// 46.875 MHz). Captured here because a TFE header included below
// (TFE_DarkForces/time.h) #defines TICKS_PER_SECOND to 145 (Dark Forces game
// ticks), which would otherwise shadow libdragon's hardware macro in the
// wall-clock helpers and make game time run several times too fast.
static const double kN64HwTicksPerSecond = (double)TICKS_PER_SECOND;

#include <TFE_System/types.h>
#include <TFE_System/system.h>
#include <TFE_System/tfeMessage.h>               // TFE_Message, getMessage (INF level text)
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
#include <TFE_Jedi/InfSystem/infSystem.h>         // inf_load + elevator/teleport/trigger tasks
#include <TFE_Jedi/InfSystem/infState.h>          // inf_clearState
#include <TFE_Jedi/Task/task.h>                   // Task, TaskFunc, ctx*, itask_yield, createSubTask
#include <TFE_Jedi/Renderer/jediRenderer.h>      // render_setResolution, drawWorld, camera transform
#include <TFE_Jedi/Level/levelData.h>            // s_levelState (sectors, palette name)
#include <TFE_Jedi/Level/robjData.h>             // ETFLAG_PLAYER (object SEQ parse)
#include <TFE_System/parser.h>                    // TFE_Parser::readLine (object SEQ parse)
#include <TFE_RenderBackend/renderBackend.h>     // setPalette
#include <TFE_FileSystem/filestream.h>           // FileStream (level palette load)
#include <TFE_Jedi/Level/rwall.h>                 // RWall (inf wall stubs)
#include <TFE_Jedi/Collision/collision.h>         // collision_moveObj (player wall collision)

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
#include <TFE_DarkForces/logic.h>                // Logic, LOGIC_PLAYER, obj_addLogic (player damage routing)
#include <TFE_DarkForces/playerLogic.h>          // PlayerLogic (step-height-aware player movement)
#include <TFE_DarkForces/playerCollision.h>      // handlePlayerCollision / playerMove (real DF physics)
#include <TFE_DarkForces/Actor/actor.h>          // actor_createTask / clearState / physics list
#include <TFE_DarkForces/time.h>                  // updateTime (drives s_curTick + s_frameTicks)
#include <TFE_Jedi/Level/rsector.h>               // sector_addObject (player-object relink)
#include <TFE_Jedi/Level/robject.h>               // allocateObject (item-drop stub)
#include <TFE_Jedi/Level/rtexture.h>              // TextureData, bitmap_load (HUD status panels)
#include <TFE_Jedi/Level/rfont.h>                 // Font, font_load (HUD number fonts)
#include <TFE_ExternalData/dfLogics.h>
#include <TFE_ExternalData/weaponExternal.h>

#include <cstring>
#include <cstddef>
#include <cstdlib>
#include <cstdio>
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
		// Hardware counter runs at 46.875 MHz (see kN64HwTicksPerSecond).
		return (f64)get_ticks() / kN64HwTicksPerSecond;
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
		f64 dt = (f64)delta / kN64HwTicksPerSecond;
		// Clamp to a sane range so a long first frame / stall can't explode movement.
		if (dt <= 0.0)   { dt = 1.0 / 60.0; }
		if (dt > 0.1)    { dt = 0.1; }
		return dt;
	}

	// Localized UI strings aren't shipped on N64; the INF system asks for level-message
	// text only for HUD popups (which are no-ops here), so return an empty string.
	const char* getMessage(TFE_Message) { return ""; }
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

	// Referenced by the INF system (elevators/triggers) for camera + night-vision checks.
	// player.cpp isn't compiled, so we own these here.
	angle14_32 s_playerYaw         = 0;
	JBool      s_playerSecMoved    = JFALSE;
	JBool      s_nightVisionActive = JFALSE;
	JBool      s_externalCameraMode = JFALSE;

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
	// Player health/inventory. HUD/weapon/pickup systems aren't linked on N64, so we
	// own these globals here (player.cpp is not compiled). Initialized in startActorSystem.
	PlayerInfo s_playerInfo   = {};
	s32        s_invincibility = 0;
	s32        s_healthMax     = 100;
	s32        s_shieldsMax    = 200;

	// HUD energy + lives. These live in player.cpp on desktop; that TU isn't compiled on
	// N64, so we own them here. Only the HUD reads them right now (headlamp/pickups that
	// would change them aren't linked yet), but they hold the real DF starting values.
	fixed16_16 s_batteryPower    = 2 * ONE_16;   // DF default battery = full (2.0)
	fixed16_16 s_batteryPowerMax = 2 * ONE_16;
	s32        s_lifeCount        = 3;

	// Apply damage to the player, faithful to the DOS shield-first model: shields take
	// half-damage twice; once shields drop below 50 the overflow bleeds into health.
	// Reaching zero health flags the player dying. Sound/screen-fx paths are omitted
	// (those subsystems aren't linked yet).
	void player_applyDamage(fixed16_16 healthDmg, fixed16_16 shieldDmg, JBool /*playHitSound*/)
	{
		using namespace TFE_Jedi;
		fixed16_16 shields = intToFixed16(s_playerInfo.shields);
		fixed16_16 health  = intToFixed16(s_playerInfo.health) + s_playerInfo.healthFract;

		if (!s_invincibility && health && shieldDmg > 0)
		{
			fixed16_16 halfShieldDmg = shieldDmg >> 1;
			shields -= halfShieldDmg; if (shields < 0) { shields = 0; }
			if (shields < FIXED(50))
			{
				// healthDmg += shieldDmg * (1 - shields/50)
				fixed16_16 fracDmgToHealth = ONE_16 - div16(shields, FIXED(50));
				healthDmg += mul16(fracDmgToHealth, shieldDmg);
			}
			shields -= halfShieldDmg; if (shields < 0) { shields = 0; }
			s32 newShields = floor16(shields);
			if (newShields > s_shieldsMax) { newShields = s_shieldsMax; }
			s_playerInfo.shields = newShields;
		}

		if (!s_invincibility && healthDmg > 0 && health)
		{
			health -= healthDmg;
			if (health < ONE_16)
			{
				s_playerInfo.healthFract = 0;
				s_playerInfo.health = 0;
				s_playerDying = JTRUE;
				s_reviveTick  = s_curTick + 436;
			}
			else
			{
				s_playerInfo.health      = floor16(health);
				s_playerInfo.healthFract = fract16(health);
			}
		}

		debugf("[player] hit -> health=%ld shields=%ld%s\n",
			(long)s_playerInfo.health, (long)s_playerInfo.shields,
			s_playerDying ? " DYING" : "");
	}

	// Minimal player task + logic so projectile hits deliver MSG_DAMAGE to the player.
	// message_sendToObj() dispatches to the object's logic task via task_runLocal(), so
	// the player object needs a logic parented to a task that owns a local message func.
	// PlayerLogic (not bare Logic) so the step-height-aware mover (handlePlayerCollision /
	// playerMove) has its move/dir/stepHeight fields. Logic is its first member, so
	// (Logic*)&s_n64PlayerLogic is a valid Logic* for obj_addLogic().
	static Task*       s_n64PlayerTask  = nullptr;
	static PlayerLogic s_n64PlayerLogic = {};

	// playerMove() pokes the automap layer on sector crossings; the automap module is not
	// linked on N64, so satisfy the reference with a no-op.
	void automap_setLayer(s32) {}

	// INF sector messages can clear the night-vision effect; we have no night-vision
	// post-process on N64, so this is a no-op.
	void disableNightVision() {}

	void n64PlayerTaskFunc(MessageType msg)
	{
		task_begin;
		while (msg != MSG_FREE_TASK) { task_yield(TASK_NO_DELAY); }
		task_end;
	}
	void n64PlayerCleanupFunc(Logic*) {}

	// Mirrors playerControlMsgFunc: projectile damage is applied as shield damage.
	void n64PlayerMsgFunc(MessageType msg)
	{
		using namespace TFE_Jedi;
		if (msg == MSG_DAMAGE)
		{
			ProjectileLogic* proj = (ProjectileLogic*)s_msgEntity;
			if (proj) { player_applyDamage(0, proj->dmg, JTRUE); }
		}
		else if (msg == MSG_EXPLOSION)
		{
			player_applyDamage(0, (fixed16_16)s_msgArg1, JTRUE);
		}
	}

	void player_setupObject(SecObject* obj)
	{
		using namespace TFE_Jedi;
		s_playerObject = obj;
		if (!obj) { return; }

		// Real player capsule so enemy projectiles register a hit on us.
		obj->entityFlags |= ETFLAG_PLAYER;
		obj->worldHeight  = 0x5cccc;   // PLAYER_HEIGHT (5.8 units)
		obj->worldWidth   = 0x1cccc;   // PLAYER_WIDTH  (1.8 units)

		// Step-height-aware mover state: limit step-up to PLAYER_STEP (3.5 units) so the
		// player can't climb onto platforms it shouldn't reach.
		s_n64PlayerLogic.stepHeight = 0x38000;   // PLAYER_STEP (3.5 units)
		s_n64PlayerLogic.move.x = 0;
		s_n64PlayerLogic.move.y = 0;
		s_n64PlayerLogic.move.z = 0;

		// Create the damage-routing task once, then attach a player logic to the object.
		if (!s_n64PlayerTask)
		{
			s_n64PlayerTask = createTask("n64player", n64PlayerTaskFunc, JFALSE, n64PlayerMsgFunc);
		}
		obj_addLogic(obj, (Logic*)&s_n64PlayerLogic, LOGIC_PLAYER, s_n64PlayerTask, n64PlayerCleanupFunc);
	}
	void player_setupEyeObject(SecObject*)   {}
	void player_getVelocity(TFE_Jedi::vec3_fixed* vel) { if (vel) { vel->x = 0; vel->y = 0; vel->z = 0; } }
	fixed16_16 player_getSquaredDistance(SecObject* obj)
	{
		if (!obj || !s_playerObject) { return FIXED(32767); }
		fixed16_16 dx = obj->posWS.x - s_playerObject->posWS.x;
		fixed16_16 dy = obj->posWS.y - s_playerObject->posWS.y;
		fixed16_16 dz = obj->posWS.z - s_playerObject->posWS.z;
		return mul16(dx, dx) + mul16(dy, dy) + mul16(dz, dz);
	}

	// ---- Sound ----
	// The DF sound_* API is implemented for real (libdragon RSP mixer) further down in this
	// file, after TFE_Paths::getFilePath is available for loading VOCs from SOUNDS.GOB.

	// ---- HUD text + agent/level-end (HUD text + mission flow not linked on N64) ----
	void hud_sendTextMessage(s32) {}
	void hud_sendTextMessage(const char*, s32, bool) {}
	void agent_levelComplete() {}
	void agent_createLevelEndTask() {}

	// ---- Hit effects / explosions (no particle/effect system yet) ----
	void spawnHitEffect(HitEffectID, RSector*, TFE_Jedi::vec3_fixed, SecObject*) {}
	void computeExplosionPushDir(TFE_Jedi::vec3_fixed*, TFE_Jedi::vec3_fixed* pushDir)   { if (pushDir) { pushDir->x = 0; pushDir->y = 0; pushDir->z = 0; } }
	void computeDamagePushVelocity(ProjectileLogic*, TFE_Jedi::vec3_fixed* vel)          { if (vel) { vel->x = 0; vel->y = 0; vel->z = 0; } }

	// ---- Pickups / items (inventory system not linked) ----
	ItemId     getPickupItemId(const char*)         { return (ItemId)0; }
	// The item/pickup system isn't linked yet, but enemy death-drops (actor.cpp,
	// mousebot.cpp, phaseTwo.cpp) dereference this result (item->posWS = ...). Return a
	// real, invisible OBJ_TYPE_SPIRIT object instead of null so those drops don't NULL-crash.
	// The drop has no pickup logic/visual until items are brought up; the caller positions it.
	SecObject* item_create(ItemId)
	{
		SecObject* obj = TFE_Jedi::allocateObject();
		if (obj) { obj->worldWidth = 0; obj->worldHeight = 0; }
		return obj;
	}
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
	bool solidWallFlagFix() { return true; }    // enforce solid-wall flag for moving-wall collision (DF default)
	bool ignoreInfLimits()  { return true; }    // no INF element-count limit on N64
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
		static GobArchive* s_archives[4] =
		{
			openStagedGob("rom:/DARK.GOB"),
			openStagedGob("rom:/TEXTURES.GOB"),
			openStagedGob("rom:/SPRITES.GOB"),
			openStagedGob("rom:/SOUNDS.GOB"),
		};

		path->archive = nullptr;
		path->index   = INVALID_FILE;
		strncpy(path->path, fileName, TFE_MAX_PATH - 1);
		path->path[TFE_MAX_PATH - 1] = 0;

		for (s32 i = 0; i < 4; i++)
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
// TFE_DarkForces sound: real SFX through the libdragon RSP mixer.
//
// The engine's own sound.cpp (iMUSE ImStartSfx + the TFE_Audio RtAudio/SDL backend)
// isn't portable to the N64, so the DF sound_* API is implemented here on top of
// libdragon's mixer. sound_load decodes a Dark Forces VOC (8-bit *unsigned* PCM,
// ~11 kHz mono, from SOUNDS.GOB) into a signed-8-bit waveform_t; sound_play /
// sound_playCued grab one of a small pool of mixer channels; cued (positional)
// sounds attenuate + pan by distance from the listener (the player camera, which
// soundUpdateN64 refreshes each frame).
// ---------------------------------------------------------------------
namespace TFE_DarkForces
{
	struct N64Sound
	{
		char       name[16];
		waveform_t wave;        // libdragon mixer waveform (read = n64SoundRead)
		s8*        pcm;         // signed 8-bit mono samples (+MIXER_LOOP_OVERREAD tail)
		s32        numSamples;
		s32        sampleRate;
		s32        loopStart;   // -1 = one-shot
		s32        priority;    // DF SoundPriority (higher wins a channel steal)
		s32        baseVolume;  // 0..127
		s32        refCount;
	};

	static const s32 N64_MAX_SOUNDS   = 128;
	static const s32 N64_SFX_CHANNELS = 16;   // mixer channels 0..15 reserved for SFX
	static const s32 N64_SND_BUDGET   = 256 * 1024;  // cap total decoded PCM (stock 4 MB RDRAM)

	static N64Sound s_n64Sounds[N64_MAX_SOUNDS];
	static s32      s_n64SoundCount = 0;
	static s32      s_n64SoundBytes = 0;   // running total of decoded PCM bytes

	struct N64SndChannel { N64Sound* snd; u32 tag; s32 priority; s32 vol; float pan; };
	static N64SndChannel s_n64Chan[N64_SFX_CHANNELS] = {};
	static u32  s_n64SndTag     = 1;
	static bool s_n64AudioReady = false;

	// Listener (player) position + facing, refreshed each frame by soundUpdateN64.
	static fixed16_16 s_sndListenerX = 0, s_sndListenerZ = 0;
	static angle14_32 s_sndListenerYaw = 0;

	static bool n64NameEq(const char* a, const char* b)
	{
		for (; *a && *b; a++, b++)
		{
			char ca = *a, cb = *b;
			if (ca >= 'A' && ca <= 'Z') { ca = (char)(ca + 32); }
			if (cb >= 'A' && cb <= 'Z') { cb = (char)(cb + 32); }
			if (ca != cb) { return false; }
		}
		return *a == *b;
	}

	// Mixer sample-generator callback: copy in-memory PCM into the mixer's sample buffer
	// (the wav64 "none/memcopy" pattern). ctx is the owning N64Sound.
	static void n64SoundRead(void* ctx, samplebuffer_t* sbuf, int wpos, int wlen, bool /*seeking*/)
	{
		N64Sound* snd = (N64Sound*)ctx;
		s8* dst = (s8*)samplebuffer_append(sbuf, wlen);
		if (dst && snd && snd->pcm) { memcpy(dst, snd->pcm + wpos, (size_t)wlen); }
	}

	// Decode a Dark Forces VOC into a malloc'd signed-8-bit mono buffer. Returns false if the
	// file is missing or has no 8-bit sound data.
	static bool n64DecodeVoc(const char* name, s8** outPcm, s32* outLen, s32* outRate, s32* outLoop)
	{
		FilePath fp;
		if (!TFE_Paths::getFilePath(name, &fp)) { return false; }
		FileStream file;
		if (!file.open(&fp, Stream::MODE_READ)) { return false; }
		const size_t fileSize = (size_t)file.getSize();
		if (fileSize < 0x1a) { file.close(); return false; }
		u8* raw = (u8*)malloc(fileSize);
		if (!raw) { file.close(); return false; }
		file.readBuffer(raw, (u32)fileSize);
		file.close();

		// VOC header: 20-byte desc then datablockOffset (u16 LE) @ 0x14.
		const u32 dataStart = (u32)raw[0x14] | ((u32)raw[0x15] << 8);
		// Sound-block PCM never exceeds the file size; +8 KB slack covers any silence blocks
		// (DF SFX rarely use them). Writes are clamped to cap so a stray block can't overrun.
		const s32 cap = (s32)fileSize + 8192;
		s8* pcm = (s8*)malloc((size_t)cap + MIXER_LOOP_OVERREAD);
		if (!pcm) { free(raw); return false; }
		s32 len = 0, rate = 11025, loopStart = -1;

		u32 p = dataStart;
		while (p + 4u <= (u32)fileSize)
		{
			const u8 type = raw[p]; p++;
			if (type == 0 || type > 7) { break; }                 // terminator / unknown
			const u32 blockLen = (u32)raw[p] | ((u32)raw[p + 1] << 8) | ((u32)raw[p + 2] << 16);
			p += 3;
			if ((size_t)p + blockLen > fileSize) { break; }
			switch (type)
			{
				case 1: // SOUND_DATA: [srByte][codec][pcm...]
				{
					rate = 1000000 / (256 - (s32)raw[p]);
					if (rate == 10989) { rate = 11025; }
					const u8* src = raw + p + 2;
					s32 n = (s32)blockLen - 2;
					if (len + n > cap) { n = cap - len; }
					for (s32 i = 0; i < n; i++) { pcm[len + i] = (s8)(src[i] ^ 0x80); }
					if (n > 0) { len += n; }
				} break;
				case 2: // SOUND_CONTINUE: raw PCM continues
				{
					const u8* src = raw + p;
					s32 n = (s32)blockLen;
					if (len + n > cap) { n = cap - len; }
					for (s32 i = 0; i < n; i++) { pcm[len + i] = (s8)(src[i] ^ 0x80); }
					if (n > 0) { len += n; }
				} break;
				case 3: // SILENCE: [len u16][srByte]
				{
					s32 n = (s32)((u32)raw[p] | ((u32)raw[p + 1] << 8));
					if (len + n > cap) { n = cap - len; }
					for (s32 i = 0; i < n; i++) { pcm[len + i] = 0; }   // signed silence = 0
					if (n > 0) { len += n; }
				} break;
				case 6: loopStart = len; break;                          // REPEAT start
				default: break;                                          // MARKER/ASCII/END ignored
			}
			p += blockLen;
		}
		free(raw);
		if (len <= 0) { free(pcm); return false; }
		// Pad the mixer loop-overread tail so the RSP never reads past the buffer.
		for (s32 i = 0; i < MIXER_LOOP_OVERREAD; i++)
		{
			pcm[len + i] = (loopStart >= 0 && len > loopStart) ? pcm[loopStart + (i % (len - loopStart))] : 0;
		}
		// Shrink from the generous upper-bound cap down to the exact size to reclaim RDRAM.
		s8* shrunk = (s8*)realloc(pcm, (size_t)len + MIXER_LOOP_OVERREAD);
		if (shrunk) { pcm = shrunk; }
		*outPcm = pcm; *outLen = len; *outRate = rate; *outLoop = loopStart;
		return true;
	}

	SoundSourceId sound_load(const char* fileName, u32 priority)
	{
		if (!s_n64AudioReady || !fileName || !fileName[0]) { return NULL_SOUND; }
		for (s32 i = 0; i < s_n64SoundCount; i++)
		{
			if (s_n64Sounds[i].pcm && n64NameEq(fileName, s_n64Sounds[i].name))
			{
				s_n64Sounds[i].refCount++;
				return (SoundSourceId)&s_n64Sounds[i];
			}
		}
		if (s_n64SoundCount >= N64_MAX_SOUNDS) { return NULL_SOUND; }

		s8* pcm; s32 len, rate, loopStart;
		if (!n64DecodeVoc(fileName, &pcm, &len, &rate, &loopStart)) { return NULL_SOUND; }

		// Stay inside the sound-memory budget: drop the sound if it would overflow (the most
		// important sounds -- weapons, then level enemies -- load first, so they win the space).
		if (s_n64SoundBytes + len > N64_SND_BUDGET) { free(pcm); return NULL_SOUND; }
		s_n64SoundBytes += len;

		N64Sound* snd = &s_n64Sounds[s_n64SoundCount++];
		memset(snd, 0, sizeof(*snd));
		strncpy(snd->name, fileName, 15);
		snd->pcm        = pcm;
		snd->numSamples = len;
		snd->sampleRate = rate;
		snd->loopStart  = loopStart;
		snd->priority   = (s32)priority;
		snd->baseVolume = 127;
		snd->refCount   = 1;
		snd->wave.name      = snd->name;
		snd->wave.bits      = 8;
		snd->wave.channels  = 1;
		snd->wave.frequency = (float)rate;
		snd->wave.len       = len;
		snd->wave.loop_len  = (loopStart >= 0) ? (len - loopStart) : 0;
		snd->wave.read      = n64SoundRead;
		snd->wave.ctx       = snd;
		return (SoundSourceId)snd;
	}

	// DF volume (0..127) + pan (-1..1) -> left/right channel gains (center = full volume).
	static void n64ApplyVolPan(s32 ch, s32 volume, float pan)
	{
		float v = (float)volume * (1.0f / 127.0f);
		if (v < 0) { v = 0; } else if (v > 1) { v = 1; }
		if (pan < -1.0f) { pan = -1.0f; } else if (pan > 1.0f) { pan = 1.0f; }
		const float l = v * ((pan > 0) ? (1.0f - pan) : 1.0f);
		const float r = v * ((pan < 0) ? (1.0f + pan) : 1.0f);
		mixer_ch_set_vol(ch, l, r);
	}

	// Grab a channel: a finished one if possible, else steal the lowest priority.
	static s32 n64AllocChannel(s32 priority)
	{
		for (s32 i = 0; i < N64_SFX_CHANNELS; i++)
		{
			if (!s_n64Chan[i].snd || !mixer_ch_playing(i)) { return i; }
		}
		s32 best = -1, bestPri = priority;
		for (s32 i = 0; i < N64_SFX_CHANNELS; i++)
		{
			if (s_n64Chan[i].priority < bestPri) { bestPri = s_n64Chan[i].priority; best = i; }
		}
		return best;
	}

	static SoundEffectId n64PlayInternal(N64Sound* snd, s32 volume, float pan)
	{
		if (!s_n64AudioReady || !snd || !snd->pcm) { return 0; }
		const s32 ch = n64AllocChannel(snd->priority);
		if (ch < 0) { return 0; }
		mixer_ch_play(ch, &snd->wave);
		n64ApplyVolPan(ch, volume, pan);
		s_n64Chan[ch].snd      = snd;
		s_n64Chan[ch].priority = snd->priority;
		s_n64Chan[ch].tag      = s_n64SndTag++;
		s_n64Chan[ch].vol      = volume;
		s_n64Chan[ch].pan      = pan;
		return ((SoundEffectId)s_n64Chan[ch].tag << 8) | (SoundEffectId)(ch + 1);
	}

	// Decode a SoundEffectId back to its channel, validating the generation tag so a stale id
	// can't touch a channel that has since been reused (or finished).
	static s32 n64ChannelOf(SoundEffectId id)
	{
		if (!id) { return -1; }
		const s32 ch = (s32)(id & 0xff) - 1;
		if (ch < 0 || ch >= N64_SFX_CHANNELS) { return -1; }
		if (s_n64Chan[ch].tag != (u32)(id >> 8) || !s_n64Chan[ch].snd) { return -1; }
		if (!mixer_ch_playing(ch)) { return -1; }
		return ch;
	}

	// Positional cue: volume + pan from the sound's world position vs the listener.
	static void n64ComputeCue(vec3_fixed pos, s32 baseVol, s32* outVol, float* outPan)
	{
		const fixed16_16 dx   = pos.x - s_sndListenerX;
		const fixed16_16 dz   = pos.z - s_sndListenerZ;
		const fixed16_16 dist = vec2Length(dx, dz);
		const s32 distU   = dist >> 16;                 // world units
		const s32 maxDist = 150;
		s32 vol = baseVol - (baseVol * distU) / maxDist;
		if (vol < 0) { vol = 0; }
		*outVol = vol;

		float pan = 0.0f;
		if (dist > 0)
		{
			fixed16_16 sinYaw, cosYaw;
			sinCosFixed(s_sndListenerYaw, &sinYaw, &cosYaw);
			// Listener right vector = (cosYaw, -sinYaw); project the offset onto it.
			const fixed16_16 rightDot = mul16(dx, cosYaw) - mul16(dz, sinYaw);
			pan = (float)rightDot / (float)dist;
		}
		*outPan = pan;
	}

	SoundEffectId sound_play(SoundSourceId id)
	{
		if (!id) { return 0; }
		N64Sound* snd = (N64Sound*)id;
		return n64PlayInternal(snd, snd->baseVolume, 0.0f);
	}

	SoundEffectId sound_playCued(SoundSourceId id, vec3_fixed pos)
	{
		if (!id) { return 0; }
		N64Sound* snd = (N64Sound*)id;
		s32 vol; float pan;
		n64ComputeCue(pos, snd->baseVolume, &vol, &pan);
		return n64PlayInternal(snd, vol, pan);
	}

	SoundEffectId sound_maintain(SoundEffectId idInstance, SoundSourceId idSound, vec3_fixed pos)
	{
		const s32 ch = n64ChannelOf(idInstance);
		if (ch >= 0)
		{
			s32 vol; float pan;
			n64ComputeCue(pos, s_n64Chan[ch].snd->baseVolume, &vol, &pan);
			n64ApplyVolPan(ch, vol, pan);
			s_n64Chan[ch].vol = vol; s_n64Chan[ch].pan = pan;
			return idInstance;
		}
		return sound_playCued(idSound, pos);
	}

	void sound_adjustCued(SoundEffectId id, vec3_fixed pos)
	{
		const s32 ch = n64ChannelOf(id);
		if (ch < 0) { return; }
		s32 vol; float pan;
		n64ComputeCue(pos, s_n64Chan[ch].snd->baseVolume, &vol, &pan);
		n64ApplyVolPan(ch, vol, pan);
		s_n64Chan[ch].vol = vol; s_n64Chan[ch].pan = pan;
	}

	void sound_stop(SoundEffectId id)
	{
		const s32 ch = n64ChannelOf(id);
		if (ch < 0) { return; }
		mixer_ch_stop(ch);
		s_n64Chan[ch].snd = nullptr;
	}

	void sound_setVolume(SoundEffectId id, s32 volume)
	{
		const s32 ch = n64ChannelOf(id);
		if (ch < 0) { return; }
		s_n64Chan[ch].vol = volume & 0x7f;
		n64ApplyVolPan(ch, s_n64Chan[ch].vol, s_n64Chan[ch].pan);
	}

	void sound_setBaseVolume(SoundSourceId id, s32 volume)
	{
		if (!id) { return; }
		((N64Sound*)id)->baseVolume = volume & 0x7f;
	}
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
		using namespace TFE_DarkForces;
		// projectile_startup() + createProjectile() read this table for each type's visual
		// (assetType/asset), motion (speed/duration/updateFunc) and combat (damage/force)
		// data. On desktop it comes from projectiles.json; here we populate it once with the
		// shipped values. Without it every projectile is a motionless, invisible, harmless
		// "spirit" (the struct default) — so this is what makes enemy AND player shots real.
		static ExternalProjectile s_projectiles[PROJ_COUNT];
		static bool s_init = false;
		if (s_init) { return s_projectiles; }
		s_init = true;

		// Field units mirror the JSON loader: damage/minDamage/speed/*Tick/bounceCount/
		// reflectVariation are raw ints; force/falloffAmount/*Bounciness are value*ONE_16;
		// duration is seconds*145.65 (game ticks). Fixed helper: X * 65536.
		#define FX(v)   ((u32)((v) * 65536.0))
		#define TICKS(s) ((u32)((s) * 145.65))

		// PUNCH (fist melee): invisible spirit, short range, no lifetime.
		{
			ExternalProjectile& p = s_projectiles[PROJ_PUNCH];
			p.assetType = "spirit"; p.updateFunc = "standard";
			p.damage = 6; p.speed = 230; p.duration = 0; p.force = FX(0.02);
		}
		// PISTOL_BOLT (player Bryar + enemy officers): red 3DO bolt.
		{
			ExternalProjectile& p = s_projectiles[PROJ_PISTOL_BOLT];
			p.assetType = "3d"; p.asset = "wrbolt.3do"; p.fullBright = true; p.zeroWidth = true;
			p.updateFunc = "standard"; p.damage = 10; p.minDamage = 1;
			p.falloffAmount = FX(1.0); p.nextFalloffTick = 14; p.damageFalloffDelta = 14;
			p.force = FX(0.01); p.speed = 250; p.horzBounciness = FX(1.0); p.vertBounciness = FX(1.0);
			p.bounceCount = 3; p.reflectVariation = 9; p.duration = TICKS(3.0);
		}
		// RIFLE_BOLT (player E-11 + enemy stormtroopers): red 3DO bolt.
		{
			ExternalProjectile& p = s_projectiles[PROJ_RIFLE_BOLT];
			p.assetType = "3d"; p.asset = "wrbolt.3do"; p.fullBright = true; p.zeroWidth = true;
			p.updateFunc = "standard"; p.damage = 10; p.minDamage = 1;
			p.falloffAmount = FX(0.8); p.nextFalloffTick = 14; p.damageFalloffDelta = 14;
			p.force = FX(0.01); p.speed = 250; p.horzBounciness = FX(1.0); p.vertBounciness = FX(1.0);
			p.bounceCount = 3; p.reflectVariation = 9; p.duration = TICKS(4.0);
		}
		// THERMAL_DET: thrown/arcing detonator frame; explodes on timeout.
		{
			ExternalProjectile& p = s_projectiles[PROJ_THERMAL_DET];
			p.assetType = "frame"; p.asset = "wdet.fme"; p.zeroWidth = true; p.movable = true;
			p.updateFunc = "arcing"; p.minDamage = 1; p.force = FX(0.1); p.speed = 80;
			p.horzBounciness = FX(0.45); p.vertBounciness = FX(0.89); p.bounceCount = -1;
			p.duration = TICKS(3.0); p.explodeOnTimeout = true;
		}
		// REPEATER: yellow bullet frame (visible sprite).
		{
			ExternalProjectile& p = s_projectiles[PROJ_REPEATER];
			p.assetType = "frame"; p.asset = "bullet.fme"; p.fullBright = true; p.zeroWidth = true;
			p.updateFunc = "standard"; p.damage = 10; p.minDamage = 1;
			p.falloffAmount = FX(0.3); p.nextFalloffTick = 19660; p.damageFalloffDelta = 19660;
			p.force = FX(0.03); p.speed = 270; p.bounceCount = 3; p.reflectVariation = 9;
			p.duration = TICKS(4.0);
		}
		// PLASMA (Fusion Cutter): green plasma WAX (visible sprite).
		{
			ExternalProjectile& p = s_projectiles[PROJ_PLASMA];
			p.assetType = "sprite"; p.asset = "wemiss.wax"; p.fullBright = true; p.zeroWidth = true;
			p.updateFunc = "standard"; p.damage = 15; p.minDamage = 1;
			p.falloffAmount = FX(0.6); p.nextFalloffTick = 29; p.damageFalloffDelta = 29;
			p.force = FX(0.05); p.speed = 100; p.horzBounciness = FX(0.9); p.vertBounciness = FX(0.9);
			p.bounceCount = 3; p.reflectVariation = 9; p.duration = TICKS(10.0);
		}
		// MORTAR: arcing shell WAX.
		{
			ExternalProjectile& p = s_projectiles[PROJ_MORTAR];
			p.assetType = "sprite"; p.asset = "wshell.wax"; p.zeroWidth = true;
			p.updateFunc = "arcing"; p.force = FX(0.2); p.speed = 110;
			p.horzBounciness = FX(0.4); p.vertBounciness = FX(0.6); p.duration = TICKS(4.0);
		}
		// CONCUSSION: invisible shockwave spirit.
		{
			ExternalProjectile& p = s_projectiles[PROJ_CONCUSSION];
			p.assetType = "spirit"; p.zeroWidth = true; p.updateFunc = "standard";
			p.force = FX(0.06); p.speed = 190; p.horzBounciness = FX(0.9); p.vertBounciness = FX(0.9);
			p.duration = TICKS(5.0);
		}
		// CANNON (Assault Cannon primary): blue plasma WAX (visible sprite).
		{
			ExternalProjectile& p = s_projectiles[PROJ_CANNON];
			p.assetType = "sprite"; p.asset = "wplasma.wax"; p.fullBright = true; p.zeroWidth = true;
			p.updateFunc = "standard"; p.damage = 30; p.force = FX(0.061523438); p.speed = 100;
			p.horzBounciness = FX(0.9); p.vertBounciness = FX(0.9); p.bounceCount = 3;
			p.reflectVariation = 18; p.duration = TICKS(4.0);
		}
		// MISSILE (Assault Cannon secondary): missile WAX.
		{
			ExternalProjectile& p = s_projectiles[PROJ_MISSILE];
			p.assetType = "sprite"; p.asset = "wmsl.wax"; p.zeroWidth = true;
			p.updateFunc = "standard"; p.force = FX(1.0); p.speed = 74;
			p.horzBounciness = FX(0.9); p.vertBounciness = FX(0.9); p.duration = TICKS(7.0);
		}
		// TURRET_BOLT (wall turrets): green 3DO bolt.
		{
			ExternalProjectile& p = s_projectiles[PROJ_TURRET_BOLT];
			p.assetType = "3d"; p.asset = "wgbolt.3do"; p.fullBright = true; p.zeroWidth = true;
			p.updateFunc = "standard"; p.damage = 17; p.minDamage = 1;
			p.falloffAmount = FX(1.0); p.nextFalloffTick = 14; p.damageFalloffDelta = 14;
			p.force = FX(0.03); p.speed = 300; p.horzBounciness = FX(1.0); p.vertBounciness = FX(1.0);
			p.bounceCount = 3; p.reflectVariation = 9; p.duration = TICKS(3.0);
		}
		// REMOTE_BOLT (interrogation/remote droids): green 3DO bolt.
		{
			ExternalProjectile& p = s_projectiles[PROJ_REMOTE_BOLT];
			p.assetType = "3d"; p.asset = "wgbolt.3do"; p.fullBright = true; p.zeroWidth = true;
			p.updateFunc = "standard"; p.damage = 6; p.minDamage = 1;
			p.falloffAmount = FX(1.0); p.nextFalloffTick = 14; p.damageFalloffDelta = 14;
			p.force = FX(0.03); p.speed = 300; p.horzBounciness = FX(1.0); p.vertBounciness = FX(1.0);
			p.bounceCount = 3; p.reflectVariation = 9; p.duration = TICKS(3.0);
		}

		#undef FX
		#undef TICKS
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
// The INF system (TFE_Jedi/InfSystem/infSystem.cpp + infState.cpp) is now linked
// for real, so the former inf_* no-op stubs were removed (they would duplicate the
// real symbols). Doors/elevators/triggers run through the real elevator/trigger tasks.

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
	// Player vertical physics. Eye height matches DF PLAYER_HEIGHT (the camera sits this
	// far above the floor); jump impulse + gravity drive vertical velocity.
	static const fixed16_16 PLAYER_EYE_HEIGHT   = 0x5cccc;   // 5.8 units (PLAYER_HEIGHT)
	static const fixed16_16 PLAYER_WIDTH        = 0x1cccc;   // 1.8 units (collision radius)
	static const fixed16_16 PLAYER_STEP         = 0x38000;   // 3.5 units (max step-up height)
	static const fixed16_16 PLAYER_JUMP_IMPULSE = -2850816;  // -43.5 units/sec (negative = up)
	// Dark Forces walk/strafe acceleration targets (units/sec). These are NOT raw speeds:
	// DF integrates them into a persistent velocity that exponential friction + a velocity
	// cap reduce to a much lower effective speed (~32 u/s). Applying them directly as a
	// per-second speed made the player ~4-8x too fast.
	static const fixed16_16 PLAYER_WALK_SPEED   = FIXED(256);
	static const fixed16_16 PLAYER_STRAFE_SPEED = FIXED(192);
	static const fixed16_16 PLAYER_FRICTION_GROUND = 0xf999;     // FRICTION_DEFAULT (~0.975 / tick)
	static const fixed16_16 PLAYER_FRICTION_AIR    = 0xfef9;     // FRICTION_FALLING (~0.996 / tick)
	static const fixed16_16 PLAYER_MAX_MOVE_DIST   = FIXED(32);  // velocity magnitude cap
	static const fixed16_16 PLAYER_VEL_STOP        = 16384;      // 0.25u: snap tiny velocity to 0
	static fixed16_16 s_playerVelX    = 0;       // persistent horizontal velocity (momentum)
	static fixed16_16 s_playerVelZ    = 0;
	static fixed16_16 s_playerVertVel = 0;
	static bool       s_onGround      = false;  // spawn airborne so the first settle fires INF_EVENT_LAND
	static s32        s_frameTickDelta = 0;      // game ticks elapsed last frame (friction steps)
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
				const fixed16_16 eyeY = floatToFixed16(lastY) - PLAYER_EYE_HEIGHT;
				RSector* sec = sector_which3D(px, eyeY, pz);
				if (sec)
				{
					s_camX = px;
					s_camZ = pz;
					s_camYaw = (angle14_32)floatDegreesToFixed(lastYaw) & ANGLE_MASK;
					s_camSector = sec;
					s_camY = sec->floorHeight - PLAYER_EYE_HEIGHT;
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

	// Per-weapon fire sound sources + the weapon-change sound. weapon.cpp (which normally
	// loads these) isn't linked, so the port loads them in startActorSystem and plays them
	// from playerFireN64 / n64_cycleWeapon. Indexed by WeaponID (0..9).
	static SoundSourceId s_n64WpnFireSnd[10] = { 0 };
	static SoundSourceId s_n64WpnChangeSnd   = 0;

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

		// Start the player at full health/shields.
		s_playerInfo.health      = s_healthMax;
		s_playerInfo.healthFract = 0;
		s_playerInfo.shields     = 100;
		s_playerDying            = JFALSE;

		// Seed the rest of the HUD-relevant player state with the real Dark Forces
		// starting loadout: Bryar pistol equipped, 100 of each starting ammo, full
		// battery, 3 lives. Weapon/pickup systems that would mutate these aren't linked
		// yet, so they stay at these values, but the HUD now reads genuine game state.
		s_playerInfo.ammoEnergy  = 100;
		s_playerInfo.ammoPower   = 100;
		s_playerInfo.ammoPlasma  = 100;
		s_playerInfo.itemPistol  = JTRUE;
		s_playerInfo.curWeapon   = 1;   // WPN_PISTOL
		s_playerInfo.newWeapon   = -1;
		s_playerInfo.maxWeapon   = 1;
		s_batteryPower           = s_batteryPowerMax;
		s_lifeCount              = 3;

		// Testing loadout: also grant the E-11 rifle, repeater and fusion so weapon
		// switching + the different projectile visuals (3DO bolts, bullet frame, plasma
		// sprite) are exercisable before pickups are wired. Pickups will supersede this.
		s_playerInfo.itemRifle   = JTRUE;
		s_playerInfo.itemAutogun = JTRUE;   // repeater
		s_playerInfo.itemFusion  = JTRUE;
		s_playerInfo.ammoShell   = 30;

		// Load the projectile assets (waxes/frames/3DO models + speeds/damage/lifetimes)
		// from the now-populated external projectile table. Without this every projectile
		// stays an invisible, motionless spirit. Must run before projectile_createTask().
		projectile_startup();

		// Load the player weapon-fire sounds from SOUNDS.GOB (weapon.cpp, which normally loads
		// these, isn't linked). Enemy + projectile sounds load themselves via the linked
		// actor/projectile code, so they need no help here.
		s_n64WpnFireSnd[1] = sound_load("pistol-1.voc", SOUND_PRIORITY_HIGH0);
		s_n64WpnFireSnd[2] = sound_load("rifle-1.voc",  SOUND_PRIORITY_HIGH0);
		s_n64WpnFireSnd[4] = sound_load("repeater.voc", SOUND_PRIORITY_HIGH3);
		s_n64WpnFireSnd[5] = sound_load("fusion1.voc",  SOUND_PRIORITY_HIGH3);
		s_n64WpnFireSnd[8] = sound_load("concuss6.voc", SOUND_PRIORITY_HIGH3);
		s_n64WpnFireSnd[9] = sound_load("plasma4.voc",  SOUND_PRIORITY_HIGH3);
		s_n64WpnChangeSnd  = sound_load("weapon1.voc",  SOUND_PRIORITY_LOW4);

		// Projectiles must exist before enemies attack: defaultAttackFunc() fires via
		// createProjectile(), which returns null (-> NULL deref crash) if s_projectiles
		// was never allocated.
		projectile_createTask();

		// Actor system: physics list, clean state, then the logic + physics tasks.
		actor_allocatePhysicsActorList();
		actor_clearState();
		actor_createTask();
	}

	// INF system (moving geometry) setup. MUST run BEFORE level_loadGeometry(): that pass
	// allocates "special" elevators (auto-doors, morphing sectors, etc.) from the elevator
	// allocator created here (inf_createElevatorTask -> s_infSerState.infElevators). If the
	// allocator doesn't exist yet, inf_allocateElevItem() returns null and allocateStop()
	// dereferences it -> NULL crash. inf_load() later populates the rest.
	void setupInfSystem()
	{
		using namespace TFE_Jedi;
		// Framebreak boundary first so task_run() always terminates each frame; every other
		// task (INF, projectile, actor) is created after this and runs before it per frame.
		s_n64LoopTask = createTask("n64loop", n64LoopTaskFunc, JTRUE);
		// Animated-texture allocator MUST exist before level_loadGeometry(): that pass calls
		// bitmap_setupAnimatedTexture() for animated wall/sign textures, which pulls from
		// s_texState.textureAnimAlloc. If the allocator is null, setup silently fails and the
		// sign texture's image stays raw .BM data; inf_createTrigger() then reads
		// animTex->frameList[0] off that garbage -> invalid-address crash.
		bitmap_setupAnimationTask();
		inf_clearState();
		inf_createElevatorTask();
		inf_createTeleportTask();
		inf_createTriggerTask();
	}

	// Advance Dark Forces game time once per frame. updateTime() advances s_curTick /
	// s_frameTicks (drives animation), but in the real engine the mission loop computes
	// s_deltaTime separately; we bypass that loop. Without this, s_deltaTime stays 0 and
	// every actor movement/rotation term (mul16(speed, s_deltaTime)) is 0 -> enemies
	// animate & shoot but never translate or turn.
	void updateGameTime()
	{
		using namespace TFE_DarkForces;
		updateTime();
		s_frameTickDelta = (s32)(s_curTick - s_prevTick);
		s_deltaTime = div16(intToFixed16(s_curTick - s_prevTick), FIXED(TICKS_PER_SECOND));
		if (s_deltaTime > MAX_DELTA_TIME) { s_deltaTime = MAX_DELTA_TIME; }
		s_prevTick = s_curTick;
		s_prevTickFract = s_curTickFract;
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
			s_camY = sector->floorHeight - PLAYER_EYE_HEIGHT;
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

	// Player "use / activate" action (nudge switches + doors). Mirrors the core of the real
	// handlePlayerUseAction(): trigger the current sector, then cast a short ray forward and
	// nudge the wall the player faces (and the sector behind it). Enough to flip switches and
	// open nudge-activated doors/elevators. (The multi-sector eye-ray loop is omitted.)
	static void playerUseActionN64()
	{
		using namespace TFE_Jedi;
		using namespace TFE_DarkForces;
		SecObject* player = s_playerObject;
		if (!player || !player->sector) { return; }
		RSector* sector = player->sector;

		// Trigger whatever the player is standing in (floor/inside-sector switches).
		message_sendToSector(sector, player, INF_EVENT_NUDGE_FRONT, MSG_TRIGGER);

		// Ray a short distance ahead along the facing direction.
		const fixed16_16 x0 = player->posWS.x;
		const fixed16_16 z0 = player->posWS.z;
		const fixed16_16 x1 = x0 + mul16(FIXED(4), s_n64PlayerLogic.dir.x);
		const fixed16_16 z1 = z0 + mul16(FIXED(4), s_n64PlayerLogic.dir.z);
		RWall* wall = collision_wallCollisionFromPath(sector, x0, z0, x1, z1);
		if (!wall) { return; }

		RSector* nextSector = wall->nextSector;

		const fixed16_16 eyeHeight = player->posWS.y - player->worldHeight;
		fixed16_16 hx, hz;
		collision_getHitPoint(&hx, &hz);
		const fixed16_16 distFromW0 = vec2Length(wall->w0->x - hx, wall->w0->z - hz);
		inf_wallAndMirrorSendMessageAtPos(wall, player, INF_EVENT_NUDGE_FRONT, distFromW0, eyeHeight);

		// Nudge the sector on the far side of a two-sided wall (doors triggered from inside).
		if (nextSector)
		{
			message_sendToSector(nextSector, player, INF_EVENT_NUDGE_BACK, MSG_TRIGGER);
		}
	}

	// ---- Player weapons -------------------------------------------------------------
	// The real weapon.cpp task (input/anim/HUD-weapon/switch machinery) isn't linked, so
	// firing is handled here in the port's control loop, but it spawns the REAL projectiles
	// via the linked projectile system (createProjectile/proj_setTransform) — the same path
	// enemies use. Each weapon maps to a projectile type, an ammo pool + per-shot cost, and
	// a fire delay (game ticks between shots). Fist/unsupported return false.
	static Tick s_playerFireCooldown = 0;   // next tick the player may fire
	void weapon_startFireAnimN64();         // defined with the weapon overlay below

	static bool n64_getWeaponFire(s32 weaponId, TFE_DarkForces::ProjectileType* projType,
								   s32** ammo, s32* consumption, Tick* fireDelay)
	{
		using namespace TFE_DarkForces;
		switch (weaponId)
		{
			case 1: *projType = PROJ_PISTOL_BOLT; *ammo = &s_playerInfo.ammoEnergy; *consumption = 1; *fireDelay = 30; return true; // pistol
			case 2: *projType = PROJ_RIFLE_BOLT;  *ammo = &s_playerInfo.ammoEnergy; *consumption = 1; *fireDelay = 14; return true; // rifle
			case 4: *projType = PROJ_REPEATER;    *ammo = &s_playerInfo.ammoPower;  *consumption = 1; *fireDelay = 11; return true; // repeater
			case 5: *projType = PROJ_PLASMA;      *ammo = &s_playerInfo.ammoPower;  *consumption = 1; *fireDelay = 14; return true; // fusion
			case 8: *projType = PROJ_CONCUSSION;  *ammo = &s_playerInfo.ammoPower;  *consumption = 4; *fireDelay = 43; return true; // concussion
			case 9: *projType = PROJ_CANNON;      *ammo = &s_playerInfo.ammoPlasma; *consumption = 1; *fireDelay = 14; return true; // cannon
			default: return false; // fist / thrown / placed weapons (not direct-fire here)
		}
	}

	// Does the player own weapon `weaponId` (WeaponID 0..9)?
	static bool n64_playerOwnsWeapon(s32 weaponId)
	{
		using namespace TFE_DarkForces;
		switch (weaponId)
		{
			case 0: return true;                            // fist (always)
			case 1: return s_playerInfo.itemPistol != 0;
			case 2: return s_playerInfo.itemRifle != 0;
			case 3: return s_playerInfo.ammoDetonator > 0;
			case 4: return s_playerInfo.itemAutogun != 0;   // repeater
			case 5: return s_playerInfo.itemFusion != 0;
			case 6: return s_playerInfo.itemMortar != 0;
			case 7: return s_playerInfo.ammoMine > 0;
			case 8: return s_playerInfo.itemConcussion != 0;
			case 9: return s_playerInfo.itemCannon != 0;
			default: return false;
		}
	}

	// Cycle to the next owned weapon (wraps around the 10 weapon slots).
	static void n64_cycleWeapon()
	{
		using namespace TFE_DarkForces;
		for (s32 step = 1; step <= 9; step++)
		{
			s32 next = (s_playerInfo.curWeapon + step) % 10;
			if (n64_playerOwnsWeapon(next))
			{
				s_playerInfo.curWeapon = next;
				if (s_n64WpnChangeSnd) { sound_play(s_n64WpnChangeSnd); }
				debugf("[weapon] switch -> %ld\n", (long)next);
				return;
			}
		}
	}

	// Fire the current weapon: spawn its projectile aimed where the camera looks, decrement
	// ammo, and apply the fire-rate cooldown. Mirrors the core of weaponFire_pistol without
	// the autoaim / spread / supercharge machinery.
	static void playerFireN64()
	{
		using namespace TFE_Jedi;
		using namespace TFE_DarkForces;
		SecObject* player = s_playerObject;
		if (!player || !player->sector) { return; }
		if (s_curTick < s_playerFireCooldown) { return; }   // fire-rate gate

		ProjectileType projType; s32* ammo; s32 consumption; Tick fireDelay;
		if (!n64_getWeaponFire(s_playerInfo.curWeapon, &projType, &ammo, &consumption, &fireDelay)) { return; }

		// Out of ammo -> click (just gate the cooldown so we don't spin every frame).
		if (ammo && *ammo < consumption) { s_playerFireCooldown = s_curTick + fireDelay; return; }

		// Spawn the bolt at the gun muzzle -- a few units in FRONT of the eye along the aim --
		// not exactly at the camera. A bolt spawned at the eye and fired straight down the view
		// axis sits on the near-clip plane on its first frame and otherwise projects to the dead
		// centre point, where it just shrinks in place: the player never sees their own shot fly.
		// (Enemy bolts use the same wrbolt.3do yet are visible because they spawn offset in front
		// of the shooter and cross the view.) Offsetting the spawn forward + slightly down/right
		// clears the near plane and makes the bolt trace a visible path from the barrel. This
		// mirrors Dark Forces' muzzle offset (weaponFire_pistol inVec ~ {0.4, 0.6, 3.2}).
		fixed16_16 sinYaw, cosYaw, sinPitch, cosPitch;
		sinCosFixed(s_camYaw,   &sinYaw,   &cosYaw);
		sinCosFixed(s_camPitch, &sinPitch, &cosPitch);
		const fixed16_16 mzFwd   = 0x33333; // 3.2 units forward along the aim
		const fixed16_16 mzRight = 0x8000;  // 0.5 units right of the eye (barrel offset)
		const fixed16_16 mzDown  = 0x6666;  // 0.4 units below the eye (+Y is down)
		const fixed16_16 fwdX = mul16(cosPitch, sinYaw); // forward (sinYaw,cosYaw) tilted by pitch
		const fixed16_16 fwdZ = mul16(cosPitch, cosYaw);
		const fixed16_16 fwdY = -sinPitch;               // +pitch looks up; up is -Y
		const fixed16_16 spawnX = player->posWS.x + mul16(mzFwd, fwdX) + mul16(mzRight, cosYaw);
		const fixed16_16 spawnZ = player->posWS.z + mul16(mzFwd, fwdZ) - mul16(mzRight, sinYaw);
		const fixed16_16 yPos   = (player->posWS.y - player->worldHeight) + mul16(mzFwd, fwdY) + mzDown;
		TFE_DarkForces::ProjectileLogic* proj = (TFE_DarkForces::ProjectileLogic*)createProjectile(projType, player->sector, spawnX, yPos, spawnZ, player);
		if (proj)
		{
			proj->prevColObj = player;   // don't collide with the shooter on frame 1
			proj->prevObj    = player;
			proj_setTransform(proj, s_camPitch, s_camYaw);
		}

		if (ammo) { *ammo -= consumption; if (*ammo < 0) { *ammo = 0; } }
		s_playerFireCooldown = s_curTick + fireDelay;
		weapon_startFireAnimN64();   // play the weapon's muzzle-flash animation

		const s32 wid = s_playerInfo.curWeapon;
		if (wid >= 0 && wid < 10 && s_n64WpnFireSnd[wid]) { sound_play(s_n64WpnFireSnd[wid]); }
	}

	// Read the controller and move the player around the level: stick to walk/turn,
	// C-buttons to strafe/tilt, A to jump, B to use/activate switches + doors. Vertical
	// motion uses DF gravity + jump impulse; the eye sits PLAYER_EYE_HEIGHT above the floor.
	void updateCameraN64()
	{
		using namespace TFE_Jedi;
		using namespace TFE_DarkForces;
		if (!s_renderReady || !s_camSector) { return; }

		const joypad_inputs_t  in      = joypad_get_inputs(JOYPAD_PORT_1);
		const joypad_buttons_t held    = joypad_get_buttons_held(JOYPAD_PORT_1);
		const joypad_buttons_t pressed = joypad_get_buttons_pressed(JOYPAD_PORT_1);

		// Analog stick with a small deadzone (neutral can drift on real sticks).
		s32 sx = in.stick_x; if (sx > -8 && sx < 8) { sx = 0; }
		s32 sy = in.stick_y; if (sy > -8 && sy < 8) { sy = 0; }

		// Look: stick X turns (yaw); C-Up/Down tilt (pitch).
		// Pitch is clamped to Dark Forces' vanilla limit (PITCH_VANILLA = 2047, ~45deg).
		// The fixed renderer's floor/ceiling draw indexes s_rcfState.rcpY (sized 4*s_height)
		// with (yShear + y + s_height*2); yShear grows with floor16(pitchOffset). At 320-wide
		// a larger limit (e.g. 2730 ~60deg) makes the bottom floor scanline index exceed the
		// table (=> assert/crash in flat_drawFloor when looking down). 2047 keeps it in range.
		s_camYaw = (s_camYaw + sx * 3) & ANGLE_MASK;
		if (held.c_up)   { s_camPitch += 96; }
		if (held.c_down) { s_camPitch -= 96; }
		if (s_camPitch >  2047) { s_camPitch =  2047; }
		if (s_camPitch < -2047) { s_camPitch = -2047; }

		// Horizontal movement: real Dark Forces velocity model. Input accelerates a
		// persistent velocity toward a target speed; exponential friction (per game tick)
		// and a velocity-magnitude cap bleed it back down. This gives DF's momentum/feel
		// and the correct effective speed, instead of treating PLAYER_WALK_SPEED as a raw
		// per-second speed (which ran ~4-8x too fast).
		fixed16_16 sinYaw, cosYaw;
		sinCosFixed(s_camYaw, &sinYaw, &cosYaw);

		// Analog stick -> forward acceleration this frame (stick maxes ~+/-85 -> +/-1.0).
		fixed16_16 stickFrac = sy * 768;
		if (stickFrac >  ONE_16) { stickFrac =  ONE_16; }
		if (stickFrac < -ONE_16) { stickFrac = -ONE_16; }
		const fixed16_16 fwdAccel = mul16(mul16(PLAYER_WALK_SPEED, s_deltaTime), stickFrac);
		fixed16_16 strafeAccel = 0;
		if (held.c_right) { strafeAccel =  mul16(PLAYER_STRAFE_SPEED, s_deltaTime); }
		if (held.c_left)  { strafeAccel = -mul16(PLAYER_STRAFE_SPEED, s_deltaTime); }

		// Exponential friction on the persistent velocity: one step per elapsed game tick
		// (less friction while airborne so jumps keep their momentum).
		const fixed16_16 friction = s_onGround ? PLAYER_FRICTION_GROUND : PLAYER_FRICTION_AIR;
		for (s32 i = 0; i < s_frameTickDelta; i++)
		{
			s_playerVelX = mul16(friction, s_playerVelX);
			s_playerVelZ = mul16(friction, s_playerVelZ);
		}
		// Snap tiny residual velocity to zero so the player doesn't creep forever.
		if (TFE_Jedi::abs(s_playerVelX) < PLAYER_VEL_STOP && TFE_Jedi::abs(s_playerVelZ) < PLAYER_VEL_STOP)
		{
			s_playerVelX = 0;
			s_playerVelZ = 0;
		}

		// Accelerate along facing (forward) and strafe (yaw + 90 degrees).
		s_playerVelX += mul16(fwdAccel, sinYaw) + mul16(strafeAccel, cosYaw);
		s_playerVelZ += mul16(fwdAccel, cosYaw) - mul16(strafeAccel, sinYaw);

		// Cap the velocity magnitude (DF limitVectorLength to PLAYER_MAX_MOVE_DIST).
		const fixed16_16 velLen = vec2Length(s_playerVelX, s_playerVelZ);
		if (velLen > PLAYER_MAX_MOVE_DIST)
		{
			const fixed16_16 scale = div16(PLAYER_MAX_MOVE_DIST, velLen);
			s_playerVelX = mul16(s_playerVelX, scale);
			s_playerVelZ = mul16(s_playerVelZ, scale);
		}

		// Per-frame movement = velocity * delta time.
		const fixed16_16 dx = mul16(s_playerVelX, s_deltaTime);
		const fixed16_16 dz = mul16(s_playerVelZ, s_deltaTime);

		SecObject* pobj = s_playerObject;
		if (pobj)
		{
			// --- Real Dark Forces movement: step-height-aware collision + gravity. ---
			// Drive everything off the player object so movement matches the original:
			// handlePlayerCollision limits step-up to PLAYER_STEP and slides along walls;
			// playerMove commits the move and crosses sector boundaries; gravity then pulls
			// the player toward the lowest reachable floor (so you fall off ledges).
			if (pobj->sector) { s_camSector = pobj->sector; }
			pobj->yaw         = s_camYaw;
			pobj->pitch       = s_camPitch;   // so projectiles aim with the look direction
			pobj->worldWidth  = PLAYER_WIDTH;
			pobj->worldHeight = PLAYER_EYE_HEIGHT;

			// Facing direction for the use-action ray / INF direction checks.
			s_n64PlayerLogic.dir.x = sinYaw;
			s_n64PlayerLogic.dir.z = cosYaw;

			// B: use / activate switches + nudge-open doors in front of the player.
			if (pressed.b) { playerUseActionN64(); }

			// Z: fire the current weapon (held = auto-fire at the weapon's rate).
			// R: cycle to the next owned weapon.
			if (held.z)    { playerFireN64(); }
			if (pressed.r) { n64_cycleWeapon(); }

			s_n64PlayerLogic.move.x     = dx;
			s_n64PlayerLogic.move.y     = 0;
			s_n64PlayerLogic.move.z     = dz;
			s_n64PlayerLogic.stepHeight = PLAYER_STEP;   // can't climb steps taller than 3.5 units

			// Collision response loop (DF runs up to 4 slide iterations). Each call to
			// handlePlayerCollision recomputes the reachable floor/ceiling for the move and,
			// when it hits a wall it can slide along, rewrites move into the slide vector.
			JBool moved = JFALSE;
			for (s32 iter = 4; iter != 0; iter--)
			{
				if (!handlePlayerCollision(&s_n64PlayerLogic, s_playerVertVel))
				{
					if (s_n64PlayerLogic.move.x || s_n64PlayerLogic.move.z) { moved = JTRUE; }
					break;
				}
				if (!s_colResponseStep) { break; }   // solid wall, nothing to slide along
			}
			if (moved) { playerMove(JFALSE); }       // commit horizontal move + sector crossing

			// --- Vertical physics (DF model: y increases downward, jump is negative). ---
			const bool wasOnGround = s_onGround;
			if (pressed.a && s_onGround) { s_playerVertVel = PLAYER_JUMP_IMPULSE; s_onGround = false; }
			s_playerVertVel += mul16(s_gravityAccel, s_deltaTime);
			fixed16_16 newY = pobj->posWS.y + mul16(s_playerVertVel, s_deltaTime);

			if (newY >= s_colCurLowestFloor)         // reached the floor -> land
			{
				newY = s_colCurLowestFloor;
				if (s_playerVertVel > 0) { s_playerVertVel = 0; }   // min(0, vel): kill downward speed

				// Touchdown from the air fires the INF "land on floor" event, so LAND-triggered
				// elevators/doors activate. The SECBASE starting gate lives on the start sector
				// and opens on INF_EVENT_LAND (not a nudge). Mirrors DF handlePlayerPhysics'
				// message_sendToSector(..., INF_EVENT_LAND, ...) on landing. s_onGround starts
				// false so the player's initial settle onto the spawn floor triggers it.
				if (!wasOnGround && pobj->sector)
				{
					message_sendToSector(pobj->sector, pobj, INF_EVENT_LAND, MSG_TRIGGER);
				}
				s_onGround = true;
			}
			else
			{
				// Head bump: don't pass through the lowest reachable ceiling.
				const fixed16_16 playerTop = newY - pobj->worldHeight - ONE_16;
				if (playerTop < s_colCurHighestCeil)
				{
					if (s_playerVertVel < 0) { s_playerVertVel = 0; }   // max(0, vel): stop rising
					const fixed16_16 newBot = s_colCurHighestCeil + pobj->worldHeight + ONE_16;
					newY = (s_colCurLowestFloor < newBot) ? s_colCurLowestFloor : newBot;
				}
				s_onGround = false;
			}
			pobj->posWS.y = newY;

			// Sync the eye camera to the player (eye sits PLAYER_EYE_HEIGHT above the feet).
			if (pobj->sector) { s_camSector = pobj->sector; }
			s_camX = pobj->posWS.x;
			s_camZ = pobj->posWS.z;
			s_camY = pobj->posWS.y - PLAYER_EYE_HEIGHT;
		}
		else
		{
			// Fallback (no player object loaded): point move + manual floor snap, no
			// step-height limit. Kept only so the camera still works if no LOGIC PLAYER
			// start exists in the level.
			if (dx || dz)
			{
				RSector* next = sector_which3D(s_camX + dx, s_camY, s_camZ + dz);
				if (next) { s_camX += dx; s_camZ += dz; s_camSector = next; }
			}
			const fixed16_16 floorEyeY = s_camSector->floorHeight - PLAYER_EYE_HEIGHT;
			if (pressed.a && s_onGround) { s_playerVertVel = PLAYER_JUMP_IMPULSE; s_onGround = false; }
			s_playerVertVel += mul16(s_gravityAccel, s_deltaTime);
			s_camY += mul16(s_playerVertVel, s_deltaTime);
			if (s_camY >= floorEyeY) { s_camY = floorEyeY; s_playerVertVel = 0; s_onGround = true; }
			else                     { s_onGround = false; }
			const fixed16_16 ceilEyeY = s_camSector->ceilingHeight + FIXED(1);
			if (s_camY < ceilEyeY) { s_camY = ceilEyeY; if (s_playerVertVel < 0) { s_playerVertVel = 0; } }
		}

		s_eyePos.x = s_camX;
		s_eyePos.y = s_camY;
		s_eyePos.z = s_camZ;
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

	// -----------------------------------------------------------------
	// HUD: the real Dark Forces status display, drawn straight into the
	// 320x200 CI8 framebuffer. We reuse the genuine STATUSLF/RT.BM panel
	// art and the HelNum/ArmNum number fonts, but blit them ourselves
	// (the engine's OffScreenBuffer/GPU HUD path pulls heavy deps). Panel
	// + font assets are column-major, bottom-to-top CI8 (TextureData);
	// index 0 is transparent. Values come from the live s_playerInfo.
	// -----------------------------------------------------------------
	static TextureData* s_hudStatusL = nullptr;
	static TextureData* s_hudStatusR = nullptr;
	static Font* s_hudHealthFont = nullptr;   // HelNum.fnt
	static Font* s_hudShieldFont = nullptr;   // ArmNum.fnt
	static Font* s_hudAmmoFont   = nullptr;   // AmoNum.fnt
	static bool  s_hudLoaded     = false;

	static Font* hud_loadFontN64(const char* name)
	{
		FilePath fp;
		if (!TFE_Paths::getFilePath(name, &fp)) { return nullptr; }
		return TFE_Jedi::font_load(&fp);
	}

	void hud_loadN64()
	{
		using namespace TFE_Jedi;
		s_hudStatusL    = bitmap_load("StatusLf.bm", 1, POOL_GAME);
		s_hudStatusR    = bitmap_load("StatusRt.bm", 1, POOL_GAME);
		s_hudHealthFont = hud_loadFontN64("HelNum.fnt");
		s_hudShieldFont = hud_loadFontN64("ArmNum.fnt");
		s_hudAmmoFont   = hud_loadFontN64("AmoNum.fnt");
		s_hudLoaded     = true;
		debugf("[hud] load statusL=%p statusR=%p hpFnt=%p shFnt=%p amFnt=%p\n",
			(void*)s_hudStatusL, (void*)s_hudStatusR, (void*)s_hudHealthFont,
			(void*)s_hudShieldFont, (void*)s_hudAmmoFont);
	}

	// Blit one column-major, bottom-to-top CI8 TextureData to the CI8 frame
	// at top-left (dstX, dstY). Index 0 is transparent. Clipped to 320x200.
	static void hud_blitCI8(const TextureData* tex, u8* fb, s32 dstX, s32 dstY)
	{
		if (!tex || !tex->image) { return; }
		const s32 w = tex->width;
		const s32 h = tex->height;
		for (s32 c = 0; c < w; c++)
		{
			const s32 sx = dstX + c;
			if (sx < 0 || sx >= 320) { continue; }
			const u8* col = tex->image + c * h;
			for (s32 r = 0; r < h; r++)
			{
				const u8 pix = col[r];
				if (!pix) { continue; }               // transparent
				const s32 sy = dstY + (h - 1 - r);    // src row 0 is bottom
				if (sy < 0 || sy >= 200) { continue; }
				fb[sy * 320 + sx] = pix;
			}
		}
	}

	// Draw a numeric string with a DF number font into the CI8 frame.
	static void hud_drawNumberN64(Font* font, u8* fb, s32 x0, s32 y0, const char* str)
	{
		if (!font || !font->glyphs) { return; }
		s32 x = x0;
		for (const char* p = str; *p; p++)
		{
			const char ch = *p;
			if (ch == ' ') { x += font->width; continue; }
			if (ch < font->minChar || ch > font->maxChar) { continue; }
			TextureData* glyph = &font->glyphs[ch - font->minChar];
			hud_blitCI8(glyph, fb, x, y0);
			x += glyph->width + font->horzSpacing;
		}
	}

	// Resolve the primary/secondary ammo counts shown on the right HUD panel for the
	// currently-equipped weapon. Mapping mirrors the shipped weapons.json (weapon index
	// order): FIST=0 (no ammo) .. CANNON=9 (plasma + missiles). Returns -1 for "no ammo"
	// (rendered as the DF "::" placeholder).
	static void hud_getWeaponAmmoN64(s32 weaponId, s32* primary, s32* secondary)
	{
		using namespace TFE_DarkForces;
		*primary   = -1;
		*secondary = -1;
		switch (weaponId)
		{
			case 1: *primary = s_playerInfo.ammoEnergy;    break;  // WPN_PISTOL
			case 2: *primary = s_playerInfo.ammoEnergy;    break;  // WPN_RIFLE
			case 3: *primary = s_playerInfo.ammoDetonator; break;  // WPN_THERMAL_DET
			case 4: *primary = s_playerInfo.ammoPower;     break;  // WPN_REPEATER
			case 5: *primary = s_playerInfo.ammoPower;     break;  // WPN_FUSION
			case 6: *primary = s_playerInfo.ammoShell;     break;  // WPN_MORTAR
			case 7: *primary = s_playerInfo.ammoMine;      break;  // WPN_MINE
			case 8: *primary = s_playerInfo.ammoPower;     break;  // WPN_CONCUSSION
			case 9: *primary = s_playerInfo.ammoPlasma;            // WPN_CANNON
					*secondary = s_playerInfo.ammoMissile; break;
			default: break;                                        // WPN_FIST / none
		}
	}

	// Composite the status panels + health/shield numbers over the world.
	void hud_drawN64(u8* fb)
	{
		using namespace TFE_DarkForces;
		if (!s_hudLoaded) { hud_loadN64(); }
		if (!s_hudStatusL || !s_hudStatusR) { return; }

		// Panels sit at the bottom corners (DF 320x200 layout: left x=0, right x=260).
		const s32 panelY      = 200 - s_hudStatusL->height;
		const s32 rightPanelX = 260;
		const s32 rightPanelY = 200 - s_hudStatusR->height;
		hud_blitCI8(s_hudStatusL, fb, 0,           panelY);
		hud_blitCI8(s_hudStatusR, fb, rightPanelX, rightPanelY);

		char buf[16];

		// Left panel: shields (15,26), health (33,26), life count (52,26) — same glyph
		// offsets as the real hud_drawAndUpdate.
		s32 shields = s_playerInfo.shields; if (shields < 0) { shields = 0; }
		s32 health  = s_playerInfo.health;  if (health  < 0) { health  = 0; }

		sprintf(buf, "%03d", (int)shields);
		hud_drawNumberN64(s_hudShieldFont, fb, 15, panelY + 26, buf);
		sprintf(buf, "%03d", (int)health);
		hud_drawNumberN64(s_hudHealthFont, fb, 33, panelY + 26, buf);
		sprintf(buf, "%1d", (int)(s_lifeCount % 10));
		hud_drawNumberN64(s_hudHealthFont, fb, 52, panelY + 26, buf);

		// Right panel: primary ammo (12,21) and secondary ammo (25,12) for the current
		// weapon. Positions are relative to the right panel origin. "no ammo" -> DF ":::".
		s32 ammo0 = -1, ammo1 = -1;
		hud_getWeaponAmmoN64(s_playerInfo.curWeapon, &ammo0, &ammo1);

		if (ammo0 < 0) { strcpy(buf, ":::"); } else { sprintf(buf, "%03d", (int)ammo0); }
		hud_drawNumberN64(s_hudAmmoFont, fb, rightPanelX + 12, rightPanelY + 21, buf);

		if (ammo1 < 0) { strcpy(buf, "::"); } else { sprintf(buf, "%02d", (int)ammo1); }
		hud_drawNumberN64(s_hudShieldFont, fb, rightPanelX + 25, rightPanelY + 12, buf);
	}

	// ---- First-person weapon overlay (weapon sprite + fire animation) -----------------
	// weapon.cpp's draw/anim task isn't linked (HUD/input/sound deps), so we draw the weapon
	// BM frames ourselves with the same column-major, bottom-up CI8 blit as the HUD. Per
	// weapon: frame textures + their screen positions, and a fire animation (frame index +
	// tick duration per step). Values transcribed from the shipped weapons.json. Idle=frame 0.
	struct N64WeaponVisual
	{
		s32         texCount;
		const char* tex[6];
		s32         xPos[6];
		s32         yPos[6];
		s32         animCount;
		s32         animFrame[6];   // frame index at each fire-anim step
		s32         animDur[6];     // ticks to hold each step
	};

	static const N64WeaponVisual s_weaponVisuals[10] =
	{
		/* 0 FIST       */ { 0 },
		/* 1 PISTOL     */ { 3, {"pistol1.bm","pistol2.bm","pistol3.bm"}, {165,169,169}, {142,136,136}, 3, {1,2,0}, {14,14,43} },
		/* 2 RIFLE      */ { 2, {"rifle1.bm","rifle2.bm"}, {113,112}, {127,114}, 2, {1,0}, {7,14} },
		/* 3 THERMAL    */ { 0 },
		/* 4 REPEATER   */ { 3, {"autogun1.bm","autogun2.bm","autogun3.bm"}, {156,163,163}, {138,140,140}, 2, {1,2}, {5,11} },
		/* 5 FUSION     */ { 6, {"fusion1.bm","fusion2.bm","fusion3.bm","fusion4.bm","fusion5.bm","fusion6.bm"}, {19,23,23,23,23,23}, {152,155,155,155,155,155}, 5, {0,1,2,3,4}, {14,20,20,20,20} },
		/* 6 MORTAR     */ { 0 },
		/* 7 MINE       */ { 0 },
		/* 8 CONCUSSION */ { 0 },
		/* 9 CANNON     */ { 0 },
	};

	static TextureData* s_weaponTex[10][6] = {};
	static bool s_weaponTexLoaded[10]      = {};
	static s32  s_weaponCurFrame = 0;    // current frame index of the drawn weapon
	static s32  s_weaponAnimStep = -1;   // -1 = idle; else index into animFrame[]
	static Tick s_weaponAnimNext = 0;    // tick to advance the fire animation
	static s32  s_weaponDrawnId  = -1;   // weapon whose textures are currently cached

	// Kick off the current weapon's fire animation (called from playerFireN64 on a real shot).
	void weapon_startFireAnimN64()
	{
		using namespace TFE_DarkForces;
		const s32 wid = s_playerInfo.curWeapon;
		if (wid < 0 || wid >= 10) { return; }
		const N64WeaponVisual& v = s_weaponVisuals[wid];
		if (v.animCount <= 0) { return; }
		s_weaponAnimStep = 0;
		s_weaponCurFrame = v.animFrame[0];
		s_weaponAnimNext = s_curTick + v.animDur[0];
	}

	// Draw the first-person weapon over the world (before the HUD panels).
	void weapon_drawN64(u8* fb)
	{
		using namespace TFE_Jedi;
		using namespace TFE_DarkForces;
		const s32 wid = s_playerInfo.curWeapon;
		if (wid < 0 || wid >= 10) { return; }
		const N64WeaponVisual& v = s_weaponVisuals[wid];
		if (v.texCount <= 0) { return; }   // no on-screen weapon (fist / unsupported)

		// Weapon changed -> reset to idle and lazily load its BM frames.
		if (wid != s_weaponDrawnId)
		{
			s_weaponDrawnId  = wid;
			s_weaponCurFrame = 0;
			s_weaponAnimStep = -1;
			if (!s_weaponTexLoaded[wid])
			{
				s_weaponTexLoaded[wid] = true;
				for (s32 i = 0; i < v.texCount; i++)
				{
					s_weaponTex[wid][i] = bitmap_load(v.tex[i], 1, POOL_GAME);
				}
			}
		}

		// Advance the fire animation; when the sequence ends, return to the idle frame.
		while (s_weaponAnimStep >= 0 && s_curTick >= s_weaponAnimNext)
		{
			s_weaponAnimStep++;
			if (s_weaponAnimStep >= v.animCount)
			{
				s_weaponAnimStep = -1;
				s_weaponCurFrame = 0;
			}
			else
			{
				s_weaponCurFrame = v.animFrame[s_weaponAnimStep];
				s_weaponAnimNext = s_curTick + v.animDur[s_weaponAnimStep];
			}
		}

		s32 f = s_weaponCurFrame;
		if (f < 0 || f >= v.texCount) { f = 0; }
		TextureData* tex = s_weaponTex[wid][f];
		if (tex) { hud_blitCI8(tex, fb, v.xPos[f], v.yPos[f]); }
	}

	// Bring up libdragon audio + the RSP mixer for SFX (called once at startup).
	void soundInitN64()
	{
		audio_init(22050, 4);     // 22 kHz output, 4 buffers (DF SFX are ~11 kHz mono)
		mixer_init(TFE_DarkForces::N64_SFX_CHANNELS);
		TFE_DarkForces::s_n64AudioReady = true;
	}

	// Refresh the sound listener from the player camera + pump the mixer. Once per frame.
	void soundUpdateN64()
	{
		if (!TFE_DarkForces::s_n64AudioReady) { return; }
		TFE_DarkForces::s_sndListenerX   = s_camX;
		TFE_DarkForces::s_sndListenerZ   = s_camZ;
		TFE_DarkForces::s_sndListenerYaw = s_camYaw;
		mixer_try_play();
	}

	void renderLevelFrame(u8* display)
	{
		if (!s_renderReady) { return; }
		memset(display, 0, 320 * 200);
		TFE_Jedi::renderer_computeCameraTransform(s_camSector, s_camPitch, s_camYaw, s_camX, s_camY, s_camZ);
		TFE_Jedi::drawWorld(display, s_camSector, s_colorMapData, s_lightRamp);
		weapon_drawN64(display);
		hud_drawN64(display);
	}
}
