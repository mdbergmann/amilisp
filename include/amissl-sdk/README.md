# AmiSSL SDK headers (vendored)

Header subset of the **AmiSSL 5.27 SDK** — the AmigaOS/MorphOS port of
OpenSSL 3.x as an Amiga shared library:

- Upstream: https://github.com/jens-maus/amissl
- Source archive: `AmiSSL-5.27-SDK.lha` (release 5.27)
- License: **Apache License 2.0** (each header carries the notice;
  full text at https://www.apache.org/licenses/LICENSE-2.0)

Vendored so the AmigaOS/MorphOS cross and native builds compile TLS support
(`-DCL_HAVE_AMISSL`, see `Makefile.cross` / `Makefile.mos`) straight from a
repo checkout, with no SDK download step.  Only headers live here — calls
go through the SDK's `inline/` (m68k gcc) and `ppcinline/` (MorphOS)
register-mapped macros via `AmiSSLBase`, so no link library is needed, and
the library itself (`amisslmaster.library`, AmiSSL 5+) is opened lazily at
runtime.

Kept subdirectories: `amissl/`, `clib/`, `inline/`, `ppcinline/`,
`libraries/`, `openssl/`, `proto/`.  Dropped (unused by AmiLisp):
`inline4/` + `interfaces/` (AmigaOS 4), `defines/` (AROS), `pragmas/`
(SAS/C, vbcc).

To upgrade: fetch the newer SDK archive, replace these directories with the
same subset, rebuild both cross variants and `Makefile.mos`, and re-run
`make -f Makefile.cross test-amiga` (the FS-UAE guest's installed AmiSSL
must be at least as new as the SDK's API version, or `InitAmiSSLMaster`
refuses with "AmiSSL version is too old").
