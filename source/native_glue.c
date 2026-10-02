/*
 * native_glue.c
 *
 * NativeActivity / native_app_glue emulation for PAC-MAN Dash! on PS Vita.
 * See native_glue.h for the integration notes.
 *
 * Flow:
 *   main thread  : native_glue_start() -> ANativeActivity_onCreate()
 *                  (the glue spawns the "android_main" thread via pthread_create)
 *                  then onStart/onResume/window/input-queue/focus callbacks.
 *   game thread  : android_main() -> ALooper_pollAll() -> our pipes / input queue.
 *   main thread  : afterwards only injects input (pd_input_key / pd_input_touch).
 */

#include "native_glue.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <psp2/apputil.h>
#include <psp2/system_param.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <falso_jni/FalsoJNI.h>

#include "utils/logger.h"
#include "utils/settings.h"

extern int close_soloader(int fd);

/* ------------------------------------------------------------------------ */
/* Tiny spin lock (critical sections are a few memcpy's long)                */
/* ------------------------------------------------------------------------ */
static volatile int g_lock_var;
static inline void lock(void)   { while (__sync_lock_test_and_set(&g_lock_var, 1)) sceKernelDelayThread(20); }
static inline void unlock(void) { __sync_lock_release(&g_lock_var); }

static inline uint64_t now_us(void) { return sceKernelGetProcessTimeWide(); }

/* ------------------------------------------------------------------------ */
/* NDK-compatible structures (layout must match android/native_activity.h)   */
/* ------------------------------------------------------------------------ */
typedef struct {
    void  (*onStart)(void *);
    void  (*onResume)(void *);
    void *(*onSaveInstanceState)(void *, size_t *);
    void  (*onPause)(void *);
    void  (*onStop)(void *);
    void  (*onDestroy)(void *);
    void  (*onWindowFocusChanged)(void *, int);
    void  (*onNativeWindowCreated)(void *, void *);
    void  (*onNativeWindowResized)(void *, void *);
    void  (*onNativeWindowRedrawNeeded)(void *, void *);
    void  (*onNativeWindowDestroyed)(void *, void *);
    void  (*onInputQueueCreated)(void *, void *);
    void  (*onInputQueueDestroyed)(void *, void *);
    void  (*onContentRectChanged)(void *, const void *);
    void  (*onConfigurationChanged)(void *);
    void  (*onLowMemory)(void *);
} pd_callbacks;

typedef struct {
    pd_callbacks *callbacks;
    void         *vm;
    void         *env;
    void         *clazz;
    const char   *internalDataPath;
    const char   *externalDataPath;
    int32_t       sdkVersion;
    void         *instance;
    void         *assetManager;
    const char   *obbPath;
} pd_activity;

static pd_callbacks g_callbacks;
static pd_activity  g_activity;
static int          g_asset_mgr_dummy;          /* opaque AAssetManager* */
static volatile int g_finish_requested;

/* ------------------------------------------------------------------------ */
/* In-memory pipes                                                           */
/* ------------------------------------------------------------------------ */
#define PIPE_FD_BASE 0x5D00
#define PIPE_COUNT   4
#define PIPE_CAP     2048

typedef struct {
    uint8_t  buf[PIPE_CAP];
    uint32_t head, count;
    int      used, r_open, w_open;
} fake_pipe;

static fake_pipe g_pipes[PIPE_COUNT];

static fake_pipe *pipe_from_fd(int fd, int *is_write) {
    if (fd < PIPE_FD_BASE || fd >= PIPE_FD_BASE + PIPE_COUNT * 2)
        return NULL;
    int idx = (fd - PIPE_FD_BASE) >> 1;
    if (!g_pipes[idx].used)
        return NULL;
    if (is_write) *is_write = (fd - PIPE_FD_BASE) & 1;
    return &g_pipes[idx];
}

int pd_pipe(int fds[2]) {
    lock();
    for (int i = 0; i < PIPE_COUNT; i++) {
        if (!g_pipes[i].used) {
            memset(&g_pipes[i], 0, sizeof(g_pipes[i]));
            g_pipes[i].used = g_pipes[i].r_open = g_pipes[i].w_open = 1;
            fds[0] = PIPE_FD_BASE + i * 2;
            fds[1] = PIPE_FD_BASE + i * 2 + 1;
            unlock();
            return 0;
        }
    }
    unlock();
    l_error("pd_pipe: no free fake pipes");
    return -1;
}

int pd_read(int fd, void *buf, size_t n) {
    int w = 0;
    fake_pipe *p = pipe_from_fd(fd, &w);
    if (!p) return (int)read(fd, buf, n);
    if (w) return -1;
    if (n == 0) return 0;

    for (;;) {
        lock();
        if (p->count) {
            size_t k = n < p->count ? n : p->count;
            for (size_t i = 0; i < k; i++)
                ((uint8_t *)buf)[i] = p->buf[(p->head + i) % PIPE_CAP];
            p->head = (p->head + k) % PIPE_CAP;
            p->count -= k;
            unlock();
            return (int)k;
        }
        int open = p->w_open;
        unlock();
        if (!open) return 0; /* EOF */
        sceKernelDelayThread(200);
    }
}

int pd_write(int fd, const void *buf, size_t n) {
    int w = 0;
    fake_pipe *p = pipe_from_fd(fd, &w);
    if (!p) return (int)write(fd, buf, n);
    if (!w) return -1;

    size_t done = 0;
    while (done < n) {
        lock();
        if (!p->r_open) { unlock(); return -1; }
        while (done < n && p->count < PIPE_CAP) {
            p->buf[(p->head + p->count) % PIPE_CAP] = ((const uint8_t *)buf)[done++];
            p->count++;
        }
        unlock();
        if (done < n) sceKernelDelayThread(200);
    }
    return (int)done;
}

int pd_close(int fd) {
    int w = 0;
    fake_pipe *p = pipe_from_fd(fd, &w);
    if (!p) return close_soloader(fd);
    lock();
    if (w) p->w_open = 0; else p->r_open = 0;
    if (!p->r_open && !p->w_open) p->used = 0;
    unlock();
    return 0;
}

static int pipe_readable_nolock(int fd) {
    fake_pipe *p = pipe_from_fd(fd, NULL);
    return p && (p->count > 0 || !p->w_open);
}

/* ------------------------------------------------------------------------ */
/* Input events                                                              */
/* ------------------------------------------------------------------------ */
#define PD_MAX_POINTERS 5
#define EVQ             128

typedef struct {
    int     type;      /* 1 = key, 2 = motion */
    int     source;
    int     action;
    int     keycode;
    int     repeat;
    int     count;
    struct { int id; float x, y; } p[PD_MAX_POINTERS];
    int64_t time;
} pd_event;

static pd_event g_ev[EVQ];
static uint32_t g_ev_head, g_ev_count;
static volatile int g_accept_input;

static void push_event(const pd_event *e) {
    if (!g_accept_input) return;
    lock();
    if (g_ev_count < EVQ) {
        g_ev[(g_ev_head + g_ev_count) % EVQ] = *e;
        g_ev_count++;
    }
    unlock();
}

void pd_input_key(int keycode, int down) {
    pd_event e;
    memset(&e, 0, sizeof(e));
    e.type = 1;
    e.source = 0x401 | 0x201;           /* GAMEPAD | DPAD */
    e.action = down ? 0 : 1;
    e.keycode = keycode;
    e.time = (int64_t)now_us() * 1000;  /* ns */
    push_event(&e);
}

typedef struct { int used, id; float x, y; } tslot;
static tslot g_ts[PD_MAX_POINTERS];

void pd_input_touch(int action, int id, float x, float y) {
    int slot = -1;
    for (int i = 0; i < PD_MAX_POINTERS; i++)
        if (g_ts[i].used && g_ts[i].id == id) slot = i;

    if (action == 0) {
        if (slot < 0) {
            for (int i = 0; i < PD_MAX_POINTERS; i++)
                if (!g_ts[i].used) { slot = i; g_ts[i].used = 1; g_ts[i].id = id; break; }
        }
        if (slot < 0) return;
    } else if (slot < 0) {
        return;
    }
    g_ts[slot].x = x;
    g_ts[slot].y = y;

    pd_event e;
    memset(&e, 0, sizeof(e));
    e.type = 2;
    e.source = 0x1002; /* TOUCHSCREEN */
    e.time = (int64_t)now_us() * 1000;

    int n = 0, rank = 0;
    for (int i = 0; i < PD_MAX_POINTERS; i++) {
        if (!g_ts[i].used) continue;
        if (i == slot) rank = n;
        e.p[n].id = i;
        e.p[n].x = g_ts[i].x;
        e.p[n].y = g_ts[i].y;
        n++;
    }
    e.count = n;

    if (action == 0)      e.action = (n == 1) ? 0 : (5 | (rank << 8)); /* DOWN / POINTER_DOWN */
    else if (action == 1) e.action = (n == 1) ? 1 : (6 | (rank << 8)); /* UP / POINTER_UP     */
    else                  e.action = 2;                                /* MOVE                */

    push_event(&e);
    if (action == 1) g_ts[slot].used = 0;
}

int32_t AInputEvent_getType(const void *ev)     { return ((const pd_event *)ev)->type; }
int32_t AInputEvent_getDeviceId(const void *ev) { (void)ev; return 1; }
int32_t AInputEvent_getSource(const void *ev)   { return ((const pd_event *)ev)->source; }

int32_t AKeyEvent_getAction(const void *ev)      { return ((const pd_event *)ev)->action; }
int32_t AKeyEvent_getFlags(const void *ev)       { (void)ev; return 0; }
int32_t AKeyEvent_getKeyCode(const void *ev)     { return ((const pd_event *)ev)->keycode; }
int32_t AKeyEvent_getScanCode(const void *ev)    { (void)ev; return 0; }
int32_t AKeyEvent_getMetaState(const void *ev)   { (void)ev; return 0; }
int32_t AKeyEvent_getRepeatCount(const void *ev) { return ((const pd_event *)ev)->repeat; }
int64_t AKeyEvent_getDownTime(const void *ev)    { return ((const pd_event *)ev)->time; }
int64_t AKeyEvent_getEventTime(const void *ev)   { return ((const pd_event *)ev)->time; }

int32_t AMotionEvent_getAction(const void *ev)      { return ((const pd_event *)ev)->action; }
int32_t AMotionEvent_getFlags(const void *ev)       { (void)ev; return 0; }
int32_t AMotionEvent_getMetaState(const void *ev)   { (void)ev; return 0; }
int32_t AMotionEvent_getButtonState(const void *ev) { (void)ev; return 0; }
int32_t AMotionEvent_getEdgeFlags(const void *ev)   { (void)ev; return 0; }
int64_t AMotionEvent_getDownTime(const void *ev)    { return ((const pd_event *)ev)->time; }
int64_t AMotionEvent_getEventTime(const void *ev)   { return ((const pd_event *)ev)->time; }
size_t  AMotionEvent_getPointerCount(const void *ev){ return (size_t)((const pd_event *)ev)->count; }

static const pd_event *ev_ptr(const void *ev, size_t idx, int *ok) {
    const pd_event *e = (const pd_event *)ev;
    *ok = idx < (size_t)e->count;
    return e;
}
int32_t AMotionEvent_getPointerId(const void *ev, size_t idx) {
    int ok; const pd_event *e = ev_ptr(ev, idx, &ok); return ok ? e->p[idx].id : 0;
}
float AMotionEvent_getX(const void *ev, size_t idx) {
    int ok; const pd_event *e = ev_ptr(ev, idx, &ok); return ok ? e->p[idx].x : 0.0f;
}
float AMotionEvent_getY(const void *ev, size_t idx) {
    int ok; const pd_event *e = ev_ptr(ev, idx, &ok); return ok ? e->p[idx].y : 0.0f;
}
float AMotionEvent_getRawX(const void *ev, size_t idx) { return AMotionEvent_getX(ev, idx); }
float AMotionEvent_getRawY(const void *ev, size_t idx) { return AMotionEvent_getY(ev, idx); }
float AMotionEvent_getPressure(const void *ev, size_t idx) { (void)ev; (void)idx; return 1.0f; }
float AMotionEvent_getSize(const void *ev, size_t idx)     { (void)ev; (void)idx; return 0.1f; }
float AMotionEvent_getAxisValue(const void *ev, int32_t axis, size_t idx) {
    switch (axis) {
        case 0: return AMotionEvent_getX(ev, idx);      /* AXIS_X        */
        case 1: return AMotionEvent_getY(ev, idx);      /* AXIS_Y        */
        case 2: return 1.0f;                            /* AXIS_PRESSURE */
        default: return 0.0f;
    }
}

/* ------------------------------------------------------------------------ */
/* ALooper + AInputQueue                                                     */
/* ------------------------------------------------------------------------ */
#define LOOPER_EVENT_INPUT 1
#define LOOPER_POLL_WAKE    (-1)
#define LOOPER_POLL_CALLBACK (-2)
#define LOOPER_POLL_TIMEOUT (-3)

#define SRC_MAX 8
typedef struct {
    int used, fd, ident, events, is_queue;
    pd_looper_cb cb;
    void *data;
} lsrc;

static lsrc g_src[SRC_MAX];
static int  g_rot;
static volatile int g_wake;
static int  g_looper_obj;          /* opaque ALooper* */
static int  g_queue_obj;           /* opaque AInputQueue* */

void *ALooper_prepare(int opts)  { (void)opts; return &g_looper_obj; }
void *ALooper_forThread(void)    { return &g_looper_obj; }
void  ALooper_acquire(void *l)   { (void)l; }
void  ALooper_release(void *l)   { (void)l; }
void  ALooper_wake(void *l)      { (void)l; g_wake = 1; }

static int add_source(int fd, int ident, int events, pd_looper_cb cb, void *data, int is_queue) {
    lock();
    int slot = -1;
    for (int i = 0; i < SRC_MAX; i++) {
        if (g_src[i].used && !is_queue && !g_src[i].is_queue && g_src[i].fd == fd) { slot = i; break; }
        if (g_src[i].used && is_queue && g_src[i].is_queue) { slot = i; break; }
    }
    if (slot < 0)
        for (int i = 0; i < SRC_MAX; i++)
            if (!g_src[i].used) { slot = i; break; }
    if (slot < 0) { unlock(); l_error("ALooper: no free source slots"); return -1; }
    g_src[slot].used = 1;
    g_src[slot].fd = fd;
    g_src[slot].ident = ident;
    g_src[slot].events = events;
    g_src[slot].cb = cb;
    g_src[slot].data = data;
    g_src[slot].is_queue = is_queue;
    unlock();
    return 1;
}

int ALooper_addFd(void *l, int fd, int ident, int events, pd_looper_cb cb, void *data) {
    (void)l;
    return add_source(fd, ident, events, cb, data, 0);
}

int ALooper_removeFd(void *l, int fd) {
    (void)l;
    int r = 0;
    lock();
    for (int i = 0; i < SRC_MAX; i++)
        if (g_src[i].used && !g_src[i].is_queue && g_src[i].fd == fd) { g_src[i].used = 0; r = 1; }
    unlock();
    return r;
}

void AInputQueue_attachLooper(void *q, void *l, int ident, pd_looper_cb cb, void *data) {
    (void)q; (void)l;
    add_source(-1, ident, LOOPER_EVENT_INPUT, cb, data, 1);
}

void AInputQueue_detachLooper(void *q) {
    (void)q;
    lock();
    for (int i = 0; i < SRC_MAX; i++)
        if (g_src[i].used && g_src[i].is_queue) g_src[i].used = 0;
    unlock();
}

int32_t AInputQueue_hasEvents(void *q) { (void)q; return g_ev_count > 0; }

int32_t AInputQueue_getEvent(void *q, void **out) {
    (void)q;
    lock();
    if (g_ev_count == 0) { unlock(); return -1; }
    *out = &g_ev[g_ev_head];
    g_ev_head = (g_ev_head + 1) % EVQ;
    g_ev_count--;
    unlock();
    return 0;
}

int32_t AInputQueue_preDispatchEvent(void *q, void *ev) { (void)q; (void)ev; return 0; }
void    AInputQueue_finishEvent(void *q, void *ev, int handled) { (void)q; (void)ev; (void)handled; }

int ALooper_pollOnce(int timeoutMillis, int *outFd, int *outEvents, void **outData) {
    uint64_t deadline = timeoutMillis > 0 ? now_us() + (uint64_t)timeoutMillis * 1000ull : 0;

    for (;;) {
        lsrc hit;
        int found = 0, woke = 0;

        lock();
        if (g_wake) { g_wake = 0; woke = 1; }
        if (!woke) {
            for (int k = 0; k < SRC_MAX; k++) {
                int i = (g_rot + k) % SRC_MAX;
                lsrc *s = &g_src[i];
                if (!s->used) continue;
                int ready = s->is_queue ? (g_ev_count > 0) : pipe_readable_nolock(s->fd);
                if (ready) { hit = *s; found = 1; g_rot = (i + 1) % SRC_MAX; break; }
            }
        }
        unlock();

        if (woke) {
            if (outFd) *outFd = -1;
            if (outEvents) *outEvents = 0;
            if (outData) *outData = NULL;
            return LOOPER_POLL_WAKE;
        }
        if (found) {
            if (hit.cb) {
                int keep = hit.cb(hit.fd, LOOPER_EVENT_INPUT, hit.data);
                if (!keep && !hit.is_queue) ALooper_removeFd(NULL, hit.fd);
                return LOOPER_POLL_CALLBACK;
            }
            if (outFd) *outFd = hit.fd;
            if (outEvents) *outEvents = LOOPER_EVENT_INPUT;
            if (outData) *outData = hit.data;
            return hit.ident;
        }

        if (timeoutMillis == 0 || (timeoutMillis > 0 && now_us() >= deadline)) {
            if (outFd) *outFd = -1;
            if (outEvents) *outEvents = 0;
            if (outData) *outData = NULL;
            return LOOPER_POLL_TIMEOUT;
        }
        sceKernelDelayThread(500);
    }
}

int ALooper_pollAll(int timeoutMillis, int *outFd, int *outEvents, void **outData) {
    for (;;) {
        int r = ALooper_pollOnce(timeoutMillis, outFd, outEvents, outData);
        if (r != LOOPER_POLL_CALLBACK) return r;
    }
}

/* ------------------------------------------------------------------------ */
/* AConfiguration                                                            */
/* ------------------------------------------------------------------------ */
typedef struct { char lang[2]; char country[2]; } pd_config;

static void detect_language(char out[2]) {
    static const char *map[] = { "ja","en","fr","es","de","it","nl","pt","ru","ko",
                                 "zh","zh","fi","sv","da","no","pl","pt","en","tr" };
    int lang = SCE_SYSTEM_PARAM_LANG_ENGLISH_US;
    sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &lang);
    const char *c = (lang >= 0 && lang < 20) ? map[lang] : "en";
    out[0] = c[0];
    out[1] = c[1];
}

void *AConfiguration_new(void) { return calloc(1, sizeof(pd_config)); }
void  AConfiguration_delete(void *cfg) { free(cfg); }
void  AConfiguration_fromAssetManager(void *cfg, void *mgr) {
    (void)mgr;
    pd_config *c = (pd_config *)cfg;
    if (!c) return;
    detect_language(c->lang);
    c->country[0] = 'U'; c->country[1] = 'S';
}
int32_t AConfiguration_getMcc(void *cfg)         { (void)cfg; return 0; }
int32_t AConfiguration_getMnc(void *cfg)         { (void)cfg; return 0; }
void AConfiguration_getLanguage(void *cfg, char *o) {
    pd_config *c = (pd_config *)cfg;
    if (c && c->lang[0]) { o[0] = c->lang[0]; o[1] = c->lang[1]; } else { o[0] = 'e'; o[1] = 'n'; }
}
void AConfiguration_getCountry(void *cfg, char *o) {
    pd_config *c = (pd_config *)cfg;
    if (c && c->country[0]) { o[0] = c->country[0]; o[1] = c->country[1]; } else { o[0] = 'U'; o[1] = 'S'; }
}
int32_t AConfiguration_getOrientation(void *cfg) { (void)cfg; return 2;   } /* LAND         */
int32_t AConfiguration_getTouchscreen(void *cfg) { (void)cfg; return 3;   } /* FINGER       */
int32_t AConfiguration_getDensity(void *cfg)     { (void)cfg; return 240; } /* HIGH         */
int32_t AConfiguration_getKeyboard(void *cfg)    { (void)cfg; return 1;   } /* NOKEYS       */
int32_t AConfiguration_getNavigation(void *cfg)  { (void)cfg; return 1;   } /* NONAV        */
int32_t AConfiguration_getKeysHidden(void *cfg)  { (void)cfg; return 1;   } /* NO           */
int32_t AConfiguration_getNavHidden(void *cfg)   { (void)cfg; return 1;   } /* NO           */
int32_t AConfiguration_getSdkVersion(void *cfg)  { (void)cfg; return 21;  }
int32_t AConfiguration_getScreenSize(void *cfg)  { (void)cfg; return 2;   } /* NORMAL       */
int32_t AConfiguration_getScreenLong(void *cfg)  { (void)cfg; return 1;   } /* NO           */
int32_t AConfiguration_getUiModeType(void *cfg)  { (void)cfg; return 1;   } /* NORMAL       */
int32_t AConfiguration_getUiModeNight(void *cfg) { (void)cfg; return 1;   } /* NO           */

/* ------------------------------------------------------------------------ */
/* ANativeWindow / ANativeActivity helpers                                   */
/* ------------------------------------------------------------------------ */
typedef struct { int width, height; } pd_window;
static pd_window g_window;

int32_t ANativeWindow_getWidth(void *w)  { return w ? ((pd_window *)w)->width  : 0; }
int32_t ANativeWindow_getHeight(void *w) { return w ? ((pd_window *)w)->height : 0; }
int32_t ANativeWindow_getFormat(void *w) { (void)w; return 1; } /* RGBA_8888 */
int32_t ANativeWindow_setBuffersGeometry(void *w, int32_t width, int32_t height, int32_t fmt) {
    (void)w; (void)width; (void)height; (void)fmt; return 0;
}
void ANativeWindow_acquire(void *w) { (void)w; }
void ANativeWindow_release(void *w) { (void)w; }

void ANativeActivity_finish(void *a) {
    (void)a;
    l_info("ANativeActivity_finish() called by the game");
    g_finish_requested = 1;
}
void ANativeActivity_setWindowFlags(void *a, uint32_t add, uint32_t rem) { (void)a; (void)add; (void)rem; }
void ANativeActivity_setWindowFormat(void *a, int32_t f) { (void)a; (void)f; }
void ANativeActivity_showSoftInput(void *a, uint32_t f)  { (void)a; (void)f; }
void ANativeActivity_hideSoftInput(void *a, uint32_t f)  { (void)a; (void)f; }

int native_glue_finish_requested(void) { return g_finish_requested; }

/* ------------------------------------------------------------------------ */
/* Sensors (no sensors exposed to the game)                                  */
/* ------------------------------------------------------------------------ */
static int g_sensor_mgr, g_sensor_queue;

void *ASensorManager_getInstance(void) { return &g_sensor_mgr; }
void *ASensorManager_getDefaultSensor(void *m, int type) { (void)m; (void)type; return NULL; }
void *ASensorManager_createEventQueue(void *m, void *l, int ident, pd_looper_cb cb, void *d) {
    (void)m; (void)l; (void)ident; (void)cb; (void)d; return &g_sensor_queue;
}
int ASensorManager_destroyEventQueue(void *m, void *q) { (void)m; (void)q; return 0; }
int ASensorEventQueue_enableSensor(void *q, void *s)  { (void)q; (void)s; return -1; }
int ASensorEventQueue_disableSensor(void *q, void *s) { (void)q; (void)s; return 0; }
int ASensorEventQueue_setEventRate(void *q, void *s, int32_t u) { (void)q; (void)s; (void)u; return 0; }
int ASensorEventQueue_hasEvents(void *q) { (void)q; return 0; }
ssize_t ASensorEventQueue_getEvents(void *q, void *e, size_t n) { (void)q; (void)e; (void)n; return 0; }
const char *ASensor_getName(void *s) { (void)s; return "none"; }
int   ASensor_getType(void *s)       { (void)s; return 0; }
float ASensor_getResolution(void *s) { (void)s; return 0.0f; }
int   ASensor_getMinDelay(void *s)   { (void)s; return 0; }

/* ------------------------------------------------------------------------ */
/* Startup                                                                   */
/* ------------------------------------------------------------------------ */
typedef void (*create_fn)(pd_activity *, void *, size_t);

int native_glue_start(so_module *mod, const char *internal_path,
                      const char *external_path, const char *obb_path) {
    create_fn create = (create_fn)so_symbol(mod, "ANativeActivity_onCreate");
    if (!create) {
        l_error("ANativeActivity_onCreate not found in the module");
        return -1;
    }

    g_window.width  = settings_display_width();
    g_window.height = settings_display_height();

    memset(&g_callbacks, 0, sizeof(g_callbacks));
    memset(&g_activity, 0, sizeof(g_activity));
    g_activity.callbacks        = &g_callbacks;
    g_activity.vm               = (void *)&jvm;
    g_activity.env              = (void *)&jni;
    /* TODO: replace by the real activity object once java.c defines it. */
    g_activity.clazz            = (void *)(*(&jni))->NewStringUTF(&jni, "activity");
    g_activity.internalDataPath = internal_path;
    g_activity.externalDataPath = external_path;
    g_activity.sdkVersion       = 21;
    g_activity.assetManager     = &g_asset_mgr_dummy;
    g_activity.obbPath          = obb_path;

    l_info("native_glue: ANativeActivity_onCreate (window %dx%d)", g_window.width, g_window.height);
    create(&g_activity, NULL, 0);   /* spawns the android_main thread and waits for it */
    l_info("native_glue: onCreate returned, app thread is running");

    pd_callbacks *cb = &g_callbacks;

    if (cb->onStart)  { cb->onStart(&g_activity);  l_info("native_glue: onStart done"); }
    if (cb->onResume) { cb->onResume(&g_activity); l_info("native_glue: onResume done"); }

    if (cb->onNativeWindowCreated) {
        cb->onNativeWindowCreated(&g_activity, &g_window);
        l_info("native_glue: onNativeWindowCreated done");
    }
    if (cb->onInputQueueCreated) {
        cb->onInputQueueCreated(&g_activity, &g_queue_obj);
        g_accept_input = 1;
        l_info("native_glue: onInputQueueCreated done");
    }
    if (cb->onWindowFocusChanged) {
        cb->onWindowFocusChanged(&g_activity, 1);
        l_info("native_glue: onWindowFocusChanged(1) done");
    }
    return 0;
}
