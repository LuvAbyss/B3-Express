/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* HIPC/CMIF requests as the services see them: the parsed request and its reply (IpcCtx), service objects, sessions
   and domains, and the service registry. ipc.c parses and answers; the dispatchers tools/swipc generates call the
   inline helpers below, so their names and signatures are part of the generator's output */
#ifndef SNX_IPC_H
#define SNX_IPC_H
#include "common/cpu/aarch64/cpu.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/hle/result.h" /* RC, RC_OK and the R_ helpers */
#include <string.h>

#define MAX_BUFS 8 /* descriptors of one kind a request may carry */

/* a buffer descriptor: A (send), B (receive), W (exchange), X (pointer) or C (receive list) */
typedef struct {
    u64 addr, size;
    u32 flags;
} IpcBuf;

struct SvcObj;

/* One request, parsed from the client's message (TLS's 0x100 bytes, or SendSyncRequestWithUserBuffer's buffer), and
   the reply the handler builds */
typedef struct IpcCtx {
    u8* msg;
    u64 msg_size;
    u32 type;
    u32 cmd;
    u32 token;
    const u8* raw; /* the arguments after the CMIF header */
    u32 raw_size;
    u64 pid;
    int has_pid;
    u32 copy_h[8]; /* a request carries copy handles only: the kernel refuses move handles */
    int ncopy;
    IpcBuf a[MAX_BUFS], b[MAX_BUFS], w[MAX_BUFS], x[MAX_BUFS], c[MAX_BUFS];
    int na, nb, nw, nx, nc;
    /* refusals, decided while parsing: kernel_rc as the kernel answers before any server sees the request, header_rc
       (sf's header checks) at once, domain_rc (sf's domain checks) once the target object is found */
    u32 kernel_rc;
    u32 domain_rc;
    u32 header_rc;
    int is_domain;
    u32 domain_obj;
    u32 in_objs[8];
    int nin_objs;
    int tipc;
    /* the reply */
    u32 rc;
    u8 out[0x100]; /* the raw data; writes are bounded by its sizeof */
    u32 out_size;
    u32 out_copy[8], out_move[8];
    int nout_copy, nout_move;
    struct SvcObj* out_objs[8];
    int nout_objs;
    struct KSession* sess;
} IpcCtx;

/* An object a session or a domain entry refers to: a service, or an interface one of its commands returned */
typedef struct SvcObj {
    const char* name;
    int (*dispatch)(struct SvcObj* self, IpcCtx* ctx);
    void (*destroy)(struct SvcObj* self);
    int refs;
    u32 ptrbuf; /* the server's pointer buffer size, which QueryPointerBufferSize answers (0x8000 unless set) */
    void* data;
    u64 u[8];
} SvcObj;

typedef struct KSession {
    KObj o; /* first, as in every kernel object */
    SvcObj* obj;
    /* NULL until ConvertCurrentObjectToDomain, then set once with an atomic store that other threads' requests load,
       and shared by the session's clones */
    struct Domain* domain;
    int closed; /* the server closed it: requests answer R_SESSIONCLOSED */
} KSession;

typedef struct Domain {
    SvcObj** obj;
    int n, cap, refs;
    PMutex lk;
} Domain;

typedef SvcObj* (*SvcCtor)(void);

SvcObj* svcobj_new(const char* name, int (*dispatch)(SvcObj*, IpcCtx*), size_t datasize);
void svcobj_ref(SvcObj* o);
void svcobj_unref(SvcObj* o);
u32 session_new(SvcObj* obj);
/* SendSyncRequestWithUserBuffer's buffer checked as the kernel locks it: 0, or the SVC's result */
u32 ipc_user_buffer(u64 addr, u64 size);
void ipc_handle(KThread* t, u32 handle, u64 msg_addr, u64 msg_size);
void ipc_out_overflow(IpcCtx* c); /* a reply that didn't fit: logged once */
void out_copy_h(IpcCtx* c, u32 h);
void out_move_h(IpcCtx* c, u32 h);
void out_obj(IpcCtx* c, SvcObj* o);
/* the answers to a request a command can't take, each logged: sf's InvalidCmifRequest, and InvalidNumInObjects (or
   NotSupported for objects sent on a session that isn't a domain) */
int ipc_bad_request(SvcObj* o, IpcCtx* c);
int ipc_bad_objects(SvcObj* o, IpcCtx* c);
SvcObj* ipc_in_obj(IpcCtx* c, int i); /* the request's object i, with a reference the caller drops */
/* for commands nothing implements: success logged, success silent, and sf's UnknownCommandId logged */
int stub_ok(SvcObj* o, IpcCtx* c);
int quiet_ok(SvcObj* o, IpcCtx* c);
int ipc_unknown(SvcObj* o, IpcCtx* c);
/* 1 for the first three times a (service, command) pair comes, among the first 512 pairs */
int ipc_warn_once(const char* name, u32 cmd);
void svc_register(const char* name, SvcCtor ctor);
SvcObj* svc_open(const char* name);
int svc_registered(const char* name);

/* 1 when n more bytes fit in the reply's raw data, else logs the overflow and returns 0 */
static inline int out_fits(IpcCtx* c, u32 n) {
    if (c->out_size <= sizeof c->out && n <= sizeof c->out - c->out_size) return 1;
    ipc_out_overflow(c);
    return 0;
}
/* appends v to the reply's raw data when it fits; dropped (and logged) otherwise */
static inline void out32(IpcCtx* c, u32 v) {
    if (!out_fits(c, 4)) return;
    memcpy(c->out + c->out_size, &v, 4);
    c->out_size += 4;
}
/* n bytes of the request's raw data at off, zeros when they are not all there */
static inline void ipc_in(IpcCtx* c, u32 off, void* dst, u32 n) {
    if (!n) return;
    if (off > c->raw_size || n > c->raw_size - off)
        memset(dst, 0, n);
    else
        memcpy(dst, c->raw + off, n);
}
/* n bytes into the reply at off. Unchecked: the generator lays out a command's outputs inside out */
static inline void ipc_out(IpcCtx* c, u32 off, const void* src, u32 n) {
    if (n) memcpy(c->out + off, src, n);
}
/* list[i] of a list of n, or an empty buffer past its end */
static inline IpcBuf ipc_buf(const IpcBuf* list, int n, int i) {
    IpcBuf none;
    if (i >= 0 && i < n) return list[i];
    memset(&none, 0, sizeof none);
    return none;
}
/* an AutoSelect buffer: the mapped descriptor when its address isn't 0, whatever its size, else the pointer one */
static inline IpcBuf ipc_buf_auto(const IpcBuf* map, int nmap, int i, const IpcBuf* ptr, int nptr, int j) {
    IpcBuf m = ipc_buf(map, nmap, i);
    return m.addr ? m : ipc_buf(ptr, nptr, j);
}
/* 1 when every X descriptor past the first nx is empty (libnx sends the unused half of an AutoSelect buffer) */
static inline int ipc_x_extra_empty(const IpcCtx* c, int nx) {
    int k;
    for (k = nx < 0 ? 0 : nx; k < c->nx && k < MAX_BUFS; k++)
        if (c->x[k].size) return 0;
    return 1;
}
/* sf's check of a request against the command's declaration: the pid flag, the A and B counts exactly, at least nx X
   descriptors with any more empty, no W, the copy handles exactly, and enough raw data (tipc_raw for a TIPC request,
   whose pid has no raw slot). C descriptors aren't checked */
static inline int ipc_counts(const IpcCtx* c, int pid, int na, int nb, int nx, int ncopy, u32 raw, u32 tipc_raw) {
    return (c->has_pid != 0) == (pid != 0) && c->na == na && c->nb == nb && c->nx >= nx && ipc_x_extra_empty(c, nx) &&
           c->nw == 0 && c->ncopy == ncopy && c->raw_size >= (c->tipc ? tipc_raw : raw);
}
/* a domain request carries exactly the objects the command declares */
static inline int ipc_objects(const IpcCtx* c, int nobj) { return !c->is_domain || c->nin_objs == nobj; }

/* the last 32 requests, for crash reports */
extern char g_ipc_ring[32][160];
extern u32 g_ipc_ring_n;
#endif
