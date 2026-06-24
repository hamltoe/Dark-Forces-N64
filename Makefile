.DEFAULT_GOAL := all

ROM_NAME := darkforces
# Default to runtime-linked lane so menu launch symbols are present in normal builds.
TFE_N64_LINK_DF_RUNTIME ?= 1
# Optional linker probe for staged mission de-shim work; keep off by default.
TFE_N64_LINK_MISSION_PROBE ?= 0
# Attempt direct mission rendering by linking real Dark Forces/Jedi modules
# while still allowing probe fallbacks for unresolved systems.
TFE_N64_DIRECT_MISSION_RENDER ?= 1

ifeq ($(TFE_N64_LINK_DF_RUNTIME),1)
BUILD_DIR := build-runtime
else
BUILD_DIR := build
endif

SOURCE_DIR := .
N64_MKDFS_ROOT := filesystem

ifndef N64_INST
$(error N64_INST is not set. Example: wsl env N64_INST=/opt/libdragon make DEBUG=1)
endif

include $(N64_INST)/include/n64.mk

TFE_DIR := TheForceEngine
ANGELSCRIPT_INCLUDE_DIR := $(TFE_DIR)/TFE_ForceScript/Angelscript/angelscript/include

N64_CFLAGS += -I$(TFE_DIR) -I. -I$(ANGELSCRIPT_INCLUDE_DIR)
N64_CXXFLAGS += -I$(TFE_DIR) -I. -I$(ANGELSCRIPT_INCLUDE_DIR)
N64_CFLAGS += -DTFE_N64_LINK_DF_RUNTIME=$(TFE_N64_LINK_DF_RUNTIME)
N64_CXXFLAGS += -DTFE_N64_LINK_DF_RUNTIME=$(TFE_N64_LINK_DF_RUNTIME)
N64_CFLAGS += -DTFE_N64_LINK_MISSION_PROBE=$(TFE_N64_LINK_MISSION_PROBE)
N64_CXXFLAGS += -DTFE_N64_LINK_MISSION_PROBE=$(TFE_N64_LINK_MISSION_PROBE)
N64_CFLAGS += -DTFE_N64_DIRECT_MISSION_RENDER=$(TFE_N64_DIRECT_MISSION_RENDER)
N64_CXXFLAGS += -DTFE_N64_DIRECT_MISSION_RENDER=$(TFE_N64_DIRECT_MISSION_RENDER)

ifeq ($(DEBUG),1)
N64_CFLAGS += -DDF_DEBUG=1
N64_CXXFLAGS += -DDF_DEBUG=1
else
N64_CFLAGS += -DDF_DEBUG=0
N64_CXXFLAGS += -DDF_DEBUG=0
endif

OBJS = \
	$(BUILD_DIR)/n64/main.o \
	$(BUILD_DIR)/n64/mission_catalog.o \
	$(BUILD_DIR)/n64/runtime_handoff.o \
	$(BUILD_DIR)/n64/runtime_bridge.o \
	$(BUILD_DIR)/n64/tfe_darkforces_runtime_link.o \
	$(BUILD_DIR)/n64/tfe_input_shim.o \
	$(BUILD_DIR)/TheForceEngine/TFE_System/platformMain.o

ifeq ($(TFE_N64_LINK_DF_RUNTIME),1)
OBJS += \
	$(BUILD_DIR)/n64/tfe_darkforces_runtime_stubs.o \
	$(BUILD_DIR)/TheForceEngine/TFE_DarkForces/darkForcesMain.o \
	$(BUILD_DIR)/TheForceEngine/TFE_Jedi/Task/task.o \
	$(BUILD_DIR)/TheForceEngine/TFE_Jedi/InfSystem/infState.o
endif

ifeq ($(TFE_N64_LINK_MISSION_PROBE),1)
OBJS += \
	$(BUILD_DIR)/n64/tfe_mission_probe_shims.o \
	$(BUILD_DIR)/TheForceEngine/TFE_DarkForces/mission.o

ifeq ($(TFE_N64_DIRECT_MISSION_RENDER),1)
OBJS += \
	$(BUILD_DIR)/TheForceEngine/TFE_Jedi/Level/level.o \
	$(BUILD_DIR)/TheForceEngine/TFE_Jedi/Level/levelBin.o \
	$(BUILD_DIR)/TheForceEngine/TFE_Jedi/Level/levelData.o \
	$(BUILD_DIR)/TheForceEngine/TFE_Jedi/Level/levelTextures.o \
	$(BUILD_DIR)/TheForceEngine/TFE_Jedi/Memory/allocator.o \
	$(BUILD_DIR)/TheForceEngine/TFE_Jedi/Level/robjData.o \
	$(BUILD_DIR)/TheForceEngine/TFE_Jedi/Level/robject.o \
	$(BUILD_DIR)/TheForceEngine/TFE_Jedi/Level/rsector.o \
	$(BUILD_DIR)/TheForceEngine/TFE_Jedi/Level/rtexture.o \
	$(BUILD_DIR)/TheForceEngine/TFE_Jedi/Level/rwall.o \
	$(BUILD_DIR)/TheForceEngine/TFE_System/parser.o \
	$(BUILD_DIR)/TheForceEngine/TFE_Asset/dfKeywords.o \
	$(BUILD_DIR)/TheForceEngine/TFE_Jedi/Math/core_math.o \
	$(BUILD_DIR)/TheForceEngine/TFE_Jedi/Math/cosTable.o \
	$(BUILD_DIR)/TheForceEngine/TFE_Jedi/Renderer/rcommon.o \
	$(BUILD_DIR)/TheForceEngine/TFE_Jedi/Renderer/rscanline.o \
	$(BUILD_DIR)/TheForceEngine/TFE_Jedi/Renderer/rsectorRender.o \
	$(BUILD_DIR)/TheForceEngine/TFE_Jedi/Renderer/jediRenderer.o \
	$(BUILD_DIR)/TheForceEngine/TFE_Jedi/Renderer/RClassic_Fixed/rclassicFixed.o \
	$(BUILD_DIR)/TheForceEngine/TFE_Jedi/Renderer/RClassic_Fixed/rclassicFixedSharedState.o \
	$(BUILD_DIR)/TheForceEngine/TFE_Jedi/Renderer/RClassic_Fixed/redgePairFixed.o \
	$(BUILD_DIR)/TheForceEngine/TFE_Jedi/Renderer/RClassic_Fixed/rflatFixed.o \
	$(BUILD_DIR)/TheForceEngine/TFE_Jedi/Renderer/RClassic_Fixed/rlightingFixed.o \
	$(BUILD_DIR)/TheForceEngine/TFE_Jedi/Renderer/RClassic_Fixed/rsectorFixed.o \
	$(BUILD_DIR)/TheForceEngine/TFE_Jedi/Renderer/RClassic_Fixed/rwallFixed.o \
	$(BUILD_DIR)/TheForceEngine/TFE_Jedi/Renderer/RClassic_Fixed/robj3d_fixed/robj3dFixed.o \
	$(BUILD_DIR)/TheForceEngine/TFE_Jedi/Renderer/RClassic_Fixed/robj3d_fixed/robj3dFixed_Clipping.o \
	$(BUILD_DIR)/TheForceEngine/TFE_Jedi/Renderer/RClassic_Fixed/robj3d_fixed/robj3dFixed_Culling.o \
	$(BUILD_DIR)/TheForceEngine/TFE_Jedi/Renderer/RClassic_Fixed/robj3d_fixed/robj3dFixed_PolygonDraw.o \
	$(BUILD_DIR)/TheForceEngine/TFE_Jedi/Renderer/RClassic_Fixed/robj3d_fixed/robj3dFixed_PolygonSetup.o \
	$(BUILD_DIR)/TheForceEngine/TFE_Jedi/Renderer/RClassic_Fixed/robj3d_fixed/robj3dFixed_TransformAndLighting.o
endif
endif

DFS_FILE :=
ifneq ($(wildcard $(N64_MKDFS_ROOT)/*),)
DFS_FILE := $(BUILD_DIR)/$(ROM_NAME).dfs
endif

all: $(ROM_NAME).z64

$(ROM_NAME).z64: N64_ROM_TITLE = "Dark Forces N64"
$(ROM_NAME).z64: $(DFS_FILE)

$(BUILD_DIR)/$(ROM_NAME).elf: $(OBJS)

$(BUILD_DIR)/n64/main.o: n64/main.cpp
	@mkdir -p $(dir $@)
	@echo "    [CXX] $<"
	$(CXX) -c $(CXXFLAGS) -o $@ $<

$(BUILD_DIR)/n64/runtime_handoff.o: n64/runtime_handoff.cpp
	@mkdir -p $(dir $@)
	@echo "    [CXX] $<"
	$(CXX) -c $(CXXFLAGS) -o $@ $<

$(BUILD_DIR)/n64/mission_catalog.o: n64/mission_catalog.cpp
	@mkdir -p $(dir $@)
	@echo "    [CXX] $<"
	$(CXX) -c $(CXXFLAGS) -o $@ $<

$(BUILD_DIR)/n64/runtime_bridge.o: n64/runtime_bridge.cpp
	@mkdir -p $(dir $@)
	@echo "    [CXX] $<"
	$(CXX) -c $(CXXFLAGS) -o $@ $<

$(BUILD_DIR)/n64/tfe_darkforces_runtime_link.o: n64/tfe_darkforces_runtime_link.cpp
	@mkdir -p $(dir $@)
	@echo "    [CXX] $<"
	$(CXX) -c $(CXXFLAGS) -o $@ $<

$(BUILD_DIR)/n64/tfe_darkforces_runtime_stubs.o: n64/tfe_darkforces_runtime_stubs.cpp
	@mkdir -p $(dir $@)
	@echo "    [CXX] $<"
	$(CXX) -c $(CXXFLAGS) -o $@ $<

$(BUILD_DIR)/n64/tfe_input_shim.o: n64/tfe_input_shim.cpp
	@mkdir -p $(dir $@)
	@echo "    [CXX] $<"
	$(CXX) -c $(CXXFLAGS) -o $@ $<

$(BUILD_DIR)/n64/tfe_mission_probe_shims.o: n64/tfe_mission_probe_shims.cpp
	@mkdir -p $(dir $@)
	@echo "    [CXX] $<"
	$(CXX) -c $(CXXFLAGS) -Wno-error -o $@ $<

$(BUILD_DIR)/TheForceEngine/TFE_DarkForces/darkForcesMain.o: TheForceEngine/TFE_DarkForces/darkForcesMain.cpp
	@mkdir -p $(dir $@)
	@echo "    [CXX] $<"
	$(CXX) -c $(CXXFLAGS) -Wno-error -o $@ $<

$(BUILD_DIR)/TheForceEngine/TFE_DarkForces/mission.o: TheForceEngine/TFE_DarkForces/mission.cpp
	@mkdir -p $(dir $@)
	@echo "    [CXX] $<"
	$(CXX) -c $(CXXFLAGS) -Wno-error -o $@ $<

$(BUILD_DIR)/TheForceEngine/TFE_Jedi/Task/task.o: TheForceEngine/TFE_Jedi/Task/task.cpp
	@mkdir -p $(dir $@)
	@echo "    [CXX] $<"
	$(CXX) -c $(CXXFLAGS) -Wno-error -o $@ $<

$(BUILD_DIR)/TheForceEngine/TFE_Jedi/InfSystem/infState.o: TheForceEngine/TFE_Jedi/InfSystem/infState.cpp
	@mkdir -p $(dir $@)
	@echo "    [CXX] $<"
	$(CXX) -c $(CXXFLAGS) -Wno-error -o $@ $<

$(BUILD_DIR)/TheForceEngine/TFE_System/platformMain.o: TheForceEngine/TFE_System/platformMain.cpp
	@mkdir -p $(dir $@)
	@echo "    [CXX] $<"
	$(CXX) -c $(CXXFLAGS) -o $@ $<

$(BUILD_DIR)/TheForceEngine/%.o: TheForceEngine/%.cpp
	@mkdir -p $(dir $@)
	@echo "    [CXX] $<"
	$(CXX) -c $(CXXFLAGS) -Wno-error -o $@ $<

show-rom-sources:
	@echo "ROM sources:"
	@echo "  n64/main.cpp"
	@echo "  n64/mission_catalog.cpp"
	@echo "  n64/runtime_handoff.cpp"
	@echo "  n64/runtime_bridge.cpp"
	@echo "  n64/tfe_darkforces_runtime_link.cpp"
	@echo "  n64/tfe_input_shim.cpp"
	@echo "  TheForceEngine/TFE_System/platformMain.cpp"
	@if [ "$(TFE_N64_LINK_DF_RUNTIME)" = "1" ]; then \
		echo "  n64/tfe_darkforces_runtime_stubs.cpp"; \
		echo "  TheForceEngine/TFE_DarkForces/darkForcesMain.cpp"; \
		echo "  TheForceEngine/TFE_Jedi/Task/task.cpp"; \
		echo "  TheForceEngine/TFE_Jedi/InfSystem/infState.cpp"; \
	fi
	@if [ "$(TFE_N64_LINK_MISSION_PROBE)" = "1" ]; then \
		echo "  n64/tfe_mission_probe_shims.cpp"; \
		echo "  TheForceEngine/TFE_DarkForces/mission.cpp"; \
	fi

clean:
	rm -rf build build-runtime *.z64 *.v64 *.dfs *.sym *.stripped

.PHONY: all clean show-rom-sources

