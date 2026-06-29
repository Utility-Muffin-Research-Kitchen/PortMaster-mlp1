SHELL := /bin/bash

TC       := /opt/mlp1-toolchain
CC       := $(TC)/bin/aarch64-buildroot-linux-gnu-gcc
SYSROOT  := $(TC)/aarch64-buildroot-linux-gnu/sysroot
APP      ?= /workspace/PortMaster-mlp1
CATA     ?= /workspace/Catastrophe
CJSON    ?= /workspace/Jawaka/third_party/cjson
OUT      := $(APP)/build/mlp1/bin/portmaster-mlp1
SRC      := $(wildcard $(APP)/src/*.c)

PKG_CONFIG_SYSROOT_DIR := $(SYSROOT)
PKG_CONFIG_LIBDIR := $(SYSROOT)/usr/lib/pkgconfig:$(SYSROOT)/usr/share/pkgconfig
export PKG_CONFIG_SYSROOT_DIR PKG_CONFIG_LIBDIR

CURL_CFLAGS  := $(shell pkg-config --cflags libcurl 2>/dev/null)
CURL_LDFLAGS := $(shell pkg-config --libs libcurl 2>/dev/null)

CFLAGS  := -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -DPLATFORM_MLP1 \
           -DPM_VERSION=\"0.1.0\" \
           -I$(APP)/src -I$(CATA)/include -I$(CJSON) \
           -I$(SYSROOT)/usr/include -I$(SYSROOT)/usr/include/SDL2 $(CURL_CFLAGS)
LDFLAGS := -L$(SYSROOT)/usr/lib -lSDL2 -lSDL2_ttf -lSDL2_image \
           $(CURL_LDFLAGS) -lGLESv2 -lEGL -lm -lpthread

.PHONY: all clean
all:
	mkdir -p $(dir $(OUT))
	$(CC) $(CFLAGS) -o $(OUT) $(SRC) $(CJSON)/cJSON.c $(LDFLAGS)

clean:
	rm -f $(OUT)

