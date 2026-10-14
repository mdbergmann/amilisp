/*
 * wbrun -- start a program the way Workbench does, from a Shell script.
 *
 *   wbrun MKICON TOOL|PROJECT <file> [<tooltype> ...]
 *       writes <file>.info: the default icon of that type with the given
 *       tool types (icon.library's PutDiskObject), so a test needs no icon
 *       files in the source tree.
 *
 *   wbrun RUN [STACK=<bytes>] <tool> [<project> ...]
 *       LoadSegs <tool>, creates a process for it with NO console
 *       (pr_CIS/pr_COS = 0, pr_CLI = 0 -- exactly a Workbench-started
 *       process), sends it a WBStartup message whose ArgList is the tool
 *       and every project as (directory lock, file name) pairs, and waits
 *       for the startup code to reply the message when the program ends.
 *       STACK is the process stack in bytes (what the icon's Stack field
 *       would give; default 8000, the size of a stock icon, so the
 *       program's own stack handling is what is tested).
 *
 * This is the whole Workbench launch protocol (workbench/startup.h): a
 * program cannot tell the difference, so the FS-UAE suite drives the
 * runtime's Workbench start (tool types, project arguments, console,
 * stack) unattended through it.  Built by Makefile.cross (`wbrun`).
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <workbench/startup.h>
#include <workbench/workbench.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/icon.h>
#include <string.h>
#include <stdlib.h>

struct Library *IconBase = NULL;

static void say(const char *s)
{
    BPTR out = Output();
    if (out) Write(out, (APTR)s, strlen(s));
}

static int mkicon(int argc, char **argv)
{
    LONG type;
    struct DiskObject *dobj;
    const char *file;
    char **tt;
    int i, n, rc = RETURN_FAIL;

    if (argc < 4) { say("wbrun MKICON TOOL|PROJECT <file> [<tooltype> ...]\n"); return RETURN_FAIL; }
    if (strcmp(argv[2], "TOOL") == 0) type = WBTOOL;
    else if (strcmp(argv[2], "PROJECT") == 0) type = WBPROJECT;
    else { say("wbrun: MKICON type must be TOOL or PROJECT\n"); return RETURN_FAIL; }
    file = argv[3];
    n = argc - 4;

    IconBase = OpenLibrary((CONST_STRPTR)"icon.library", 36);
    if (!IconBase) { say("wbrun: no icon.library 36\n"); return RETURN_FAIL; }
    dobj = GetDefDiskObject(type);
    if (!dobj) { say("wbrun: GetDefDiskObject failed\n"); CloseLibrary(IconBase); return RETURN_FAIL; }

    tt = (char **)AllocVec((ULONG)(n + 1) * sizeof(char *), MEMF_CLEAR);
    if (tt) {
        for (i = 0; i < n; i++) tt[i] = argv[4 + i];
        tt[n] = NULL;
        dobj->do_ToolTypes = (n > 0) ? (STRPTR *)tt : NULL;
        dobj->do_DefaultTool = (STRPTR)((type == WBPROJECT) ? "amilisp" : "");
        dobj->do_StackSize = 8000;
        dobj->do_CurrentX = NO_ICON_POSITION;
        dobj->do_CurrentY = NO_ICON_POSITION;
        if (PutDiskObject((CONST_STRPTR)file, dobj)) {
            rc = RETURN_OK;
        } else {
            say("wbrun: PutDiskObject failed for "); say(file); say("\n");
        }
        dobj->do_ToolTypes = NULL;    /* ours, not the icon's */
        dobj->do_DefaultTool = NULL;
        FreeVec(tt);
    }
    FreeDiskObject(dobj);
    CloseLibrary(IconBase);
    return rc;
}

/* Lock the directory part of PATH and return the file part. */
static BPTR lock_dir_of(const char *path, const char **name)
{
    char dir[512];
    LONG len;
    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    *name = (const char *)FilePart((STRPTR)dir);
    len = (LONG)(*name - dir);
    *name = (const char *)FilePart((STRPTR)path);
    dir[len] = '\0';                  /* "" = the current directory */
    return Lock((CONST_STRPTR)dir, ACCESS_READ);
}

static int run(int argc, char **argv)
{
    ULONG stack = 8000;
    int first = 2, i, n;
    BPTR seg;
    struct MsgPort *reply;
    struct WBStartup *wbs;
    struct WBArg *args;
    struct Process *proc;
    const char *toolname;
    BPTR home;
    int rc = RETURN_FAIL;

    if (argc > first && strncmp(argv[first], "STACK=", 6) == 0) {
        stack = (ULONG)atol(argv[first] + 6);
        first++;
    }
    if (argc <= first) { say("wbrun RUN [STACK=<bytes>] <tool> [<project> ...]\n"); return RETURN_FAIL; }
    n = argc - first;                  /* tool + projects */

    seg = LoadSeg((CONST_STRPTR)argv[first]);
    if (!seg) { say("wbrun: cannot LoadSeg "); say(argv[first]); say("\n"); return RETURN_FAIL; }
    reply = CreateMsgPort();
    wbs = (struct WBStartup *)AllocVec(sizeof(struct WBStartup), MEMF_PUBLIC | MEMF_CLEAR);
    args = (struct WBArg *)AllocVec((ULONG)n * sizeof(struct WBArg), MEMF_PUBLIC | MEMF_CLEAR);
    if (!reply || !wbs || !args) { say("wbrun: out of memory\n"); goto out; }

    for (i = 0; i < n; i++) {
        const char *name;
        args[i].wa_Lock = lock_dir_of(argv[first + i], &name);
        args[i].wa_Name = (STRPTR)name;
        if (!args[i].wa_Lock) { say("wbrun: cannot lock the directory of "); say(argv[first + i]); say("\n"); goto out; }
    }
    toolname = (const char *)args[0].wa_Name;

    /* PROGDIR: is the tool's drawer, as Workbench sets it; DOS unlocks
     * the lock when the process ends. */
    home = DupLock(args[0].wa_Lock);
    proc = CreateNewProcTags(NP_Seglist, (ULONG)seg,
                             NP_FreeSeglist, FALSE,
                             NP_HomeDir, (ULONG)home,
                             NP_Name, (ULONG)toolname,
                             NP_StackSize, stack,
                             NP_Cli, FALSE,
                             NP_Input, 0,
                             NP_Output, 0,
                             NP_CloseInput, FALSE,
                             NP_CloseOutput, FALSE,
                             TAG_END);
    if (!proc) {
        if (home) UnLock(home);
        say("wbrun: CreateNewProc failed\n");
        goto out;
    }

    wbs->sm_Message.mn_Node.ln_Type = NT_MESSAGE;
    wbs->sm_Message.mn_ReplyPort = reply;
    wbs->sm_Message.mn_Length = sizeof(struct WBStartup);
    wbs->sm_Process = &proc->pr_MsgPort;
    wbs->sm_Segment = seg;
    wbs->sm_NumArgs = n;
    wbs->sm_ToolWindow = NULL;
    wbs->sm_ArgList = args;
    PutMsg(&proc->pr_MsgPort, &wbs->sm_Message);

    /* The program's startup code replies the message when it exits. */
    WaitPort(reply);
    while (GetMsg(reply) == NULL) WaitPort(reply);
    rc = RETURN_OK;

out:
    if (args) {
        for (i = 0; i < n; i++)
            if (args[i].wa_Lock) UnLock(args[i].wa_Lock);
        FreeVec(args);
    }
    if (wbs) FreeVec(wbs);
    if (reply) DeleteMsgPort(reply);
    UnLoadSeg(seg);
    return rc;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "MKICON") == 0) return mkicon(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "RUN") == 0) return run(argc, argv);
    say("wbrun MKICON TOOL|PROJECT <file> [<tooltype> ...]\n"
        "wbrun RUN [STACK=<bytes>] <tool> [<project> ...]\n");
    return RETURN_FAIL;
}
