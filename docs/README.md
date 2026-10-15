# AmiLisp Package Reference

AmiLisp ships a number of packages beyond the standard `COMMON-LISP` /
`COMMON-LISP-USER`. These add platform extensions, threading, a foreign-function
interface, the Gray-streams protocol, the CLOS Metaobject Protocol, and the
AmigaOS GUI bindings.

`COMMON-LISP-USER` already `:use`s `EXT`, `MP`, `FFI`, `AMILISP`, and `MOP` (and
`AMIGA` on AmigaOS), so most of these symbols are available unqualified at the
REPL. The pages below document each package; the **package prefix** column is the
qualified name you would use from a package that does not inherit it.

| Package | Prefix | What it provides | Doc |
|---------|--------|------------------|-----|
| `EXT` | `ext:` | amilisp extensions — TCP sockets, GC control, environment access, exit hooks, debug/introspection | [ext.md](ext.md) |
| `MP` | `mp:` | Multiprocessing — threads, locks, condition variables, memory barriers | [mp.md](mp.md) |
| `FFI` | `ffi:` | Foreign-function interface — foreign pointers, typed peek/poke, libffi calls & callbacks | [ffi.md](ffi.md) |
| `GRAY` | `gray:` (nick `...`) | Gray-streams protocol — define your own stream classes in Lisp | [gray.md](gray.md) |
| `MOP` | `mop:` | CLOS Metaobject Protocol (AMOP / closer-mop subset) | [mop.md](mop.md) |
| `AMILISP` | `amilisp:` | Implementation extras — IEEE float bits, package-local nicknames, JIT/trace toggles | [amilisp.md](amilisp.md) |
| `AMIGA`, `AMIGA.*` | `amiga:`, `amiga.intuition:` … | AmigaOS bindings — raw library calls, FFI tag lists, Intuition, Graphics, GadTools | [amiga.md](amiga.md) |

Each page lists the exported symbols and points at the runnable test file that is
the authoritative, always-current specification for that package's behavior.

## Keeping the lists in sync

The symbol lists here are derived from the real exports — live from a running
image (`do-external-symbols`) for the host packages, and from source for the
AmigaOS packages. A committed snapshot lives in
[`package-symbols.txt`](package-symbols.txt), and `make docs-check` fails if the
real exports drift from it (or if `amilisp.md` references a AMILISP symbol that
is no longer exported — that curated set is
[`amilisp-documented-symbols.txt`](amilisp-documented-symbols.txt)).

```
make docs-check     # verify docs are in sync with the real exports (CI/pre-commit)
make docs-update    # regenerate package-symbols.txt after editing the prose
```

When a package's exports change: update the affected `*.md` prose (and, for a new
user-facing AMILISP symbol, `amilisp-documented-symbols.txt`), then run
`make docs-update`. The machinery lives in `tools/docs/`.

The `.guide` files of the binary release are generated from these pages by
`make guide` (`tools/docs/md2guide.lisp`); `make test` fails on Markdown the
converter does not handle, and on a link to a heading that does not exist.
