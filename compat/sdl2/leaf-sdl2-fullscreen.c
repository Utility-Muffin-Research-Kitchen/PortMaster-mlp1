#define _GNU_SOURCE

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

typedef unsigned int Uint32;
typedef struct SDL_Window SDL_Window;

#define SDL_WINDOW_FULLSCREEN 0x00000001u
#define SDL_WINDOW_FULLSCREEN_DESKTOP (SDL_WINDOW_FULLSCREEN | 0x00001000u)

static SDL_Window *(*real_SDL_CreateWindow)(const char *, int, int, int, int, Uint32);
static int (*real_SDL_SetWindowFullscreen)(SDL_Window *, Uint32);
static void (*real_SDL_SetWindowSize)(SDL_Window *, int, int);

static int leaf_enabled(void)
{
    const char *value = getenv("LEAF_PM_SDL_FORCE_FULLSCREEN");
    return value && value[0] && value[0] != '0';
}

static int leaf_env_int(const char *name, int fallback)
{
    const char *value = getenv(name);
    if (!value || !value[0]) {
        return fallback;
    }

    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value || parsed <= 0 || parsed > 16384) {
        return fallback;
    }
    return (int)parsed;
}

static int leaf_target_width(void)
{
    return leaf_env_int("LEAF_PM_SDL_FULLSCREEN_WIDTH", leaf_env_int("DISPLAY_WIDTH", 960));
}

static int leaf_target_height(void)
{
    return leaf_env_int("LEAF_PM_SDL_FULLSCREEN_HEIGHT", leaf_env_int("DISPLAY_HEIGHT", 720));
}

static void leaf_resolve_symbols(void)
{
    if (!real_SDL_CreateWindow) {
        union {
            void *object;
            SDL_Window *(*function)(const char *, int, int, int, int, Uint32);
        } symbol = { dlsym(RTLD_NEXT, "SDL_CreateWindow") };
        real_SDL_CreateWindow = symbol.function;
    }
    if (!real_SDL_SetWindowFullscreen) {
        union {
            void *object;
            int (*function)(SDL_Window *, Uint32);
        } symbol = { dlsym(RTLD_NEXT, "SDL_SetWindowFullscreen") };
        real_SDL_SetWindowFullscreen = symbol.function;
    }
    if (!real_SDL_SetWindowSize) {
        union {
            void *object;
            void (*function)(SDL_Window *, int, int);
        } symbol = { dlsym(RTLD_NEXT, "SDL_SetWindowSize") };
        real_SDL_SetWindowSize = symbol.function;
    }
}

__attribute__((constructor)) static void leaf_sdl2_fullscreen_ctor(void)
{
    fprintf(stderr, "[leaf-sdl2-fullscreen] loaded enabled=%d target=%dx%d\n",
            leaf_enabled(), leaf_target_width(), leaf_target_height());
}

SDL_Window *SDL_CreateWindow(const char *title, int x, int y, int w, int h, Uint32 flags)
{
    leaf_resolve_symbols();
    if (!real_SDL_CreateWindow) {
        fprintf(stderr, "[leaf-sdl2-fullscreen] SDL_CreateWindow missing\n");
        return NULL;
    }

    if (!leaf_enabled()) {
        return real_SDL_CreateWindow(title, x, y, w, h, flags);
    }

    int target_w = leaf_target_width();
    int target_h = leaf_target_height();
    Uint32 forced_flags = flags | SDL_WINDOW_FULLSCREEN_DESKTOP;

    fprintf(stderr,
            "[leaf-sdl2-fullscreen] SDL_CreateWindow requested=%dx%d flags=0x%x forcing=%dx%d flags=0x%x\n",
            w, h, flags, target_w, target_h, forced_flags);

    SDL_Window *window = real_SDL_CreateWindow(title, x, y, target_w, target_h, forced_flags);
    if (window && real_SDL_SetWindowSize) {
        real_SDL_SetWindowSize(window, target_w, target_h);
    }
    if (window && real_SDL_SetWindowFullscreen) {
        real_SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
    }
    return window;
}

int SDL_SetWindowFullscreen(SDL_Window *window, Uint32 flags)
{
    leaf_resolve_symbols();
    if (!real_SDL_SetWindowFullscreen) {
        return -1;
    }
    if (leaf_enabled()) {
        flags = SDL_WINDOW_FULLSCREEN_DESKTOP;
        fprintf(stderr, "[leaf-sdl2-fullscreen] SDL_SetWindowFullscreen forcing flags=0x%x\n", flags);
    }
    return real_SDL_SetWindowFullscreen(window, flags);
}

void SDL_SetWindowSize(SDL_Window *window, int w, int h)
{
    leaf_resolve_symbols();
    if (!real_SDL_SetWindowSize) {
        return;
    }
    if (leaf_enabled()) {
        w = leaf_target_width();
        h = leaf_target_height();
        fprintf(stderr, "[leaf-sdl2-fullscreen] SDL_SetWindowSize forcing=%dx%d\n", w, h);
    }
    real_SDL_SetWindowSize(window, w, h);
}
