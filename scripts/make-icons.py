#!/usr/bin/env python3
"""Generate the Workbench icons of the binary release (icons/*.info).

Each icon is a classic (OS 1.3/2.x-style) DiskObject project icon.  Six
have C:IconX as their default tool, so a double-click on Workbench or
Ambient runs the launcher script of the same name next to it -- one
script per binary of the release, no platform detection in the scripts:
`icons/AmiLisp` starts bin/aos3/amilisp in a console window,
`icons/AmiLisp-FPU` the hard-float build, `icons/AmiLisp-MOS` the MorphOS
build; `icons/Clamacs`, `icons/Clamacs-FPU`, `icons/Clamacs-MOS` the
editor on the same three.  The guide icons have SYS:Utilities/MultiView as
their default tool, so a double-click opens the guide: `icons/Guide.info`
is copied next to every reference guide under docs/ by
scripts/make-binary-release.sh, and `README-FIRST.guide.info`,
`amilisp.guide.info`, `clamacs.guide.info` are the same image with a
fixed position for the three guides in the package root.

The package root is a 3 x 3 grid -- the amilisp launchers in the first
column, the Clamacs launchers in the second (soft-float, FPU, MorphOS from
top to bottom), the three guides in the third -- by fixed icon positions
(do_CurrentX/Y) instead of Workbench's own placement, and
`icons/Drawer.info` is the icon OF the package drawer (shipped beside it in
the archive as amilisp-<version>.info): a drawer icon carries the drawer
window's size and position and the "show icons, view by icon" setting,
which is what makes the grid fit and show up as laid out.

Why classic icons: every Workbench from 1.3 on renders a 2-bitplane
DiskObject with the standard 4-colour palette (0 grey, 1 black, 2 white,
3 blue), no icon.library, NewIcons or OS3.5 colour-icon support needed --
and the file is ~500 bytes.  The image is the same 48x24 lambda card for
all of them; a badge tells the FPU and MorphOS builds, the editor and the
guides apart.

    python3 scripts/make-icons.py [OUTDIR]        # default: icons/

Layout written (big-endian, as icon.library reads it):

    struct DiskObject   78 bytes (magic 0xE310, version 1, embedded Gadget)
    struct Image        20 bytes (GadgetRender), then the bitplanes
    default tool        LONG length + NUL-terminated string
    tool types          LONG (n+1)*4, then per entry LONG length + string

Pointer fields in the file are flags only (non-zero = "follows").  Checked
in as binaries because a Python is not part of the release build's
requirements; re-run this after changing the art and commit the .info files.
"""
import os
import struct
import sys

# --- art ------------------------------------------------------------------

W, H = 48, 24

# Colour indices of the standard Workbench 2.x+ 4-colour icon palette.
GREY, BLACK, WHITE, BLUE = 0, 1, 2, 3

LAMBDA = [
    "###...........",
    ".###..........",
    "..###.........",
    "...###........",
    "....###.......",
    ".....###......",
    "......###.....",
    "......####....",
    ".....##.###...",
    "....##...###..",
    "...##.....###.",
    "..##.......###",
    ".##.........##",
    "##...........#",
]

# 3x5 pixel capitals for the badge text.
FONT = {
    "A": ["###", "#.#", "###", "#.#", "#.#"],
    "C": ["###", "#..", "#..", "#..", "###"],
    "D": ["##.", "#.#", "#.#", "#.#", "##."],
    "E": ["###", "#..", "##.", "#..", "###"],
    "F": ["###", "#..", "##.", "#..", "#.."],
    "I": ["###", ".#.", ".#.", ".#.", "###"],
    "M": ["#.#", "###", "###", "#.#", "#.#"],
    "O": ["###", "#.#", "#.#", "#.#", "###"],
    "P": ["##.", "#.#", "##.", "#..", "#.."],
    "S": ["###", "#..", "###", "..#", "###"],
    "U": ["#.#", "#.#", "#.#", "#.#", "###"],
}


def blank():
    return [[GREY] * W for _ in range(H)]


def card(px):
    """White card with a black border and a one-pixel shadow, inset 1px."""
    left, top, right, bottom = 1, 1, W - 3, H - 3
    for y in range(top, bottom + 1):
        for x in range(left, right + 1):
            px[y][x] = WHITE
    for x in range(left, right + 1):
        px[top][x] = BLACK
        px[bottom][x] = BLACK
    for y in range(top, bottom + 1):
        px[y][left] = BLACK
        px[y][right] = BLACK
    # shadow along the right and bottom edges
    for y in range(top + 1, bottom + 2):
        px[y][right + 1] = BLACK
    for x in range(left + 1, right + 2):
        px[bottom + 1][x] = BLACK


def blit(px, sprite, x0, y0, colour):
    for dy, row in enumerate(sprite):
        for dx, ch in enumerate(row):
            if ch == "#":
                px[y0 + dy][x0 + dx] = colour


def text(px, s, x0, y0, colour):
    for i, ch in enumerate(s):
        blit(px, FONT[ch], x0 + i * 4, y0, colour)


def badge(px, s):
    """Blue tab in the lower right corner of the card with white text."""
    tw = len(s) * 4 - 1
    x1, y1 = W - 4, H - 4          # inside the border, above the shadow
    x0, y0 = x1 - tw - 3, y1 - 7
    for y in range(y0, y1):
        for x in range(x0, x1):
            px[y][x] = BLUE
    text(px, s, x0 + 2, y0 + 1, WHITE)


def art(kind):
    px = blank()
    card(px)
    if kind == "amilisp":
        blit(px, LAMBDA, 17, 5, BLACK)
    elif kind in ("amilisp-fpu", "amilisp-mos"):
        blit(px, LAMBDA, 12, 5, BLACK)
        badge(px, "FPU" if kind == "amilisp-fpu" else "MOS")
    elif kind in ("clamacs", "clamacs-fpu", "clamacs-mos"):
        blit(px, LAMBDA, 8, 5, BLACK)
        # a text cursor and two "lines" to the right of the lambda
        for y in range(6, 18):
            px[y][27] = BLACK
        for x in range(30, 42):
            px[7][x] = BLACK
            px[10][x] = BLACK
        # the editor's card; the build's badge on the FPU and MorphOS ones
        badge(px, {"clamacs": "IDE", "clamacs-fpu": "FPU", "clamacs-mos": "MOS"}[kind])
    elif kind == "guide":
        # a small lambda and the "lines of text" of a document page
        blit(px, LAMBDA, 4, 4, BLACK)
        for y in (6, 9, 12, 15):
            for x in range(21, 43 if y < 15 else 28):
                px[y][x] = BLACK
        badge(px, "DOC")
    else:
        raise ValueError(kind)
    return px


# --- DiskObject writer ----------------------------------------------------

WBTOOL, WBPROJECT = 3, 4
GFLG_GADGIMAGE = 0x0004
GACT_RELVERIFY, GACT_IMMEDIATE = 0x0001, 0x0002
GTYP_BOOLGADGET = 0x0001


def planes(px, depth=2):
    """Bitplane data: per plane, H rows of ceil(W/16) words, MSB first."""
    row_words = (W + 15) // 16
    out = bytearray()
    for plane in range(depth):
        for y in range(H):
            bits = 0
            for x in range(W):
                if (px[y][x] >> plane) & 1:
                    bits |= 1 << (row_words * 16 - 1 - x)
            out += bits.to_bytes(row_words * 2, "big")
    return bytes(out)


def cstring(s):
    b = s.encode("latin-1") + b"\0"
    return struct.pack(">I", len(b)) + b


NO_ICON_POSITION = 0x80000000   # Workbench places the icon itself
WBDRAWER = 2                    # (1 is WBDISK; type 2 is what SYS:Prefs.info carries)


def gadget_header():
    gadget = struct.pack(
        ">IhhhhHHHIIIIIHI",
        0,                      # NextGadget
        0, 0, W, H,             # LeftEdge, TopEdge, Width, Height
        GFLG_GADGIMAGE,         # Flags: image gadget, complement highlight
        GACT_RELVERIFY | GACT_IMMEDIATE,
        GTYP_BOOLGADGET,
        1,                      # GadgetRender (follows)
        0,                      # SelectRender
        0,                      # GadgetText
        0,                      # MutualExclude
        0,                      # SpecialInfo
        0,                      # GadgetID
        1,                      # UserData: 1 = OS 2.x icon revision
    )
    assert len(gadget) == 44
    return struct.pack(">HH", 0xE310, 1) + gadget


def image_header():
    image = struct.pack(">hhhhhIBBI", 0, 0, W, H, 2, 1, 0x03, 0x00, 0)
    assert len(image) == 20
    return image


def diskobject(px, default_tool, tooltypes, stack, pos=None):
    """A project icon.  POS = (x, y) pins it in its drawer window (pixels
    from the window's inner top-left, the image's top-left corner; the
    label is centred under the image); None lets Workbench place it."""
    x, y = pos if pos else (NO_ICON_POSITION, NO_ICON_POSITION)
    head = gadget_header() + struct.pack(
        ">BBIIIIIIi",
        WBPROJECT, 0,           # do_Type, pad
        1,                      # do_DefaultTool (follows)
        1 if tooltypes else 0,  # do_ToolTypes (follows)
        x,                      # do_CurrentX
        y,                      # do_CurrentY
        0,                      # do_DrawerData
        0,                      # do_ToolWindow
        stack,                  # do_StackSize
    )
    assert len(head) == 78
    body = head + image_header() + planes(px) + cstring(default_tool)
    if tooltypes:
        body += struct.pack(">I", (len(tooltypes) + 1) * 4)
        for t in tooltypes:
            body += cstring(t)
    return body


def drawerobject(px, left, top, width, height):
    """A drawer icon.  ON DISK the 62-byte struct DrawerData of workbench.h
    is NOT contiguous -- icon.library has written it in two pieces since
    OS 2.0, and reads it back the same way:

        DiskObject (78)  DrawerData FIRST 56 BYTES  Image (20 + planes)
        [default tool] [tool types] [tool window]  DrawerData2 (6)

    The first 56 bytes are the struct NewWindow Workbench opens the drawer
    with (outer size and position on the Workbench screen) plus the view's
    scroll offsets dd_CurrentX/Y; the last 6 -- dd_Flags, dd_ViewModes, the
    OS 2.x additions -- come at the very end of the file and are read only
    when do_Gadget.UserData carries the OS 2.x revision (1).  This is the
    layout of every drawer icon AmigaOS itself writes: SYS:Prefs.info on an
    AmigaOS 3.2.3 Vampire and Devs.info/Utilities.info of the OS 3.9
    Workbench in verify/realamiga/aos3 all have the NewWindow at offset 78
    and the Image header at 134, with the six bytes last (dumped and
    compared 2026-09-15); a file with the 62 bytes in one block after the
    image is rejected by icon.library and Workbench silently shows its
    default drawer instead.  tests/test_icons.sh pins this layout.  Do not
    "fix" it to match the in-memory struct.

    dd_Flags 1 = show only files with icons, dd_ViewModes 1 = view by icon,
    so the fixed icon positions apply whatever the user's Workbench default
    is."""
    head = gadget_header() + struct.pack(
        ">BBIIIIIIi",
        WBDRAWER, 0,            # do_Type, pad
        0,                      # do_DefaultTool
        0,                      # do_ToolTypes
        NO_ICON_POSITION,       # do_CurrentX
        NO_ICON_POSITION,       # do_CurrentY
        1,                      # do_DrawerData (follows)
        0,                      # do_ToolWindow
        0,                      # do_StackSize
    )
    assert len(head) == 78
    WBENCHSCREEN = 1
    newwindow = struct.pack(
        ">hhhhBBIIIIIIIhhHHH",
        left, top, width, height,
        0xFF, 0xFF,             # DetailPen, BlockPen: the screen's
        0, 0x0200107F,          # IDCMPFlags; Flags as Workbench 3.x writes
                                # them (size/drag/depth/close gadgets, ...)
        0, 0, 0, 0, 0,          # FirstGadget, CheckMark, Title, Screen, BitMap
        98, 68,                 # MinWidth, MinHeight (as SYS:Prefs.info)
        0xFFFF, 0xFFFF,         # MaxWidth, MaxHeight
        WBENCHSCREEN,           # Type
    )
    assert len(newwindow) == 48
    drawerdata = newwindow + struct.pack(">ii", 0, 0)    # + dd_CurrentX/Y
    assert len(drawerdata) == 56
    drawerdata2 = struct.pack(">IH", 1, 1)                # dd_Flags, dd_ViewModes
    return head + drawerdata + image_header() + planes(px) + drawerdata2


def iconx(window):
    """An IconX launcher: the console window of the run, a 128K stack."""
    return "C:IconX", ["WINDOW=" + window], 131072


MULTIVIEW = ("SYS:Utilities/MultiView", [], 16384)   # tool as installed by
# AmigaOS 3.x and MorphOS; a project icon's stack is what Workbench starts
# the tool with

# The package root, a 3 x 3 grid: amilisp launchers, Clamacs launchers,
# guides as the columns; soft-float, FPU, MorphOS as the rows.
# Columns 170 px apart so the longest label ("README-FIRST.guide", 144 px
# in Topaz 8, centred under its 48 px image) clears its neighbours, and the
# first column far enough in that that label is not clipped at the left
# edge; rows 52 px apart (24 px image + label + gap).  Pixels from the
# drawer window's inner top-left.
COL = (64, 234, 404)
ROW = (8, 60, 112)

ICONS = {
    # name: (art kind, default tool, tool types, stack, position or None)
    "AmiLisp":     ("amilisp",) + iconx("CON:0/20/640/236/AmiLisp/CLOSE") + ((COL[0], ROW[0]),),
    "AmiLisp-FPU": ("amilisp-fpu",) + iconx("CON:0/20/640/236/AmiLisp-FPU/CLOSE") + ((COL[0], ROW[1]),),
    "AmiLisp-MOS": ("amilisp-mos",) + iconx("CON:0/20/640/236/AmiLisp-MOS/CLOSE") + ((COL[0], ROW[2]),),
    "Clamacs":     ("clamacs",) + iconx("NIL:") + ((COL[1], ROW[0]),),
    "Clamacs-FPU": ("clamacs-fpu",) + iconx("NIL:") + ((COL[1], ROW[1]),),
    "Clamacs-MOS": ("clamacs-mos",) + iconx("NIL:") + ((COL[1], ROW[2]),),
    "README-FIRST.guide": ("guide",) + MULTIVIEW + ((COL[2], ROW[0]),),
    "amilisp.guide":     ("guide",) + MULTIVIEW + ((COL[2], ROW[1]),),
    "clamacs.guide":      ("guide",) + MULTIVIEW + ((COL[2], ROW[2]),),
    # the reference guides under docs/: Workbench places them
    "Guide":       ("guide",) + MULTIVIEW + (None,),
}

# The package drawer's own icon (amilisp-<version>.info beside the drawer):
# a window on the Workbench screen big enough for the 3 x 3 grid above --
# 580 x 212 outer, so ~550 x 182 inside the borders, title bar and
# scrollers on a stock 3.x screen font (the two-row 580 x 160 was checked
# on a Vampire, OS 3.2.3; one more row of 52 px), still inside a 640 x 256
# PAL Workbench at 40/30.
DRAWER = ("amilisp", 40, 30, 580, 212)


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "icons")
    os.makedirs(outdir, exist_ok=True)
    for name, (kind, tool, tooltypes, stack, pos) in ICONS.items():
        data = diskobject(art(kind), tool, tooltypes, stack, pos)
        path = os.path.join(outdir, name + ".info")
        with open(path, "wb") as f:
            f.write(data)
        print("%s: %d bytes" % (path, len(data)))
    kind, left, top, width, height = DRAWER
    data = drawerobject(art(kind), left, top, width, height)
    path = os.path.join(outdir, "Drawer.info")
    with open(path, "wb") as f:
        f.write(data)
    print("%s: %d bytes (drawer)" % (path, len(data)))


if __name__ == "__main__":
    main()
