/* The addon host's shared declarations (host/addons/).
 *
 * The host runs Lua addons written for our own API (xi.*), for Ashita v4 and for Windower 4, in one
 * LuaJIT state each, on the game thread with the guest lock held. This header is what the
 * pieces share.
 *
 * Addresses: the guest's are 32-bit (guest.h, rd32/wr32). Lua sees host addresses
 * (rt_guest_base + guest) so that `ffi.cast` on them works; xi_guest_addr turns either kind back
 * into a guest address. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "guest.h"
#include "runtime.h"

#if defined(_MSC_VER)
#include <string.h>
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* --- logging --------------------------------------------------------------------------------- */

/* Into host64.log, prefixed "[addons] ". */
void xi_log(const char* fmt, ...);
/* The same, once per distinct key for the life of the process (repeating errors). */
void xi_log_once(const char* key, const char* fmt, ...);

/* --- guest memory ---------------------------------------------------------------------------- */

/* Whether [a, a+n) is committed guest memory (reads there are safe). */
int xi_mapped(uint32_t a, uint32_t n);
/* A number Lua passed as an address: < 2^32 is a guest address; inside the guest window, a host
 * address. Returns 0 (and *ok = 0) for anything else. */
uint32_t xi_guest_addr(double v, int* ok);
/* The host address Lua sees for a guest address (0 stays 0). */
double xi_host_addr(uint32_t guest);

/* FFXiMain's image: its guest range and its code (the unpacked .text, for pattern scans). */
void xi_image(uint32_t* base, uint32_t* size, uint32_t* text, uint32_t* text_size);
/* Ashita-style pattern ("8B 0D ?? ?? ?? ?? 85 C9", or "8B0D????????85C9"; ?? any byte) in
 * [start, start+size): the address of match number `count` (0-based) plus `offset`, or 0. */
uint32_t xi_find_pattern(uint32_t start, uint32_t size, const char* pattern, int32_t offset, uint32_t count);

/* --- paths ----------------------------------------------------------------------------------- */

/* The data dir (~/Library/Application Support/FFXIRecompile/FFXI/ on macOS), with a trailing '/'. */
const char* xi_data_dir(void);
/* The game's install folder on the host (the retail "FINAL FANTASY XI" folder), trailing '/'. */
const char* xi_game_dir(void);
/* A Windows-shaped or host path from an addon made into a host path (slashes, the Ashita and
 * Windower install prefixes, case-insensitive lookup of each component that doesn't exist as
 * written). Writes out; returns out. */
char* xi_host_path(const char* in, char* out, size_t n);

/* --- the game -------------------------------------------------------------------------------- */

/* Write a line to the chat log in chat mode `mode` (Shift-JIS with FFXI's codes). Lines written
 * before the chat log exists wait for it. */
void xi_chat_write(int mode, const char* text);
/* Run a line as if typed (`/echo hi`, `//addon load x`), on the next frame (injected = 1). */
void xi_chat_queue(int mode, const char* line);

#ifdef __cplusplus
}
#endif
