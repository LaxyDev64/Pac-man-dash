/*
 * pd_extra_imports.c - see pd_extra_imports.h
 *
 * Vorbis note: the game imports the Tremor (libvorbisidec) API, but the project
 * already links the full libvorbisfile. Same function names, two differences:
 *   - ov_read: Tremor has 4 args and always returns 16-bit signed little-endian,
 *     libvorbisfile has 7 -> we call it with (bigendian=0, word=2, sgned=1).
 *   - OggVorbis_File: the game allocated its struct for Tremor's size, so the real
 *     libvorbisfile struct is allocated on the host side and the first word of the
 *     game's struct stores the pointer to it. The game only passes the struct back
 *     to ov_*, so this is transparent.
 */
#include "pd_extra_imports.h"

#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/threadmgr.h>

#include <vorbis/vorbisfile.h>

int pd_gettid(void) {
    return (int)sceKernelGetThreadId();
}

int pd_pthread_attr_getschedparam(const void *attr, void *param) {
    (void)attr;
    if (param) memset(param, 0, sizeof(int));   /* sched_param { int sched_priority; } */
    return 0;
}

int pd_pthread_attr_setschedpolicy(void *attr, int policy) {
    (void)attr; (void)policy;
    return 0;
}

void pd_glGetAttachedShaders(unsigned int program, int maxCount, int *count, unsigned int *shaders) {
    (void)program; (void)maxCount; (void)shaders;
    if (count) *count = 0;
}

/* ---- Vorbis proxy ---- */
static OggVorbis_File *real_vf(void *vf) {
    return vf ? *(OggVorbis_File **)vf : NULL;
}

int pd_ov_open_callbacks(void *datasource, void *vf, const char *initial, long ibytes,
                         void *cb_read, void *cb_seek, void *cb_close, void *cb_tell) {
    ov_callbacks cb;
    cb.read_func  = (size_t (*)(void *, size_t, size_t, void *))cb_read;
    cb.seek_func  = (int (*)(void *, ogg_int64_t, int))cb_seek;
    cb.close_func = (int (*)(void *))cb_close;
    cb.tell_func  = (long (*)(void *))cb_tell;

    OggVorbis_File *r = (OggVorbis_File *)calloc(1, sizeof(OggVorbis_File));
    if (!r) { *(OggVorbis_File **)vf = NULL; return -1; }

    int ret = ov_open_callbacks(datasource, r, initial, ibytes, cb);
    if (ret < 0) {
        free(r);
        *(OggVorbis_File **)vf = NULL;
        return ret;
    }
    *(OggVorbis_File **)vf = r;
    return ret;
}

int pd_ov_clear(void *vf) {
    OggVorbis_File *r = real_vf(vf);
    if (!r) return 0;
    int ret = ov_clear(r);
    free(r);
    *(OggVorbis_File **)vf = NULL;
    return ret;
}

long pd_ov_read(void *vf, char *buffer, int length, int *bitstream) {
    OggVorbis_File *r = real_vf(vf);
    return r ? ov_read(r, buffer, length, 0 /* little endian */, 2 /* 16-bit */, 1 /* signed */, bitstream) : -1;
}

long long pd_ov_pcm_total(void *vf, int i) {
    OggVorbis_File *r = real_vf(vf);
    return r ? (long long)ov_pcm_total(r, i) : -1;
}

int pd_ov_pcm_seek(void *vf, long long pos) {
    OggVorbis_File *r = real_vf(vf);
    return r ? ov_pcm_seek(r, (ogg_int64_t)pos) : -1;
}
