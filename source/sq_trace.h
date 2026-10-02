/*
 * sq_trace.h
 *
 * Squirrel script tracer / patcher for PAC-MAN Dash!.
 *
 * libmain.so statically links Squirrel and exports all sq_* functions, and the
 * game scripts (the .nut.m files in assets/anddata/script) are encrypted on disk. The game
 * has to decrypt them before handing them to Squirrel, so hooking the compile
 * entry points gives us the plain scripts:
 *
 *   sq_compilebuffer  -> dumps the source text, can patch it before compiling
 *   sq_readclosure    -> dumps precompiled bytecode (if the game uses that path)
 *   sq_compile        -> dumps text coming from a lexer callback
 *
 * Dumps go to ux0:data/pacmandash/dump/ and every call is logged with l_info.
 */
#ifndef SQ_TRACE_H
#define SQ_TRACE_H

#include <so_util/so_util.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Call after soloader_init_all() (module relocated) and before the game starts. */
void sq_trace_init(so_module *mod);

#ifdef __cplusplus
}
#endif

#endif
