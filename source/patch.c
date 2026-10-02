/*
 * patch.c
 *
 * Runtime patches for PAC-MAN Dash!'s libmain.so (minimal first-build version).
 *
 * The CE DX patches (offsets, FMOD hooks, course/lock hooks) do not apply to
 * this library and have been removed. Add Dash-specific patches here as the
 * first runs show what is needed. The Squirrel script hooks live in sq_trace.c.
 *
 * so_patch() is called by the loader after the module is relocated.
 */

#include <kubridge.h>
#include <so_util/so_util.h>
#include <stdint.h>

#include "utils/logger.h"

extern so_module so_mod;

void so_patch(void) {
    l_info("so_patch: no Dash-specific patches yet (text_base=0x%08X)",
           (unsigned)so_mod.text_base);
}
