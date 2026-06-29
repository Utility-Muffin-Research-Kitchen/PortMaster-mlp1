SHELL := /bin/bash

APP_ID := portmaster-mlp1
PAK_NAME := PortMaster
VERSION := 0.1.1
BUILD ?= build
PLATFORM ?= mac
WORKSPACE_ROOT ?= $(abspath ..)
MLP1_TOOLCHAIN_IMAGE ?= ghcr.io/utility-muffin-research-kitchen/mlp1-toolchain:latest

CC ?= cc
CSTD := -std=gnu11
CWARN := -Wall -Wextra -Wpedantic -Wno-unused-parameter
CDEBUG ?= -g -O0

DEFAULT_CATASTROPHE_DIR := $(if $(wildcard ../Catastrophe/include/catastrophe.h),$(abspath ../Catastrophe),third_party/catastrophe)
CATASTROPHE_DIR ?= $(DEFAULT_CATASTROPHE_DIR)
CATASTROPHE_INCLUDE := $(CATASTROPHE_DIR)/include
CATASTROPHE_RES := $(CATASTROPHE_DIR)/res

DEFAULT_CJSON_DIR := $(if $(wildcard ../Jawaka/third_party/cjson/cJSON.c),$(abspath ../Jawaka/third_party/cjson),third_party/cjson)
CJSON_DIR ?= $(DEFAULT_CJSON_DIR)
CJSON_SRC := $(CJSON_DIR)/cJSON.c

SDL_CFLAGS := $(shell pkg-config --cflags sdl2 SDL2_ttf SDL2_image)
SDL_LDFLAGS := $(shell pkg-config --libs sdl2 SDL2_ttf SDL2_image)
CURL_CFLAGS := $(shell pkg-config --cflags libcurl 2>/dev/null)
CURL_LDFLAGS := $(shell pkg-config --libs libcurl 2>/dev/null)
ifneq ($(PLATFORM),mlp1)
ifeq ($(strip $(CURL_LDFLAGS)),)
CURL_LDFLAGS := -lcurl
endif
endif

CFLAGS_PLATFORM :=
ifeq ($(PLATFORM),mlp1)
CFLAGS_PLATFORM += -DPLATFORM_MLP1
ifeq ($(strip $(CURL_LDFLAGS)),)
$(error PLATFORM=mlp1 requires libcurl in the toolchain)
endif
endif

SRC := $(wildcard src/*.c)
APP_BIN := $(BUILD)/bin/$(APP_ID)
MLP1_BUILD := build/mlp1
MLP1_BIN := $(MLP1_BUILD)/bin/$(APP_ID)
PACKAGE_ROOT := $(BUILD)/package
PACKAGE_DIR := $(PACKAGE_ROOT)/$(PAK_NAME).pak
MLP1_PACKAGE := $(MLP1_BUILD)/package/$(PAK_NAME).pak
PAKRAT_ZIP := $(MLP1_BUILD)/$(PAK_NAME).mlp1.pak.zip

CFLAGS_COMMON := $(CSTD) $(CWARN) $(CDEBUG) $(CFLAGS_PLATFORM) \
	-DPM_VERSION=\"$(VERSION)\" \
	-Isrc -I$(CATASTROPHE_INCLUDE) -I$(CJSON_DIR) $(SDL_CFLAGS) $(CURL_CFLAGS)
LDLIBS_COMMON := $(SDL_LDFLAGS) $(CURL_LDFLAGS) -lm -lpthread
ifeq ($(shell uname -s),Darwin)
LDLIBS_COMMON += -lobjc
endif

.PHONY: all native run-native mlp1 package package-build package-mlp1 package-platform dist-pakrat local-pakrat-feed pakrat-local-smoke fetch-ui-runtime-sources build-ui-runtime-reference build-ui-runtime-cpython spruce-bin-closure clean

all: native

native: $(APP_BIN)

$(APP_BIN): $(SRC) $(CJSON_SRC)
	@mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS_COMMON) -o "$@" $(SRC) $(CJSON_SRC) $(LDLIBS_COMMON)

run-native: native
	CAT_WINDOW_WIDTH=960 CAT_WINDOW_HEIGHT=720 \
	CAT_FONTS_DIR="$(CATASTROPHE_RES)" \
	CAT_THEMES_DIR="$(CATASTROPHE_RES)/themes" \
	PORTMASTER_MLP1_PAK_DIR="$(CURDIR)" \
	"$(APP_BIN)"

mlp1:
	@VERSION="$(VERSION)" ./scripts/build-mlp1.sh

package: native
	@$(MAKE) BUILD="$(BUILD)" PLATFORM="$(PLATFORM)" BIN="$(APP_BIN)" package-build

package-build:
	@test -n "$(BIN)" || { echo "BIN is required" >&2; exit 1; }
	@rm -rf "$(PACKAGE_ROOT)"
	@mkdir -p "$(PACKAGE_DIR)/bin" "$(PACKAGE_DIR)/locks" "$(PACKAGE_DIR)/scripts" \
		"$(PACKAGE_DIR)/patches/portmaster-gui/mlp1" \
		"$(PACKAGE_DIR)/overlays/portmaster-gui/mlp1" \
		"$(PACKAGE_DIR)/compat/armhf" "$(PACKAGE_DIR)/LICENSES"
	@cp -f "$(BIN)" "$(PACKAGE_DIR)/bin/$(APP_ID)"
	@cp -f pak/launch.sh pak/pak.json "$(PACKAGE_DIR)/"
	@cp -f locks/*.json "$(PACKAGE_DIR)/locks/"
	@cp -f scripts/*.sh "$(PACKAGE_DIR)/scripts/"
	@cp -f patches/portmaster-gui/mlp1/*.patch "$(PACKAGE_DIR)/patches/portmaster-gui/mlp1/" 2>/dev/null || true
	@cp -f overlays/portmaster-gui/mlp1/* "$(PACKAGE_DIR)/overlays/portmaster-gui/mlp1/" 2>/dev/null || true
	@cp -f compat/armhf/* "$(PACKAGE_DIR)/compat/armhf/" 2>/dev/null || true
	@if [ -f LICENSE ]; then cp -f LICENSE "$(PACKAGE_DIR)/LICENSE"; fi
	@if [ -d LICENSES ]; then cp -Rf LICENSES/. "$(PACKAGE_DIR)/LICENSES/"; fi
	@chmod 755 "$(PACKAGE_DIR)/launch.sh" "$(PACKAGE_DIR)/bin/$(APP_ID)" "$(PACKAGE_DIR)"/scripts/*.sh
	@find "$(PACKAGE_DIR)" -type f | sort

package-mlp1: mlp1
	@$(MAKE) BUILD="$(MLP1_BUILD)" PLATFORM=mlp1 BIN="$(MLP1_BIN)" package-build

package-platform:
	@test -n "$(PLATFORM)" || { echo "usage: make package-platform PLATFORM=<platform>" >&2; exit 1; }
	@case "$(PLATFORM)" in \
		mlp1) $(MAKE) package-mlp1 ;; \
		mac) $(MAKE) package ;; \
		*) echo "unsupported PortMaster package platform: $(PLATFORM)" >&2; exit 1 ;; \
	esac

dist-pakrat: package-mlp1
	@rm -f "$(PAKRAT_ZIP)"
	@cd "$(MLP1_BUILD)/package" && zip -qr "../$(PAK_NAME).mlp1.pak.zip" "$(PAK_NAME).pak"
	@shasum -a 256 "$(PAKRAT_ZIP)"
	@echo "=== Pak Rat archive: $(PAKRAT_ZIP) ==="

local-pakrat-feed: dist-pakrat
	@tools/make-local-pakrat-feed.sh --skip-build

pakrat-local-smoke:
	@tools/pakrat-local-smoke.sh

fetch-ui-runtime-sources:
	@sh scripts/fetch-ui-runtime-sources.sh

build-ui-runtime-reference:
	@sh scripts/build-ui-runtime-reference.sh

build-ui-runtime-cpython:
	@bash scripts/build-ui-runtime-cpython.sh

spruce-bin-closure:
	@python3 scripts/generate-spruce-bin-closure.py

clean:
	rm -rf build dist ports/mlp1/pak/bin
