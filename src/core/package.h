#ifndef CL_PACKAGE_H
#define CL_PACKAGE_H

#include "types.h"

/*
 * CL packages: CL, CL-USER, KEYWORD
 */

extern CL_Obj cl_package_cl;       /* COMMON-LISP package */
extern CL_Obj cl_package_cl_user;  /* COMMON-LISP-USER package */
extern CL_Obj cl_package_keyword;  /* KEYWORD package */
extern CL_Obj cl_package_ext;      /* EXT (implementation extensions) package */
extern CL_Obj cl_package_amilisp;  /* AMILISP (implementation internals) package */
extern CL_Obj cl_package_mop;      /* MOP (CLOS Metaobject Protocol) package */
extern CL_Obj cl_package_mp;      /* MP (multiprocessing) package */
extern CL_Obj cl_package_ffi;    /* FFI (foreign function interface) package */
extern CL_Obj cl_package_amiga;  /* AMIGA (AmigaOS-specific) package */
/* C-side mirror of *PACKAGE* — a PER-THREAD slot (CL_Thread.current_package),
 * accessed through a function-backed macro so every existing consumer
 * (reader, printer, INTERN, package-function defaults) resolves to the
 * CALLING thread's slot without package.h needing the CL_Thread layout.
 * It must be per-thread: with a shared global, any thread's *PACKAGE*
 * bind redirected every OTHER thread's reader — under Sly this interned
 * a compiling worker's symbols into SLYNK-IO-PACKAGE, baking poisoned
 * FASLs into the cache (see CL_Thread.current_package in thread.h). */
CL_Obj *cl_current_package_ref(void);
#define cl_current_package (*cl_current_package_ref())

/* Package registry — alist ((name-str . pkg) ...) */
extern CL_Obj cl_package_registry;

/* Thread-safety: rwlock protecting package registry and symbol tables */
extern void *cl_package_rwlock;

/* Initialize the three standard packages */
void cl_package_init(void);

/* Create a new package */
CL_Obj cl_make_package(const char *name);

/* Add a symbol to a package's symbol table (allocates — must NOT be
 * called while holding cl_package_rwlock) */
void cl_package_add_symbol(CL_Obj package, CL_Obj symbol);

/* Non-allocating: link SYMBOL into PACKAGE through the pre-allocated cons
 * CELL.  The only add-symbol form that may run while cl_package_rwlock is
 * held (allocating under the lock risks an STW-vs-rwlock deadlock). */
void cl_package_add_symbol_cell(CL_Obj package, CL_Obj symbol, CL_Obj cell);

/* Look up a symbol in a package (and its use-list) */
CL_Obj cl_package_find_symbol(const char *name, uint32_t len, CL_Obj package);

/* No-lock variant for use from within locked sections (e.g. cl_intern_in) */
CL_Obj cl_package_find_symbol_nolock(const char *name, uint32_t len, CL_Obj package);

/* Find only exported symbols in a package (no use-list search) */
CL_Obj cl_package_find_external(const char *name, uint32_t len, CL_Obj package);

/* Find symbol with status:
   0 = not found, 1 = :INTERNAL, 2 = :EXTERNAL, 3 = :INHERITED.
   Like every lookup above, consults the demand-interned binding tables
   (bindtab.c) and materialises a lazy hit before returning. */
CL_Obj cl_find_symbol_with_status(const char *name, uint32_t len,
                                   CL_Obj package, int *status);

/* Under cl_package_rwlock (any mode), for cl_intern_in's re-check and
 * bindtab.c: the complete non-allocating lookup.  Returns the symbol
 * (raw; CL_UNBOUND when absent) and *status as above; a lazy binding-table
 * hit returns CL_UNBOUND with *lazy_pkg set and the entry coordinates the
 * caller passes to cl_bindtab_materialize after unlocking. */
CL_Obj cl_package_lookup_nolock(const char *name, uint32_t len, CL_Obj package,
                                int *status, CL_Obj *lazy_pkg,
                                int *name_idx, int *def_idx, int *want_setter);

/* PACKAGE's own table only, raw symbol (SYM_NIL not normalized),
 * CL_UNBOUND when absent.  Lock must be held. */
CL_Obj cl_package_find_own_symbol_nolock(const char *name, uint32_t len,
                                         CL_Obj package);

/* Non-allocating export of a present, not-yet-exported SYMBOL through the
 * pre-consed CELL; write lock must be held (bindtab.c's materialiser). */
void cl_package_push_export_cell_nolock(CL_Obj package, CL_Obj symbol,
                                        CL_Obj cell);

/* cl_symbol_external_p without taking the lock (holder owns it). */
int cl_package_exported_p_nolock(CL_Obj sym, CL_Obj package);

/* Map the heap-allocated SYM_NIL (storage shadow for the NIL constant)
 * to the user-facing CL_NIL value.  Apply at any boundary that returns
 * a possibly-NIL symbol to user code so EQ-based identity holds across
 * (intern "NIL" :common-lisp), 'NIL, and the NIL constant. */
CL_Obj cl_normalize_nil_symbol(CL_Obj sym);

/* Find package by name or nickname */
CL_Obj cl_find_package(const char *name, uint32_t len);

/* Register package in global registry */
void cl_register_package(CL_Obj pkg);

/* Export/unexport symbols */
void cl_export_symbol(CL_Obj sym, CL_Obj package);
void cl_unexport_symbol(CL_Obj sym, CL_Obj package);

/* True iff PACKAGE's exported_symbols list contains SYM.  Per-package
 * source of truth; the global CL_SYM_EXPORTED flag is a hint at best. */
int cl_symbol_external_p(CL_Obj sym, CL_Obj package);

/* Import symbol into package */
void cl_import_symbol(CL_Obj sym, CL_Obj package);

/* Shadow: create internal symbol that shadows inherited one */
void cl_shadow_symbol(const char *name, uint32_t len, CL_Obj package);

/* Use/unuse package */
void cl_use_package(CL_Obj pkg_to_use, CL_Obj using_pkg);
void cl_unuse_package(CL_Obj pkg_to_unuse, CL_Obj using_pkg);

/* CDR-10: Package-local nicknames */
void cl_add_package_local_nickname(CL_Obj nick_str, CL_Obj target_pkg, CL_Obj in_pkg);
void cl_remove_package_local_nickname(const char *name, uint32_t len, CL_Obj from_pkg);

/* Export all symbols currently in CL package (called after symbol init) */
void cl_package_export_all_cl_symbols(void);

/* Export only newly-defined CL symbols that have real bindings
   (function, value, macro, type, struct, or CLOS class).
   Already-exported symbols are left unchanged. */
void cl_package_export_defined_cl_symbols(void);

/* Pre-intern the %-prefixed internal helper names that boot.lisp and
   clos.lisp define, in the AMILISP package.  Must be called before
   boot.lisp loads — see comment at the call site in repl.c. */
void cl_intern_clos_internals_in_amilisp(void);

/* Refresh the CALLING thread's cl_current_package slot from its dynamic
   *PACKAGE* binding (TLV if set, else the symbol's global value).  Must be
   called whenever the dynamic binding of *PACKAGE* changes at runtime —
   currently from OP_DYNBIND / OP_DYNUNBIND / OP_GSTORE on *PACKAGE* —
   so `let`-bindings propagate to this thread's consumers of
   cl_current_package (reader, printer, INTERN, etc.).  Strictly
   thread-local on both the read and the write side: it never touches
   another thread's slot. */
void cl_sync_current_package_from_dynamic(void);

#endif /* CL_PACKAGE_H */
