/* amilisp.rexx -- editor-independent ARexx client for the AmiLisp port
 *
 * Usage from a Shell:
 *
 *     rx amilisp.rexx PING
 *     rx amilisp.rexx LOAD Work:src/foo.lisp
 *     rx amilisp.rexx "EVAL (room)"
 *
 * Prints the reply and exits with the command's return code, so it also
 * works as a build step:
 *
 *     rx amilisp.rexx LOAD Work:src/foo.lisp
 *     IF WARN THEN ...
 *
 * Start the port on the amilisp side first:
 *     (require "amiga/arexx")
 *     (amiga.arexx:start)
 */

OPTIONS RESULTS

/* See the note in load-current-file.ced: rc 10 means `the file has errors',
 * and the default FAILAT of 10 would abort this macro before it could print
 * them. */
OPTIONS FAILAT 21

PARSE ARG COMMAND

IF STRIP(COMMAND) = '' THEN DO
    SAY 'usage: rx amilisp.rexx <command>'
    SAY '       PING | VERSION | LOAD <file> | COMPILE-FILE <file>'
    SAY '       EVAL <form> | IN-PACKAGE <pkg> | LASTRESULT'
    EXIT 20
END

PORT = FindPort()
IF PORT = '' THEN DO
    SAY 'No AmiLisp ARexx port found.'
    SAY 'In amilisp:  (require "amiga/arexx")  (amiga.arexx:start)'
    EXIT 20
END

ADDRESS VALUE PORT
COMMAND
CODE = RC

IF CODE = 0 THEN
    SAY RESULT
ELSE DO
    /* A non-zero rc means ARexx dropped RESULT; LASTRESULT hands the same
     * text back under a zero rc. */
    'LASTRESULT'
    SAY RESULT
END

EXIT CODE

/* Returns the name of the first live amilisp port: AMILISP, else the
 * AMILISP.<n> a second instance claimed. */
FindPort:
    IF SHOW('P', 'AMILISP') THEN RETURN 'AMILISP'
    DO i = 1 TO 9
        IF SHOW('P', 'AMILISP.'i) THEN RETURN 'AMILISP.'i
    END
    RETURN ''
