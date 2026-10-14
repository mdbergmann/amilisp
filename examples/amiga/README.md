# AmigaOS examples

Runnable programs for the AmigaOS / MorphOS build of AmiLisp.  Each one
is a single file: `amilisp --load <file>` from the repository (or release)
root runs it.  The GUI ones need a 128K stack (`stack 131072`) on
AmigaOS, see the main README.

| Directory | What | Needs |
|-----------|------|-------|
| `gfx/bouncing-lines.lisp` | Bouncing colour-cycling lines in a window — the `AMIGA.INTUITION` / `AMIGA.GFX` curated bindings, an IDCMP event loop, FPS overlay | AmigaOS 3.1+ or MorphOS |
| `gfx/doublebuffer.lisp` | The NDK 3.1 `intuition/doublebuffer.c` example — a face bouncing at the frame rate on a HIRES custom screen through two `ScreenBuffer`s (`AllocScreenBuffer` / `ChangeScreenBuffer`, the `dbi_SafeMessage` reply protocol, the held-off `WaitTOF` retry), an attached control screen (`SA_Parent`) with GadTools sliders, `LendMenus`, two windows on one IDCMP port | AmigaOS 3.0+ (V39) |
| `gfx/sprite.lisp` | The RKM "Simple Sprite" example (`ssprite.c`) — a hardware sprite: `GetSprite` / `ChangeSprite` / `MoveSprite` / `FreeSprite`, the sprite colour registers via `SetRGB4`, image data in CHIP memory, sprite DMA off/on through a custom-chip register write | AmigaOS 3+ on a native chipset (real or FS-UAE) |
| `gfx/screenshot.lisp` | Save the front screen (or the Workbench) as a PPM file — the FS-UAE harness's photographer as a program: `LockPubScreen`, the screen list under `LockIBase`, RTG screens through cybergraphics / Picasso96 `ReadPixelArray` (both opened by hand and called by LVO), chipset screens from their bitplanes in CHIP memory resolved through the ViewPort's `ColorMap`, the file written straight from the foreign buffer | AmigaOS 3+ or MorphOS (cybergraphics.library or Picasso96 for RTG screens) |
| `arexx/` | `amilisp.rexx`, a shell client for the `AMIGA.AREXX` development port, and a CygnusEd macro that saves and loads the current file | ARexx |
| `reaction/` | ReAction GUIs — ports of the NDK 3.2 `Examples/` programs (below) | AmigaOS 3.5+/3.2 or MorphOS (ReAction classes) |
| `mui/` | MUI GUIs — the `AMIGA.MUI` helpers over `muimaster.library` (below) | AmigaOS 3.x with MUI 3.8+, or MorphOS |
| `asyncio/copyfile.lisp` | Double-buffered asynchronous file copy over DOS packets — the `AMIGA.ASYNCIO` port of the NDK 3.1 AsynchIO package, timed against plain synchronous streams and byte-verified | AmigaOS 3.1+ or MorphOS |
| `iff/sift.lisp` | The NDK 3.1 `sift` program as Lisp — `AMIGA.IFF` over iffparse.library builds a nested IFF with back-patched chunk sizes, prints its IFFCheck-like listing (files or the clipboard, the C's `-c`), reads chunk data back | AmigaOS 3.1+ or MorphOS |
| `audio/ahi-play.lisp` | Sound through AHI — the opt-in `AMIGA.AHI` module: lists the audio modes of the installed drivers, plays a tone on the user's unit 0 through the device interface (one request, then two queued gapless at two pitches), then a major chord on three channels of AHI's mixer through the low-level API, no hooks | AHI 4+ installed (AmigaOS 3.x, Aminet `ahiusr`) or MorphOS |

## ReAction (`reaction/`)

Common Lisp ports of the ReAction examples shipped with the AmigaOS 3.2
NDK, written against the generated raw class modules
(`lib/amiga/raw/classes/window`, `gadgets/*`, `images/*`,
`classes/requester`) and the `AMIGA.REACTION` helpers
(`lib/amiga/reaction.lisp`: `RA_OpenWindow` / `RA_HandleInput`, object
creation, over `lib/amiga/boopsi.lisp`'s `DoMethod`, string pools and
label lists, which it re-exports).  Each file names the NDK program it
ports and what it demonstrates.

| Example | NDK source | Shows |
|---------|-----------|-------|
| `buttons.lisp` | `Buttons.c` | window.class + layout.gadget + 100 button.gadget objects, built in a loop; the basic event loop |
| `checkbox.lisp` | `CheckBox.c` | checkbox.gadget, label.image in a bevelled group, an iconifiable window (`WINDOW_AppPort`, `WMHI_ICONIFY` / `WMHI_UNICONIFY`) |
| `chooser.lisp` | `Chooser1.c` | chooser.gadget over a node list (`new-list`, `alloc-chooser-node-a`), `CHILD_Label` |
| `clicktab.lisp` | `ClickTab.c` | clicktab.gadget tabs switching a page.gadget (`PAGE_Current` + `RethinkLayout`) |
| `fuelgauge.lisp` | `FuelGauge.c` | fuelgauge.gadget animated from the program with `set-gadget-attrs`, busy pointer via `set-attrs` |
| `integer.lisp` | `Integer.c` | integer.gadget with/without arrows, `GA_TabCycle`, `ActivateLayoutGadget`, reading `INTEGER_Number` back |
| `listbrowser.lisp` | `ListBrowser1.c` | listbrowser.gadget with three columns (the `struct ColumnInfo` array built through the generated `gadgets/listbrowser` struct accessors), node attributes read back, multi-select / auto-fit toggled live |
| `requester.lisp` | `Requester.c` | requester.class: info, multi-button, string (with chooser presets) and integer requesters through `open-requester` |

Run one interactively:

```
amilisp --load examples/amiga/reaction/listbrowser.lisp
```

Run one unattended — the window closes itself after the timeout (this
is how the examples are smoke-tested):

```
amilisp --eval '(require "amiga/reaction")' \
        --eval '(setf amiga.reaction:*event-loop-timeout* 5)' \
        --load examples/amiga/reaction/listbrowser.lisp
```

On a system without the ReAction classes (the host, a bare AmigaOS 3.1)
each example loads and says so instead of failing.

## MUI (`mui/`)

Common Lisp MUI programs written against the generated raw module
(`lib/amiga/raw/muimaster`: the 30 `muimaster.library` functions and
every `MUIA_` / `MUIM_` / `MUIV_` / `MUIC_` constant of `libraries/mui.h`)
and the `AMIGA.MUI` helpers (`lib/amiga/mui.lisp`: objects by class name,
`MUI_MakeObject`, the `MUIM_Notify` idiom, the `MUIM_Application_NewInput`
loop, over `lib/amiga/boopsi.lisp`'s `DoMethod` and string pools, which
it re-exports).  Most are ports of the demos in the MUI 3.8 developer
kit (`MUI:Developer/C/Examples/`), the hook and custom-class ones
(`Layout.c`, `Slidorama.c`, `Class1.c`) included — their Lisp functions
are called by MUI itself.  `group-layout` and `numeric` are the
attribute-only counterparts of `layout` and `slidorama`, and `hooks`
collects the hook idioms the kit spreads over `psi.c`, `Layout.c` and
`AppWindow.c`.  Each file names its source and what it demonstrates.

| Example | SDK source | Shows |
|---------|-----------|-------|
| `hello.lisp` | — | The minimal MUI application: Application / Window / Group / Text / `MUI_MakeObject` button, `MUIM_Notify` on `MUIA_Window_CloseRequest` and `MUIA_Pressed`, `MUIA_Window_Open`, the event loop |
| `layout.lisp` | `Layout.c` | A custom layout hook: `MUIA_Group_LayoutHook` is a Lisp function (`pool-hook`) answering `MUILM_MINMAX` from the children's `area-min-width` / `area-min-height` with `set-min-max`, and `MUILM_LAYOUT` by scattering `layout-children` at random with `layout-child` (`MUI_Layout`); plus the SDK's button game — `MUIM_CallHook` with a parameter, a reward button shown through `MUIA_ShowMe` |
| `group-layout.lisp` | — | The other side of `layout`: what MUI's own layout engine does with the group attributes — `MUIA_Group_Horiz` / `_Columns` / `_SameSize`, `MUIA_Weight`, every `MUIV_Frame_*`, the `MUII_*` backgrounds, `MUIA_FrameTitle` |
| `balancing.lisp` | `Balancing.c` | `Balance.mui` objects between weighted rectangles, buttons and labels; `MUIA_ObjectID`; `MUIV_Window_Width_Screen(50)` via `window-size-screen` |
| `pages.lisp` | `Pages.c` | `Register.mui` tabs over four pages of string / cycle / radio / checkmark / slider objects, plus a `MUIA_Group_PageMode` group switched by a cycle through `notify` with `MUIV_TriggerValue` |
| `menus.lisp` | `Menus.c` | A menu strip built from `Menustrip` / `Menu` / `Menuitem` objects (shortcuts, checkmark, toggle and radio items, separators), menu selections as return IDs, `MUIA_Application_MenuAction`, `MUIM_FindUData` / `SetUData` / `GetUData`, a menu inserted and removed at run time, `MUI_Request` |
| `showhide.lisp` | `ShowHide.c` | Checkmarks that show and hide buttons through `MUIA_ShowMe` with `MUIV_TriggerValue`; buttons added and removed live with `MUIM_Group_InitChange` / `OM_ADDMEMBER` / `OM_REMMEMBER` / `MUIM_Group_ExitChange` |
| `slidorama.lisp` | `Slidorama.c` | Four custom classes with Lisp dispatchers: a `Levelmeter.mui` subclass that follows the mouse (`OM_NEW` with `GetTagData` on the `opSet`, `request-idcmp` / `reject-idcmp` in `MUIM_Setup` / `MUIM_Cleanup`, `MUIM_HandleInput` over the `IntuiMessage`), a `Slider.mui` whose `MUIM_Numeric_Stringify` prints `:-))` ratings, and one dispatcher shared by a `Slider.mui` and a `Numericbutton.mui` subclass printing `mm:ss`; around them the C's knob table and numeric buttons |
| `numeric.lisp` | — | The notification-only side of `slidorama`: knobs, sliders, numeric buttons, a gauge with its scale and a levelmeter; `MUIA_Numeric_Format`; a slider driving the gauge and the levelmeter by notification |
| `virtual.lisp` | `Virtual.c` | `Scrollgroup.mui` / `Virtgroup.mui` viewports: a long text with the window's border scrollers, the `MUII_*` images and backgrounds, a list filled by `MUIM_List_Insert`, a virtual group inside a virtual group scrolled by arrow buttons, `MUIM_Window_SetCycleChain` |
| `requester.lisp` | — | `MUI_Request`: an information requester, a yes/no question, a three-way choice with a default, and one formatted with `%ld` / `%s` parameters |
| `class1.lisp` | `Class1.c` | A custom class written in Lisp: `create-custom-class` over `Area.mui` with a dispatcher answering `MUIM_AskMinMax` (`add-min-max` after `do-super-method`) and `MUIM_Draw` (a fan of lines through graphics.library inside `area-mleft` … `area-mbottom`, `area-rastport`, the `TEXTPEN` of `area-draw-info`), `new-object` of the class's `custom-class-class` |
| `hooks.lisp` | (`psi.c`, `AppWindow.c`) | `struct Hook`s that call Lisp: a `MUIA_List_DisplayHook` formatting two columns and the title row (`pool-hook`, the builtin String construct / destruct hooks, `MUIM_List_InsertSingle`), buttons whose `MUIA_Pressed` runs a Lisp hook through `MUIM_CallHook` with a parameter, the selection running one with `MUIV_TriggerValue` (`MUIA_List_Active` under a `MUIV_EveryTime` trigger — the only place MUI substitutes it), and a `MUIA_Listview_DoubleClick` running one that asks the list for its active entry with `MUIM_List_GetEntry` |

Run one interactively:

```
amilisp --load examples/amiga/mui/hello.lisp
```

Run one unattended — the window closes itself after the timeout:

```
amilisp --eval '(require "amiga/mui")' \
        --eval '(setf amiga.mui:*event-loop-timeout* 5)' \
        --load examples/amiga/mui/hello.lisp
```

On a system without MUI (the host, an Amiga without `muimaster.library`)
each example loads and says so instead of failing.

## Graphics (`gfx/`)

`bouncing-lines.lisp` uses the curated `AMIGA.INTUITION` / `AMIGA.GFX`
bindings; `doublebuffer.lisp` and `sprite.lisp` are ports of the NDK 3.1
and RKM Companion C examples written against the generated raw bindings
(`AMIGA.RAW.GRAPHICS`, `AMIGA.RAW.INTUITION`, `AMIGA.RAW.EXEC`, plus the
GadTools helpers) and keep the C's structure — each file names its
source and what it demonstrates.  Both open their own custom screen;
the sprite one has to, because a hardware sprite is invisible over an
RTG screen.  `run` returns a plist of what happened (frames swapped,
sprite number and final position), which is what the test suite checks.

`screenshot.lisp` is the photographer of the harness below as a program
of its own: it saves whatever screen is in front — or the Workbench,
`(screenshot:grab :screen :workbench :file "RAM:wb.ppm")` — as a binary
PPM, reading RTG screens through cybergraphics / Picasso96 and chipset
screens from their bitplanes; `grab` returns a plist (`:width :height
:depth :method :bytes`) the test suite checks against the screen, and
`screen-rgb` hands any program the pixels of a screen.

Run one unattended — its loop returns after the timeout:

```
amilisp --eval '(require "amiga/intuition")' \
        --eval '(setf amiga.intuition:*event-loop-timeout* 5)' \
        --load examples/amiga/gfx/doublebuffer.lisp
```

`make -f Makefile.cross examples-amiga` (= `verify/realamiga/run-examples.sh`)
runs all the GUI examples — `gfx/`, `reaction/` and `mui/` — in FS-UAE (whose
OS 3.9 Workbench has the ReAction classes and MUI 3.8), photographs every new window on
the Workbench screen and every custom screen, and converts the shots to
PNG under `build/amiga/shots/`.
