/* hooks.h -- lifecycle hooks over the executor's parsed COMMAND tree. */

#ifndef _RASH_HOOKS_H_
#define _RASH_HOOKS_H_

#include "command.h"

struct fd_bitmap;

extern int rash_hooks_active (void);
extern int rash_hooks_execute (COMMAND *, int, int, int, struct fd_bitmap *);
/* After expand_words / alias-already-substituted argv; non-zero means deny. */
extern int rash_hooks_before_simple (WORD_LIST *words);
/* After a simple command finishes; optional capped stdout/stderr capture. */
extern void rash_hooks_after_simple (WORD_LIST *words, int status,
				    const char *captured_stdout, size_t stdout_len,
				    const char *captured_stderr, size_t stderr_len);
/* Parent, top of execute_pipeline, before download-pipe/fork. Non-zero → deny. */
extern int rash_hooks_before_pipeline (COMMAND *command);
/* Start of execute_builtin; non-zero → skip builtin. */
extern int rash_hooks_on_builtin (WORD_LIST *words);
/* Start of execute_function after nest-max; non-zero → skip function body. */
extern int rash_hooks_on_function (const char *name, WORD_LIST *words);
/* Start of shell_execve, before execve(2). Non-zero → do not exec. */
extern int rash_hooks_on_exec (const char *path, char **args);
/* Before open(2) for a path-bearing redirect. Non-zero → RASH_DENIED_REDIRECT. */
extern int rash_hooks_on_redirect (const char *path, enum r_instruction ri, int redirector_fd);
/* Download→shell pipe seam: 1 if intercepted (*result set), 0 to fall through. */
extern int rash_hooks_try_download_pipe (COMMAND *command, int asynchronous,
					 int pipe_in, int pipe_out,
					 struct fd_bitmap *fds_to_close,
					 int *result);
extern int rash_hooks_want_stdio_capture (void);
/* Capture stdout/stderr for on_stdio_bundle when RASH_CAPTURE_FD is a valid FD. */
extern int rash_hooks_want_stdio_bundle (void);
/* After a simple command: invoke on_stdio_bundle and write a returned string to RASH_CAPTURE_FD. */
extern void rash_hooks_stdio_bundle (WORD_LIST *words, int status,
				    const char *captured_stdout, size_t stdout_len,
				    const char *captured_stderr, size_t stderr_len);
extern void rash_hooks_command_begin (void);
extern void rash_hooks_command_end (void);
extern void rash_hooks_command_unwind (void *);
extern int rash_hooks_reload (void);
extern void rash_hooks_configure_builtin (void);

#endif /* _RASH_HOOKS_H_ */
