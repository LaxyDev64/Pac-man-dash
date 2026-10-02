/*
 * pd_extra_imports.h / pd_extra_imports.c
 *
 * The 23 imports of PAC-MAN Dash!'s libmain.so that the CE DX dynlib.c table
 * does not cover:
 *
 *   SL_IID_EFFECTSEND, SL_IID_PLAYBACKRATE          (OpenSL ES interface IDs)
 *   _Unwind_* (11) and __gnu_uldivmod_helper        (libgcc, resolved to the host's)
 *   gettid, pthread_attr_getschedparam, pthread_attr_setschedpolicy
 *   glGetAttachedShaders                            (stub: reports 0 shaders)
 *   ov_clear, ov_open_callbacks, ov_pcm_seek, ov_pcm_total, ov_read
 *                                                   (Tremor API, served by libvorbisfile)
 *
 * dynlib.c integration:
 *   1. #include "pd_extra_imports.h" AFTER <SLES/OpenSLES.h>
 *   2. add the single line   PD_EXTRA_DYNLIB_ENTRIES   inside default_dynlib[]
 *   3. CMake: add source/pd_extra_imports.c (vorbisfile and ogg are already linked)
 *
 * If the linker complains about an _Unwind_* symbol, replace that entry by
 * { "name", (uintptr_t)&ret0 } in the macro below.
 * If it complains about SL_IID_EFFECTSEND / SL_IID_PLAYBACKRATE, your OpenSL ES
 * implementation does not define them yet: add them there.
 */
#ifndef PD_EXTRA_IMPORTS_H
#define PD_EXTRA_IMPORTS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* libgcc / unwinder symbols, declared as data like __cxa_throw in dynlib.c */
extern void *_Unwind_Complete;
extern void *_Unwind_DeleteException;
extern void *_Unwind_GetDataRelBase;
extern void *_Unwind_GetLanguageSpecificData;
extern void *_Unwind_GetRegionStart;
extern void *_Unwind_GetTextRelBase;
extern void *_Unwind_RaiseException;
extern void *_Unwind_Resume;
extern void *_Unwind_Resume_or_Rethrow;
extern void *_Unwind_VRS_Get;
extern void *_Unwind_VRS_Set;
extern void *__gnu_uldivmod_helper;

int  pd_gettid(void);
int  pd_pthread_attr_getschedparam(const void *attr, void *param);
int  pd_pthread_attr_setschedpolicy(void *attr, int policy);
void pd_glGetAttachedShaders(unsigned int program, int maxCount, int *count, unsigned int *shaders);

int       pd_ov_open_callbacks(void *datasource, void *vf, const char *initial, long ibytes,
                               /* ov_callbacks, 4 function pointers, passed by value */
                               void *cb_read, void *cb_seek, void *cb_close, void *cb_tell);
int       pd_ov_clear(void *vf);
long      pd_ov_read(void *vf, char *buffer, int length, int *bitstream);
long long pd_ov_pcm_total(void *vf, int i);
int       pd_ov_pcm_seek(void *vf, long long pos);

#define PD_EXTRA_DYNLIB_ENTRIES \
    { "SL_IID_EFFECTSEND",   (uintptr_t)&SL_IID_EFFECTSEND   }, \
    { "SL_IID_PLAYBACKRATE", (uintptr_t)&SL_IID_PLAYBACKRATE }, \
    { "_Unwind_Complete",                (uintptr_t)&_Unwind_Complete                }, \
    { "_Unwind_DeleteException",         (uintptr_t)&_Unwind_DeleteException         }, \
    { "_Unwind_GetDataRelBase",          (uintptr_t)&_Unwind_GetDataRelBase          }, \
    { "_Unwind_GetLanguageSpecificData", (uintptr_t)&_Unwind_GetLanguageSpecificData }, \
    { "_Unwind_GetRegionStart",          (uintptr_t)&_Unwind_GetRegionStart          }, \
    { "_Unwind_GetTextRelBase",          (uintptr_t)&_Unwind_GetTextRelBase          }, \
    { "_Unwind_RaiseException",          (uintptr_t)&_Unwind_RaiseException          }, \
    { "_Unwind_Resume",                  (uintptr_t)&_Unwind_Resume                  }, \
    { "_Unwind_Resume_or_Rethrow",       (uintptr_t)&_Unwind_Resume_or_Rethrow       }, \
    { "_Unwind_VRS_Get",                 (uintptr_t)&_Unwind_VRS_Get                 }, \
    { "_Unwind_VRS_Set",                 (uintptr_t)&_Unwind_VRS_Set                 }, \
    { "__gnu_uldivmod_helper",           (uintptr_t)&__gnu_uldivmod_helper           }, \
    { "gettid",                          (uintptr_t)&pd_gettid                       }, \
    { "pthread_attr_getschedparam",      (uintptr_t)&pd_pthread_attr_getschedparam   }, \
    { "pthread_attr_setschedpolicy",     (uintptr_t)&pd_pthread_attr_setschedpolicy  }, \
    { "glGetAttachedShaders",            (uintptr_t)&pd_glGetAttachedShaders         }, \
    { "ov_open_callbacks",               (uintptr_t)&pd_ov_open_callbacks            }, \
    { "ov_clear",                        (uintptr_t)&pd_ov_clear                     }, \
    { "ov_read",                         (uintptr_t)&pd_ov_read                      }, \
    { "ov_pcm_total",                    (uintptr_t)&pd_ov_pcm_total                 }, \
    { "ov_pcm_seek",                     (uintptr_t)&pd_ov_pcm_seek                  },

#ifdef __cplusplus
}
#endif

#endif
