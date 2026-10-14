#include "builtins.h"
#include "symbol.h"
#include "package.h"
#include "bindtab.h"
#include "mem.h"
#include "error.h"
#include "vm.h"
#include "string_utils.h"
#include "../platform/platform.h"
#include <string.h>

/* Pre-interned keyword symbols */
static CL_Obj KW_INTERNAL = CL_NIL;
static CL_Obj KW_EXTERNAL = CL_NIL;
static CL_Obj KW_INHERITED = CL_NIL;
static CL_Obj KW_NICKNAMES = CL_NIL;
static CL_Obj KW_USE = CL_NIL;
static CL_Obj KW_LOCAL_NICKNAMES = CL_NIL;

/* ---- Helpers ---- */

/* Coerce argument to package leniently: NIL -> current package, package ->
 * itself, string/symbol -> cl_find_package (CL_NIL when no such package
 * exists — the caller decides whether that's an error).  Non-designator
 * objects always signal a type error. */
static CL_Obj coerce_to_package_or_nil(CL_Obj arg)
{
    if (CL_NULL_P(arg)) return cl_current_package;

    if (CL_PACKAGE_P(arg)) return arg;

    if (CL_STRING_P(arg)) {
        CL_String *s = (CL_String *)CL_OBJ_TO_PTR(arg);
        return cl_find_package(s->data, s->length);
    }

    if (CL_SYMBOL_P(arg)) {
        CL_Symbol *sym = (CL_Symbol *)CL_OBJ_TO_PTR(arg);
        CL_String *name = (CL_String *)CL_OBJ_TO_PTR(sym->name);
        return cl_find_package(name->data, name->length);
    }

    cl_error(CL_ERR_TYPE, "Not a package designator");
    return CL_NIL;
}

/* Coerce argument to package: as above, but a name that designates no
 * package is an error (the behavior of INTERN, EXPORT, IN-PACKAGE, ...). */
static CL_Obj coerce_to_package(CL_Obj arg)
{
    CL_Obj pkg = coerce_to_package_or_nil(arg);
    if (!CL_NULL_P(pkg)) return pkg;

    if (CL_STRING_P(arg)) {
        CL_String *s = (CL_String *)CL_OBJ_TO_PTR(arg);
        cl_error(CL_ERR_GENERAL, "Package \"%.*s\" not found",
                 (int)s->length, s->data);
    }
    if (CL_SYMBOL_P(arg)) {
        CL_Symbol *sym = (CL_Symbol *)CL_OBJ_TO_PTR(arg);
        CL_String *name = (CL_String *)CL_OBJ_TO_PTR(sym->name);
        cl_error(CL_ERR_GENERAL, "Package \"%.*s\" not found",
                 (int)name->length, name->data);
    }
    cl_error(CL_ERR_GENERAL, "Package not found");
    return CL_NIL;
}

/* Get C string + length from a string or symbol name */
static void get_name_str(CL_Obj arg, const char **out_name, uint32_t *out_len)
{
    if (CL_STRING_P(arg)) {
        CL_String *s = (CL_String *)CL_OBJ_TO_PTR(arg);
        *out_name = s->data;
        *out_len = s->length;
    } else if (CL_NULL_P(arg)) {
        /* NIL is a symbol with name "NIL" */
        *out_name = "NIL";
        *out_len = 3;
    } else if (CL_SYMBOL_P(arg)) {
        CL_Symbol *sym = (CL_Symbol *)CL_OBJ_TO_PTR(arg);
        CL_String *s = (CL_String *)CL_OBJ_TO_PTR(sym->name);
        *out_name = s->data;
        *out_len = s->length;
    } else {
        cl_error(CL_ERR_TYPE, "Expected string or symbol");
    }
}

/* Return status keyword from int code */
static CL_Obj status_to_keyword(int status)
{
    switch (status) {
    case 1: return KW_INTERNAL;
    case 2: return KW_EXTERNAL;
    case 3: return KW_INHERITED;
    default: return CL_NIL;
    }
}

/* ---- Builtins ---- */

/* (make-package name &key nicknames use) */
static CL_Obj bi_make_package(CL_Obj *args, int nargs)
{
    const char *name;
    uint32_t len;
    CL_Obj pkg, existing;
    CL_Obj nicknames = CL_NIL;
    CL_Obj use = CL_NIL;
    CL_Obj local_nicks = CL_NIL;
    int i;

    get_name_str(args[0], &name, &len);

    /* Check for existing package */
    existing = cl_find_package(name, len);
    if (!CL_NULL_P(existing)) {
        cl_error(CL_ERR_GENERAL, "Package already exists");
        return CL_NIL;
    }

    /* Parse keyword args */
    for (i = 1; i + 1 < nargs; i += 2) {
        if (args[i] == KW_NICKNAMES) {
            nicknames = args[i + 1];
        } else if (args[i] == KW_USE) {
            use = args[i + 1];
        } else if (args[i] == KW_LOCAL_NICKNAMES) {
            local_nicks = args[i + 1];
        }
    }

    /* GC SAFETY: cl_make_package and every loop below allocate and can
     * compact — the keyword-list locals (read from args before this point)
     * and all walk cursors must be forwarded roots. */
    CL_GC_PROTECT(nicknames);
    CL_GC_PROTECT(use);
    CL_GC_PROTECT(local_nicks);
    pkg = cl_make_package(name);
    CL_GC_PROTECT(pkg);

    /* Set nicknames — convert designators (symbols/strings) to string objects */
    if (!CL_NULL_P(nicknames)) {
        CL_Obj nick_list = CL_NIL;
        CL_Obj rest = nicknames;
        CL_GC_PROTECT(nick_list);
        CL_GC_PROTECT(rest);
        while (!CL_NULL_P(rest)) {
            CL_Obj nick = cl_car(rest);
            const char *nstr;
            uint32_t nlen;
            get_name_str(nick, &nstr, &nlen);
            CL_Obj nick_string = cl_make_string(nstr, nlen);
            nick_list = cl_cons(nick_string, nick_list);
            rest = cl_cdr(rest);
        }
        /* Re-derive the package pointer AFTER the allocating loop — a
         * pre-loop CL_Package* would be a stale write target. */
        ((CL_Package *)CL_OBJ_TO_PTR(pkg))->nicknames = nick_list;
        CL_GC_UNPROTECT(2);
    }

    /* Register */
    cl_register_package(pkg);

    /* Process :use list (cl_use_package conses — `use` is protected) */
    while (!CL_NULL_P(use)) {
        CL_Obj used_pkg = coerce_to_package(cl_car(use));
        cl_use_package(used_pkg, pkg);
        use = cl_cdr(use);
    }

    /* Process :local-nicknames list — each entry is (nick-name target-package) */
    while (!CL_NULL_P(local_nicks)) {
        CL_Obj pair = cl_car(local_nicks);
        CL_Obj nick_designator = cl_car(pair);
        CL_Obj nick_str, target_pkg;
        const char *nick_name;
        uint32_t nick_len;

        get_name_str(nick_designator, &nick_name, &nick_len);
        nick_str = cl_make_string(nick_name, nick_len);
        /* Re-read the pair from the protected cursor: cl_make_string above
         * may have compacted. */
        pair = cl_car(local_nicks);
        target_pkg = coerce_to_package(cl_car(cl_cdr(pair)));
        cl_add_package_local_nickname(nick_str, target_pkg, pkg);
        local_nicks = cl_cdr(local_nicks);
    }

    CL_GC_UNPROTECT(4);  /* pkg, local_nicks, use, nicknames */
    return pkg;
}

/* (find-package name) */
static CL_Obj bi_find_package(CL_Obj *args, int nargs)
{
    const char *name;
    uint32_t len;
    (void)nargs;

    if (CL_PACKAGE_P(args[0])) return args[0];

    get_name_str(args[0], &name, &len);
    return cl_find_package(name, len);
}

/* (clamiga::%set-current-package designator) — runtime in-package fallback.
 * Used by compile_in_package when the named package can't be resolved at
 * compile time (e.g. when a top-level let-body bundles `(load <file>)` and
 * `(in-package :PKG)` so that PKG is created by the load only at run time). */
static CL_Obj bi_pct_set_current_package(CL_Obj *args, int nargs)
{
    const char *name;
    uint32_t len;
    CL_Obj pkg;
    (void)nargs;

    if (CL_PACKAGE_P(args[0])) {
        pkg = args[0];
    } else {
        get_name_str(args[0], &name, &len);
        pkg = cl_find_package(name, len);
        if (CL_NULL_P(pkg))
            cl_error(CL_ERR_GENERAL, "Package %.*s not found", (int)len, name);
    }
    cl_set_symbol_value(SYM_STAR_PACKAGE, pkg);
    cl_current_package = pkg;
    return pkg;
}

/* (delete-package package) */
static CL_Obj bi_delete_package(CL_Obj *args, int nargs)
{
    CL_Obj pkg = coerce_to_package(args[0]);
    CL_Obj prev = CL_NIL;
    CL_Obj reg = cl_package_registry;
    (void)nargs;

    /* Remove from registry */
    while (!CL_NULL_P(reg)) {
        CL_Obj entry = cl_car(reg);
        if (cl_cdr(entry) == pkg) {
            if (CL_NULL_P(prev)) {
                cl_package_registry = cl_cdr(reg);
            } else {
                CL_Cons *c = (CL_Cons *)CL_OBJ_TO_PTR(prev);
                c->cdr = cl_cdr(reg);
            }
            return SYM_T;
        }
        prev = reg;
        reg = cl_cdr(reg);
    }
    return CL_NIL;
}

/* (rename-package package new-name &optional new-nicknames) */
static CL_Obj bi_rename_package(CL_Obj *args, int nargs)
{
    CL_Obj pkg = coerce_to_package(args[0]);
    CL_Obj name_str;
    const char *new_name;
    uint32_t new_len;

    /* GC SAFETY: cl_make_string can compact — never write through a
     * CL_Package* captured before an allocation; re-derive from the
     * protected pkg handle at each store. */
    CL_GC_PROTECT(pkg);
    get_name_str(args[1], &new_name, &new_len);
    name_str = cl_make_string(new_name, new_len);
    ((CL_Package *)CL_OBJ_TO_PTR(pkg))->name = name_str;

    if (nargs > 2 && !CL_NULL_P(args[2])) {
        CL_Obj nick_list = CL_NIL;
        CL_Obj rest = args[2];
        CL_GC_PROTECT(nick_list);
        CL_GC_PROTECT(rest);
        while (!CL_NULL_P(rest)) {
            CL_Obj nick = cl_car(rest);
            const char *nstr;
            uint32_t nlen;
            get_name_str(nick, &nstr, &nlen);
            CL_Obj nick_string = cl_make_string(nstr, nlen);
            nick_list = cl_cons(nick_string, nick_list);
            rest = cl_cdr(rest);
        }
        ((CL_Package *)CL_OBJ_TO_PTR(pkg))->nicknames = nick_list;
        CL_GC_UNPROTECT(2);
    } else {
        ((CL_Package *)CL_OBJ_TO_PTR(pkg))->nicknames = CL_NIL;
    }

    CL_GC_UNPROTECT(1);
    return pkg;
}

/* (export symbols &optional package) */
static CL_Obj bi_export(CL_Obj *args, int nargs)
{
    CL_Obj symbols = args[0];
    CL_Obj pkg = (nargs > 1) ? coerce_to_package(args[1]) : cl_current_package;

    if (CL_SYMBOL_P(symbols) || CL_NULL_P(symbols)) {
        /* Single symbol */
        cl_export_symbol(symbols, pkg);
    } else {
        /* List of symbols */
        /* GC SAFETY: cl_export_symbol conses (package lists grow) — the cursor
         * and pkg are re-read across those allocations. */
        CL_GC_PROTECT(symbols);
        CL_GC_PROTECT(pkg);
        while (!CL_NULL_P(symbols)) {
            cl_export_symbol(cl_car(symbols), pkg);
            symbols = cl_cdr(symbols);
        }
        CL_GC_UNPROTECT(2);
    }
    return SYM_T;
}

/* (unexport symbols &optional package) */
static CL_Obj bi_unexport(CL_Obj *args, int nargs)
{
    CL_Obj symbols = args[0];
    CL_Obj pkg = (nargs > 1) ? coerce_to_package(args[1]) : cl_current_package;

    /* export-set mutation on a lazy package: go eager first (bindtab.c) —
     * pkg/symbols must be rooted first, since materialising a whole table
     * allocates heavily and can compact. */
    CL_GC_PROTECT(symbols);
    CL_GC_PROTECT(pkg);
    cl_bindtab_materialize_all(pkg);
    if (CL_SYMBOL_P(symbols) || CL_NULL_P(symbols)) {
        cl_unexport_symbol(symbols, pkg);
    } else {
        while (!CL_NULL_P(symbols)) {
            cl_unexport_symbol(cl_car(symbols), pkg);
            symbols = cl_cdr(symbols);
        }
    }
    CL_GC_UNPROTECT(2);
    return SYM_T;
}

/* (import symbols &optional package) */
static CL_Obj bi_import(CL_Obj *args, int nargs)
{
    CL_Obj symbols = args[0];
    CL_Obj pkg = (nargs > 1) ? coerce_to_package(args[1]) : cl_current_package;

    if (CL_SYMBOL_P(symbols) || CL_NULL_P(symbols)) {
        cl_import_symbol(symbols, pkg);
    } else {
        /* GC SAFETY: cl_import_symbol conses (package lists grow) — the cursor
         * and pkg are re-read across those allocations. */
        CL_GC_PROTECT(symbols);
        CL_GC_PROTECT(pkg);
        while (!CL_NULL_P(symbols)) {
            cl_import_symbol(cl_car(symbols), pkg);
            symbols = cl_cdr(symbols);
        }
        CL_GC_UNPROTECT(2);
    }
    return SYM_T;
}

/* (use-package packages-to-use &optional package) */
static CL_Obj bi_use_package(CL_Obj *args, int nargs)
{
    CL_Obj to_use = args[0];
    CL_Obj pkg = (nargs > 1) ? coerce_to_package(args[1]) : cl_current_package;

    if (CL_PACKAGE_P(to_use) || CL_STRING_P(to_use) || CL_SYMBOL_P(to_use)) {
        cl_use_package(coerce_to_package(to_use), pkg);
    } else {
        /* List.  GC SAFETY: cl_use_package conses — protect cursor+pkg. */
        CL_GC_PROTECT(to_use);
        CL_GC_PROTECT(pkg);
        while (!CL_NULL_P(to_use)) {
            cl_use_package(coerce_to_package(cl_car(to_use)), pkg);
            to_use = cl_cdr(to_use);
        }
        CL_GC_UNPROTECT(2);
    }
    return SYM_T;
}

/* (unuse-package packages-to-unuse &optional package) */
static CL_Obj bi_unuse_package(CL_Obj *args, int nargs)
{
    CL_Obj to_unuse = args[0];
    CL_Obj pkg = (nargs > 1) ? coerce_to_package(args[1]) : cl_current_package;

    if (CL_PACKAGE_P(to_unuse) || CL_STRING_P(to_unuse) || CL_SYMBOL_P(to_unuse)) {
        cl_unuse_package(coerce_to_package(to_unuse), pkg);
    } else {
        CL_GC_PROTECT(to_unuse);
        CL_GC_PROTECT(pkg);
        while (!CL_NULL_P(to_unuse)) {
            cl_unuse_package(coerce_to_package(cl_car(to_unuse)), pkg);
            to_unuse = cl_cdr(to_unuse);
        }
        CL_GC_UNPROTECT(2);
    }
    return SYM_T;
}

/* (shadow symbols &optional package) */
static CL_Obj bi_shadow(CL_Obj *args, int nargs)
{
    CL_Obj symbols = args[0];
    CL_Obj pkg = (nargs > 1) ? coerce_to_package(args[1]) : cl_current_package;

    if (CL_STRING_P(symbols) || CL_SYMBOL_P(symbols)) {
        const char *name;
        uint32_t len;
        get_name_str(symbols, &name, &len);
        cl_shadow_symbol(name, len, pkg);
    } else {
        /* GC SAFETY: cl_shadow_symbol interns/conses — protect cursor+pkg. */
        CL_GC_PROTECT(symbols);
        CL_GC_PROTECT(pkg);
        while (!CL_NULL_P(symbols)) {
            const char *name;
            uint32_t len;
            get_name_str(cl_car(symbols), &name, &len);
            cl_shadow_symbol(name, len, pkg);
            symbols = cl_cdr(symbols);
        }
        CL_GC_UNPROTECT(2);
    }
    return SYM_T;
}

/* (find-symbol name &optional package) — returns 2 values: symbol, status */
static CL_Obj bi_find_symbol(CL_Obj *args, int nargs)
{
    const char *name;
    uint32_t len;
    CL_Obj pkg;
    int status;
    CL_Obj sym;

    if (!CL_STRING_P(args[0])) {
        cl_error(CL_ERR_TYPE, "FIND-SYMBOL: name must be a string");
        return CL_NIL;
    }

    {
        CL_String *s = (CL_String *)CL_OBJ_TO_PTR(args[0]);
        name = s->data;
        len = s->length;
    }

    /* DELIBERATE deviation from SBCL/CLISP (project decision, 2026-07-27):
       FIND-SYMBOL treats a designator that names no package as "not found"
       and returns (values nil nil) instead of signaling.  CLHS leaves a
       non-designating name undefined (find-symbol's Exceptional Situations
       is "None"), so this is a permissible choice — and it makes
       FIND-SYMBOL usable as a pure probe for optional packages, e.g.
       quicklisp-install's (find-symbol "*PROXY-URL*" "QL-HTTP"), which
       legitimately runs before the fetched quicklisp client has defined
       QL-HTTP (lib/quicklisp.lisp is only the bootstrap).  Non-designator
       objects still signal a type error; INTERN/EXPORT/... keep signaling
       on a missing package.  Do NOT "fix" this back to signaling. */
    pkg = (nargs > 1) ? coerce_to_package_or_nil(args[1]) : cl_current_package;
    if (CL_NULL_P(pkg)) {
        cl_mv_count = 2;
        cl_mv_values[0] = CL_NIL;
        cl_mv_values[1] = CL_NIL;
        return CL_NIL;
    }
    sym = cl_find_symbol_with_status(name, len, pkg, &status);

    cl_mv_count = 2;
    cl_mv_values[0] = sym;
    cl_mv_values[1] = status_to_keyword(status);
    return sym;
}

/* (intern name &optional package) — returns 2 values: symbol, status */
static CL_Obj bi_intern(CL_Obj *args, int nargs)
{
    const char *name;
    uint32_t len;
    CL_Obj pkg;
    int status;
    CL_Obj existing;
    CL_Obj sym;
    char namebuf[256];
    char *heapname = NULL;

    if (!CL_ANY_STRING_P(args[0])) {
        cl_error(CL_ERR_TYPE, "INTERN: name must be a string");
        return CL_NIL;
    }

    /* Copy the name into a GC-immune C buffer BEFORE any allocating call.
     * args[0]'s character data lives in the moving heap; cl_intern_in /
     * cl_find_symbol_with_status allocate (and may compact), which would
     * relocate args[0] and dangle a raw s->data pointer — interning a
     * garbage name.  A wide string (e.g. from (make-array n :element-type
     * 'character), as alexandria's SYMBOLICATE uses) is narrowed here too;
     * amilisp's symbol table is byte-based and names are overwhelmingly
     * ASCII/Latin-1, which round-trips exactly. */
    {
        uint32_t i;
        len = cl_string_length(args[0]);
        if (len < sizeof(namebuf)) {
            name = namebuf;
        } else {
            heapname = (char *)platform_alloc(len + 1);
            if (!heapname) {
                cl_error(CL_ERR_STORAGE, "INTERN: out of memory for name");
                return CL_NIL;
            }
            name = heapname;
        }
        for (i = 0; i < len; i++)
            ((char *)name)[i] = (char)cl_string_char_at(args[0], i);
    }

    pkg = (nargs > 1) ? coerce_to_package(args[1]) : cl_current_package;

    /* Check if already present */
    existing = cl_find_symbol_with_status(name, len, pkg, &status);
    if (status != 0) {
        if (heapname) platform_free(heapname);
        cl_mv_count = 2;
        cl_mv_values[0] = existing;
        cl_mv_values[1] = status_to_keyword(status);
        return existing;
    }

    /* Create new symbol in package */
    sym = cl_intern_in(name, len, pkg);
    if (heapname) platform_free(heapname);
    cl_mv_count = 2;
    cl_mv_values[0] = sym;
    cl_mv_values[1] = CL_NIL; /* NIL = newly created */
    return sym;
}

/* (unintern symbol &optional package) */
static CL_Obj bi_unintern(CL_Obj *args, int nargs)
{
    CL_Obj sym = args[0];
    CL_Obj pkg_obj;
    CL_Package *pkg;
    CL_Vector *tbl;
    CL_Symbol *s;
    uint32_t idx;
    CL_Obj prev, list;

    if (!CL_SYMBOL_P(sym)) {
        cl_error(CL_ERR_TYPE, "UNINTERN: not a symbol");
        return CL_NIL;
    }

    pkg_obj = (nargs > 1) ? coerce_to_package(args[1]) : cl_current_package;
    /* A lazy package (demand-interned binding table, bindtab.c) goes
     * eager before its symbol set is mutated, so an uninterned table name
     * stays gone instead of being rebuilt by the next lookup.
     * GC SAFETY: sym/pkg_obj must be rooted first — materialising a whole
     * table allocates heavily and can compact. */
    CL_GC_PROTECT(sym);
    CL_GC_PROTECT(pkg_obj);
    cl_bindtab_materialize_all(pkg_obj);
    CL_GC_UNPROTECT(2);
    pkg = (CL_Package *)CL_OBJ_TO_PTR(pkg_obj);
    tbl = (CL_Vector *)CL_OBJ_TO_PTR(pkg->symbols);
    s = (CL_Symbol *)CL_OBJ_TO_PTR(sym);
    idx = s->hash % tbl->length;

    /* Remove from bucket chain */
    prev = CL_NIL;
    list = tbl->data[idx];
    while (!CL_NULL_P(list)) {
        if (cl_car(list) == sym) {
            if (CL_NULL_P(prev)) {
                tbl->data[idx] = cl_cdr(list);
            } else {
                CL_Cons *c = (CL_Cons *)CL_OBJ_TO_PTR(prev);
                c->cdr = cl_cdr(list);
            }
            pkg->sym_count--;
            /* Clear home package if this was it (and the per-home export
             * flag with it — it tracks the OLD home's list, see types.h) */
            if (s->package == pkg_obj) {
                s->package = CL_NIL;
                s->flags &= ~CL_SYM_EXPORTED_HOME;
            }
            return SYM_T;
        }
        prev = list;
        list = cl_cdr(list);
    }
    return CL_NIL;
}

/* (package-name package) */
static CL_Obj bi_package_name(CL_Obj *args, int nargs)
{
    CL_Obj pkg = coerce_to_package(args[0]);
    CL_Package *p = (CL_Package *)CL_OBJ_TO_PTR(pkg);
    (void)nargs;
    return p->name;
}

/* (package-use-list package) */
static CL_Obj bi_package_use_list(CL_Obj *args, int nargs)
{
    CL_Obj pkg = coerce_to_package(args[0]);
    CL_Package *p = (CL_Package *)CL_OBJ_TO_PTR(pkg);
    (void)nargs;
    return p->use_list;
}

/* (package-nicknames package) */
static CL_Obj bi_package_nicknames(CL_Obj *args, int nargs)
{
    CL_Obj pkg = coerce_to_package(args[0]);
    CL_Package *p = (CL_Package *)CL_OBJ_TO_PTR(pkg);
    (void)nargs;
    return p->nicknames;
}

/* (list-all-packages) */
static CL_Obj bi_list_all_packages(CL_Obj *args, int nargs)
{
    CL_Obj result = CL_NIL;
    CL_Obj reg = cl_package_registry;
    (void)args;
    (void)nargs;

    CL_GC_PROTECT(result);
    while (!CL_NULL_P(reg)) {
        CL_Obj entry = cl_car(reg);
        result = cl_cons(cl_cdr(entry), result);
        reg = cl_cdr(reg);
    }
    CL_GC_UNPROTECT(1);
    return result;
}

/* (%package-symbols package) — list of all symbols in package */
static CL_Obj bi_package_symbols(CL_Obj *args, int nargs)
{
    CL_Obj pkg_obj = coerce_to_package(args[0]);
    CL_Obj result = CL_NIL;
    uint32_t i, tbl_len;
    (void)nargs;

    /* Enumeration of a lazy package (demand-interned binding table,
     * bindtab.c) materialises every entry first — CLHS enumerates the
     * whole symbol set, and from here on the package is ordinary. */
    CL_GC_PROTECT(pkg_obj);
    cl_bindtab_materialize_all(pkg_obj);
    /* GC SAFETY: cl_cons can compact — re-derive the symbol-table pointer
     * from the protected package handle on every access and root the
     * bucket cursor (raw vector pointers and cursor locals go stale). */
    CL_GC_PROTECT(result);
    tbl_len = ((CL_Vector *)CL_OBJ_TO_PTR(
                   ((CL_Package *)CL_OBJ_TO_PTR(pkg_obj))->symbols))->length;
    {
        CL_Obj bucket = CL_NIL;
        CL_GC_PROTECT(bucket);
        for (i = 0; i < tbl_len; i++) {
            bucket = ((CL_Vector *)CL_OBJ_TO_PTR(
                          ((CL_Package *)CL_OBJ_TO_PTR(pkg_obj))->symbols))->data[i];
            while (!CL_NULL_P(bucket)) {
                result = cl_cons(cl_normalize_nil_symbol(cl_car(bucket)), result);
                bucket = cl_cdr(bucket);
            }
        }
        CL_GC_UNPROTECT(1);
    }
    CL_GC_UNPROTECT(2);
    return result;
}

/* (%package-external-symbols package) — list of exported symbols in package.
 * Source of truth is the package's own exported_symbols list (per-package,
 * not the global CL_SYM_EXPORTED flag). */
static CL_Obj bi_package_external_symbols(CL_Obj *args, int nargs)
{
    CL_Obj pkg_obj = coerce_to_package(args[0]);
    CL_Obj list;
    CL_Obj result = CL_NIL;
    (void)nargs;
    CL_GC_PROTECT(pkg_obj);
    cl_bindtab_materialize_all(pkg_obj);   /* lazy package: go eager (bindtab.c) */
    CL_GC_PROTECT(result);
    /* GC SAFETY: keeping the package ALIVE via pkg_obj does not FORWARD the
     * `list` C local — after a compaction inside cl_cons the cursor holds a
     * stale pre-move offset.  Root the cursor itself. */
    list = ((CL_Package *)CL_OBJ_TO_PTR(pkg_obj))->exported_symbols;
    CL_GC_PROTECT(list);
    while (!CL_NULL_P(list)) {
        result = cl_cons(cl_normalize_nil_symbol(cl_car(list)), result);
        list = cl_cdr(list);
    }
    CL_GC_UNPROTECT(3);
    return result;
}

/* ---- CDR-10: Package-local nicknames ---- */

/* (package-local-nicknames package) — returns alist of (nick-string . package) */
static CL_Obj bi_package_local_nicknames(CL_Obj *args, int nargs)
{
    CL_Obj pkg = coerce_to_package(args[0]);
    CL_Package *p = (CL_Package *)CL_OBJ_TO_PTR(pkg);
    (void)nargs;
    return p->local_nicknames;
}

/* (add-package-local-nickname nick-name target-package &optional package) */
static CL_Obj bi_add_package_local_nickname(CL_Obj *args, int nargs)
{
    const char *nick_name;
    uint32_t nick_len;
    CL_Obj nick_str, target_pkg, in_pkg;

    get_name_str(args[0], &nick_name, &nick_len);
    nick_str = cl_make_string(nick_name, nick_len);
    target_pkg = coerce_to_package(args[1]);
    in_pkg = (nargs > 2) ? coerce_to_package(args[2]) : cl_current_package;
    cl_add_package_local_nickname(nick_str, target_pkg, in_pkg);
    return SYM_T;
}

/* (remove-package-local-nickname nick-name &optional package) */
static CL_Obj bi_remove_package_local_nickname(CL_Obj *args, int nargs)
{
    const char *nick_name;
    uint32_t nick_len;
    CL_Obj from_pkg;

    get_name_str(args[0], &nick_name, &nick_len);
    from_pkg = (nargs > 1) ? coerce_to_package(args[1]) : cl_current_package;
    cl_remove_package_local_nickname(nick_name, nick_len, from_pkg);
    return SYM_T;
}

/* (package-used-by-list package) — list of packages that use this one */
static CL_Obj bi_package_used_by_list(CL_Obj *args, int nargs)
{
    CL_Obj target = coerce_to_package(args[0]);
    CL_Obj result = CL_NIL;
    CL_Obj reg = cl_package_registry;
    (void)nargs;

    /* GC SAFETY: cl_cons can compact — reg is re-read after it, and the
     * identity compare against target must see the forwarded offset. */
    CL_GC_PROTECT(result);
    CL_GC_PROTECT(reg);
    CL_GC_PROTECT(target);
    while (!CL_NULL_P(reg)) {
        CL_Obj pkg = cl_cdr(cl_car(reg));
        CL_Package *p = (CL_Package *)CL_OBJ_TO_PTR(pkg);
        CL_Obj uses = p->use_list;
        while (!CL_NULL_P(uses)) {
            if (cl_car(uses) == target) {
                result = cl_cons(pkg, result);
                break;
            }
            uses = cl_cdr(uses);
        }
        reg = cl_cdr(reg);
    }
    CL_GC_UNPROTECT(3);
    return result;
}

/* Helper: unintern a symbol from a package's own symbol table (no lock). */
static void unintern_own_nolock(CL_Obj sym, CL_Obj package)
{
    CL_Package *pkg = (CL_Package *)CL_OBJ_TO_PTR(package);
    CL_Vector *tbl = (CL_Vector *)CL_OBJ_TO_PTR(pkg->symbols);
    CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(sym);
    uint32_t idx = s->hash % tbl->length;
    CL_Obj prev = CL_NIL, list = tbl->data[idx];

    while (!CL_NULL_P(list)) {
        if (cl_car(list) == sym) {
            if (CL_NULL_P(prev))
                tbl->data[idx] = cl_cdr(list);
            else {
                CL_Cons *c = (CL_Cons *)CL_OBJ_TO_PTR(prev);
                c->cdr = cl_cdr(list);
            }
            pkg->sym_count--;
            if (s->package == package) {
                s->package = CL_NIL;
                s->flags &= ~CL_SYM_EXPORTED_HOME;
            }
            return;
        }
        prev = list;
        list = cl_cdr(list);
    }
}

/* Per CLHS, shadowing-import replaces conflicting symbols instead of
   signaling an error.  If sym is already present (same object), just
   ensure it is on the shadowing list.  If a *different* symbol with
   the same name is present, unintern the old one first, then import. */
static void shadowing_import_one(CL_Obj sym, CL_Obj package)
{
    CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(sym);
    CL_String *sname = (CL_String *)CL_OBJ_TO_PTR(s->name);
    CL_Package *pkg = (CL_Package *)CL_OBJ_TO_PTR(package);
    CL_Obj existing = cl_package_find_symbol_nolock(sname->data, sname->length, package);

    if (existing == sym) {
        /* Already present — just ensure on shadowing list */
    } else if (existing != CL_UNBOUND) {
        /* Different symbol with same name — remove old, import new */
        unintern_own_nolock(existing, package);
        /* Re-fetch pkg pointer (unintern doesn't allocate, but be safe) */
        pkg = (CL_Package *)CL_OBJ_TO_PTR(package);
        {
            CL_Vector *tbl = (CL_Vector *)CL_OBJ_TO_PTR(pkg->symbols);
            uint32_t idx = s->hash % tbl->length;
            CL_GC_PROTECT(package);
            CL_GC_PROTECT(sym);
            tbl->data[idx] = cl_cons(sym, tbl->data[idx]);
            CL_GC_UNPROTECT(2);
            pkg->sym_count++;
        }
    } else {
        /* Not present at all — plain import */
        CL_Vector *tbl = (CL_Vector *)CL_OBJ_TO_PTR(pkg->symbols);
        uint32_t idx = s->hash % tbl->length;
        CL_GC_PROTECT(package);
        CL_GC_PROTECT(sym);
        tbl->data[idx] = cl_cons(sym, tbl->data[idx]);
        CL_GC_UNPROTECT(2);
        pkg->sym_count++;
    }

    /* Add to shadowing symbols list if not already there */
    {
        CL_Obj sl = pkg->shadowing_symbols;
        while (!CL_NULL_P(sl)) {
            if (cl_car(sl) == sym) return;
            sl = cl_cdr(sl);
        }
        CL_GC_PROTECT(package);
        CL_GC_PROTECT(sym);
        pkg = (CL_Package *)CL_OBJ_TO_PTR(package);
        pkg->shadowing_symbols = cl_cons(sym, pkg->shadowing_symbols);
        CL_GC_UNPROTECT(2);
    }
}

/* (shadowing-import symbol &optional package) */
static CL_Obj bi_shadowing_import(CL_Obj *args, int nargs)
{
    CL_Obj symbols = args[0];
    CL_Obj pkg = (nargs > 1) ? coerce_to_package(args[1]) : cl_current_package;

    /* symbol-set mutation on a lazy package: go eager first (bindtab.c) —
     * pkg/symbols must be rooted first, since materialising a whole table
     * (and shadowing_import_one's own conses below) allocates and can
     * compact. */
    CL_GC_PROTECT(symbols);
    CL_GC_PROTECT(pkg);
    cl_bindtab_materialize_all(pkg);
    if (CL_SYMBOL_P(symbols)) {
        shadowing_import_one(symbols, pkg);
    } else {
        while (!CL_NULL_P(symbols)) {
            shadowing_import_one(cl_car(symbols), pkg);
            symbols = cl_cdr(symbols);
        }
    }
    CL_GC_UNPROTECT(2);
    return SYM_T;
}

/* (package-shadowing-symbols package) */
static CL_Obj bi_package_shadowing_symbols(CL_Obj *args, int nargs)
{
    CL_Obj pkg = coerce_to_package(args[0]);
    CL_Package *p = (CL_Package *)CL_OBJ_TO_PTR(pkg);
    (void)nargs;
    return p->shadowing_symbols;
}

/* (packagep object) => boolean */
static CL_Obj bi_packagep(CL_Obj *args, int nargs)
{
    (void)nargs;
    return CL_PACKAGE_P(args[0]) ? CL_T : CL_NIL;
}

/* (find-all-symbols name) => list
 * Searches every registered package for symbols with the given name. */
static CL_Obj bi_find_all_symbols(CL_Obj *args, int nargs)
{
    const char *name;
    uint32_t len;
    CL_Obj result = CL_NIL;
    CL_Obj reg;
    CL_Obj nsym;
    (void)nargs;

    get_name_str(args[0], &name, &len);

    CL_GC_PROTECT(result);
    reg = cl_package_registry;
    CL_GC_PROTECT(reg);
    while (!CL_NULL_P(reg)) {
        CL_Obj entry = cl_car(reg);
        CL_Obj pkg = cl_cdr(entry);
        CL_Obj sym = cl_package_find_symbol(name, len, pkg);
        if (!CL_NULL_P(sym)) {
            /* Check for duplicate (symbol may be accessible in multiple packages) */
            CL_Obj scan = result;
            int found = 0;
            while (!CL_NULL_P(scan)) {
                if (cl_car(scan) == sym) { found = 1; break; }
                scan = cl_cdr(scan);
            }
            if (!found) {
                nsym = cl_normalize_nil_symbol(sym);
                CL_GC_PROTECT(nsym);
                result = cl_cons(nsym, result);
                CL_GC_UNPROTECT(1);
            }
        }
        reg = cl_cdr(reg);
    }
    CL_GC_UNPROTECT(2); /* result, reg */
    return result;
}

/* (copy-symbol symbol &optional copy-properties) */
static CL_Obj bi_copy_symbol(CL_Obj *args, int nargs)
{
    CL_Obj sym_obj = args[0];
    int copy_props = (nargs > 1 && !CL_NULL_P(args[1]));
    CL_Obj new_sym;

    if (!CL_SYMBOL_OR_NIL_P(sym_obj))
        cl_signal_type_error(sym_obj, "SYMBOL", "COPY-SYMBOL");

    if (CL_NULL_P(sym_obj)) {
        /* (copy-symbol nil) — fresh uninterned symbol named "NIL".
         * With copy-props=T, CLHS says the new symbol's value, function,
         * and plist cells inherit from the original.  NIL is bound to
         * itself, so the new symbol's value cell becomes NIL too (so
         * boundp succeeds on the copy).  NIL has no function binding;
         * its plist comes from SYM_NIL's storage shadow. */
        new_sym = cl_make_uninterned_symbol(cl_make_string("NIL", 3));
        if (copy_props) {
            CL_Symbol *ns = (CL_Symbol *)CL_OBJ_TO_PTR(new_sym);
            CL_Symbol *src = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_NIL);
            ns->value = CL_NIL;
            ns->plist = src->plist;
        }
        return new_sym;
    }

    {
        CL_Symbol *orig = (CL_Symbol *)CL_OBJ_TO_PTR(sym_obj);
        CL_Symbol *ns;
        new_sym = cl_make_uninterned_symbol(orig->name);
        if (copy_props) {
            /* Re-fetch orig after potential GC compaction in cl_make */
            orig = (CL_Symbol *)CL_OBJ_TO_PTR(sym_obj);
            ns = (CL_Symbol *)CL_OBJ_TO_PTR(new_sym);
            ns->value = orig->value;
            /* A fresh uninterned copy: nothing can cache it yet. */
            ns->function = orig->function;   /* symfn-raw: fresh symbol */
            ns->plist = orig->plist;
        }
    }
    return new_sym;
}

/* ---- Init ---- */

void cl_builtins_package_init(void)
{
    /* Pre-intern keywords */
    KW_INTERNAL  = cl_intern_keyword("INTERNAL", 8);
    KW_EXTERNAL  = cl_intern_keyword("EXTERNAL", 8);
    KW_INHERITED = cl_intern_keyword("INHERITED", 9);
    KW_NICKNAMES       = cl_intern_keyword("NICKNAMES", 9);
    KW_USE             = cl_intern_keyword("USE", 3);
    KW_LOCAL_NICKNAMES = cl_intern_keyword("LOCAL-NICKNAMES", 15);

    defun("MAKE-PACKAGE", bi_make_package, 1, -1);
    defun("FIND-PACKAGE", bi_find_package, 1, 1);
    defun("DELETE-PACKAGE", bi_delete_package, 1, 1);
    defun("RENAME-PACKAGE", bi_rename_package, 2, 3);
    defun("EXPORT", bi_export, 1, 2);
    defun("UNEXPORT", bi_unexport, 1, 2);
    defun("IMPORT", bi_import, 1, 2);
    defun("USE-PACKAGE", bi_use_package, 1, 2);
    defun("UNUSE-PACKAGE", bi_unuse_package, 1, 2);
    defun("SHADOW", bi_shadow, 1, 2);
    defun("FIND-SYMBOL", bi_find_symbol, 1, 2);
    defun("INTERN", bi_intern, 1, 2);
    defun("UNINTERN", bi_unintern, 1, 2);
    defun("PACKAGE-NAME", bi_package_name, 1, 1);
    defun("PACKAGE-USE-LIST", bi_package_use_list, 1, 1);
    defun("PACKAGE-NICKNAMES", bi_package_nicknames, 1, 1);
    defun("LIST-ALL-PACKAGES", bi_list_all_packages, 0, 0);
    cl_register_builtin("%PACKAGE-SYMBOLS", bi_package_symbols, 1, 1, cl_package_clamiga);
    cl_register_builtin("%PACKAGE-EXTERNAL-SYMBOLS", bi_package_external_symbols, 1, 1, cl_package_clamiga);
    cl_register_builtin("%SET-CURRENT-PACKAGE", bi_pct_set_current_package, 1, 1, cl_package_clamiga);
    /* Package-local nicknames (CDR-10) live in CLAMIGA.
     * CL :uses CLAMIGA so internal code (boot.lisp, clos.lisp, tests)
     * sees them as bare symbols.  User packages that only (:use :common-lisp)
     * must add (:use :clamiga) or qualify with the CLAMIGA: prefix. */
    cl_register_builtin("PACKAGE-LOCAL-NICKNAMES", bi_package_local_nicknames, 1, 1, cl_package_clamiga);
    cl_register_builtin("ADD-PACKAGE-LOCAL-NICKNAME", bi_add_package_local_nickname, 2, 3, cl_package_clamiga);
    cl_register_builtin("REMOVE-PACKAGE-LOCAL-NICKNAME", bi_remove_package_local_nickname, 1, 2, cl_package_clamiga);
    defun("PACKAGE-USED-BY-LIST", bi_package_used_by_list, 1, 1);
    defun("SHADOWING-IMPORT", bi_shadowing_import, 1, 2);
    defun("PACKAGE-SHADOWING-SYMBOLS", bi_package_shadowing_symbols, 1, 1);
    defun("COPY-SYMBOL", bi_copy_symbol, 1, 2);
    defun("PACKAGEP", bi_packagep, 1, 1);
    defun("FIND-ALL-SYMBOLS", bi_find_all_symbols, 1, 1);

    /* Register cached symbols for GC compaction forwarding */
    cl_gc_register_root(&KW_INTERNAL);
    cl_gc_register_root(&KW_EXTERNAL);
    cl_gc_register_root(&KW_INHERITED);
    cl_gc_register_root(&KW_NICKNAMES);
    cl_gc_register_root(&KW_USE);
    cl_gc_register_root(&KW_LOCAL_NICKNAMES);
}
