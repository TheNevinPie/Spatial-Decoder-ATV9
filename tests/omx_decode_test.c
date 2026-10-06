// omx_decode_test.c — Harness Phase B: crash-localization tracer (device-side).
//
// Case-driven isolation (fresh process per case):
//   A: Loaded -> Idle, no buffers
//   B: Loaded -> Idle, output buffers allocated, no input
//   C: Idle -> Executing, buffers allocated, no Fill/Empty calls
//   D: Executing + FillThisBuffer x N, no compressed input
//   E: Executing + ONE input buffer (real AC-3 bytes, no EOS)
//   F: Full decode: all input + EOS, drain to output EOS
//
// Every OMX operation is traced (args + return code) with the component
// object/vptr snapshot, and every callback logs entry/exit plus full
// header state. stdout/stderr are unbuffered so a crash keeps the trace.
//
// Usage:
//   LD_LIBRARY_PATH=/data/local/tmp omx_decode_test <CASE:A-F> \
//       <adapter.so> <comp-name> <in.ac3> <out.raw> <ts.log>

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include <OMX_Component.h>
#include <OMX_Core.h>
#include <OMX_Index.h>
#include <OMX_Audio.h>

#define FACTORY_MANGLED \
    "_Z22createSoftOMXComponentPKcPK16OMX_CALLBACKTYPEPvPP17OMX_COMPONENTTYPE"

typedef void *(*FactoryFunc)(
        const char *, const OMX_CALLBACKTYPE *, OMX_PTR, OMX_COMPONENTTYPE **);

#define MAXBUFS 256

#define BUF_SIZE (64 * 1024)
#define IN_CHUNK_DEFAULT 8192
#define IN_CHUNK_MAX (64 * 1024)
static size_t g_in_chunk = IN_CHUNK_DEFAULT;
#define IN_CHUNK (g_in_chunk)
#define STATE_TIMEOUT_MS 5000
#define OBSERVE_MS 3000
#define EOS_TIMEOUT_MS 25000
static int g_eos_timeout_ms = EOS_TIMEOUT_MS;

typedef struct {
    pthread_mutex_t lock;
    int emptyDone;
    int fillDone;
    size_t outBytes;
    size_t outFrames;
    int outEos;
    long long firstTs;
    long long lastTs;
    int tsWentBackwards;
    int events;
    int flushDone;
    int portCmdDone;
    OMX_U32 portCmdData;
    FILE *raw;
    FILE *tslog;
    OMX_COMPONENTTYPE *comp;
    OMX_BUFFERHEADERTYPE *outHdrs[MAXBUFS];
    // Deferred re-fill queue. Production clients (MediaCodec) never
    // re-submit a buffer re-entrantly from inside FillBufferDone; the
    // framework still marks it owned-by-us until the return path
    // completes, so synchronous re-fill trips
    // CHECK(!buffer->mOwnedByUs). Main thread pumps this queue.
    OMX_BUFFERHEADERTYPE *pendingRefill[MAXBUFS];
    int nPendingRefill;
    // PERBUF_REUSE=1: production-faithful input cycling. The component
    // owns a submitted input buffer until EmptyBufferDone returns it;
    // the feed loop only (re-)submits buffers popped from this free-list
    // (MediaCodec dequeue semantics), instead of blind ring-cursor reuse.
    OMX_BUFFERHEADERTYPE *inFree[MAXBUFS];
    int nInFree;
    // RESUBMIT_DELAY_MS=D: a returned input buffer only becomes
    // re-submittable D ms after its EmptyBufferDone (measures whether
    // the framework's ownership clear lands late relative to the
    // callback — i.e. abort is a race vs structural).
    long long inFreeWhen[MAXBUFS];
    // FREE_EACH_FILL probe: stash EVERY filled header; main thread issues
    // freeBuffer on each (synchronous flag read). Independent of the
    // refill queue. Any abort here reads the live flag state.
    OMX_BUFFERHEADERTYPE *freeQueue[64];
    int nFreeQueue;
    // FREE_AFTER_FILL probe: stash first filled header for main-thread
    // freeBuffer() — a legal synchronous read of the framework's
    // per-buffer ownership flag (free CHECKs !owned, like re-fill does).
    OMX_BUFFERHEADERTYPE *probeHdr;
    int probeDone;
} Ctx;

static long long now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static const char *omx_err_name(OMX_ERRORTYPE e) {
    switch (e) {
        case OMX_ErrorNone: return "None";
        case OMX_ErrorInsufficientResources: return "InsufficientResources";
        case OMX_ErrorUndefined: return "Undefined";
        case OMX_ErrorInvalidComponentName: return "InvalidComponentName";
        case OMX_ErrorComponentNotFound: return "ComponentNotFound";
        case OMX_ErrorBadParameter: return "BadParameter";
        case OMX_ErrorNotImplemented: return "NotImplemented";
        case OMX_ErrorIncorrectStateTransition: return "IncorrectStateTransition";
        case OMX_ErrorIncorrectStateOperation: return "IncorrectStateOperation";
        case OMX_ErrorBadPortIndex: return "BadPortIndex";
        default: return "?";
    }
}

static void dump_obj(const char *tag, OMX_COMPONENTTYPE *comp) {
    void *priv = comp ? comp->pComponentPrivate : NULL;
    void *vptr = NULL;
    if (priv) {
        vptr = *(void * volatile *)priv;
    }
    printf("[trace] %s comp=%p priv=%p vptr=%p\n",
            tag, (void *)comp, priv, vptr);
}

static void dump_hdr(const char *tag, OMX_BUFFERHEADERTYPE *b) {
    if (!b) {
        printf("[trace] %s hdr=NULL\n", tag);
        return;
    }
    printf("[trace] %s hdr=%p pBuf=%p alloc=%u filled=%u off=%u flags=0x%x "
           "ts=%lld inPort=%u outPort=%u\n",
            tag, (void *)b, (void *)b->pBuffer, b->nAllocLen, b->nFilledLen,
            b->nOffset, b->nFlags, (long long)b->nTimeStamp,
            b->nInputPortIndex, b->nOutputPortIndex);
}

static OMX_ERRORTYPE onEvent(OMX_HANDLETYPE h, OMX_PTR app,
        OMX_EVENTTYPE e, OMX_U32 d1, OMX_U32 d2, OMX_PTR ed) {
    (void)h; (void)ed;
    Ctx *c = (Ctx *)app;
    printf("[cb] EventHandler enter event=%d d1=%u d2=%u\n", e, d1, d2);
    pthread_mutex_lock(&c->lock);
    c->events++;
    // OMX_EventCmdComplete(0) + OMX_CommandFlush(1): flush round-trip done.
    if (e == 0 && d1 == 1) {
        c->flushDone++;
    }
    // Any port disable/enable completion (d1==2/3): record latest.
    if (e == 0 && (d1 == 2 || d1 == 3)) {
        c->portCmdDone++;
        c->portCmdData = d2;
    }
    pthread_mutex_unlock(&c->lock);
    printf("[cb] EventHandler exit\n");
    return OMX_ErrorNone;
}

static OMX_ERRORTYPE onEmpty(OMX_HANDLETYPE h, OMX_PTR app,
        OMX_BUFFERHEADERTYPE *b) {
    (void)h;
    Ctx *c = (Ctx *)app;
    printf("[cb] EmptyBufferDone enter\n");
    dump_hdr("cb-empty", b);
    pthread_mutex_lock(&c->lock);
    c->emptyDone++;
    if (getenv("PERBUF_REUSE") && c->nInFree < MAXBUFS) {
        c->inFree[c->nInFree] = b;
        c->inFreeWhen[c->nInFree] = now_ms();
        c->nInFree++;
    }
    pthread_mutex_unlock(&c->lock);
    printf("[cb] EmptyBufferDone exit\n");
    return OMX_ErrorNone;
}

static OMX_ERRORTYPE onFill(OMX_HANDLETYPE h, OMX_PTR app,
        OMX_BUFFERHEADERTYPE *b) {
    (void)h;
    Ctx *c = (Ctx *)app;
    printf("[cb] FillBufferDone enter\n");
    dump_hdr("cb-fill", b);
    pthread_mutex_lock(&c->lock);
    c->fillDone++;
    if (b->nFilledLen > 0 && b->pBuffer != NULL) {
        if (c->raw) {
            fwrite(b->pBuffer + b->nOffset, 1, b->nFilledLen, c->raw);
        }
        c->outBytes += b->nFilledLen;
        c->outFrames += b->nFilledLen / 4;
    }
    if (c->fillDone == 1) {
        c->firstTs = (long long)b->nTimeStamp;
    }
    if (getenv("FREE_AFTER_FILL") && !c->probeDone) {
        c->probeHdr = b;
        c->probeDone = 1;
    }
    if (getenv("FREE_EACH_FILL") && c->nFreeQueue < 64) {
        c->freeQueue[c->nFreeQueue++] = b;
    }
    if ((long long)b->nTimeStamp < c->lastTs && c->fillDone > 1) {
        c->tsWentBackwards++;
    }
    c->lastTs = (long long)b->nTimeStamp;
    if (c->tslog) {
        fprintf(c->tslog, "fill #%d len=%u ts=%lld flags=0x%x\n",
                c->fillDone, b->nFilledLen, (long long)b->nTimeStamp, b->nFlags);
    }
    int eos = (b->nFlags & OMX_BUFFERFLAG_EOS) != 0;
    if (eos) {
        c->outEos = 1;
    } else if (c->nPendingRefill < MAXBUFS && !getenv("FREE_AFTER_FILL")) {
        // Deferred: main thread re-submits outside the callback.
        // (Skipped entirely under FREE_AFTER_FILL probe: no re-fill may
        // be posted, so the probe freeBuffer is the only flag reader.)
        c->pendingRefill[c->nPendingRefill++] = b;
    }
    OMX_COMPONENTTYPE *comp = c->comp;
    (void)comp;
    pthread_mutex_unlock(&c->lock);
    printf("[cb] FillBufferDone exit eos=%d (refill deferred)\n", eos);
    return OMX_ErrorNone;
}

// Pump deferred output re-fills from the main thread (production pattern).
static void pump_refills(Ctx *c) {    // NO_REFILL=1: consume each output buffer exactly once (isolates the
    // re-fill ownership question from decode/EOS evidence).
    if (getenv("NO_REFILL")) {
        pthread_mutex_lock(&c->lock);
        c->nPendingRefill = 0;
        pthread_mutex_unlock(&c->lock);
        return;
    }    for (;;) {
        OMX_BUFFERHEADERTYPE *b = NULL;
        pthread_mutex_lock(&c->lock);
        if (c->nPendingRefill > 0) {
            b = c->pendingRefill[--c->nPendingRefill];
        }
        pthread_mutex_unlock(&c->lock);
        if (!b) {
            return;
        }
        printf("[op] FillThisBuffer(deferred %p) enter\n", (void *)b);
        OMX_ERRORTYPE err = c->comp->FillThisBuffer(c->comp, b);
        printf("[op] FillThisBuffer(deferred) -> %s\n", omx_err_name(err));
        dump_obj("after-deferred-FillThisBuffer", c->comp);
    }
}

// Pump FREE_EACH_FILL probes: freeBuffer every filled header (synchronous
// flag read). A CHECK abort here reports the live flag state.
static int nFreedOk = 0;
static void pump_free(Ctx *c) {
    for (;;) {
        OMX_BUFFERHEADERTYPE *b = NULL;
        pthread_mutex_lock(&c->lock);
        if (c->nFreeQueue > 0) {
            b = c->freeQueue[--c->nFreeQueue];
        }
        pthread_mutex_unlock(&c->lock);
        if (!b) {
            return;
        }
        printf("[op] freeBuffer(probe %p) enter\n", (void *)b);
        OMX_ERRORTYPE err = c->comp->FreeBuffer(c->comp, 1, b);
        printf("[op] freeBuffer(probe) -> %s\n", omx_err_name(err));
        if (err == OMX_ErrorNone) {
            nFreedOk++;
        }
    }
}

// Pop one component-returned input buffer (blocking). NULL on timeout.
// Production-faithful: a buffer is only (re-)submitted after its
// EmptyBufferDone arrived (MediaCodec dequeue semantics).
static int g_takeCount = 0;
// Submitted-header log: a popped buffer is a resubmit iff its header
// was submitted before in this run (seeds vs callback-returns
// interleave, so take ordinals cannot distinguish them).
#define MAXSUBMITLOG 2048
static OMX_BUFFERHEADERTYPE *g_submitted[MAXSUBMITLOG];
static int g_nSubmitted = 0;
static OMX_BUFFERHEADERTYPE *take_free_input(Ctx *c) {
    long long t0 = now_ms();
    long delay = 0;
    const char *rd = getenv("RESUBMIT_DELAY_MS");
    if (rd) {
        delay = atol(rd);
    }
    // RESUBMIT_AFTER_FILLS=N: resubmits (headers submitted before in
    // this run) wait until at least N output fills have been delivered.
    // Decides whether the framework's input-ownership clear is coupled
    // to output progress.
    long afterFills = -1;
    const char *raf = getenv("RESUBMIT_AFTER_FILLS");
    if (raf) {
        afterFills = atol(raf);
    }
    for (;;) {
        pthread_mutex_lock(&c->lock);
        OMX_BUFFERHEADERTYPE *b = NULL;
        int fills = c->fillDone;
        // Scan for any eligible entry (fresh seeds must not starve
        // behind a gated resubmit on top of the LIFO stack).
        for (int s = c->nInFree - 1; s >= 0 && !b; s--) {
            OMX_BUFFERHEADERTYPE *top = c->inFree[s];
            int isResubmit = 0;
            for (int i = 0; i < g_nSubmitted; i++) {
                if (g_submitted[i] == top) {
                    isResubmit = 1;
                    break;
                }
            }
            long long age = now_ms() - c->inFreeWhen[s];
            if (age >= delay
                    && !(afterFills >= 0 && isResubmit
                        && fills < afterFills)) {
                b = top;
                c->inFree[s] = c->inFree[c->nInFree - 1];
                c->inFreeWhen[s] = c->inFreeWhen[c->nInFree - 1];
                c->nInFree--;
            }
        }
        pthread_mutex_unlock(&c->lock);
        if (b) {
            g_takeCount++;
            int seen = 0;
            for (int i = 0; i < g_nSubmitted; i++) {
                if (g_submitted[i] == b) {
                    seen = 1;
                    break;
                }
            }
            if (!seen && g_nSubmitted < MAXSUBMITLOG) {
                g_submitted[g_nSubmitted++] = b;
            }
            return b;
        }
        if (now_ms() - t0 > STATE_TIMEOUT_MS) {
            return NULL;
        }
        pump_refills(c);
        pump_free(c);
        usleep(5000);
    }
}

static int wait_state(Ctx *c, OMX_STATETYPE want, int timeoutMs) {
    long long t0 = now_ms();
    OMX_STATETYPE s = OMX_StateInvalid;
    OMX_ERRORTYPE err = OMX_ErrorNone;
    while (now_ms() - t0 < timeoutMs) {
        err = c->comp->GetState(c->comp, &s);
        if (err == OMX_ErrorNone && s == want) {
            printf("[trace] wait_state(%d) reached, err=%s\n",
                    want, omx_err_name(err));
            return 0;
        }
        usleep(10000);
    }
    printf("[trace] wait_state(%d) TIMEOUT last=%d err=%s\n",
            want, s, omx_err_name(err));
    return -1;
}

typedef struct {
    const char *libPath;
    const char *compName;
    const char *inPath;
    const char *outPath;
    const char *tsPath;
} Args;

static int stage_create(Ctx *ctx, Args *a) {
    void *handle = dlopen(a->libPath, RTLD_NOW);
    printf("[op] dlopen(%s) -> %p %s\n", a->libPath, handle,
            handle ? "ok" : dlerror());
    if (!handle) {
        return -1;
    }
    FactoryFunc factory = (FactoryFunc)dlsym(handle, FACTORY_MANGLED);
    printf("[op] dlsym(factory) -> %p\n", (void *)factory);
    if (!factory) {
        return -1;
    }

    memset(ctx, 0, sizeof(*ctx));
    pthread_mutex_init(&ctx->lock, NULL);

    // LIFETIME REQUIREMENT (harness defect fix, Case B evidence):
    // SoftOMXComponent stores the OMX_CALLBACKTYPE *pointer* (it does not
    // copy the struct). A stack local that dies on return leaves
    // mCallbacks dangling; the first notify() then calls garbage as
    // EventHandler (observed: stale buffer-header address -> SEGV_ACCERR
    // on NX heap). Production (ACodec) uses a long-lived struct; the
    // harness must too. Hence function-static lifetime here.
    static OMX_CALLBACKTYPE cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.EventHandler = onEvent;
    cbs.EmptyBufferDone = onEmpty;
    cbs.FillBufferDone = onFill;

    OMX_COMPONENTTYPE *comp = NULL;
    printf("[op] factory(%s) enter\n", a->compName);
    void *obj = factory(a->compName, &cbs, ctx, &comp);
    printf("[op] factory -> obj=%p comp=%p\n", obj, (void *)comp);
    if (!obj || !comp) {
        return -1;
    }
    // HOLD_STRONG=1 emulates SoftOMXPlugin ownership: it calls
    // codec->incStrong(this) immediately after the factory returns and
    // holds that ref until destroyComponentInstance. Without it, the
    // framework's wp<>-promote/decStrong pair in AHandlerReflector can
    // delete the zero-strong-ref object on the first processed command.
    // Harness instrumentation only; NOT part of the adapter.
    if (getenv("HOLD_STRONG")) {
        void *ulog = dlopen("libutils.so", RTLD_NOW);
        printf("[op] HOLD_STRONG: libutils=%p\n", ulog);
        if (ulog) {
            void (*incStrong)(const void *, const void *) =
                (void (*)(const void *, const void *))
                dlsym(ulog, "_ZNK7android7RefBase9incStrongEPKv");
            printf("[op] HOLD_STRONG: incStrong=%p\n", (void *)incStrong);
            if (incStrong) {
                static int token;
                incStrong(obj, &token);
                printf("[op] HOLD_STRONG: incStrong(obj) done\n");
            }
        }
    }
    ctx->comp = comp;
    dump_obj("after-create", comp);

    OMX_STATETYPE s = OMX_StateInvalid;
    OMX_ERRORTYPE err = comp->GetState(comp, &s);
    printf("[op] GetState -> %s state=%d\n", omx_err_name(err), s);
    return (err == OMX_ErrorNone && s == OMX_StateLoaded) ? 0 : -1;
}

static int stage_to_idle(Ctx *ctx) {
    OMX_COMPONENTTYPE *comp = ctx->comp;
    printf("[op] SendCommand(StateSet,Idle) enter\n");
    OMX_ERRORTYPE err = comp->SendCommand(comp, OMX_CommandStateSet,
            OMX_StateIdle, NULL);
    printf("[op] SendCommand(StateSet,Idle) -> %s\n", omx_err_name(err));
    dump_obj("after-SendCommand-Idle", comp);
    if (err != OMX_ErrorNone) {
        return -1;
    }
    return wait_state(ctx, OMX_StateIdle, STATE_TIMEOUT_MS);
}

static int g_nIn = 4;
static int g_nOut = 4;

static int stage_alloc(Ctx *ctx, OMX_BUFFERHEADERTYPE **inHdrs, int nIn) {
    OMX_COMPONENTTYPE *comp = ctx->comp;
    // Production-faithful: size AND COUNT buffers from the port
    // definitions (no-recycle discipline needs count headroom).
    OMX_U32 portSize[2] = { BUF_SIZE, BUF_SIZE };
    // OVERRIDE_IN_SIZE=N: force input allocation size (tests whether the
    // ownership lifecycle depends on buffer sizing; larger is always legal:
    // useBuffer only rejects size < nBufferSize).
    const char *ovr = getenv("OVERRIDE_IN_SIZE");
    if (ovr) {
        portSize[0] = (OMX_U32)atoi(ovr);
        printf("[op] OVERRIDE_IN_SIZE active: in=%u\n", portSize[0]);
    }
    for (int p = 0; p < 2; p++) {
        OMX_PARAM_PORTDEFINITIONTYPE def;
        memset(&def, 0, sizeof(def));
        def.nSize = sizeof(def);
        def.nVersion.s.nVersionMajor = 1;
        def.nPortIndex = (OMX_U32)p;
        OMX_ERRORTYPE gerr = comp->GetParameter(comp,
                (OMX_INDEXTYPE)0x02000001, &def);
        printf("[op] GetParameter(PortDefinition port %d) -> %s "
               "count=%u size=%u\n",
                p, omx_err_name(gerr), def.nBufferCountActual,
                def.nBufferSize);
        if (gerr == OMX_ErrorNone && def.nBufferSize > portSize[p]) {
            portSize[p] = def.nBufferSize;
        }
        if (gerr == OMX_ErrorNone && def.nBufferCountActual > 0) {
            int want = (int)def.nBufferCountActual;
            if (want > MAXBUFS) {
                want = MAXBUFS;
            }
            if (p == 0) {
                g_nIn = want;
            } else {
                g_nOut = want;
            }
        }
    }
    if (nIn > g_nIn) {
        nIn = g_nIn;
    }
    printf("[trace] buffer counts: in=%d out=%d\n", g_nIn, g_nOut);
    for (int i = 0; i < nIn; i++) {
        printf("[op] AllocateBuffer(in %d, %u bytes) enter\n", i, portSize[0]);
        OMX_ERRORTYPE err = comp->AllocateBuffer(comp, &inHdrs[i], 0, NULL,
                portSize[0]);
        printf("[op] AllocateBuffer(in %d) -> %s\n", i, omx_err_name(err));
        dump_hdr("alloc-in", inHdrs[i]);
        dump_obj("after-alloc-in", comp);
        if (err != OMX_ErrorNone) {
            return -1;
        }
    }
    for (int i = 0; i < g_nOut; i++) {
        printf("[op] AllocateBuffer(out %d, %u bytes) enter\n", i, portSize[1]);
        OMX_ERRORTYPE err = comp->AllocateBuffer(comp, &ctx->outHdrs[i], 1,
                NULL, portSize[1]);
        printf("[op] AllocateBuffer(out %d) -> %s\n", i, omx_err_name(err));
        dump_hdr("alloc-out", ctx->outHdrs[i]);
        dump_obj("after-alloc-out", comp);
        if (err != OMX_ErrorNone) {
            return -1;
        }
    }
    return 0;
}

static int stage_to_executing(Ctx *ctx) {
    OMX_COMPONENTTYPE *comp = ctx->comp;
    printf("[op] SendCommand(StateSet,Executing) enter\n");
    OMX_ERRORTYPE err = comp->SendCommand(comp, OMX_CommandStateSet,
            OMX_StateExecuting, NULL);
    printf("[op] SendCommand(StateSet,Executing) -> %s\n", omx_err_name(err));
    dump_obj("after-SendCommand-Executing", comp);
    if (err != OMX_ErrorNone) {
        return -1;
    }
    return wait_state(ctx, OMX_StateExecuting, STATE_TIMEOUT_MS);
}

static void observe_ms(Ctx *ctx, int ms) {
    printf("[trace] observing %d ms\n", ms);
    long long t0 = now_ms();
    while (now_ms() - t0 < ms) {
        pump_refills(ctx);
        pump_free(ctx);
        usleep(20000);
    }
    pump_refills(ctx);
    pump_free(ctx);
}

static void print_evidence(Ctx *ctx, int sentBufs) {
    pthread_mutex_lock(&ctx->lock);
    printf("EVIDENCE emptyDone=%d sentBufs=%d\n", ctx->emptyDone, sentBufs);
    printf("EVIDENCE fillDone=%d outBytes=%zu outFrames=%zu\n",
            ctx->fillDone, ctx->outBytes, ctx->outFrames);
    printf("EVIDENCE outEos=%d events=%d tsFirst=%lld tsLast=%lld "
           "tsBackwards=%d freedOk=%d\n",
            ctx->outEos, ctx->events, ctx->firstTs, ctx->lastTs,
            ctx->tsWentBackwards, nFreedOk);
    pthread_mutex_unlock(&ctx->lock);
}

int main(int argc, char **argv) {
    if (argc != 7) {
        fprintf(stderr, "usage: %s <CASE:A-F> <adapter.so> <comp-name> "
                "<in.ac3> <out.raw> <ts.log>\n", argv[0]);
        return 2;
    }
    char kase = argv[1][0];
    Args a = { argv[2], argv[3], argv[4], argv[5], argv[6] };

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    const char *ich = getenv("IN_CHUNK_BYTES");
    if (ich) {
        long v = atol(ich);
        if (v >= 1024 && v <= (long)IN_CHUNK_MAX) { g_in_chunk = (size_t)v; }
        printf("[op] IN_CHUNK active: %lu\n", (unsigned long)g_in_chunk);
    }
    const char *eto = getenv("EOS_TIMEOUT_OVERRIDE_MS");
    if (eto) {
        long v = atol(eto);
        if (v >= 5000 && v <= 300000) { g_eos_timeout_ms = (int)v; }
        printf("[op] EOS timeout override: %d ms\n", g_eos_timeout_ms);
    }
    // START_DELAY_MS: sleep before dlopen so a debugger can attach and
    // set breakpoints before any OMX traffic. Timing only; no behavior.
    const char *sdelay = getenv("START_DELAY_MS");
    if (sdelay) {
        int ms = atoi(sdelay);
        printf("startup delay %d ms\n", ms);
        usleep(ms * 1000);
    }
    printf("CASE %c start lib=%s comp=%s\n", kase, a.libPath, a.compName);

    Ctx ctx;
    if (stage_create(&ctx, &a) != 0) {
        printf("CASE %c FAIL at create\n", kase);
        fflush(stdout);
        _exit(1);
    }
    OMX_BUFFERHEADERTYPE *inHdrs[MAXBUFS];
    memset(inHdrs, 0, sizeof(inHdrs));

    if (kase == '0') {
        // A0: create only. No SendCommand. Observe, then a single GetState.
        // Discriminates "SendCommand processing kills it" from
        // "mere looper-thread existence + time kills it".
        observe_ms(&ctx, OBSERVE_MS);
        dump_obj("case-A0-end", ctx.comp);
        OMX_STATETYPE s = OMX_StateInvalid;
        OMX_ERRORTYPE err = ctx.comp->GetState(ctx.comp, &s);
        printf("[op] A0 single GetState -> %s state=%d\n",
                omx_err_name(err), s);
        if (err != OMX_ErrorNone || s != OMX_StateLoaded) {
            printf("CASE 0 FAIL\n");
            _exit(1);
        }
        printf("CASE 0 PASS\n");
        fflush(stdout);
        _exit(0);
    }

    if (kase == 'A') {
        // Idle command with no buffers: the Loaded->Idle transition
        // cannot complete (ports never populated), so the PASS criterion
        // is command acceptance + continued liveness, not Idle itself.
        if (stage_to_idle(&ctx) == 0) {
            printf("CASE A note: reached Idle without buffers (unexpected)\n");
        } else {
            printf("CASE A note: Idle correctly pending (ports unpopulated)\n");
        }
        observe_ms(&ctx, OBSERVE_MS);
        dump_obj("case-A-end", ctx.comp);
        OMX_STATETYPE s = OMX_StateInvalid;
        OMX_ERRORTYPE err = ctx.comp->GetState(ctx.comp, &s);
        printf("[op] final GetState -> %s state=%d\n", omx_err_name(err), s);
        if (err != OMX_ErrorNone) {
            printf("CASE A FAIL at final GetState\n");
            _exit(1);
        }
        printf("CASE A PASS\n");
        fflush(stdout);
        _exit(0);
    }

    // B..F allocate all buffers first (required to reach Idle).
    if (stage_alloc(&ctx, inHdrs, MAXBUFS) != 0) {
        printf("CASE %c FAIL at alloc\n", kase);
        _exit(1);
    }
    if (stage_to_idle(&ctx) != 0) {
        printf("CASE %c FAIL at to-idle\n", kase);
        _exit(1);
    }
    printf("state: Idle\n");

    if (kase == 'B') {
        observe_ms(&ctx, OBSERVE_MS);
        dump_obj("case-B-end", ctx.comp);
        print_evidence(&ctx, 0);
        printf("CASE B PASS\n");
        fflush(stdout);
        _exit(0);
    }

    if (stage_to_executing(&ctx) != 0) {
        printf("CASE %c FAIL at to-executing\n", kase);
        _exit(1);
    }
    printf("state: Executing\n");

    if (kase == 'C') {
        observe_ms(&ctx, OBSERVE_MS);
        dump_obj("case-C-end", ctx.comp);
        print_evidence(&ctx, 0);
        printf("CASE C PASS\n");
        fflush(stdout);
        _exit(0);
    }

    // D..F queue output buffers.
    for (int i = 0; i < g_nOut; i++) {
        printf("[op] FillThisBuffer(out %d %p) enter\n", i,
                (void *)ctx.outHdrs[i]);
        OMX_ERRORTYPE err = ctx.comp->FillThisBuffer(ctx.comp, ctx.outHdrs[i]);
        printf("[op] FillThisBuffer(out %d) -> %s\n", i, omx_err_name(err));
        dump_obj("after-FillThisBuffer", ctx.comp);
        if (err != OMX_ErrorNone) {
            printf("CASE %c FAIL at fill-this-buffer %d\n", kase, i);
            _exit(1);
        }
    }

    // DOUBLE_SUBMIT=1: immediately re-submit out0 WITHOUT any decode or
    // consume in between. Decides whether re-submit of a still-queued
    // buffer is silently dropped (skip) or CHECK-aborts.
    if (getenv("DOUBLE_SUBMIT")) {
        printf("[op] FillThisBuffer(DOUBLE out0 %p) enter\n",
                (void *)ctx.outHdrs[0]);
        OMX_ERRORTYPE derr = ctx.comp->FillThisBuffer(ctx.comp,
                ctx.outHdrs[0]);
        printf("[op] FillThisBuffer(DOUBLE) -> %s\n", omx_err_name(derr));
        dump_obj("after-double-submit", ctx.comp);
        observe_ms(&ctx, 4000);
        dump_obj("double-submit-end", ctx.comp);
        print_evidence(&ctx, 0);
        printf("DOUBLE_SUBMIT done (survival == silent drop)\n");
        fflush(stdout);
        _exit(0);
    }

    if (kase == 'D') {
        // DISABLE_PROBE=1: after fills are submitted, disable output port
        // 1, wait for completion, re-enable it, wait again, then re-fill
        // out0. Decides whether port disable/enable clears mOwnedByUs
        // (disable/enable-cycling viability for buffer recycling).
        if (getenv("DISABLE_PROBE")) {
            printf("[op] SendCommand(PortDisable, 1) enter\n");
            OMX_ERRORTYPE derr = ctx.comp->SendCommand(ctx.comp,
                    OMX_CommandPortDisable, 1, NULL);
            printf("[op] SendCommand(PortDisable, 1) -> %s\n",
                    omx_err_name(derr));
            // Wait for the disable to take effect as evidenced by the
            // RETURN of all submitted output buffers (fillDone), with or
            // without a CmdComplete event. The buffer returns are the
            // recycling signal; CmdComplete is informational.
            long long dt0 = now_ms();
            int ok = 0;
            while (now_ms() - dt0 < 10000) {
                int fd;
                pthread_mutex_lock(&ctx.lock);
                fd = ctx.fillDone;
                pthread_mutex_unlock(&ctx.lock);
                if (fd >= g_nOut) {
                    ok = 1;
                    break;
                }
                usleep(20000);
            }
            printf("[trace] disable returns observed: %d (fillDone quorum)\n",
                    ok);
            dump_obj("post-disable", ctx.comp);
            if (!ok) {
                printf("DISABLE_PROBE FAIL: buffers not returned\n");
                fflush(stdout);
                _exit(1);
            }
            printf("[op] SendCommand(PortEnable, 1) enter\n");
            derr = ctx.comp->SendCommand(ctx.comp,
                    OMX_CommandPortEnable, 1, NULL);
            printf("[op] SendCommand(PortEnable, 1) -> %s\n",
                    omx_err_name(derr));
            // Do not gate on the enable CmdComplete (completion events
            // for port transitions proved unreliable here); continued
            // decode progress is the real signal. Brief settle instead.
            usleep(500000);
            dump_obj("post-enable", ctx.comp);
            printf("[op] FillThisBuffer(post-enable out0 %p) enter\n",
                    (void *)ctx.outHdrs[0]);
            derr = ctx.comp->FillThisBuffer(ctx.comp, ctx.outHdrs[0]);
            printf("[op] FillThisBuffer(post-enable) -> %s\n",
                    omx_err_name(derr));
            dump_obj("after-post-enable-fill", ctx.comp);
            observe_ms(&ctx, 5000);
            dump_obj("disable-probe-end", ctx.comp);
            print_evidence(&ctx, 0);
            printf("DISABLE_PROBE done (survival == flags cleared)\n");
            fflush(stdout);
            _exit(0);
        }
        // FLUSH_PROBE=1: after fills are submitted, flush BOTH ports
        // (OMX_ALL), wait for completion, then re-fill out0. Decides
        // whether port-flush clears mOwnedByUs (flush-cycling viability).
        if (getenv("FLUSH_PROBE")) {
            printf("[op] SendCommand(Flush, ALL) enter\n");
            OMX_ERRORTYPE ferr = ctx.comp->SendCommand(ctx.comp,
                    OMX_CommandFlush, 0xFFFFFFFF, NULL);
            printf("[op] SendCommand(Flush, ALL) -> %s\n",
                    omx_err_name(ferr));
            long long ft0 = now_ms();
            int ok = 0;
            while (now_ms() - ft0 < 8000) {
                int fd;
                pthread_mutex_lock(&ctx.lock);
                fd = ctx.flushDone;
                pthread_mutex_unlock(&ctx.lock);
                if (fd > 0) {
                    ok = 1;
                    break;
                }
                // NOTE: deliberately NO pump_refills here: the only
                // re-submit in this probe must be the manual one below.
                usleep(20000);
            }
            printf("[trace] flush completion observed: %d\n", ok);
            dump_obj("post-flush", ctx.comp);
            if (!ok) {
                printf("FLUSH_PROBE FAIL: no flush completion\n");
                fflush(stdout);
                _exit(1);
            }
            printf("[op] FillThisBuffer(post-flush out0 %p) enter\n",
                    (void *)ctx.outHdrs[0]);
            ferr = ctx.comp->FillThisBuffer(ctx.comp, ctx.outHdrs[0]);
            printf("[op] FillThisBuffer(post-flush) -> %s\n",
                    omx_err_name(ferr));
            dump_obj("after-post-flush-fill", ctx.comp);
            observe_ms(&ctx, 5000);
            dump_obj("flush-probe-end", ctx.comp);
            print_evidence(&ctx, 0);
            printf("FLUSH_PROBE done (survival == flags cleared)\n");
            fflush(stdout);
            _exit(0);
        }
        // PROBE_FREE_OUT0=1: immediately freeBuffer(out0) after submit,
        // with no input ever fed. Baseline read of the submit-path flag:
        //   free returns None => flag false after submit
        //   CHECK abort        => flag true after submit
        if (getenv("PROBE_FREE_OUT0")) {
            printf("[op] freeBuffer(1, out0 %p) enter\n",
                    (void *)ctx.outHdrs[0]);
            OMX_ERRORTYPE ferr =
                    ctx.comp->FreeBuffer(ctx.comp, 1, ctx.outHdrs[0]);
            printf("[op] freeBuffer(out0) -> %s\n", omx_err_name(ferr));
            printf("PROBE free-after-submit returned %s\n",
                    omx_err_name(ferr));
            fflush(stdout);
            _exit(ferr == OMX_ErrorNone ? 0 : 1);
        }
        observe_ms(&ctx, OBSERVE_MS);
        dump_obj("case-D-end", ctx.comp);
        print_evidence(&ctx, 0);
        printf("CASE D PASS\n");
        fflush(stdout);
        _exit(0);
    }

    // E/F need input data + raw/ts outputs.
    ctx.raw = fopen(a.outPath, "wb");
    ctx.tslog = fopen(a.tsPath, "w");
    if (!ctx.raw || !ctx.tslog) {
        printf("CASE %c FAIL opening outputs\n", kase);
        _exit(1);
    }
    FILE *fin = fopen(a.inPath, "rb");
    if (!fin) {
        printf("CASE %c FAIL opening input\n", kase);
        _exit(1);
    }
    unsigned char *chunk = malloc(IN_CHUNK);
    size_t got = fread(chunk, 1, IN_CHUNK, fin);
    printf("[op] read %zu input bytes\n", got);

    // PERBUF_REUSE=1: seed the input free-list with the first
    // IN_BUF_COUNT (default 4) allocated buffers; only those cycle.
    // (IN_BUF_COUNT + 4096B chunks replays the extractor's 313x4096B
    // sample feed with production-style buffer reuse.)
    if (getenv("PERBUF_REUSE")) {
        int want = 4;
        const char *ibc = getenv("IN_BUF_COUNT");
        if (ibc) {
            want = atoi(ibc);
        }
        if (want < 1) {
            want = 1;
        }
        if (want > g_nIn) {
            want = g_nIn;
        }
        pthread_mutex_lock(&ctx.lock);
        ctx.nInFree = 0;
        for (int i = 0; i < want; i++) {
            ctx.inFree[ctx.nInFree] = inHdrs[i];
            ctx.inFreeWhen[ctx.nInFree] = 0;
            ctx.nInFree++;
        }
        pthread_mutex_unlock(&ctx.lock);
        printf("[op] PERBUF_REUSE active: %d input buffers cycling\n", want);
    }

    // E: exactly one input buffer, no EOS.
    OMX_BUFFERHEADERTYPE *first = inHdrs[0];
    if (getenv("PERBUF_REUSE")) {
        first = take_free_input(&ctx);
        if (!first) {
            printf("CASE %c FAIL: no free input buffer\n", kase);
            _exit(1);
        }
    }
    memcpy(first->pBuffer, chunk, got);
    first->nOffset = 0;
    first->nFilledLen = (OMX_U32)got;
    first->nTimeStamp = 0;
    first->nFlags = 0;
    dump_hdr("submit-in0", first);
    printf("[op] EmptyThisBuffer(in0) enter\n");
    OMX_ERRORTYPE err = ctx.comp->EmptyThisBuffer(ctx.comp, first);
    printf("[op] EmptyThisBuffer(in0) -> %s\n", omx_err_name(err));
    dump_obj("after-EmptyThisBuffer", ctx.comp);
    if (err != OMX_ErrorNone) {
        printf("CASE %c FAIL at empty-this-buffer\n", kase);
        _exit(1);
    }

    if (kase == 'E') {
        // FREE_AFTER_FILL probe driver: as soon as the first filled
        // header is stashed, attempt freeBuffer on it synchronously.
        //   free returns None  => flag was false (clear ran somewhere)
        //   CHECK abort        => flag was true (never cleared)
        // Either outcome is recorded; then stop (header is gone).
        long long probe_t0 = now_ms();
        while (getenv("FREE_AFTER_FILL") && now_ms() - probe_t0 < 15000) {
            OMX_BUFFERHEADERTYPE *ph = NULL;
            pthread_mutex_lock(&ctx.lock);
            if (ctx.probeHdr) {
                ph = ctx.probeHdr;
            }
            pthread_mutex_unlock(&ctx.lock);
            if (ph) {
                break;
            }
            pump_refills(&ctx);
            usleep(20000);
        }
        if (getenv("FREE_AFTER_FILL")) {
            OMX_BUFFERHEADERTYPE *ph = NULL;
            pthread_mutex_lock(&ctx.lock);
            ph = ctx.probeHdr;
            pthread_mutex_unlock(&ctx.lock);
            if (!ph) {
                printf("PROBE: no fill arrived, cannot probe\n");
                fflush(stdout);
                _exit(2);
            }
            printf("[op] freeBuffer(1, probe %p) enter\n", (void *)ph);
            OMX_ERRORTYPE ferr = ctx.comp->FreeBuffer(ctx.comp, 1, ph);
            printf("[op] freeBuffer(probe) -> %s\n", omx_err_name(ferr));
            printf("PROBE freeBuffer returned %s (flag was %s)\n",
                    omx_err_name(ferr),
                    ferr == OMX_ErrorNone ? "FALSE/clear" : "TRUE/held?");
            fflush(stdout);
            _exit(ferr == OMX_ErrorNone ? 0 : 1);
        }
        observe_ms(&ctx, 10000);
        // TWO_INPUTS=1: submit a second input chunk (tests whether the
        // recycled output buffer from the deferred re-fill produces a
        // second FillBufferDone — i.e., whether the re-fill message was
        // actually processed vs. still queued at verdict time).
        if (getenv("TWO_INPUTS")) {
            size_t got2 = fread(chunk, 1, IN_CHUNK, fin);
            printf("[op] read second chunk %zu bytes\n", got2);
            memcpy(inHdrs[1]->pBuffer, chunk, got2);
            inHdrs[1]->nOffset = 0;
            inHdrs[1]->nFilledLen = (OMX_U32)got2;
            inHdrs[1]->nTimeStamp = 20000;
            inHdrs[1]->nFlags = 0;
            dump_hdr("submit-in1", inHdrs[1]);
            err = ctx.comp->EmptyThisBuffer(ctx.comp, inHdrs[1]);
            printf("[op] EmptyThisBuffer(in1) -> %s\n", omx_err_name(err));
            observe_ms(&ctx, 8000);
        }
        // SETTLE_MS: extra alive time (ms) after observe so any pending
        // looper message MUST be processed before the verdict. Any async
        // abort will fire here instead of hiding behind _exit.
        const char *settle = getenv("SETTLE_MS");
        if (settle) {
            observe_ms(&ctx, atoi(settle));
        }
        dump_obj("case-E-end", ctx.comp);
        print_evidence(&ctx, 1);
        fclose(ctx.raw);
        fclose(ctx.tslog);
        printf("CASE E PASS\n");
        fflush(stdout);
        _exit(0);
    }

    // F: feed the rest of the file, EOS on the last buffer.
    long sent = (long)got;
    int sentBufs = 1, inCursor = 1;
    int feedDone = 0;
    while (!feedDone) {
        got = fread(chunk, 1, IN_CHUNK, fin);
        int last = (got < IN_CHUNK);
        // SAMPLE_CAP=N: stop after N data submits total (first submit +
        // loop submits), forcing EOS on the Nth. Replays a head-segment
        // of an extractor feed without buffer reuse (N <= pool).
        const char *scap = getenv("SAMPLE_CAP");
        if (scap && !last) {
            int cap = atoi(scap);
            if (cap > 0 && sentBufs + 1 >= cap) {
                last = 1;
            }
        }
        OMX_BUFFERHEADERTYPE *h;
        if (getenv("PERBUF_REUSE")) {
            h = take_free_input(&ctx);
            if (!h) {
                printf("CASE F FAIL: input stall (no free buffer)\n");
                _exit(1);
            }
        } else {
            long long wt0 = now_ms();
            for (;;) {
                int done;
                pthread_mutex_lock(&ctx.lock);
                done = ctx.emptyDone;
                pthread_mutex_unlock(&ctx.lock);
                if (sentBufs - done < g_nIn) {
                    break;
                }
                if (now_ms() - wt0 > STATE_TIMEOUT_MS) {
                    printf("CASE F FAIL: input stall\n");
                    _exit(1);
                }
                pump_refills(&ctx);
                pump_free(&ctx);
                usleep(5000);
            }
            h = inHdrs[inCursor];
            inCursor = (inCursor + 1) % g_nIn;
        }
        memcpy(h->pBuffer, chunk, got);
        h->nOffset = 0;
        h->nFilledLen = (OMX_U32)got;
        h->nTimeStamp = sentBufs * 20000;
        h->nFlags = last ? OMX_BUFFERFLAG_EOS : 0;
        dump_hdr("submit-in", h);
        printf("[op] EmptyThisBuffer enter\n");
        err = ctx.comp->EmptyThisBuffer(ctx.comp, h);
        printf("[op] EmptyThisBuffer -> %s\n", omx_err_name(err));
        if (err != OMX_ErrorNone) {
            printf("CASE F FAIL at empty-this-buffer\n");
            _exit(1);
        }
        if (getenv("LOCKSTEP")) {
            long long lt0 = now_ms();
            for (;;) {
                int dd;
                pthread_mutex_lock(&ctx.lock);
                dd = ctx.emptyDone;
                pthread_mutex_unlock(&ctx.lock);
                if (dd >= sentBufs + 1) break;
                if (now_ms() - lt0 > 10000) {
                    printf("LOCKSTEP stall\n");
                    break;
                }
                pump_refills(&ctx);
                pump_free(&ctx);
                usleep(2000);
            }
        }
        sent += (long)got;
        sentBufs++;
        if (last) {
            feedDone = 1;
        }
    }
    free(chunk);
    fclose(fin);
    printf("fed %ld bytes in %d buffers (last=EOS)\n", sent, sentBufs);

    long long t0 = now_ms();
    for (;;) {
        pthread_mutex_lock(&ctx.lock);
        int eos = ctx.outEos;
        int fills = ctx.fillDone;
        size_t bytes = ctx.outBytes;
        pthread_mutex_unlock(&ctx.lock);
        if (eos) {
            break;
        }
        if (now_ms() - t0 > g_eos_timeout_ms) {
            printf("CASE F FAIL: EOS timeout (fills=%d bytes=%zu)\n",
                    fills, bytes);
            fclose(ctx.raw);
            fclose(ctx.tslog);
            fflush(stdout);
            _exit(1);
        }
        pump_refills(&ctx);
        pump_free(&ctx);
        usleep(20000);
    }
    usleep(300000);
    dump_obj("case-F-end", ctx.comp);
    print_evidence(&ctx, sentBufs);
    int rc = 0;
    if (ctx.emptyDone != sentBufs) {
        printf("FAIL: input completion %d != %d sent\n",
                ctx.emptyDone, sentBufs);
        rc = 1;
    }
    if (ctx.outBytes == 0 || !ctx.outEos) {
        printf("FAIL: no output or no EOS\n");
        rc = 1;
    }
    if (ctx.tsWentBackwards != 0) {
        printf("FAIL: timestamps backwards %d times\n", ctx.tsWentBackwards);
        rc = 1;
    }
    if (rc == 0) {
        printf("CASE F PASS\n");
    }
    fclose(ctx.raw);
    fclose(ctx.tslog);
    fflush(stdout);
    _exit(rc);
    return rc;
}
