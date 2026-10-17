#ifndef CL_IMAGE_H
#define CL_IMAGE_H

/*
 * Heap images: EXT:SAVE-IMAGE / --image  (see specs/image-save-load.md)
 *
 * An image is a raw dump of the arena payload plus exactly the C-side state
 * the GC enumerates as roots (the "restore set == GC root set" invariant):
 * the registered global roots, the named shared tables marked in gc_mark,
 * the readtable pool, the main thread's persistent slots, and the side
 * buffers owned by CL_Bytecode / string-output-stream objects.
 *
 * Because heap references are arena-relative offsets, the arena payload
 * reloads at any base address and into any arena of size >= bump with no
 * relocation pass; EQ/EQL hashes hash the (preserved) offset, so no rehash
 * is needed either.  The only per-process fixups are C pointers: builtin
 * CL_CFunc pointers (relinked by name through the builtin registry),
 * CL_Bytecode side buffers (re-attached from the blob section), interned
 * source-file paths (one table entry per file), OS handles (locks/condvars recreated; file/socket
 * streams cannot exist at save), and foreign pointers (invalidated).
 *
 * Images are strictly per-build: a fingerprint over the version, the FASL
 * format (which keys the bytecode encoding), every heap struct's size and
 * the feature flags is verified before any byte of the file is interpreted.
 */

#include "types.h"

#define CL_IMAGE_MAGIC   0x434C494Du   /* "CLIM" */
#define CL_IMAGE_VERSION 5             /* bump on ANY format change.
                                        * v5: SOURCES section -- each
                                        * source-file name stored once, a
                                        * blob names it by a u16 index
                                        * instead of carrying a copy.
                                        * v4: CL_Lock / CL_CondVar became
                                        * heap words (state/depth/waiters
                                        * instead of side-table ids).
                                        * v3: CL_Package gained the
                                        * `bindings` slot (demand-interned
                                        * binding tables, bindtab.c).
                                        * v2: TYPE_FFI_STUB heap type (new
                                        * enum member, CL_TYPE_MAX moved). */

/* Header flag bits (informational; the fingerprint is authoritative) */
#define CL_IMAGE_FLAG_WIDE    0x0001
#define CL_IMAGE_FLAG_FPU     0x0002
#define CL_IMAGE_FLAG_LE      0x0004

#define CL_IMAGE_FPRINT_LEN   32

/* On-disk header, native byte order (images are per-build).  Serialized
 * field-by-field — never fwrite the struct — so compiler padding can't
 * leak into the format. */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint8_t  fingerprint[CL_IMAGE_FPRINT_LEN];
    uint32_t arena_size;   /* arena size at save (informational) */
    uint32_t bump;         /* bytes of arena payload in the ARENA section */
    uint32_t n_roots;      /* entries in the ROOTS section */
    uint32_t n_blobs;      /* CL_Bytecode entries in the BLOBS section */
    uint32_t boot_roots;   /* n_global_roots at end of C init when saving */
} CL_ImageHeader;

#define CL_IMAGE_HEADER_BYTES (4 + 2 + 2 + CL_IMAGE_FPRINT_LEN + 5 * 4)

/* --- Delivered executables: (EXT:SAVE-IMAGE path :EXECUTABLE T) ---
 *
 * A delivered executable is a copy of the running amilisp binary with the
 * image appended and, as the file's last bytes, this trailer (native byte
 * order -- the only reader is the binary the trailer is appended to):
 *
 *     u32 image offset   (from the start of the file)
 *     u32 image length   (up to 3 zero bytes follow: the trailer starts
 *                         on a longword)
 *     u32 heap size      (:HEAP-SIZE, bytes; 0 = the default rule)
 *     8 bytes            CL_IMAGE_EXE_MAGIC
 *
 * ELF, Mach-O and PE loaders ignore bytes behind the last section they
 * were told about; AmigaOS LoadSeg does not, so there the image and the
 * trailer are wrapped in a HUNK_DEBUG, which it skips (image.c).  amilisp
 * looks at its own file's tail before it parses its command line: with a
 * trailer there, the command line belongs to the program. */
#define CL_IMAGE_EXE_MAGIC        "CLAMIEXE"
#define CL_IMAGE_EXE_MAGIC_LEN    8
#define CL_IMAGE_EXE_TRAILER_BYTES (3 * 4 + CL_IMAGE_EXE_MAGIC_LEN)

/* --- Boot-time hookup (main.c) --- */

/* Snapshot n_global_roots at the end of the full C init (after
 * cl_debugger_init, before any boot/userinit Lisp runs).  This is the
 * root count an image records as boot_roots and the count a restoring
 * process must match at the same point. */
void cl_image_note_boot_roots(void);

/* --- Saving --- */

/* Arm a pending save (called by EXT:SAVE-IMAGE after validating
 * preconditions).  The dump itself runs at the next top-level safe point
 * where the main thread is at rest.  Signals a Lisp error on invalid
 * arguments or failed preconditions.
 *
 * SHAKE: shed every demand-interned binding table just before the dump
 * (bindtab.h, spec Phase 3) — the delivery mode.  It shrinks the image by
 * whatever the tables weigh (~150 KB for the four common raw modules) at
 * the price of closing those packages at the names already referenced, in
 * the image AND in this session. */
void cl_image_save_request(const char *path, int quit, int shake);

/* The same for a delivered executable: PATH becomes a copy of the running
 * binary with the image appended (see CL_IMAGE_EXE_MAGIC).  HEAP_SIZE is
 * the arena the executable starts with (0 = default).  Signals a Lisp
 * error when the running binary cannot find or read its own file. */
void cl_image_save_executable_request(const char *path, int quit, int shake,
                                      uint32_t heap_size);

/* Non-zero while a save is armed. */
int cl_image_save_pending_p(void);

/* Execute a pending save if the main thread is at rest (empty VM/dyn/NLX/
 * handler/restart/gc-root stacks).  Called from the top-level drivers
 * (REPL loop, main.c action loops).  Returns 1 when the save ran with
 * :quit t and the caller should proceed to process shutdown, 0 otherwise.
 * Save failures are reported to the user; the session continues. */
int cl_image_save_run_if_pending(void);

/* --- Restoring --- */

/* Read an image file's HEADER and verify everything that can be verified
 * before the heap exists (magic, version, fingerprint, byte order,
 * structural sanity).  The payload is read by cl_image_restore_staged,
 * once the arena has its contiguous block -- a whole image read first
 * could leave no block big enough for it.  Returns 0 on success; on
 * failure returns nonzero and (unless quiet) prints why.  Call after
 * platform_init and before cl_mem_init. */
int cl_image_stage(const char *path, int quiet);

/* Look for an image appended to the running executable and stage it.
 * Returns 1 when there is one and it is staged, 0 when the executable
 * carries none (the normal amilisp), -1 when it carries one that cannot be
 * used (reason printed) -- fatal for the caller: a delivered program must
 * not turn into a REPL.  Works before platform_init and before the command
 * line is parsed, which is the point (see CL_IMAGE_EXE_MAGIC). */
int cl_image_stage_embedded(void);

/* Non-zero when the staged/restored image came out of the executable. */
int cl_image_embedded_p(void);

/* The :HEAP-SIZE a delivered executable was saved with (0 = none). */
uint32_t cl_image_embedded_heap_size(void);

/* Call the :TOPLEVEL function a delivered executable was saved with, in
 * its own CL_CATCH.  Returns 0 when the image names none (nothing ran),
 * 1 when it ran -- the caller then shuts down; cl_exit_code is 1 after an
 * unhandled error, and whatever (QUIT n) set otherwise. */
int cl_image_run_toplevel(void);

/* Arena bytes the staged image needs (header bump).  0 if none staged. */
uint32_t cl_image_staged_bump(void);

/* Non-zero when an image has been staged for restore. */
int cl_image_staged_p(void);

/* Drop a staged image (e.g. auto-discovery rejected, falling back to a
 * normal boot). */
void cl_image_discard_staged(void);

/* Restore the staged image into the freshly-initialized runtime.  Must be
 * called at the exact point cl_repl_init would otherwise run (full C init
 * done, no Lisp executed).  Returns 0 on success.  Failures before the
 * arena is overwritten return nonzero (caller may fall back to a normal
 * boot); failures after are fatal-but-clean (message + exit). */
int cl_image_restore_staged(void);

/* T once a restore completed this process. */
int cl_image_restored_p(void);

/* The restored image's file name without its directory ("amilisp.img"),
 * "embedded image" for a delivered executable, "" before any restore.
 * For the REPL banner. */
const char *cl_image_restored_name(void);

/* --- Lisp surface (registered from cl_builtins_init) --- */
void cl_image_builtins_init(void);

/* Run EXT:*RESTORE-HOOKS* (most recent first, each in its own CL_CATCH)
 * and set EXT:*IMAGE-RESTORED-P* to T.  Called from the restored-boot
 * path in repl.c after the user init file ran. */
void cl_image_run_restore_hooks(void);

/* Run EXT::*SYSTEM-RESTORE-HOOKS* (internal; same calling rules): the
 * runtime's library modules re-derive the OS state they captured at load
 * time -- AMIGA.FFI:DEFINE-LIBRARY-VARIABLE's library bases, whose foreign
 * pointers the restore zeroed.  Called from the restored-boot path in
 * repl.c BEFORE the user init file, so ~/.amilisprc can call the OS. */
void cl_image_run_system_restore_hooks(void);

#endif /* CL_IMAGE_H */
