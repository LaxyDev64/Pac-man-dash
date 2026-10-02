/*
 * main.c
 *
 * ARMv7 Shared Libraries loader. PAC-MAN Dash! (com.namcobandaigames.pacmandash.v01)
 *
 * Based on the PAC-MAN Championship Edition DX loader.
 *
 * Copyright (C) 2021 Andy Nguyen
 * Copyright (C) 2021-2023 Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/init.h"
#include "utils/glutil.h"
#include "utils/settings.h"
#include "utils/logger.h"

#include <psp2/kernel/threadmgr.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>

#include "reimpl/controls.h"
#include "reimpl/egl.h"
#include "native_glue.h"
#include "sq_trace.h"

#include <vitasdk.h>
#include <kubridge.h>
#include <psp2/io/fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

int _newlib_heap_size_user = 256 * 1024 * 1024;
unsigned int sceUserMainThreadStackSize = 4 * 1024 * 1024;

#ifdef USE_SCELIBC_IO
int sceLibcHeapSize = 24 * 1024 * 1024;
#endif

so_module so_mod;
/* Kept only so other boilerplate files (init.c / patch.c) that still reference
 * it keep linking. PAC-MAN Dash! has no FMOD. */
so_module fmod_mod;

/*
 * Startup sequence for a NativeActivity game:
 *
 *   soloader_init_all()      loads libmain.so, resolves imports, runs init_array
 *                            (and JNI_OnLoad through FalsoJNI, if jni_init does it)
 *   native_glue_start()      ANativeActivity_onCreate -> android_main thread,
 *                            then onStart/onResume/window/input/focus
 *   main loop                only feeds Vita input into the game's AInputQueue.
 *
 * Rendering happens inside the game's own thread: it creates its EGL context
 * (reimpl/egl.c) and calls eglSwapBuffers itself.
 *
 * Set -DPD_INIT_GL_ON_MAIN_THREAD=1 if vitaGL misbehaves when it is initialised
 * lazily from the game thread by eglInitialize (falls back to CE DX behaviour).
 */

/* Android key codes */
#define AKEYCODE_BACK           4
#define AKEYCODE_DPAD_UP        19
#define AKEYCODE_DPAD_DOWN      20
#define AKEYCODE_DPAD_LEFT      21
#define AKEYCODE_DPAD_RIGHT     22
#define AKEYCODE_BUTTON_A       96
#define AKEYCODE_BUTTON_B       97
#define AKEYCODE_BUTTON_X       99
#define AKEYCODE_BUTTON_Y       100
#define AKEYCODE_BUTTON_L1      102
#define AKEYCODE_BUTTON_R1      103
#define AKEYCODE_BUTTON_START   108

#define ANALOG_DEADZONE 50

static const struct {
    uint32_t vita;
    int      android;
} key_map[] = {
    { SCE_CTRL_UP,       AKEYCODE_DPAD_UP      },
    { SCE_CTRL_DOWN,     AKEYCODE_DPAD_DOWN    },
    { SCE_CTRL_LEFT,     AKEYCODE_DPAD_LEFT    },
    { SCE_CTRL_RIGHT,    AKEYCODE_DPAD_RIGHT   },
    { SCE_CTRL_CROSS,    AKEYCODE_BUTTON_A     },
    { SCE_CTRL_CIRCLE,   AKEYCODE_BUTTON_B     },
    { SCE_CTRL_SQUARE,   AKEYCODE_BUTTON_X     },
    { SCE_CTRL_TRIANGLE, AKEYCODE_BUTTON_Y     },
    { SCE_CTRL_LTRIGGER, AKEYCODE_BUTTON_L1    },
    { SCE_CTRL_RTRIGGER, AKEYCODE_BUTTON_R1    },
    { SCE_CTRL_START,    AKEYCODE_BUTTON_START },
    { SCE_CTRL_SELECT,   AKEYCODE_BACK         },
};

static uint32_t prev_buttons = 0;

static void process_input(void) {
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(0, &pad, 1);

    /* Buttons + left stick as d-pad */
    uint32_t cur = pad.buttons;
    int lx = (int)pad.lx - 128;
    int ly = (int)pad.ly - 128;
    if (abs(lx) > ANALOG_DEADZONE || abs(ly) > ANALOG_DEADZONE) {
        if (abs(lx) > abs(ly)) {
            cur |= (lx < 0) ? SCE_CTRL_LEFT : SCE_CTRL_RIGHT;
        } else {
            cur |= (ly < 0) ? SCE_CTRL_UP : SCE_CTRL_DOWN;
        }
    }

    for (size_t i = 0; i < sizeof(key_map) / sizeof(key_map[0]); i++) {
        int now = (cur & key_map[i].vita) ? 1 : 0;
        int was = (prev_buttons & key_map[i].vita) ? 1 : 0;
        if (now != was)
            pd_input_key(key_map[i].android, now);
    }
    prev_buttons = cur;
}

/* Controls callbacks required by the boilerplate (reimpl/controls.h) */
void controls_handler_key(int32_t keycode, ControlsAction action) {
    (void)keycode;
    (void)action;
}

void controls_handler_touch(int32_t id, float x, float y, ControlsAction action) {
    /* Coordinates are forwarded untouched, like in the CE DX loader.
     * If touches land in the wrong place, scale here to the ANativeWindow
     * size (settings_display_width/height). */
    int a = (action == CONTROLS_ACTION_DOWN) ? 0 :
            (action == CONTROLS_ACTION_UP)   ? 1 : 2;
    pd_input_touch(a, id, x, y);
}

void controls_handler_analog(ControlsStickId which, float x, float y, ControlsAction action) {
    (void)which; (void)x; (void)y; (void)action;
}

/*
 * Exit after a fault; diagnostic builds also save the crash register state.
 */
static void crash_abort_handler(KuKernelAbortContext *ctx) {
#ifdef SOLOADER_FILE_LOGGING
    SceUID fd = sceIoOpen(WRITABLE_PATH "debug.log",
                          SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (fd >= 0) {
        char buf[640];
        int len = sceClibSnprintf(buf, sizeof(buf),
            "\n=== ABORT HANDLER ===\n"
            "Type: %s\n"
            "PC:  0x%08X  (libmain+0x%08X)\n"
            "LR:  0x%08X  (libmain+0x%08X)\n"
            "SP:  0x%08X\n"
            "FAR: 0x%08X  (libmain+0x%08X)\n"
            "FSR: 0x%08X  SPSR: 0x%08X\n"
            "R0=0x%08X R1=0x%08X R2=0x%08X R3=0x%08X\n"
            "R4=0x%08X R5=0x%08X R6=0x%08X R7=0x%08X\n"
            "R8=0x%08X R9=0x%08X R10=0x%08X R11=0x%08X R12=0x%08X\n"
            "text_base=0x%08X size=0x%X\n"
            "=== END ABORT ===\n",
            ctx->abortType == KU_KERNEL_ABORT_TYPE_DATA_ABORT ? "DATA ABORT" : "PREFETCH ABORT",
            ctx->pc, ctx->pc - so_mod.text_base,
            ctx->lr, ctx->lr - so_mod.text_base,
            ctx->sp,
            ctx->FAR, ctx->FAR - so_mod.text_base,
            ctx->FSR, ctx->SPSR,
            ctx->r0, ctx->r1, ctx->r2, ctx->r3,
            ctx->r4, ctx->r5, ctx->r6, ctx->r7,
            ctx->r8, ctx->r9, ctx->r10, ctx->r11, ctx->r12,
            so_mod.text_base, so_mod.text_size);
        sceIoWrite(fd, buf, len);
        sceIoClose(fd);
    }
#endif
    sceClibPrintf("ABORT: type=%d PC=0x%08X LR=0x%08X FAR=0x%08X\n",
                  ctx->abortType, ctx->pc, ctx->lr, ctx->FAR);
    sceKernelExitProcess(0);
}

int main() {
    SceAppUtilInitParam appUtilParam;
    SceAppUtilBootParam appUtilBootParam;
    memset(&appUtilParam, 0, sizeof(SceAppUtilInitParam));
    memset(&appUtilBootParam, 0, sizeof(SceAppUtilBootParam));
    sceAppUtilInit(&appUtilParam, &appUtilBootParam);

#ifdef SOLOADER_FILE_LOGGING
    {
        SceUID fd = sceIoOpen(WRITABLE_PATH "debug.log",
                              SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
        if (fd >= 0) sceIoClose(fd);
    }
#endif

    /* Loads libmain.so (see SO_PATH), resolves imports, runs constructors. */
    soloader_init_all();

    {
        int ret = kuKernelRegisterAbortHandler(crash_abort_handler, NULL, NULL);
        l_info("kuKernelRegisterAbortHandler: %d", ret);
    }

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);

    /* Dumps the decrypted Squirrel scripts to ux0:data/pacmandash/dump/ and
     * applies the text patches in sq_trace.c (build with -DPD_SQ_DUMP=0 to stop dumping). */
    sq_trace_init(&so_mod);

#if defined(PD_INIT_GL_ON_MAIN_THREAD) && PD_INIT_GL_ON_MAIN_THREAD
    gl_init();
    egl_mark_gl_initialized(); /* the game's own eglInitialize must not init twice */
    l_info("GL initialised on the main thread (%dx%d)",
           settings_display_width(), settings_display_height());
#endif

    /* Starts the android_main thread and walks through the activity lifecycle. */
    if (native_glue_start(&so_mod, DATA_PATH_NOSLASH, DATA_PATH_NOSLASH, DATA_PATH_NOSLASH) < 0) {
        l_error("native_glue_start failed");
        sceKernelExitProcess(0);
    }

    l_info("Entering input loop");
    while (!native_glue_finish_requested()) {
        controls_poll_touch();   /* -> controls_handler_touch -> pd_input_touch */
        process_input();         /* -> pd_input_key */
        sceKernelDelayThread(4000);
    }

    l_info("Game requested finish, exiting");
    sceKernelExitProcess(0);
    return 0;
}
