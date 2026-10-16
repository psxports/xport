XPORT_WEB_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
PROJECT_WEB_MAKEFILE := $(firstword $(MAKEFILE_LIST))
XPORT_ROOT := $(abspath $(XPORT_WEB_DIR)/../../..)
EMSDK_ROOT := $(XPORT_ROOT)/tools/emsdk
EMSDK_PYTHON := $(EMSDK_ROOT)/python/3.13.3_64bit/python.exe
EMCC := $(EMSDK_PYTHON) $(EMSDK_ROOT)/upstream/emscripten/emcc.py --em-config $(EMSDK_ROOT)/.emscripten
WEB_PACKAGER := $(XPORT_ROOT)/tools/web_packager.py
WEB_PACKAGER_FLAGS ?=
WEB_EXCLUDE_FILES ?=
DIST_DIR ?= ../../../../.github/site/$(SHORT_NAME)
OUTPUT := $(DIST_DIR)/xport-runtime.js

XPORT_SOURCES := \
	$(XPORT_WEB_DIR)/main.c \
	$(XPORT_ROOT)/src/psx.c \
	$(XPORT_ROOT)/src/psx_gpu.c \
	$(XPORT_ROOT)/src/vid_ogl.c \
	$(XPORT_ROOT)/src/psx_font.c \
	$(XPORT_ROOT)/src/psx_spu.c \
	$(XPORT_ROOT)/src/psx_xa.c \
	$(XPORT_ROOT)/src/psx_mdec.c \
	$(XPORT_ROOT)/src/psx_press.c \
	$(XPORT_ROOT)/src/psx_vlc.c \
	$(XPORT_ROOT)/src/psx_stream.c \
	$(XPORT_ROOT)/src/xport_trace.c
SOURCES := $(XPORT_SOURCES) $(APP_SOURCES)
OBJ_DIR := ../../../_build/web/$(CONFIG)
LINK_RESPONSE := $(OBJ_DIR)/link.rsp
object_for = $(OBJ_DIR)/$(subst :,_,$(subst /,_,$(basename $(1)))).o
dep_for = $(patsubst %.o,%.d,$(call object_for,$(1)))
OBJECTS := $(foreach source,$(SOURCES),$(call object_for,$(source)))
DEPFILES := $(foreach source,$(SOURCES),$(call dep_for,$(source)))

COMMON_FLAGS := -std=c11 -fdeclspec -fno-strict-aliasing -fwrapv -Wno-error=incompatible-pointer-types -Wno-error=incompatible-function-pointer-types
COMMON_FLAGS += -Wno-error=implicit-function-declaration -Wno-error=int-conversion -Wno-ignored-attributes -Wno-incompatible-library-redeclaration
COMMON_FLAGS += -DAP_WEB=1 -DGAPI_OGL -DXPORT_NATIVE -I$(PROJECT_ROOT)/src -I$(XPORT_ROOT)/src $(APP_DEFINES)
COMMON_FLAGS += -DSHORT_NAME=\"$(SHORT_NAME)\"
LINK_FLAGS := -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 -sASYNCIFY -sEMULATE_FUNCTION_POINTER_CASTS -sALLOW_MEMORY_GROWTH=1 -sFORCE_FILESYSTEM=1 -sEXIT_RUNTIME=0 -sENVIRONMENT=web
LINK_FLAGS += -sEXPORTED_RUNTIME_METHODS=callMain,FS,addRunDependency,removeRunDependency

ifeq ($(CONFIG),Debug)
BUILD_FLAGS := -O0 -gsource-map -D_DEBUG -sASSERTIONS=2
else
BUILD_FLAGS := -O3 -DNDEBUG
endif

.PHONY: all clean packages

all: $(OUTPUT) packages

$(DIST_DIR):
	mkdir -p $(DIST_DIR)

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

define compile_rule
$(call object_for,$(1)): $(1) $(XPORT_WEB_DIR)/Makefile $(PROJECT_WEB_MAKEFILE) | $(OBJ_DIR)
	$$(EMCC) $$(COMMON_FLAGS) $$(BUILD_FLAGS) -MMD -MP -MF $(call dep_for,$(1)) -c $(1) -o $$@
endef

$(foreach source,$(SOURCES),$(eval $(call compile_rule,$(source))))

-include $(DEPFILES)

$(OUTPUT): $(OBJECTS) | $(DIST_DIR)
	$(file >$(LINK_RESPONSE),$(OBJECTS))
	$(EMCC) $(filter-out -x c++,$(COMMON_FLAGS)) $(BUILD_FLAGS) $(LINK_FLAGS) @$(LINK_RESPONSE) -o $(OUTPUT)

packages: $(OUTPUT) $(XPORT_WEB_DIR)/index.html | $(DIST_DIR)
	$(EMSDK_PYTHON) $(WEB_PACKAGER) --project $(PROJECT_ROOT) --output $(DIST_DIR) --short-name $(SHORT_NAME) --full-name "$(FULL_NAME)" --index-template $(XPORT_WEB_DIR)/index.html $(foreach pattern,$(WEB_EXCLUDE_FILES),--exclude "$(pattern)") $(WEB_PACKAGER_FLAGS)

clean:
	rm -f $(OBJECTS) $(DEPFILES) $(LINK_RESPONSE) $(DIST_DIR)/index.html $(DIST_DIR)/xport.js $(DIST_DIR)/xport.wasm $(DIST_DIR)/xport.wasm.map $(DIST_DIR)/xport.data $(DIST_DIR)/xport.data.js $(DIST_DIR)/xport-*.data $(DIST_DIR)/xport-*.js $(DIST_DIR)/xport-*.wasm $(DIST_DIR)/xport-*.wasm.map
