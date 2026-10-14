/* Off-heap allocation leak tracer (opt-in, compiled to nothing otherwise).
 *
 * WHY THIS EXISTS
 *
 * amilisp allocates two very different kinds of memory.  Lisp objects live in
 * the GC arena, and ROOM accounts for every byte of it.  Everything else —
 * bytecode bodies, constants pools, compiler scratch, hash indexes, I/O
 * buffers, the JIT's code — is platform_alloc'd and invisible to every
 * statistic the runtime otherwise reports.
 *
 * On a host that gap is harmless: the kernel reclaims the whole address space
 * at exit.  On AmigaOS there is no such reclaim.  A block amilisp does not
 * hand back before the process ends is gone from the system pool until the
 * machine is rebooted, so a run that forgets 3 MB costs the user 3 MB of Fast
 * RAM per launch — which is exactly what was measured on a Vampire before the
 * shutdown paths were fixed.  A leak a host build cannot even observe is, on
 * the target platform, the most user-visible bug the runtime has.
 *
 * HOW TO USE IT
 *
 *   make host DEBUG_FLAGS=-DDEBUG_MEM_TRACK
 *   make -f Makefile.cross amiga DEBUG_FLAGS=-DDEBUG_MEM_TRACK
 *
 * Then run with CLAMIGA_MEM_DIAG=1.  At exit every block still outstanding is
 * reported, grouped by the file and line that allocated it, largest first —
 * the answer to "what did we forget to free?", not a hint toward it.
 *
 * HOW IT WORKS
 *
 * platform.h redirects platform_alloc / platform_free to the two functions
 * below, tagging each call site via __FILE__ / __LINE__.  Live blocks are
 * held in a SIDE TABLE keyed by pointer — deliberately not a header prefixed
 * to each allocation.  A header would have to be read back on free, and
 * amilisp legitimately hands platform_free pointers this tracer never issued:
 * platform_file_read returns a malloc'd (POSIX) or AllocVec'd (Amiga) buffer
 * that its caller releases with platform_free.  Reading a header off one of
 * those is a wild access before the start of the block.  A pointer lookup
 * simply misses instead, and the block is passed through to the real free.
 *
 * The table is fixed-size and allocation-free: the tracer must not perturb
 * what it measures, and it has to keep working after the allocator it wraps
 * has begun to fail.
 *
 * This file is the ONE place that must reach the real allocator, so it
 * defines CL_MEM_TRACK_IMPL before including platform.h to opt out of the
 * macros.  The platform back-ends do the same, since they define the
 * functions being wrapped.
 */

#define CL_MEM_TRACK_IMPL 1

#include "../platform/platform.h"
#include "../platform/platform_thread.h"
#include "mem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef DEBUG_MEM_TRACK

/* Live-block capacity.  A full boot holds on the order of 30k blocks (one per
 * bytecode body, constants pool, line map, &key array, JIT buffer...), so this
 * leaves roughly 2x headroom.  Overflow is reported rather than silently
 * mis-accounted. */
#define MT_CAP      65536
#define MT_BUCKETS  65536       /* power of two — index is a mask, not a mod */

typedef struct {
    void         *ptr;
    unsigned long size;
    const char   *file;         /* __FILE__ — static storage, compared by pointer */
    unsigned long line;
    int           next;         /* chain link, or -1 */
} MtNode;

static MtNode mt_nodes[MT_CAP];
static int    mt_buckets[MT_BUCKETS];
static int    mt_free_head = -1;
static int    mt_initialized = 0;

/* platform_alloc/platform_free are called from every OS thread amilisp
 * spawns (mp:make-thread workers allocate/free their own CL_Thread, VM
 * stack, frames, nlx stack), so the table below is genuinely shared,
 * mutable state across threads.  mt_init() itself runs from the first
 * platform_alloc of the process, always on the main thread before any
 * worker exists, so it is safe to create the mutex there without a
 * separate one-time-init guard. */
static void *mt_mutex = NULL;

/* One row per distinct call site. */
#define MT_SITES 512

typedef struct {
    const char   *file;
    unsigned long line;
    unsigned long live_bytes;
    unsigned long live_blocks;
} MtSite;

static MtSite mt_sites[MT_SITES];
static int    mt_site_count = 0;

static unsigned long mt_live_bytes = 0;
static unsigned long mt_live_blocks = 0;
static unsigned long mt_peak_bytes = 0;
static unsigned long mt_untracked_frees = 0;
static unsigned long mt_overflows = 0;

static void mt_init(void)
{
    int i;
    for (i = 0; i < MT_BUCKETS; i++) mt_buckets[i] = -1;
    for (i = 0; i < MT_CAP - 1; i++) mt_nodes[i].next = i + 1;
    mt_nodes[MT_CAP - 1].next = -1;
    mt_free_head = 0;
    platform_mutex_init(&mt_mutex);
    mt_initialized = 1;
}

static unsigned long mt_hash(const void *p)
{
    /* Allocator results are at least 8-byte aligned, so drop the dead low
     * bits before Knuth's golden-ratio multiplier. */
    unsigned long v = (unsigned long)(size_t)p >> 3;
    return (v * 2654435761UL) & (MT_BUCKETS - 1);
}

static MtSite *mt_site_for(const char *file, unsigned long line)
{
    int i;
    for (i = 0; i < mt_site_count; i++)
        if (mt_sites[i].line == line && mt_sites[i].file == file)
            return &mt_sites[i];
    if (mt_site_count >= MT_SITES) return NULL;
    mt_sites[mt_site_count].file = file;
    mt_sites[mt_site_count].line = line;
    mt_sites[mt_site_count].live_bytes = 0;
    mt_sites[mt_site_count].live_blocks = 0;
    return &mt_sites[mt_site_count++];
}

void *cl_mem_track_alloc(unsigned long size, const char *file, int line)
{
    void *p;
    unsigned long b;
    int idx;
    MtSite *s;

    /* CLAMIGA_MEM_FAIL_OVER=<bytes>: refuse every request of at least that
     * size, so the suite can drive an allocation-failure exit (the heap
     * arena on a 24 MB 68020) on a host whose allocator never says no. */
    {
        static long fail_over = -1;
        if (fail_over < 0) {
            /* platform_getenv, not getenv: libnix's getenv() is not safe
             * this early on AmigaOS (it crashed the process). */
            char ebuf[16];
            const char *e = platform_getenv("CLAMIGA_MEM_FAIL_OVER", ebuf,
                                            (int)sizeof(ebuf));
            fail_over = (e && *e) ? atol(e) : 0;
        }
        if (fail_over > 0 && size >= (unsigned long)fail_over)
            return NULL;
    }

    p = platform_alloc(size);
    if (!p) return NULL;
    if (!mt_initialized) mt_init();

    platform_mutex_lock(mt_mutex);

    if (mt_free_head < 0) {
        /* Table full: the block is real and must still be returned, it just
         * cannot be attributed.  Counted so the report says so. */
        mt_overflows++;
        platform_mutex_unlock(mt_mutex);
        return p;
    }
    idx = mt_free_head;
    mt_free_head = mt_nodes[idx].next;

    b = mt_hash(p);
    mt_nodes[idx].ptr  = p;
    mt_nodes[idx].size = size;
    mt_nodes[idx].file = file;
    mt_nodes[idx].line = (unsigned long)line;
    mt_nodes[idx].next = mt_buckets[b];
    mt_buckets[b] = idx;

    mt_live_bytes += size;
    mt_live_blocks++;
    if (mt_live_bytes > mt_peak_bytes) mt_peak_bytes = mt_live_bytes;

    s = mt_site_for(file, (unsigned long)line);
    if (s) { s->live_bytes += size; s->live_blocks++; }

    platform_mutex_unlock(mt_mutex);
    return p;
}

void cl_mem_track_free(void *ptr)
{
    unsigned long b;
    int idx, prev = -1, found = 0;

    if (!ptr) return;
    if (!mt_initialized) { platform_free(ptr); return; }

    platform_mutex_lock(mt_mutex);

    b = mt_hash(ptr);
    for (idx = mt_buckets[b]; idx >= 0; prev = idx, idx = mt_nodes[idx].next) {
        if (mt_nodes[idx].ptr != ptr) continue;

        if (prev < 0) mt_buckets[b] = mt_nodes[idx].next;
        else          mt_nodes[prev].next = mt_nodes[idx].next;

        mt_live_bytes -= mt_nodes[idx].size;
        mt_live_blocks--;
        {
            MtSite *s = mt_site_for(mt_nodes[idx].file, mt_nodes[idx].line);
            if (s) { s->live_bytes -= mt_nodes[idx].size; s->live_blocks--; }
        }
        mt_nodes[idx].ptr = NULL;
        mt_nodes[idx].next = mt_free_head;
        mt_free_head = idx;
        found = 1;
        break;
    }

    /* Not one of ours — a buffer the platform layer allocated directly (see
     * the header comment), or one issued while the table was full. */
    if (!found) mt_untracked_frees++;

    platform_mutex_unlock(mt_mutex);
    platform_free(ptr);
}

/* Strip the directory so the report stays readable in an 80-column shell. */
static const char *mt_basename(const char *path)
{
    const char *p, *base = path;
    if (!path) return "?";
    for (p = path; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    return base;
}

void cl_mem_track_report(void)
{
    char line[160];
    int i, n;

    if (!cl_mem_diag) return;

    snprintf(line, sizeof(line),
             "[mem] leak report: %lu block(s), %lu bytes still allocated "
             "(peak %lu)\n",
             mt_live_blocks, mt_live_bytes, mt_peak_bytes);
    platform_write_string(line);
    if (mt_overflows) {
        snprintf(line, sizeof(line),
                 "[mem]   WARNING: %lu allocation(s) untracked - table full "
                 "(raise MT_CAP); figures below are incomplete\n",
                 mt_overflows);
        platform_write_string(line);
    }

    /* Selection sort by live bytes: at most MT_SITES rows and it runs once,
     * so this needs no comparison-sort helper (and no allocation) here. */
    n = mt_site_count;
    for (i = 0; i < n; i++) {
        int j, best = i;
        for (j = i + 1; j < n; j++)
            if (mt_sites[j].live_bytes > mt_sites[best].live_bytes) best = j;
        if (best != i) {
            MtSite t = mt_sites[i];
            mt_sites[i] = mt_sites[best];
            mt_sites[best] = t;
        }
        if (mt_sites[i].live_bytes == 0) break;   /* the rest are clean */
        snprintf(line, sizeof(line),
                 "[mem]   %8lu bytes in %5lu block(s)  %s:%lu\n",
                 mt_sites[i].live_bytes, mt_sites[i].live_blocks,
                 mt_basename(mt_sites[i].file), mt_sites[i].line);
        platform_write_string(line);
    }
    platform_flush_output();
}

#else

/* Keep the translation unit non-empty when the tracer is compiled out. */
int cl_mem_track_disabled = 1;

#endif /* DEBUG_MEM_TRACK */
