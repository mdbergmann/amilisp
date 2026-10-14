/*
 * compiler_special.c — Control flow, loops, patterns
 *
 * Split from compiler.c. Contains: block, return-from, return,
 * tagbody, go, catch, unwind-protect, flet, labels,
 * dolist, dotimes, do, destructuring-bind.
 */

#include "compiler_internal.h"

/* --- Destructuring --- */

/* Emit bytecode that unconditionally signals an arity error for the
 * destructuring-bind guards: CLHS requires an error when the supplied list
 * does not match the structure of the lambda list (too few or too many
 * elements).  HELPER_SYM is a cached, GC-rooted AMILISP runtime builtin
 * (cl_dbind_too_few_sym / cl_dbind_too_many_sym) that does the signalling.
 * The call never returns, so bytecode following a guard runs only when the
 * guard passed. */
static void emit_dbind_error(CL_Compiler *c, CL_Obj helper_sym)
{
    /* Emit a 0-arg call to the cached helper symbol as raw bytecode.  This
     * allocates NOTHING (the symbol is pre-interned and GC-rooted, cl_emit/
     * cl_add_constant only touch the compiler's own buffers), so no GC
     * compaction can occur here — compile_destructure_pattern therefore needs
     * no GC root over its `pattern` cursor, and cannot leak one across a
     * compile-time cl_error longjmp.  The helper signals and never returns. */
    int idx = cl_add_constant(c, helper_sym);
    cl_emit(c, OP_FLOAD);
    cl_emit_u16(c, (uint16_t)idx);
    cl_emit_call(c, OP_CALL, 0);       /* 0 arguments */
    cl_emit(c, OP_POP);  /* helper never returns; keeps the stack balanced */
}

/* Recursive destructuring pattern walker.
 * pos_slot: local slot holding the current list position.
 * pattern: the destructuring lambda list (nested list).
 * Allocates local slots for each bound variable. */
static void compile_destructure_pattern(CL_Compiler *c, int pos_slot,
                                         CL_Obj pattern)
{
    CL_CompEnv *env = c->env;
    int saved_tail = c->in_tail;
    c->in_tail = 0;

    /* NB: this walker must remain allocation-free in the required-parameter
     * path.  The arity guards emit calls to pre-interned helper symbols
     * (emit_dbind_error allocates nothing), and the &optional/&key default
     * compilation already GC-protects its own `rest` cursor — so the `pattern`
     * cursor needs no GC root for the plain walk.  (An earlier version
     * GC-protected `pattern` for the whole function; that root leaked across
     * a compile-time cl_error longjmp during heavy cold loads, corrupting the
     * heap.)  The two nested-pattern recursion sites are the exception: they
     * DO allocate (nested defaults compile via compile_expr) and re-read
     * `pattern` afterwards, so each protects the cursor just across the
     * recursive call. */
    while (!CL_NULL_P(pattern)) {
        CL_Obj elem = cl_car(pattern);

        /* &whole var — bind var to the ENTIRE list being destructured and
         * consume no elements (CLHS 3.4.4/3.4.5; may only appear first, where
         * pos_slot still holds the full list).  Skipping this left the walker
         * to treat `&whole` and its var as ordinary required parameters,
         * mis-binding them and inflating the required-arity count — which the
         * arity guards then reported as a spurious "too few elements". */
        if (elem == SYM_AMP_WHOLE) {
            CL_Obj whole_var = cl_car(cl_cdr(pattern));
            if (CL_SYMBOL_P(whole_var)) {
                int slot = cl_env_add_local(env, whole_var);
                cl_emit(c, OP_LOAD);
                cl_emit(c, (uint8_t)pos_slot);
                cl_emit(c, OP_STORE);
                cl_emit(c, (uint8_t)slot);
                cl_emit(c, OP_POP);
            }
            pattern = cl_cdr(cl_cdr(pattern)); /* skip &whole + var */
            continue;
        }

        /* &aux var | (var [init]) ... — local let*-style bindings that take no
         * list element and impose no arity constraint (CLHS 3.4.1).  Terminal:
         * nothing list-consuming may follow.  init is compiled BEFORE the var
         * is added to the env so it cannot resolve to its own (yet-unset) slot,
         * while earlier aux vars / params remain visible. */
        if (elem == SYM_AMP_AUX) {
            CL_Obj rest = cl_cdr(pattern);
            CL_GC_PROTECT(rest);
            /* CONS_P (not !NULL_P): a dotted tail after &aux is malformed
             * per CLHS 3.4.5 (the dotted abbreviation stands in the &rest
             * position, which precedes &aux) — diagnose it clearly below
             * rather than faulting in cl_car. */
            while (CL_CONS_P(rest)) {
                CL_Obj spec = cl_car(rest);
                CL_Obj var = spec, init = CL_NIL;
                int slot;
                if (CL_CONS_P(spec)) {
                    var = cl_car(spec);
                    if (!CL_NULL_P(cl_cdr(spec)))
                        init = cl_car(cl_cdr(spec));
                }
                compile_expr(c, init);          /* value (var not yet in scope) */
                /* GC SAFETY: compile_expr(init) can compact — re-derive var
                 * from the protected cursor before registering it, else a
                 * stale symbol offset lands in env->locals. */
                spec = cl_car(rest);
                var = CL_CONS_P(spec) ? cl_car(spec) : spec;
                slot = cl_env_add_local(env, var);
                cl_emit(c, OP_STORE);
                cl_emit(c, (uint8_t)slot);
                cl_emit(c, OP_POP);
                rest = cl_cdr(rest);
            }
            if (!CL_NULL_P(rest))
                cl_error(CL_ERR_GENERAL,
                         "destructuring-bind: dotted lambda-list tail %s after &aux "
                         "(the dotted abbreviation stands in the &rest position, "
                         "which must precede &aux)",
                         CL_SYMBOL_P(rest) ? cl_symbol_name(rest) : "?");
            CL_GC_UNPROTECT(1);
            goto done; /* aux is terminal — no arity check */
        }

        /* &rest / &body — bind remaining list to next var.  The var may be a
         * nested destructuring pattern (e.g. macro lambda lists like alexandria
         * if-let's `&body (then &optional else)`); in that case stash the rest
         * list in a temp and recurse rather than binding a cons as a "local". */
        if (elem == SYM_AMP_REST || elem == SYM_AMP_BODY) {
            CL_Obj rest_var = cl_car(cl_cdr(pattern));
            if (CL_CONS_P(rest_var)) {
                int tmp = alloc_temp_slot(env);
                cl_emit(c, OP_LOAD);
                cl_emit(c, (uint8_t)pos_slot);
                cl_emit(c, OP_STORE);
                cl_emit(c, (uint8_t)tmp);
                cl_emit(c, OP_POP);
                /* GC SAFETY: the recursion can compile nested defaults
                 * (allocates, can compact) and `pattern` is re-read after. */
                CL_GC_PROTECT(pattern);
                compile_destructure_pattern(c, tmp, rest_var);
                CL_GC_UNPROTECT(1);
                pattern = cl_cdr(cl_cdr(pattern)); /* skip &rest pattern */
                if (!CL_NULL_P(pattern) && cl_car(pattern) == SYM_AMP_KEY)
                    continue;
                goto done;
            }
            {
            int slot = cl_env_add_local(env, rest_var);
            cl_emit(c, OP_LOAD);
            cl_emit(c, (uint8_t)pos_slot);
            cl_emit(c, OP_STORE);
            cl_emit(c, (uint8_t)slot);
            cl_emit(c, OP_POP);
            /* Skip to &key if present, otherwise done */
            pattern = cl_cdr(cl_cdr(pattern)); /* skip &rest var */
            if (!CL_NULL_P(pattern) && cl_car(pattern) == SYM_AMP_KEY)
                continue; /* let &key handler process it */
            goto done; /* &rest absorbs the remainder — no too-many check */
            }
        }

        /* &optional — remaining elements are optional with defaults */
        if (elem == SYM_AMP_OPTIONAL) {
            CL_Obj rest = cl_cdr(pattern);
            /* GC-protect cursor — compile_expr (default_val) can compact, making rest stale */
            CL_GC_PROTECT(rest);
            /* CONS_P (not !NULL_P): a dotted tail — (a &optional b . c),
             * the CLHS 3.4.5 abbreviation for a trailing &rest var — ends
             * the loop and is bound below instead of faulting in cl_car. */
            while (CL_CONS_P(rest)) {
                CL_Obj opt = cl_car(rest);
                CL_Obj var;
                CL_Obj default_val = CL_NIL;
                int slot, skip_pos;

                if (elem == SYM_AMP_KEY) {
                    /* &key after &optional: break to outer loop to process keys */
                    pattern = rest;  /* rest starts at &key */
                    CL_GC_UNPROTECT(1);
                    goto optional_done;
                }
                if (elem == SYM_AMP_AUX) {
                    /* &aux after &optional: hand back to the outer loop, which
                     * binds the aux vars and terminates (no arity check). */
                    pattern = rest;  /* rest starts at &aux */
                    CL_GC_UNPROTECT(1);
                    goto optional_done;
                }
                if (elem == SYM_AMP_REST || elem == SYM_AMP_BODY) {
                    /* &rest after &optional */
                    CL_Obj rest_var = cl_car(cl_cdr(rest));
                    CL_Obj after;
                    slot = cl_env_add_local(env, rest_var);
                    cl_emit(c, OP_LOAD);
                    cl_emit(c, (uint8_t)pos_slot);
                    cl_emit(c, OP_STORE);
                    cl_emit(c, (uint8_t)slot);
                    cl_emit(c, OP_POP);
                    CL_GC_UNPROTECT(1);
                    /* &key may follow &rest (e.g. (a &optional b &rest r &key k)).
                     * The plain &rest handler above checks for this; the one
                     * nested in &optional must too, or the &key vars are never
                     * bound ("Unbound variable: K").  Hand off to the outer loop
                     * with pattern pointing at &key, mirroring the &key-after-
                     * &optional case above. */
                    after = cl_cdr(cl_cdr(rest)); /* skip &rest, rest_var */
                    if (!CL_NULL_P(after) && cl_car(after) == SYM_AMP_KEY) {
                        pattern = after;
                        goto optional_done;
                    }
                    goto done;
                }

                CL_Obj supplied_p = CL_NIL;
                int supplied_slot = -1;

                if (CL_CONS_P(opt)) {
                    var = cl_car(opt);
                    if (!CL_NULL_P(cl_cdr(opt)))
                        default_val = cl_car(cl_cdr(opt));
                    if (!CL_NULL_P(cl_cdr(opt)) &&
                        !CL_NULL_P(cl_cdr(cl_cdr(opt))))
                        supplied_p = cl_car(cl_cdr(cl_cdr(opt)));
                } else {
                    var = opt;
                }

                slot = cl_env_add_local(env, var);
                if (!CL_NULL_P(supplied_p))
                    supplied_slot = cl_env_add_local(env, supplied_p);

                /* If pos is NIL, use default; else take (car pos) */
                cl_emit(c, OP_LOAD);
                cl_emit(c, (uint8_t)pos_slot);
                skip_pos = cl_emit_jump(c, OP_JNIL);

                /* pos not nil: var = (car pos), pos = (cdr pos), supplied-p = T */
                cl_emit(c, OP_LOAD);
                cl_emit(c, (uint8_t)pos_slot);
                cl_emit(c, OP_CAR);
                cl_emit(c, OP_STORE);
                cl_emit(c, (uint8_t)slot);
                cl_emit(c, OP_POP);
                cl_emit(c, OP_LOAD);
                cl_emit(c, (uint8_t)pos_slot);
                cl_emit(c, OP_CDR);
                cl_emit(c, OP_STORE);
                cl_emit(c, (uint8_t)pos_slot);
                cl_emit(c, OP_POP);
                if (supplied_slot >= 0) {
                    cl_emit(c, OP_T);
                    cl_emit(c, OP_STORE);
                    cl_emit(c, (uint8_t)supplied_slot);
                    cl_emit(c, OP_POP);
                }
                {
                    int end_pos = cl_emit_jump(c, OP_JMP);
                    cl_patch_jump(c, skip_pos);
                    /* pos is nil: use default, supplied-p = NIL */
                    compile_expr(c, default_val);
                    cl_emit(c, OP_STORE);
                    cl_emit(c, (uint8_t)slot);
                    cl_emit(c, OP_POP);
                    if (supplied_slot >= 0) {
                        cl_emit(c, OP_NIL);
                        cl_emit(c, OP_STORE);
                        cl_emit(c, (uint8_t)supplied_slot);
                        cl_emit(c, OP_POP);
                    }
                    cl_patch_jump(c, end_pos);
                }

                rest = cl_cdr(rest);
                elem = CL_CONS_P(rest) ? cl_car(rest) : CL_NIL;
            }
            if (!CL_NULL_P(rest)) {
                /* Dotted tail: bind it to the remaining list, like the
                 * required-path dotted handling below. */
                if (CL_SYMBOL_P(rest)) {
                    int slot = cl_env_add_local(env, rest);
                    cl_emit(c, OP_LOAD);
                    cl_emit(c, (uint8_t)pos_slot);
                    cl_emit(c, OP_STORE);
                    cl_emit(c, (uint8_t)slot);
                    cl_emit(c, OP_POP);
                }
                CL_GC_UNPROTECT(1);
                goto done; /* dotted tail absorbs the remainder — no too-many check */
            }
            CL_GC_UNPROTECT(1);
            /* Optionals exhausted with no following &rest/&key: per CLHS any
             * leftover element is a too-many arity error.  "Should signal"
             * (CLHS 1.4.2.3) — required only in safe code, so (safety 0)
             * elides the guard (spec 1.3 check elision). */
            if (c->optimize_settings.safety >= 1) {
                int ok_pos;
                cl_emit(c, OP_LOAD);
                cl_emit(c, (uint8_t)pos_slot);
                ok_pos = cl_emit_jump(c, OP_JNIL);
                emit_dbind_error(c, cl_dbind_too_many_sym);
                cl_patch_jump(c, ok_pos);
            }
            goto done;
        optional_done:
            /* Continue outer loop — pattern points to &key */
            continue;
        }

        /* &key — keyword destructuring (plist-based) */
        if (elem == SYM_AMP_KEY) {
            CL_Obj rest = cl_cdr(pattern);
            /* The section's keywords, collected for the unknown-keyword
             * check emitted after the loop (CLHS 3.4.1.4 via 3.4.4). */
            CL_Obj allowed = CL_NIL;
            int allow_other_keys = 0;
            int scan_slot = alloc_temp_slot(env);
            /* GC-protect both cursors — compile_expr (default_val), cl_cons
             * and cl_intern_keyword can compact, making them stale */
            CL_GC_PROTECT(rest);
            CL_GC_PROTECT(allowed);

            /* CONS_P (not !NULL_P): a dotted tail after &key is malformed
             * per CLHS 3.4.5 (the dotted abbreviation stands in the &rest
             * position, which precedes &key) — diagnose it clearly below
             * rather than faulting in cl_car. */
            while (CL_CONS_P(rest)) {
                CL_Obj spec = cl_car(rest);
                CL_Obj var, keyword_sym, default_val = CL_NIL;
                CL_Obj supplied_p = CL_NIL;
                int var_slot, supplied_slot = -1;
                int found_pos, not_found_pos, done_pos;
                const char *kw_name;

                /* &allow-other-keys: this section takes any keyword, so no
                 * unknown-keyword check is emitted for it. */
                if (CL_SYMBOL_P(spec) && spec == SYM_AMP_ALLOW_OTHER_KEYS) {
                    allow_other_keys = 1;
                    rest = cl_cdr(rest);
                    continue;
                }

                /* Parse key spec: var | (var default) | (var default supplied-p) |
                   ((keyword var) default [supplied-p]).
                   Two steps: first the keyword, then everything else. */
                if (CL_CONS_P(spec) && CL_CONS_P(cl_car(spec))) {
                    /* ((keyword var) default ...) */
                    keyword_sym = cl_car(cl_car(spec));
                } else {
                    /* keyword derived from var name */
                    kw_name = cl_symbol_name(CL_CONS_P(spec) ? cl_car(spec) : spec);
                    keyword_sym = cl_intern_keyword(kw_name,
                                     (uint32_t)strlen(kw_name));
                }
                allowed = cl_cons(keyword_sym, allowed);
                /* GC SAFETY: cl_intern_keyword may intern a NEW keyword and
                 * cl_cons allocates — either can compact.  Re-derive the
                 * keyword, spec and the rest of the parse from the two
                 * protected cursors before using any of them. */
                keyword_sym = cl_car(allowed);
                spec = cl_car(rest);
                if (CL_CONS_P(spec)) {
                    CL_Obj first = cl_car(spec);
                    var = CL_CONS_P(first) ? cl_car(cl_cdr(first)) : first;
                    if (!CL_NULL_P(cl_cdr(spec)))
                        default_val = cl_car(cl_cdr(spec));
                    if (!CL_NULL_P(cl_cdr(spec)) &&
                        !CL_NULL_P(cl_cdr(cl_cdr(spec))))
                        supplied_p = cl_car(cl_cdr(cl_cdr(spec)));
                } else {
                    var = spec;
                }

                var_slot = cl_env_add_local(env, var);
                if (!CL_NULL_P(supplied_p))
                    supplied_slot = cl_env_add_local(env, supplied_p);

                /* Emit bytecode to scan plist for keyword:
                 *   scan_slot = pos_slot (copy plist)
                 *   loop: if scan_slot is nil → not_found
                 *         if (car scan_slot) == keyword → found
                 *         scan_slot = (cddr scan_slot)
                 *         goto loop
                 * found: var = (cadr scan_slot), supplied-p = T
                 * not_found: var = default, supplied-p = NIL
                 */
                cl_emit(c, OP_LOAD);
                cl_emit(c, (uint8_t)pos_slot);
                cl_emit(c, OP_STORE);
                cl_emit(c, (uint8_t)scan_slot);
                cl_emit(c, OP_POP);

                /* loop: */
                {
                    int loop_start = c->code_pos;

                    /* if scan_slot is nil → not_found */
                    cl_emit(c, OP_LOAD);
                    cl_emit(c, (uint8_t)scan_slot);
                    not_found_pos = cl_emit_jump(c, OP_JNIL);

                    /* (car scan_slot) == keyword? */
                    cl_emit(c, OP_LOAD);
                    cl_emit(c, (uint8_t)scan_slot);
                    cl_emit(c, OP_CAR);
                    cl_emit_const(c, keyword_sym);
                    cl_emit(c, OP_EQ);
                    found_pos = cl_emit_jump(c, OP_JTRUE);

                    /* Not found: scan_slot = (cddr scan_slot) */
                    cl_emit(c, OP_LOAD);
                    cl_emit(c, (uint8_t)scan_slot);
                    cl_emit(c, OP_CDR);
                    /* Protect against odd-length plist */
                    {
                        int odd_pos = cl_emit_jump(c, OP_JNIL);
                        /* JNIL consumed TOS; reload (cdr scan_slot) */
                        cl_emit(c, OP_LOAD);
                        cl_emit(c, (uint8_t)scan_slot);
                        cl_emit(c, OP_CDR);
                        cl_emit(c, OP_CDR);
                        cl_emit(c, OP_STORE);
                        cl_emit(c, (uint8_t)scan_slot);
                        cl_emit(c, OP_POP);
                        cl_emit_loop_jump(c, OP_JMP, loop_start);
                        cl_patch_jump(c, odd_pos);
                        /* Odd-length: fall through to not_found */
                    }

                    /* not_found: var = default, supplied-p = NIL */
                    cl_patch_jump(c, not_found_pos);
                    compile_expr(c, default_val);
                    cl_emit(c, OP_STORE);
                    cl_emit(c, (uint8_t)var_slot);
                    cl_emit(c, OP_POP);
                    if (supplied_slot >= 0) {
                        cl_emit(c, OP_NIL);
                        cl_emit(c, OP_STORE);
                        cl_emit(c, (uint8_t)supplied_slot);
                        cl_emit(c, OP_POP);
                    }
                    done_pos = cl_emit_jump(c, OP_JMP);

                    /* found: var = (cadr scan_slot), supplied-p = T */
                    cl_patch_jump(c, found_pos);
                    cl_emit(c, OP_LOAD);
                    cl_emit(c, (uint8_t)scan_slot);
                    cl_emit(c, OP_CDR);
                    cl_emit(c, OP_CAR);
                    cl_emit(c, OP_STORE);
                    cl_emit(c, (uint8_t)var_slot);
                    cl_emit(c, OP_POP);
                    if (supplied_slot >= 0) {
                        cl_emit(c, OP_T);
                        cl_emit(c, OP_STORE);
                        cl_emit(c, (uint8_t)supplied_slot);
                        cl_emit(c, OP_POP);
                    }
                    cl_patch_jump(c, done_pos);
                }

                rest = cl_cdr(rest);
            }
            if (!CL_NULL_P(rest)) {
                /* Drop the roots before the longjmp: a GC root left behind
                 * by a compile-time cl_error corrupts the heap later (see
                 * the NB at the top of this function). */
                CL_GC_UNPROTECT(2);
                cl_error(CL_ERR_GENERAL,
                         "destructuring-bind: dotted lambda-list tail %s after &key "
                         "(the dotted abbreviation stands in the &rest position, "
                         "which must precede &key)",
                         CL_SYMBOL_P(rest) ? cl_symbol_name(rest) : "?");
            }

            /* Unknown-keyword check (CLHS 3.4.1.4 via 3.4.4): unless the
             * section says &allow-other-keys, call %DBIND-CHECK-KEYS with
             * the list and the section's keywords; it signals PROGRAM-ERROR
             * for a keyword not in that set (and honours a :allow-other-keys
             * t in the list itself).  "Should signal" (CLHS 1.4.2.3) — safe
             * code only, so (safety 0) elides it like the arity guards. */
            if (!allow_other_keys && c->optimize_settings.safety >= 1) {
                int idx;
                /* `allowed` was consed in reverse; put it back in lambda-list
                 * order so the error message lists keywords as written.
                 * In place, no allocation. */
                CL_Obj prev = CL_NIL, cur = allowed;
                while (CL_CONS_P(cur)) {
                    CL_Obj next = cl_cdr(cur);
                    ((CL_Cons *)CL_OBJ_TO_PTR(cur))->cdr = prev;
                    prev = cur;
                    cur = next;
                }
                allowed = prev;

                idx = cl_add_constant(c, cl_dbind_check_keys_sym);
                cl_emit(c, OP_FLOAD);
                cl_emit_u16(c, (uint16_t)idx);
                cl_emit(c, OP_LOAD);
                cl_emit(c, (uint8_t)pos_slot);
                cl_emit_const(c, allowed);
                cl_emit_call(c, OP_CALL, 2);
                cl_emit(c, OP_POP);
            }
            CL_GC_UNPROTECT(2);
            goto done;
        }

        /* Required parameter: the list must still have an element here.
         * (CLHS: too-few elements "should signal" — safe code only, so
         * (safety 0) elides the guard; see too-many note above.) */
        if (c->optimize_settings.safety >= 1) {
            int ok_pos;
            cl_emit(c, OP_LOAD);
            cl_emit(c, (uint8_t)pos_slot);
            ok_pos = cl_emit_jump(c, OP_JTRUE);  /* element present → bind */
            emit_dbind_error(c, cl_dbind_too_few_sym);
            cl_patch_jump(c, ok_pos);
        }

        if (CL_CONS_P(elem)) {
            /* Nested pattern: extract sublist, recurse.
             * GC SAFETY: the recursion is NOT allocation-free — a nested
             * &optional/&key/&aux default compiles via compile_expr and can
             * compact.  Protect the `pattern` cursor across it (locally, not
             * for the whole walk — see the NB at function top). */
            int sub_slot = alloc_temp_slot(env);
            cl_emit(c, OP_LOAD);
            cl_emit(c, (uint8_t)pos_slot);
            cl_emit(c, OP_CAR);
            cl_emit(c, OP_STORE);
            cl_emit(c, (uint8_t)sub_slot);
            cl_emit(c, OP_POP);
            CL_GC_PROTECT(pattern);
            compile_destructure_pattern(c, sub_slot, elem);
            CL_GC_UNPROTECT(1);
        } else if (CL_SYMBOL_P(elem)) {
            /* Simple variable: bind (car pos) */
            int slot = cl_env_add_local(env, elem);
            cl_emit(c, OP_LOAD);
            cl_emit(c, (uint8_t)pos_slot);
            cl_emit(c, OP_CAR);
            cl_emit(c, OP_STORE);
            cl_emit(c, (uint8_t)slot);
            cl_emit(c, OP_POP);
        }

        /* Advance pos = (cdr pos) */
        cl_emit(c, OP_LOAD);
        cl_emit(c, (uint8_t)pos_slot);
        cl_emit(c, OP_CDR);
        cl_emit(c, OP_STORE);
        cl_emit(c, (uint8_t)pos_slot);
        cl_emit(c, OP_POP);

        pattern = cl_cdr(pattern);

        /* Dotted pair tail: (a b . rest) — bind rest to remaining list */
        if (!CL_NULL_P(pattern) && !CL_CONS_P(pattern)) {
            if (CL_SYMBOL_P(pattern)) {
                int slot = cl_env_add_local(env, pattern);
                cl_emit(c, OP_LOAD);
                cl_emit(c, (uint8_t)pos_slot);
                cl_emit(c, OP_STORE);
                cl_emit(c, (uint8_t)slot);
                cl_emit(c, OP_POP);
            }
            goto done; /* dotted tail absorbs the remainder — no too-many check */
        }
    }

    /* Natural loop exit: a proper, required-only pattern (no &rest/&optional/
     * &key/&body and no dotted tail) was fully consumed.  Per CLHS the list
     * must now be exhausted; any leftover element is a too-many arity error.
     * The &-keyword and dotted paths jump straight to `done:`, skipping this.
     * Elided at (safety 0) — see the "should signal" notes above. */
    if (c->optimize_settings.safety >= 1) {
        int ok_pos;
        cl_emit(c, OP_LOAD);
        cl_emit(c, (uint8_t)pos_slot);
        ok_pos = cl_emit_jump(c, OP_JNIL);  /* list empty → ok */
        emit_dbind_error(c, cl_dbind_too_many_sym);
        cl_patch_jump(c, ok_pos);
    }
done:
    c->in_tail = saved_tail;
}

void compile_destructuring_bind(CL_Compiler *c, CL_Obj form)
{
    /* (destructuring-bind pattern expr body...) */
    CL_Obj pattern = cl_car(cl_cdr(form));
    CL_Obj expr = cl_car(cl_cdr(cl_cdr(form)));
    CL_Obj body = cl_cdr(cl_cdr(cl_cdr(form)));
    CL_Obj rest_body;
    CL_CompEnv *env = c->env;
    int saved_local_count = env->local_count;
    int saved_tail = c->in_tail;
    /* Scope this destructuring-bind's own body (declare (optimize ...)) —
     * saved/restored the same way compile_body does for LET/lambda bodies
     * (CLHS 3.3.4), but captured here so it is already in effect when
     * compile_destructure_pattern emits the too-few/too-many arity guards
     * below, not just for the body compiled afterward. */
    CL_OptimizeSettings saved_optimize = c->optimize_settings;
    int pos_slot;

    /* GC-protect pattern and body — compile_expr can compact, making them stale */
    CL_GC_PROTECT(pattern);
    CL_GC_PROTECT(body);

    /* Compile expression, store in temp slot */
    c->in_tail = 0;
    compile_expr(c, expr);
    pos_slot = alloc_temp_slot(env);
    cl_emit(c, OP_STORE);
    cl_emit(c, (uint8_t)pos_slot);
    cl_emit(c, OP_POP);

    /* Strip and apply the body's own leading declarations (including a
     * body-scoped (declare (optimize (safety 0)))) BEFORE walking the
     * pattern, so it elides THIS destructuring-bind's own arity guards —
     * not just an enclosing scope's declaration.  rest_body is a cursor
     * into the GC-protected `body` list; protect it separately since a
     * default-value form compiled below (compile_destructure_pattern) can
     * compact and leave a bare local cursor stale. */
    rest_body = process_body_declarations(c, body);
    CL_GC_PROTECT(rest_body);

    /* Walk pattern, binding variables */
    compile_destructure_pattern(c, pos_slot, pattern);

    /* Compile the remainder of the body (declarations already stripped) */
    c->in_tail = saved_tail;
    compile_body(c, rest_body);

    CL_GC_UNPROTECT(3);  /* rest_body, pattern, body */
    cl_env_clear_boxed(env, saved_local_count);

    /* Restore scope */
    env->local_count = saved_local_count;
    c->optimize_settings = saved_optimize;
}

/* --- Block / Return --- */

/* The NLX-detection scanners below decide whether a BLOCK/TAGBODY must be
 * compiled with a non-local-exit frame (so a RETURN-FROM/GO from inside a
 * nested closure can longjmp out) instead of a cheap local jump.  The
 * decision is made by scanning the *source* body for closure forms and
 * RETURN-FROM/GO references.
 *
 * Crucially, a user macro can hide a closure: `(in-lambda (go start))`
 * expands to `(funcall (lambda () (go start)))`.  Scanning only the
 * unexpanded source misses the lambda, the block/tagbody is wrongly
 * compiled with local jumps, and the GO/RETURN-FROM inside the expanded
 * closure can't reach its tag ("GO: no tag named ..."). This idiom — a
 * macro wrapping its body in a closure — is pervasive (e.g. SLY's
 * with-connection, without-sly-interrupts).  So the scanners macroexpand
 * macro calls before recursing, mirroring scan_body_for_boxing(). */

static int scan_nlx_macro_depth = 0;
#define SCAN_NLX_MACRO_MAX_DEPTH 50

/* Whether a form with this HEAD is a macro call the scan expands: a global
 * macro, or a local one — a MACROLET the compiler is inside of, or one the
 * scan itself walked into (scan_nlx_install_macrolet).  cl_macro_p knows
 * the global table only. */
static int scan_nlx_macro_call_p(CL_Obj head)
{
    if (!CL_SYMBOL_P(head)) return 0;
    if (cl_macro_p(head)) return 1;
    return cl_active_compiler && cl_active_compiler->env &&
           !CL_NULL_P(cl_env_lookup_local_macro(cl_active_compiler->env, head));
}

/* Install the expanders of a MACROLET's BINDINGS into ENV for the scan of
 * its body; the caller pops them again.  Compiling an expander runs the
 * compiler and the VM, so it is guarded like an expansion: a malformed
 * binding leaves the scan without these expanders (the macro's calls are
 * then walked as function calls, as before) instead of aborting it. */
static void scan_nlx_install_macrolet(CL_CompEnv *env, CL_Obj bindings)
{
    int saved_lmc = env->local_macro_count;
    int saved_sp, saved_fp, saved_dyn, saved_nlx, saved_handler,
        saved_restart, saved_debugger, saved_gc_roots;

    if (scan_nlx_macro_depth >= SCAN_NLX_MACRO_MAX_DEPTH) return;

    saved_sp = cl_vm.sp;
    saved_fp = cl_vm.fp;
    saved_dyn = cl_dyn_top;
    saved_nlx = cl_nlx_top;
    saved_handler = cl_handler_top;
    saved_restart = cl_restart_top;
    saved_debugger = cl_debugger_enabled;
    saved_gc_roots = gc_root_count;
    cl_debugger_enabled = 0;

    scan_nlx_macro_depth++;
    {
        int err; CL_CATCH(err);
        if (err == 0) {
            cl_macrolet_install_expanders(env, bindings);
            CL_UNCATCH();
        } else {
            CL_UNCATCH();
            cl_vm.sp = saved_sp;
            cl_vm.fp = saved_fp;
            cl_dynbind_restore_to(saved_dyn);
            cl_nlx_top = saved_nlx;
            gc_root_count = saved_gc_roots;
            env->local_macro_count = saved_lmc;
        }
    }
    scan_nlx_macro_depth--;
    cl_debugger_enabled = saved_debugger;
    cl_handler_top = saved_handler;
    cl_restart_top = saved_restart;
}

/* Macroexpand FORM once for NLX-detection scanning, with full VM/compiler
 * state save+restore so a side-effecting or failing expander cannot corrupt
 * compiler state.  Returns the expansion, or FORM unchanged when its head is
 * not a macro, expansion failed, or the macro-chain depth cap was hit.
 * Mirrors the macro-expansion guard in scan_body_for_boxing(). */
static CL_Obj scan_nlx_macroexpand_1(CL_Obj form)
{
    CL_Obj head, expanded;
    int saved_sp, saved_fp, saved_dyn, saved_nlx, saved_handler,
        saved_restart, saved_debugger, saved_gc_roots;

    if (!CL_CONS_P(form)) return form;
    head = cl_car(form);
    if (!scan_nlx_macro_call_p(head)) return form;
    if (scan_nlx_macro_depth >= SCAN_NLX_MACRO_MAX_DEPTH) return form;

    saved_sp = cl_vm.sp;
    saved_fp = cl_vm.fp;
    saved_dyn = cl_dyn_top;
    saved_nlx = cl_nlx_top;
    saved_handler = cl_handler_top;
    saved_restart = cl_restart_top;
    saved_debugger = cl_debugger_enabled;
    /* GC SAFETY: FORM is handed to the expander AFTER cl_build_lex_env, which
     * conses whenever the compiler env carries local macros (any macro call
     * inside a MACROLET body -- LOOP expands into one), and the error path
     * returns it.  Same hazard, same fix as in scan_body_for_boxing;
     * registered before the root count is saved so the error path's restore
     * keeps it. */
    CL_GC_PROTECT(form);
    saved_gc_roots = gc_root_count;
    cl_debugger_enabled = 0;  /* Suppress debugger during expansion */
    expanded = form;

    scan_nlx_macro_depth++;
    {
        int err; CL_CATCH(err);
        if (err == 0) {
            CL_Obj scan_env = CL_NIL;
            if (cl_active_compiler)
                scan_env = cl_build_lex_env(cl_active_compiler->env);
            expanded = cl_macroexpand_1_env(form, scan_env);
            CL_UNCATCH();
        } else {
            /* Expansion errored: restore state and treat form as opaque. */
            CL_UNCATCH();
            cl_vm.sp = saved_sp;
            cl_vm.fp = saved_fp;
            cl_dynbind_restore_to(saved_dyn);
            cl_nlx_top = saved_nlx;
            gc_root_count = saved_gc_roots;
            expanded = form;
        }
    }
    cl_debugger_enabled = saved_debugger;
    cl_handler_top = saved_handler;
    cl_restart_top = saved_restart;
    scan_nlx_macro_depth--;
    CL_GC_UNPROTECT(1); /* form */
    return expanded;
}

/* Recursion-depth cap for the NLX walker — guards against C-stack overflow
 * from pathologically nested forms (mirrors scan_body_for_boxing). */
static int scan_nlx_recurse_depth = 0;
/* > 0 while scanning inside the argument list of a call form — see the
 * RETURN-FROM / GO check at the top of nlx_scan.  Reset by the two scan
 * entry points so a scan abandoned by a longjmp cannot leave it raised. */
static int scan_nlx_arg_depth = 0;
extern CL_Obj bi_special_operator_p(CL_Obj *args, int n);   /* builtins_mutation.c */
#define SCAN_NLX_MAX_RECURSE_DEPTH 500
/* GC roots left free for what one more level takes: its own cursors, a
 * macro expansion, a MACROLET's expander compile. */
#define SCAN_NLX_ROOT_HEADROOM 128

/* NLX walker modes:
 *   NLX_ANY_CLOSURE — true if the body contains any closure form
 *                     (lambda/flet/labels/restart-case).  Used for TAGBODY
 *                     and the coarse anonymous-BLOCK check.
 *   NLX_FIND_RF     — true if (return-from <tag> ...) (or bare (return) when
 *                     anonymous) appears anywhere.  Used to test a closure's
 *                     subtree.
 *   NLX_BLOCK       — true if a closure or unwind-protect form contains a
 *                     matching return-from (named block), or any closure /
 *                     uwp-with-(return) exists (anonymous block).
 *   NLX_FUNUSE      — local-function inlining escape scan (see "Local-
 *                     function inlining" below).  Never returns 1; it
 *                     records, in the active compiler's funuse_* state,
 *                     which of the FLET/LABELS names under analysis must
 *                     keep a closure — #'NAME anywhere, (declare (notinline
 *                     NAME)), or a call from inside a closure — and counts
 *                     the direct call sites of the rest.  `anon` carries
 *                     "inside a closure" in this mode; `tag` is unused. */
enum { NLX_ANY_CLOSURE, NLX_FIND_RF, NLX_BLOCK, NLX_FUNUSE };

static int nlx_scan(CL_Obj form, int mode, CL_Obj tag, int anon);

/* NLX_FUNUSE: index of SYM among the names under analysis
 * (c->env->local_funs[c->funuse_lo ..]), or -1.  Reads the names through
 * the env entries, which the compiler's GC walkers forward, so a
 * compaction inside a macro expansion cannot stale the comparison. */
static int funuse_index(CL_Obj sym)
{
    CL_Compiler *c = cl_active_compiler;
    int i;
    if (!c || c->funuse_n == 0 || !CL_SYMBOL_P(sym) || !c->env) return -1;
    for (i = 0; i < c->funuse_n; i++)
        if (c->env->local_funs[c->funuse_lo + i].name == sym) return i;
    return -1;
}

static void funuse_escape(int fi)
{
    cl_active_compiler->funuse_escaped |= ((uint32_t)1 << fi);
}

/* Walk a list of body forms — all in code (evaluated) position. */
static int nlx_scan_body(CL_Obj body, int mode, CL_Obj tag, int anon)
{
    int r = 0;
    /* GC SAFETY: nlx_scan macroexpands body macros (e.g. INCF) in the VM and
     * thus allocates/compacts.  Protect the `body` cursor so its re-read
     * (cl_cdr) doesn't follow a stale offset after a compaction relocates the
     * list — an unprotected cursor here corrupted the source form being
     * compiled ("Unbound variable: NEW<n>" from a dropped LET* binding). */
    CL_GC_PROTECT(body);
    while (CL_CONS_P(body)) {
        if (nlx_scan(cl_car(body), mode, tag, anon)) { r = 1; break; }
        body = cl_cdr(body);
    }
    CL_GC_UNPROTECT(1);
    return r;
}

/* Walk a quasiquote template, descending only into unquoted (evaluated)
 * subforms — the rest is data.  Mirrors scan_qq_for_boxing(). */
static int nlx_scan_qq(CL_Obj tmpl, int mode, CL_Obj tag, int anon)
{
    if (CL_VECTOR_P(tmpl)) {
        CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(tmpl);
        uint32_t i, n = cl_vector_active_length(v);
        int r = 0;
        /* GC SAFETY: nlx_scan macroexpands via the VM (allocates/compacts) —
         * protect the vector and re-derive its raw data each iteration. */
        CL_GC_PROTECT(tmpl);
        for (i = 0; i < n; i++) {
            CL_Obj *data = cl_vector_data((CL_Vector *)CL_OBJ_TO_PTR(tmpl));
            if (nlx_scan_qq(data[i], mode, tag, anon)) { r = 1; break; }
        }
        CL_GC_UNPROTECT(1);
        return r;
    }
    if (!CL_CONS_P(tmpl)) return 0;
    if (cl_car(tmpl) == SYM_UNQUOTE || cl_car(tmpl) == SYM_UNQUOTE_SPLICING)
        return nlx_scan(cl_car(cl_cdr(tmpl)), mode, tag, anon);
    if (cl_car(tmpl) == SYM_QUASIQUOTE) return 0;  /* deeper nesting: data */
    {
        CL_Obj cur = tmpl;
        int r = 0;
        /* GC SAFETY: protect the walk cursor across the allocating recursion. */
        CL_GC_PROTECT(cur);
        while (CL_CONS_P(cur)) {
            if (nlx_scan_qq(cl_car(cur), mode, tag, anon)) { r = 1; break; }
            cur = cl_cdr(cur);
        }
        CL_GC_UNPROTECT(1);
        return r;
    }
}

/* Single-form NLX walker.  Sees through user macros (a macro can hide a
 * closure or a return-from), and — like scan_body_for_boxing — descends
 * only into CODE positions, skipping binding names, lambda lists,
 * destructuring patterns, case keys, declarations and quoted data so it
 * never macroexpands a symbol that merely names a macro in a non-code slot
 * (e.g. a destructuring-bind pattern produced by DEFMACRO). */
static int nlx_scan(CL_Obj form, int mode, CL_Obj tag, int anon)
{
    CL_Obj head, rest;
    int r = 0;

    /* Too deep to go on: the recursion cap, or the GC root stack nearly
     * full — every level holds roots across its recursive call, and a wide
     * AND/OR (one nesting level per argument once expanded) reaches the
     * ceiling, where cl_gc_push_root aborts the process.  What lies below
     * is unknown, so answer the safe way round: the exit needs the
     * non-local path, no candidate is inlined. */
    if (scan_nlx_recurse_depth >= SCAN_NLX_MAX_RECURSE_DEPTH ||
        gc_root_count >= CL_GC_ROOT_STACK_SIZE - SCAN_NLX_ROOT_HEADROOM) {
        if (!CL_CONS_P(form)) return 0;
        if (mode == NLX_FUNUSE) {
            if (cl_active_compiler)
                cl_active_compiler->funuse_escaped = ~(uint32_t)0;
            return 0;
        }
        return 1;
    }
    if (!CL_CONS_P(form)) {
        /* A bare symbol is a symbol-macro reference when one is bound: the
         * escape scan must see what it expands to (`(symbol-macrolet ((s
         * #'f)) ... s)` references F).  The other modes keep their existing
         * blind spot — the sole consequence there is a BLOCK compiled with
         * the cheap local exit, which the runtime still handles. */
        if (mode == NLX_FUNUSE && CL_SYMBOL_P(form) && !CL_NULL_P(form)) {
            CL_CompEnv *env = cl_active_compiler ? cl_active_compiler->env : NULL;
            CL_Obj exp = CL_NIL;
            if (env && cl_env_lookup_symbol_macro_p(env, form, &exp)) {
                scan_nlx_recurse_depth++;
                r = nlx_scan(exp, mode, tag, anon);
                scan_nlx_recurse_depth--;
                return r;
            }
        }
        return 0;
    }
    head = cl_car(form);
    rest = cl_cdr(form);

    /* Local-function inlining escape scan (NLX_FUNUSE). */
    if (mode == NLX_FUNUSE) {
        int fi;
        /* #'NAME — the closure escapes.  (function (lambda ...)) falls
         * through to the general walk, which scans the lambda as a closure. */
        if (head == SYM_FUNCTION && CL_CONS_P(rest) && CL_SYMBOL_P(cl_car(rest))) {
            fi = funuse_index(cl_car(rest));
            if (fi >= 0) funuse_escape(fi);
            return 0;
        }
        /* (declare ... (notinline NAME ...)) forbids inlining (CLHS 3.2.2.3);
         * no other declaration mentions a function in a way that matters. */
        if (head == SYM_DECLARE) {
            CL_Obj specs = rest;
            while (CL_CONS_P(specs)) {
                CL_Obj spec = cl_car(specs);
                if (CL_CONS_P(spec) && cl_car(spec) == SYM_NOTINLINE_DECL) {
                    CL_Obj fns = cl_cdr(spec);
                    while (CL_CONS_P(fns)) {
                        fi = funuse_index(cl_car(fns));
                        if (fi >= 0) funuse_escape(fi);
                        fns = cl_cdr(fns);
                    }
                }
                specs = cl_cdr(specs);
            }
            return 0;
        }
        /* (load-time-value form) inside a candidate's body: the function
         * must be compiled exactly once.  An inlined body is compiled at
         * every call site, and compile_load_time_value evaluates the form
         * (REPL/LOAD) or mints a memo cell (COMPILE-FILE) per compilation —
         * two call sites would see two different objects, which is exactly
         * what serapeum's STATIC-LET / STATIC-LOAD-TIME-VALUE build on not
         * happening.  The form itself runs in the null lexical environment,
         * so it cannot reference the candidates: nothing below to scan. */
        if (head == SYM_LOAD_TIME_VALUE) {
            CL_Compiler *c = cl_active_compiler;
            if (c->funuse_cur >= 0) funuse_escape(c->funuse_cur);
            return 0;
        }
        /* (NAME args...) — from inside a closure the call needs the closure
         * (the body is inlined only from the defining function's own code);
         * at the definition level it is a direct call site.  Either way the
         * arguments are code.  NAME shadows a global macro of the same
         * name, so this must come before the macro-expansion branch. */
        fi = funuse_index(head);
        if (fi >= 0) {
            CL_Compiler *c = cl_active_compiler;
            if (anon) funuse_escape(fi);
            else if (c->funuse_calls[fi] < 255) c->funuse_calls[fi]++;
            scan_nlx_recurse_depth++;
            nlx_scan_body(rest, mode, tag, anon);
            scan_nlx_recurse_depth--;
            return 0;
        }
        /* An inner FLET/LABELS: its function bodies are closures, its body
         * is not — unlike the closure-form rule below, which would treat
         * the whole form as a closure and refuse every helper called from
         * the body of a nested FLET. */
        if (head == SYM_FLET || head == SYM_LABELS) {
            CL_Obj defs, body;
            if (!CL_CONS_P(rest)) return 0;
            defs = cl_car(rest);
            body = cl_cdr(rest);
            scan_nlx_recurse_depth++;
            CL_GC_PROTECT(defs);
            CL_GC_PROTECT(body);
            while (CL_CONS_P(defs)) {
                CL_Obj def = cl_car(defs);
                if (CL_CONS_P(def) && CL_CONS_P(cl_cdr(def)))
                    nlx_scan_body(cl_cdr(cl_cdr(def)), mode, tag, 1);
                defs = cl_cdr(defs);
            }
            nlx_scan_body(body, mode, tag, anon);
            CL_GC_UNPROTECT(2);
            scan_nlx_recurse_depth--;
            return 0;
        }
    }

    /* RETURN-FROM / RETURN detection. */
    if (mode == NLX_FIND_RF) {
        if (head == SYM_RETURN_FROM && CL_CONS_P(rest) && cl_car(rest) == tag)
            return 1;
        if (anon && head == SYM_RETURN)
            return 1;
    }

    /* A RETURN-FROM (or GO) sitting inside the ARGUMENT LIST of a call —
     * (block b (list 1 (return-from b 2))), (tagbody a (f (go a))) — must
     * take the NLX path too.  The local-jump exit only stores the value and
     * jumps; the call's already-pushed arguments (the 1 above) stay on the
     * operand stack, so everything the enclosing form pushes afterwards
     * lands one slot too high (observed: FORMAT called with its arguments
     * shifted, a call landing on "Not a function: symbol T").  The NLX
     * landing restores the stack pointer from the frame.  Statement-level
     * special forms (IF/PROGN/LET bodies...) leave nothing pending and keep
     * the cheap local exit — see the arg-depth bookkeeping below. */
    if (scan_nlx_arg_depth > 0) {
        if (mode == NLX_BLOCK) {
            if (head == SYM_RETURN_FROM && CL_CONS_P(rest) && cl_car(rest) == tag)
                return 1;
            if (anon && head == SYM_RETURN)
                return 1;
        }
        if (mode == NLX_ANY_CLOSURE && head == SYM_GO)
            return 1;
    }

    /* Closure-creating forms.  Must appear as (OP ...) — a bare symbol
     * LAMBDA used as a variable does not create a closure. */
    if (head == SYM_LAMBDA || head == SYM_NAMED_LAMBDA
        || head == SYM_LABELS || head == SYM_FLET
        || head == SYM_RESTART_CASE) {
        if (mode == NLX_ANY_CLOSURE)
            return 1;
        if (mode == NLX_BLOCK) {
            /* Anonymous block promotes on any closure; a named block only
             * when a matching return-from crosses this closure boundary. */
            if (anon) return 1;
            return nlx_scan(form, NLX_FIND_RF, tag, anon);
        }
        /* NLX_FUNUSE: everything below this form runs inside a closure. */
        if (mode == NLX_FUNUSE && !anon)
            return nlx_scan(form, NLX_FUNUSE, tag, 1);
        /* NLX_FIND_RF: fall through to the structural handlers below, which
         * descend the closure body (skipping its lambda list). */
    }

    /* A return-from to our tag nested inside a dynamic-extent form —
     * unwind-protect, catch, or handler-bind — must force the BLOCK onto the
     * NLX path.  A cheap local OP_JMP would bypass the matching cleanup pop
     * (OP_UWPOP / OP_UNCATCH / OP_HANDLER_POP), leaving the catch / handler
     * frame stranded on its runtime stack.  This leaks one frame per *normal*
     * exit, so a hot path overflows: handler-case expands to
     *   (block B (catch 'T (return-from B (handler-bind ... form))))
     * and a local return-from B jumps over the OP_UNCATCH, leaking a CATCH
     * NLX frame on every call → eventual "NLX stack overflow" (seen reading
     * an SSL stream byte-by-byte via cl+ssl's handler-case in stream-read-byte).
     * The NLX BLOCK_RETURN path instead restores cl_nlx_top / cl_handler_top /
     * cl_restart_top from the block frame's saved marks, unwinding them. */
    if (mode == NLX_BLOCK &&
        (head == SYM_UNWIND_PROTECT || head == SYM_CATCH ||
         head == SYM_HANDLER_BIND ||
         (cl_handler_case_sym != CL_NIL && head == cl_handler_case_sym)))
        return nlx_scan(form, NLX_FIND_RF, tag, anon);

    scan_nlx_recurse_depth++;

    /* (quote ...) / (declare ...) / (defmacro ...) — no evaluated code. */
    if (head == SYM_QUOTE || head == SYM_DECLARE || head == SYM_DEFMACRO)
        goto done;

    /* (quasiquote tmpl) — scan only unquoted subforms. */
    if (head == SYM_QUASIQUOTE) {
        if (CL_CONS_P(rest)) r = nlx_scan_qq(cl_car(rest), mode, tag, anon);
        goto done;
    }

    /* (setq/setf place value ...) — scan value forms (places aren't code
     * that creates closures, and expanding a setf-place macro would be a
     * spurious side effect).
     * GC SAFETY (all handlers below): the recursive nlx_scan/nlx_scan_body
     * calls macroexpand in the VM and can compact — every cursor or local
     * re-read after such a call must be GC-protected (same as the
     * handler-bind case further down). */
    if (head == SYM_SETQ || head == SYM_SETF) {
        CL_Obj p = rest;
        CL_GC_PROTECT(p);
        while (CL_CONS_P(p) && CL_CONS_P(cl_cdr(p))) {
            if (nlx_scan(cl_car(cl_cdr(p)), mode, tag, anon)) { r = 1; break; }
            p = cl_cdr(cl_cdr(p));
        }
        CL_GC_UNPROTECT(1);
        goto done;
    }

    /* (lambda params body...) — skip params. */
    if (head == SYM_LAMBDA) {
        if (CL_CONS_P(rest)) r = nlx_scan_body(cl_cdr(rest), mode, tag, anon);
        goto done;
    }
    /* (defun name params body...) — skip name + params.
     * (named-lambda name params body...) has the same shape. */
    if (head == SYM_DEFUN || head == SYM_NAMED_LAMBDA) {
        if (CL_CONS_P(rest) && CL_CONS_P(cl_cdr(rest)))
            r = nlx_scan_body(cl_cdr(cl_cdr(rest)), mode, tag, anon);
        goto done;
    }
    /* (flet/labels ((name params body...)...) body...) — skip names/params. */
    if (head == SYM_FLET || head == SYM_LABELS) {
        CL_Obj defs, body;
        if (!CL_CONS_P(rest)) goto done;
        defs = cl_car(rest);
        body = cl_cdr(rest);
        CL_GC_PROTECT(defs);
        CL_GC_PROTECT(body);
        while (CL_CONS_P(defs)) {
            CL_Obj def = cl_car(defs);
            if (CL_CONS_P(def) && CL_CONS_P(cl_cdr(def)))
                if (nlx_scan_body(cl_cdr(cl_cdr(def)), mode, tag, anon)) { r = 1; break; }
            defs = cl_cdr(defs);
        }
        if (!r) r = nlx_scan_body(body, mode, tag, anon);
        CL_GC_UNPROTECT(2);
        goto done;
    }
    /* (let/let* ((var value)...) body...) — scan values + body, skip names. */
    if (head == SYM_LET || head == SYM_LETSTAR) {
        CL_Obj bindings, body;
        int idx = 0;
        if (!CL_CONS_P(rest)) goto done;
        bindings = cl_car(rest);
        body = cl_cdr(rest);
        CL_GC_PROTECT(bindings);
        CL_GC_PROTECT(body);
        while (CL_CONS_P(bindings)) {
            CL_Obj clause = cl_car(bindings);
            if (CL_CONS_P(clause) && CL_CONS_P(cl_cdr(clause))) {
                /* A parallel LET evaluates every init form before it binds
                 * any variable (compile_let pushes them all, then stores),
                 * so an init after the first runs with the earlier values
                 * pending on the operand stack — a local exit from there
                 * would leave them behind.  LET* binds as it goes. */
                int saved_depth = scan_nlx_arg_depth;
                if (head == SYM_LET && idx > 0) scan_nlx_arg_depth++;
                r = nlx_scan(cl_car(cl_cdr(clause)), mode, tag, anon);
                scan_nlx_arg_depth = saved_depth;
                if (r) break;
            }
            idx++;
            bindings = cl_cdr(bindings);
        }
        if (!r) r = nlx_scan_body(body, mode, tag, anon);
        CL_GC_UNPROTECT(2);
        goto done;
    }
    /* (destructuring-bind pattern value body...) — skip pattern. */
    if (head == SYM_DESTRUCTURING_BIND) {
        if (!CL_CONS_P(rest) || !CL_CONS_P(cl_cdr(rest))) goto done;
        CL_GC_PROTECT(rest);
        if (nlx_scan(cl_car(cl_cdr(rest)), mode, tag, anon)) r = 1;
        else r = nlx_scan_body(cl_cdr(cl_cdr(rest)), mode, tag, anon);
        CL_GC_UNPROTECT(1);
        goto done;
    }
    /* (multiple-value-bind (vars...) value body...) — skip var list. */
    if (head == SYM_MULTIPLE_VALUE_BIND) {
        if (!CL_CONS_P(rest) || !CL_CONS_P(cl_cdr(rest))) goto done;
        CL_GC_PROTECT(rest);
        if (nlx_scan(cl_car(cl_cdr(rest)), mode, tag, anon)) r = 1;
        else r = nlx_scan_body(cl_cdr(cl_cdr(rest)), mode, tag, anon);
        CL_GC_UNPROTECT(1);
        goto done;
    }
    /* (do/do* ((var init [step])...) (end result...) body...) — skip names. */
    if (head == SYM_DO || head == SYM_DO_STAR) {
        CL_Obj clauses, endc, body;
        if (!CL_CONS_P(rest)) goto done;
        clauses = cl_car(rest);
        if (!CL_CONS_P(cl_cdr(rest))) goto done;
        endc = cl_car(cl_cdr(rest));
        body = cl_cdr(cl_cdr(rest));
        CL_GC_PROTECT(clauses);
        CL_GC_PROTECT(endc);
        CL_GC_PROTECT(body);
        {
        int idx = 0;
        while (CL_CONS_P(clauses)) {
            CL_Obj clause = cl_car(clauses);
            if (CL_CONS_P(clause) && CL_CONS_P(cl_cdr(clause))) {
                /* clause is re-read after the first nlx_scan below, which can
                 * compact — keep a protected cursor for it too.  A parallel
                 * DO evaluates all inits (and all steps) before assigning,
                 * like LET: forms after the first see pending temporaries. */
                int saved_depth = scan_nlx_arg_depth;
                CL_GC_PROTECT(clause);
                if (head == SYM_DO && idx > 0) scan_nlx_arg_depth++;
                if (nlx_scan(cl_car(cl_cdr(clause)), mode, tag, anon)) r = 1;
                else if (CL_CONS_P(cl_cdr(cl_cdr(clause))) &&
                         nlx_scan(cl_car(cl_cdr(cl_cdr(clause))), mode, tag, anon))
                    r = 1;
                scan_nlx_arg_depth = saved_depth;
                CL_GC_UNPROTECT(1);
                if (r) break;
            }
            idx++;
            clauses = cl_cdr(clauses);
        }
        }
        if (!r && CL_CONS_P(endc) && nlx_scan_body(endc, mode, tag, anon)) r = 1;
        if (!r) r = nlx_scan_body(body, mode, tag, anon);
        CL_GC_UNPROTECT(3);
        goto done;
    }
    /* (case/typecase keyform (key body...)...) — skip keys. */
    if (head == SYM_CASE || head == SYM_ECASE ||
        head == SYM_TYPECASE || head == SYM_ETYPECASE) {
        CL_Obj clauses;
        if (!CL_CONS_P(rest)) goto done;
        CL_GC_PROTECT(rest);
        if (nlx_scan(cl_car(rest), mode, tag, anon)) {
            r = 1;
            CL_GC_UNPROTECT(1);
            goto done;
        }
        clauses = cl_cdr(rest);
        CL_GC_UNPROTECT(1);
        CL_GC_PROTECT(clauses);
        while (CL_CONS_P(clauses)) {
            CL_Obj clause = cl_car(clauses);
            if (CL_CONS_P(clause) && nlx_scan_body(cl_cdr(clause), mode, tag, anon)) { r = 1; break; }
            clauses = cl_cdr(clauses);
        }
        CL_GC_UNPROTECT(1);
        goto done;
    }
    /* (handler-bind ((type handler-form)...) body...) — the clause CAR is a
     * condition TYPE specifier, not evaluated code.  Scan only the handler
     * EXPRESSION (cadr) of each clause and the body, skipping the type spec.
     * Without this the general fall-through expands (type handler-form) as a
     * macro call when the condition type is also a macro (e.g. serapeum's
     * DISPATCH-CASE-ERROR, a `(&key type datum)` macro that is also the
     * condition class), signaling "odd number of keyword arguments" out of
     * the speculative expansion — the nlx_scan twin of the scan_body_for_boxing
     * handler-bind guard.  HANDLER-CASE is a real macro that expands to
     * handler-bind + typecase, both covered (typecase keys skipped above). */
    if (head == SYM_HANDLER_BIND) {
        CL_Obj clauses, body;
        if (!CL_CONS_P(rest)) goto done;
        clauses = cl_car(rest);
        body = cl_cdr(rest);
        /* Protect both the clause cursor and body: the recursive nlx_scan below
         * macroexpands handler expressions and can compact, staling bare offset
         * locals.  `body` (= cl_cdr(rest)) must be pre-computed and protected
         * here — after the while loop `rest` itself is stale, so re-deriving
         * cl_cdr(rest) at line 850 would walk garbage. */
        CL_GC_PROTECT(clauses);
        CL_GC_PROTECT(body);
        while (CL_CONS_P(clauses)) {
            CL_Obj clause = cl_car(clauses);
            if (CL_CONS_P(clause) && CL_CONS_P(cl_cdr(clause)))
                if (nlx_scan(cl_car(cl_cdr(clause)), mode, tag, anon)) { r = 1; break; }
            clauses = cl_cdr(clauses);
        }
        CL_GC_UNPROTECT(2);
        if (!r) r = nlx_scan_body(body, mode, tag, anon);
        goto done;
    }

    /* (amilisp::%handler-case form (type ([var]) . body)...) — the clause
     * CAR is a type spec and the CADR a variable list, not code: scan the
     * form and each clause BODY only (same reasoning as handler-bind). */
    if (cl_handler_case_sym != CL_NIL && head == cl_handler_case_sym) {
        CL_Obj clauses;
        if (!CL_CONS_P(rest)) goto done;
        clauses = cl_cdr(rest);
        CL_GC_PROTECT(clauses);
        r = nlx_scan(cl_car(rest), mode, tag, anon);
        while (!r && CL_CONS_P(clauses)) {
            CL_Obj clause = cl_car(clauses);
            if (CL_CONS_P(clause) && CL_CONS_P(cl_cdr(clause)))
                r = nlx_scan_body(cl_cdr(cl_cdr(clause)), mode, tag, anon);
            clauses = cl_cdr(clauses);
        }
        CL_GC_UNPROTECT(1);
        goto done;
    }

    /* (macrolet (bindings) body...) — the bindings are templates, not code;
     * scan the body with their expanders installed, so a call of a local
     * macro is expanded like a global one (the macro branch below).  What
     * a local macro hides is as real as what a global one hides: a call of
     * an inlining candidate from inside a closure (flexi-streams' decoders
     * lost eight READ-SEQUENCE* methods to the inlining guard over it), a
     * RETURN-FROM crossing a lambda.  Mirrors scan_body_for_boxing(). */
    if (head == SYM_MACROLET) {
        CL_CompEnv *env = cl_active_compiler ? cl_active_compiler->env : NULL;
        int saved_lmc = env ? env->local_macro_count : 0;
        CL_Obj body;
        if (!CL_CONS_P(rest)) goto done;
        body = cl_cdr(rest);
        CL_GC_PROTECT(body);
        if (env) scan_nlx_install_macrolet(env, cl_car(rest));
        r = nlx_scan_body(body, mode, tag, anon);
        CL_GC_UNPROTECT(1);
        /* compile_macrolet installs them again for the real compile. */
        if (env) env->local_macro_count = saved_lmc;
        goto done;
    }
    /* (symbol-macrolet ((sym expansion)...) body...) — skip bindings, but
     * register them into the active compiler env while scanning the body.
     * scan_nlx_macroexpand_1 builds each expanded macro's &environment from
     * cl_build_lex_env(cl_active_compiler->env); without the symbol-macros in
     * scope an &environment-aware expander (e.g. ironclad's
     * GET-KECCAK-ROTATE-OFFSET, which (eval (trivial-macroexpand-all x env)) on
     * an x bound by an enclosing symbol-macrolet) sees x unbound and signals
     * "Unbound variable: X" mid-scan — a failure that an outer ASDF
     * handler/unwind-protect hijacks before scan_nlx_macroexpand_1's CL_CATCH
     * can contain it.  Registering the bindings makes the expansion fold to its
     * constant and succeed.  Mirrors the symbol-macrolet case in
     * scan_body_for_boxing(). */
    if (head == SYM_SYMBOL_MACROLET) {
        CL_CompEnv *env = cl_active_compiler ? cl_active_compiler->env : NULL;
        int saved_smc = env ? env->symbol_macro_count : 0;
        if (!CL_CONS_P(rest)) goto done;
        if (env) {
            CL_Obj b = cl_car(rest);
            while (CL_CONS_P(b)) {
                CL_Obj binding = cl_car(b);
                if (CL_CONS_P(binding) && CL_CONS_P(cl_cdr(binding)))
                    cl_env_add_symbol_macro(env, cl_car(binding),
                                            cl_car(cl_cdr(binding)));
                b = cl_cdr(b);
            }
        }
        r = nlx_scan_body(cl_cdr(rest), mode, tag, anon);
        if (env) env->symbol_macro_count = saved_smc;
        goto done;
    }

    /* Macro call: expand and scan the expansion.  THIS is what lets us see
     * a closure or return-from hidden behind a user macro.
     * GC SAFETY: scan_nlx_macroexpand_1 runs the macro in the VM and so
     * allocates/compacts.  Protect `form` across it — otherwise the caller's
     * source list (e.g. a TAGBODY/DOTIMES body sharing this cons) is left
     * with a stale reference that, after the freed slot is reused by the
     * expansion's own conses, reads back a different form ("Unbound variable:
     * NEW<n>" from a dropped INCF/DECF LET* binding). */
    if (scan_nlx_macro_call_p(head)) {
        CL_Obj expanded;
        CL_GC_PROTECT(form);
        expanded = scan_nlx_macroexpand_1(form);
        if (expanded != form) {
            CL_GC_PROTECT(expanded);
            r = nlx_scan(expanded, mode, tag, anon);
            CL_GC_UNPROTECT(1);
            CL_GC_UNPROTECT(1);
            goto done;
        }
        CL_GC_UNPROTECT(1);
    }

    /* General form (function call / other special form): every element is
     * evaluated code.  (Includes ((lambda ...) args) immediate calls — the
     * head cons is scanned too.)
     * GC SAFETY: protect `cur` — nlx_scan recursion can macroexpand and
     * compact, and the cursor is re-read (cl_cdr) afterward.
     *
     * A function call (or MULTIPLE-VALUE-CALL, PROGV and THROW, whose first
     * subform is a pending temporary just like an argument) evaluates its subforms with
     * the earlier ones already on the operand stack: bump the arg depth so
     * a RETURN-FROM / GO found below is routed to the NLX path (see the
     * check at the top).  A special operator that fell through to here
     * (IF, PROGN, BLOCK, TAGBODY, THE, LOCALLY, ...) evaluates its subforms
     * at statement level and leaves the depth alone. */
    {
        CL_Obj cur = form;
        int saved_arg_depth = scan_nlx_arg_depth;
        int is_call = 1;
        if (CL_SYMBOL_P(head) && head != SYM_MULTIPLE_VALUE_CALL &&
            head != SYM_PROGV && head != SYM_THROW) {
            CL_Obj sop_args[1];
            sop_args[0] = head;
            if (bi_special_operator_p(sop_args, 1) != CL_NIL)
                is_call = 0;
        }
        if (is_call)
            scan_nlx_arg_depth++;
        CL_GC_PROTECT(cur);
        while (CL_CONS_P(cur)) {
            if (nlx_scan(cl_car(cur), mode, tag, anon)) { r = 1; break; }
            cur = cl_cdr(cur);
        }
        CL_GC_UNPROTECT(1);
        scan_nlx_arg_depth = saved_arg_depth;
    }

done:
    scan_nlx_recurse_depth--;
    return r;
}

/* Check if BODY contains any closure-creating form (lambda/labels/flet/
 * restart-case), including ones produced by macro expansion.  Used to decide
 * if a TAGBODY needs NLX for cross-closure go support. */
static int tree_has_closure_forms(CL_Obj tree)
{
    scan_nlx_arg_depth = 0;
    return nlx_scan_body(tree, NLX_ANY_CLOSURE, CL_NIL, 0);
}

/* Decide whether a BLOCK must use the NLX path.  A named block needs NLX
 * only when a matching (return-from <tag> ...) crosses a closure boundary or
 * sits inside an unwind-protect; an anonymous block (NIL tag) promotes on
 * any closure too, since a macro-hidden (return ...) cannot be matched
 * against a specific tag.  All recursion sees through macro expansions. */
static int tree_needs_nlx_block(CL_Obj body, CL_Obj tag)
{
    scan_nlx_arg_depth = 0;
    return nlx_scan_body(body, NLX_BLOCK, tag, CL_NULL_P(tag));
}

/* Strip leading declarations from body, then delegate to the
 * trampoline-aware compile_progn_tail (which pushes a PROGN_ITER frame
 * when the body has multiple forms so iteration stays off the C stack).
 * Body must be GC-protected by caller. */
static CL_Obj compile_nontail_body(CL_Compiler *c, CL_Obj body)
{
    CL_Obj rest = process_body_declarations(c, body);
    return compile_progn_tail(c, rest);
}

CL_Obj compile_block(CL_Compiler *c, CL_Obj form)
{
    CL_Obj tag = cl_car(cl_cdr(form));
    CL_Obj body = cl_cdr(cl_cdr(form));
    int saved_block_count = c->block_count;
    int needs_nlx;
    CL_BlockInfo *bi;
    CL_TailFrame *tf;

    /* Hard cap: writing to c->blocks[CL_MAX_BLOCKS] would clobber the
     * adjacent block_count field (silent stack-corruption that surfaces
     * later as a SIGBUS in compile_return_from when iterating
     * c->blocks[].tag).  Raise a clean compile error instead.  Checked
     * before the GC_PROTECTs below so the error path leaks no roots. */
    if (c->block_count >= CL_MAX_BLOCKS) {
        cl_error(CL_ERR_OVERFLOW,
                 "BLOCK nesting depth exceeded (%d): "
                 "consider raising CL_MAX_BLOCKS", CL_MAX_BLOCKS);
    }

    /* GC SAFETY: tree_needs_nlx_block → nlx_scan_body MACROEXPANDS the body
     * forms (to see through macros like a (push ...) hiding a return-from),
     * which allocates and can trigger a compacting GC.  `body` and `tag` are
     * unprotected C locals derived from `form`; without protecting them ACROSS
     * the scan they hold stale arena offsets afterward, and compile_nontail_body
     * would walk relocated/garbage cons cells — dropping the real body (e.g. a
     * LOOP expansion's (let* ...)) and returning a freed-then-reused cell as the
     * "tail", surfacing as "Unbound variable: ITEM<n>/ACC<n>" under GC stress.
     * Same class as the compile_tagbody/tree_has_closure_forms fix.  Protect
     * BEFORE the scan, not after. */
    CL_GC_PROTECT(body);
    CL_GC_PROTECT(tag);
    needs_nlx = tree_needs_nlx_block(body, tag);

    /* Push block info (for compile-time lookup by return-from) */
    bi = &c->blocks[c->block_count++];
    bi->tag = tag;
    bi->n_patches = 0;
    bi->uses_nlx = needs_nlx;
    bi->dyn_depth = c->special_depth;
    bi->mv_state = CL_MV_ONE;   /* identity; merged with each exit */
    bi->exit_stale_mask = 0;

    /* CRITICAL: push the BLOCK frame BEFORE compile_nontail_body so the
     * BLOCK postlude drains AFTER any PROGN_ITER frames the body
     * iteration pushes.  Otherwise BLOCK_LOCAL would emit its closing
     * STORE/POP/LOAD before non-tail body forms had been compiled. */
    if (needs_nlx) {
        int tag_idx, block_push_pos;
        int saved_tail = c->in_tail;

        bi->result_slot = -1;

        tag_idx = cl_add_constant(c, tag);
        cl_emit(c, OP_BLOCK_PUSH);
        cl_emit_u16(c, (uint16_t)tag_idx);
        block_push_pos = c->code_pos;
        cl_emit_i32(c, 0); /* placeholder offset to landing */

        /* Disable tail calls inside NLX blocks — the body must
         * return to OP_BLOCK_POP so the NLX frame is properly popped.
         * Without this, tail calls leak NLX BLOCK frames. */
        c->in_tail = 0;

        tf = cl_tail_push(c);
        tf->kind = CL_TAIL_BLOCK_NLX;
        tf->saved_block_count = saved_block_count;
        tf->saved_tail = saved_tail;
        tf->block_push_pos = block_push_pos;

        {
            CL_Obj tail = compile_nontail_body(c, body);
            CL_GC_UNPROTECT(2);  /* tag, body */
            return tail;  /* CL_NIL means caller emits OP_NIL for empty body */
        }
    } else {
        CL_CompEnv *env = c->env;
        int saved_local_count = env->local_count;
        int result_slot;

        result_slot = env->local_count;
        env->locals[result_slot] = CL_NIL;  /* Clear stale binding */
        env->local_count++;
        if (env->local_count > env->max_locals)
            env->max_locals = env->local_count;
        bi->result_slot = result_slot;

        tf = cl_tail_push(c);
        tf->kind = CL_TAIL_BLOCK_LOCAL;
        tf->saved_block_count = saved_block_count;
        tf->saved_local_count = saved_local_count;
        tf->result_slot = result_slot;

        {
            CL_Obj tail = compile_nontail_body(c, body);
            CL_GC_UNPROTECT(2);  /* tag, body */
            return tail;
        }
    }
}

/* Helper: trampoline prelude common to RETURN-FROM and RETURN.
 * `tag` is the block tag (possibly NIL for RETURN), `val_form` is the value
 * (CL_NIL means "no value form supplied — emit OP_NIL inline"), and
 * `has_value` distinguishes (return-from X) from (return-from X NIL).
 * Returns the value form for the trampoline, or CL_NIL when the caller
 * should emit OP_NIL (then drain). */
static CL_Obj return_from_prelude(CL_Compiler *c, CL_Obj tag,
                                  CL_Obj val_form, int has_value)
{
    int saved_tail = c->in_tail;
    int i, tag_idx;
    CL_TailFrame *tf;

    for (i = c->block_count - 1; i >= 0; i--) {
        if (i < c->hide_block_hi && i >= c->hide_block_lo) {
            /* Blocks opened between a local function's definition and the
             * call site where its body is being inlined — invisible to it
             * (compile_local_inline_call). */
            i = c->hide_block_lo;
            continue;
        }
        if (c->blocks[i].tag == tag) {
            CL_BlockInfo *bi = &c->blocks[i];
            c->in_tail = 0;
            tf = cl_tail_push(c);
            if (bi->uses_nlx) {
                tag_idx = cl_add_constant(c, tag);
                tf->kind = CL_TAIL_RETURN_FROM_NLX;
                tf->saved_tail = saved_tail;
                tf->tag_idx = tag_idx;
            } else {
                tf->kind = CL_TAIL_RETURN_FROM_LOCAL;
                tf->saved_tail = saved_tail;
                tf->bi_index = i;
                tf->unwind_count = c->special_depth - bi->dyn_depth;
            }
            return has_value ? val_form : CL_NIL;
        }
    }

    for (i = 0; i < c->outer_block_count; i++) {
        if (c->outer_blocks[i] == tag) {
            c->in_tail = 0;
            tag_idx = cl_add_constant(c, tag);
            tf = cl_tail_push(c);
            tf->kind = CL_TAIL_OUTER_RETURN_FROM;
            tf->saved_tail = saved_tail;
            tf->tag_idx = tag_idx;
            return has_value ? val_form : CL_NIL;
        }
    }

    cl_error(CL_ERR_GENERAL, "RETURN-FROM: no block named %s",
             CL_NULL_P(tag) ? "NIL" : cl_symbol_name(tag));
    return CL_NIL;
}

CL_Obj compile_return_from(CL_Compiler *c, CL_Obj form)
{
    CL_Obj tag = cl_car(cl_cdr(form));
    CL_Obj rest_after_tag = cl_cdr(cl_cdr(form));
    int has_value = !CL_NULL_P(rest_after_tag);
    CL_Obj val_form = has_value ? cl_car(rest_after_tag) : CL_NIL;
    return return_from_prelude(c, tag, val_form, has_value);
}

CL_Obj compile_return(CL_Compiler *c, CL_Obj form)
{
    /* (return [value]) => return-from NIL */
    CL_Obj rest = cl_cdr(form);
    int has_value = !CL_NULL_P(rest);
    CL_Obj val_form = has_value ? cl_car(rest) : CL_NIL;
    return return_from_prelude(c, CL_NIL, val_form, has_value);
}

/* Emit deferred postlude for a block / return-from tail frame.  Called
 * from compile_expr's drain loop in LIFO order. */
void emit_block_or_return_postlude(CL_Compiler *c, CL_TailFrame *tf)
{
    switch ((CL_TailKind)tf->kind) {
    case CL_TAIL_BLOCK_NLX: {
        int jmp_pos;
        cl_emit(c, OP_BLOCK_POP);
        jmp_pos = cl_emit_jump(c, OP_JMP);
        /* Landing: longjmp arrives here with result on stack */
        cl_patch_jump(c, tf->block_push_pos);
        /* End: both paths converge with result on TOS */
        cl_patch_jump(c, jmp_pos);
        c->in_tail = tf->saved_tail;
        c->block_count = tf->saved_block_count;
        break;
    }
    case CL_TAIL_BLOCK_LOCAL: {
        CL_BlockInfo *bi = &c->blocks[tf->saved_block_count];  /* this block's slot */
        cl_emit(c, OP_STORE);
        cl_emit(c, (uint8_t)tf->result_slot);
        cl_emit(c, OP_POP);
        /* The STORE/JMP/LOAD round trip runs no Lisp, so the MV buffer still
         * describes whichever arm produced the result — the fall-through
         * (c->mv_state) or a local RETURN-FROM (merged into bi->mv_state).
         * Patch the exits here, giving the stale ones their own reset when
         * another arm can deliver several values. */
        cl_mv_close_patch_join(c, bi->exit_patches, bi->n_patches,
                               bi->exit_stale_mask,
                               cl_mv_merge(c->mv_state, bi->mv_state));
        cl_emit(c, OP_LOAD);
        cl_emit(c, (uint8_t)tf->result_slot);
        cl_env_clear_boxed(c->env, tf->saved_local_count);
        c->env->local_count = tf->saved_local_count;
        c->block_count = tf->saved_block_count;
        break;
    }
    case CL_TAIL_RETURN_FROM_LOCAL: {
        CL_BlockInfo *bi = &c->blocks[tf->bi_index];
        c->in_tail = tf->saved_tail;
        /* Record this exit's MV state for the block's join (see above). */
        bi->mv_state = cl_mv_merge(bi->mv_state, c->mv_state);
        if (tf->unwind_count > 0) {
            cl_emit(c, OP_DYNUNBIND);
            cl_emit(c, (uint8_t)tf->unwind_count);
        }
        cl_emit(c, OP_STORE);
        cl_emit(c, (uint8_t)bi->result_slot);
        cl_emit(c, OP_POP);
        if (bi->n_patches < CL_MAX_BLOCK_PATCHES) {
            if (c->mv_state == CL_MV_STALE)
                bi->exit_stale_mask |= (uint32_t)1 << bi->n_patches;
            bi->exit_patches[bi->n_patches++] = cl_emit_jump(c, OP_JMP);
        }
        break;
    }
    case CL_TAIL_RETURN_FROM_NLX:
    case CL_TAIL_OUTER_RETURN_FROM:
        c->in_tail = tf->saved_tail;
        /* OP_BLOCK_RETURN snapshots the MV state into the NLX frame and the
         * landing pad restores it, so it has to describe the value form. */
        cl_mv_normalize(c);
        cl_emit(c, OP_BLOCK_RETURN);
        cl_emit_u16(c, (uint16_t)tf->tag_idx);
        break;
    default:
        break;  /* not a block/return frame */
    }
}

/* --- Tagbody / Go --- */

static int is_tagbody_tag(CL_Obj form)
{
    /* Tags are symbols or integers (CL spec) */
    return CL_SYMBOL_P(form) || CL_FIXNUM_P(form);
}

void compile_tagbody(CL_Compiler *c, CL_Obj form)
{
    CL_Obj body = cl_cdr(form);
    int saved_tagbody_count = c->tagbody_count;
    int saved_tail = c->in_tail;
    CL_TagbodyInfo *tb;
    CL_Obj cursor;
    int i;
    int needs_nlx;

    /* Hard cap: writing past tagbodies[] would clobber adjacent fields
     * (silent corruption that surfaces later as wild memory access).
     * Raise a clean compile error instead.  Checked before the GC_PROTECTs
     * below so the error-unwind path has no root to leak. */
    if (c->tagbody_count >= CL_MAX_TAGBODIES) {
        cl_error(CL_ERR_OVERFLOW,
                 "TAGBODY nesting depth exceeded (%d): "
                 "consider raising CL_MAX_TAGBODIES", CL_MAX_TAGBODIES);
    }

    /* GC SAFETY: protect `body` BEFORE tree_has_closure_forms — that scan
     * macroexpands body macros (e.g. INCF) in the VM, allocating/compacting.
     * `body` is reachable via `form`, but `form` is a by-value copy of the
     * caller's expr whose own protection can lapse during the scan; without an
     * explicit root here the body's cons cells can be relocated and a stale
     * reference left behind, corrupting the form to be compiled (observed as
     * a dropped INCF/DECF LET* binding → "Unbound variable: NEW<n>"). */
    CL_GC_PROTECT(body);
    needs_nlx = tree_has_closure_forms(body);

    c->in_tail = 0;

    /* GC SAFETY: `cursor`/`item` index into the tagbody source list and are
     * re-read after each `compile_expr` (which allocates and can trigger
     * compaction).  The tagbody-info struct fields (id, tag symbols) are
     * GC-traced, but these raw iteration cursors are not — without protection
     * a compaction leaves `cursor` holding a stale arena offset, so
     * `cl_cdr(cursor)` re-reads the body and re-emits it (the body runs twice
     * per loop iteration).  `body` is already protected above (before the
     * tree_has_closure_forms scan); protect `cursor` here too. */
    cursor = body;
    CL_GC_PROTECT(cursor);

    /* Push tagbody info.
     * GC SAFETY: the slot becomes visible to the compiler GC mark/update
     * walkers the moment tagbody_count is bumped, but slots are reused
     * (count is saved/restored per tagbody) — `id` still holds the previous
     * occupant's dead cons offset.  The cl_cons below allocates and can
     * compact; the walkers would then trace/forward that dangling offset
     * (observed as SIGBUS in cl_alloc while compiling nested dotimes).
     * Initialize every traced field BEFORE the first allocation. */
    tb = &c->tagbodies[c->tagbody_count++];
    tb->n_tags = 0;
    tb->uses_nlx = needs_nlx;
    tb->id = CL_NIL;

    /* Create unique tagbody identifier for NLX */
    if (needs_nlx)
        tb->id = cl_cons(CL_NIL, CL_NIL);  /* fresh cons cell as unique id */

    /* First pass: collect all tags */
    cursor = body;
    while (!CL_NULL_P(cursor)) {
        CL_Obj item = cl_car(cursor);
        if (is_tagbody_tag(item)) {
            CL_TagInfo *ti;
            if (tb->n_tags >= CL_MAX_TAGBODY_TAGS)
                cl_error(CL_ERR_OVERFLOW,
                         "TAGBODY has too many tags (max %d): "
                         "consider raising CL_MAX_TAGBODY_TAGS",
                         CL_MAX_TAGBODY_TAGS);
            ti = &tb->tags[tb->n_tags++];
            ti->tag = item;
            ti->code_pos = -1;
            ti->n_forward = 0;
        }
        cursor = cl_cdr(cursor);
    }

    if (needs_nlx) {
        /* NLX path: set up NLX frame, compile body, then dispatch landing */
        int id_idx, push_pos, jmp_pos;

        id_idx = cl_add_constant(c, tb->id);
        cl_emit(c, OP_TAGBODY_PUSH);
        cl_emit_u16(c, (uint16_t)id_idx);
        push_pos = c->code_pos;
        cl_emit_i32(c, 0);  /* placeholder offset to landing */

        /* Compile body (same as non-NLX) */
        cursor = body;
        while (!CL_NULL_P(cursor)) {
            CL_Obj item = cl_car(cursor);
            if (is_tagbody_tag(item)) {
                for (i = 0; i < tb->n_tags; i++) {
                    if (tb->tags[i].tag == item) {
                        int j;
                        tb->tags[i].code_pos = c->code_pos;
                        for (j = 0; j < tb->tags[i].n_forward; j++)
                            cl_patch_jump(c, tb->tags[i].forward_patches[j]);
                        tb->tags[i].n_forward = 0;
                        break;
                    }
                }
            } else {
                compile_expr(c, item);
                cl_emit(c, OP_POP);
            }
            cursor = cl_cdr(cursor);
        }

        /* Normal exit: pop NLX frame, jump past dispatch */
        cl_emit(c, OP_TAGBODY_POP);
        jmp_pos = cl_emit_jump(c, OP_JMP);

        /* Landing: longjmp from cross-closure GO arrives here.
         * TOS = tag index (fixnum). Dispatch to the right tag position.
         * Each JTRUE lands in a per-tag shim that pops the tag index
         * before jumping to the tag's body, so the body resumes with the
         * stack at the same height as it was at OP_TAGBODY_PUSH (matching
         * the fall-through / local-go entry conditions). */
        {
            int shim_jumps[CL_MAX_TAGBODY_TAGS];
            int default_jmp;

            cl_patch_jump(c, push_pos);

            for (i = 0; i < tb->n_tags; i++) {
                if (tb->tags[i].code_pos >= 0) {
                    cl_emit(c, OP_DUP);
                    cl_emit_const(c, CL_MAKE_FIXNUM(i));
                    cl_emit(c, OP_EQ);
                    shim_jumps[i] = cl_emit_jump(c, OP_JTRUE);
                } else {
                    shim_jumps[i] = -1;
                }
            }
            /* Default fallthrough: pop tag index, jump past shims. */
            cl_emit(c, OP_POP);
            default_jmp = cl_emit_jump(c, OP_JMP);

            /* Per-tag shims: pop tag index then jump to tag body. */
            for (i = 0; i < tb->n_tags; i++) {
                if (shim_jumps[i] >= 0) {
                    cl_patch_jump(c, shim_jumps[i]);
                    cl_emit(c, OP_POP);
                    cl_emit_loop_jump(c, OP_JMP, tb->tags[i].code_pos);
                }
            }

            cl_patch_jump(c, default_jmp);
        }

        /* Past dispatch */
        cl_patch_jump(c, jmp_pos);

        /* tagbody returns NIL */
        cl_emit(c, OP_NIL);
        c->mv_state = CL_MV_ONE;   /* statements' MV states don't survive */
    } else {
        /* Local path (non-NLX): efficient local jumps */
        cursor = body;
        while (!CL_NULL_P(cursor)) {
            CL_Obj item = cl_car(cursor);
            if (is_tagbody_tag(item)) {
                for (i = 0; i < tb->n_tags; i++) {
                    if (tb->tags[i].tag == item) {
                        int j;
                        tb->tags[i].code_pos = c->code_pos;
                        for (j = 0; j < tb->tags[i].n_forward; j++)
                            cl_patch_jump(c, tb->tags[i].forward_patches[j]);
                        tb->tags[i].n_forward = 0;
                        break;
                    }
                }
            } else {
                compile_expr(c, item);
                cl_emit(c, OP_POP);
            }
            cursor = cl_cdr(cursor);
        }

        /* tagbody returns NIL */
        cl_emit(c, OP_NIL);
        c->mv_state = CL_MV_ONE;   /* statements' MV states don't survive */
    }

    /* Restore */
    c->tagbody_count = saved_tagbody_count;
    c->in_tail = saved_tail;

    CL_GC_UNPROTECT(2);
}

void compile_go(CL_Compiler *c, CL_Obj form)
{
    CL_Obj tag = cl_car(cl_cdr(form));
    int i, j;

    /* Search tagbodies innermost-first for matching tag */
    for (i = c->tagbody_count - 1; i >= 0; i--) {
        CL_TagbodyInfo *tb = &c->tagbodies[i];
        if (i < c->hide_tagbody_hi && i >= c->hide_tagbody_lo) {
            /* Hidden from an inlined local function's body — see the
             * matching skip in return_from_prelude. */
            i = c->hide_tagbody_lo;
            continue;
        }
        for (j = 0; j < tb->n_tags; j++) {
            if (tb->tags[j].tag == tag) {
                CL_TagInfo *ti = &tb->tags[j];
                if (tb->uses_nlx) {
                    /* NLX-based tagbody: emit OP_TAGBODY_GO */
                    int id_idx = cl_add_constant(c, tb->id);
                    cl_emit_const(c, CL_MAKE_FIXNUM(j));  /* tag index */
                    cl_emit(c, OP_TAGBODY_GO);
                    cl_emit_u16(c, (uint16_t)id_idx);
                } else if (ti->code_pos >= 0) {
                    /* Backward jump to known position */
                    cl_emit_loop_jump(c, OP_JMP, ti->code_pos);
                } else {
                    /* Forward jump — patch later */
                    if (ti->n_forward < CL_MAX_BLOCK_PATCHES)
                        ti->forward_patches[ti->n_forward++] = cl_emit_jump(c, OP_JMP);
                }
                return;
            }
        }
    }

    /* Check outer tagbody tags (from enclosing scopes, for cross-closure go) */
    for (i = 0; i < c->outer_tag_count; i++) {
        if (c->outer_tags[i].tag == tag) {
            int id_idx = cl_add_constant(c, c->outer_tags[i].tagbody_id);
            cl_emit_const(c, CL_MAKE_FIXNUM(c->outer_tags[i].tag_index));
            cl_emit(c, OP_TAGBODY_GO);
            cl_emit_u16(c, (uint16_t)id_idx);
            return;
        }
    }

    cl_error(CL_ERR_GENERAL, "GO: no tag named %s",
             CL_SYMBOL_P(tag) ? cl_symbol_name(tag) : "?");
}

/* --- Catch --- */

void compile_catch(CL_Compiler *c, CL_Obj form)
{
    /* (catch tag body...) */
    CL_Obj tag_form = cl_car(cl_cdr(form));
    CL_Obj body;
    int saved_tail = c->in_tail;
    int catch_pos, jmp_pos;

    c->in_tail = 0;

    /* GC SAFETY: compile_expr(tag_form) allocates and can compact; protect
     * `form` and re-derive `body` afterwards (same shape as compile_if). */
    CL_GC_PROTECT(form);

    /* Compile tag expression (push tag value on stack) */
    compile_expr(c, tag_form);

    body = cl_cdr(cl_cdr(form));
    CL_GC_UNPROTECT(1);
    CL_GC_PROTECT(body);

    /* OP_CATCH offset_to_landing: setjmp; on throw jumps to landing */
    cl_emit(c, OP_CATCH);
    catch_pos = c->code_pos;
    cl_emit_i32(c, 0); /* placeholder */

    /* Compile body (progn).  Keep in_tail = 0: the body must fall through to
     * the OP_UNCATCH below so the catch (NLX) frame is popped on normal exit.
     * Restoring saved_tail here would let the body's tail form emit OP_TAILCALL,
     * replacing the frame and skipping OP_UNCATCH — one leaked NLX frame per
     * call until the frame stack overflows (same class as the handler-bind /
     * NLX-block tail leak). */
    if (CL_NULL_P(body))
        cl_emit(c, OP_NIL);
    else
        compile_progn(c, body);

    CL_GC_UNPROTECT(1);  /* body */

    /* OP_UNCATCH: pop catch frame (normal exit) */
    cl_emit(c, OP_UNCATCH);

    /* The throw landing arrives with the MV state restored from the NLX
     * frame, so that arm describes the thrown values.  Normalize the
     * normal-exit arm here — before the JMP, so only that arm pays — and
     * both paths reach the join self-describing. */
    cl_mv_normalize(c);

    /* JMP past the throw landing */
    jmp_pos = cl_emit_jump(c, OP_JMP);

    /* [landing]: throw arrives here with result on stack */
    cl_patch_jump(c, catch_pos);

    /* [past_landing]: both paths converge, result is on stack */
    cl_patch_jump(c, jmp_pos);
    /* The throw arm can deliver several values (the landing restores them
     * from the NLX frame), and the normal arm was normalized above, so the
     * join is self-describing and possibly multi-valued. */
    c->mv_state = CL_MV_MANY;

    c->in_tail = saved_tail;  /* restore for the caller's continuation */
}

/* --- %handler-case (the HANDLER-CASE special form) --- */

#define CL_HC_MAX_CLAUSES 64   /* = CL_MAX_HANDLER_BINDINGS: one binding each */

/* The K-th clause of (%handler-case form clause...), re-derived from the
 * (GC-protected) form so a compaction between two reads cannot stale it. */
static CL_Obj hc_nth_clause(CL_Obj form, int k)
{
    CL_Obj cl = cl_cdr(cl_cdr(form));
    while (k-- > 0) cl = cl_cdr(cl);
    return cl_car(cl);
}

void compile_handler_case(CL_Compiler *c, CL_Obj form)
{
    /* (amilisp::%handler-case form (typespec ([var]) declaration* form*)...)
     *
     * What boot.lisp's HANDLER-CASE macro expands to, after rewriting a
     * :no-error clause away (CLHS 9.2.23's own expansion).  It used to expand
     * to (block (catch (return-from (handler-bind ((type (lambda (c) ...)))
     * form)))) + typecase: a cons, a closure and three NLX frames on EVERY
     * entry, and the clause type tested twice.  Emitted shape:
     *
     *   HANDLER_CASE_PUSH <types> landing   ; 1 NLX frame + N bindings
     *     <form>                            ; in_tail = 0
     *   HANDLER_CASE_POP                    ; normal exit: values of form
     *   JMP end
     * landing:                              ; entered with [condition]
     *   JMP clause_0 ; JMP clause_1 ; ...   ; 5 bytes each: the transfer
     * clause_k:                             ;   resumes at landing + 5k
     *   STORE g; POP   (var clause)         ; or just POP
     *   <(let ((var g)) . body)>            ; LET handles declarations
     *   JMP end
     * end:
     *
     * A clause body is compiled as a LET so `(declare ...)` forms and a
     * special VAR work exactly as in any binding form; the condition is
     * handed to it through a gensym-named local slot. */
    CL_Obj types = CL_NIL;
    int n = 0, k, types_idx;
    int saved_tail = c->in_tail;
    int push_pos, end_chain = CL_JUMP_CHAIN_END;
    int table_pos[CL_HC_MAX_CLAUSES];

    CL_GC_PROTECT(form);
    c->in_tail = 0;

    {
        CL_Obj cl = cl_cdr(cl_cdr(form));
        while (CL_CONS_P(cl)) { n++; cl = cl_cdr(cl); }
        if (n > CL_HC_MAX_CLAUSES)
            cl_error(CL_ERR_OVERFLOW,
                     "HANDLER-CASE: too many clauses (%d, max %d)",
                     n, CL_HC_MAX_CLAUSES);
    }
    for (k = 0; k < n; k++) {
        CL_Obj cl = hc_nth_clause(form, k);
        if (!CL_CONS_P(cl) || !CL_CONS_P(cl_cdr(cl)) ||
            !(CL_NULL_P(cl_car(cl_cdr(cl))) || CL_CONS_P(cl_car(cl_cdr(cl)))))
            cl_error(CL_ERR_GENERAL,
                     "HANDLER-CASE: malformed clause %d - expected "
                     "(typespec ([var]) form*)", k);
    }

    /* The clause TYPE list, in clause order, as one constant.  Built back to
     * front; cl_cons forwards its own operands and `types` is rooted. */
    CL_GC_PROTECT(types);
    for (k = n - 1; k >= 0; k--)
        types = cl_cons(cl_car(hc_nth_clause(form, k)), types);
    types_idx = cl_add_constant(c, types);
    CL_GC_UNPROTECT(1);   /* types */

    cl_emit(c, OP_HANDLER_CASE_PUSH);
    cl_emit_u16(c, (uint16_t)types_idx);
    push_pos = c->code_pos;
    cl_emit_i32(c, 0);   /* landing, patched below */

    /* The protected form.  Keep in_tail = 0: it must fall through to the
     * POP so the frame and bindings are unwound on normal exit (a tail
     * call would replace the frame and leak them — see compile_catch). */
    compile_expr(c, cl_car(cl_cdr(form)));
    cl_emit(c, OP_HANDLER_CASE_POP);
    /* Normal arm: the form's own values.  Normalize a stale state here,
     * before the jump, so only this arm pays and the join is
     * self-describing (as compile_catch does). */
    cl_mv_normalize(c);
    end_chain = cl_emit_jump_chain(c, OP_JMP, end_chain);

    /* landing: the per-clause jump table (must stay exactly 5 bytes per
     * entry — OP_JMP + i32 — since the VM lands at landing + 5 * k). */
    cl_patch_jump(c, push_pos);
    for (k = 0; k < n; k++)
        table_pos[k] = cl_emit_jump(c, OP_JMP);

    for (k = 0; k < n; k++) {
        CL_Obj cl = hc_nth_clause(form, k);
        CL_Obj var_list = cl_car(cl_cdr(cl));
        CL_Obj let_form;
        CL_CompEnv *env = c->env;
        int saved_local_count = env->local_count;

        cl_patch_jump(c, table_pos[k]);
        /* stack: [condition] */
        if (CL_CONS_P(var_list)) {
            CL_Obj g = cl_gensym_with_name("HC");
            CL_Obj binding, bindings;
            int slot;
            CL_GC_PROTECT(g);
            slot = cl_env_add_local(env, g);
            cl_emit(c, OP_STORE);
            cl_emit(c, (uint8_t)slot);
            cl_emit(c, OP_POP);
            /* (let ((var g)) . body) — every operand is re-derived from
             * the rooted form or rooted g right before the cons that
             * consumes it. */
            binding = cl_cons(g, CL_NIL);                               /* (g) */
            binding = cl_cons(cl_car(cl_car(cl_cdr(hc_nth_clause(form, k)))),
                              binding);                                 /* (var g) */
            bindings = cl_cons(binding, CL_NIL);                        /* ((var g)) */
            let_form = cl_cons(bindings, cl_cdr(cl_cdr(hc_nth_clause(form, k))));
            let_form = cl_cons(SYM_LET, let_form);
            CL_GC_UNPROTECT(1);   /* g */
        } else {
            cl_emit(c, OP_POP);
            let_form = cl_cons(CL_NIL, cl_cdr(cl_cdr(hc_nth_clause(form, k))));
            let_form = cl_cons(SYM_LET, let_form);
        }
        CL_GC_PROTECT(let_form);
        compile_expr(c, let_form);
        CL_GC_UNPROTECT(1);
        cl_env_clear_boxed(env, saved_local_count);
        env->local_count = saved_local_count;
        cl_mv_normalize(c);
        if (k < n - 1)
            end_chain = cl_emit_jump_chain(c, OP_JMP, end_chain);
    }

    cl_patch_jump_chain(c, end_chain);
    /* Every arm reaches the join self-describing; the form's arm and the
     * LET bodies may be multi-valued. */
    c->mv_state = CL_MV_MANY;
    c->in_tail = saved_tail;
    CL_GC_UNPROTECT(1);   /* form */
}

/* --- Unwind-protect --- */

void compile_unwind_protect(CL_Compiler *c, CL_Obj form)
{
    /* (unwind-protect protected-form cleanup1 cleanup2 ...)
     *
     * Per CLHS: unwind-protect returns the values that result from
     * evaluating protected-form. We must capture the full multiple-value
     * state of protected-form across cleanup-form evaluation — single-value
     * save/restore would silently drop secondary values, which breaks
     * common idioms like (mv-bind (v p) (with-lock-held (l) (gethash k h)))
     * where p (presentp) is the second value. */
    CL_Obj protected_form = cl_car(cl_cdr(form));
    CL_Obj cleanup_forms;
    int saved_tail = c->in_tail;
    int uwprot_pos, jmp_pos;

    c->in_tail = 0;

    /* OP_UWPROT offset_to_cleanup_landing */
    cl_emit(c, OP_UWPROT);
    uwprot_pos = c->code_pos;
    cl_emit_i32(c, 0); /* placeholder */

    /* GC SAFETY: compile_expr(protected_form) allocates and can compact;
     * protect `form` and derive cleanup_forms only afterwards (same shape
     * as compile_if — a pre-compaction read would be a stale offset). */
    CL_GC_PROTECT(form);

    /* Compile protected form (leaves primary on stack, MV buffer holds rest) */
    compile_expr(c, protected_form);

    cleanup_forms = cl_cdr(cl_cdr(form));
    CL_GC_UNPROTECT(1);

    /* OP_MV_SAVE: pop the primary and park it, with the rest of the MV
     * buffer, on the per-thread value-save stack (vm.h CL_MV_SAVE_SIZE) —
     * no list is consed and no local slot is needed.  Normalize first so
     * the record holds the protected form's own values (CLHS 5.2:
     * unwind-protect returns the protected form's values). */
    cl_mv_normalize(c);
    cl_emit(c, OP_MV_SAVE);

    /* OP_UWPOP: normal exit, pop frame, clear pending */
    cl_emit(c, OP_UWPOP);

    /* JMP to cleanup_start (skip the landing) */
    jmp_pos = cl_emit_jump(c, OP_JMP);

    /* [cleanup_landing]: a non-local transfer through the frame arrives
     * here; the landing parked an EMPTY value record for the MV_RESTORE
     * below, so both paths reach the cleanup with one record on top. */
    cl_patch_jump(c, uwprot_pos);

    /* [cleanup_start]: both paths merge */
    cl_patch_jump(c, jmp_pos);

    /* Compile cleanup forms, discarding results */
    {
        CL_Obj cf = cleanup_forms;
        CL_GC_PROTECT(cf);
        while (!CL_NULL_P(cf)) {
            compile_expr(c, cl_car(cf));
            cl_emit(c, OP_POP);
            cf = cl_cdr(cf);
        }
        CL_GC_UNPROTECT(1);
    }

    /* OP_UWRETHROW: if pending throw, re-initiate (never returns); else nop */
    cl_emit(c, OP_UWRETHROW);

    /* OP_MV_RESTORE: pop the record back into the MV buffer and push the
     * primary — the protected form's values are re-established exactly as
     * VALUES-LIST used to do, with no call and no allocation. */
    cl_emit(c, OP_MV_RESTORE);
    c->mv_state = CL_MV_MANY;   /* all of the protected form's values */

    /* Restore */
    c->in_tail = saved_tail;
}

/* --- Local-function inlining (specs/performance.md 4.2 item 5) ------------
 *
 * A FLET/LABELS function that never escapes is not compiled into a closure
 * at all: each call to it compiles to its body in place —
 *
 *   args (call-site env) ; STORE params ; (BLOCK name (LOCALLY . body))
 *
 * — with the parameters bound as fresh locals and everything the call site
 * bound SINCE the definition (variables, local functions, macros, symbol
 * macros, blocks, go tags) hidden from the body through the hide_* bands in
 * CL_CompEnv / CL_Compiler, so its free names resolve in the definition
 * environment exactly as they would in the closure.  What it saves: the
 * closure allocation on every entry to the FLET (one heap object per
 * captured variable set), and per call the OP_CALL frame push, arity check
 * and OP_RET (bench-prims "flet-call": 43 ns → the body alone).
 *
 * "Never escapes" is decided by one macro-aware walk over the form
 * (nlx_scan in NLX_FUNUSE mode) — a function keeps its closure when any of
 * these appears in its scope: #'NAME; (declare (notinline NAME)); a call
 * from inside a closure (a LAMBDA, a sibling's or nested local function's
 * body, a HANDLER-BIND handler, a RESTART-CASE clause ...), since the body
 * is only ever inlined into the defining function's own code; for LABELS,
 * any reference from a group member's body (recursion, direct or mutual).
 * Also excluded: names that are not plain symbols, lambda lists with any
 * &-keyword, more than CL_LOCAL_INLINE_MAX_PARAMS parameters, a special
 * parameter, a (special ...) declaration in the body, a LOAD-TIME-VALUE
 * anywhere in the body (the same walk finds it, through macros — its
 * value must be one object per function, not one per call site), and —
 * to bound code growth — a body over CL_LOCAL_INLINE_MAX_SIZE conses
 * called from more than one site.  (optimize (space > speed)) or (debug 3)
 * turns it off for the form; AMILISP_NO_LOCAL_INLINE=1 for the process.
 *
 * The analysis is what makes the closure-free binding safe; two guards in
 * compiler.c (inline_local_misuse) report the case it cannot foresee — a
 * macro that expands differently on each expansion — as a clear compile
 * error rather than loading an empty slot. */

/* Shape test: returns the parameter count, or -1 when BINDING is not
 * inlinable on shape alone (see the list above). */
static int local_inline_shape(CL_Obj binding)
{
    CL_Obj name, ll, body, p;
    int n = 0;

    if (!CL_CONS_P(binding) || !CL_CONS_P(cl_cdr(binding))) return -1;
    name = cl_car(binding);
    if (!CL_SYMBOL_P(name) || CL_NULL_P(name) || name == SYM_T) return -1;
    if (((CL_Symbol *)CL_OBJ_TO_PTR(name))->package == cl_package_keyword) return -1;

    ll = cl_car(cl_cdr(binding));
    for (p = ll; !CL_NULL_P(p); p = cl_cdr(p)) {
        CL_Obj v, q;
        if (!CL_CONS_P(p)) return -1;                 /* dotted list */
        v = cl_car(p);
        if (!CL_SYMBOL_P(v) || CL_NULL_P(v) || v == SYM_T) return -1;
        if (cl_symbol_name(v)[0] == '&') return -1;   /* any lambda-list keyword */
        if (((CL_Symbol *)CL_OBJ_TO_PTR(v))->package == cl_package_keyword) return -1;
        if (cl_symbol_specialp(v)) return -1;         /* would need a dynamic binding */
        for (q = ll; q != p; q = cl_cdr(q))
            if (cl_car(q) == v) return -1;            /* duplicate parameter */
        if (++n > CL_LOCAL_INLINE_MAX_PARAMS) return -1;
    }

    /* Leading declarations: a (special ...) changes how the body's
     * references bind — leave that to the closure path. */
    for (body = cl_cdr(cl_cdr(binding)); CL_CONS_P(body); body = cl_cdr(body)) {
        CL_Obj f = cl_car(body);
        CL_Obj specs;
        if (CL_ANY_STRING_P(f) && CL_CONS_P(cl_cdr(body))) continue;
        if (!CL_CONS_P(f) || cl_car(f) != SYM_DECLARE) break;
        for (specs = cl_cdr(f); CL_CONS_P(specs); specs = cl_cdr(specs)) {
            CL_Obj spec = cl_car(specs);
            if (CL_CONS_P(spec) && cl_car(spec) == SYM_SPECIAL_DECL) return -1;
        }
    }
    return n;
}

/* Cons count of TREE, stopping once it exceeds LIMIT (returns LIMIT + 1). */
static int local_inline_size(CL_Obj tree, int limit)
{
    int n = 0;
    while (CL_CONS_P(tree) && n <= limit) {
        n++;
        if (CL_CONS_P(cl_car(tree)))
            n += local_inline_size(cl_car(tree), limit - n);
        tree = cl_cdr(tree);
    }
    return n;
}

/* Run the escape scan for the N local functions of FORM (a FLET or LABELS
 * form, GC-protected by the caller).  The candidate names are registered
 * as env->local_funs[lo .. lo+N) for the duration of the walk, so the
 * walker identifies them by entry (funuse_index), and unregistered again.
 * Every function body is walked as a closure; the form's body is not.  For
 * FLET a sibling reference inside a function body names an OUTER function,
 * so counting it as an escape is merely conservative; for LABELS it is the
 * recursion check.  Results stay in c->funuse_escaped / c->funuse_calls. */
static void local_inline_scan(CL_Compiler *c, CL_Obj form, int n)
{
    CL_CompEnv *env = c->env;
    int lo = env->local_fun_count;
    int i;
    CL_Obj b;

    c->funuse_lo = lo;
    c->funuse_n = 0;
    c->funuse_cur = -1;
    c->funuse_escaped = 0;
    memset(c->funuse_calls, 0, sizeof(c->funuse_calls));

    b = cl_car(cl_cdr(form));
    for (i = 0; i < n; i++, b = cl_cdr(b)) {
        CL_Obj binding = cl_car(b);
        CL_Obj name = CL_CONS_P(binding) ? cl_car(binding) : CL_NIL;
        if (cl_env_add_local_fun(env, name, -1) < 0) {
            /* No room to register — nothing in this form is inlined. */
            env->local_fun_count = lo;
            c->funuse_escaped = ~(uint32_t)0;
            return;
        }
    }
    c->funuse_n = n;

    /* GC SAFETY: the walk macroexpands (allocates / compacts); every cursor
     * re-read after a scan call is rooted.  form itself is the caller's.
     * funuse_cur names the body being walked so a LOAD-TIME-VALUE inside it
     * (at any depth — a nested closure's copy is compiled per site too)
     * vetoes that candidate; the form's own body is nobody's. */
    scan_nlx_arg_depth = 0;
    b = cl_car(cl_cdr(form));
    CL_GC_PROTECT(b);
    for (i = 0; CL_CONS_P(b); i++, b = cl_cdr(b)) {
        CL_Obj def = cl_car(b);
        c->funuse_cur = i < n ? i : -1;
        if (CL_CONS_P(def) && CL_CONS_P(cl_cdr(def)))
            nlx_scan_body(cl_cdr(cl_cdr(def)), NLX_FUNUSE, CL_NIL, 1);
    }
    CL_GC_UNPROTECT(1);
    c->funuse_cur = -1;
    nlx_scan_body(cl_cdr(cl_cdr(form)), NLX_FUNUSE, CL_NIL, 0);

    c->funuse_n = 0;
    env->local_fun_count = lo;   /* unregister the scan entries */
}

/* Decide inlining for candidate I of the form the scan just ran over.
 * ESCAPED / CALLS are the scan results (copied out by the caller, since a
 * nested FLET compiled meanwhile reuses the scratch fields). */
static int local_inline_decide(CL_Obj binding, int i, uint32_t escaped,
                               const uint8_t *calls)
{
    int nparams, size;
    if (i >= CL_LOCAL_INLINE_MAX_NAMES) return 0;
    if (escaped & ((uint32_t)1 << i)) return 0;
    nparams = local_inline_shape(binding);
    if (nparams < 0) return 0;
    size = local_inline_size(cl_cdr(cl_cdr(binding)), CL_LOCAL_INLINE_MAX_SIZE);
    if (calls[i] > 1 && size > CL_LOCAL_INLINE_MAX_SIZE) return 0;
#ifdef DEBUG_COMPILER
    fprintf(stderr, "; inline: local function %s (%d params, %d call sites, %d conses)\n",
            cl_symbol_name(cl_car(binding)), nparams, calls[i], size);
#endif
    return 1;
}

/* Whether the effective optimize policy allows inlining at all. */
static int local_inline_policy_ok(CL_Compiler *c)
{
    return cl_local_inline_enabled &&
           c->optimize_settings.space <= c->optimize_settings.speed &&
           c->optimize_settings.debug < 3;
}

static void local_inline_fill(CL_Compiler *c, CL_Obj b, int idx,
                              CL_Obj inline_body,
                              int local_mark, int fun_mark,
                              int macro_mark, int smacro_mark,
                              int block_mark, int tagbody_mark);

/* Register BINDING (GC-protected through the cursor B it was read from) as
 * an inlined local function: an anonymous slot so the name shadows global
 * macros / compiler macros like any local function, the entry's inline
 * fields, and the definition marks.  FUN_MARK / LOCAL_MARK are the env
 * counts the body may see up to (FLET hides its own group, LABELS not). */
static void local_inline_register(CL_Compiler *c, CL_Obj b, int slot,
                                  int local_mark, int fun_mark,
                                  int macro_mark, int smacro_mark,
                                  int block_mark, int tagbody_mark)
{
    CL_CompEnv *env = c->env;
    int idx = cl_env_add_local_fun(env, cl_car(cl_car(b)), slot);
    if (idx < 0) {
        /* Same silent overflow as the closure path (see compile_labels);
         * a call then falls through to the global name — nothing is
         * emitted that would read the slot. */
        return;
    }
    local_inline_fill(c, b, idx, CL_NIL, local_mark, fun_mark,
                      macro_mark, smacro_mark, block_mark, tagbody_mark);
}

/* Fill entry IDX (already registered) with the inline fields.  The body
 * form is (LOCALLY . body) — LOCALLY so the function's declarations and
 * docstring stay legal — wrapped in (BLOCK name ...) only when the body
 * returns from it; pass INLINE_BODY = CL_NIL to have it built here.  B is
 * the GC-protected bindings cursor at this binding; the conses are
 * sequenced and re-read from it, as compile_flet builds its lambda form. */
static void local_inline_fill(CL_Compiler *c, CL_Obj b, int idx,
                              CL_Obj inline_body,
                              int local_mark, int fun_mark,
                              int macro_mark, int smacro_mark,
                              int block_mark, int tagbody_mark)
{
    CL_CompEnv *env = c->env;
    /* GC SAFETY: the caller's cursor is protected in the CALLER's frame;
     * this parameter is a copy of its value, which the compactor does not
     * forward.  The RETURN-FROM scan below macroexpands (allocates, and
     * under gc-stress compacts at every allocation), so the re-reads
     * through B after it -- the LOCALLY cons, the block name, the
     * parameter list -- would walk a stale offset: `(defparameter *x*
     * (flet ((f () 1)) (f)))' loaded under stress failed with "CDR:
     * argument is not of type LIST (got SYMBOL)" here (the clamacs menu
     * table, 2026-09-20).  Protect the copy. */
    CL_GC_PROTECT(b);
    if (CL_NULL_P(inline_body)) {
        /* The implicit BLOCK only when the body can RETURN-FROM it (a
         * macro-aware scan, like compile_block's): a local-exit block
         * costs a result-slot STORE/POP/LOAD round trip per call. */
        int needs_block;
        scan_nlx_arg_depth = 0;
        needs_block = nlx_scan_body(cl_cdr(cl_cdr(cl_car(b))), NLX_FIND_RF,
                                    cl_car(cl_car(b)), 0);
        inline_body = cl_cons(SYM_LOCALLY, cl_cdr(cl_cdr(cl_car(b))));
        if (needs_block) {
            CL_GC_PROTECT(inline_body);
            inline_body = cl_cons(inline_body, CL_NIL);
            inline_body = cl_cons(cl_car(cl_car(b)), inline_body);
            inline_body = cl_cons(SYM_BLOCK, inline_body);
            CL_GC_UNPROTECT(1);
        }
    }
    env->local_funs[idx].inline_body     = inline_body;
    env->local_funs[idx].inline_params   = cl_car(cl_cdr(cl_car(b)));
    CL_GC_UNPROTECT(1);                                   /* b */
    env->local_funs[idx].def_local_mark  = local_mark;
    env->local_funs[idx].def_fun_mark    = fun_mark;
    env->local_funs[idx].def_macro_mark  = macro_mark;
    env->local_funs[idx].def_smacro_mark = smacro_mark;
    env->local_funs[idx].def_block_mark  = block_mark;
    env->local_funs[idx].def_tagbody_mark = tagbody_mark;
}

/* A call to inlined local function env->local_funs[FUN_IDX] with ARGS
 * (GC-protected by compile_call).  Emits the arguments, binds the
 * parameters, then compiles the body with the call site's newer bindings
 * hidden.  Leaves c->in_tail = SAVED_TAIL and mv_state as the body's. */
void compile_local_inline_call(CL_Compiler *c, int fun_idx, CL_Obj args,
                               int saved_tail)
{
    CL_CompEnv *env = c->env;
    int nparams = 0, nargs = 0, i;
    int saved_local_count;
    CL_Obj cur;

    for (cur = env->local_funs[fun_idx].inline_params; CL_CONS_P(cur); cur = cl_cdr(cur))
        nparams++;
    for (cur = args; CL_CONS_P(cur); cur = cl_cdr(cur))
        nargs++;

    /* 1. The arguments, left to right, in the CALL-SITE environment. */
    c->in_tail = 0;
    cur = args;
    CL_GC_PROTECT(cur);
    while (CL_CONS_P(cur)) {
        compile_expr(c, cl_car(cur));
        cur = cl_cdr(cur);
    }
    CL_GC_UNPROTECT(1);

    if (nargs != nparams) {
        /* Evaluated and dropped, then the same PROGRAM-ERROR the VM's arity
         * check raises for the closure call — at run time, when the call
         * is reached, not when the function is compiled. */
        for (i = 0; i < nargs; i++)
            cl_emit(c, OP_POP);
        cl_emit_const(c, env->local_funs[fun_idx].name);
        cl_emit_const(c, CL_MAKE_FIXNUM(nparams));
        cl_emit_const(c, CL_MAKE_FIXNUM(nargs));
        cl_emit_call_global(c, OP_CALL_GLOBAL,
                            (uint16_t)cl_add_constant(c, cl_local_arity_error_sym), 3);
        c->in_tail = saved_tail;
        return;
    }

    /* 2. Bind the parameters as fresh locals above everything the call
     *    site holds (the last argument is on top of the stack).  Boxing
     *    follows LET: a parameter both assigned and captured by a closure
     *    in the body lives in a cell. */
    saved_local_count = env->local_count;
    if (nparams > 0) {
        CL_Obj params[CL_LOCAL_INLINE_MAX_PARAMS];
        uint8_t needs_boxing[CL_LOCAL_INLINE_MAX_PARAMS];
        int slots[CL_LOCAL_INLINE_MAX_PARAMS];
        cur = env->local_funs[fun_idx].inline_params;
        for (i = 0; i < nparams; i++, cur = cl_cdr(cur))
            params[i] = cl_car(cur);
        for (i = 0; i < nparams; i++)
            CL_GC_PROTECT(params[i]);
        /* The body list is (LOCALLY . body) inside the BLOCK: scan that. */
        determine_boxed_vars(cl_cdr(cl_cdr(env->local_funs[fun_idx].inline_body)),
                             params, nparams, needs_boxing);
        for (i = 0; i < nparams; i++) {
            slots[i] = cl_env_add_local(env, params[i]);
            if (slots[i] < 0)
                cl_error(CL_ERR_OVERFLOW,
                         "Too many local variable slots in one function (max %d): "
                         "simplify or split the function", CL_MAX_LOCALS);
        }
        cl_gc_pop_roots(nparams);
        for (i = nparams - 1; i >= 0; i--) {
            cl_emit(c, OP_STORE);
            cl_emit(c, (uint8_t)slots[i]);
            cl_emit(c, OP_POP);
            if (needs_boxing[i]) {
                cl_emit(c, OP_LOAD);
                cl_emit(c, (uint8_t)slots[i]);
                cl_emit(c, OP_MAKE_CELL);
                cl_emit(c, OP_STORE);
                cl_emit(c, (uint8_t)slots[i]);
                cl_emit(c, OP_POP);
                env->boxed[slots[i]] = 1;
            }
        }
    }

    /* 3. Hide what the call site bound since the definition, 4. the body
     *    (its own compile_expr drains every tail frame it pushes, so the
     *    bands are restored right after the block), 5. restore. */
    {
        CL_LocalFun *lf = &env->local_funs[fun_idx];
        int s_ll = env->hide_local_lo,  s_lh = env->hide_local_hi;
        int s_fl = env->hide_fun_lo,    s_fh = env->hide_fun_hi;
        int s_ml = env->hide_macro_lo,  s_mh = env->hide_macro_hi;
        int s_sl = env->hide_smacro_lo, s_sh = env->hide_smacro_hi;
        int s_bl = c->hide_block_lo,    s_bh = c->hide_block_hi;
        int s_tl = c->hide_tagbody_lo,  s_th = c->hide_tagbody_hi;

        env->hide_local_lo  = lf->def_local_mark;  env->hide_local_hi  = saved_local_count;
        env->hide_fun_lo    = lf->def_fun_mark;    env->hide_fun_hi    = env->local_fun_count;
        env->hide_macro_lo  = lf->def_macro_mark;  env->hide_macro_hi  = env->local_macro_count;
        env->hide_smacro_lo = lf->def_smacro_mark; env->hide_smacro_hi = env->symbol_macro_count;
        c->hide_block_lo    = lf->def_block_mark;  c->hide_block_hi    = c->block_count;
        c->hide_tagbody_lo  = lf->def_tagbody_mark; c->hide_tagbody_hi = c->tagbody_count;

        c->in_tail = saved_tail;
        compile_expr(c, env->local_funs[fun_idx].inline_body);

        env->hide_local_lo  = s_ll; env->hide_local_hi  = s_lh;
        env->hide_fun_lo    = s_fl; env->hide_fun_hi    = s_fh;
        env->hide_macro_lo  = s_ml; env->hide_macro_hi  = s_mh;
        env->hide_smacro_lo = s_sl; env->hide_smacro_hi = s_sh;
        c->hide_block_lo    = s_bl; c->hide_block_hi    = s_bh;
        c->hide_tagbody_lo  = s_tl; c->hide_tagbody_hi  = s_th;
    }

    cl_env_clear_boxed(env, saved_local_count);
    env->local_count = saved_local_count;
    c->in_tail = saved_tail;
}

/* --- Flet / Labels --- */

CL_Obj compile_flet(CL_Compiler *c, CL_Obj form)
{
    /* (flet ((name (params) body...) ...) body...)
     *
     * Trampoline-aware: prelude compiles each local function and pushes
     * a tail frame that restores local + local-fun counts in the
     * postlude.  Body's tail form returns to the driver. */
    CL_Obj bindings = cl_car(cl_cdr(form));
    CL_Obj body;
    CL_CompEnv *env = c->env;
    int saved_local_count = env->local_count;
    int saved_fun_count = env->local_fun_count;
    int saved_tail = c->in_tail;
    CL_TailFrame *tf;
    /* Local-function inlining: one escape scan over the whole form before
     * any function is compiled; the results are copied out because a
     * nested FLET compiled during phase 1 reuses the scratch fields.  The
     * marks are what an inlined body may see (everything bound later at a
     * call site is hidden from it). */
    int macro_mark = env->local_macro_count;
    int smacro_mark = env->symbol_macro_count;
    int block_mark = c->block_count;
    int tagbody_mark = c->tagbody_count;
    uint32_t escaped = ~(uint32_t)0;
    uint8_t calls[CL_LOCAL_INLINE_MAX_NAMES];
    int n_bindings = 0, bi = 0;

    /* GC SAFETY: phase 1's compile_expr calls compact — protect `form` and
     * derive `body` only after them. */
    CL_GC_PROTECT(form);

    { CL_Obj b = bindings; while (CL_CONS_P(b)) { n_bindings++; b = cl_cdr(b); } }
    memset(calls, 0, sizeof(calls));
    if (local_inline_policy_ok(c) && n_bindings <= CL_LOCAL_INLINE_MAX_NAMES) {
        local_inline_scan(c, form, n_bindings);   /* macroexpands: compacts */
        escaped = c->funuse_escaped;
        memcpy(calls, c->funuse_calls, sizeof(calls));
    }
    bindings = cl_car(cl_cdr(form));   /* re-derive after the scan */

    /* Phase 1: compile each function in outer scope, store in anonymous slots */
    {
        CL_Obj b = bindings;
        CL_GC_PROTECT(b);
        while (!CL_NULL_P(b)) {
            CL_Obj binding = cl_car(b);
            CL_Obj fname = cl_car(binding);
            CL_Obj fbody = cl_cdr(cl_cdr(binding));
            CL_Obj lambda_form;
            int slot;

            if (local_inline_decide(binding, bi, escaped, calls)) {
                /* No closure: an anonymous slot (never written or read —
                 * it only makes the name a local function for every
                 * shadowing check) and the inline entry.  FLET functions
                 * cannot see their own group: fun_mark = saved_fun_count. */
                slot = alloc_temp_slot(env);
                local_inline_register(c, b, slot, saved_local_count,
                                      saved_fun_count, macro_mark, smacro_mark,
                                      block_mark, tagbody_mark);
                bi++;
                b = cl_cdr(b);
                continue;
            }
            bi++;

            /* Build (lambda (params) (block name body...)) per CL spec:
             * flet functions have an implicit block named after the function.
             * GC SAFETY: each cl_cons can compact — build the chain one call
             * at a time (cl_cons forwards its own args) and re-derive
             * lambda_list from the protected cursor b only after the conses
             * that precede its use; a nested cl_cons(lambda_list, cl_cons(..))
             * may read lambda_list before the inner cons moves it. */
            {
                CL_Obj block_form = cl_cons(fname, fbody);
                CL_Obj lambda_list;
                block_form = cl_cons(SYM_BLOCK, block_form);
                lambda_form = cl_cons(block_form, CL_NIL);
                lambda_list = cl_car(cl_cdr(cl_car(b)));  /* re-derive post-compaction */
                lambda_form = cl_cons(lambda_list, lambda_form);
                lambda_form = cl_cons(SYM_LAMBDA, lambda_form);
            }
            CL_GC_PROTECT(lambda_form);

            /* Hand the local function's own name to compile_lambda, which
             * claims pending_lambda_name at entry — so it lands on this lambda
             * and not on the first one nested inside its body.  Re-derive from
             * the protected cursor: the conses above may have compacted the
             * pre-build snapshot.  A (setf foo) local stays anonymous, since
             * every consumer of bc->name (printer, backtrace) reads it as a
             * symbol. */
            fname = cl_car(cl_car(b));
            if (CL_SYMBOL_P(fname))
                pending_lambda_name = fname;

            c->in_tail = 0;
            compile_expr(c, lambda_form);
            CL_GC_UNPROTECT(1);

            /* Allocate anonymous slot and store */
            slot = env->local_count;
            env->local_count++;
            if (env->local_count > env->max_locals)
                env->max_locals = env->local_count;
            /* Store CL_NIL in locals[] to keep it anonymous */
            env->locals[slot] = CL_NIL;

            cl_emit(c, OP_STORE);
            cl_emit(c, (uint8_t)slot);
            cl_emit(c, OP_POP);

            /* Register in function namespace */
            /* Re-read fname from protected cursor b: compile_expr above may
             * have triggered compaction, staling the pre-compile snapshot. */
            fname = cl_car(cl_car(b));
            /* -1 (overflow past CL_MAX_LOCAL_FUNS) left silent on purpose;
             * see the matching note in compile_labels. */
            cl_env_add_local_fun(env, fname, slot);

            b = cl_cdr(b);
        }
        CL_GC_UNPROTECT(1);
    }

    c->in_tail = saved_tail;

    body = cl_cdr(cl_cdr(form));  /* re-derive: phase 1 may have compacted */
    CL_GC_UNPROTECT(1);

    tf = cl_tail_push(c);
    tf->kind = CL_TAIL_FLET;
    tf->saved_local_count = saved_local_count;
    tf->saved_block_count = saved_fun_count;  /* reused: saved_local_fun_count */
    tf->saved_tail = saved_tail;
    tf->saved_notinline_count = c->notinline_count;
    return compile_body_tail(c, body);
}

CL_Obj compile_labels(CL_Compiler *c, CL_Obj form)
{
    /* (labels ((name (params) body...) ...) body...)
     *
     * Like flet but functions can reference each other (mutual recursion).
     * We pre-allocate local slots for all function names first, then compile
     * each function body (which can now resolve cross-references via the
     * local function namespace).
     *
     * Trampoline-aware: same shape as compile_flet, postlude restores env. */
    CL_Obj bindings = cl_car(cl_cdr(form));
    CL_Obj body;
    CL_CompEnv *env = c->env;
    int saved_local_count = env->local_count;
    int saved_fun_count = env->local_fun_count;
    int saved_tail = c->in_tail;
    /* Local-function inlining — see compile_flet.  A LABELS member is
     * inlined only when no member's body (its own included) references
     * it, which the scan reports as an escape; the group stays visible to
     * an inlined body (fun_mark / local_mark cover the group's entries). */
    int macro_mark = env->local_macro_count;
    int smacro_mark = env->symbol_macro_count;
    int block_mark = c->block_count;
    int tagbody_mark = c->tagbody_count;
    uint32_t escaped = ~(uint32_t)0;
    uint8_t calls[CL_LOCAL_INLINE_MAX_NAMES];
    uint8_t is_inline[CL_MAX_LOCAL_FUNS];
    int n_bindings = 0;

    /* GC SAFETY: phase 2's compile_expr calls compact — protect `form` and
     * derive `body` only after them. */
    CL_GC_PROTECT(form);

    { CL_Obj b = bindings; while (CL_CONS_P(b)) { n_bindings++; b = cl_cdr(b); } }
    memset(calls, 0, sizeof(calls));
    memset(is_inline, 0, sizeof(is_inline));
    if (local_inline_policy_ok(c) && n_bindings <= CL_LOCAL_INLINE_MAX_NAMES) {
        local_inline_scan(c, form, n_bindings);   /* macroexpands: compacts */
        escaped = c->funuse_escaped;
        memcpy(calls, c->funuse_calls, sizeof(calls));
    }
    bindings = cl_car(cl_cdr(form));   /* re-derive after the scan */

    /* Phase 1: pre-allocate slots, initialize to NIL, box them, and register
     * all function names. Boxing is required so that closures compiled in
     * phase 2 capture a reference to the box cell (not a copy of NIL).
     * An inlined member gets its slot (anonymous, never touched) and its
     * entry, but no initialization or box. */
    {
        CL_Obj b = bindings;
        int n = 0;
        while (!CL_NULL_P(b)) {
            CL_Obj binding = cl_car(b);
            CL_Obj fname = cl_car(binding);
            int slot = env->local_count;
            env->local_count++;
            if (env->local_count > env->max_locals)
                env->max_locals = env->local_count;
            env->locals[slot] = CL_NIL;  /* anonymous slot */

            if (n < CL_MAX_LOCAL_FUNS &&
                local_inline_decide(binding, n, escaped, calls)) {
                is_inline[n] = 1;
                cl_env_add_local_fun(env, fname, slot);
                n++;
                b = cl_cdr(b);
                continue;
            }

            /* Initialize slot to NIL */
            cl_emit(c, OP_CONST);
            cl_emit_u16(c, cl_add_constant(c, CL_NIL));
            cl_emit(c, OP_STORE);
            cl_emit(c, (uint8_t)slot);
            cl_emit(c, OP_POP);

            /* Register in function namespace.  A -1 return (more than
             * CL_MAX_LOCAL_FUNS functions simultaneously in scope) is left
             * silent on purpose: deeply-nested LABELS/FLET forms accumulate
             * entries in this shared env, and the overflowing names are
             * harmless unless actually referenced — a reference to an unbound
             * one falls through to a global FLOAD and surfaces cleanly as
             * "Undefined function" at runtime rather than corrupting state. */
            cl_env_add_local_fun(env, fname, slot);

            n++;
            b = cl_cdr(b);
        }

        /* Box all slots so closures capture by reference */
        {
            int i;
            for (i = 0; i < n; i++) {
                int slot = saved_local_count + i;
                if (i < CL_MAX_LOCAL_FUNS && is_inline[i]) continue;
                cl_emit(c, OP_LOAD);
                cl_emit(c, (uint8_t)slot);
                cl_emit(c, OP_MAKE_CELL);
                cl_emit(c, OP_STORE);
                cl_emit(c, (uint8_t)slot);
                cl_emit(c, OP_POP);
                env->boxed[slot] = 1;
            }
        }

        /* The inline entries, now that the whole group is registered (an
         * inlined body sees every member, so the fun mark is the count
         * after registration). */
        {
            int i, fun_mark = env->local_fun_count;
            int local_mark = env->local_count;
            b = cl_car(cl_cdr(form));
            CL_GC_PROTECT(b);
            for (i = 0; i < n && CL_CONS_P(b); i++, b = cl_cdr(b)) {
                int idx;
                if (!(i < CL_MAX_LOCAL_FUNS && is_inline[i])) continue;
                idx = cl_env_lookup_local_fun_index(env, cl_car(cl_car(b)));
                if (idx < 0 || idx < saved_fun_count) continue;   /* not registered */
                local_inline_fill(c, b, idx, CL_NIL, local_mark, fun_mark,
                                  macro_mark, smacro_mark, block_mark,
                                  tagbody_mark);
            }
            CL_GC_UNPROTECT(1);
        }
    }

    /* Phase 2: compile each function and store in its pre-allocated (boxed) slot */
    {
        CL_Obj b = cl_car(cl_cdr(form));   /* re-derive: phase 1's inline entries consed */
        int slot = saved_local_count;  /* first allocated slot */
        int i = 0;
        CL_GC_PROTECT(b);
        while (!CL_NULL_P(b)) {
            CL_Obj binding = cl_car(b);
            CL_Obj fname = cl_car(binding);
            CL_Obj fbody = cl_cdr(cl_cdr(binding));
            CL_Obj lambda_form;

            if (i < CL_MAX_LOCAL_FUNS && is_inline[i]) {
                /* Inlined: no closure to build. */
                i++;
                slot++;
                b = cl_cdr(b);
                continue;
            }
            i++;

            /* Build (lambda (params) (block name body...)) per CL spec:
             * labels functions have an implicit block named after the function.
             * GC SAFETY: same sequenced-cons + re-derive shape as compile_flet
             * — see the comment there. */
            {
                CL_Obj block_form = cl_cons(fname, fbody);
                CL_Obj lambda_list;
                block_form = cl_cons(SYM_BLOCK, block_form);
                lambda_form = cl_cons(block_form, CL_NIL);
                lambda_list = cl_car(cl_cdr(cl_car(b)));  /* re-derive post-compaction */
                lambda_form = cl_cons(lambda_list, lambda_form);
                lambda_form = cl_cons(SYM_LAMBDA, lambda_form);
            }
            CL_GC_PROTECT(lambda_form);

            /* Name the local function — see the matching note in compile_flet. */
            fname = cl_car(cl_car(b));
            if (CL_SYMBOL_P(fname))
                pending_lambda_name = fname;

            c->in_tail = 0;
            compile_expr(c, lambda_form);
            CL_GC_UNPROTECT(1);

            /* Store in boxed slot */
            cl_emit(c, OP_CELL_SET_LOCAL);
            cl_emit(c, (uint8_t)slot);
            cl_emit(c, OP_POP);

            slot++;
            b = cl_cdr(b);
        }
        CL_GC_UNPROTECT(1);
    }

    /* Phase 3: compile body via trampoline */
    c->in_tail = saved_tail;
    body = cl_cdr(cl_cdr(form));  /* re-derive: phase 2 may have compacted */
    CL_GC_UNPROTECT(1);
    {
        CL_TailFrame *tf = cl_tail_push(c);
        tf->kind = CL_TAIL_LABELS;
        tf->saved_local_count = saved_local_count;
        tf->saved_block_count = saved_fun_count;  /* reused: saved_local_fun_count */
        tf->saved_tail = saved_tail;
        tf->saved_notinline_count = c->notinline_count;
        return compile_body_tail(c, body);
    }
}

/* --- Loop forms --- */

/* Shared loop building blocks used by DOLIST / DOTIMES / DO / DO*. */

/* CLHS 3.6/6.1.6: the body of a loop macro is an implicit TAGBODY.  Strip
 * DECLARE forms, wrap the rest in a single (tagbody ...), compile it, and POP
 * its NIL result.  `body` must already be GC-protected by the caller. */
static void compile_implicit_tagbody_body(CL_Compiler *c, CL_Obj body)
{
    CL_Obj b = body;
    CL_Obj tb_forms = CL_NIL, tb_tail = CL_NIL;
    CL_Obj tagbody_form;
    /* GC-protect b/tb_forms/tb_tail — cl_cons inside the loop can compact. */
    CL_GC_PROTECT(b);
    CL_GC_PROTECT(tb_forms);
    CL_GC_PROTECT(tb_tail);
    while (!CL_NULL_P(b)) {
        CL_Obj bform = cl_car(b);
        if (!(CL_CONS_P(bform) && cl_car(bform) == SYM_DECLARE)) {
            CL_Obj cell = cl_cons(bform, CL_NIL);
            if (CL_NULL_P(tb_forms)) tb_forms = cell;
            else ((CL_Cons *)CL_OBJ_TO_PTR(tb_tail))->cdr = cell;
            tb_tail = cell;
        }
        b = cl_cdr(b);
    }
    tagbody_form = cl_cons(SYM_TAGBODY, tb_forms);
    CL_GC_PROTECT(tagbody_form);
    compile_expr(c, tagbody_form);
    cl_emit(c, OP_POP);  /* tagbody always leaves NIL */
    CL_GC_UNPROTECT(4);  /* tagbody_form, tb_tail, tb_forms, b */
}

/* Allocate a result slot and push an implicit BLOCK NIL for a loop form.
 * Returns the result slot index; *bi_out receives the block-info pointer. */
static int push_loop_nil_block(CL_Compiler *c, CL_BlockInfo **bi_out)
{
    CL_CompEnv *env = c->env;
    int result_slot = env->local_count;
    CL_BlockInfo *bi;
    env->locals[result_slot] = CL_NIL;  /* Clear stale binding */
    env->local_count++;
    if (env->local_count > env->max_locals)
        env->max_locals = env->local_count;
    if (c->block_count >= CL_MAX_BLOCKS)
        cl_error(CL_ERR_OVERFLOW,
                 "BLOCK nesting depth exceeded (%d): "
                 "consider raising CL_MAX_BLOCKS", CL_MAX_BLOCKS);
    bi = &c->blocks[c->block_count++];
    bi->tag = CL_NIL;
    bi->n_patches = 0;
    bi->result_slot = result_slot;
    bi->uses_nlx = 0;
    bi->dyn_depth = c->special_depth;
    bi->mv_state = CL_MV_ONE;   /* identity; merged with each exit */
    bi->exit_stale_mask = 0;
    *bi_out = bi;
    return result_slot;
}

/* Loop epilogue: patch all RETURN-FROM NIL exits to here, load the result
 * slot, clear boxed locals, and restore the block/local counts. */
static void emit_loop_epilogue(CL_Compiler *c, CL_BlockInfo *bi,
                               int result_slot, int saved_local_count,
                               int saved_block_count)
{
    CL_CompEnv *env = c->env;
    /* c->mv_state still describes the result form stored into the slot
     * (STORE/POP/JMP/LOAD run no Lisp); join it with the RETURN exits,
     * normalizing the stale ones if another arm is multi-valued. */
    cl_mv_close_patch_join(c, bi->exit_patches, bi->n_patches,
                           bi->exit_stale_mask,
                           cl_mv_merge(c->mv_state, bi->mv_state));
    cl_emit(c, OP_LOAD);
    cl_emit(c, (uint8_t)result_slot);
    cl_env_clear_boxed(env, saved_local_count);
    c->block_count = saved_block_count;
    env->local_count = saved_local_count;
}

void compile_dolist(CL_Compiler *c, CL_Obj form)
{
    /* (dolist (var list-form [result-form]) body...) */
    CL_Obj binding = cl_car(cl_cdr(form));
    CL_Obj body = cl_cdr(cl_cdr(form));
    CL_Obj var = cl_car(binding);
    CL_Obj list_form = cl_car(cl_cdr(binding));
    CL_Obj result_form = cl_car(cl_cdr(cl_cdr(binding)));
    CL_CompEnv *env = c->env;
    int saved_local_count = env->local_count;
    int saved_tail = c->in_tail;
    int saved_block_count = c->block_count;
    int var_slot, iter_slot, result_slot;
    int loop_start, jnil_pos;
    int var_boxed = 0;
    CL_BlockInfo *bi;

    /* GC SAFETY: see compile_dotimes — these locals index into the
     * (protected) enclosing form but are not rewritten by the compactor, and
     * are re-read after compile_expr(list_form) / the tagbody compile.  Protect
     * them so a compaction can't leave them pointing at stale memory. */
    CL_GC_PROTECT(form);
    CL_GC_PROTECT(binding);
    CL_GC_PROTECT(body);
    CL_GC_PROTECT(var);
    CL_GC_PROTECT(result_form);

    c->in_tail = 0;

    /* Allocate iter slot FIRST — list-form must be evaluated before
     * the loop variable is bound (CL spec), so we compile list-form
     * into iter_slot while the outer env is still visible. */
    iter_slot = env->local_count;
    env->locals[iter_slot] = CL_NIL;
    env->local_count++;
    if (env->local_count > env->max_locals)
        env->max_locals = env->local_count;

    /* Compile list-form BEFORE adding var to env.
     * This ensures (dolist (x (f x)) ...) evaluates (f x) using
     * the OUTER binding of x, not the new loop variable. */
    compile_expr(c, list_form);
    cl_emit(c, OP_STORE);
    cl_emit(c, (uint8_t)iter_slot);
    cl_emit(c, OP_POP);

    /* NOW allocate var slot — after list-form is compiled */
    var_slot = cl_env_add_local(env, var);

    /* Check if loop var is captured in body (always mutated by loop).
     * GC SAFETY: protect `cur` across scan_body_for_boxing — see
     * compile_dotimes for the full rationale (macroexpansion compacts; an
     * unprotected cursor derails into garbage). */
    {
        uint8_t captured = 0, mutated = 0;
        CL_Obj cur = body;
        CL_GC_PROTECT(cur);
        while (CL_CONS_P(cur)) {
            scan_body_for_boxing(cl_car(cur), &var, 1, &mutated, &captured, 0);
            cur = cl_cdr(cur);
        }
        if (!CL_NULL_P(cl_cdr(cl_cdr(binding))))
            scan_body_for_boxing(result_form, &var, 1, &mutated, &captured, 0);
        var_boxed = captured;
        CL_GC_UNPROTECT(1);
    }

    /* If boxed, initialize cell in var_slot */
    if (var_boxed) {
        cl_emit(c, OP_NIL);
        cl_emit(c, OP_MAKE_CELL);
        cl_emit(c, OP_STORE);
        cl_emit(c, (uint8_t)var_slot);
        cl_emit(c, OP_POP);
        env->boxed[var_slot] = 1;
    }

    /* Allocate result slot + push implicit block NIL */
    result_slot = push_loop_nil_block(c, &bi);

    /* loop_start: LOAD iter_slot, JNIL -> end */
    loop_start = c->code_pos;
    cl_emit(c, OP_LOAD);
    cl_emit(c, (uint8_t)iter_slot);
    jnil_pos = cl_emit_jump(c, OP_JNIL);

    /* Set var = (car iter) */
    cl_emit(c, OP_LOAD);
    cl_emit(c, (uint8_t)iter_slot);
    cl_emit(c, OP_CAR);
    if (var_boxed) {
        cl_emit(c, OP_CELL_SET_LOCAL);
    } else {
        cl_emit(c, OP_STORE);
    }
    cl_emit(c, (uint8_t)var_slot);
    cl_emit(c, OP_POP);

    /* Advance iter = (cdr iter) */
    cl_emit(c, OP_LOAD);
    cl_emit(c, (uint8_t)iter_slot);
    cl_emit(c, OP_CDR);
    cl_emit(c, OP_STORE);
    cl_emit(c, (uint8_t)iter_slot);
    cl_emit(c, OP_POP);

    /* CLHS 3.6: the body of DOLIST is an implicit TAGBODY. */
    compile_implicit_tagbody_body(c, body);

    /* Backward jump to loop_start */
    cl_emit_loop_jump(c, OP_JMP, loop_start);

    /* end: */
    cl_patch_jump(c, jnil_pos);

    /* CL spec: var is NIL during result-form evaluation */
    cl_emit(c, OP_NIL);
    if (var_boxed) {
        cl_emit(c, OP_CELL_SET_LOCAL);
    } else {
        cl_emit(c, OP_STORE);
    }
    cl_emit(c, (uint8_t)var_slot);
    cl_emit(c, OP_POP);

    /* Compile result-form or NIL, store in result_slot */
    c->in_tail = saved_tail;
    if (!CL_NULL_P(cl_cdr(cl_cdr(binding)))) {
        compile_expr(c, result_form);
    } else {
        cl_emit(c, OP_NIL);
    }
    cl_emit(c, OP_STORE);
    cl_emit(c, (uint8_t)result_slot);
    cl_emit(c, OP_POP);

    emit_loop_epilogue(c, bi, result_slot, saved_local_count, saved_block_count);

    CL_GC_UNPROTECT(5);  /* form, binding, body, var, result_form */
}

void compile_dotimes(CL_Compiler *c, CL_Obj form)
{
    /* (dotimes (var count-form [result-form]) body...) */
    CL_Obj binding = cl_car(cl_cdr(form));
    CL_Obj body = cl_cdr(cl_cdr(form));
    CL_Obj var = cl_car(binding);
    CL_Obj count_form = cl_car(cl_cdr(binding));
    CL_Obj result_form = cl_car(cl_cdr(cl_cdr(binding)));
    CL_CompEnv *env = c->env;
    int saved_local_count = env->local_count;
    int saved_tail = c->in_tail;
    int saved_block_count = c->block_count;
    int var_slot, limit_slot, result_slot;
    int loop_start, jtrue_pos;
    int var_boxed = 0;
    CL_BlockInfo *bi;

    /* GC SAFETY: form/binding/body/var/result_form are sub-cells of the
     * (protected) enclosing form, but these C locals hold raw arena offsets
     * that the compactor does NOT rewrite.  They are re-read after the
     * allocating compile_expr(count_form) and the tagbody compile below;
     * without protection a compaction relocates the dotimes form and leaves
     * these locals pointing at stale (eventually overwritten) memory, so a
     * later cl_cdr(binding) faults with "CDR: argument is not of type LIST". */
    CL_GC_PROTECT(form);
    CL_GC_PROTECT(binding);
    CL_GC_PROTECT(body);
    CL_GC_PROTECT(var);
    CL_GC_PROTECT(result_form);

    c->in_tail = 0;

    /* Allocate limit slot FIRST — count-form must be evaluated before
     * the loop variable is bound (CL spec). */
    limit_slot = env->local_count;
    env->locals[limit_slot] = CL_NIL;
    env->local_count++;
    if (env->local_count > env->max_locals)
        env->max_locals = env->local_count;

    /* Compile count-form BEFORE adding var to env.
     * This ensures (dotimes (x (f x)) ...) evaluates (f x) using
     * the OUTER binding of x, not the new loop variable. */
    compile_expr(c, count_form);
    cl_emit(c, OP_STORE);
    cl_emit(c, (uint8_t)limit_slot);
    cl_emit(c, OP_POP);

    /* NOW allocate var slot — after count-form is compiled */
    var_slot = cl_env_add_local(env, var);

    /* Check if loop var is captured in body (always mutated by loop).
     * GC SAFETY: `cur` is re-read after scan_body_for_boxing, which
     * macroexpands body macros (e.g. INCF) in the VM and so allocates/
     * compacts.  Protect the cursor — otherwise cl_cdr(stale cur) walks
     * garbage and the captured/mutated scan derails (a body gensym then
     * compiles to a global → "Unbound variable: NEW<n>"). */
    {
        uint8_t captured = 0, mutated = 0;
        CL_Obj cur = body;
        CL_GC_PROTECT(cur);
        while (CL_CONS_P(cur)) {
            scan_body_for_boxing(cl_car(cur), &var, 1, &mutated, &captured, 0);
            cur = cl_cdr(cur);
        }
        if (!CL_NULL_P(cl_cdr(cl_cdr(binding))))
            scan_body_for_boxing(result_form, &var, 1, &mutated, &captured, 0);
        var_boxed = captured;
        CL_GC_UNPROTECT(1);
    }

    /* Allocate result slot + push implicit block NIL */
    result_slot = push_loop_nil_block(c, &bi);

    /* var = 0 (boxed: wrap in cell) */
    cl_emit_const(c, CL_MAKE_FIXNUM(0));
    if (var_boxed) {
        cl_emit(c, OP_MAKE_CELL);
        env->boxed[var_slot] = 1;
    }
    cl_emit(c, OP_STORE);
    cl_emit(c, (uint8_t)var_slot);
    cl_emit(c, OP_POP);

    /* loop_start: LOAD var, LOAD limit, GE, JTRUE -> end */
    loop_start = c->code_pos;
    cl_emit(c, OP_LOAD);
    cl_emit(c, (uint8_t)var_slot);
    if (var_boxed) cl_emit(c, OP_CELL_REF);
    cl_emit(c, OP_LOAD);
    cl_emit(c, (uint8_t)limit_slot);
    cl_emit(c, OP_GE);
    jtrue_pos = cl_emit_jump(c, OP_JTRUE);

    /* CLHS 3.6: the body of DOTIMES is an implicit TAGBODY. */
    compile_implicit_tagbody_body(c, body);

    /* Increment: var = var + 1 */
    cl_emit(c, OP_LOAD);
    cl_emit(c, (uint8_t)var_slot);
    if (var_boxed) cl_emit(c, OP_CELL_REF);
    cl_emit_const(c, CL_MAKE_FIXNUM(1));
    cl_emit(c, OP_ADD);
    if (var_boxed) {
        cl_emit(c, OP_CELL_SET_LOCAL);
    } else {
        cl_emit(c, OP_STORE);
    }
    cl_emit(c, (uint8_t)var_slot);
    cl_emit(c, OP_POP);

    /* Backward jump to loop_start */
    cl_emit_loop_jump(c, OP_JMP, loop_start);

    /* end: */
    cl_patch_jump(c, jtrue_pos);

    /* Compile result-form or NIL, store in result_slot */
    c->in_tail = saved_tail;
    if (!CL_NULL_P(cl_cdr(cl_cdr(binding)))) {
        compile_expr(c, result_form);
    } else {
        cl_emit(c, OP_NIL);
    }
    cl_emit(c, OP_STORE);
    cl_emit(c, (uint8_t)result_slot);
    cl_emit(c, OP_POP);

    emit_loop_epilogue(c, bi, result_slot, saved_local_count, saved_block_count);

    CL_GC_UNPROTECT(5);  /* form, binding, body, var, result_form */
}

/* Shared implementation of DO (sequential=0) and DO* (sequential=1).
 * The two forms differ only in their init and step phases:
 *   - DO evaluates all inits in parallel (onto the stack, then stores
 *     back-to-front) and steps in parallel; DO* binds/steps sequentially.
 * Everything else — binding parse, boxing pre-scan, implicit block NIL,
 * end-test loop, implicit tagbody body, result forms — is identical.
 * (Mirrors compile_let(c, form, sequential) for LET/LET*.) */
static void compile_do_impl(CL_Compiler *c, CL_Obj form, int sequential)
{
    /* (do  ((var init [step])...) (end-test result...) body...)
     * (do* ((var init [step])...) (end-test result...) body...) */
    CL_Obj var_clauses = cl_car(cl_cdr(form));
    CL_Obj end_clause = cl_car(cl_cdr(cl_cdr(form)));
    CL_Obj body = cl_cdr(cl_cdr(cl_cdr(form)));
    CL_Obj end_test = cl_car(end_clause);
    CL_Obj result_forms = cl_cdr(end_clause);
    CL_CompEnv *env = c->env;
    int saved_local_count = env->local_count;
    int saved_tail = c->in_tail;
    int saved_block_count = c->block_count;

    /* Parse bindings into arrays */
    CL_Obj vars[CL_MAX_BINDINGS];
    CL_Obj inits[CL_MAX_BINDINGS];
    CL_Obj steps[CL_MAX_BINDINGS];  /* CL_NIL if no step form */
    int has_step[CL_MAX_BINDINGS];
    uint8_t do_boxed[CL_MAX_BINDINGS];
    int n = 0;
    int i;
    int loop_start, jtrue_pos;
    int result_slot;
    CL_BlockInfo *bi;

    {
        CL_Obj vc = var_clauses;
        while (!CL_NULL_P(vc) && n < CL_MAX_BINDINGS) {
            CL_Obj clause = cl_car(vc);
            /* CLHS 6.1.1: a var-spec may be a bare symbol (var with no
             * init/step, bound to NIL) or a list (var [init [step]]). */
            if (CL_CONS_P(clause)) {
                vars[n] = cl_car(clause);
                inits[n] = cl_car(cl_cdr(clause));
                if (!CL_NULL_P(cl_cdr(cl_cdr(clause)))) {
                    steps[n] = cl_car(cl_cdr(cl_cdr(clause)));
                    has_step[n] = 1;
                } else {
                    steps[n] = CL_NIL;
                    has_step[n] = 0;
                }
            } else {
                vars[n] = clause;
                inits[n] = CL_NIL;
                steps[n] = CL_NIL;
                has_step[n] = 0;
            }
            n++;
            vc = cl_cdr(vc);
        }
    }

    /* GC-protect BEFORE the boxing pre-scan — scan_body_for_boxing
     * macroexpands in the VM (allocates, can compact), and compile_expr
     * below compacts too, making inits[]/vars[]/steps[]/end_test/
     * result_forms/body stale.  var_clauses is re-walked by each cursor. */
    CL_GC_PROTECT(var_clauses);
    CL_GC_PROTECT(body);
    CL_GC_PROTECT(end_test);
    CL_GC_PROTECT(result_forms);

    /* Check which do vars are captured (all stepped vars are mutated by loop) */
    {
        uint8_t mutated[CL_MAX_BINDINGS], captured[CL_MAX_BINDINGS];
        CL_Obj cur;
        memset(mutated, 0, (size_t)n);
        memset(captured, 0, (size_t)n);
        memset(do_boxed, 0, (size_t)n);
        /* Root vars[] and steps[] across the scans: find_var_index compares
         * vars[] by identity, and steps[i] is scanned after earlier scans may
         * have compacted (same discipline as determine_boxed_vars). */
        for (i = 0; i < n; i++)
            CL_GC_PROTECT(vars[i]);
        for (i = 0; i < n; i++)
            CL_GC_PROTECT(steps[i]);
        /* Scan body, end-test, result forms, and step forms — with a
         * protected cursor (each scan can compact and stale the cursor). */
        cur = body;
        CL_GC_PROTECT(cur);
        while (CL_CONS_P(cur)) {
            scan_body_for_boxing(cl_car(cur), vars, n, mutated, captured, 0);
            cur = cl_cdr(cur);
        }
        scan_body_for_boxing(end_test, vars, n, mutated, captured, 0);
        cur = result_forms;
        while (CL_CONS_P(cur)) {
            scan_body_for_boxing(cl_car(cur), vars, n, mutated, captured, 0);
            cur = cl_cdr(cur);
        }
        CL_GC_UNPROTECT(1);  /* cur */
        for (i = 0; i < n; i++) {
            scan_body_for_boxing(steps[i], vars, n, mutated, captured, 0);
        }
        cl_gc_pop_roots(2 * n);  /* vars[], steps[] */
        /* Stepped vars are always mutated; all do vars with step forms need boxing if captured */
        for (i = 0; i < n; i++) {
            do_boxed[i] = (has_step[i] && captured[i]) ? 1 :
                          (mutated[i] && captured[i]) ? 1 : 0;
        }
    }

    c->in_tail = 0;

    if (sequential) {
        /* DO*: sequential init — evaluate and store each var immediately.
         * Walk var_clauses with a protected cursor — compile_expr can compact
         * inits[]/vars[] stale. */
        CL_Obj vc = var_clauses;
        CL_GC_PROTECT(vc);
        for (i = 0; i < n; i++) {
            CL_Obj clause = cl_car(vc);  /* bare symbol => init NIL, sym is var */
            compile_expr(c, CL_CONS_P(clause) ? cl_car(cl_cdr(clause)) : CL_NIL);
            /* Re-read var from protected cursor after compile_expr may have compacted */
            clause = cl_car(vc);
            cl_env_add_local(env, CL_CONS_P(clause) ? cl_car(clause) : clause);
            if (do_boxed[i]) {
                cl_emit(c, OP_MAKE_CELL);
                env->boxed[saved_local_count + i] = 1;
            }
            cl_emit(c, OP_STORE);
            cl_emit(c, (uint8_t)(saved_local_count + i));
            cl_emit(c, OP_POP);
            vc = cl_cdr(vc);
        }
        CL_GC_UNPROTECT(1);
    } else {
        /* DO: parallel init — evaluate all init forms onto stack.
         * Walk var_clauses with a protected cursor — compile_expr can compact
         * inits[] stale. */
        {
            CL_Obj vc = var_clauses;
            CL_GC_PROTECT(vc);
            for (i = 0; i < n; i++) {
                CL_Obj clause = cl_car(vc);  /* bare symbol => init is NIL */
                compile_expr(c, CL_CONS_P(clause) ? cl_car(cl_cdr(clause)) : CL_NIL);
                vc = cl_cdr(vc);
            }
            CL_GC_UNPROTECT(1);
        }

        /* Register all vars as locals — re-walk var_clauses; compile_expr above made vars[] stale */
        {
            CL_Obj vc = var_clauses;
            for (i = 0; i < n; i++) {
                CL_Obj clause = cl_car(vc);  /* bare symbol => the symbol is the var */
                cl_env_add_local(env, CL_CONS_P(clause) ? cl_car(clause) : clause);
                vc = cl_cdr(vc);
            }
        }

        /* Store back-to-front */
        for (i = n - 1; i >= 0; i--) {
            cl_emit(c, OP_STORE);
            cl_emit(c, (uint8_t)(saved_local_count + i));
            cl_emit(c, OP_POP);
        }

        /* Box vars that need it */
        for (i = 0; i < n; i++) {
            if (do_boxed[i]) {
                cl_emit(c, OP_LOAD);
                cl_emit(c, (uint8_t)(saved_local_count + i));
                cl_emit(c, OP_MAKE_CELL);
                cl_emit(c, OP_STORE);
                cl_emit(c, (uint8_t)(saved_local_count + i));
                cl_emit(c, OP_POP);
                env->boxed[saved_local_count + i] = 1;
            }
        }
    }

    /* Allocate result slot + push implicit block NIL */
    result_slot = push_loop_nil_block(c, &bi);

    /* loop_start: compile end-test, JTRUE -> end */
    loop_start = c->code_pos;
    compile_expr(c, end_test);
    jtrue_pos = cl_emit_jump(c, OP_JTRUE);

    /* Compile body as an implicit tagbody per CLHS 6.1.6. */
    compile_implicit_tagbody_body(c, body);

    if (sequential) {
        /* DO*: sequential step — evaluate and store each immediately.
         * Walk var_clauses with a protected cursor — compile_expr can compact
         * steps[] stale. */
        CL_Obj vc = var_clauses;
        CL_GC_PROTECT(vc);
        for (i = 0; i < n; i++) {
            if (has_step[i]) {
                CL_Obj clause = cl_car(vc);
                compile_expr(c, cl_car(cl_cdr(cl_cdr(clause))));
                if (do_boxed[i]) {
                    cl_emit(c, OP_CELL_SET_LOCAL);
                } else {
                    cl_emit(c, OP_STORE);
                }
                cl_emit(c, (uint8_t)(saved_local_count + i));
                cl_emit(c, OP_POP);
            }
            vc = cl_cdr(vc);
        }
        CL_GC_UNPROTECT(1);
    } else {
        /* DO: parallel step — evaluate all step forms (or load current value).
         * Walk var_clauses with a protected cursor — compile_expr can compact
         * steps[] stale. */
        {
            CL_Obj vc = var_clauses;
            CL_GC_PROTECT(vc);
            for (i = 0; i < n; i++) {
                if (has_step[i]) {
                    CL_Obj clause = cl_car(vc);
                    compile_expr(c, cl_car(cl_cdr(cl_cdr(clause))));
                } else {
                    cl_emit(c, OP_LOAD);
                    cl_emit(c, (uint8_t)(saved_local_count + i));
                    if (do_boxed[i]) cl_emit(c, OP_CELL_REF);
                }
                vc = cl_cdr(vc);
            }
            CL_GC_UNPROTECT(1);
        }

        /* Store all back-to-front */
        for (i = n - 1; i >= 0; i--) {
            if (do_boxed[i]) {
                cl_emit(c, OP_CELL_SET_LOCAL);
            } else {
                cl_emit(c, OP_STORE);
            }
            cl_emit(c, (uint8_t)(saved_local_count + i));
            cl_emit(c, OP_POP);
        }
    }

    /* Backward jump to loop_start */
    cl_emit_loop_jump(c, OP_JMP, loop_start);

    /* end: */
    cl_patch_jump(c, jtrue_pos);

    /* Compile result forms as progn, or NIL, store in result_slot */
    c->in_tail = saved_tail;
    if (!CL_NULL_P(result_forms)) {
        compile_progn(c, result_forms);
    } else {
        cl_emit(c, OP_NIL);
    }
    cl_emit(c, OP_STORE);
    cl_emit(c, (uint8_t)result_slot);
    cl_emit(c, OP_POP);

    CL_GC_UNPROTECT(4);  /* var_clauses, body, end_test, result_forms */

    emit_loop_epilogue(c, bi, result_slot, saved_local_count, saved_block_count);
}

void compile_do(CL_Compiler *c, CL_Obj form)
{
    /* (do ((var init [step])...) (end-test result...) body...) */
    compile_do_impl(c, form, 0);
}

void compile_do_star(CL_Compiler *c, CL_Obj form)
{
    /* (do* ((var init [step])...) (end-test result...) body...)
     * Like do, but bindings and steps are sequential. */
    compile_do_impl(c, form, 1);
}

/* --- handler-bind --- */

CL_Obj compile_handler_bind(CL_Compiler *c, CL_Obj form)
{
    /* (handler-bind ((type handler-expr) ...) body...)
     *
     * Trampoline-aware: prelude pushes each handler, body's tail form
     * returns to the driver, postlude emits OP_HANDLER_POP <count>.
     *
     * CLHS 9.1.4.1: when a condition is signalled, applicable handlers of
     * a handler-bind are tried in the order they are written (top to
     * bottom).  cl_signal_condition walks the flat handler stack from the
     * top down (newest first, so inner handler-binds take precedence), so
     * to make a single handler-bind's clauses fire in textual order we must
     * push them so the textually-FIRST clause ends up on top — i.e. push in
     * reverse textual order.  (Handler expressions are normally lambda /
     * #'function literals with no side effects, so the reversed evaluation
     * order of the clause expressions is not observable.) */
    CL_Obj clauses = cl_car(cl_cdr(form));
    CL_Obj body = cl_cdr(cl_cdr(form));
    CL_Obj rev = CL_NIL;
    int count = 0;
    CL_Obj cl;
    CL_TailFrame *tf;

    /* Build a reverse-order copy of the clause list, then push from it so
     * the first textual clause lands on top of the handler stack.  Protect
     * body (used after the loop), rev, and the cursor cl — cl_cons (build
     * loop) and compile_expr (push loop) can compact, making bare offset
     * locals stale.  Re-read type_sym after compile_expr. */
    CL_GC_PROTECT(body);
    CL_GC_PROTECT(rev);
    cl = clauses;
    CL_GC_PROTECT(cl);
    for (; !CL_NULL_P(cl); cl = cl_cdr(cl))
        rev = cl_cons(cl_car(cl), rev);

    cl = rev;
    while (!CL_NULL_P(cl)) {
        CL_Obj clause      = cl_car(cl);
        CL_Obj handler_expr = cl_car(cl_cdr(clause));
        int type_idx;

        compile_expr(c, handler_expr);   /* Push handler closure on VM stack */
        /* Re-read type_sym after compile_expr — compaction may have moved it */
        type_idx = cl_add_constant(c, cl_car(cl_car(cl)));
        cl_emit(c, OP_HANDLER_PUSH);
        cl_emit_u16(c, (uint16_t)type_idx);
        count++;
        cl = cl_cdr(cl);
    }
    CL_GC_UNPROTECT(3); /* cl, rev, body */

    tf = cl_tail_push(c);
    tf->kind = CL_TAIL_HANDLER_BIND;
    tf->saved_macro_count = count;  /* reused: handler count for HANDLER_POP */
    /* Disable tail calls inside the body — the body must return to the
     * OP_HANDLER_POP postlude so the pushed handler bindings are popped.
     * Without this, a handler-bind whose body's tail form is a call (the
     * extremely common `(defun f () (handler-bind (...) (g)))` shape) emits
     * OP_TAILCALL, which replaces the frame and skips OP_HANDLER_POP, leaking
     * one binding per call until the 64-slot handler stack overflows.  Mirrors
     * the CL_TAIL_BLOCK_NLX handling of NLX blocks above. */
    tf->saved_tail = c->in_tail;
    c->in_tail = 0;
    return compile_body_tail(c, body);
}

/* --- restart-case --- */

/* Parse the leading :report / :interactive / :test options of a restart
 * clause (CLHS 9.1).  Stores each supplied option expression through the
 * out-params (CL_UNBOUND = not supplied) and returns the remaining forms
 * (the restart body). */
static CL_Obj parse_restart_options(CL_Obj forms, CL_Obj *report,
                                    CL_Obj *interactive, CL_Obj *test)
{
    static CL_Obj KW_REPORT = CL_NIL;
    static CL_Obj KW_INTERACTIVE = CL_NIL;
    static CL_Obj KW_TEST = CL_NIL;

    if (CL_NULL_P(KW_REPORT)) {
        /* cl_intern_keyword allocates on first call; protect `forms` so the
         * caller's clause tree isn't stale when we walk it after returning. */
        CL_GC_PROTECT(forms);
        KW_REPORT = cl_intern_keyword("REPORT", 6);
        KW_INTERACTIVE = cl_intern_keyword("INTERACTIVE", 11);
        KW_TEST = cl_intern_keyword("TEST", 4);
        CL_GC_UNPROTECT(1);
        /* These interned keywords live in the arena and are relocated by the
         * compacting GC; register the caches so the compactor forwards them.
         * Otherwise a post-compaction `head == KW_*` compares against a stale
         * offset and silently fails to parse :REPORT/:INTERACTIVE/:TEST. */
        cl_gc_register_root(&KW_REPORT);
        cl_gc_register_root(&KW_INTERACTIVE);
        cl_gc_register_root(&KW_TEST);
    }

    *report = CL_UNBOUND;
    *interactive = CL_UNBOUND;
    *test = CL_UNBOUND;

    while (!CL_NULL_P(forms)) {
        CL_Obj head = cl_car(forms);
        CL_Obj *slot;
        if (head == KW_REPORT)           slot = report;
        else if (head == KW_INTERACTIVE) slot = interactive;
        else if (head == KW_TEST)        slot = test;
        else break;
        /* keyword + its value */
        forms = cl_cdr(forms);
        if (!CL_NULL_P(forms)) {
            *slot = cl_car(forms);
            forms = cl_cdr(forms);
        }
    }
    return forms;
}

/* Push a restart option onto the stack for OP_RESTART_PUSH.
 *  - not supplied            -> NIL
 *  - :report "string"        -> the literal string (a report string)
 *  - any other expression    -> (function <expr>), i.e. a report/interactive/
 *                               test function (CLHS accepts a symbol or
 *                               lambda expression acceptable to FUNCTION). */
static void compile_restart_option(CL_Compiler *c, CL_Obj expr, int allow_string)
{
    if (expr == CL_UNBOUND) {
        cl_emit_const(c, CL_NIL);
    } else if (allow_string && CL_ANY_STRING_P(expr)) {
        cl_emit_const(c, expr);
    } else {
        CL_Obj inner, fn_form;
        CL_GC_PROTECT(expr);
        inner = cl_cons(expr, CL_NIL);
        CL_GC_PROTECT(inner);
        fn_form = cl_cons(SYM_FUNCTION, inner);
        CL_GC_UNPROTECT(1); /* inner */
        CL_GC_PROTECT(fn_form);
        compile_expr(c, fn_form);
        CL_GC_UNPROTECT(2); /* expr, fn_form */
    }
}

void compile_restart_case(CL_Compiler *c, CL_Obj form)
{
    /* (restart-case form (name (params...) [:report str] body...)...) */
    CL_Obj main_form = cl_car(cl_cdr(form));
    CL_Obj clauses = cl_cdr(cl_cdr(form));
    CL_CompEnv *env = c->env;
    int saved_local_count = env->local_count;
    int saved_tail = c->in_tail;
    int count = 0;
    int catch_pos, jmp_pos;
    int dispatch_slot;
    CL_Obj cl_iter;
    CL_Obj catch_tag;

    c->in_tail = 0;

    /* GC SAFETY: protect main_form and clauses *before* the catch-frame setup
     * below.  cl_cons (catch tag) and cl_emit_const both allocate and can
     * trigger a compacting GC, which would leave these freshly-read offsets
     * stale.  This bites when restart-case arrives via a macro expansion (a
     * fresh arena tree only reachable through the driver's protected `expr`,
     * not through `form` here) under heap pressure: a later cl_cdr then walks
     * pre-compaction garbage ("CDR: argument is not of type LIST" at compile
     * time).  Protecting up front (rather than at 2731 as before) closes the
     * window. */
    CL_GC_PROTECT(main_form);
    CL_GC_PROTECT(clauses);

    /* Reserve a local slot for the dispatch cons at the catch landing.
     * Must be allocated before main_form compiles (same pattern as
     * compile_unwind_protect) so nested forms don't reuse this index. */
    dispatch_slot = env->local_count;
    env->locals[dispatch_slot] = CL_NIL;
    env->local_count++;
    if (env->local_count > env->max_locals)
        env->max_locals = env->local_count;

    /* Generate unique catch tag (a fresh cons cell) */
    catch_tag = cl_cons(CL_NIL, CL_NIL);
    CL_GC_PROTECT(catch_tag);  /* used in every iteration after compile_expr */

    /* Set up catch frame with the unique tag */
    cl_emit_const(c, catch_tag);  /* push tag */
    cl_emit(c, OP_CATCH);
    catch_pos = c->code_pos;
    cl_emit_i32(c, 0);  /* placeholder for landing offset */

    /* Push restart bindings: for each clause, compile lambda + push tag + OP_RESTART_PUSH.
     * Protect the cursor — compile_expr/cl_cons inside the loop can compact,
     * making bare locals stale.  Re-read restart_name after allocating calls. */
    cl_iter = clauses;
    CL_GC_PROTECT(cl_iter);    /* loop cursor */
    while (!CL_NULL_P(cl_iter)) {
        CL_Obj clause = cl_car(cl_iter);
        CL_Obj params = cl_car(cl_cdr(clause));
        CL_Obj report = CL_UNBOUND, interactive = CL_UNBOUND, test = CL_UNBOUND;
        CL_Obj clause_body = parse_restart_options(cl_cdr(cl_cdr(clause)),
                                                   &report, &interactive, &test);
        CL_Obj lambda_form;
        int name_idx;

        /* Re-read params from the protected cl_iter: parse_restart_options
         * may have triggered a compacting GC (keyword intern on first call),
         * leaving the pre-call `params` value stale. */
        params = cl_car(cl_cdr(cl_car(cl_iter)));

        /* Protect the option expressions across the allocating compile
         * calls below — compaction is a moving GC. */
        CL_GC_PROTECT(report);
        CL_GC_PROTECT(interactive);
        CL_GC_PROTECT(test);

        /* Build (lambda (params...) body...) and compile it */
        lambda_form = cl_list_star3(SYM_LAMBDA, params, clause_body);
        compile_expr(c, lambda_form);  /* pushes closure (handler) on stack */

        /* Push the restart's :report / :interactive / :test operands, then
         * the catch tag.  OP_RESTART_PUSH pops them in reverse. */
        compile_restart_option(c, report, 1);       /* report may be a string */
        compile_restart_option(c, interactive, 0);
        compile_restart_option(c, test, 0);
        cl_emit_const(c, catch_tag);
        CL_GC_UNPROTECT(3);

        /* OP_RESTART_PUSH: pops tag/test/interactive/report/handler, builds
         * the first-class restart object, pushes the restart binding */
        /* Re-read restart_name after allocating calls — compaction may have moved it */
        name_idx = cl_add_constant(c, cl_car(cl_car(cl_iter)));
        cl_emit(c, OP_RESTART_PUSH);
        cl_emit_u16(c, (uint16_t)name_idx);
        count++;
        cl_iter = cl_cdr(cl_iter);
    }
    CL_GC_UNPROTECT(4); /* cl_iter, catch_tag, clauses, main_form */

    /* Compile the main form */
    compile_expr(c, main_form);

    /* Normal exit: pop restart bindings, uncatch, jump past landing */
    cl_emit(c, OP_RESTART_POP);
    cl_emit(c, (uint8_t)count);
    cl_emit(c, OP_UNCATCH);
    jmp_pos = cl_emit_jump(c, OP_JMP);

    /* [landing]: invoke-restart threw (handler . args-list) cons here.
     * Unwind-protect cleanups have already run; now apply the handler in
     * the dynamic environment captured at restart establishment (CLHS). */
    cl_patch_jump(c, catch_pos);
    cl_emit(c, OP_DUP);
    cl_emit(c, OP_CDR);                         /* TOS = args-list, below = cons */
    cl_emit(c, OP_STORE);
    cl_emit(c, (uint8_t)dispatch_slot);         /* save args-list (peeks, no pop) */
    cl_emit(c, OP_POP);                         /* discard args-list copy */
    cl_emit(c, OP_CAR);                         /* TOS = handler */
    cl_emit(c, OP_LOAD);
    cl_emit(c, (uint8_t)dispatch_slot);         /* push args-list */
    cl_emit(c, OP_APPLY);                       /* result on stack */
    c->mv_state = CL_MV_MANY;                   /* the handler's values */

    /* [past_landing]: both paths converge with one value on stack */
    cl_patch_jump(c, jmp_pos);

    cl_env_clear_boxed(env, saved_local_count);
    env->local_count = saved_local_count;
    c->in_tail = saved_tail;
}

/* --- Macrolet --- */

/* Build the macrolet local-macro expanders for BINDINGS (each
 * (name lambda-list . body)) and install them into ENV via
 * cl_env_add_local_macro.  Factored out of compile_macrolet so compile-file's
 * top-level processor can reconstruct a macrolet's lexical macro environment
 * and process its body forms as top-level forms (CLHS 3.2.3.1). */
void cl_macrolet_install_expanders(CL_CompEnv *env, CL_Obj bindings)
{
    /* Compile each macro expander at compile time */
    {
        CL_Obj b = bindings;
        CL_GC_PROTECT(b);
        while (!CL_NULL_P(b)) {
            CL_Obj binding = cl_car(b);
            CL_Obj mname = cl_car(binding);
            CL_Obj lambda_list = cl_car(cl_cdr(binding));
            CL_Obj mbody = cl_cdr(cl_cdr(binding));
            CL_Obj lambda_form, bytecode, closure;

            /* GC SAFETY: mname is held until cl_env_add_local_macro below, which
             * is reached only AFTER cl_compile + cl_vm_eval (both allocate and
             * can compact).  Without protection the macro would be registered
             * under a stale/garbage name offset after a compaction — the local
             * macrolet expander could then not be found (treated as an undefined
             * function) or resolve to the wrong symbol. */
            CL_GC_PROTECT(mname);
            CL_GC_PROTECT(lambda_list);
            CL_GC_PROTECT(mbody);

            /* Strip &environment (same as defmacro).  The env-var is
             * bound at the top of the body to the current lexical env,
             * captured by the caller and read via %MACROEXPAND-ENV.
             *
             * IMPORTANT: rebuild the lambda list NON-DESTRUCTIVELY.  The
             * macrolet binding's lambda list is shared source structure and
             * cl_macrolet_install_expanders runs more than once on it: the
             * boxing pre-scan (scan_body_for_boxing) installs the expanders
             * to see through macrolet calls, then the real compile_macrolet
             * installs them again.  Splicing &environment out by mutating a
             * prior cons's cdr would permanently drop it from the source, so
             * the 2nd install would never re-bind the env var and the
             * expander body's reference to it became "Unbound variable: ENV"
             * (e.g. ironclad sha3.lisp's STATE-AREF macrolets, whose
             * &environment is not the first lambda-list element). */
            {
                CL_Obj env_var = CL_NIL;
                int found_env = 0;
                CL_Obj cur2 = lambda_list;
                while (!CL_NULL_P(cur2)) {
                    if (cl_car(cur2) == SYM_AMP_ENVIRONMENT) {
                        CL_Obj next2 = cl_cdr(cur2);
                        env_var = CL_NULL_P(next2) ? CL_NIL : cl_car(next2);
                        found_env = 1;
                        break;
                    }
                    cur2 = cl_cdr(cur2);
                }
                if (found_env) {
                    /* Copy lambda_list, omitting "&environment <var>".
                     * cl_cons below can compact, so GC-protect the walk
                     * cursor cur2 (points into the protected lambda_list, but
                     * is a separate C local that would otherwise go stale)
                     * along with the list being built. */
                    CL_Obj new_ll = CL_NIL, new_tail = CL_NIL;
                    CL_GC_PROTECT(env_var);
                    CL_GC_PROTECT(new_ll);
                    CL_GC_PROTECT(new_tail);
                    CL_GC_PROTECT(cur2);
                    cur2 = lambda_list;
                    while (!CL_NULL_P(cur2)) {
                        CL_Obj p = cl_car(cur2);
                        if (p == SYM_AMP_ENVIRONMENT) {
                            cur2 = cl_cdr(cur2);              /* skip &environment */
                            if (!CL_NULL_P(cur2))
                                cur2 = cl_cdr(cur2);         /* skip the env var */
                            continue;
                        }
                        {
                            CL_Obj cell = cl_cons(p, CL_NIL);
                            if (CL_NULL_P(new_ll)) new_ll = cell;
                            else ((CL_Cons *)CL_OBJ_TO_PTR(new_tail))->cdr = cell;
                            new_tail = cell;
                        }
                        cur2 = cl_cdr(cur2);
                    }
                    lambda_list = new_ll;
                    if (CL_SYMBOL_P(env_var)) {
                        /* (let ((env-var (%macroexpand-env))) . mbody), one
                         * allocation per statement (mem.h, cl_list2) */
                        CL_Obj f = cl_intern_in("%MACROEXPAND-ENV", 16,
                                                cl_package_amilisp);
                        f = cl_cons(f, CL_NIL);
                        f = cl_list2(env_var, f);
                        f = cl_cons(f, CL_NIL);
                        f = cl_list_star3(SYM_LET, f, mbody);
                        mbody = cl_cons(f, CL_NIL);
                    }
                    CL_GC_UNPROTECT(4); /* cur2, new_tail, new_ll, env_var */
                }
            }

            /* Handle &whole (same as defmacro) */
            {
                CL_Obj whole_var = CL_NIL;
                if (!CL_NULL_P(lambda_list) && cl_car(lambda_list) == SYM_AMP_WHOLE) {
                    CL_Obj rest2 = cl_cdr(lambda_list);
                    if (!CL_NULL_P(rest2)) {
                        whole_var = cl_car(rest2);
                        lambda_list = cl_cdr(rest2);
                    }
                }
                if (CL_NULL_P(whole_var)) {
                    whole_var = defmacro_gensym();
                }
                lambda_list = cl_cons(whole_var, lambda_list);
            }

            /* Transform destructuring lambda lists (same as defmacro) */
            if (defmacro_needs_destructuring(lambda_list)) {
                CL_Obj new_ll = CL_NIL, new_ll_tail = CL_NIL;
                CL_Obj cur = lambda_list;
                int in_opt_key = 0;

                CL_GC_PROTECT(new_ll);
                CL_GC_PROTECT(new_ll_tail);
                CL_GC_PROTECT(cur);     /* walks on across the conses below */

                while (!CL_NULL_P(cur)) {
                    CL_Obj param = cl_car(cur);
                    CL_Obj cell;

                    if (CL_SYMBOL_P(param) &&
                        (param == SYM_AMP_OPTIONAL || param == SYM_AMP_KEY)) {
                        in_opt_key = 1;
                        cell = cl_cons(param, CL_NIL);
                        if (CL_NULL_P(new_ll)) { new_ll = cell; }
                        else { ((CL_Cons *)CL_OBJ_TO_PTR(new_ll_tail))->cdr = cell; }
                        new_ll_tail = cell;
                        cur = cl_cdr(cur);
                        continue;
                    }

                    if (!in_opt_key && CL_CONS_P(param)) {
                        CL_Obj gs = defmacro_gensym();
                        CL_Obj db_form;
                        CL_GC_PROTECT(gs);
                        /* (destructuring-bind param gs . mbody); param is
                         * re-read, the gensym may have moved it */
                        db_form = cl_cons(gs, mbody);
                        db_form = cl_list_star3(SYM_DESTRUCTURING_BIND,
                                                cl_car(cur), db_form);
                        mbody = cl_cons(db_form, CL_NIL);
                        cell = cl_cons(gs, CL_NIL);
                        if (CL_NULL_P(new_ll)) { new_ll = cell; }
                        else { ((CL_Cons *)CL_OBJ_TO_PTR(new_ll_tail))->cdr = cell; }
                        new_ll_tail = cell;
                        CL_GC_UNPROTECT(1); /* gs */
                    } else if (CL_SYMBOL_P(param) &&
                               (param == SYM_AMP_BODY || param == SYM_AMP_REST)) {
                        CL_Obj next_param = CL_NULL_P(cl_cdr(cur)) ? CL_NIL : cl_car(cl_cdr(cur));
                        in_opt_key = 0;
                        if (CL_CONS_P(next_param)) {
                            CL_Obj gs = defmacro_gensym();
                            CL_Obj db_form;
                            CL_GC_PROTECT(gs);
                            /* next_param re-read: the gensym may move it */
                            db_form = cl_cons(gs, mbody);
                            db_form = cl_list_star3(SYM_DESTRUCTURING_BIND,
                                                    cl_car(cl_cdr(cur)),
                                                    db_form);
                            mbody = cl_cons(db_form, CL_NIL);
                            cell = cl_cons(cl_car(cur), CL_NIL); /* &body/&rest */
                            if (CL_NULL_P(new_ll)) { new_ll = cell; }
                            else { ((CL_Cons *)CL_OBJ_TO_PTR(new_ll_tail))->cdr = cell; }
                            new_ll_tail = cell;
                            cell = cl_cons(gs, CL_NIL);
                            ((CL_Cons *)CL_OBJ_TO_PTR(new_ll_tail))->cdr = cell;
                            new_ll_tail = cell;
                            cur = cl_cdr(cur); /* skip the list pattern */
                            CL_GC_UNPROTECT(1); /* gs */
                        } else {
                            cell = cl_cons(param, CL_NIL);
                            if (CL_NULL_P(new_ll)) { new_ll = cell; }
                            else { ((CL_Cons *)CL_OBJ_TO_PTR(new_ll_tail))->cdr = cell; }
                            new_ll_tail = cell;
                        }
                    } else {
                        cell = cl_cons(param, CL_NIL);
                        if (CL_NULL_P(new_ll)) { new_ll = cell; }
                        else { ((CL_Cons *)CL_OBJ_TO_PTR(new_ll_tail))->cdr = cell; }
                        new_ll_tail = cell;
                    }
                    cur = cl_cdr(cur);
                }

                lambda_list = new_ll;
                CL_GC_UNPROTECT(3); /* cur, new_ll, new_ll_tail */
            }

            /* Build (lambda (params) body...) */
            lambda_form = cl_list_star3(SYM_LAMBDA, lambda_list, mbody);
            CL_GC_PROTECT(lambda_form);

            /* Wrap in 2-arg (form environment) trampoline — matches
             * compile_defmacro so local-macrolet expanders have the
             * same shape and can be invoked uniformly. */
            {
                CL_Obj inner_gs = defmacro_gensym();
                CL_Obj form_gs, env_gs, cme_sym;
                CL_Obj inner_call, outer_ll, outer_lambda;
                CL_Obj let_binding, let_bindings, wrap_form;
                CL_GC_PROTECT(inner_gs);
                form_gs = defmacro_gensym();
                CL_GC_PROTECT(form_gs);
                env_gs = defmacro_gensym();
                CL_GC_PROTECT(env_gs);
                cme_sym = cl_intern_in("%CALL-MACRO-EXPANDER", 20,
                                       cl_package_amilisp);
                inner_call = cl_cons(env_gs, CL_NIL);
                inner_call = cl_cons(form_gs, inner_call);
                inner_call = cl_cons(inner_gs, inner_call);
                inner_call = cl_cons(cme_sym, inner_call);
                CL_GC_PROTECT(inner_call);
                outer_ll = cl_cons(env_gs, CL_NIL);
                outer_ll = cl_cons(form_gs, outer_ll);
                CL_GC_PROTECT(outer_ll);
                outer_lambda = cl_cons(inner_call, CL_NIL);
                outer_lambda = cl_cons(outer_ll, outer_lambda);
                outer_lambda = cl_cons(SYM_LAMBDA, outer_lambda);
                CL_GC_PROTECT(outer_lambda);
                let_binding = cl_cons(lambda_form, CL_NIL);
                let_binding = cl_cons(inner_gs, let_binding);
                CL_GC_PROTECT(let_binding);
                let_bindings = cl_cons(let_binding, CL_NIL);
                CL_GC_PROTECT(let_bindings);
                wrap_form = cl_cons(outer_lambda, CL_NIL);
                wrap_form = cl_cons(let_bindings, wrap_form);
                wrap_form = cl_cons(SYM_LET, wrap_form);
                CL_GC_PROTECT(wrap_form);

                /* Compile and evaluate at compile time to get closure */
                bytecode = cl_compile(wrap_form);
                CL_GC_PROTECT(bytecode);
                closure = cl_vm_eval(bytecode);
                CL_GC_UNPROTECT(10); /* bytecode + 9 wrapper temps */
            }
            CL_GC_UNPROTECT(3); /* lambda_form, mbody, lambda_list */

            cl_env_add_local_macro(env, mname, closure);
            CL_GC_UNPROTECT(1); /* mname */

            b = cl_cdr(b);
        }
        CL_GC_UNPROTECT(1);
    }
}

CL_Obj compile_macrolet(CL_Compiler *c, CL_Obj form)
{
    /* (macrolet ((name (params) body...) ...) body...) */
    CL_Obj bindings = cl_car(cl_cdr(form));
    CL_Obj body;
    CL_CompEnv *env = c->env;
    int saved_macro_count = env->local_macro_count;
    CL_TailFrame *tf;

    /* GC SAFETY: install_expanders compiles+evals each expander (allocates,
     * can compact); protect `form` and derive `body` only afterwards. */
    CL_GC_PROTECT(form);
    cl_macrolet_install_expanders(env, bindings);
    body = cl_cdr(cl_cdr(form));
    CL_GC_UNPROTECT(1);

    /* Trampoline body — postlude restores local_macro_count and notinline_count. */
    tf = cl_tail_push(c);
    tf->kind = CL_TAIL_MACROLET;
    tf->saved_macro_count = saved_macro_count;
    tf->saved_notinline_count = c->notinline_count;
    return compile_body_tail(c, body);
}

/* --- Symbol-macrolet --- */

CL_Obj compile_symbol_macrolet(CL_Compiler *c, CL_Obj form)
{
    /* (symbol-macrolet ((sym expansion) ...) body...) — register each
     * symbol macro, push a tail frame to restore the count after the
     * body chain drains, and return the body's tail form. */
    CL_Obj bindings = cl_car(cl_cdr(form));
    CL_Obj body = cl_cdr(cl_cdr(form));
    CL_CompEnv *env = c->env;
    int saved_symbol_macro_count = env->symbol_macro_count;
    CL_TailFrame *tf;

    {
        CL_Obj b = bindings;
        while (!CL_NULL_P(b)) {
            CL_Obj binding = cl_car(b);
            CL_Obj sym = cl_car(binding);
            CL_Obj expansion = cl_car(cl_cdr(binding));
            cl_env_add_symbol_macro(env, sym, expansion);
            b = cl_cdr(b);
        }
    }

    tf = cl_tail_push(c);
    tf->kind = CL_TAIL_SYMBOL_MACROLET;
    tf->saved_macro_count = saved_symbol_macro_count;
    tf->saved_notinline_count = c->notinline_count;
    return compile_body_tail(c, body);
}

/* --- PROGV --- */

CL_Obj compile_progv(CL_Compiler *c, CL_Obj form)
{
    /* (progv symbols-form values-form body...) — emit BIND prelude,
     * trampoline the body, postlude emits UNBIND. */
    CL_Obj symbols_form = cl_car(cl_cdr(form));
    CL_Obj values_form;
    CL_Obj body;
    int saved_tail = c->in_tail;
    CL_TailFrame *tf;

    c->in_tail = 0;
    /* GC SAFETY: each compile_expr allocates and can compact; protect `form`
     * and re-derive values_form/body after the preceding compile. */
    CL_GC_PROTECT(form);
    compile_expr(c, symbols_form);
    values_form = cl_car(cl_cdr(cl_cdr(form)));
    compile_expr(c, values_form);
    body = cl_cdr(cl_cdr(cl_cdr(form)));
    CL_GC_UNPROTECT(1);
    cl_emit(c, OP_PROGV_BIND);

    tf = cl_tail_push(c);
    tf->kind = CL_TAIL_PROGV;
    tf->saved_tail = saved_tail;
    return compile_body_tail(c, body);
}
