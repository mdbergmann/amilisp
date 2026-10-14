#include "test.h"
#include "core/repl.h"
#include <string.h>

/*
 * Regression test for the AmigaOS user init file names.
 *
 * Bug (0.9.1): every document — README.md, the binary release's
 * README-FIRST.md, the Clamacs editor — told the user to put
 * (require "amiga/arexx") (amiga.arexx:start) into S:.clamigarc, mirroring
 * ~/.clamigarc on the host, while repl.c's load_user_init() only ever read
 * S:clamiga.lisp.  The file the docs describe was silently ignored, so the
 * editor never found the port a user had configured.
 *
 * The 0.12 rename to AmiLisp added S:.amilisprc as the documented name and
 * kept the older two as fallbacks: an init file written for a runtime that
 * was still called clamiga keeps working.
 *
 * Contract pinned here (the list is compiled on every platform so the host
 * suite can see it):
 *   1. S:.amilisprc is the FIRST candidate — the documented name wins.
 *   2. S:.clamigarc is the SECOND — the documented name before the rename.
 *   3. S:clamiga.lisp is the THIRD — the pre-0.9.1 name.
 *   4. Nothing else is tried, and the list is NULL-terminated.
 */

TEST(documented_name_first)
{
    ASSERT(cl_userinit_paths_amiga[0] != NULL);
    ASSERT_STR_EQ(cl_userinit_paths_amiga[0], "S:.amilisprc");
}

TEST(pre_rename_name_second)
{
    ASSERT(cl_userinit_paths_amiga[1] != NULL);
    ASSERT_STR_EQ(cl_userinit_paths_amiga[1], "S:.clamigarc");
}

TEST(legacy_name_third)
{
    ASSERT(cl_userinit_paths_amiga[2] != NULL);
    ASSERT_STR_EQ(cl_userinit_paths_amiga[2], "S:clamiga.lisp");
}

TEST(list_ends_there)
{
    ASSERT(cl_userinit_paths_amiga[3] == NULL);
}

int main(void)
{
    test_init();

    RUN(documented_name_first);
    RUN(pre_rename_name_second);
    RUN(legacy_name_third);
    RUN(list_ends_there);

    REPORT();
}
