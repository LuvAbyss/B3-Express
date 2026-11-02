/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Inline keyboard lifecycle and fixed-size SDK replies, with a deterministic host dialog. */
#include "nx/hle/services/services_int.h"
#include <math.h>
#include <stdarg.h>

#include "nx/hle/services/am.c"

u8* g_mem;
int g_verbose;
int g_opt_docked, g_opt_lang = 1;
u64 g_title_id = 0x0100BF00112C0000ull;
PMutex g_klock;
static unsigned test_checks, test_objects, test_kobjs;
static u32 test_next_handle = 0x100;
u64 plat_ns(void) {
    static u64 now;
    return now += 16666666ull;
}

static void test_fail(const char* expression, int line) {
    fprintf(stderr, "am_service.c:%d: check failed: %s\n", line, expression);
    exit(1);
}
#define CHECK(expr)                                                                                                    \
    do {                                                                                                               \
        test_checks++;                                                                                                 \
        if (!(expr)) test_fail(#expr, __LINE__);                                                                       \
    } while (0)

void pm_init(PMutex* m) { (void)m; }
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }
void logf_(int level, const char* format, ...) {
    (void)level;
    (void)format;
}
void fatal(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    exit(1);
}

SvcObj* svcobj_new(const char* name, int (*dispatch)(SvcObj*, IpcCtx*), size_t size) {
    SvcObj* obj = calloc(1, sizeof *obj);
    CHECK(obj != NULL);
    obj->name = name;
    obj->dispatch = dispatch;
    obj->refs = 1;
    if (size) {
        obj->data = calloc(1, size);
        CHECK(obj->data != NULL);
    }
    test_objects++;
    return obj;
}
void svcobj_unref(SvcObj* obj) {
    if (!obj || --obj->refs) return;
    if (obj->destroy) obj->destroy(obj);
    free(obj->data);
    free(obj);
    test_objects--;
}
void out_obj(IpcCtx* c, SvcObj* obj) {
    CHECK(c->nout_objs < 8);
    c->out_objs[c->nout_objs++] = obj;
}
void out_copy_h(IpcCtx* c, u32 handle) {
    CHECK(c->nout_copy < 8);
    c->out_copy[c->nout_copy++] = handle;
}
void ipc_out_overflow(IpcCtx* c) {
    (void)c;
    test_fail("IPC output overflow", __LINE__);
}
/* sf's InvalidCmifRequest, for a request that doesn't send what its command declares */
int ipc_bad_request(SvcObj* o, IpcCtx* c) {
    (void)o, (void)c;
    return RC(11, 420);
}
/* sf's InvalidNumInObjects, for a domain request with another number of objects */
int ipc_bad_objects(SvcObj* o, IpcCtx* c) {
    (void)o, (void)c;
    return RC(10, 235);
}
int stub_ok(SvcObj* o, IpcCtx* c) {
    fprintf(stderr, "unexpected stub: %s command %u\n", o->name, c->cmd);
    test_fail("a command without a handler", __LINE__);
    return -1;
}
int quiet_ok(SvcObj* o, IpcCtx* c) { return stub_ok(o, c); }
void svc_register(const char* name, SvcCtor ctor) {
    (void)name;
    (void)ctor;
}

/* kernel objects: a handle takes a reference, as h_new does, and the test keeps count of what is alive */
void kobj_ref(KObj* o) { o->refs++; }
void kobj_unref(KObj* o) {
    CHECK(o->refs > 0);
    if (--o->refs) return;
    if (o->destroy) o->destroy(o);
    free(o);
    test_kobjs--;
}
KEvent* kevent_new(const char* name) {
    KEvent* e = calloc(1, sizeof *e);
    CHECK(e != NULL);
    e->o.type = KO_EVENT;
    e->o.name = name;
    test_kobjs++;
    return e;
}
void kevent_signal(KEvent* e) { e->signaled = 1; }
static void shm_destroy(KObj* o) { free(((KShmem*)o)->host); }
KShmem* kshmem_new(u64 size, const char* name) {
    KShmem* s = calloc(1, sizeof *s);
    CHECK(s != NULL);
    s->o.type = KO_SHMEM;
    s->o.name = name;
    s->o.destroy = shm_destroy;
    s->size = size;
    s->host = calloc(1, size);
    CHECK(s->host != NULL);
    test_kobjs++;
    return s;
}
u8* kshmem_ptr(KShmem* s) { return s->host; }
/* the handle table: the handle a test reads back names the object it was made for */
static KObj* test_handles[64];
u32 h_new(KObj* o) {
    CHECK(test_next_handle - 0x100 < 64);
    kobj_ref(o);
    test_handles[test_next_handle - 0x100] = o;
    return test_next_handle++;
}
static void close_handles(void) {
    for (u32 h = 0x100; h < test_next_handle; h++) kobj_unref(test_handles[h - 0x100]);
    test_next_handle = 0x100;
}
u32 ev_handle(KEvent* e) { return h_new(&e->o); }
int new_event(u32* event, const char* name, int signaled) {
    KEvent* e = kevent_new(name);
    e->signaled = signaled;
    *event = h_new(&e->o);
    return 0;
}
KEvent* owned_event(const char* name) {
    KEvent* e = kevent_new(name);
    kobj_ref(&e->o);
    return e;
}

void svcobj_ref(SvcObj* o) { o->refs++; }
SvcObj* ipc_in_obj(IpcCtx* c, int index) {
    (void)c;
    (void)index;
    return NULL;
}
void kevent_clear(KEvent* e) { e->signaled = 0; }
KObj* h_get_ref(u32 h, int type) {
    (void)h;
    (void)type;
    return NULL;
}
volatile int g_quit;
void set_language_code(int lang, u8 code[8]) {
    (void)lang;
    memcpy(code, "en-US", 6);
}
static int test_players = 1;
int hid_connected_players(u32* selected_id) {
    *selected_id = 0;
    return test_players;
}
static const char* dialog_text = "Aé😀";
static const char* dialog_then; /* what the dialog answers from its second time on, when set */
static int dialog_cancel, dialog_calls;
static int modern_packet;
int plat_text_input(char* out, size_t cap, const char* initial) {
    (void)initial;
    snprintf(out, cap, "%s", dialog_calls++ && dialog_then ? dialog_then : dialog_text);
    return dialog_cancel;
}
static void put32(u8* dst, u32 v) { memcpy(dst, &v, 4); }
static u32 get32(const u8* src) {
    u32 v;
    memcpy(&v, src, 4);
    return v;
}
static void send_request(SvcObj* a, u32 request, u64 flags, int utf8) {
    u8 packet[0x4EC] = {0};
    SvcObj* storage;
    put32(packet, request);
    memcpy(packet + 12, &flags, 8);
    put32(packet + 4 + (modern_packet ? 0x38 : 0x40), 20);
    packet[4 + (modern_packet ? 0x484 : 0x45C)] = (u8)utf8;
    storage = am_storage_new(modern_packet ? sizeof packet : 0x4A4, packet);
    CHECK(nn_am_service_ILibraryAppletAccessor_PushInteractiveInData(a, storage) == 0);
    svcobj_unref(storage);
}
static SvcObj* reply(SvcObj* a, u32 state, u32 type, u32 size) {
    SvcObj* storage = NULL;
    StorageData* data;
    CHECK(nn_am_service_ILibraryAppletAccessor_PopInteractiveOutData(a, &storage) == 0);
    data = storage->data;
    CHECK(data->size == size);
    CHECK(get32(data->d) == state);
    CHECK(get32(data->d + 4) == type);
    return storage;
}
static void test_inline(void) {
    SvcObj* a;
    SvcObj* s;
    LibApplet* la;
    StorageData* data;
    u8 completed;
    u32 handle;
    dialog_cancel = 0;
    CHECK(nn_am_service_ILibraryAppletCreator_CreateLibraryApplet(NULL, 0x11, 3, &a) == 0);
    la = a->data;
    CHECK(nn_am_service_ILibraryAppletAccessor_GetAppletStateChangedEvent(a, &handle) == 0);
    CHECK(!la->state_ev->signaled);
    CHECK(nn_am_service_ILibraryAppletAccessor_IsCompleted(a, &completed) == 0 && !completed);
    send_request(a, 0xA, 1, 0);
    CHECK(la->interactive_ev->signaled);
    svcobj_unref(reply(a, 1, 0, 9));
    CHECK(!la->interactive_ev->signaled);
    CHECK(nn_am_service_ILibraryAppletAccessor_PopInteractiveOutData(a, &s) == RC(128, 2));
    send_request(a, 0xA, 4, 0);
    svcobj_unref(reply(a, 3, 1, 8));
    svcobj_unref(reply(a, 3, 2, 0x404));
    s = reply(a, 3, 5, 0x3F8);
    data = s->data;
    CHECK(get32(data->d + 8 + 0x3EC) == 4);
    CHECK(data->d[8] == 'A' && data->d[10] == 0xE9);
    CHECK(data->d[12] == 0x3D && data->d[13] == 0xD8);
    svcobj_unref(s);
    svcobj_unref(reply(a, 1, 1, 8));
    CHECK(!la->state_ev->signaled);
    send_request(a, 0xA, 4 | 0x20, 1);
    svcobj_unref(reply(a, 3, 1, 8));
    svcobj_unref(reply(a, 3, 7, 0x7EC));
    s = reply(a, 3, 9, 0x7E0);
    CHECK(strcmp((char*)((StorageData*)s->data)->d + 8, dialog_text) == 0);
    svcobj_unref(s);
    svcobj_unref(reply(a, 1, 1, 8));
    dialog_cancel = 1;
    send_request(a, 0xA, 4, 1);
    svcobj_unref(reply(a, 3, 1, 8));
    svcobj_unref(reply(a, 3, 6, 8));
    svcobj_unref(reply(a, 1, 1, 8));
    send_request(a, 4, 0, 0);
    CHECK(la->state_ev->signaled);
    CHECK(nn_am_service_ILibraryAppletAccessor_IsCompleted(a, &completed) == 0 && completed);
    svcobj_unref(a);
    close_handles();
    CHECK(test_objects == 0 && test_kobjs == 0);
}
/* a library applet run to its end: its pushed storages, then what PopOutData gives */
static SvcObj* run_applet(u32 id, SvcObj* const* in, int nin) {
    SvcObj *a, *out = NULL;
    int i;
    CHECK(nn_am_service_ILibraryAppletCreator_CreateLibraryApplet(NULL, id, 0, &a) == 0);
    for (i = 0; i < nin; i++) CHECK(nn_am_service_ILibraryAppletAccessor_PushInData(a, in[i]) == 0);
    CHECK(nn_am_service_ILibraryAppletAccessor_PopOutData(a, &out) == 0 && out);
    CHECK(nn_am_service_ILibraryAppletAccessor_PopOutData(a, &out) == RC(128, 2)); /* given once */
    svcobj_unref(a);
    return out;
}
/* the standard keyboard: SwkbdConfigCommon's limits (max at 0x3ac, min at 0x3b0) and UTF-8 flag (0x3bd), the dialog
   asked again until the text is long enough, and the reply's SwkbdResult and text */
static SvcObj* swkbd(u32 max, u32 min, int utf8) {
    u8 config[0x3e0] = {0};
    SvcObj *in[2], *out;
    memcpy(config + 0x3ac, &max, 4);
    memcpy(config + 0x3b0, &min, 4);
    config[0x3bd] = (u8)utf8;
    in[0] = am_storage_new(0x20, NULL);
    in[1] = am_storage_new(sizeof config, config);
    dialog_calls = 0;
    out = run_applet(0x11, in, 2);
    svcobj_unref(in[0]);
    svcobj_unref(in[1]);
    CHECK(((StorageData*)out->data)->size == 0x7d8);
    return out;
}
static void test_swkbd(void) {
    SvcObj* out;
    u8* d;
    u16 units[4];
    dialog_cancel = 0;
    dialog_then = NULL;
    /* "Aé😀" is four UTF-16 units: three at most keeps the pair whole, cut before it */
    out = swkbd(3, 0, 0);
    d = ((StorageData*)out->data)->d;
    CHECK(get32(d) == 0 && d[4] == 'A' && d[6] == 0xE9 && d[7] == 0 && d[8] == 0 && d[9] == 0);
    svcobj_unref(out);
    out = swkbd(0, 0, 1);
    CHECK(!strcmp((char*)((StorageData*)out->data)->d + 4, "Aé😀"));
    svcobj_unref(out);
    /* too short for the minimum: asked again, and the second answer is taken */
    dialog_then = "hello";
    out = swkbd(0, 5, 1);
    CHECK(dialog_calls == 2 && get32(((StorageData*)out->data)->d) == 0);
    CHECK(!strcmp((char*)((StorageData*)out->data)->d + 4, "hello"));
    svcobj_unref(out);
    dialog_then = NULL;
    /* a minimum above the maximum is the maximum */
    out = swkbd(2, 9, 0);
    d = ((StorageData*)out->data)->d;
    CHECK(dialog_calls == 1 && get32(d) == 0 && d[4] == 'A' && d[6] == 0xE9 && d[8] == 0);
    svcobj_unref(out);
    /* cancelled */
    dialog_cancel = 1;
    out = swkbd(0, 0, 0);
    CHECK(get32(((StorageData*)out->data)->d) == 1 && ((StorageData*)out->data)->d[4] == 0);
    svcobj_unref(out);
    dialog_cancel = 0;
    /* malformed UTF-8 from the host: an overlong form, a surrogate and a cut sequence, each part U+FFFD */
    CHECK(am_utf8_to_16("\xC0\xAF", units, 4) == 2 && units[0] == 0xFFFD && units[1] == 0xFFFD);
    CHECK(am_utf8_to_16("\xED\xA0\x80", units, 4) == 3 && units[2] == 0xFFFD);
    CHECK(am_utf8_to_16("a\xE2\x82", units, 4) == 2 && units[0] == 'a' && units[1] == 0xFFFD);
    CHECK(am_utf8_to_16("\xF4\x90\x80\x80", units, 4) == 4); /* past U+10FFFF */
    CHECK(am_utf8_to_16("\xF0\x9F\x98\x80", units, 4) == 2 && units[0] == 0xD83D && units[1] == 0xDE00);
#ifndef _WIN32
    /* a scripted run's text, cancelled when it is shorter than the minimum */
    setenv("SNX_SWKBD_TEXT", "hi", 1);
    out = swkbd(0, 0, 1);
    CHECK(dialog_calls == 0 && get32(((StorageData*)out->data)->d) == 0);
    CHECK(!strcmp((char*)((StorageData*)out->data)->d + 4, "hi"));
    svcobj_unref(out);
    out = swkbd(0, 3, 1);
    CHECK(get32(((StorageData*)out->data)->d) == 1);
    svcobj_unref(out);
    unsetenv("SNX_SWKBD_TEXT");
#endif
}
/* the other applets' outputs: the controller applet's players against the game's minimum, a web applet's return
   value, and zeros from the rest */
static void test_applets(void) {
    u8 arg[0x40] = {2};
    SvcObj *in[2], *out;
    in[0] = am_storage_new(0x20, NULL);
    in[1] = am_storage_new(sizeof arg, arg);
    test_players = 1;
    out = run_applet(0x0c, in, 2);
    CHECK(((StorageData*)out->data)->size == 0xc && ((StorageData*)out->data)->d[0] == 1);
    CHECK(get32(((StorageData*)out->data)->d + 8) == 2); /* fewer than two: cancelled */
    svcobj_unref(out);
    test_players = 2;
    out = run_applet(0x0c, in, 2);
    CHECK(((StorageData*)out->data)->d[0] == 2 && get32(((StorageData*)out->data)->d + 8) == 0);
    svcobj_unref(out);
    test_players = 1;
    out = run_applet(0x14, in, 0); /* Shop */
    CHECK(((StorageData*)out->data)->size == 0x1010);
    svcobj_unref(out);
    out = run_applet(0x0e, in, 0); /* the error applet */
    CHECK(((StorageData*)out->data)->size == 0x10 && get32(((StorageData*)out->data)->d) == 0);
    svcobj_unref(out);
    svcobj_unref(in[0]);
    svcobj_unref(in[1]);
    close_handles();
}
/* am_init: the focus message, then the performance mode's, and the launchable event signaled */
static void test_messages(void) {
    nn_am_AppletMessage m = 0;
    u32 h;
    am_init();
    CHECK(nn_am_service_ICommonStateGetter_ReceiveMessage(NULL, &m) == 0 && m == 15);
    CHECK(am.msg_event->signaled);
    CHECK(nn_am_service_ICommonStateGetter_ReceiveMessage(NULL, &m) == 0 && m == 31);
    CHECK(!am.msg_event->signaled);
    CHECK(nn_am_service_ICommonStateGetter_ReceiveMessage(NULL, &m) == RC(128, 3));
    CHECK(nn_am_service_ISelfController_GetLibraryAppletLaunchableEvent(NULL, &h) == 0);
    CHECK(test_handles[h - 0x100] == &am.launchable_event->o && am.launchable_event->signaled);
    close_handles();
}
int main(void) {
    test_inline();
    modern_packet = 1;
    test_inline();
    test_swkbd();
    test_applets();
    CHECK(test_objects == 0 && test_kobjs == 0);
    test_messages();
    printf("am_service: %u checks passed\n", test_checks);
    return 0;
}
