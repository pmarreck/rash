/* hooks.h -- lifecycle hooks over the executor's parsed COMMAND tree. */

#ifndef _RASH_HOOKS_H_
#define _RASH_HOOKS_H_

#include "command.h"

struct fd_bitmap;

extern int rash_hooks_active (void);
extern int rash_hooks_execute (COMMAND *, int, int, int, struct fd_bitmap *);

#endif /* _RASH_HOOKS_H_ */
