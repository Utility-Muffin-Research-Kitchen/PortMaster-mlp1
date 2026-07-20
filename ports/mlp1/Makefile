SHELL := /bin/bash

TC       := /opt/mlp1-toolchain
CC       := $(TC)/bin/aarch64-buildroot-linux-gnu-gcc
SYSROOT  := $(TC)/aarch64-buildroot-linux-gnu/sysroot
APP      ?= /workspace/PortMaster-mlp1
CATA     ?= /workspace/Catastrophe
CJSON    ?= /workspace/Jawaka/third_party/cjson
OUT      := $(APP)/build/mlp1/bin/portmaster-mlp1
EGL_SHIM := $(APP)/build/mlp1/compat/egl/aarch64/libEGL.so.1
SRC      := $(wildcard $(APP)/src/*.c)
VERSION  ?= 0.2.0

PKG_CONFIG_SYSROOT_DIR := $(SYSROOT)
PKG_CONFIG_LIBDIR := $(SYSROOT)/usr/lib/pkgconfig:$(SYSROOT)/usr/share/pkgconfig
export PKG_CONFIG_SYSROOT_DIR PKG_CONFIG_LIBDIR

CURL_CFLAGS  := $(shell pkg-config --cflags libcurl 2>/dev/null)
CURL_LDFLAGS := $(shell pkg-config --libs libcurl 2>/dev/null)

CFLAGS  := -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -DPLATFORM_MLP1 \
           -DPM_VERSION=\"$(VERSION)\" \
           -I$(APP)/src -I$(CATA)/include -I$(CJSON) \
           -I$(SYSROOT)/usr/include -I$(SYSROOT)/usr/include/SDL2 $(CURL_CFLAGS)
LDFLAGS := -L$(SYSROOT)/usr/lib -lSDL2 -lSDL2_ttf -lSDL2_image \
           $(CURL_LDFLAGS) -lGLESv2 -lEGL -lm -lpthread

.PHONY: all clean
all: $(OUT) $(EGL_SHIM)

$(OUT): $(SRC) $(CJSON)/cJSON.c Makefile
	mkdir -p $(dir $(OUT))
	$(CC) $(CFLAGS) -o $(OUT) $(SRC) $(CJSON)/cJSON.c $(LDFLAGS)

$(EGL_SHIM): $(APP)/compat/egl/leaf-egl-gles-shim.c
	mkdir -p $(dir $(EGL_SHIM))
	$(CC) -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter \
		-I$(SYSROOT)/usr/include -shared -fPIC \
		-Wl,-soname,libEGL.so.1 -o $(EGL_SHIM) $< -ldl
	cp -f $(EGL_SHIM) $(dir $(EGL_SHIM))/libEGL.so

clean:
	rm -f $(OUT) $(EGL_SHIM) $(dir $(EGL_SHIM))/libEGL.so
