// Per-title differences, in one place.
//
// The runtime is shared by more than one game; what differs between them is data and behaviour that belongs to the
// title, not to the port: how its SDK is entered, which kernel behaviour that SDK expects, and which image its
// extraction produced. Keeping those behind this interface is what lets the shared files stay close to upstream: a
// change there touches one call site, not every title's copy of the logic.
//
// The target is chosen at build time (tools/build_game.py --target), because the build already produces one binary
// per title: build/<version>/totk. Everything else is shared code.
#pragma once

#include <stdint.h>

#include "cpu.h"

// sdk_flags
#define RT_SDK_USER_CRITICAL_SECTIONS 1u   // the SDK takes its critical sections in user mode (Ryza 2 and older): the
                                           // kernel has to own them, or a condition variable cannot release and hand off

// A mount prefix the title uses for a RomFS folder of its own: "DISC:" is the path it opens its data through, and the
// port maps it onto the RomFS folder that data lives in.
typedef struct { const char *prefix; const char *romfs_dir; } RTMountAlias;

// A module's constructor count, the sanity check that the ExeFS is the game the build was made for.
typedef struct { const char *name; uint64_t entries; } RTInitCount;

// Function-level hook entry: replace an internal guest function at module+offset with a host GuestFn.
typedef struct RTFuncHook {
    const char *module;
    uint64_t offset;
    GuestFn hook;
    GuestFn *orig_out;   // if non-NULL, receives the original lifted function pointer
} RTFuncHook;

typedef struct RTTarget {
    const char *name;
    // The guest main thread's first call: how this title's SDK is entered. The newest SDK goes through
    // nn::init::Start, which runs every module's constructor and then nnMain; older titles call nnosInitialize and
    // reach nnMain from their own entry.
    void (*guest_main)(Cpu *c);
    unsigned sdk_flags;
    const char *romfs_image_name;
    // The title's own mount prefixes into the RomFS, terminated by a NULL prefix; NULL when it uses "rom:" only.
    const RTMountAlias *mount_aliases;
    // The application pool this title needs, when it is larger than the retail default; 0 keeps the default.
    uint64_t pool_min;
    // The constructor counts this title's modules have; NULL keeps the shared tables for the TotK versions.
    const RTInitCount *init_counts;
    int init_count_n;
    // Present interval override: 0 = follow game (default), 1 = force 60 Hz interval, etc.
    int present_interval_override;
    // Function hooks: replaces indirect dispatch table slots at boot time without instruction patching.
    const RTFuncHook *func_hooks;
    int func_hook_n;
    // Target engine defaults:
    int split_draws_off;
    int feedback_off;
    int free_cores;
    int frame_queue;
    int replay_async_off;
    const char *vk_present;
} RTTarget;

const RTTarget *rt_target(void);
