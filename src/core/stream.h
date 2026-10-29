#ifndef CL_STREAM_H
#define CL_STREAM_H

#include "types.h"

/*
 * CL Stream support.
 *
 * Streams are heap objects (TYPE_STREAM) supporting three backends:
 *   CONSOLE — wraps platform_getchar/platform_write_string
 *   FILE    — wraps platform_file_* handle-based I/O
 *   STRING  — reads from CL string or writes to growable C buffer
 *
 * String output buffers live in platform_alloc'd memory (outside GC arena)
 * and are tracked via a side table indexed by out_buf_handle.
 */

/* --- Output buffer side table --- */

/* Slots per directory block.  The side table is a segmented directory that
 * grows on demand, one block at a time (see stream.c); this is the size of
 * one block — NOT a table capacity cap. */
#define CL_STREAM_BUF_TABLE_SIZE 256

/* Singleton console streams */
extern CL_Obj cl_stdin_stream;   /* Console input stream */
extern CL_Obj cl_stdout_stream;  /* Console output stream */
extern CL_Obj cl_stderr_stream;  /* Console error output stream */

/* Initialize stream subsystem */
void cl_stream_init(void);

/* Shutdown stream subsystem (free buffers) */
void cl_stream_shutdown(void);

/* Process-exit teardown: release the segmented side tables cl_stream_shutdown
 * deliberately keeps (output-buffer directory blocks, per-socket lock blocks
 * and their OS mutexes) plus the module locks.  Separate from
 * cl_stream_shutdown because unit-test teardowns call that one and then
 * re-init; this is the "nothing comes after" variant. */
void cl_stream_release_tables(void);

/* Allocate a stream object on the heap */
CL_Obj cl_make_stream(uint32_t direction, uint32_t stream_type);

/* Allocate an output buffer, return handle (0 = invalid) */
uint32_t cl_stream_alloc_outbuf(uint32_t initial_size);

/* Re-materialize an outbuf slot at a FIXED handle with the given contents
 * (image restore; see image.c).  Single-threaded use only.  Returns 0 on
 * success, -1 on bad handle / OOM / occupied slot. */
int cl_stream_outbuf_install_at(uint32_t handle, const char *data,
                                uint32_t len);

/* Enumerate allocated outbuf slots (image writer): smallest in-use handle
 * greater than AFTER, or 0 when exhausted.  Single-threaded use only. */
uint32_t cl_stream_outbuf_next_used(uint32_t after);

/* Free an output buffer by handle */
void cl_stream_free_outbuf(uint32_t handle);

/* Mark-driven outbuf reclamation, driven by the GC.  cl_gc()/cl_gc_compact()
 * call _gc_mark_begin() before marking, _gc_mark_use() for every live output
 * stream's handle during marking, and _gc_reclaim() after marking to free
 * slots no live stream pinned.  This replaces freeing outbufs from dead-corpse
 * finalization, which was unsafe under handle reuse (see stream.c). */
void cl_stream_outbuf_gc_mark_begin(void);
void cl_stream_outbuf_gc_mark_use(uint32_t handle);
void cl_stream_outbuf_gc_reclaim(void);
/* Release a freshly-allocated outbuf's creation-window pin once its owning
 * stream is set (see stream.c outbuf_pinned). */
void cl_stream_outbuf_unpin(uint32_t handle);

/* Get pointer to output buffer data */
char *cl_stream_outbuf_data(uint32_t handle);

/* Occupancy diagnostics: *used = allocated slots, *capacity = total slots
 * across all currently allocated directory blocks (grows on demand). */
void cl_stream_outbuf_table_stats(uint32_t *used, uint32_t *capacity);

/* Get output buffer length */
uint32_t cl_stream_outbuf_len(uint32_t handle);

/* Append a char to output buffer, growing if needed.
 * Updates the stream's out_buf_size on growth. */
void cl_stream_outbuf_putchar(CL_Stream *st, int ch);

/* Append a string to output buffer */
void cl_stream_outbuf_write(CL_Stream *st, const char *str);

/* Reset output buffer (for get-output-stream-string) */
void cl_stream_outbuf_reset(uint32_t handle);

/* --- Stream I/O operations --- */

/* Read one character from stream. Returns -1 on EOF.
 * With CL_WIDE_STRINGS, decodes UTF-8 multi-byte sequences. */
int  cl_stream_read_char(CL_Obj stream);

/* Read one raw byte from stream. Returns -1 on EOF.
 * Never decodes UTF-8 — for binary (unsigned-byte 8) streams. */
int  cl_stream_read_byte(CL_Obj stream);

/* Bulk raw-byte I/O for READ-SEQUENCE / WRITE-SEQUENCE.  `buf` MUST be C
 * memory (not arena) — the platform call may block inside a GC safe region.
 * read_bytes returns the byte count actually read (may be short at EOF);
 * write_bytes returns 0 on success.  Both return -2 when the stream kind has
 * no bulk path (string/console/broadcast/...) — the caller must fall back to
 * the per-element read-byte/write-byte loop, which keeps those kinds'
 * existing semantics. */
int32_t cl_stream_read_bytes(CL_Obj stream, char *buf, uint32_t len);
int32_t cl_stream_write_bytes(CL_Obj stream, const char *buf, uint32_t len);

/* Write one character to stream.
 * With CL_WIDE_STRINGS, encodes non-ASCII as UTF-8. */
void cl_stream_write_char(CL_Obj stream, int ch);

/* Write one raw byte to stream.
 * Never encodes UTF-8 — for binary (unsigned-byte 8) streams. */
void cl_stream_write_byte(CL_Obj stream, int byte);

/* Write a string (len bytes) to stream. */
void cl_stream_write_string(CL_Obj stream, const char *str, uint32_t len);

/* Arena-safe variant: writes strobj (a base string) [start,end) by chunking
 * through a C buffer, re-deriving the source per chunk — required whenever
 * the source lives in the arena and the stream can block (file/socket). */
void cl_stream_write_lisp_string(CL_Obj stream, CL_Obj strobj,
                                 uint32_t start, uint32_t end);

/* Peek at next character without consuming. Returns -1 on EOF. */
int  cl_stream_peek_char(CL_Obj stream);

/* Push a character back onto stream (only one level). */
void cl_stream_unread_char(CL_Obj stream, int ch);

/* Close a stream. */
void cl_stream_close(CL_Obj stream);

/* Create string input stream reading from string[start..end). */
CL_Obj cl_make_string_input_stream(CL_Obj string, uint32_t start, uint32_t end);

/* Create input stream reading from a C buffer (data stays outside GC arena).
 * The caller must keep `data` alive until the stream is done.
 * The data pointer is stored in the outbuf side table.
 * Returns NIL when all slots are taken by live streams (nesting too deep);
 * the caller still owns `data` then. */
CL_Obj cl_make_cbuf_input_stream(const char *data, uint32_t len);
/* GC sweep hook: a C-buffer stream that died without CLOSE gives its slot
 * back and frees its buffer (the owner was unwound past its cleanup). */
void cl_stream_cbuf_gc_release(CL_Stream *st);

/* Create string output stream with growable buffer. */
CL_Obj cl_make_string_output_stream(void);

/* Extract accumulated string from string output stream and reset buffer. */
CL_Obj cl_get_output_stream_string(CL_Obj stream);

/* Return SSTREAM's accumulated string (as cl_get_output_stream_string) and, in
 * addition, free the stream's platform output buffer and clear its handle.
 * For transient string-output-streams whose outbuf would otherwise leak; folds
 * the get-string + free-outbuf idiom repeated in printer/format/io/condition. */
CL_Obj cl_finish_string_output_stream(CL_Obj sstream);

/* Create a bidirectional TCP socket stream connected to host:port.
 * connect_ms > 0 bounds the connect handshake (unreachable host fails fast);
 * 0 blocks until the OS gives up.  Returns CL_NIL on connection failure. */
CL_Obj cl_make_socket_stream(const char *host, int port, int connect_ms);
/* Connected UDP datagram socket stream (CL_STREAM_FLAG_DGRAM set); I/O via
 * the EXT:UDP-STREAM-SEND/-RECEIVE builtins, closed with CL:CLOSE. */
CL_Obj cl_make_udp_socket_stream(const char *host, int port);
/* Unconnected UDP datagram socket stream bound to addr:port (addr NULL =
 * every address of `family`, PLATFORM_AF_INET/INET6); I/O via
 * EXT:UDP-SEND-TO / EXT:UDP-RECEIVE-FROM.  The bound port is written to
 * *actual_port.  Returns CL_NIL on failure. */
CL_Obj cl_make_udp_bound_socket_stream(const char *addr, int port, int family,
                                       int reuse, int *actual_port);

/* Create a listening (server) socket stream bound to `port`.
 * loopback != 0 binds 127.0.0.1 only; otherwise all interfaces.
 * When actual_port != NULL the OS-assigned port (relevant if port==0) is
 * written there.  Returns CL_NIL on failure. */
CL_Obj cl_make_listen_stream(int port, int loopback, int *actual_port);
/* The same, bound to one dotted-quad IPv4 address (NULL: every interface);
 * NIL when the address is not one of this machine's or not dotted-quad. */
CL_Obj cl_make_listen_stream_addr(int port, const char *bind_addr, int *actual_port);

/* Accept one connection on a listening socket stream (blocks).
 * Returns a bidirectional socket stream, or CL_NIL on error. */
CL_Obj cl_socket_stream_accept(CL_Obj listener);

/* Bound local port of an open listening socket stream, or -1 if `stream` is
 * not one.  Lets callers discover the port when socket-listen was given 0
 * (OS-assigned ephemeral port). */
int cl_listen_stream_local_port(CL_Obj stream);

/* Get/set the read (which==0) or write (which==1) timeout in milliseconds on a
 * socket stream; 0 = block indefinitely.  The setter mirrors the value into the
 * platform socket layer so a blocked read/write gives up after the deadline and
 * raises EXT:SOCKET-TIMEOUT.  Returns 0 / no-op for non-socket streams. */
uint32_t cl_socket_stream_get_timeout(CL_Obj stream, int which);
void     cl_socket_stream_set_timeout(CL_Obj stream, int which, uint32_t ms);

/* Create a synonym stream that delegates to the value of symbol. */
CL_Obj cl_make_synonym_stream(CL_Obj symbol);

/* Create a two-way stream: reads from input_stream, writes to output_stream. */
CL_Obj cl_make_two_way_stream(CL_Obj input_stream, CL_Obj output_stream);

/* Create a broadcast stream fanning writes out to a list of output streams. */
CL_Obj cl_make_broadcast_stream_list(CL_Obj streams);

/* Create a concatenated stream reading a list of input streams in order. */
CL_Obj cl_make_concatenated_stream_list(CL_Obj streams);

/* Create an echo stream: reads come from input_stream and are echoed to
 * output_stream; direct writes go to output_stream unechoed (CLHS 21.1). */
CL_Obj cl_make_echo_stream(CL_Obj input_stream, CL_Obj output_stream);

/* --- Stream-aware write helpers ---
 *
 * Write a C string to the stream currently bound to `sym`.  If the symbol
 * value is a stream object (TYPE_STREAM heap object), cl_stream_write_string
 * is used; otherwise falls back to platform_write_string so these are safe
 * before cl_stream_init and in fatal-error paths.
 */
void cl_write_cstring_to_stream_sym(CL_Obj sym, const char *s);

/* Convenience wrappers targeting the standard CL stream variables. */
void cl_write_cstring_to_stdout(const char *s);
void cl_write_cstring_to_error(const char *s);
void cl_write_cstring_to_debug_io(const char *s);
void cl_write_cstring_to_trace(const char *s);

/* FRESH-LINE core (builtins_stream.c): newline on `stream` unless it is at
 * column 0; returns 1 if a newline was written. */
int cl_stream_fresh_line(CL_Obj stream);

/* REPL column support: fresh-line on the stream bound to `sym`, and re-sync
 * that stream's column to 0 after the terminal echoed the user's Enter (see
 * stream.c for why the echo is invisible to charpos). */
void cl_stream_sym_fresh_line(CL_Obj sym);
void cl_stream_sym_note_bol(CL_Obj sym);

/* Resolve a stream-designator argument at args[idx] to a concrete stream:
 * missing or NIL yields the standard input/output stream, T yields
 * *terminal-io*, a stream yields itself, anything else is a TYPE-ERROR.
 * Shared by builtins_stream.c and builtins_io.c (which used to carry
 * byte-identical copies). */
CL_Obj cl_resolve_input_stream(CL_Obj *args, int n, int idx);
CL_Obj cl_resolve_output_stream(CL_Obj *args, int n, int idx);

#endif /* CL_STREAM_H */
