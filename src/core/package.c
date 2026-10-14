#include "package.h"
#include "symbol.h"
#include "mem.h"
#include "error.h"
#include "compiler.h"
#include "bindtab.h"
#include "../platform/platform.h"
#include "../platform/platform_thread.h"
#include "thread.h"
#include <string.h>

/* External tables for checking symbol bindings */
extern CL_Obj struct_table;           /* builtins_struct.c */
extern CL_Obj cl_clos_class_table;    /* builtins_struct.c */

CL_Obj cl_package_cl = CL_NIL;
CL_Obj cl_package_cl_user = CL_NIL;
CL_Obj cl_package_keyword = CL_NIL;
CL_Obj cl_package_ext = CL_NIL;
CL_Obj cl_package_amilisp = CL_NIL;
CL_Obj cl_package_mop = CL_NIL;
CL_Obj cl_package_mp = CL_NIL;
CL_Obj cl_package_ffi = CL_NIL;
CL_Obj cl_package_amiga = CL_NIL;
CL_Obj cl_package_registry = CL_NIL;

void *cl_package_rwlock = NULL;

/* Package-registry rwlock guards.  The lock is NULL until the threading
 * subsystem installs it (single-threaded startup runs lock-free), so every
 * acquire/release is guarded by a NULL check — these inlines fold the ~40
 * copies of that guard.  static inline → identical codegen, no call. */
static inline void pkg_lock_read(void)
{
    if (cl_package_rwlock) platform_rwlock_rdlock(cl_package_rwlock);
}
static inline void pkg_lock_write(void)
{
    if (cl_package_rwlock) platform_rwlock_wrlock(cl_package_rwlock);
}
static inline void pkg_unlock(void)
{
    if (cl_package_rwlock) platform_rwlock_unlock(cl_package_rwlock);
}

/* Refresh the CALLING thread's cl_current_package slot from its dynamic
 * *PACKAGE* binding.  Both the cl_symbol_value read and the
 * cl_current_package write resolve through the calling thread (TLV view /
 * per-thread slot), so a *PACKAGE* bind on one thread can never redirect
 * another thread's reader — the SLYNK-IO-PACKAGE FASL-poisoning class.
 * Safe to call before SYM_STAR_PACKAGE is interned (SYM_STAR_PACKAGE
 * starts at CL_NIL during early init) or before a package object is
 * installed — in both cases we leave cl_current_package unchanged. */
void cl_sync_current_package_from_dynamic(void)
{
    CL_Obj sp = SYM_STAR_PACKAGE;
    CL_Obj val;
    if (!CL_SYMBOL_P(sp))
        return;
    val = cl_symbol_value(sp);
    if (CL_NULL_P(val) || !CL_PACKAGE_P(val))
        return;
    cl_current_package = val;
}

/* ---- helpers ---- */

/* Compare CL_String object against C string */
static int str_eq(CL_Obj str_obj, const char *name, uint32_t len)
{
    CL_String *s = (CL_String *)CL_OBJ_TO_PTR(str_obj);
    return s->length == len && memcmp(s->data, name, len) == 0;
}

/* See package.h for rationale. */
CL_Obj cl_normalize_nil_symbol(CL_Obj sym)
{
    if (sym == SYM_NIL && SYM_NIL != CL_NIL) return CL_NIL;
    return sym;
}

#define normalize_nil(sym) cl_normalize_nil_symbol(sym)

/* Search a single package's own symbol table (no use-list).
 * Returns CL_UNBOUND when not found — distinct from CL_NIL because the
 * symbol NIL itself is interned in COMMON-LISP, so a CL_NIL return would
 * be ambiguous between "found NIL" and "missing".
 *
 * Internal callers receive the raw heap symbol (so identity comparisons
 * against the exported_symbols / shadowing_symbols lists keep working);
 * the public boundary normalizes SYM_NIL to CL_NIL. */
static CL_Obj find_own_symbol(const char *name, uint32_t len, CL_Obj package)
{
    CL_Package *pkg = (CL_Package *)CL_OBJ_TO_PTR(package);
    CL_Vector *tbl = (CL_Vector *)CL_OBJ_TO_PTR(pkg->symbols);
    uint32_t hash = cl_hash_string(name, len);
    uint32_t idx = hash % tbl->length;
    CL_Obj list = tbl->data[idx];

    while (!CL_NULL_P(list)) {
        CL_Obj sym = cl_car(list);
        CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(sym);
        CL_String *sname = (CL_String *)CL_OBJ_TO_PTR(s->name);
        if (sname->length == len && memcmp(sname->data, name, len) == 0) {
            return sym;
        }
        list = cl_cdr(list);
    }
    return CL_UNBOUND;
}

/* ---- nolock helpers for internal use under existing lock ---- */

/* Per-package exported-symbols list membership check.  Each package
 * tracks its own export set so a symbol can be EXTERNAL in its home
 * and INTERNAL in a using/importing package (CLHS 11.1).  Holders must
 * already own cl_package_rwlock for read or write. */
static int exported_p_nolock(CL_Obj sym, CL_Obj package)
{
    CL_Package *pkg = (CL_Package *)CL_OBJ_TO_PTR(package);
    CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(sym);
    CL_Obj list;
    /* O(1) answers for the two dominant cases — see CL_SYM_EXPORTED_HOME
     * in types.h.  (1) PACKAGE is the symbol's home: the exact per-home
     * flag IS the list membership.  (2) No package at all exports the
     * symbol (the hint flag is clear): the list cannot contain it.  Only
     * "exported from a non-home package" (import + export, or exporting
     * an inherited symbol) still walks the list. */
    if (s->package == package)
        return (s->flags & CL_SYM_EXPORTED_HOME) != 0;
    if (!(s->flags & CL_SYM_EXPORTED))
        return 0;
    list = pkg->exported_symbols;
    while (!CL_NULL_P(list)) {
        if (cl_car(list) == sym) return 1;
        list = cl_cdr(list);
    }
    return 0;
}

/* Returns CL_UNBOUND when the name is not exported from package
 * (or not present at all).  See find_own_symbol comment about NIL.
 * Internal use only — does not normalize SYM_NIL. */
static CL_Obj find_external_nolock(const char *name, uint32_t len, CL_Obj package)
{
    CL_Obj sym = find_own_symbol(name, len, package);
    if (sym != CL_UNBOUND && exported_p_nolock(sym, package))
        return sym;
    return CL_UNBOUND;
}

int cl_package_exported_p_nolock(CL_Obj sym, CL_Obj package)
{
    return exported_p_nolock(sym, package);
}

int cl_symbol_external_p(CL_Obj sym, CL_Obj package)
{
    int r;
    if (CL_NULL_P(sym) || CL_NULL_P(package)) return 0;
    pkg_lock_read();
    r = exported_p_nolock(sym, package);
    pkg_unlock();
    return r;
}

/* Returns CL_UNBOUND when name is not present in package or its use-list. */
CL_Obj cl_package_find_symbol_nolock(const char *name, uint32_t len, CL_Obj package)
{
    CL_Obj sym;
    sym = find_own_symbol(name, len, package);
    if (sym != CL_UNBOUND) return sym;
    {
        CL_Package *pkg = (CL_Package *)CL_OBJ_TO_PTR(package);
        CL_Obj uses = pkg->use_list;
        while (!CL_NULL_P(uses)) {
            CL_Obj found = find_external_nolock(name, len, cl_car(uses));
            if (found != CL_UNBOUND) return found;
            uses = cl_cdr(uses);
        }
    }
    return CL_UNBOUND;
}

/* Link SYMBOL into PACKAGE's own bucket chain through the pre-allocated
 * cons CELL — plain stores only, NO allocation.  This is the only way a
 * symbol may be added while holding cl_package_rwlock: allocating under
 * the write lock can trigger a stop-the-world GC that waits for every
 * peer thread to park, while a peer blocked on the rdlock (any intern
 * fast path) can never park — a circular STW-vs-rwlock hang.  Callers
 * cons CELL outside the lock; the raw pointers derived here are stable
 * because no allocation (hence no compaction) happens under the lock. */
static void package_link_symbol_cell(CL_Obj package, CL_Obj symbol,
                                     CL_Obj cell)
{
    CL_Package *pkg = (CL_Package *)CL_OBJ_TO_PTR(package);
    CL_Vector *tbl = (CL_Vector *)CL_OBJ_TO_PTR(pkg->symbols);
    CL_Symbol *sym = (CL_Symbol *)CL_OBJ_TO_PTR(symbol);
    CL_Cons *c = (CL_Cons *)CL_OBJ_TO_PTR(cell);
    uint32_t idx = sym->hash % tbl->length;
    c->car = symbol;
    c->cdr = tbl->data[idx];
    tbl->data[idx] = cell;
    pkg->sym_count++;
}

/* Import SYM into PACKAGE's own table via the pre-allocated CELL (consed
 * by the caller OUTSIDE the package lock — see package_link_symbol_cell).
 * Returns 0 on success or no-op, -1 on a name conflict.  Deliberately
 * does NOT cl_error here: the callers hold cl_package_rwlock, and a
 * longjmp from under the write lock would leak it and deadlock every
 * later intern. */
static int import_symbol_nolock(CL_Obj sym, CL_Obj package, CL_Obj cell)
{
    CL_Symbol *s;
    CL_String *sname;
    CL_Obj existing;

    if (CL_NULL_P(sym)) return 0;
    s = (CL_Symbol *)CL_OBJ_TO_PTR(sym);
    sname = (CL_String *)CL_OBJ_TO_PTR(s->name);

    existing = find_own_symbol(sname->data, sname->length, package);
    if (existing != CL_UNBOUND) {
        if (existing == sym) return 0;
        return -1;   /* conflict — caller reports after unlocking */
    }

    {
        CL_Package *pkg = (CL_Package *)CL_OBJ_TO_PTR(package);
        CL_Vector *tbl = (CL_Vector *)CL_OBJ_TO_PTR(pkg->symbols);
        CL_Cons *c = (CL_Cons *)CL_OBJ_TO_PTR(cell);
        uint32_t idx = s->hash % tbl->length;
        c->car = sym;
        c->cdr = tbl->data[idx];
        tbl->data[idx] = cell;
        pkg->sym_count++;
    }
    return 0;
}

/* ---- public API ---- */

CL_Obj cl_make_package(const char *name)
{
    CL_Obj name_str, symtab;
    CL_Package *pkg;

    name_str = cl_make_string(name, (uint32_t)strlen(name));
    CL_GC_PROTECT(name_str);

    symtab = cl_make_vector(CL_SYMTAB_SIZE);
    CL_GC_PROTECT(symtab);

    pkg = (CL_Package *)cl_alloc(TYPE_PACKAGE,
                                  sizeof(CL_Package));
    CL_GC_UNPROTECT(2);

    if (!pkg) return CL_NIL;
    pkg->name = name_str;
    pkg->symbols = symtab;
    pkg->use_list = CL_NIL;
    pkg->nicknames = CL_NIL;
    pkg->local_nicknames = CL_NIL;
    pkg->shadowing_symbols = CL_NIL;
    pkg->exported_symbols = CL_NIL;
    pkg->bindings = CL_NIL;
    pkg->sym_count = 0;
    return CL_PTR_TO_OBJ(pkg);
}

/* Non-allocating variant: link SYMBOL as a present symbol of PACKAGE
 * (setting its home package) through the pre-allocated CELL.  Safe to
 * call while holding cl_package_rwlock — see package_link_symbol_cell. */
void cl_package_add_symbol_cell(CL_Obj package, CL_Obj symbol, CL_Obj cell)
{
    CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(symbol);
    package_link_symbol_cell(package, symbol, cell);
    s->package = package;
    /* A new home: the symbol is not on THIS package's export list yet
     * (the per-home flag must track the list exactly, see types.h). */
    s->flags &= ~CL_SYM_EXPORTED_HOME;
}

/* Allocating convenience wrapper — must NOT be called while holding
 * cl_package_rwlock (the cons can trigger a stop-the-world GC; see
 * package_link_symbol_cell).  Lock-free callers (boot, single-threaded
 * setup) only. */
void cl_package_add_symbol(CL_Obj package, CL_Obj symbol)
{
    CL_Obj cell;
    CL_GC_PROTECT(package);
    CL_GC_PROTECT(symbol);
    cell = cl_cons(symbol, CL_NIL);
    CL_GC_UNPROTECT(2);
    cl_package_add_symbol_cell(package, symbol, cell);
}

/* Raw-symbol view of find_own_symbol for bindtab.c's locked re-check
 * (CL_UNBOUND when absent; SYM_NIL not normalized).  Holders must own
 * cl_package_rwlock. */
CL_Obj cl_package_find_own_symbol_nolock(const char *name, uint32_t len,
                                         CL_Obj package)
{
    return find_own_symbol(name, len, package);
}

/* Push SYM onto PACKAGE's export list through the pre-consed CELL and set
 * the export flags — plain stores only, for use under the write lock by a
 * caller that has already verified SYM is present in PACKAGE and not yet
 * exported (bindtab.c's materialiser).  Mirrors the tail of
 * cl_export_symbol. */
void cl_package_push_export_cell_nolock(CL_Obj package, CL_Obj symbol,
                                        CL_Obj cell)
{
    CL_Package *pkg = (CL_Package *)CL_OBJ_TO_PTR(package);
    CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(symbol);
    ((CL_Cons *)CL_OBJ_TO_PTR(cell))->cdr = pkg->exported_symbols;
    pkg->exported_symbols = cell;
    s->flags |= CL_SYM_EXPORTED;
    if (s->package == package)
        s->flags |= CL_SYM_EXPORTED_HOME;
}

/* The one lookup behind FIND-SYMBOL, INTERN, the reader and use-list
 * inheritance, extended with the demand-interned binding tables
 * (bindtab.c): a name that is not PRESENT in a package but is in the
 * package's table is reported as a lazy hit, and the caller materialises
 * it after dropping the lock.
 *
 * Order (CLHS 11.1.1.2 — present symbols shadow inherited ones):
 *   1. PACKAGE's own table; then PACKAGE's binding table (a table entry
 *      IS a present symbol of the package that has not been built yet,
 *      so it wins over anything inherited);
 *   2. each used package in use-list order: its present EXTERNAL
 *      symbols, then its binding table — the two are one external set.
 *
 * Returns the symbol (raw, CL_UNBOUND when absent) and *status
 * (0 absent, 1 :INTERNAL, 2 :EXTERNAL, 3 :INHERITED).  On a lazy hit the
 * return is CL_UNBOUND, *status is the status the materialised symbol
 * will have, and *lazy_pkg / *name_idx / *def_idx / *want_setter are
 * what cl_bindtab_materialize needs.  Non-allocating; holders must own
 * cl_package_rwlock (read is enough). */
static CL_Obj lookup_nolock(const char *name, uint32_t len, CL_Obj package,
                            int *status, CL_Obj *lazy_pkg,
                            int *name_idx, int *def_idx, int *want_setter)
{
    CL_Package *pkg = (CL_Package *)CL_OBJ_TO_PTR(package);
    CL_Obj sym, uses;

    *lazy_pkg = CL_NIL;
    sym = find_own_symbol(name, len, package);
    if (sym != CL_UNBOUND) {
        *status = exported_p_nolock(sym, package) ? 2 : 1;
        return sym;
    }
    if (!CL_NULL_P(pkg->bindings) &&
        cl_bindtab_probe_nolock(package, name, len, 1,
                                name_idx, def_idx, want_setter)) {
        *lazy_pkg = package;
        *status = *want_setter ? 1 : 2;
        return CL_UNBOUND;
    }
    uses = pkg->use_list;
    while (!CL_NULL_P(uses)) {
        CL_Obj used = cl_car(uses);
        CL_Obj found = find_external_nolock(name, len, used);
        if (found != CL_UNBOUND) {
            *status = 3;
            return found;
        }
        if (!CL_NULL_P(((CL_Package *)CL_OBJ_TO_PTR(used))->bindings) &&
            cl_bindtab_probe_nolock(used, name, len, 0,
                                    name_idx, def_idx, want_setter)) {
            *lazy_pkg = used;
            *status = 3;
            return CL_UNBOUND;
        }
        uses = cl_cdr(uses);
    }
    *status = 0;
    return CL_UNBOUND;
}

/* Locked lookup + (outside the lock) materialisation of a lazy hit.
 * EXTERNAL_ONLY restricts the search to PACKAGE's own externals (no
 * use-list, no internal setters) — cl_package_find_external's contract. */
static CL_Obj lookup_and_materialize(const char *name, uint32_t len,
                                     CL_Obj package, int external_only,
                                     int *status)
{
    CL_Obj sym, lazy_pkg = CL_NIL;
    int name_idx = 0, def_idx = -1, want_setter = 0;

    pkg_lock_read();
    if (external_only) {
        sym = find_external_nolock(name, len, package);
        if (sym != CL_UNBOUND) {
            *status = 2;
        } else if (!CL_NULL_P(((CL_Package *)CL_OBJ_TO_PTR(package))->bindings) &&
                   cl_bindtab_probe_nolock(package, name, len, 0,
                                           &name_idx, &def_idx, &want_setter)) {
            lazy_pkg = package;
            *status = 2;
        } else {
            *status = 0;
        }
    } else {
        sym = lookup_nolock(name, len, package, status, &lazy_pkg,
                            &name_idx, &def_idx, &want_setter);
    }
    pkg_unlock();

    if (sym != CL_UNBOUND) return sym;
    if (!CL_NULL_P(lazy_pkg))
        return cl_bindtab_materialize(lazy_pkg, name, len, name_idx, def_idx,
                                      want_setter, 0);
    return CL_UNBOUND;
}

/* Public API: returns CL_NIL when not found (preserved historical contract
 * for C callers that use CL_NULL_P-as-not-found).  Callers that need to
 * distinguish "found NIL" from "missing" should use
 * cl_find_symbol_with_status. */
CL_Obj cl_package_find_external(const char *name, uint32_t len, CL_Obj package)
{
    int status;
    CL_Obj result = lookup_and_materialize(name, len, package, 1, &status);
    if (result == CL_UNBOUND) return CL_NIL;
    return normalize_nil(result);
}

CL_Obj cl_package_find_symbol(const char *name, uint32_t len, CL_Obj package)
{
    int status;
    CL_Obj result = lookup_and_materialize(name, len, package, 0, &status);
    if (result == CL_UNBOUND) return CL_NIL;
    return normalize_nil(result);
}

CL_Obj cl_find_symbol_with_status(const char *name, uint32_t len,
                                   CL_Obj package, int *status)
{
    CL_Obj sym = lookup_and_materialize(name, len, package, 0, status);
    if (sym == CL_UNBOUND) {
        *status = 0;
        return CL_NIL;
    }
    return normalize_nil(sym);
}

/* cl_intern_in's write-locked re-check: what is there NOW, plus a lazy
 * probe — a binding table registered between the fast path and the
 * write lock must still win over a fresh unbound symbol (otherwise that
 * symbol would shadow its own definition forever).  Returns the symbol,
 * or CL_UNBOUND; *lazy_pkg etc. as lookup_nolock. */
CL_Obj cl_package_lookup_nolock(const char *name, uint32_t len, CL_Obj package,
                                int *status, CL_Obj *lazy_pkg,
                                int *name_idx, int *def_idx, int *want_setter)
{
    return lookup_nolock(name, len, package, status, lazy_pkg,
                         name_idx, def_idx, want_setter);
}

CL_Obj cl_find_package(const char *name, uint32_t len)
{
    CL_Obj reg;
    CL_Obj result = CL_NIL;

    pkg_lock_read();

    /* CDR-10: Check local nicknames of current package first */
    if (!CL_NULL_P(cl_current_package)) {
        CL_Package *cur = (CL_Package *)CL_OBJ_TO_PTR(cl_current_package);
        CL_Obj lnicks = cur->local_nicknames;
        while (!CL_NULL_P(lnicks)) {
            CL_Obj entry = cl_car(lnicks);
            if (str_eq(cl_car(entry), name, len)) {
                result = cl_cdr(entry);
                pkg_unlock();
                return result;
            }
            lnicks = cl_cdr(lnicks);
        }
    }

    reg = cl_package_registry;

    while (!CL_NULL_P(reg)) {
        CL_Obj entry = cl_car(reg);
        CL_Obj pkg = cl_cdr(entry);
        CL_Package *p = (CL_Package *)CL_OBJ_TO_PTR(pkg);

        /* Check primary name */
        if (str_eq(p->name, name, len)) {
            pkg_unlock();
            return pkg;
        }

        /* Check nicknames */
        {
            CL_Obj nicks = p->nicknames;
            while (!CL_NULL_P(nicks)) {
                if (str_eq(cl_car(nicks), name, len)) {
                    pkg_unlock();
                    return pkg;
                }
                nicks = cl_cdr(nicks);
            }
        }

        reg = cl_cdr(reg);
    }
    pkg_unlock();
    return CL_NIL;
}

void cl_register_package(CL_Obj pkg)
{
    CL_Package *p = (CL_Package *)CL_OBJ_TO_PTR(pkg);
    CL_Obj entry, cell;

    /* Both conses run OUTSIDE the write lock; the registry is linked with
     * plain stores inside it (allocating under cl_package_rwlock risks an
     * STW-vs-rwlock deadlock — see package_link_symbol_cell). */
    CL_GC_PROTECT(pkg);
    entry = cl_cons(p->name, pkg);
    cell = cl_cons(entry, CL_NIL);
    CL_GC_UNPROTECT(1);
    pkg_lock_write();
    ((CL_Cons *)CL_OBJ_TO_PTR(cell))->cdr = cl_package_registry;
    cl_package_registry = cell;
    pkg_unlock();
}

void cl_export_symbol(CL_Obj sym, CL_Obj package)
{
    CL_Symbol *s;
    CL_String *sname;
    CL_Obj cell_import, cell_export;
    int conflict;
    if (CL_NULL_P(sym)) return;
    /* A lazy package (binding table attached) about to IMPORT a symbol
     * that is not present: the import could silently shadow a table name
     * of the same spelling, so flip the package eager first (materialise
     * every table entry, drop the table) and let the ordinary conflict
     * check see the real symbol set.  Rare — exporting a foreign symbol
     * from a generated binding package.
     * GC SAFETY: sym/package must be rooted before cl_bindtab_materialize_all
     * — materialising a whole table allocates heavily and can compact. */
    CL_GC_PROTECT(sym);
    CL_GC_PROTECT(package);
    if (!CL_NULL_P(((CL_Package *)CL_OBJ_TO_PTR(package))->bindings)) {
        CL_Symbol *s0 = (CL_Symbol *)CL_OBJ_TO_PTR(sym);
        CL_String *n0 = (CL_String *)CL_OBJ_TO_PTR(s0->name);
        CL_Obj present;
        pkg_lock_read();
        present = find_own_symbol(n0->data, n0->length, package);
        pkg_unlock();
        if (present == CL_UNBOUND)
            cl_bindtab_materialize_all(package);
    }
    /* Pre-cons both cells the locked section might need OUTSIDE the lock
     * (allocating under cl_package_rwlock risks an STW-vs-rwlock deadlock
     * — see package_link_symbol_cell).  An unused cell simply becomes
     * garbage.  All raw pointers are derived after the conses; they stay
     * valid through the lock because no allocation happens under it. */
    cell_import = cl_cons(sym, CL_NIL);
    CL_GC_PROTECT(cell_import);
    cell_export = cl_cons(sym, CL_NIL);
    CL_GC_UNPROTECT(3);
    s = (CL_Symbol *)CL_OBJ_TO_PTR(sym);
    sname = (CL_String *)CL_OBJ_TO_PTR(s->name);

    pkg_lock_write();

    /* If symbol is not present in package's own table, import it first.
       Per CL spec: "If the symbol is accessible via use-package,
       it is first imported into package, after which it is exported." */
    conflict = 0;
    if (find_own_symbol(sname->data, sname->length, package) == CL_UNBOUND) {
        conflict = import_symbol_nolock(sym, package, cell_import);
    }

    /* Add to package's exported list (idempotent). */
    if (!conflict && !exported_p_nolock(sym, package)) {
        CL_Package *pkg = (CL_Package *)CL_OBJ_TO_PTR(package);
        ((CL_Cons *)CL_OBJ_TO_PTR(cell_export))->cdr = pkg->exported_symbols;
        pkg->exported_symbols = cell_export;
    }
    /* Keep the legacy global flag in sync (used by printer / describe
     * fast paths and by FASL loader as a "any package exports this"
     * heuristic).  Source of truth for find-symbol is the per-package
     * list above — mirrored EXACTLY into CL_SYM_EXPORTED_HOME when
     * PACKAGE is the symbol's home, so exported_p_nolock can answer the
     * home case without walking the list. */
    if (!conflict) {
        s->flags |= CL_SYM_EXPORTED;
        if (s->package == package)
            s->flags |= CL_SYM_EXPORTED_HOME;
    }
    pkg_unlock();
    if (conflict)
        cl_error(CL_ERR_GENERAL,
                 "EXPORT conflict: symbol already present in package");
}

/* Unexport SYM from PACKAGE.  Removes SYM from PACKAGE's exported list
 * (the per-package source of truth).  The legacy CL_SYM_EXPORTED flag is
 * cleared only when the symbol's home package is PACKAGE — keeping the
 * "any package considers this symbol exported" hint useful for printer
 * and describe fast paths. */
/* Splice the first cons whose car == ITEM out of the singly-linked list rooted
 * at *head (a package struct field), relinking cdrs in place — no allocation,
 * so the raw pointers derived under the package write lock stay valid.
 * Returns 1 if an element was removed.  Shared by UNEXPORT and UNUSE-PACKAGE
 * (the by-string local-nickname remover keeps its own loop — different key). */
static int pkg_list_remove_eq(CL_Obj *head, CL_Obj item)
{
    CL_Obj prev = CL_NIL, list = *head;
    while (!CL_NULL_P(list)) {
        if (cl_car(list) == item) {
            CL_Obj next = cl_cdr(list);
            if (CL_NULL_P(prev)) *head = next;
            else ((CL_Cons *)CL_OBJ_TO_PTR(prev))->cdr = next;
            return 1;
        }
        prev = list;
        list = cl_cdr(list);
    }
    return 0;
}

void cl_unexport_symbol(CL_Obj sym, CL_Obj package)
{
    CL_Symbol *s;
    CL_Package *pkg;
    if (CL_NULL_P(sym)) return;
    s = (CL_Symbol *)CL_OBJ_TO_PTR(sym);
    pkg_lock_write();
    pkg = (CL_Package *)CL_OBJ_TO_PTR(package);
    pkg_list_remove_eq(&pkg->exported_symbols, sym);
    if (s->package == package)
        s->flags &= ~(CL_SYM_EXPORTED | CL_SYM_EXPORTED_HOME);
    pkg_unlock();
}

void cl_import_symbol(CL_Obj sym, CL_Obj package)
{
    CL_Obj cell;
    int conflict;
    if (CL_NULL_P(sym)) return;
    /* Importing into a lazy package: the imported symbol could shadow a
     * not-yet-built table name — flip eager so the conflict check below
     * sees the complete symbol set (see cl_export_symbol).
     * GC SAFETY: sym/package must be rooted before cl_bindtab_materialize_all
     * — materialising a whole table allocates heavily and can compact. */
    CL_GC_PROTECT(sym);
    CL_GC_PROTECT(package);
    if (!CL_NULL_P(((CL_Package *)CL_OBJ_TO_PTR(package))->bindings))
        cl_bindtab_materialize_all(package);
    /* Pre-cons the bucket cell outside the lock; report a conflict only
     * AFTER unlocking (a longjmp from under the write lock would leak it
     * and deadlock every later intern). */
    cell = cl_cons(sym, CL_NIL);
    CL_GC_UNPROTECT(2);
    pkg_lock_write();
    conflict = import_symbol_nolock(sym, package, cell);
    pkg_unlock();
    if (conflict)
        cl_error(CL_ERR_GENERAL,
                 "IMPORT conflict: symbol already present in package");
}

void cl_shadow_symbol(const char *name, uint32_t len, CL_Obj package)
{
    CL_Obj existing;
    char namebuf[256];
    char *heapname = NULL;
    uint32_t h;

    /* NAME may point INTO the moving heap (a Lisp string's data); the
     * allocations below can relocate it, and the re-check under the lock
     * reads it after those allocations.  Copy to GC-immune C memory
     * up-front (mirrors cl_intern_in). */
    if (len < sizeof(namebuf)) {
        memcpy(namebuf, name, len);
        name = namebuf;
    } else {
        heapname = (char *)platform_alloc(len);
        if (heapname) {
            memcpy(heapname, name, len);
            name = heapname;
        }
    }
    h = cl_hash_string(name, len);

    /* Fast path: already directly present — or present in the package's
     * binding table, which CLHS-wise is the same thing ("a symbol with
     * that name is already present: no new symbol is created"): build it. */
    {
        int name_idx = 0, def_idx = -1, want_setter = 0, lazy = 0;
        pkg_lock_read();
        existing = find_own_symbol(name, len, package);
        if (existing == CL_UNBOUND &&
            !CL_NULL_P(((CL_Package *)CL_OBJ_TO_PTR(package))->bindings))
            lazy = cl_bindtab_probe_nolock(package, name, len, 1,
                                           &name_idx, &def_idx, &want_setter);
        pkg_unlock();
        if (existing != CL_UNBOUND) {
            if (heapname) platform_free(heapname);
            return;
        }
        if (lazy) {
            (void)cl_bindtab_materialize(package, name, len, name_idx, def_idx,
                                         want_setter, 0);
            if (heapname) platform_free(heapname);
            return;
        }
    }

    /* Create the symbol AND its bucket cell OUTSIDE the lock (allocating
     * under cl_package_rwlock risks an STW-vs-rwlock deadlock — see
     * package_link_symbol_cell), then re-check + link with plain stores
     * inside it (mirrors the cl_intern_in slow path). */
    {
        CL_Obj name_str;
        CL_Obj sym;
        CL_Obj cell;
        CL_Symbol *s;

        CL_GC_PROTECT(package);
        name_str = cl_make_string(name, len);
        CL_GC_PROTECT(name_str);
        sym = cl_make_symbol(name_str);
        CL_GC_PROTECT(sym);
        cell = cl_cons(sym, CL_NIL);
        CL_GC_UNPROTECT(3);

        s = (CL_Symbol *)CL_OBJ_TO_PTR(sym);
        s->hash = h;

        pkg_lock_write();
        existing = find_own_symbol(name, len, package);
        if (existing == CL_UNBOUND)
            cl_package_add_symbol_cell(package, sym, cell);
        /* else: raced with a concurrent intern/shadow — theirs wins,
         * our fresh symbol and cell become garbage. */
        pkg_unlock();
    }
    if (heapname) platform_free(heapname);
}

void cl_use_package(CL_Obj pkg_to_use, CL_Obj using_pkg)
{
    CL_Package *user;
    CL_Obj list;

    if (pkg_to_use == using_pkg) return;

    /* Pre-cons the use-list cell OUTSIDE the lock (allocating under
     * cl_package_rwlock risks an STW-vs-rwlock deadlock — see
     * package_link_symbol_cell); the dup-check and the link both run
     * inside it with plain stores.  On the already-using path the fresh
     * cell simply becomes garbage. */
    {
        CL_Obj cell;
        CL_GC_PROTECT(using_pkg);
        CL_GC_PROTECT(pkg_to_use);
        cell = cl_cons(pkg_to_use, CL_NIL);
        CL_GC_UNPROTECT(2);

        pkg_lock_write();
        user = (CL_Package *)CL_OBJ_TO_PTR(using_pkg);

        /* Check if already in use-list */
        list = user->use_list;
        while (!CL_NULL_P(list)) {
            if (cl_car(list) == pkg_to_use) {
                pkg_unlock();
                return; /* already using */
            }
            list = cl_cdr(list);
        }

        ((CL_Cons *)CL_OBJ_TO_PTR(cell))->cdr = user->use_list;
        user->use_list = cell;
        pkg_unlock();
    }
}

void cl_unuse_package(CL_Obj pkg_to_unuse, CL_Obj using_pkg)
{
    CL_Package *user;
    pkg_lock_write();
    user = (CL_Package *)CL_OBJ_TO_PTR(using_pkg);
    pkg_list_remove_eq(&user->use_list, pkg_to_unuse);
    pkg_unlock();
}

void cl_add_package_local_nickname(CL_Obj nick_str, CL_Obj target_pkg, CL_Obj in_pkg)
{
    CL_Package *p;
    CL_String *ns;
    CL_Obj list;
    CL_Obj entry, cell;

    /* Pre-cons both cells OUTSIDE the lock (allocating under
     * cl_package_rwlock risks an STW-vs-rwlock deadlock — see
     * package_link_symbol_cell); dup-check and link run inside it with
     * plain stores.  On the replace path the fresh cells become garbage. */
    CL_GC_PROTECT(in_pkg);
    CL_GC_PROTECT(nick_str);
    CL_GC_PROTECT(target_pkg);
    entry = cl_cons(nick_str, target_pkg);
    CL_GC_PROTECT(entry);
    cell = cl_cons(entry, CL_NIL);
    CL_GC_UNPROTECT(4);

    pkg_lock_write();
    p = (CL_Package *)CL_OBJ_TO_PTR(in_pkg);
    ns = (CL_String *)CL_OBJ_TO_PTR(nick_str);
    list = p->local_nicknames;

    /* Check for duplicate — replace if exists */
    while (!CL_NULL_P(list)) {
        CL_Obj e = cl_car(list);
        if (str_eq(cl_car(e), ns->data, ns->length)) {
            /* Replace target */
            CL_Cons *c = (CL_Cons *)CL_OBJ_TO_PTR(e);
            c->cdr = target_pkg;
            pkg_unlock();
            return;
        }
        list = cl_cdr(list);
    }

    /* Prepend the pre-built (nick . target) entry. */
    ((CL_Cons *)CL_OBJ_TO_PTR(cell))->cdr = p->local_nicknames;
    p->local_nicknames = cell;
    pkg_unlock();
}

void cl_remove_package_local_nickname(const char *name, uint32_t len, CL_Obj from_pkg)
{
    CL_Package *p;
    CL_Obj prev, list;

    pkg_lock_write();
    p = (CL_Package *)CL_OBJ_TO_PTR(from_pkg);
    prev = CL_NIL;
    list = p->local_nicknames;

    while (!CL_NULL_P(list)) {
        CL_Obj entry = cl_car(list);
        if (str_eq(cl_car(entry), name, len)) {
            if (CL_NULL_P(prev)) {
                p->local_nicknames = cl_cdr(list);
            } else {
                CL_Cons *c = (CL_Cons *)CL_OBJ_TO_PTR(prev);
                c->cdr = cl_cdr(list);
            }
            pkg_unlock();
            return;
        }
        prev = list;
        list = cl_cdr(list);
    }
    pkg_unlock();
}

/* Export every symbol presently in PACKAGE's own symbol table.
 * Sets the legacy CL_SYM_EXPORTED flag and pushes each symbol onto
 * pkg->exported_symbols.
 *
 * GC compaction during cl_cons can relocate the package, the symbol
 * table vector, and the chain conses, so we re-read the chain pointer
 * for each iteration and use a counted advance to make progress. */
/* Mark every symbol in `list` with CL_SYM_LISTED (used as a transient
 * O(1) "is this symbol already in pkg->exported_symbols" check during
 * bulk-export passes — replaces an O(n) linear scan that made the
 * surrounding loop O(n²)). */
static void mark_exported_listed(CL_Obj list)
{
    while (!CL_NULL_P(list)) {
        CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(cl_car(list));
        s->flags |= CL_SYM_LISTED;
        list = cl_cdr(list);
    }
}

static void clear_exported_listed(CL_Obj list)
{
    while (!CL_NULL_P(list)) {
        CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(cl_car(list));
        s->flags &= ~CL_SYM_LISTED;
        list = cl_cdr(list);
    }
}

/* Export every symbol in PACKAGE's own table for which PRED returns true
 * (PRED == NULL exports all).  Sets CL_SYM_EXPORTED and pushes each onto
 * pkg->exported_symbols.  GC compaction during cl_cons can relocate the
 * package, its symbol-table vector, and the chain conses, so the chain
 * pointer is re-read every iteration and a counted advance makes progress.
 * PRED must not allocate (it runs between the raw `s` deref and its use —
 * the same constraint the inlined symbol_has_binding check already relied on).
 * Shared by cl_package_export_all_cl_symbols and the CL-defined discovery. */
static void export_symbols_where(CL_Obj package, int (*pred)(CL_Obj sym))
{
    uint32_t i, table_len;
    CL_GC_PROTECT(package);
    {
        CL_Package *pkg = (CL_Package *)CL_OBJ_TO_PTR(package);
        CL_Vector *tbl = (CL_Vector *)CL_OBJ_TO_PTR(pkg->symbols);
        table_len = tbl->length;
        mark_exported_listed(pkg->exported_symbols);
    }
    for (i = 0; i < table_len; i++) {
        uint32_t consumed = 0;
        for (;;) {
            CL_Package *pkg = (CL_Package *)CL_OBJ_TO_PTR(package);
            CL_Vector *tbl = (CL_Vector *)CL_OBJ_TO_PTR(pkg->symbols);
            CL_Obj list = tbl->data[i];
            uint32_t skip = consumed;
            CL_Obj sym;
            CL_Symbol *s;
            while (skip-- > 0 && !CL_NULL_P(list)) list = cl_cdr(list);
            if (CL_NULL_P(list)) break;
            sym = cl_car(list);
            s = (CL_Symbol *)CL_OBJ_TO_PTR(sym);
            if (pred == NULL || pred(sym)) {
                s->flags |= CL_SYM_EXPORTED;
                /* Every symbol in PACKAGE's own table that passes PRED
                 * ends up on the export list (already there, or pushed
                 * below) — so the per-home flag is exact here too. */
                if (s->package == package)
                    s->flags |= CL_SYM_EXPORTED_HOME;
                if (!(s->flags & CL_SYM_LISTED)) {
                    CL_Obj cell;
                    CL_GC_PROTECT(sym);
                    cell = cl_cons(sym, CL_NIL);
                    CL_GC_UNPROTECT(1);
                    pkg = (CL_Package *)CL_OBJ_TO_PTR(package);
                    ((CL_Cons *)CL_OBJ_TO_PTR(cell))->cdr = pkg->exported_symbols;
                    pkg->exported_symbols = cell;
                    s = (CL_Symbol *)CL_OBJ_TO_PTR(sym); /* re-resolve after cl_cons */
                    s->flags |= CL_SYM_LISTED;
                }
            }
            consumed++;
        }
    }
    {
        CL_Package *pkg = (CL_Package *)CL_OBJ_TO_PTR(package);
        clear_exported_listed(pkg->exported_symbols);
    }
    CL_GC_UNPROTECT(1);
}

static void export_all_present_symbols(CL_Obj package)
{
    export_symbols_where(package, NULL);
}

void cl_package_export_all_cl_symbols(void)
{
    export_all_present_symbols(cl_package_cl);
    export_all_present_symbols(cl_package_keyword);
    export_all_present_symbols(cl_package_amilisp);
    export_all_present_symbols(cl_package_mop);
}

/* Helper: check if a CL symbol has a real definition (function, value,
   macro, type, struct, or CLOS class).  Used to filter out stray symbols
   that were accidentally interned in CL by boot.lisp macro bodies. */
static int symbol_has_binding(CL_Obj sym)
{
    extern int cl_is_struct_type(CL_Obj type_sym);
    extern int cl_clos_class_exists(CL_Obj name);
    extern int cl_is_builtin_type_name(const char *name);

    CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(sym);

    /* Has function or value binding */
    if (s->function != CL_UNBOUND) return 1;
    if (s->value != CL_UNBOUND)    return 1;

    /* Is special or constant */
    if (s->flags & (CL_SYM_SPECIAL | CL_SYM_CONSTANT)) return 1;

    /* Is a macro or type expander */
    if (cl_macro_p(sym))                          return 1;
    if (!CL_NULL_P(cl_get_type_expander(sym)))    return 1;

    /* Is a struct type or CLOS class */
    if (cl_is_struct_type(sym))   return 1;
    if (cl_clos_class_exists(sym)) return 1;

    /* Is a built-in type name recognized by typep */
    if (cl_is_builtin_type_name(cl_symbol_name(sym))) return 1;

    return 0;
}

/* Export predicate for the COMMON-LISP discovery pass.  Two rules:
 *  1. Require an actual Lisp-level binding (symbol_has_binding).  The global
 *     CL_SYM_EXPORTED flag is NOT a valid heuristic here: it is set whenever
 *     ANY package exports the symbol, so a user defpackage that does
 *     (:export #:body) would flip it on the same BODY symbol boot.lisp
 *     accidentally interned in CL, which would then re-add BODY to
 *     COMMON-LISP's external set and break the ANSI NO-EXTRA-SYMBOLS test.
 *  2. Never export internal `%`-prefixed helpers.  No standard CL symbol
 *     starts with `%`; boot.lisp / clos.lisp define `%loop-...`/`%ccase-...`
 *     helpers as plain DEFUNs in CL (the current package during bootstrap),
 *     so whichever happen to be defined before this pass would otherwise leak
 *     into COMMON-LISP's external set — a timing-dependent inconsistency.
 * Non-allocating, as export_symbols_where requires. */
static int cl_symbol_is_exportable_defined(CL_Obj sym)
{
    const char *nm;
    if (!symbol_has_binding(sym)) return 0;
    nm = cl_symbol_name(sym);
    if (nm && nm[0] == '%') return 0;
    return 1;
}

void cl_package_export_defined_cl_symbols(void)
{
    /* Walk CL package, exporting each symbol with a real binding (or already
     * pre-flagged from boot) so find-symbol returns :EXTERNAL. */
    export_symbols_where(cl_package_cl, cl_symbol_is_exportable_defined);

    /* Keywords, AMILISP, and MOP are always fully exported */
    export_all_present_symbols(cl_package_keyword);
    export_all_present_symbols(cl_package_amilisp);
    export_all_present_symbols(cl_package_mop);
}

/* Internal helpers defined later by boot.lisp and clos.lisp.  Pre-interning
 * these in AMILISP before those files load means the reader resolves bare
 * %FOO via AMILISP's exports (CL and CL-USER both :use AMILISP), so the
 * defun creates the function on the AMILISP symbol rather than interning
 * a fresh CL::%FOO.  Keeping them out of CL is what makes the ANSI
 * NO-EXTRA-SYMBOLS-EXPORTED-FROM-COMMON-LISP test pass.
 *
 * Names that ARE registered with cl_package_amilisp from C (in builtins.c,
 * builtins_io.c, compiler.c etc.) are intentionally omitted — those slots
 * are already filled.
 *
 * KEEP THIS LIST COMPLETE.  A %-helper defined in boot.lisp / clos.lisp but
 * missing here interns in whatever package is current at read time — CL for
 * a boot-time SOURCE load, CL-USER under `make fasl` — so the two load paths
 * produce two distinct symbols with one name, and any cross-file reference
 * (a FASL compiled on the other path, or C code) silently lands on the
 * unbound one.  Found on the real Amiga 2026-08-09: a deploy that stamped
 * clos.lisp newer than clos.fasl source-compiled it against the shipped
 * boot.fasl and lost the dispatch self-heal chain, the reader/writer-IC
 * helpers, and defstruct's %REGISTER-STRUCT-CLASS MOP hookup.
 * tests/test_boot_source_compile.sh diffs the two boot paths and fails on
 * any %-helper divergence — run it after touching boot.lisp / clos.lisp. */
static const char *const clos_internal_names[] = {
    "+STANDARD-CLASS-SLOT-LAYOUT+",
    "%ACCESSIBLE-SYMBOLS",
    "%ACCESSOR-READER-BODY", "%ACCESSOR-WRITER-BODY",
    "%ADD-METHOD-TO-GF", "%APROPOS-PACKAGE", "%ASSIGN-SLOT-LOCATIONS",
    "%BOA-PATCH-DEFAULTS", "%BODY-SIMPLE-PRIMARY-P",
    "%CNM-CHAIN", "%CNM-INVOKE",
    "%BUILD-DISCRIMINATING-FUNCTION",
    "%BUILD-EFFECTIVE-METHOD", "%BUILD-LONG-EFFECTIVE-METHOD",
    "%BUILD-SHORT-EFFECTIVE-METHOD", "%BUILD-SLOT-INDEX-TABLE",
    "%C3-MERGE", "%CALL-METHOD-DESIGNATOR-FORM", "%CALL-METHOD-IMPL",
    "%CALL-WITH-METHOD-COMBINATION",
    "%CCASE-PLACE-P",
    "%CLASS-OF", "%CLONE-METHOD-COMBINATION", "%CLOS-TRACE",
    "%COMPUTE-APPLICABLE-METHODS",
    "%COMPUTE-BUILTIN-CPL", "%COMPUTE-CLASS-PRECEDENCE-LIST",
    "%COMPUTE-DEFAULT-INITARGS-DEFAULT",
    "%COMPUTE-EFFECTIVE-SLOT-DEFINITION-DEFAULT",
    "%COMPUTE-EFFECTIVE-SLOTS",
    "%COMPUTE-EQL-VALUE-SETS", "%COMPUTE-GF-CACHEABLE-P",
    "%COMPUTE-SLOTS-DEFAULT",
    "%COPY-STRUCT",
    "%DBIND-EXPAND", "%DBIND-PARSE-KEY", "%DBIND-PARSE-OPT",
    "%DCM-SPLIT-ENV", "%DCM-SPLIT-WHOLE",
    "%DEFINE-LONG-METHOD-COMBINATION",
    "%DEFINE-SHORT-METHOD-COMBINATION",
    "%DEFSTRUCT-PARSE-SLOT",
    "%DEMOTE-ALL-READER-GFS", "%DEMOTE-ALL-WRITER-GFS",
    "%DEMOTE-READER-GF", "%DEMOTE-WRITER-GF",
    "%DIRECT-TO-EFFECTIVE", "%DISPATCH-BUILD-EMF",
    "%DISPATCH-DUMP-METADATA", "%DISPATCH-HEAL-EMPTY",
    "%DISPATCH-NEGATIVE-HIT", "%DISPATCH-STANDARD-EMF",
    "%EFFECTIVE-SLOT-DEF-P", "%EMF-DIRECT-P",
    "%ENSURE-CLASS", "%ENSURE-CLASS-VIA-METACLASS", "%ENSURE-DIRS-HELPER",
    "%EXPAND-EXTENDED-LOOP", "%EXPAND-SIMPLE-LOOP",
    "%FILTER-METHODS-BY-SPEC",
    "%FINALIZE-AND-REGISTER-CLASS",
    /* class redefinition in place (CLHS 4.3.6) */
    "%FINALIZE-AND-REGISTER-STRUCT-TYPE", "%UNFINALIZE-CLASS",
    "%REFINALIZE-SUBCLASSES", "%REUSABLE-CLASS-METAOBJECT-P",
    "%UNLINK-FROM-OLD-SUPERS", "%INITIALIZE-CLASS-METAOBJECT",
    "%FINALIZE-INHERITANCE-BODY",
    "%FIND-INHERITED-CLASS-SLOT-CELL",
    "%FIND-INITARG-VALUE", "%FIND-SLOT-DEF",
    "%FIND-STRUCT-SLOT-INDEX",
    "%FOLD-PARENT-INTO-EFFECTIVE",
    "%GET-DEFSETF-SETTER",
    "%GF-1-NO-METHOD-ERROR", "%GF-2-NO-METHOD-ERROR", "%GF-2-RESOLVE",
    "%GF-DISPATCH", "%GF-DISPATCH-1", "%GF-DISPATCH-1-SLOW",
    "%GF-DISPATCH-2", "%GF-DISPATCH-2-SLOW", "%GF-DISPATCH-CACHED",
    "%GF-DISPATCH-ENTRY", "%GF-DISPATCH-EQL",
    "%GF-LAMBDA-LIST-REQUIRED-COUNT", "%GF-OR-NAME",
    "%GF-READER-1-MISS", "%GF-READER-SLOT-NAME",
    "%GF-ROSTER-HAS-PRIMARY-P",
    "%GF-WRITER-1-MISS", "%GF-WRITER-SLOT-NAME",
    "%HASH-TABLE-PAIRS",
    "%INITARG-TO-SLOT-INDEX",
    "%INSTALL-METHOD-IN-GF",
    "%INVALIDATE-ALL-GF-CACHES",
    "%LOOP-ACCUM-BODY", "%LOOP-ACCUM-KEYWORD-P",
    "%LOOP-ALL-SIMPLE-SETQ-P", "%LOOP-BUILD-THEN-STEP",
    "%LOOP-DESTRUCTURE-ASSIGNS", "%LOOP-DESTRUCTURE-BINDINGS",
    "%LOOP-DESTRUCTURE-VARS",
    "%LOOP-KEYWORD-P", "%LOOP-KEYWORD-SYM-P",
    "%LOOP-LIST-ACCUM-P",
    "%LOOP-PARALLELIZE-SETQS",
    "%LOOP-PARSE-COND-SUBCLAUSE", "%LOOP-PARSE-FOR",
    "%LOOP-SIMPLE-TYPE-SPEC-P", "%LOOP-SKIP-TYPE-SPEC",
    "%MAKE-ANON-METHOD", "%MAKE-AROUND-CHAIN",
    "%MAKE-BOOTSTRAP-CLASS",
    "%MAKE-DIRECT-SLOT-DEF", "%MAKE-DISPATCH-CACHE",
    "%MAKE-EFFECTIVE-SLOT-DEF",
    "%MAKE-METHOD-CHAIN", "%MAKE-METHOD-DESIGNATOR-P",
    "%MAKE-READER-DISCRIMINATOR", "%MAKE-RECURSIVE-LOCK",
    "%MAKE-STRUCT", "%MAKE-WRITER-DISCRIMINATOR",
    "%MATCH-QUALIFIER-PATTERN",
    "%MERGE-METACLASS-DEFAULT-INITARGS", "%METACLASS-P",
    "%METHOD-APPLICABLE-P", "%METHOD-COMBINATION-KEY",
    "%METHOD-GROUP-ORDER",
    "%METHOD-MORE-SPECIFIC-P", "%METHODS-HAVE-PRIMARY-P",
    "%NO-APPLICABLE-METHOD",
    "%NOTE-READER-METHOD", "%NOTE-WRITER-METHOD",
    "%NOTIFY-DEPENDENTS",
    "%OBJECT-LOAD-FORM-SLOT-NAMES", "%OBJECT-LOAD-FORM-TYPE-NAME",
    "%PACKAGE-EXTERNAL-SYMBOLS", "%PACKAGE-SYMBOLS",
    "%PARSE-SLOT-SPEC", "%PARSE-SPECIALIZED-LAMBDA-LIST",
    "%PLACE-DIRECT-MUTATOR-P", "%PLACE-DIRECT-MUTATORS",
    "%READER-IC-ENTRIES", "%RECOMPUTE-METHODS-UNTIL",
    "%REFRESH-READER-DISCRIMINATOR", "%REFRESH-WRITER-DISCRIMINATOR",
    "%REGISTER-CONDITION-CLASS", "%REGISTER-CONDITION-TYPE",
    "%REGISTER-SETF-EXPANDER", "%REGISTER-SETF-FUNCTION",
    "%REGISTER-STANDARD-COMBINATION",
    "%REGISTER-STRUCT-CLASS", "%REGISTER-STRUCT-TYPE",
    "%REMF",
    "%STRUCT-KEYWORD-CTOR-EXPAND",
    "%REPORT-DISPATCH-NO-PRIMARY",
    "%RESOLVE-METHOD-COMBINATION", "%RESOLVE-SPECIALIZERS",
    "%SYMBOL-MACRO-EXPANSION",
    "%SEQ-CTOR-INFO", "%SEQ-ELT-TYPE-IS-BIT",
    "%SEQ-ELT-TYPE-IS-CHAR", "%SEQ-LEN-ARG",
    "%SET-CLASS-CPL",
    "%SET-CLASS-DEFAULT-INITARGS",
    "%SET-CLASS-DIRECT-DEFAULT-INITARGS",
    "%SET-CLASS-DIRECT-METHODS",
    "%SET-CLASS-DIRECT-SLOTS",
    "%SET-CLASS-DIRECT-SUBCLASSES",
    "%SET-CLASS-EFFECTIVE-SLOTS",
    "%SET-CLASS-FINALIZED-P",
    "%SET-CLASS-SLOT-INDEX-TABLE",
    "%SET-CLOS-CLASS-TABLE",
    "%SET-CONDITION-DEFAULT-INITARGS",
    "%SET-CONDITION-SLOT-VALUE",
    "%SET-DOCUMENTATION", "%SET-FIND-CLASS",
    "%SET-GF-CACHEABLE-P", "%SET-GF-DISCRIMINATING-FUNCTION",
    "%SET-GF-DISPATCH-CACHE", "%SET-GF-EQL-VALUE-SETS",
    "%SET-GF-INLINE-CACHE",
    "%SET-GF-METHOD-COMBINATION", "%SET-GF-METHODS",
    "%SET-MEMBER",
    "%SET-METHOD-GENERIC-FUNCTION", "%SET-METHOD-SIMPLE-PRIMARY-P",
    "%SET-SLOT-DEFINITION-DOCUMENTATION",
    "%SET-SLOT-DEFINITION-INITARGS",
    "%SET-SLOT-DEFINITION-INITFORM",
    "%SET-SLOT-DEFINITION-INITFUNCTION",
    "%SET-SLOT-DEFINITION-LOCATION",
    "%SET-SLOT-DEFINITION-TYPE",
    "%SET-SLOT-VALUE", "%SET-SLOT-VALUE-SLOW",
    "%SET-STANDARD-INSTANCE-ACCESS",
    "%SETF-ELT", "%SETF-READTABLE-CASE",
    "%SHORT-COMBINE",
    "%SLOT-ACCESS-PROTOCOL-GF-P",
    "%SLOT-BOUNDP-SLOW", "%SLOT-NAME-CONSTANT-P",
    "%SLOT-SPEC->DIRECT-DEF-FORM",
    "%SLOT-SPEC-ALL-OPTIONS", "%SLOT-SPEC-HAS-OPTION-P",
    "%SLOT-SPEC-NAME", "%SLOT-SPEC-OPTION",
    "%SLOT-VALUE-SLOW",
    "%STRUCT-CHANGE-CLASS",
    "%STRUCT-REF", "%STRUCT-SET",
    "%STRUCT-SETTER-NAME",
    "%STRUCT-SLOT-NAMES", "%STRUCT-SLOT-SPECS",
    "%STRUCT-TYPE-NAME",
    "%SUBCLASSP", "%SUBST-IT",
    "%SYMBOL-CONSTANT-P",
    "%TREE-CONTAINS-SYMBOL-NAMED-P",
    "%UNINSTALL-METHOD-FROM-GF",
    /* AmiLisp internal CLOS state and helpers (non-MOP).  These are
     * implementation details that boot.lisp / clos.lisp introduce while
     * loading in the CL package; pre-interning them here keeps them in
     * AMILISP so they don't pollute COMMON-LISP's external symbols. */
    "*%CLOS-LOAD-START*", "*%CLOS-PREV*",
    "*CNM*",
    "*CLASS-TABLE*",
    "*CLOS-DIAGNOSE-NO-APPLICABLE*", "*CLOS-DIAGNOSE-NO-PRIMARY*",
    "*CURRENT-METHOD-ARGS*",
    "*DISPATCH-HEAL-RETRIES*",
    "*DOCUMENTATION-TABLE*",
    "*EQL-SPECIALIZER-INTERN-LOCK*", "*EQL-SPECIALIZER-TABLE*",
    "*GENERIC-FUNCTION-TABLE*",
    "*GF-CACHE-HEALS*", "*GF-METHODS-LOCK*",
    "*METAOBJECT-DEPENDENTS*",
    "*METHOD-COMBINATIONS*",
    /* *PRINT-OBJECT-HOOK* is interned directly in AMILISP by symbol.c. */
    "*READER-GFS*", "*READER-METHOD-SLOTS*",
    "*SLOT-ACCESS-PROTOCOL-EXTENDED-P*",
    "*SLOT-UNBOUND-MARKER*",
    "*WRITER-GFS*", "*WRITER-METHOD-SLOTS*",
    /* BODY and TEST are too generic to safely pre-intern in AMILISP —
     * doing so would alias user-code symbols of the same name through
     * the CL :use AMILISP inheritance chain.  They leak from
     * boot.lisp / clos.lisp macro expansions and need a source-level
     * fix (e.g. wrapping the offending macros with GENSYM).
     * CONDITIONP / CONDITION-TYPE-NAME / CONDITION-SLOT-VALUE are
     * registered as AMILISP builtins by builtins_condition.c. */
    "GF-CACHEABLE-P", "GF-DISCRIMINATING-FUNCTION",
    "GF-DISPATCH-CACHE", "GF-EQL-VALUE-SETS",
    "GF-INLINE-CACHE",
    "GF-LAMBDA-LIST", "GF-METHOD-COMBINATION",
    "GF-METHODS", "GF-NAME",
    "METHOD-COMBINATION-NAME", "METHOD-COMBINATION-OPTIONS",
    "METHOD-COMBINATION-TYPE", "METHOD-SIMPLE-PRIMARY-P",
    /* NAMED-LAMBDA, QUASIQUOTE, UNQUOTE, UNQUOTE-SPLICING are interned
     * directly in AMILISP by symbol.c (cl_symbol_init) — listed there
     * to avoid duplication. */
    /* STRUCTUREP, ADD/REMOVE-PACKAGE-LOCAL-NICKNAME, PACKAGE-LOCAL-NICKNAMES
     * are registered as AMILISP builtins by builtins_struct.c /
     * builtins_package.c. */
    NULL
};

/* CLOS Metaobject Protocol API names (AMOP / closer-mop).  These are
 * defined by clos.lisp while it loads in the CL package; pre-interning
 * them in MOP and having CL :use MOP makes the reader resolve bare
 * MOP-symbol references to the inherited MOP symbol so defun/defmethod
 * bind on the MOP symbol. */
static const char *const mop_api_names[] = {
    "ACCESSOR-METHOD-SLOT-DEFINITION",
    "ADD-DEPENDENT", "ADD-DIRECT-METHOD", "ADD-DIRECT-SUBCLASS",
    "CLASS-DEFAULT-INITARGS",
    "CLASS-DIRECT-DEFAULT-INITARGS",
    "CLASS-DIRECT-METHODS", "CLASS-DIRECT-SLOTS",
    "CLASS-DIRECT-SUBCLASSES", "CLASS-DIRECT-SUPERCLASSES",
    "CLASS-EFFECTIVE-SLOTS",
    "CLASS-FINALIZED-P", "CLASS-PRECEDENCE-LIST",
    "CLASS-PROTOTYPE",
    "CLASS-SLOT-INDEX-TABLE",
    "CLASS-SLOTS", "CLASSP",
    "COMPUTE-APPLICABLE-METHODS-USING-CLASSES",
    "COMPUTE-CLASS-PRECEDENCE-LIST",
    "COMPUTE-DEFAULT-INITARGS",
    "COMPUTE-DISCRIMINATING-FUNCTION",
    "COMPUTE-EFFECTIVE-METHOD",
    "COMPUTE-EFFECTIVE-SLOT-DEFINITION",
    "COMPUTE-SLOTS",
    "DIRECT-SLOT-DEFINITION", "DIRECT-SLOT-DEFINITION-CLASS",
    "EFFECTIVE-SLOT-DEFINITION", "EFFECTIVE-SLOT-DEFINITION-CLASS",
    "ENSURE-CLASS", "ENSURE-CLASS-USING-CLASS",
    "ENSURE-GENERIC-FUNCTION-USING-CLASS", "ENSURE-METHOD",
    "EQL-SPECIALIZER", "EQL-SPECIALIZER-OBJECT", "EQL-SPECIALIZER-P",
    "EXTRACT-LAMBDA-LIST", "EXTRACT-SPECIALIZER-NAMES",
    "FINALIZE-INHERITANCE",
    "FIND-METHOD-COMBINATION",
    "FORWARD-REFERENCED-CLASS",
    "FUNCALLABLE-STANDARD-CLASS",
    "FUNCALLABLE-STANDARD-INSTANCE-ACCESS",
    "FUNCALLABLE-STANDARD-OBJECT",
    "GENERIC-FUNCTION-ARGUMENT-PRECEDENCE-ORDER",
    "GENERIC-FUNCTION-DECLARATIONS", "GENERIC-FUNCTION-LAMBDA-LIST",
    "GENERIC-FUNCTION-METHOD-CLASS",
    "GENERIC-FUNCTION-METHOD-COMBINATION",
    "GENERIC-FUNCTION-METHODS", "GENERIC-FUNCTION-NAME",
    "INTERN-EQL-SPECIALIZER",
    "MAKE-METHOD-LAMBDA", "MAP-DEPENDENTS",
    "METAOBJECT",
    "METHOD-FUNCTION", "METHOD-GENERIC-FUNCTION",
    "METHOD-LAMBDA-LIST", "METHOD-SPECIALIZERS",
    "READER-METHOD-CLASS",
    "REMOVE-DEPENDENT", "REMOVE-DIRECT-METHOD",
    "REMOVE-DIRECT-SUBCLASS",
    "SET-FUNCALLABLE-INSTANCE-FUNCTION",
    "SLOT-BOUNDP-USING-CLASS",
    "SLOT-DEFINITION",
    "SLOT-DEFINITION-ALLOCATION", "SLOT-DEFINITION-DOCUMENTATION",
    "SLOT-DEFINITION-INITARGS", "SLOT-DEFINITION-INITFORM",
    "SLOT-DEFINITION-INITFUNCTION", "SLOT-DEFINITION-LOCATION",
    "SLOT-DEFINITION-NAME", "SLOT-DEFINITION-READERS",
    "SLOT-DEFINITION-TYPE", "SLOT-DEFINITION-WRITERS",
    "SLOT-MAKUNBOUND-USING-CLASS",
    "SLOT-VALUE-USING-CLASS",
    "SPECIALIZER",
    "SPECIALIZER-DIRECT-GENERIC-FUNCTIONS",
    "SPECIALIZER-DIRECT-METHODS",
    "STANDARD-ACCESSOR-METHOD",
    "STANDARD-DIRECT-SLOT-DEFINITION",
    "STANDARD-EFFECTIVE-SLOT-DEFINITION",
    "STANDARD-INSTANCE-ACCESS",
    "STANDARD-METHOD-COMBINATION",
    "STANDARD-READER-METHOD", "STANDARD-SLOT-DEFINITION",
    "STANDARD-WRITER-METHOD",
    "UPDATE-DEPENDENT", "VALIDATE-SUPERCLASS",
    "WRITER-METHOD-CLASS",
    NULL
};

void cl_intern_clos_internals_in_amilisp(void)
{
    const char *const *p;
    for (p = clos_internal_names; *p != NULL; p++) {
        (void)cl_intern_in(*p, (uint32_t)strlen(*p), cl_package_amilisp);
    }
    for (p = mop_api_names; *p != NULL; p++) {
        (void)cl_intern_in(*p, (uint32_t)strlen(*p), cl_package_mop);
    }
}

void cl_package_init(void)
{
    if (!cl_package_rwlock)
        platform_rwlock_init(&cl_package_rwlock);

    /* After a shutdown/re-init cycle (test harnesses) the registry
     * still holds PREVIOUS-arena offsets; cl_make_package below would
     * prepend onto that stale tail, and gc_mark_obj(cl_package_registry)
     * would then set "mark bits" at interior positions of unrelated
     * fresh-arena objects — silent corruption whose victim moves with
     * heap layout.  No-op in a normal once-per-process boot. */
    cl_package_registry = CL_NIL;

    /* Create the three standard packages */
    cl_package_cl = cl_make_package("COMMON-LISP");
    CL_GC_PROTECT(cl_package_cl);

    cl_package_keyword = cl_make_package("KEYWORD");
    CL_GC_PROTECT(cl_package_keyword);

    cl_package_cl_user = cl_make_package("COMMON-LISP-USER");
    CL_GC_PROTECT(cl_package_cl_user);

    cl_package_ext = cl_make_package("EXT");
    CL_GC_PROTECT(cl_package_ext);

    cl_package_amilisp = cl_make_package("AMILISP");
    CL_GC_PROTECT(cl_package_amilisp);

    cl_package_mop = cl_make_package("MOP");
    CL_GC_PROTECT(cl_package_mop);

    cl_package_mp = cl_make_package("MP");
    CL_GC_PROTECT(cl_package_mp);

    cl_package_ffi = cl_make_package("FFI");
    CL_GC_PROTECT(cl_package_ffi);

    /* The AMIGA package exists on every platform so that AmigaOS binding
     * modules (the lib/amiga/ tree) can be READ, compiled and unit-tested
     * on the host — only the library-call builtins are platform-specific
     * (builtins_amiga.c installs stubs that signal a clear error on
     * non-Amiga builds). */
    cl_package_amiga = cl_make_package("AMIGA");
    CL_GC_PROTECT(cl_package_amiga);

    /* Register in global registry */
    cl_register_package(cl_package_cl);
    cl_register_package(cl_package_keyword);
    cl_register_package(cl_package_cl_user);
    cl_register_package(cl_package_ext);
    cl_register_package(cl_package_amilisp);
    cl_register_package(cl_package_mop);
    cl_register_package(cl_package_mp);
    cl_register_package(cl_package_ffi);
    cl_register_package(cl_package_amiga);

    /* Add nicknames.  Derive the package pointer only AFTER the string
     * and cons allocations — cl_package_cl/_cl_user are registered roots,
     * so re-reading them post-alloc always yields the forwarded package
     * (identity at boot, but correct by contract). */
    {
        CL_Obj nick = cl_make_string("CL", 2);
        CL_Package *cl_pkg;
        nick = cl_cons(nick, CL_NIL);
        cl_pkg = (CL_Package *)CL_OBJ_TO_PTR(cl_package_cl);
        cl_pkg->nicknames = nick;
    }
    {
        CL_Obj nick = cl_make_string("CL-USER", 7);
        CL_Package *user_pkg;
        nick = cl_cons(nick, CL_NIL);
        user_pkg = (CL_Package *)CL_OBJ_TO_PTR(cl_package_cl_user);
        user_pkg->nicknames = nick;
    }
    /* AMILISP is the implementation package's name since the rename to
     * AmiLisp (0.12); CLAMIGA, its name before, stays as a nickname so
     * user code and the library forks that say clamiga:: keep working. */
    {
        CL_Obj nick = cl_make_string("CLAMIGA", 7);
        CL_Package *impl_pkg;
        nick = cl_cons(nick, CL_NIL);
        impl_pkg = (CL_Package *)CL_OBJ_TO_PTR(cl_package_amilisp);
        impl_pkg->nicknames = nick;
    }

    /* CL-USER uses CL, EXT, AMILISP, MOP, MP, FFI, AMIGA */
    cl_use_package(cl_package_cl, cl_package_cl_user);
    cl_use_package(cl_package_ext, cl_package_cl_user);
    cl_use_package(cl_package_amilisp, cl_package_cl_user);
    cl_use_package(cl_package_mop, cl_package_cl_user);
    cl_use_package(cl_package_mp, cl_package_cl_user);
    cl_use_package(cl_package_ffi, cl_package_cl_user);
    cl_use_package(cl_package_amiga, cl_package_cl_user);

    /* EXT uses CL */
    cl_use_package(cl_package_cl, cl_package_ext);

    /* MP uses CL */
    cl_use_package(cl_package_cl, cl_package_mp);

    /* FFI uses CL */
    cl_use_package(cl_package_cl, cl_package_ffi);

    /* AMIGA uses CL and FFI */
    cl_use_package(cl_package_cl, cl_package_amiga);
    cl_use_package(cl_package_ffi, cl_package_amiga);

    /* AMILISP uses CL; CL uses AMILISP so boot.lisp/clos.lisp can
       access internal builtins without package qualification.  Same
       trick for MOP — clos.lisp defines MOP API symbols expecting them
       to live in MOP, but the file itself loads in CL; CL :use MOP
       makes the reader resolve unqualified MOP names to the inherited
       MOP symbol so defun/defmethod bind on the MOP symbol. */
    cl_use_package(cl_package_cl, cl_package_amilisp);
    cl_use_package(cl_package_amilisp, cl_package_cl);
    cl_use_package(cl_package_mop, cl_package_cl);
    cl_use_package(cl_package_cl, cl_package_mop);

    cl_current_package = cl_package_cl_user;

    /* Pre-intern and export standard CL type names that are recognized by
       the C-level typep but have no Lisp-level function/value binding.
       Must happen early so packages that :use CL inherit them correctly. */
    {
        static const char *builtin_type_names[] = {
            "SIMPLE-VECTOR", "SIMPLE-ARRAY", "SIMPLE-BIT-VECTOR",
            "BASE-CHAR", "STANDARD-CHAR",
            "COMPILED-FUNCTION", "ATOM", "BOOLEAN",
            "FIXNUM", "BIGNUM", "BIT-VECTOR",
            "SINGLE-FLOAT", "DOUBLE-FLOAT", "SHORT-FLOAT", "LONG-FLOAT",
            NULL
        };
        const char **p;
        for (p = builtin_type_names; *p; p++) {
            CL_Obj sym = cl_intern_in(*p, (uint32_t)strlen(*p), cl_package_cl);
            cl_export_symbol(sym, cl_package_cl);
        }
    }

    /* Pop ALL package roots pushed above.  They must NOT stay on the
     * thread root stack: the same addresses are registered as global
     * roots right below, and gc_forward is not idempotent (the
     * forwarding table is keyed by pre-compaction offsets), so a slot
     * reachable from two root lists would be forwarded twice on
     * compaction — the second lookup maps the already-new offset
     * through an unrelated object's old offset, corrupting the global.
     * cl_gc_register_root does not allocate, so no GC can run in the
     * unprotected window before registration. */
    CL_GC_UNPROTECT(9);   /* CL KEYWORD CL-USER EXT AMILISP MOP MP FFI AMIGA */

    /* Register package globals for GC compaction forwarding */
    cl_gc_register_root(&cl_package_cl);
    cl_gc_register_root(&cl_package_cl_user);
    cl_gc_register_root(&cl_package_keyword);
    cl_gc_register_root(&cl_package_ext);
    cl_gc_register_root(&cl_package_amilisp);
    cl_gc_register_root(&cl_package_mop);
    cl_gc_register_root(&cl_package_mp);
    cl_gc_register_root(&cl_package_ffi);
    cl_gc_register_root(&cl_package_amiga);
    /* cl_current_package is NOT registered here: it is a per-thread slot
     * (CL_Thread.current_package) marked and forwarded by the thread walk
     * in gc_visit_thread_roots.  Registering the
     * main thread's slot as a global root as well would forward it TWICE
     * on compaction — the double-forward corruption documented above. */
}
