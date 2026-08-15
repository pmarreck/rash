/* hooks.h -- lifecycle hooks over the executor's parsed COMMAND tree. */

#ifndef _RASH_HOOKS_H_
#define _RASH_HOOKS_H_

#include "command.h"

struct fd_bitmap;

extern int rash_hooks_active (void);
extern int rash_hooks_execute (COMMAND *, int, int, int, struct fd_bitmap *);
extern void rash_hooks_command_begin (void);
extern void rash_hooks_command_end (void);
extern void rash_hooks_command_unwind (void *);
extern int rash_hooks_reload (void);
extern void rash_hooks_configure_builtin (void);

#endif /* _RASH_HOOKS_H_ */
