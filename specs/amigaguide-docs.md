# AmigaGuide documentation: Markdown to `.guide` converter

Status: IMPLEMENTED (2026-09-14; the record of what was built and verified
is at the end, under "Implementation record")
Date: 2026-09-07

## Goal

Ship the user documentation in AmigaGuide format next to the Markdown, so
that it can be read on the Amiga itself with MultiView (AmigaOS 3.x,
MorphOS) instead of only on GitHub or a host machine.  The `.guide` files
are generated from the Markdown at build/release time and are never
edited by hand, so they cannot drift from `docs/*.md` and `README.md`.

## Why an own converter

No usable Markdown to AmigaGuide tool exists (surveyed 2026-09-07):

- `betajaen/Md2Guide` (C#, .NET Core 3.1, two commits, 2020): no tables
  (our reference pages are ~600 table rows), ordinary links are dropped
  (only its own `guide://` syntax links), one node per file (no section
  nodes, so anchor links have no target), output forced to ASCII (every
  em dash becomes `?`).  Fixing all of that means forking C# and adding
  an end-of-life .NET runtime to the release path.
- `milos-p-lab/MarkdownGuideHtmlConverter` (C#, active): AmigaGuide to
  HTML/Markdown only, no `.guide` writer.
- pandoc: no AmigaGuide writer; a custom Lua writer is the same work as
  our own converter plus a pandoc dependency (not installed).
- Aminet: nothing in `text/hyper`.

Our documentation uses a narrow Markdown subset (see Scope), so a
purpose-built converter stays small.  It is written in Common Lisp and
run by amilisp itself, like `tools/docs/dump-package-symbols.lisp`: no
new host dependency, and it runs on the Amiga as well, which makes it a
real-target test of the reader, string and stream code.

## Scope

### Inputs

The Markdown files the binary release ships (the layout of 2026-09-15;
originally every guide went to `docs/`, see the implementation record):

| Source | Output | Role |
|--------|--------|------|
| `README-FIRST.md` | `README-FIRST.guide` | Getting started: what is where, the binaries, the heap image, Clamacs and the ARexx port |
| `README.md` | `amilisp.guide` | The user manual (features, usage, GUI, limitations) |
| `clamacs/README.md` | `clamacs.guide` | The Clamacs editor/IDE (when the submodule is checked out) |
| `docs/README.md` | `docs/README.guide` | Package reference index |
| `docs/ext.md` | `docs/ext.guide` | `EXT` reference |
| `docs/mp.md` | `docs/mp.guide` | `MP` reference |
| `docs/ffi.md` | `docs/ffi.guide` | `FFI` reference |
| `docs/gray.md` | `docs/gray.guide` | `GRAY` reference |
| `docs/mop.md` | `docs/mop.guide` | `MOP` reference |
| `docs/amilisp.md` | `docs/amilisp.guide` | `AMILISP` reference |
| `docs/amiga.md` | `docs/amiga.guide` | `AMIGA.*` reference |

The output paths are the release layout: the three guides a user opens
first sit in the package root next to the Workbench icons, the reference
under `docs/` next to its Markdown.  Every guide gets a Workbench icon
(`icons/Guide.info`, default tool `SYS:Utilities/MultiView`, copied as
`<name>.guide.info` by the release script), so the documentation is
visible and readable from Workbench.  The top-level README is named
`amilisp.guide` rather than `README.guide` (taken by the index) or
`clamiga.guide` (taken by the `AMILISP` package page; AmigaDOS file names
are case-insensitive).  `README-FIRST.md` lives in the repository root so
that its links resolve identically in the checkout and in the release
(same relative position); it replaces the `README-BINARY.txt` the release
script used to generate.

Not converted: `docs/benchmarks.md`, `docs/sento-bench-results-*.md`,
`specs/*.md`, `docs/*.txt`, `examples/**/README.md`.  Links into them
degrade to plain text (see Links).

### Markdown subset

What the nine files actually use, measured 2026-09-07, and therefore
what the converter must support:

- ATX headings level 1 to 3 (no level 4+ today; level 4+ renders as a
  bold line inside the enclosing node).
- Paragraphs, hard-wrapped in the source.
- Inline: `` `code` ``, `**bold**`, `*italic*`, `[text](target)`,
  `![alt](image)` (README only), bare URLs.
- Fenced code blocks (```` ``` ````, with or without a language tag).
- Bullet lists (`-`, `*`), ordered lists (`1.`), one level of nesting
  (README).
- Block quotes (`>`), used as notes.
- Pipe tables with a header row and a `|---|` separator; cells contain
  inline markup, links and `\|` for a literal pipe (two rows in
  `docs/amiga.md`); descriptions run to 700 characters.
- Thematic breaks (`---` after a blank line; `docs/amiga.md`).
- Trailing/leading blank lines, HTML comments: none today.

Everything else is **an error, not a silent passthrough**: setext
headings, reference-style links, footnotes, raw HTML blocks, indented
(non-fenced) code blocks, task lists, definition lists.  The converter
prints `file:line: unsupported: <what>` and exits non-zero, so a new
construct in the docs breaks `make test` instead of producing a garbled
guide.  Support is added when a construct is actually needed.

## Target format

AmigaGuide as documented in the Aminet `text/hyper/amigaguidedocs`
reference (V33 to V40).  The converter emits only V39 commands plus the
node/link/escape basics, so the output renders identically on
`amigaguide.library` V39 (AmigaOS 3.0), V40 (3.1, the FS-UAE harness),
3.5/3.9, MorphOS, and AmigaGuideJS.  No V40 attributes (`@{code}`,
`@{par}`, `@{lindent}`, `@SMARTWRAP`) are used; see Wrapping.

### File skeleton

```
@DATABASE ext.guide
@$VER: ext.guide 0.9 (07.09.2026)
@AUTHOR Manfred Bergmann
@(C) Copyright 2026 Manfred Bergmann, Apache-2.0
@WIDTH 78
@REM generated by tools/docs/md2guide.lisp from docs/ext.md -- do not edit

@NODE main "EXT - AmiLisp Extensions"
@{b}@{u}EXT - AmiLisp Extensions@{uu}@{ub}

<paragraphs of the level-1 section>

@{b}Contents@{ub}

  @{" TCP networking " LINK "tcp-networking"}
  @{" TLS " LINK "tls"}
  ...
@ENDNODE

@NODE tcp-networking "TCP networking"
@{b}@{u}TCP networking@{uu}@{ub}

...
@ENDNODE
```

- `@DATABASE` must be the first line of the file.
- `@$VER:` uses the version and date reported by the running amilisp
  (`lisp-implementation-version`, `0.9.0` today, and the build date), in
  Amiga `$VER` form so the AmigaDOS `Version` command reads it.
- `@WIDTH 78` matches the 76-column body plus indent (see Wrapping).
- `@TOC` is not emitted: the Contents button defaults to node `main`.
- `@INDEX` is emitted only when an index node exists (phase 2).

### Character set and escaping

AmigaOS text is ISO-8859-1.  The source files are UTF-8 and contain
about 500 characters outside Latin-1 (424 em dashes, ellipses, arrows,
math symbols).  The converter maps them:

| Code point | Char | Output |
|-----------|------|--------|
| U+2014 | — | `--` |
| U+2013 | – | `-` |
| U+2026 | … | `...` |
| U+2192 | → | `->` |
| U+2190 | ← | `<-` |
| U+2194 | ↔ | `<->` |
| U+21C4 | ⇄ | `<->` |
| U+2265 | ≥ | `>=` |
| U+2264 | ≤ | `<=` |
| U+2261 | ≡ | `==` |
| U+2260 | ≠ | `/=` |
| U+201C U+201D | “ ” | `"` |
| U+2018 U+2019 | ‘ ’ | `'` |
| U+00A0 | nbsp | space |
| U+1F642 | 🙂 | `:-)` |

Latin-1 characters (`×`, `§`, umlauts) pass through and are written as
their single Latin-1 byte.  Any other character above U+007F is an error
naming the file, line and code point; the table grows when the docs do.
Output is written with amilisp's `:latin-1` external format (or as raw
bytes); input is read as characters, the runtime decodes UTF-8.

AmigaGuide escaping applies to every character of body text, including
code blocks: `@` becomes `\@` and `\` becomes `\\`.  Only the
converter's own commands contain unescaped `@`.

### Wrapping

The converter lays out the text itself: paragraphs are re-flowed to 76
columns, list items and definition entries get hanging indents, code
blocks are emitted verbatim.  Neither `@WORDWRAP` nor `@SMARTWRAP` is
used.  This is the conventional AmigaGuide look (every Aminet guide),
renders the same in every viewer version, and keeps the tables-as-
definition-entries layout deterministic.  The cost is no reflow on wide
RTG screens; adding `@SMARTWRAP` later is an isolated change to the
paragraph emitter and can be evaluated once the fixed layout is in
MultiView.

## Conversion rules

### Headings and nodes

- The level-1 heading is the document title; its text (inline code
  markers stripped) is the `main` node title and the first line of the
  node.  Everything before the first level-2 heading is the body of
  `main`, followed by the Contents list.
- **Every level-2 and level-3 heading becomes a node.**  Node name =
  the GitHub anchor slug of the heading text: lower-case, backticks and
  all characters other than letters, digits, space, hyphen and
  underscore removed, spaces to hyphens, duplicates suffixed `-1`,
  `-2`.  This keeps every existing `page.md#anchor` link valid without
  editing the Markdown.  Node title = heading text with inline code
  markers stripped and `"` replaced by `'`.
- A level-2 node's body is the content up to its first level-3 heading;
  it ends with a `Sections` list linking its level-3 children, so the
  children stay reachable.  Level-3 nodes contain their content to the
  next level-2 or level-3 heading.
- Level 4+ headings (none today) are a bold line inside the current
  node.
- Nodes are emitted in document order, so MultiView's Browse buttons
  walk the document without `@NEXT`/`@PREV`.
- The Contents list in `main` links every level-2 node, with its
  level-3 nodes indented two spaces beneath it.

### Paragraphs and inline markup

- Consecutive non-blank lines form a paragraph; it is re-flowed to 76
  columns.  Two spaces at line end (hard break) are not used in the
  docs and are an error.
- `**bold**` becomes `@{b}...@{ub}`; `*italic*` becomes `@{i}...@{ui}`.
- `` `code` `` is emitted as plain text: AmigaGuide text is monospace
  already, and colouring it (`@{fg fill}`) is a one-line toggle to
  judge in the rendering probe, off by default.
- Bare URLs stay as text.

### Links

| Markdown target | Output |
|-----------------|--------|
| `#anchor` | `@{"text" LINK "anchor"}` |
| `page.md#anchor`, `page.md` (a sibling) | `@{"text" LINK "page.guide/anchor"}`, `.../main` |
| `docs/page.md#anchor` (from the root) | `@{"text" LINK "docs/page.guide/anchor"}` |
| `../README.md#anchor` (from `docs/`) | `@{"text" LINK "/amilisp.guide/anchor"}` |
| a file that is not converted (`benchmarks.md`, `../specs/x.md`, `tests/...`) | `text (path)` as plain text |
| `http://`, `https://` | `text (url)` as plain text |

A target is resolved against the source layout (the linking file's
directory plus the target path, normalized) and looked up in the input
set; the link is then emitted as the **path of the target guide relative
to the linking guide's directory** in the output layout, in AmigaDOS
spelling: a sibling is its bare name, a guide below is `dir/name`, and
each step up is one leading `/` (`..` means nothing to AmigaDOS).
amigaguide.library resolves the file part of a link against the
directory of the document the link is in, so this is what makes the
root/`docs/` layout work in both directions (verified on a Vampire V4,
AmigaOS 3.2.3, MultiView 47: `sub/child.guide/below` from the root and
`/fixture.guide/tables` from the subdirectory both open the right node).
The converter converts the whole set in one run, so it resolves every
anchor against the parsed heading set of the target file; **a dangling
link is an error**, same-file or cross-file.  Link text inside table
cells and list items is supported.  MultiView cannot open URLs on stock
3.1, so URLs stay text.

### Images

`![alt](path)` (five README screenshots, not shipped in the release)
emits the alt text as an italic line.  No datatype link is attempted.

### Code blocks

Fenced blocks are emitted verbatim (escaped), indented two spaces, with
a blank line before and after.  Lines longer than 76 columns are left
alone (MultiView clips, the source is authoritative).  The language tag
is ignored.

### Lists

- Bullet items: `- ` marker, continuation lines indented to align with
  the text (hanging indent), re-flowed to 76 columns.  Nested items
  indent two more columns.
- Ordered items: the number as written (`1.`), same hanging indent.
- A blank line separates the list from surrounding paragraphs; items
  are not blank-line separated.

### Block quotes

Rendered as a paragraph indented by four columns.  The `**Bold:**` lead
that the README notes use keeps its bold.

### Tables

A pipe table becomes one definition entry per body row:

- The first cell is emitted in bold, wrapped with a hanging indent.
- A column whose header is `Kind` is folded into the first line as
  `[kind]` after the bold text.
- A column whose header is `Description` follows on the next lines,
  indented four columns, without a label.
- Every other column follows as `Header: cell`, indented four columns.
- Rows are separated by a blank line; the header row itself is not
  printed.
- The cell splitter treats `\|` as a literal pipe, emitted as `|`.
  A body row whose cell count differs from the header's is an error.

Input (`docs/ext.md`):

```
| Signature | Kind | Description |
|-----------|------|-------------|
| `(socket-listen port &optional loopback)` | function | Open a listening server socket on a port (port `0` picks a free one; `loopback` non-`nil` binds 127.0.0.1 only) |
```

Output:

```
@{b}(socket-listen port &optional loopback)@{ub}  [function]
    Open a listening server socket on a port (port 0 picks a free one;
    loopback non-nil binds 127.0.0.1 only)
```

The signature tables of the reference pages, the package index table in
`docs/README.md` (`Package | Prefix | What it provides | Doc`, where the
`Doc` cell is a link) and the option/benchmark tables in the README all
fall out of this one rule.  Column layout is not attempted: the longest
description cells are several hundred characters and a MultiView window
on a 640-pixel screen shows 77 columns.

### Thematic breaks

`---` on its own line after a blank line emits nothing (sections are
nodes already).  `---` or `===` directly under a text line is a setext
heading and therefore an error.

## Tooling and build integration

- `tools/docs/md2guide.lisp`: the converter, plain CL, no dependencies
  beyond what boot provides.  Entry point
  `(md2guide:convert-set inputs output-dir)` where `inputs` is a list of
  `(source-path . guide-name)`; a single-file `convert` for tests and
  ad-hoc use.  Reads the whole file, parses into a block list, resolves
  links across the set, then emits.  Diagnostics are `file:line:
  message` on standard error, exit code 1 on any error.
- `tools/docs/md2guide.sh <amilisp-binary> <output-dir>`: wraps the
  above for the fixed input set, invoked like `package-symbols.sh`
  (`AMILISP_NO_USERINIT=1 ... --non-interactive --load ... --eval ...`).
  `<output-dir>` is laid out as the release root (the root guides at the
  top, the reference under `docs/`; the converter creates the directory).
- `make guide` (Makefile, next to `docs-check`/`docs-update`): builds
  that layout under `build/guide/` with the host binary.  Generated files
  are not committed.
- `scripts/make-binary-release.sh`: after copying `README-FIRST.md`,
  `README.md` and `docs/*.md` into the stage, runs the converter into the
  stage root, then copies `icons/Guide.info` next to every `.guide` as its
  `.info`; the smoke test checks that each guide exists at its release
  path, starts with `@DATABASE`, has its icon, and that the root guides
  link down into `docs/` and the `AMIGA.*` reference links back up.
- Windows/MSYS2 host: the script is POSIX sh and the converter runs in
  amilisp, so it works there unchanged; paths handed to amilisp go
  through `tests/shpath.sh`'s `native_path` where the shell test
  compares them.

## Tests

Both host and Amiga, per project rule.

1. `tests/test_md2guide.sh` (registered with the other shell tests in
   `make test`):
   - **Golden fixture**: `tests/md2guide/fixture.md` exercises every
     supported construct (all heading levels, duplicate headings,
     inline markup nested in cells and items, every link form, a
     `Kind`/`Description` table, a generic table, nested lists, quotes,
     code with `@` and `\`, every mapped character, a Latin-1 pass-
     through).  Output must be byte-identical to
     `tests/md2guide/fixture.guide` (committed as Latin-1 bytes; the
     `.gitattributes` LF rule applies, encoding does not matter to git).
   - **Error fixtures**: one file per unsupported construct
     (`setext.md`, `refstyle-link.md`, `html-block.md`, `dangling-
     link.md`, `unmapped-char.md`); each must fail with the expected
     `file:line:` diagnostic and produce no output file.
   - **Real docs**: convert the nine shipped files into a temp dir; must
     succeed with zero diagnostics.  This is the drift gate: a new
     construct or a bad link in `docs/` fails `make test`.
   - **Structural checks** over the real output, in the shell script so
     they are independent of the converter: first line is `@DATABASE`;
     `@NODE`/`@ENDNODE` balance; every `LINK "x"` and `LINK "f/x"`
     names an existing `@NODE` in that file; no byte above 0x7F other
     than the Latin-1 pass-through set; no unescaped `@` outside a
     command (`@` not preceded by `\` and not followed by `{`, `NODE`,
     ...).
2. `tests/amiga/run-tests.lisp`: load `tools/docs/md2guide.lisp` from
   the mounted repo volume, convert `tests/md2guide/fixture.md` to `T:`,
   compare byte-for-byte with the committed `fixture.guide`.  Proves the
   converter is portable Lisp and gives the real-target run.  The
   fixture, not the nine docs, to keep suite time flat.
3. `make test-gc-stress`: run the fixture conversion under
   `AMILISP_GC_STRESS=1` (one line in the existing target list).  The
   converter allocates strings and conses heavily; this is cheap
   coverage of those paths, even though no C changes are involved.

## Verification on target

Step 0 of the implementation is a **rendering probe**: a hand-written
`probe.guide` containing one instance of every emitted construct
(bold/underline title, hanging-indent list, definition entry, code
block with `\@`, same-file link, cross-file link, Latin-1 character,
`@$VER`), opened in MultiView under FS-UAE (Workbench 3.1, the
`test-amiga` setup, `amigaguide.datatype` present in the system tree).
It settles the questions the reference does not answer: exact
underline/bold rendering of the title, whether inline code should be
coloured, and how clipped long code lines look.  Then the same file on
the Vampire (AmigaOS 3.x, via the `vamp` MCP tools) and on MorphOS
(`mos` MCP tools, MultiView with its AmigaGuide viewer), screenshots
kept under `build/amiga/shots/`.

Acceptance: `docs/amilisp.guide` and `docs/ext.guide` open in
MultiView on all three, every Contents link jumps, `Version
docs/ext.guide` prints the version string, and a cross-file link from
the index reaches the `EXT` page.

## Documentation updates

- `README.md`: one paragraph in the documentation section: the binary
  release ships the reference and this README as AmigaGuide files under
  `docs/`, built by `make guide`; point at `tests/md2guide/fixture.md`
  and `fixture.guide` as the executable example of the mapping.
- `docs/README.md`: one line that the `.guide` files are generated from
  these pages, and that `make test` fails on Markdown the converter
  does not handle.
- `scripts/make-binary-release.sh` header comment: `docs/` now lists
  `*.guide`.

## Non-goals (this iteration)

- Full CommonMark.  The converter is strict on purpose.
- `@SMARTWRAP` reflow: evaluate after the fixed layout is in MultiView.
- An `@INDEX` node listing every signature alphabetically with a link
  to its node.  Natural phase 2 for the reference pages; the table
  parser already has the signatures.
- Converting `specs/`, benchmarks, or example READMEs.
- Shipping the converter as a `lib/` module.  It stays a tool under
  `tools/docs/`; if users ask for on-Amiga Markdown conversion it can
  move later.
- Images as datatype links.

## Implementation order

1. Rendering probe in MultiView (FS-UAE); fix the open rendering
   choices in this spec.
2. `md2guide.lisp`: block parser (headings, paragraphs, fences, lists,
   quotes, tables, breaks) with strict errors; inline parser; slugs;
   link resolution across a set; character map; emitter with wrapping.
3. Golden fixture and error fixtures; `tests/test_md2guide.sh`; the
   real-docs conversion and structural checks; register in `make test`.
4. Amiga suite entry and gc-stress line.
5. `md2guide.sh`, `make guide`, release-script wiring, README-BINARY.
6. Convert the real docs, open them on FS-UAE, Vampire and MorphOS,
   record the acceptance run here.
7. README and `docs/README.md` updates.

Estimated size: converter 400 to 500 lines of Lisp, fixtures and shell
test 200 lines, wiring under 50 lines.

## Decisions taken (override if wanted)

- One `.guide` per `.md`, same basename; the manual is `amilisp.guide`.
  Since 2026-09-15 the three guides a user opens first
  (`README-FIRST.guide`, `amilisp.guide`, `clamacs.guide`) sit in the
  package root next to the Workbench icons, the reference under `docs/`,
  and every guide has an icon (originally all under `docs/`, no icons).
- Level-2 and level-3 headings are nodes; node names are the GitHub
  anchor slugs.
- Fixed 76-column layout, no `@SMARTWRAP`.
- Tables become definition entries; no column layout.
- Inline code is plain text (monospace already).
- Unsupported Markdown is an error; the real docs are converted in
  `make test`.
- Generated guides are not committed; they are built by `make guide`
  and by the release script.

## Implementation record (2026-09-14)

Built as specified, with these outcomes and deviations:

- **Rendering probe** (`build/amiga/probe.guide`, MultiView on the OS 3.9
  Workbench of the FS-UAE `test-amiga` setup, `amigaguide.datatype` 44):
  the bold+underlined title, the hanging-indent lists, the definition
  entries, the four-column quote and the two-space code block all render
  as laid out; `\@` and `\\` in body text show as `@` and `\`; a link
  button's quoted label is taken **literally** (a `\\` in it shows both
  backslashes), so labels are written unescaped with `"` replaced by `'`;
  Latin-1 characters render; a code line past the window's width runs
  off the right edge without wrapping.  `@{fg fill}` colours inline code
  on this datatype but is a V40 attribute -- inline code stays plain.
- **Character mapping**: mapped characters travel through parsing as
  placeholder characters in the unused C1 range (`#x80` + index into
  `*char-map*`) and are spelled out only on output.  Slugs and column
  widths therefore see the original text: GitHub's anchor for
  `Fixture — the mapping` is `fixture--the-mapping`, and mapping the em
  dash to `--` first would have produced `fixture----the-mapping`.
- **Input decoding** is the converter's own: files are read as Latin-1
  bytes and UTF-8 is decoded in Lisp, so the output is byte-identical on
  the byte-string m68k build (`char-code-limit` 256) and on wide builds.
- **Clamacs**: the editor's `README.md` (from the `clamacs/` submodule)
  is a tenth input, `docs/clamacs.guide`, and ships next to
  `docs/clamacs.md`.  `tools/docs/md2guide.sh` skips it with a note when
  the submodule is not checked out (CI checks out without submodules).
- **Extra strictness** beyond the list above: an unmatched backtick, a
  table row whose cell count differs from the header's, an unterminated
  fence, a lazy (unindented) continuation line in a list, a code fence
  or a second nesting level inside a list, a block inside a block quote,
  and a file that does not start with a level-1 heading are errors.  A
  link whose text equals its target (`[examples/](examples/)`) is emitted
  once, not as `examples/ (examples/)`.
- **Titles** longer than 76 columns wrap like body text (the `AMIGA.*`
  page's is 185 columns).
- **Badges** (an image inside a link, the README's CI status) are dropped
  entirely; a lone italic "CI" at the top of the manual meant nothing.
  Plain images keep their alt text as an italic line.
- **Found in the docs**: three signature rows in `docs/amiga.md` were
  glued to the paragraph above them (GitHub rendered them as text); the
  Clamacs README spelled the `C-x `` ` `` key as a backslash-escaped
  backtick inside a code span.  Both fixed.

Verification:

- `tests/test_md2guide.sh` in `make test`: golden fixture set
  (`tests/md2guide/fixture.md` + `other.md`, for the cross-file links),
  twelve error fixtures under `tests/md2guide/errors/`, the real docs
  through `md2guide.sh`, and the structural checks (plus: the `@$VER:`
  line carries the version from `src/core/types.h`, no unindented body
  line wider than 76 visible columns, the index links every reference
  page).  Also run under `AMILISP_GC_STRESS=1` in `make test-gc-stress`.
- `tests/amiga/md2guide-tests.lisp` in the Amiga suite: the fixture set
  converted on the target is byte-identical to the golden files.
- Acceptance in FS-UAE (OS 3.9 Workbench, 68040 JIT config): the ten
  real pages converted **by the m68k binary itself** in 97 s;
  `docs/amilisp.guide`, `ext.guide` and `clamacs.guide` open in
  MultiView, screenshots under `build/amiga/shots/`.
- Vampire V4 (AmigaOS 3.2.3, Kickstart 47.13, MultiView 47, 1280x720
  RTG), 2026-09-14: the ten guides pushed to `Work:Download/guide-test/`;
  `Version ext.guide` prints `ext.guide 0.9.0`; `amilisp.guide` opens
  with 63 nodes; the Contents link and the in-text "Known Limitations"
  link jump (node 60/63), the Contents button returns, and the Package
  Reference node's `docs/ext.md` link opens `ext.guide` (cross-file,
  1/11).  Screenshots under `build/amiga/shots-guide/vampire/`.
- MorphOS (3.20, MultiView 3.17), 2026-09-15, from the unpacked 0.10.0
  release under `Work:Download/`: `README-FIRST.guide` opens (11 nodes),
  the Contents link to Documentation jumps, and its `docs/README.guide`
  link opens the package reference -- the cross-drawer link works there
  too.  `SYS:Utilities/Multiview` exists on MorphOS (case-insensitive
  file system), so the guide icons' default tool resolves.

## Layout change (2026-09-15): root guides, README-FIRST, icons

Asked for after the 0.10 packaging: the guides a user opens first belong
in the package root next to the Workbench icons, there should be a
getting-started guide, and every guide should have an icon.

- **Output paths carry directories.**  The input set maps
  `README-FIRST.md`, `README.md` and `clamacs/README.md` to
  `README-FIRST.guide`, `amilisp.guide` and `clamacs.guide` in the output
  root and the reference to `docs/*.guide`; `convert-set` creates the
  directories (`ensure-directories-exist`), and `@DATABASE`/`@$VER:` carry
  the bare file name.  `make guide`, the test and the release script all
  produce the same layout, the release script straight into its staged
  root.
- **Cross-file links are directory-relative** (see Links above): the
  path of the target guide relative to the linking guide's directory,
  with AmigaDOS's leading-slash parent.  Verified in both directions on
  the Vampire (MultiView 47, `amigaguide.datatype` of AmigaOS 3.2.3)
  before the layout was changed: the fixture set's `sub/child.guide`
  opened from `Work:Download/guide-layout/`, its "the fixture" link
  (`/fixture.guide/tables`) opened the fixture's Tables node, the
  fixture's "Child" link (`sub/child.guide/below`) opened the child's
  Below node.  The golden fixture set grew a third file,
  `tests/md2guide/sub/child.md`, that links up; `fixture.md` links down
  to it; the Amiga suite converts it into a `T:sub/` the converter
  creates.
- **`README-FIRST.md`** is a committed Markdown file in the repository
  root (the same relative position as in the release, so its links
  resolve in both), converted like the others and gated by `make test`
  like the others.  It carries what `README-BINARY.txt` used to say --
  the layout, the binaries, the stack and heap, the heap image, the
  libraries -- plus the two lines every Clamacs user needs in
  `S:.amilisprc` and why (the editor launches `PROGDIR:amilisp` when no
  port is found, and only the rc file opens the port).  The release
  script no longer writes `README-BINARY.txt`; a `--snapshot` build gets
  a `SNAPSHOT.txt` with the snapshot note instead.
- **Icons**: `scripts/make-icons.py` draws a fourth icon, `icons/Guide.info`
  (the lambda card with the lines of a page and a `DOC` badge), a project
  icon whose default tool is `SYS:Utilities/MultiView`, 16K stack, no tool
  types.  The release script copies it next to every `.guide` (root and
  `docs/`) as `<name>.guide.info`.  Workbench starts the default tool
  with the project's drawer as its current directory, which is also how
  MultiView opened the guides in the Vampire check above.
- `tests/test_md2guide.sh` converts the real docs into the release layout
  and resolves every `LINK` target relative to the linking guide's
  directory the way the library does; it also asserts the cross-layout
  links (README-FIRST to manual/index/editor, manual into `docs/`,
  `AMIGA.*` reference up to the manual) and that `@DATABASE` carries the
  bare name.
- **Root layout and drawer icon** (same day, second request): the six
  root icons carry fixed positions (`do_CurrentX/Y`; launchers in one
  row, the three guides beneath, columns 170 px apart starting at 64 so
  the 144 px "README-FIRST.guide" label is not clipped), and the archives
  carry `amilisp-<version>.info` beside the drawer: a `WBDRAWER` icon
  (`icons/Drawer.info`) whose DrawerData opens the window at 580x160 and
  whose DrawerData2 says "show only files with icons, view by icon".
  Two things learned on the Vampire: `WBDRAWER` is type **2** (1 is
  `WBDISK`; with type 1 Workbench silently used its default drawer icon
  and window), and Workbench keeps a drawer's DiskObject cached while
  the parent window is open, so a replaced `.info` is only seen after
  the parent window is closed and reopened -- a never-opened drawer is
  the reliable test.  icon.library itself (checked with
  `amiga/raw/icon`'s GetDiskObject from amilisp on the box) read every
  variant fine.  Verified: the drawer opens at 580x160 with the two rows
  and the icon-less `README.md`/`LICENSE` hidden.
- **On disk, `struct DrawerData` is split** -- its first 56 bytes
  (NewWindow + dd_CurrentX/Y) right after the 78-byte DiskObject, the
  Image next, and dd_Flags/dd_ViewModes as the last six bytes of the
  file.  That is what every drawer icon AmigaOS writes looks like
  (SYS:Prefs.info on the Vampire, the OS 3.9 Devs.info/Utilities.info
  in `verify/realamiga/aos3`, all dumped), and it is what icon.library
  reads.  The review gate once "corrected" the generator to write the
  62-byte in-memory struct in one block after the image, which
  icon.library rejects (Workbench silently shows its default drawer);
  `tests/test_icons.sh` (in `make test`) now pins the byte layout of
  every shipped icon and checks the generator reproduces the committed
  files.
