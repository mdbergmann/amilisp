/*
 * platform_amiga_rexx.c — ARexx host port transport (AmigaOS 3+ / MorphOS).
 *
 * Gives amilisp a public exec message port that speaks the ARexx host
 * protocol, so an editor macro can drive the running Lisp:
 *
 *     OPTIONS RESULTS
 *     ADDRESS CLAMIGA 'LOAD Work:src/foo.lisp'
 *     SAY RESULT
 *
 * This file is transport ONLY — it moves command strings in and reply
 * strings out.  It knows nothing about Lisp: what a command *means* is
 * decided one layer up (lib/dev-commands.lisp, driven by the handler
 * thread in lib/amiga/arexx.lisp).  Keeping the split here is what lets
 * the command/diagnostics layer be tested on the host, where there is no
 * ARexx at all.
 *
 * Ownership rules that the API enforces rather than documents:
 *   - CreateMsgPort() allocates a signal bit for the CALLING task and sets
 *     mp_SigTask to it.  So the task that opens the port is the only task
 *     that may Wait on it — platform_arexx_wait() rejects a call from any
 *     other task instead of blocking forever on a signal that will never
 *     be delivered to it.  The handler thread therefore opens its own port.
 *   - Exactly one message is in flight at a time (single handler thread).
 *     wait() stashes it; reply() answers and clears it.  A handler that
 *     dies without replying would hang the ARexx sender forever, so the
 *     next wait() auto-replies the stale message with a failure code.
 *
 * ARexx reply protocol (rexx/storage.h, and why the rc/RESULT split is the
 * way it is): the interpreter only reads rm_Result2 as an argstring when
 * rm_Result1 is 0.  With a non-zero rm_Result1 the second field is a
 * numeric secondary code, so a result STRING cannot ride along with a
 * failure rc — attaching one would have ARexx free a pointer it never
 * allocated.  That is a protocol constraint, not a choice; the command
 * layer works with it by keeping the last reply fetchable (LASTRESULT).
 */

#include "platform.h"

#ifdef PLATFORM_AMIGA

#include <exec/types.h>
#include <exec/ports.h>
#include <exec/tasks.h>
#include <proto/exec.h>
#include <rexx/storage.h>
#include <rexx/rxslib.h>
#include <proto/rexxsyslib.h>
#include <string.h>
#include <stdio.h>

/* Library base for the CreateArgstring/CreateRexxMsg inline macros.  The
 * definition must match the extern in proto/rexxsyslib.h, and the two SDKs
 * disagree: the m68k NDK declares `struct RxsLib *`, the MorphOS SDK
 * `struct Library *`. */
#ifdef PLATFORM_MORPHOS
typedef struct Library RexxSysBase_t;
#else
typedef struct RxsLib RexxSysBase_t;
#endif
RexxSysBase_t *RexxSysBase = NULL;

/* GC stop-the-world cooperation (core/thread.c).  Forward-declared rather
 * than #included so the platform layer stays free of core/VM types — same
 * pattern as the socket/stdin blocking calls in platform_amiga.c.  Every
 * Wait() below is bracketed: a thread parked in Wait cannot reach a GC
 * safepoint, and a stop-the-world GC would wait on it forever. */
extern void cl_gc_enter_safe_region(void);
extern void cl_gc_leave_safe_region(void);

#define REXX_PORT_NAME_MAX 64

static struct MsgPort  *rexx_port = NULL;
static char             rexx_port_name[REXX_PORT_NAME_MAX];
static struct RexxMsg  *rexx_pending = NULL;   /* received, not yet replied */
static struct Task     *rexx_owner = NULL;     /* task that owns the port */
static BYTE             rexx_wake_sig = -1;    /* wake bit for close() */
static volatile int     rexx_stop = 0;

/* rexxsyslib is shared by every thread of the process: the open port holds
 * it for its lifetime, and each send() (from any thread) and each reply
 * holds it for the span of its calls.  RexxSysBase is one global that every
 * CreateArgstring/DeleteRexxMsg macro re-reads at the call, so the library
 * may be closed only when NOBODY is between an acquire and its release.
 * The old scheme opened it once and closed it from whichever side finished
 * first -- the port owner's close(), or another thread's send() -- while a
 * send() was still parked waiting for its reply.  That sender then called
 * DeleteArgstring through a NULL/expunged base and jumped into garbage: the
 * "68k exception at 0x4e" in CL-Thread at the end of the Clamacs drive,
 * where the editor closes its port while its REPL thread waits on a send.
 * Every count change is Forbid()ed: several threads acquire and release. */
static uint32_t rexx_lib_users = 0;

/* Take a reference on rexxsyslib, opening it on first use.  Returns 0 when
 * the library is not available (nothing was taken). */
static int rexx_acquire_lib(void)
{
    int have;
    Forbid();
    if (!RexxSysBase)
        RexxSysBase = (RexxSysBase_t *)OpenLibrary((STRPTR)RXSNAME, 0);
    have = RexxSysBase != NULL;
    if (have)
        rexx_lib_users++;
    Permit();
    return have;
}

/* Drop a reference; the last one closes the library.  A process that only
 * drives other applications (no port of its own) must not keep it open --
 * an AmigaOS library left open cannot be expunged. */
static void rexx_release_lib(void)
{
    Forbid();
    if (rexx_lib_users > 0 && --rexx_lib_users == 0 && RexxSysBase) {
        CloseLibrary((struct Library *)RexxSysBase);
        RexxSysBase = NULL;
    }
    Permit();
}

uint32_t platform_arexx_lib_users(void)
{
    return rexx_lib_users;
}

const char *platform_arexx_strerror(int code)
{
    switch (code) {
    case PLATFORM_AREXX_OK:          return "ok";
    case PLATFORM_AREXX_ALREADY:     return "an ARexx port is already open in this process";
    case PLATFORM_AREXX_NOLIB:       return "rexxsyslib.library is not available "
                                            "(ARexx is not installed on this system)";
    case PLATFORM_AREXX_NOMEM:       return "out of memory creating the message port";
    case PLATFORM_AREXX_NONAME:      return "no free port name (tried BASE and BASE.1 .. BASE.99)";
    case PLATFORM_AREXX_NOTOWNER:    return "the ARexx port must be waited on by the task that opened it";
    case PLATFORM_AREXX_NOTOPEN:     return "no ARexx port is open";
    default:                         return "unknown ARexx error";
    }
}

int platform_arexx_open(const char *basename, char *name_out, int name_size)
{
    char base[REXX_PORT_NAME_MAX];
    int i, n;

    if (rexx_port)
        return PLATFORM_AREXX_ALREADY;

    /* ARexx upcases the host name in `ADDRESS <name>` before FindPort, so a
     * lowercase port name is simply unreachable from a macro.  Upcase here
     * rather than making every caller remember that. */
    for (i = 0; i < REXX_PORT_NAME_MAX - 8 && basename[i]; i++) {
        char c = basename[i];
        base[i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
    }
    base[i] = '\0';
    if (i == 0)
        return PLATFORM_AREXX_NONAME;

    /* The port's own reference, released by platform_arexx_close(). */
    if (!rexx_acquire_lib())
        return PLATFORM_AREXX_NOLIB;

    rexx_port = CreateMsgPort();
    if (!rexx_port) {
        rexx_release_lib();
        return PLATFORM_AREXX_NOMEM;
    }

    rexx_wake_sig = AllocSignal(-1L);
    if (rexx_wake_sig < 0) {
        DeleteMsgPort(rexx_port);
        rexx_port = NULL;
        rexx_release_lib();
        return PLATFORM_AREXX_NOMEM;
    }

    /* Claim a unique public name.  FindPort + AddPort must be atomic against
     * a second amilisp starting concurrently, hence the Forbid. */
    Forbid();
    for (n = 0; n < 100; n++) {
        if (n == 0)
            snprintf(rexx_port_name, sizeof(rexx_port_name), "%s", base);
        else
            snprintf(rexx_port_name, sizeof(rexx_port_name), "%s.%d", base, n);
        if (!FindPort((STRPTR)rexx_port_name))
            break;
    }
    if (n >= 100) {
        Permit();
        FreeSignal(rexx_wake_sig);
        rexx_wake_sig = -1;
        DeleteMsgPort(rexx_port);
        rexx_port = NULL;
        rexx_release_lib();
        return PLATFORM_AREXX_NONAME;
    }
    rexx_port->mp_Node.ln_Name = rexx_port_name;
    rexx_port->mp_Node.ln_Pri  = 0;
    AddPort(rexx_port);
    Permit();

    rexx_owner = FindTask(NULL);
    rexx_stop = 0;
    rexx_pending = NULL;

    if (name_out && name_size > 0)
        snprintf(name_out, (size_t)name_size, "%s", rexx_port_name);
    return PLATFORM_AREXX_OK;
}

int platform_arexx_is_open(void)
{
    return rexx_port != NULL;
}

int platform_arexx_port_name(char *buf, int bufsize)
{
    if (!rexx_port || !buf || bufsize <= 0)
        return 0;
    snprintf(buf, (size_t)bufsize, "%s", rexx_port_name);
    return 1;
}

void platform_arexx_request_stop(void)
{
    rexx_stop = 1;
    /* Kick the handler out of Wait().  The Forbid closes the window between
     * reading rexx_owner and signalling it: without it the handler could
     * finish platform_arexx_close() and exit in between, and Signal() would
     * be aimed at a freed Task.  Task exit needs a task switch, which Forbid
     * suspends -- and platform_arexx_close() clears rexx_owner under the same
     * Forbid, so either we see the pointer and the task is still there, or we
     * see NULL and do nothing. */
    Forbid();
    if (rexx_owner && rexx_wake_sig >= 0)
        Signal(rexx_owner, 1UL << rexx_wake_sig);
    Permit();
}

int platform_arexx_stop_requested(void)
{
    return rexx_stop;
}

int platform_arexx_wait(const char **cmd_out)
{
    struct RexxMsg *rm;
    ULONG mask;

    if (cmd_out)
        *cmd_out = NULL;
    if (!rexx_port)
        return PLATFORM_AREXX_NOTOPEN;
    if (FindTask(NULL) != rexx_owner)
        return PLATFORM_AREXX_NOTOWNER;

    /* A previous command whose handler blew up without replying: answer it
     * now rather than leaving the ARexx side blocked forever. */
    if (rexx_pending)
        platform_arexx_reply(PLATFORM_AREXX_RC_FATAL, NULL, 0);

    mask = 1UL << rexx_port->mp_SigBit;
    if (rexx_wake_sig >= 0)
        mask |= 1UL << rexx_wake_sig;

    for (;;) {
        if (rexx_stop)
            return 0;
        rm = (struct RexxMsg *)GetMsg(rexx_port);
        if (rm)
            break;
        cl_gc_enter_safe_region();
        Wait(mask);
        cl_gc_leave_safe_region();
    }

    rexx_pending = rm;
    /* ARG0 is NULL for a message sent with no argument at all; the caller
     * sees it as an empty command rather than as "no message". */
    if (cmd_out)
        *cmd_out = (const char *)ARG0(rm);
    return 1;
}

void platform_arexx_reply(int32_t rc, const char *result, uint32_t result_len)
{
    struct RexxMsg *rm = rexx_pending;
    if (!rm)
        return;
    rexx_pending = NULL;

    rm->rm_Result1 = (LONG)rc;
    rm->rm_Result2 = 0;
    /* See the protocol note at the top: a result string is only legal with
     * rm_Result1 == 0, and only when the caller asked for one. */
    if (rc == 0 && result && (rm->rm_Action & RXFF_RESULT) && rexx_acquire_lib()) {
        UBYTE *as = CreateArgstring((STRPTR)result, (ULONG)result_len);
        if (as)
            rm->rm_Result2 = (LONG)as;
        rexx_release_lib();
    }
    ReplyMsg((struct Message *)rm);
}

void platform_arexx_close(void)
{
    struct MsgPort *port = rexx_port;
    struct RexxMsg *rm;

    if (!port)
        return;   /* never opened, or open() failed (it released its ref) */

    if (rexx_pending)
        platform_arexx_reply(PLATFORM_AREXX_RC_FATAL, NULL, 0);

    /* Remove the port and answer everything still queued (or arriving during
     * the removal) before the port memory goes away.  A dropped message is a
     * permanently hung ARexx script, so the drain is inside the Forbid that
     * makes the port unreachable. */
    Forbid();
    RemPort(port);
    while ((rm = (struct RexxMsg *)GetMsg(port)) != NULL) {
        rm->rm_Result1 = PLATFORM_AREXX_RC_FATAL;
        rm->rm_Result2 = 0;
        ReplyMsg((struct Message *)rm);
    }
    rexx_port = NULL;
    /* Cleared inside the Forbid so platform_arexx_request_stop() can never
     * observe a stale owner (see the comment there). */
    rexx_owner = NULL;
    Permit();

    DeleteMsgPort(port);
    if (rexx_wake_sig >= 0) {
        FreeSignal(rexx_wake_sig);
        rexx_wake_sig = -1;
    }
    rexx_port_name[0] = '\0';
    rexx_stop = 0;

    /* The port's reference.  A send() still waiting on its reply holds its
     * own, so the library stays open under it. */
    rexx_release_lib();
}

/* --- Sending (test harness / scripting from Lisp) ---------------------
 *
 * Builds and posts a real RXCOMM message, exactly as the ARexx interpreter
 * would, then waits for the reply.  This is what lets the Amiga test suite
 * exercise the full host protocol in-process — no RexxMast, no rx, no second
 * machine — while still testing the bytes that a real editor macro produces.
 */
int platform_arexx_send(const char *portname, const char *cmd,
                        int32_t *rc_out, char *result, int result_size,
                        int32_t *rc2_out)
{
    struct MsgPort *reply_port;
    struct MsgPort *dest;
    struct RexxMsg *rm;
    UBYTE *arg;

    if (rc_out)  *rc_out = PLATFORM_AREXX_RC_FATAL;
    if (rc2_out) *rc2_out = 0;
    if (result && result_size > 0) result[0] = '\0';

    /* This send's own reference, held until its last DeleteRexxMsg --
     * across the wait for the reply, which is when another thread (the
     * port owner closing, another send finishing) used to close it. */
    if (!rexx_acquire_lib())
        return PLATFORM_AREXX_NOLIB;

    reply_port = CreateMsgPort();
    if (!reply_port) {
        rexx_release_lib();
        return PLATFORM_AREXX_NOMEM;
    }

    rm = CreateRexxMsg(reply_port, NULL, (STRPTR)portname);
    if (!rm) {
        DeleteMsgPort(reply_port);
        rexx_release_lib();
        return PLATFORM_AREXX_NOMEM;
    }

    arg = CreateArgstring((STRPTR)cmd, (ULONG)strlen(cmd));
    if (!arg) {
        DeleteRexxMsg(rm);
        DeleteMsgPort(reply_port);
        rexx_release_lib();
        return PLATFORM_AREXX_NOMEM;
    }
    ARG0(rm) = (STRPTR)arg;
    rm->rm_Action = RXCOMM | RXFF_RESULT;

    Forbid();
    dest = FindPort((STRPTR)portname);
    if (dest)
        PutMsg(dest, (struct Message *)rm);
    Permit();

    if (!dest) {
        /* (APTR): the argstring parameter is UBYTE* on the m68k NDK but
         * STRPTR (char*) on MorphOS; void* converts to both silently. */
        DeleteArgstring((APTR)arg);
        ARG0(rm) = NULL;
        DeleteRexxMsg(rm);
        DeleteMsgPort(reply_port);
        rexx_release_lib();
        return PLATFORM_AREXX_NOPORT;
    }

    /* Wait for our own message to come back.  Bracketed for STW GC like every
     * other blocking wait in the runtime. */
    for (;;) {
        struct RexxMsg *got = (struct RexxMsg *)GetMsg(reply_port);
        if (got == rm)
            break;
        if (got)
            continue;   /* not ours — nothing else uses this private port */
        cl_gc_enter_safe_region();
        WaitPort(reply_port);
        cl_gc_leave_safe_region();
    }

    if (rc_out)
        *rc_out = (int32_t)rm->rm_Result1;
    if (rm->rm_Result1 == 0) {
        if (rm->rm_Result2) {
            const char *s = (const char *)rm->rm_Result2;
            if (result && result_size > 0)
                snprintf(result, (size_t)result_size, "%s", s);
            DeleteArgstring((APTR)rm->rm_Result2);
            rm->rm_Result2 = 0;
        }
    } else if (rc2_out) {
        *rc2_out = (int32_t)rm->rm_Result2;
    }

    DeleteArgstring((APTR)arg);
    ARG0(rm) = NULL;
    DeleteRexxMsg(rm);
    DeleteMsgPort(reply_port);
    rexx_release_lib();
    return PLATFORM_AREXX_OK;
}

#endif /* PLATFORM_AMIGA */
