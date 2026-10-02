/*
 * native_glue.h
 *
 * NativeActivity / native_app_glue emulation layer for PAC-MAN Dash! (PS Vita).
 *
 * Provides, on the host side, everything libmain.so expects from Android:
 *   - a fake ANativeActivity + lifecycle callbacks
 *   - ALooper (poll based), AInputQueue, AInputEvent/AKeyEvent/AMotionEvent
 *   - ANativeWindow, AConfiguration, ASensor (stubs)
 *   - an in-memory pipe() (Vita newlib has no usable pipe) with matching
 *     read/write/close wrappers that fall through to the normal ones for real fds
 *
 * Integration in dynlib.c (see bottom of this file).
 */
#ifndef NATIVE_GLUE_H
#define NATIVE_GLUE_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <so_util/so_util.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*pd_looper_cb)(int fd, int events, void *data);

/* ---- Public API used by main.c ---- */

/* Calls ANativeActivity_onCreate and drives the Android lifecycle
 * (start, resume, window + input queue created, focus). Blocks until the game's
 * android_main thread has processed those commands. Returns 0 on success. */
int  native_glue_start(so_module *mod,
                       const char *internal_path,
                       const char *external_path,
                       const char *obb_path);

/* Non-zero once the game called ANativeActivity_finish(). */
int  native_glue_finish_requested(void);

/* Input injection (call from the main thread). */
void pd_input_key(int android_keycode, int down);
/* action: 0 = down, 1 = up, anything else = move. id = touch report id. */
void pd_input_touch(int action, int id, float x, float y);

/* ---- fd wrappers (registered as "pipe", "read", "write", "close") ---- */
int pd_pipe(int fds[2]);
int pd_read(int fd, void *buf, size_t n);
int pd_write(int fd, const void *buf, size_t n);
int pd_close(int fd);

/* ---- Android NDK surface (opaque handles are void *) ---- */
void *ALooper_prepare(int opts);
void *ALooper_forThread(void);
void  ALooper_acquire(void *looper);
void  ALooper_release(void *looper);
int   ALooper_addFd(void *looper, int fd, int ident, int events, pd_looper_cb cb, void *data);
int   ALooper_removeFd(void *looper, int fd);
int   ALooper_pollOnce(int timeoutMillis, int *outFd, int *outEvents, void **outData);
int   ALooper_pollAll(int timeoutMillis, int *outFd, int *outEvents, void **outData);
void  ALooper_wake(void *looper);

void    AInputQueue_attachLooper(void *queue, void *looper, int ident, pd_looper_cb cb, void *data);
void    AInputQueue_detachLooper(void *queue);
int32_t AInputQueue_hasEvents(void *queue);
int32_t AInputQueue_getEvent(void *queue, void **outEvent);
int32_t AInputQueue_preDispatchEvent(void *queue, void *event);
void    AInputQueue_finishEvent(void *queue, void *event, int handled);

int32_t AInputEvent_getType(const void *ev);
int32_t AInputEvent_getDeviceId(const void *ev);
int32_t AInputEvent_getSource(const void *ev);

int32_t AKeyEvent_getAction(const void *ev);
int32_t AKeyEvent_getFlags(const void *ev);
int32_t AKeyEvent_getKeyCode(const void *ev);
int32_t AKeyEvent_getScanCode(const void *ev);
int32_t AKeyEvent_getMetaState(const void *ev);
int32_t AKeyEvent_getRepeatCount(const void *ev);
int64_t AKeyEvent_getDownTime(const void *ev);
int64_t AKeyEvent_getEventTime(const void *ev);

int32_t AMotionEvent_getAction(const void *ev);
int32_t AMotionEvent_getFlags(const void *ev);
int32_t AMotionEvent_getMetaState(const void *ev);
int32_t AMotionEvent_getButtonState(const void *ev);
int32_t AMotionEvent_getEdgeFlags(const void *ev);
int64_t AMotionEvent_getDownTime(const void *ev);
int64_t AMotionEvent_getEventTime(const void *ev);
size_t  AMotionEvent_getPointerCount(const void *ev);
int32_t AMotionEvent_getPointerId(const void *ev, size_t idx);
float   AMotionEvent_getX(const void *ev, size_t idx);
float   AMotionEvent_getY(const void *ev, size_t idx);
float   AMotionEvent_getRawX(const void *ev, size_t idx);
float   AMotionEvent_getRawY(const void *ev, size_t idx);
float   AMotionEvent_getPressure(const void *ev, size_t idx);
float   AMotionEvent_getSize(const void *ev, size_t idx);
float   AMotionEvent_getAxisValue(const void *ev, int32_t axis, size_t idx);

void *  AConfiguration_new(void);
void    AConfiguration_delete(void *cfg);
void    AConfiguration_fromAssetManager(void *cfg, void *mgr);
int32_t AConfiguration_getMcc(void *cfg);
int32_t AConfiguration_getMnc(void *cfg);
void    AConfiguration_getLanguage(void *cfg, char *outLanguage);
void    AConfiguration_getCountry(void *cfg, char *outCountry);
int32_t AConfiguration_getOrientation(void *cfg);
int32_t AConfiguration_getTouchscreen(void *cfg);
int32_t AConfiguration_getDensity(void *cfg);
int32_t AConfiguration_getKeyboard(void *cfg);
int32_t AConfiguration_getNavigation(void *cfg);
int32_t AConfiguration_getKeysHidden(void *cfg);
int32_t AConfiguration_getNavHidden(void *cfg);
int32_t AConfiguration_getSdkVersion(void *cfg);
int32_t AConfiguration_getScreenSize(void *cfg);
int32_t AConfiguration_getScreenLong(void *cfg);
int32_t AConfiguration_getUiModeType(void *cfg);
int32_t AConfiguration_getUiModeNight(void *cfg);

int32_t ANativeWindow_getWidth(void *win);
int32_t ANativeWindow_getHeight(void *win);
int32_t ANativeWindow_getFormat(void *win);
int32_t ANativeWindow_setBuffersGeometry(void *win, int32_t w, int32_t h, int32_t fmt);
void    ANativeWindow_acquire(void *win);
void    ANativeWindow_release(void *win);

void ANativeActivity_finish(void *activity);
void ANativeActivity_setWindowFlags(void *activity, uint32_t addFlags, uint32_t removeFlags);
void ANativeActivity_setWindowFormat(void *activity, int32_t format);
void ANativeActivity_showSoftInput(void *activity, uint32_t flags);
void ANativeActivity_hideSoftInput(void *activity, uint32_t flags);

void *  ASensorManager_getInstance(void);
void *  ASensorManager_getDefaultSensor(void *mgr, int type);
void *  ASensorManager_createEventQueue(void *mgr, void *looper, int ident, pd_looper_cb cb, void *data);
int     ASensorManager_destroyEventQueue(void *mgr, void *queue);
int     ASensorEventQueue_enableSensor(void *queue, void *sensor);
int     ASensorEventQueue_disableSensor(void *queue, void *sensor);
int     ASensorEventQueue_setEventRate(void *queue, void *sensor, int32_t usec);
int     ASensorEventQueue_hasEvents(void *queue);
ssize_t ASensorEventQueue_getEvents(void *queue, void *events, size_t count);
const char *ASensor_getName(void *sensor);
int     ASensor_getType(void *sensor);
float   ASensor_getResolution(void *sensor);
int     ASensor_getMinDelay(void *sensor);

/*
 * ---- dynlib.c integration -------------------------------------------------
 *
 * 1. #include "native_glue.h" near the other includes.
 * 2. In default_dynlib[]: DELETE the old lines for "pipe", "read", "write" and
 *    "close", and add this single line anywhere inside the table:
 *
 *        PD_DYNLIB_ENTRIES
 *
 * (All entries must live in the same table: so_resolve() is called once.)
 */
#define PDE(x) { #x, (uintptr_t)&x },

#define PD_DYNLIB_ENTRIES \
    { "pipe",  (uintptr_t)&pd_pipe  }, \
    { "read",  (uintptr_t)&pd_read  }, \
    { "write", (uintptr_t)&pd_write }, \
    { "close", (uintptr_t)&pd_close }, \
    PDE(ALooper_prepare) PDE(ALooper_forThread) PDE(ALooper_acquire) PDE(ALooper_release) \
    PDE(ALooper_addFd) PDE(ALooper_removeFd) PDE(ALooper_pollOnce) PDE(ALooper_pollAll) \
    PDE(ALooper_wake) \
    PDE(AInputQueue_attachLooper) PDE(AInputQueue_detachLooper) PDE(AInputQueue_hasEvents) \
    PDE(AInputQueue_getEvent) PDE(AInputQueue_preDispatchEvent) PDE(AInputQueue_finishEvent) \
    PDE(AInputEvent_getType) PDE(AInputEvent_getDeviceId) PDE(AInputEvent_getSource) \
    PDE(AKeyEvent_getAction) PDE(AKeyEvent_getFlags) PDE(AKeyEvent_getKeyCode) \
    PDE(AKeyEvent_getScanCode) PDE(AKeyEvent_getMetaState) PDE(AKeyEvent_getRepeatCount) \
    PDE(AKeyEvent_getDownTime) PDE(AKeyEvent_getEventTime) \
    PDE(AMotionEvent_getAction) PDE(AMotionEvent_getFlags) PDE(AMotionEvent_getMetaState) \
    PDE(AMotionEvent_getButtonState) PDE(AMotionEvent_getEdgeFlags) PDE(AMotionEvent_getDownTime) \
    PDE(AMotionEvent_getEventTime) PDE(AMotionEvent_getPointerCount) PDE(AMotionEvent_getPointerId) \
    PDE(AMotionEvent_getX) PDE(AMotionEvent_getY) PDE(AMotionEvent_getRawX) PDE(AMotionEvent_getRawY) \
    PDE(AMotionEvent_getPressure) PDE(AMotionEvent_getSize) PDE(AMotionEvent_getAxisValue) \
    PDE(AConfiguration_new) PDE(AConfiguration_delete) PDE(AConfiguration_fromAssetManager) \
    PDE(AConfiguration_getMcc) PDE(AConfiguration_getMnc) PDE(AConfiguration_getLanguage) \
    PDE(AConfiguration_getCountry) PDE(AConfiguration_getOrientation) PDE(AConfiguration_getTouchscreen) \
    PDE(AConfiguration_getDensity) PDE(AConfiguration_getKeyboard) PDE(AConfiguration_getNavigation) \
    PDE(AConfiguration_getKeysHidden) PDE(AConfiguration_getNavHidden) PDE(AConfiguration_getSdkVersion) \
    PDE(AConfiguration_getScreenSize) PDE(AConfiguration_getScreenLong) PDE(AConfiguration_getUiModeType) \
    PDE(AConfiguration_getUiModeNight) \
    PDE(ANativeWindow_getWidth) PDE(ANativeWindow_getHeight) PDE(ANativeWindow_getFormat) \
    PDE(ANativeWindow_setBuffersGeometry) PDE(ANativeWindow_acquire) PDE(ANativeWindow_release) \
    PDE(ANativeActivity_finish) PDE(ANativeActivity_setWindowFlags) PDE(ANativeActivity_setWindowFormat) \
    PDE(ANativeActivity_showSoftInput) PDE(ANativeActivity_hideSoftInput) \
    PDE(ASensorManager_getInstance) PDE(ASensorManager_getDefaultSensor) \
    PDE(ASensorManager_createEventQueue) PDE(ASensorManager_destroyEventQueue) \
    PDE(ASensorEventQueue_enableSensor) PDE(ASensorEventQueue_disableSensor) \
    PDE(ASensorEventQueue_setEventRate) PDE(ASensorEventQueue_hasEvents) PDE(ASensorEventQueue_getEvents) \
    PDE(ASensor_getName) PDE(ASensor_getType) PDE(ASensor_getResolution) PDE(ASensor_getMinDelay)

#ifdef __cplusplus
}
#endif

#endif /* NATIVE_GLUE_H */
