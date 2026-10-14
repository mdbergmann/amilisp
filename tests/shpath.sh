# Shared by the shell tests: spell a path the way the amilisp binary will.
#
# Under MSYS2 / Git Bash the shell and a NATIVE amilisp.exe disagree about
# paths.  The shell's "$(pwd)" is "/c/Users/you/cl-amiga" and its /tmp is a
# mount that amilisp.exe knows nothing about, while amilisp prints and resolves
# "C:/Users/you/cl-amiga".  Command-line arguments are converted automatically
# on the way to a native process, but a path compared against amilisp's OUTPUT,
# or handed over inside an environment variable or a generated .lisp file, is
# not — so a test that builds an expected string from $(pwd) compares two
# spellings of the same directory and fails.
#
# native_path prints the spelling amilisp uses (MSYS tools accept it too), and
# is the identity function on every other host.
native_path() {
    if command -v cygpath >/dev/null 2>&1; then
        cygpath -m "$1"
    else
        printf '%s\n' "$1"
    fi
}
