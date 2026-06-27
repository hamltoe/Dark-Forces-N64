# Dark Forces N64 — root Makefile (libdragon n64.mk lane)
#
# Build:  wsl env N64_INST=/opt/libdragon make DEBUG=1
# Output: darkforces.z64
#
# This compiles The Force Engine's entry point + the N64 (libdragon) render
# backend into an N64 ROM. The engine source list (SRC_CXX) is grown as more
# modules are linked into the ROM.

BUILD_DIR = build
include $(N64_INST)/include/n64.mk

N64_ROM_TITLE = "Dark Forces N64"

TFE_DIR  = TheForceEngine
INCFLAGS = -I$(TFE_DIR) -I$(TFE_DIR)/TFE_ForceScript/Angelscript/angelscript/include

# DEBUG=1 enables debug logging (debug.log on hardware) and on-screen prints.
DEBUG ?= 0
ifeq ($(DEBUG),1)
  DF_FLAGS = -DDF_DEBUG=1
else
  DF_FLAGS = -DDF_DEBUG=0
endif

# Engine include dirs + relax -Werror for the whole ROM build chain.
# Appended AFTER n64.mk's N64_*FLAGS so -Wno-error wins over its -Werror.
%.z64: CFLAGS   += $(INCFLAGS) $(DF_FLAGS) -Wno-error
%.z64: CXXFLAGS += $(INCFLAGS) $(DF_FLAGS) -Wno-error

# Real engine modules linked into the ROM (the fixed-point software render path).
# NOTE: must be defined BEFORE SRC_CXX/OBJS because make expands rule prerequisites
# at parse time; if defined later it would expand to empty in the elf rule.
ENGINE_SRC = \
	$(TFE_DIR)/TFE_System/math.cpp \
	$(TFE_DIR)/TFE_System/parser.cpp \
	$(TFE_DIR)/TFE_Memory/memoryRegion.cpp \
	$(TFE_DIR)/TFE_Memory/chunkedArray.cpp \
	$(TFE_DIR)/TFE_Jedi/Math/core_math.cpp \
	$(TFE_DIR)/TFE_Jedi/Math/cosTable.cpp \
	$(TFE_DIR)/TFE_Jedi/Level/rtexture.cpp \
	$(TFE_DIR)/TFE_Jedi/Level/rwall.cpp \
	$(TFE_DIR)/TFE_Jedi/Level/rsector.cpp \
	$(TFE_DIR)/TFE_Jedi/Level/levelData.cpp \
	$(TFE_DIR)/TFE_Jedi/Level/level.cpp \
	$(TFE_DIR)/TFE_Asset/spriteAsset_Jedi.cpp \
	$(TFE_DIR)/TFE_Asset/modelAsset_jedi.cpp \
	$(TFE_DIR)/TFE_Jedi/Renderer/jediRenderer.cpp \
	$(TFE_DIR)/TFE_Jedi/Renderer/rcommon.cpp \
	$(TFE_DIR)/TFE_Jedi/Renderer/rscanline.cpp \
	$(TFE_DIR)/TFE_Jedi/Renderer/rsectorRender.cpp \
	$(TFE_DIR)/TFE_Jedi/Renderer/screenDraw.cpp \
	$(TFE_DIR)/TFE_Jedi/Renderer/virtualFramebuffer.cpp \
	$(TFE_DIR)/TFE_Jedi/Renderer/RClassic_Fixed/rclassicFixed.cpp \
	$(TFE_DIR)/TFE_Jedi/Renderer/RClassic_Fixed/rclassicFixedSharedState.cpp \
	$(TFE_DIR)/TFE_Jedi/Renderer/RClassic_Fixed/redgePairFixed.cpp \
	$(TFE_DIR)/TFE_Jedi/Renderer/RClassic_Fixed/rflatFixed.cpp \
	$(TFE_DIR)/TFE_Jedi/Renderer/RClassic_Fixed/rlightingFixed.cpp \
	$(TFE_DIR)/TFE_Jedi/Renderer/RClassic_Fixed/robj3d_fixed/robj3dFixed.cpp \
	$(TFE_DIR)/TFE_Jedi/Renderer/RClassic_Fixed/robj3d_fixed/robj3dFixed_Clipping.cpp \
	$(TFE_DIR)/TFE_Jedi/Renderer/RClassic_Fixed/robj3d_fixed/robj3dFixed_Culling.cpp \
	$(TFE_DIR)/TFE_Jedi/Renderer/RClassic_Fixed/robj3d_fixed/robj3dFixed_PolygonDraw.cpp \
	$(TFE_DIR)/TFE_Jedi/Renderer/RClassic_Fixed/robj3d_fixed/robj3dFixed_PolygonSetup.cpp \
	$(TFE_DIR)/TFE_Jedi/Renderer/RClassic_Fixed/robj3d_fixed/robj3dFixed_TransformAndLighting.cpp \
	$(TFE_DIR)/TFE_Jedi/Renderer/RClassic_Fixed/rsectorFixed.cpp \
	$(TFE_DIR)/TFE_Jedi/Renderer/RClassic_Fixed/rwallFixed.cpp \
	$(TFE_DIR)/TFE_Jedi/Task/task.cpp \
	$(TFE_DIR)/TFE_DarkForces/time.cpp \
	$(TFE_DIR)/TFE_DarkForces/random.cpp \
	$(TFE_DIR)/TFE_Jedi/Memory/list.cpp \
	$(TFE_DIR)/TFE_DarkForces/playerCollision.cpp \
	$(TFE_DIR)/TFE_Jedi/Collision/collision.cpp \
	$(TFE_DIR)/TFE_Jedi/InfSystem/message.cpp \
	$(TFE_DIR)/TFE_DarkForces/logic.cpp \
	$(TFE_DIR)/TFE_DarkForces/animLogic.cpp \
	$(TFE_DIR)/TFE_DarkForces/projectile.cpp \
	$(TFE_DIR)/TFE_DarkForces/Actor/actor.cpp \
	$(TFE_DIR)/TFE_DarkForces/Actor/animTables.cpp \
	$(TFE_DIR)/TFE_DarkForces/Actor/troopers.cpp \
	$(TFE_DIR)/TFE_DarkForces/Actor/enemies.cpp \
	$(TFE_DIR)/TFE_DarkForces/Actor/exploders.cpp \
	$(TFE_DIR)/TFE_DarkForces/Actor/flyers.cpp \
	$(TFE_DIR)/TFE_DarkForces/Actor/bobaFett.cpp \
	$(TFE_DIR)/TFE_DarkForces/Actor/dragon.cpp \
	$(TFE_DIR)/TFE_DarkForces/Actor/mousebot.cpp \
	$(TFE_DIR)/TFE_DarkForces/Actor/phaseOne.cpp \
	$(TFE_DIR)/TFE_DarkForces/Actor/phaseTwo.cpp \
	$(TFE_DIR)/TFE_DarkForces/Actor/phaseThree.cpp \
	$(TFE_DIR)/TFE_DarkForces/Actor/scenery.cpp \
	$(TFE_DIR)/TFE_DarkForces/Actor/sewer.cpp \
	$(TFE_DIR)/TFE_DarkForces/Actor/turret.cpp \
	$(TFE_DIR)/TFE_DarkForces/Actor/welder.cpp

# Source list — grow this as engine modules are linked into the ROM.
SRC_CXX = \
	$(TFE_DIR)/main.cpp \
	$(TFE_DIR)/TFE_RenderBackend/N64/renderBackend_n64.cpp \
	$(TFE_DIR)/TFE_System/log.cpp \
	$(TFE_DIR)/TFE_System/n64_shims.cpp \
	$(TFE_DIR)/TFE_FileSystem/filestream.cpp \
	$(TFE_DIR)/TFE_FileSystem/memorystream.cpp \
	$(TFE_DIR)/TFE_Archive/gobArchive.cpp \
	$(TFE_DIR)/TFE_Asset/dfKeywords.cpp \
	$(TFE_DIR)/TFE_Jedi/Memory/allocator.cpp \
	$(TFE_DIR)/TFE_Jedi/Level/levelBin.cpp \
	$(TFE_DIR)/TFE_Jedi/Level/robject.cpp \
	$(TFE_DIR)/TFE_Jedi/Level/robjData.cpp \
	$(ENGINE_SRC)

OBJS = $(SRC_CXX:%.cpp=$(BUILD_DIR)/%.o)

all: darkforces.z64

# Stage real game data into the DragonFS image (available at runtime as rom:/...).
filesystem/%.GOB: GOBs/%.GOB
	@mkdir -p filesystem
	@echo "    [DATA] $@"
	@cp $< $@

$(BUILD_DIR)/darkforces.dfs: filesystem/DARK.GOB filesystem/TEXTURES.GOB filesystem/SPRITES.GOB $(wildcard filesystem/*)
$(BUILD_DIR)/darkforces.elf: $(OBJS)

darkforces.z64: N64_ROM_TITLE = "Dark Forces N64"
darkforces.z64: $(BUILD_DIR)/darkforces.dfs

# --- engine-check: compile-only check of the engine modules (to build/check/*.o),
#     handy for isolating compile traps without a full ROM link.
ENGINE_CHECK_OBJ = $(ENGINE_SRC:%.cpp=$(BUILD_DIR)/check/%.o)
ENGINE_CHECK_CXXFLAGS = $(N64_CXXFLAGS) $(INCFLAGS) $(DF_FLAGS) -Wno-error

$(BUILD_DIR)/check/%.o: %.cpp
	@mkdir -p $(dir $@)
	@echo "    [CHK] $<"
	@$(N64_CXX) -c $(ENGINE_CHECK_CXXFLAGS) -o $@ $<

engine-check: $(ENGINE_CHECK_OBJ)
	@echo "engine-check: compiled $(words $(ENGINE_CHECK_SRC)) modules"

clean:
	rm -rf $(BUILD_DIR) darkforces.z64

-include $(shell find $(BUILD_DIR) -name '*.d' 2>/dev/null)

.PHONY: all clean engine-check
