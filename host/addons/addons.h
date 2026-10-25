/* The addon host's interface to host64 (host/addons/). */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AddonsSetup
{
    const char* data_dir; /* where ashita/, windower/ and xi/ live (created if missing) */
    const char* game_dir; /* the game's install folder on the host */
    const char* const* dat_overlays; /* DAT overlay folders, first wins (resources read through them) */
    unsigned ndat_overlays;
} AddonsSetup;

/* Before the game starts: installs the wraps (command line, chat log, packets) and the input and
 * overlay hooks. Nothing runs until the first frame. */
void addons_init(const AddonsSetup* s);
/* Every frame, from the D3D8 Present hook (game thread, guest lock held). */
void addons_frame(void);
/* The game is ending (its own /shutdown, the window closing): every addon's unload, once. */
void addons_shutdown(void);
/* Config > Addons (modern.c): the installed addons, switched on and off from the game's menu. */
const struct ModernAddons* addons_menu(void);
/* --addon-harness: runs a script of addon loads, commands, packets and frames with the game's image
 * mapped but the game not started (no window, no GPU), printing chat lines to stdout. The exit code. */
int addons_harness(const char* script);

#ifdef __cplusplus
}
#endif
