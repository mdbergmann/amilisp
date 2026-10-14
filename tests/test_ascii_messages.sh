#!/bin/sh
# Every string literal in the C sources and every string in the shipped
# Lisp library must be plain ASCII.
#
# Why: the messages reach the user on an AmigaOS console, in a MUI text
# object and in Clamacs's echo area, none of which speak UTF-8 -- an
# em-dash written as three UTF-8 bytes comes out as "â\200\224" or as a
# lone replacement glyph ("<number>  ? invoke restart by number" was the
# debugger's help text on the Amiga).  The ISO-8859-1 console has no
# em-dash or arrow at all, so a hyphen and "->" are what the source has to
# say.  Comments are free to use whatever they like: this checks literals
# only, with comments stripped first.
#
# Run: sh tests/test_ascii_messages.sh        (no amilisp binary needed)

ROOT=$(cd "$(dirname "$0")/.." && pwd)
status=0

# The scanners below are perl one-liners under xargs; without perl they
# find nothing and the check would pass vacuously -- skip out loud instead.
if ! command -v perl >/dev/null 2>&1; then
    echo "  skip  ascii_messages (no perl)"
    exit 0
fi

# C: drop /* */ and // comments, then report every "..." literal that holds
# a byte above 0x7F.  A `//' inside a string ("http://...") loses the rest
# of that line to the comment stripper, which only ever hides a literal --
# never flags a comment -- so the check stays one-sided and safe.
c_hits=$(cd "$ROOT" && find src -name '*.c' -o -name '*.h' | sort | \
    xargs perl -0777 -ne '
        s{/\*(.*?)\*/}{ "\n" x ($1 =~ tr/\n//) }gse;
        s{//[^\n]*}{}g;
        while (m{"(?:[^"\\\n]|\\.)*"}g) {
            my $lit = $&;
            if ($lit =~ /[\x80-\xff]/) {
                my $line = 1 + (substr($_, 0, $-[0]) =~ tr/\n//);
                print "$ARGV:$line: $lit\n";
            }
        }')

# Lisp: drop ; comments and #| |# blocks, then the same over "..." strings
# (a doubled backslash or an escaped quote is the only escape the reader
# knows inside a string).
lisp_hits=$(cd "$ROOT" && find lib -name '*.lisp' | sort | \
    xargs perl -0777 -ne '
        s{#\|(.*?)\|#}{ "\n" x ($1 =~ tr/\n//) }gse;
        s{;[^\n]*}{}g;
        while (m{"(?:[^"\\]|\\.)*"}gs) {
            my $lit = $&;
            if ($lit =~ /[\x80-\xff]/) {
                my $line = 1 + (substr($_, 0, $-[0]) =~ tr/\n//);
                $lit =~ s/\n.*//s;
                print "$ARGV:$line: $lit\n";
            }
        }')

if [ -n "$c_hits" ]; then
    echo "FAIL  C string literals with non-ASCII bytes (use - for an em-dash, -> for an arrow):"
    echo "$c_hits" | sed 's/^/        /'
    status=1
else
    echo "  ok  C string literals in src/ are ASCII"
fi

if [ -n "$lisp_hits" ]; then
    echo "FAIL  Lisp strings in lib/ with non-ASCII characters:"
    echo "$lisp_hits" | sed 's/^/        /'
    status=1
else
    echo "  ok  Lisp strings in lib/ are ASCII"
fi

exit $status
