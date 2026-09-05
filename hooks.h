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
/* Before open(2) for a path-bearing redirect. Non-zero → RASH_DENIED_REDIRECT. */
extern int rash_hooks_on_redirect (const char *path, enum r_instruction ri, int redirector_fd);
extern int rash_hooks_want_stdio_capture (void);
extern void rash_hooks_command_begin (void);
extern void rash_hooks_command_end (void);
extern void rash_hooks_command_unwind (void *);
extern int rash_hooks_reload (void);
extern void rash_hooks_configure_builtin (void);

#endif /* _RASH_HOOKS_H_ */
