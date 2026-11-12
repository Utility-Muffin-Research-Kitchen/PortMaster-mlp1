#define _GNU_SOURCE

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Part of leaf-port-shim.so (see pwm-guard.c for the other part).
 *
 * Leaf's launcher exports SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1 to every game
 * so a game keeps its controller while Leaf's volume and brightness toast holds
 * keyboard focus. SDL lets an environment variable outrank SDL_SetHint, so a
 * game that sets the same hint itself is refused, and some games treat that as
 * fatal (aurora-based ports such as Dusklight stop at window creation).
 *
 * For the hints listed in LEAF_PM_SDL_YIELD_HINTS (comma-separated), a game's
 * own SDL_SetHint call is applied at override priority whenever the variable is
 * in the environment. The game gets the value it asked for, as it would on a
 * system where Leaf never set the variable. Games that never set the hint keep
 * Leaf's value, and every other call passes through unchanged.
 *
 * SDL2 and SDL3 share these names, the argument order and the priority values.
 * Their return types differ (SDL_bool, an int, in SDL2; bool in SDL3), so only
 * the low byte of the real result is read and 0 or 1 is returned, which both
 * kinds of caller accept.
 */

#define LEAF_SDL_HINT_OVERRIDE 2

typedef int (*leaf_set_hint_fn)(const char *, const char *);
typedef int (*leaf_set_hint_priority_fn)(const char *, const char *, int);

static leaf_set_hint_fn real_SDL_SetHint;
static leaf_set_hint_priority_fn real_SDL_SetHintWithPriority;

static leaf_set_hint_fn leaf_resolve_set_hint(void)
{
    if (!real_SDL_SetHint) {
        union {
            void *object;
            leaf_set_hint_fn function;
        } symbol = { dlsym(RTLD_NEXT, "SDL_SetHint") };
        real_SDL_SetHint = symbol.function;
    }
    return real_SDL_SetHint;
}

static leaf_set_hint_priority_fn leaf_resolve_set_hint_priority(void)
{
    if (!real_SDL_SetHintWithPriority) {
        union {
            void *object;
            leaf_set_hint_priority_fn function;
        } symbol = { dlsym(RTLD_NEXT, "SDL_SetHintWithPriority") };
        real_SDL_SetHintWithPriority = symbol.function;
    }
    return real_SDL_SetHintWithPriority;
}

static int leaf_result(int result)
{
    return (result & 0xff) != 0;
}

static int leaf_yields(const char *name)
{
    if (!name || !name[0] || !getenv(name)) {
        return 0;
    }

    const char *item = getenv("LEAF_PM_SDL_YIELD_HINTS");
    if (!item) {
        return 0;
    }

    size_t name_len = strlen(name);
    while (*item) {
        const char *end = strchr(item, ',');
        size_t item_len = end ? (size_t)(end - item) : strlen(item);
        if (item_len == name_len && strncmp(item, name, name_len) == 0) {
            return 1;
        }
        if (!end) {
            break;
        }
        item = end + 1;
    }
    return 0;
}

static void leaf_log_yield(const char *name, const char *value)
{
    fprintf(stderr, "[leaf-port-shim] %s=%s set by the game takes priority over the environment\n",
            name, value ? value : "(reset)");
}

int SDL_SetHint(const char *name, const char *value)
{
    if (leaf_yields(name)) {
        leaf_set_hint_priority_fn set_hint_priority = leaf_resolve_set_hint_priority();
        if (set_hint_priority) {
            leaf_log_yield(name, value);
            return leaf_result(set_hint_priority(name, value, LEAF_SDL_HINT_OVERRIDE));
        }
    }

    leaf_set_hint_fn set_hint = leaf_resolve_set_hint();
    return set_hint ? leaf_result(set_hint(name, value)) : 0;
}

int SDL_SetHintWithPriority(const char *name, const char *value, int priority)
{
    leaf_set_hint_priority_fn set_hint_priority = leaf_resolve_set_hint_priority();
    if (!set_hint_priority) {
        return 0;
    }

    if (priority < LEAF_SDL_HINT_OVERRIDE && leaf_yields(name)) {
        leaf_log_yield(name, value);
        priority = LEAF_SDL_HINT_OVERRIDE;
    }
    return leaf_result(set_hint_priority(name, value, priority));
}
