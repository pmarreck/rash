/* hooks.c -- lazy LuaJIT lifecycle hooks over unexpanded shell commands. */

#include <config.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "shell.h"
#include "command.h"
#include "execute_cmd.h"
#include "builtins.h"
#include "builtins/common.h"
#include "hooks.h"
#include "stat-time.h"
#include "trap.h"
#include "unwind_prot.h"
#include "xmalloc.h"

#define RASH_COMMAND_METATABLE "rash.command"
#define RASH_HOOK_MAX_BYTES (1024 * 1024)
#define RASH_HOOK_INSTRUCTION_TICKS 100
#define RASH_HOOK_MAX_TICKS 1000
#define RASH_DENY_REASON_MAX 512
#define RASH_SPAWN_OUTPUT_MAX (64 * 1024)
#define RASH_SPAWN_ARGV_MAX 64

#if defined (HAVE_LSTAT)
#  define RASH_LSTAT lstat
#else
#  define RASH_LSTAT stat
#endif

typedef struct {
  COMMAND *command;
  int asynchronous;
  int pipe_in;
  int pipe_out;
  struct fd_bitmap *fds_to_close;
  int active;
  int executed;
  int denied;
  int result;
  int lua_stack_base;
  char deny_reason[RASH_DENY_REASON_MAX];
} RASH_HOOK_CONTEXT;

typedef struct {
  COMMAND *command;
} RASH_COMMAND_PROXY;

typedef struct {
  char *name;
  dev_t device;
  ino_t inode;
  mode_t mode;
  uid_t owner;
  off_t size;
  time_t modified_seconds;
  long modified_nanoseconds;
} RASH_HOOK_FILE;

typedef struct {
  int present;
  RASH_HOOK_FILE directory;
  RASH_HOOK_FILE *files;
  size_t count;
} RASH_HOOK_MANIFEST;

typedef struct {
  RASH_HOOK_CONTEXT *context;
  int next_hook;
  int called;
} RASH_HOOK_RUNNER;

static lua_State *rash_lua;
static lua_State *retired_lua;
static int hook_table_ref = LUA_NOREF;
static int before_table_ref = LUA_NOREF;
static int after_table_ref = LUA_NOREF;
static int hook_count;
static int before_count;
static int after_count;
static int *hook_enforcing;
static int *before_enforcing;
static int *after_enforcing;
static int loading_file_enforcing;
static int current_hook_enforcing;
static RASH_HOOK_CONTEXT *active_hook_context;
static int in_before_stage;
static int stage_denied;
static char stage_deny_reason[RASH_DENY_REASON_MAX];
static int hook_state;
static int hook_execution_depth;
static int hook_command_depth;
static int hook_reload_builtin_enabled;
static char *hook_directory;
static int hook_allow_unowned;
static int hook_enforce_unowned;
static RASH_HOOK_MANIFEST hook_manifest;
static int rash_lua_call_depth;
static int rash_lua_instruction_ticks;

static int rash_lua_hook (lua_State *);
static int rash_lua_before (lua_State *);
static int rash_lua_after (lua_State *);
static int rash_lua_warn (lua_State *);
static int rash_lua_deny (lua_State *);
static int rash_lua_spawn (lua_State *);
static int rash_command_index (lua_State *);
static int rash_hook_run (lua_State *);
static void rash_lua_instruction_limit (lua_State *, lua_Debug *);
static int rash_lua_pcall (lua_State *, int, int);
static void rash_unwind_hook_context (void *);
static int rash_hooks_initialize (int);
static void rash_mark_denied (RASH_HOOK_CONTEXT *, const char *);
static void rash_push_expanded_ctx (lua_State *, WORD_LIST *, int,
				   const char *, size_t, const char *, size_t);

static void
rash_hooks_close_retired_lua (void)
{
  if (retired_lua)
    {
      lua_close (retired_lua);
      retired_lua = 0;
    }
}

static void
rash_hook_warning (const char *prefix, const char *path)
{
  fprintf (stderr, "rash: warning: %s%s\n", prefix, path ? path : "");
}

static void
rash_hook_file_dispose (RASH_HOOK_FILE *file)
{
  free (file->name);
  memset (file, 0, sizeof (*file));
}

static void
rash_hook_file_set (RASH_HOOK_FILE *file, const char *name, const struct stat *st)
{
  struct timespec modified;

  memset (file, 0, sizeof (*file));
  file->name = name ? savestring (name) : 0;
  file->device = st->st_dev;
  file->inode = st->st_ino;
  file->mode = st->st_mode;
  file->owner = st->st_uid;
  file->size = st->st_size;
  modified = get_stat_mtime (st);
  file->modified_seconds = modified.tv_sec;
  file->modified_nanoseconds = modified.tv_nsec;
}

static int
rash_hook_file_matches (const RASH_HOOK_FILE *file, const struct stat *st)
{
  struct timespec modified;

  modified = get_stat_mtime (st);
  return file->device == st->st_dev && file->inode == st->st_ino &&
    file->mode == st->st_mode && file->owner == st->st_uid &&
    file->size == st->st_size && file->modified_seconds == modified.tv_sec &&
    file->modified_nanoseconds == modified.tv_nsec;
}

static void
rash_hook_manifest_dispose (RASH_HOOK_MANIFEST *manifest)
{
  size_t i;

  rash_hook_file_dispose (&manifest->directory);
  for (i = 0; i < manifest->count; i++)
    rash_hook_file_dispose (&manifest->files[i]);
  free (manifest->files);
  memset (manifest, 0, sizeof (*manifest));
}

static void
rash_hook_manifest_replace (RASH_HOOK_MANIFEST *replacement)
{
  rash_hook_manifest_dispose (&hook_manifest);
  hook_manifest = *replacement;
  memset (replacement, 0, sizeof (*replacement));
}

static void
rash_advisory_message (const char *message, size_t length)
{
  size_t i;

  fputs ("rash: advisory hook: ", stderr);
  for (i = 0; i < length; i++)
    {
      if (message[i] == '\n' || message[i] == '\r')
	fputs ("\\n", stderr);
      else
	fputc ((unsigned char)message[i], stderr);
    }
  fputc ('\n', stderr);
}

static const char *
rash_command_kind (COMMAND *command)
{
  if (command == 0)
    return "none";

  switch (command->type)
    {
    case cm_simple:
      return "simple";
    case cm_connection:
      return "connection";
    case cm_for:
      return "for";
    case cm_case:
      return "case";
    case cm_while:
      return "while";
    case cm_if:
      return "if";
    case cm_function_def:
      return "function_def";
    case cm_group:
      return "group";
    case cm_subshell:
      return "subshell";
    case cm_coproc:
      return "coproc";
    default:
      return "other";
    }
}

static void
rash_push_command (lua_State *L, COMMAND *command)
{
  RASH_COMMAND_PROXY *proxy;

  if (command == 0)
    {
      lua_pushnil (L);
      return;
    }

  proxy = (RASH_COMMAND_PROXY *)lua_newuserdata (L, sizeof (*proxy));
  proxy->command = command;
  luaL_getmetatable (L, RASH_COMMAND_METATABLE);
  lua_setmetatable (L, -2);
}

static void
rash_push_words (lua_State *L, WORD_LIST *words)
{
  WORD_LIST *word;
  int index;

  lua_newtable (L);
  for (word = words, index = 1; word; word = word->next, index++)
    {
      lua_pushstring (L, word->word && word->word->word ? word->word->word : "");
      lua_rawseti (L, -2, index);
    }
}

static void
rash_push_redirects (lua_State *L, REDIRECT *redirects)
{
  REDIRECT *redirect;
  int index;
  const char *word;

  lua_newtable (L);
  for (redirect = redirects, index = 1; redirect; redirect = redirect->next, index++)
    {
      lua_newtable (L);
      lua_pushboolean (L, INPUT_REDIRECT (redirect->instruction));
      lua_setfield (L, -2, "is_input");
      lua_pushboolean (L, CLOBBERING_REDIRECT (redirect->instruction)
			|| redirect->instruction == r_output_force);
      lua_setfield (L, -2, "is_clobber");
      word = 0;
      if (redirect->redirectee.filename && redirect->redirectee.filename->word)
	word = redirect->redirectee.filename->word;
      if (word)
	lua_pushstring (L, word);
      else
	lua_pushnil (L);
      lua_setfield (L, -2, "word");
      lua_rawseti (L, -2, index);
    }
}

/* Exposes parsed fields only when a Lua hook requests them.  This avoids a
   whole-tree marshal on every command and keeps words unexpanded. */
static int
rash_command_index (lua_State *L)
{
  RASH_COMMAND_PROXY *proxy;
  const char *field;
  COMMAND *command;

  proxy = (RASH_COMMAND_PROXY *)luaL_checkudata (L, 1, RASH_COMMAND_METATABLE);
  field = luaL_checkstring (L, 2);
  command = proxy->command;

  if (strcmp (field, "kind") == 0)
    lua_pushstring (L, rash_command_kind (command));
  else if (command && command->type == cm_connection && strcmp (field, "left") == 0)
    rash_push_command (L, command->value.Connection->first);
  else if (command && command->type == cm_connection && strcmp (field, "right") == 0)
    rash_push_command (L, command->value.Connection->second);
  else if (command && command->type == cm_connection && strcmp (field, "connector") == 0)
    {
      int connector;

      connector = command->value.Connection->connector;
      if (connector == '|')
	lua_pushliteral (L, "|");
      else if (connector == ';')
	lua_pushliteral (L, ";");
      else if (connector == '&')
	lua_pushliteral (L, "&");
      else if (connector == '\n')
	lua_pushliteral (L, "\n");
      else
	lua_pushinteger (L, connector);
    }
  else if (command && command->type == cm_simple && strcmp (field, "words") == 0)
    rash_push_words (L, command->value.Simple->words);
  else if (command && command->type == cm_simple && strcmp (field, "redirects") == 0)
    rash_push_redirects (L, command->value.Simple->redirects);
  else
    lua_pushnil (L);

  return 1;
}

static int
rash_register_callback (lua_State *L, int table_ref, int **enforcing_bits, int *count)
{
  size_t n;

  luaL_checktype (L, 1, LUA_TFUNCTION);
  lua_rawgeti (L, LUA_REGISTRYINDEX, table_ref);
  n = lua_objlen (L, -1);
  lua_pushvalue (L, 1);
  lua_rawseti (L, -2, (int)n + 1);
  lua_pop (L, 1);
  *enforcing_bits = (int *)xrealloc (*enforcing_bits, (*count + 1) * sizeof (int));
  (*enforcing_bits)[*count] = loading_file_enforcing;
  (*count)++;
  return 0;
}

static int
rash_lua_hook (lua_State *L)
{
  return rash_register_callback (L, hook_table_ref, &hook_enforcing, &hook_count);
}

static int
rash_lua_before (lua_State *L)
{
  /* Runs after expand_words: aliases already substituted at parse time;
     variables/globs/command-subs are expanded. */
  return rash_register_callback (L, before_table_ref, &before_enforcing, &before_count);
}

static int
rash_lua_after (lua_State *L)
{
  return rash_register_callback (L, after_table_ref, &after_enforcing, &after_count);
}

static void
rash_mark_denied (RASH_HOOK_CONTEXT *context, const char *reason)
{
  size_t length;

  if (context == 0 || context->denied)
    return;
  context->denied = 1;
  context->executed = 1;
  context->result = EXECUTION_FAILURE;
  if (reason == 0)
    reason = "command denied by lifecycle hook";
  length = strlen (reason);
  if (length >= RASH_DENY_REASON_MAX)
    length = RASH_DENY_REASON_MAX - 1;
  memcpy (context->deny_reason, reason, length);
  context->deny_reason[length] = '\0';
  fprintf (stderr, "rash: denied: %s\n", context->deny_reason);
}

static int
rash_lua_deny (lua_State *L)
{
  RASH_HOOK_CONTEXT *context;
  const char *reason;
  size_t length;

  reason = luaL_checkstring (L, 1);
  if (current_hook_enforcing == 0)
    {
      rash_hook_warning ("advisory hook called rash.deny; deny ignored (unowned hooks cannot enforce): ", reason);
      return 0;
    }

  /* Expanded-stage before hooks use stage_denied; parse-stage uses context. */
  if (in_before_stage)
    {
      if (stage_denied == 0)
	{
	  stage_denied = 1;
	  length = strlen (reason);
	  if (length >= RASH_DENY_REASON_MAX)
	    length = RASH_DENY_REASON_MAX - 1;
	  memcpy (stage_deny_reason, reason, length);
	  stage_deny_reason[length] = '\0';
	  fprintf (stderr, "rash: denied: %s\n", stage_deny_reason);
	}
      return 0;
    }

  context = active_hook_context;
  if (context == 0 || context->active == 0)
    return luaL_error (L, "rash.deny must run inside a lifecycle hook");
  if (context->executed && context->denied == 0)
    return luaL_error (L, "rash.deny cannot run after run()");
  rash_mark_denied (context, reason);
  return 0;
}

/* Direct exec that never enters rash_hooks_execute — physics, not an env token. */
static int
rash_lua_spawn (lua_State *L)
{
  char *argv[RASH_SPAWN_ARGV_MAX + 1];
  char stdout_buf[RASH_SPAWN_OUTPUT_MAX];
  char stderr_buf[RASH_SPAWN_OUTPUT_MAX];
  size_t stdout_len, stderr_len, argc, i;
  int out_pipe[2], err_pipe[2], status, rc;
  pid_t child;
  struct pollfd pollfds[2];
  sigset_t block_set, prev_set;

  luaL_checktype (L, 1, LUA_TTABLE);
  argc = lua_objlen (L, 1);
  if (argc == 0 || argc > RASH_SPAWN_ARGV_MAX)
    return luaL_error (L, "rash.spawn expects 1..%d argv entries", RASH_SPAWN_ARGV_MAX);

  /* Dup strings before any further Lua stack ops / fork; luaL_checkstring
     pointers are only valid while the value remains on the stack. */
  for (i = 0; i < argc; i++)
    {
      const char *arg;

      lua_rawgeti (L, 1, (int)i + 1);
      arg = luaL_checkstring (L, -1);
      argv[i] = savestring (arg);
      lua_pop (L, 1);
    }
  argv[argc] = 0;

  if (pipe (out_pipe) < 0 || pipe (err_pipe) < 0)
    {
      for (i = 0; i < argc; i++)
	free (argv[i]);
      return luaL_error (L, "rash.spawn cannot create pipes: %s", strerror (errno));
    }

  /* Bash's SIGCHLD handler reaps children; block it so waitpid sees ours. */
  sigemptyset (&block_set);
  sigaddset (&block_set, SIGCHLD);
  sigprocmask (SIG_BLOCK, &block_set, &prev_set);

  child = fork ();
  if (child < 0)
    {
      sigprocmask (SIG_SETMASK, &prev_set, 0);
      close (out_pipe[0]); close (out_pipe[1]);
      close (err_pipe[0]); close (err_pipe[1]);
      for (i = 0; i < argc; i++)
	free (argv[i]);
      return luaL_error (L, "rash.spawn fork failed: %s", strerror (errno));
    }

  if (child == 0)
    {
      int maxfd, fd;

      sigprocmask (SIG_SETMASK, &prev_set, 0);
      close (out_pipe[0]);
      close (err_pipe[0]);
      if (dup2 (out_pipe[1], STDOUT_FILENO) < 0 || dup2 (err_pipe[1], STDERR_FILENO) < 0)
	_exit (127);
      close (out_pipe[1]);
      close (err_pipe[1]);
      maxfd = sysconf (_SC_OPEN_MAX);
      if (maxfd < 0)
	maxfd = 256;
      for (fd = 3; fd < maxfd; fd++)
	close (fd);
      execvp (argv[0], argv);
      _exit (127);
    }

  for (i = 0; i < argc; i++)
    free (argv[i]);

  close (out_pipe[1]);
  close (err_pipe[1]);
  stdout_len = stderr_len = 0;

  /* Interleave reads so a full pipe cannot deadlock the child. */
  {
    char scratch[4096];
    int out_open, err_open;

    out_open = err_open = 1;
    while (out_open || err_open)
      {
	pollfds[0].fd = out_open ? out_pipe[0] : -1;
	pollfds[0].events = POLLIN;
	pollfds[1].fd = err_open ? err_pipe[0] : -1;
	pollfds[1].events = POLLIN;
	if (poll (pollfds, 2, -1) < 0)
	  {
	    if (errno == EINTR)
	      continue;
	    close (out_pipe[0]);
	    close (err_pipe[0]);
	    kill (child, SIGKILL);
	    waitpid (child, &status, 0);
	    sigprocmask (SIG_SETMASK, &prev_set, 0);
	    return luaL_error (L, "rash.spawn poll failed: %s", strerror (errno));
	  }
	if (out_open && (pollfds[0].revents & (POLLIN | POLLHUP | POLLERR)))
	  {
	    ssize_t got;

	    got = read (out_pipe[0], scratch, sizeof (scratch));
	    if (got > 0)
	      {
		size_t copy;

		copy = (size_t)got;
		if (stdout_len + copy > RASH_SPAWN_OUTPUT_MAX)
		  copy = RASH_SPAWN_OUTPUT_MAX - stdout_len;
		if (copy)
		  {
		    memcpy (stdout_buf + stdout_len, scratch, copy);
		    stdout_len += copy;
		  }
	      }
	    else
	      out_open = 0;
	  }
	if (err_open && (pollfds[1].revents & (POLLIN | POLLHUP | POLLERR)))
	  {
	    ssize_t got;

	    got = read (err_pipe[0], scratch, sizeof (scratch));
	    if (got > 0)
	      {
		size_t copy;

		copy = (size_t)got;
		if (stderr_len + copy > RASH_SPAWN_OUTPUT_MAX)
		  copy = RASH_SPAWN_OUTPUT_MAX - stderr_len;
		if (copy)
		  {
		    memcpy (stderr_buf + stderr_len, scratch, copy);
		    stderr_len += copy;
		  }
	      }
	    else
	      err_open = 0;
	  }
      }
  }

  close (out_pipe[0]);
  close (err_pipe[0]);
  rc = -1;
  while (waitpid (child, &status, 0) < 0)
    {
      if (errno != EINTR)
	{
	  sigprocmask (SIG_SETMASK, &prev_set, 0);
	  return luaL_error (L, "rash.spawn waitpid failed: %s", strerror (errno));
	}
    }
  sigprocmask (SIG_SETMASK, &prev_set, 0);
  if (WIFEXITED (status))
    rc = WEXITSTATUS (status);
  else if (WIFSIGNALED (status))
    rc = 128 + WTERMSIG (status);

  lua_pushinteger (L, rc);
  lua_pushlstring (L, stdout_buf, stdout_len);
  lua_pushlstring (L, stderr_buf, stderr_len);
  return 3;
}

static int
rash_lua_warn (lua_State *L)
{
  size_t length;
  const char *message;

  message = luaL_checklstring (L, 1, &length);
  rash_advisory_message (message, length);
  return 0;
}

static int rash_invoke_hook (lua_State *, RASH_HOOK_CONTEXT *, int);

/* A fixed instruction allowance keeps a malformed advisory hook from
   monopolizing the shell before its command reaches the executor. */
static void
rash_lua_instruction_limit (lua_State *L, lua_Debug *debug)
{
  (void)debug;
  if (--rash_lua_instruction_ticks <= 0)
    luaL_error (L, "rash advisory hook exceeded its instruction allowance");
}

static int
rash_lua_pcall (lua_State *L, int nargs, int nresults)
{
  int outermost, status;

  outermost = rash_lua_call_depth++ == 0;
  if (outermost)
    {
      rash_lua_instruction_ticks = RASH_HOOK_MAX_TICKS;
      lua_sethook (L, rash_lua_instruction_limit, LUA_MASKCOUNT,
		   RASH_HOOK_INSTRUCTION_TICKS);
    }
  status = lua_pcall (L, nargs, nresults, 0);
  if (--rash_lua_call_depth == 0)
    {
      lua_sethook (L, 0, 0, 0);
      rash_lua_instruction_ticks = 0;
    }
  return status;
}

/* Bash can nonlocally unwind while a hook has called run().  Put the Lua
   stack and lifecycle flag back into a reusable state before that jump. */
static void
rash_unwind_hook_context (void *opaque)
{
  RASH_HOOK_CONTEXT *context;

  context = (RASH_HOOK_CONTEXT *)opaque;
  context->active = 0;
  lua_sethook (rash_lua, 0, 0, 0);
  rash_lua_call_depth = 0;
  rash_lua_instruction_ticks = 0;
  lua_settop (rash_lua, context->lua_stack_base);
}

/* A runner is valid only during its owning lifecycle call.  Retained runners
   fail loudly instead of keeping a stale COMMAND pointer alive. */
static int
rash_hook_run (lua_State *L)
{
  RASH_HOOK_RUNNER *runner;
  RASH_HOOK_CONTEXT *context;
  int result;

  runner = (RASH_HOOK_RUNNER *)lua_touserdata (L, lua_upvalueindex (1));
  context = runner ? runner->context : 0;
  if (context == 0 || context->active == 0)
    return luaL_error (L, "rash hook run() escaped its command lifecycle");
  if (runner->called)
    return luaL_error (L, "rash hook called run() more than once");

  runner->called = 1;
  result = rash_invoke_hook (L, context, runner->next_hook);
  lua_pushinteger (L, result);
  return 1;
}

static int
rash_execute_original (RASH_HOOK_CONTEXT *context)
{
  if (context->executed)
    return context->result;

  context->executed = 1;
  hook_execution_depth++;
  context->result = execute_command_internal (context->command,
					       context->asynchronous,
					       context->pipe_in,
					       context->pipe_out,
					       context->fds_to_close);
  hook_execution_depth--;
  return context->result;
}

/* Invokes hooks as Rack-style middleware.  Advisory hooks that error or skip
   run() fall through.  Enforcing hooks that error or call rash.deny stop the
   command (fail closed). */
static int
rash_invoke_hook (lua_State *L, RASH_HOOK_CONTEXT *context, int index)
{
  RASH_HOOK_RUNNER *runner;
  int base, status, enforcing;

  if (context->denied)
    return context->result;
  if (index > hook_count)
    return rash_execute_original (context);

  enforcing = hook_enforcing && hook_enforcing[index - 1];
  current_hook_enforcing = enforcing;
  active_hook_context = context;

  base = lua_gettop (L);
  lua_rawgeti (L, LUA_REGISTRYINDEX, hook_table_ref);
  lua_rawgeti (L, -1, index);
  lua_remove (L, -2);
  rash_push_command (L, context->command);
  runner = (RASH_HOOK_RUNNER *)lua_newuserdata (L, sizeof (*runner));
  runner->context = context;
  runner->next_hook = index + 1;
  runner->called = 0;
  lua_pushcclosure (L, rash_hook_run, 1);

  status = rash_lua_pcall (L, 2, 0);
  if (status != 0)
    {
      const char *lua_error;

      lua_error = lua_tostring (L, -1);
      if (enforcing)
	{
	  rash_hook_warning ("enforcing hook failed; denying command: ",
			     lua_error ? lua_error : "(no error object)");
	  lua_pop (L, 1);
	  rash_mark_denied (context, "enforcing lifecycle hook failed");
	}
      else
	{
	  rash_hook_warning ("advisory hook failed; continuing command execution: ",
			     lua_error ? lua_error : "(no error object)");
	  lua_pop (L, 1);
	}
    }
  lua_settop (L, base);
  active_hook_context = 0;
  current_hook_enforcing = 0;

  if (context->denied)
    return context->result;
  if (context->executed == 0)
    return rash_invoke_hook (L, context, index + 1);
  return context->result;
}

static int
rash_lua_ready (void)
{
  lua_State *L;

  L = luaL_newstate ();
  if (L == 0)
    {
      rash_hook_warning ("cannot allocate LuaJIT state; hooks are disabled", 0);
      return 0;
    }

  /* Safe subset only: no io/os/package/debug/FFI. Base gives tostring/type;
     string gives find/format for lexical policy. */
  lua_pushcfunction (L, luaopen_base);
  lua_pushstring (L, "");
  lua_call (L, 1, 0);
  lua_pushcfunction (L, luaopen_string);
  lua_pushstring (L, LUA_STRLIBNAME);
  lua_call (L, 1, 0);

  lua_newtable (L);
  lua_pushcfunction (L, rash_lua_hook);
  lua_setfield (L, -2, "hook");
  lua_pushcfunction (L, rash_lua_before);
  lua_setfield (L, -2, "before");
  lua_pushcfunction (L, rash_lua_after);
  lua_setfield (L, -2, "after");
  lua_pushcfunction (L, rash_lua_warn);
  lua_setfield (L, -2, "warn");
  lua_pushcfunction (L, rash_lua_deny);
  lua_setfield (L, -2, "deny");
  lua_pushcfunction (L, rash_lua_spawn);
  lua_setfield (L, -2, "spawn");
  lua_setglobal (L, "rash");

  luaL_newmetatable (L, RASH_COMMAND_METATABLE);
  lua_pushcfunction (L, rash_command_index);
  lua_setfield (L, -2, "__index");
  lua_pop (L, 1);

  lua_newtable (L);
  hook_table_ref = luaL_ref (L, LUA_REGISTRYINDEX);
  lua_newtable (L);
  before_table_ref = luaL_ref (L, LUA_REGISTRYINDEX);
  lua_newtable (L);
  after_table_ref = luaL_ref (L, LUA_REGISTRYINDEX);
  rash_lua = L;
  return 1;
}

static int
rash_lua_name (const char *name)
{
  size_t length;

  length = strlen (name);
  return length > 4 && strcmp (name + length - 4, ".lua") == 0;
}

static int
rash_hook_name_compare (const void *left, const void *right)
{
  const char *const *left_name;
  const char *const *right_name;

  left_name = (const char *const *)left;
  right_name = (const char *const *)right;
  return strcmp (*left_name, *right_name);
}

/* Capture enough filesystem identity to detect an edit in place as well as a
   directory entry change.  The hot path only stats this cached manifest. */
static int
rash_hook_manifest_collect (const char *directory, RASH_HOOK_MANIFEST *manifest)
{
  struct dirent *entry;
  struct stat st;
  char **names;
  char *path;
  DIR *dir;
  size_t allocated, count, i, path_length;

  memset (manifest, 0, sizeof (*manifest));
  if (stat (directory, &st) < 0)
    return 0;
  dir = opendir (directory);
  if (dir == 0)
    return 0;

  names = 0;
  allocated = count = 0;
  while ((entry = readdir (dir)) != 0)
    {
      if (rash_lua_name (entry->d_name) == 0)
	continue;
      if (count == allocated)
	{
	  allocated = allocated ? allocated * 2 : 8;
	  names = (char **)xrealloc (names, allocated * sizeof (*names));
	}
      names[count++] = savestring (entry->d_name);
    }
  closedir (dir);

  qsort (names, count, sizeof (*names), rash_hook_name_compare);
  rash_hook_file_set (&manifest->directory, 0, &st);
  manifest->files = count ? (RASH_HOOK_FILE *)xmalloc (count * sizeof (*manifest->files)) : 0;
  if (count)
    memset (manifest->files, 0, count * sizeof (*manifest->files));
  manifest->count = count;
  for (i = 0; i < count; i++)
    {
      path_length = strlen (directory) + strlen (names[i]) + 2;
      path = (char *)xmalloc (path_length);
      sprintf (path, "%s/%s", directory, names[i]);
      if (RASH_LSTAT (path, &st) < 0)
	{
	  free (path);
	  while (i < count)
	    free (names[i++]);
	  free (names);
	  rash_hook_manifest_dispose (manifest);
	  return 0;
	}
      rash_hook_file_set (&manifest->files[i], names[i], &st);
      free (path);
      free (names[i]);
    }
  free (names);
  manifest->present = 1;
  return 1;
}

static int
rash_hook_manifest_matches (const char *directory)
{
  struct stat st;
  char *path;
  size_t i, path_length;

  if (hook_manifest.present == 0 || stat (directory, &st) < 0 ||
      rash_hook_file_matches (&hook_manifest.directory, &st) == 0)
    return 0;
  for (i = 0; i < hook_manifest.count; i++)
    {
      path_length = strlen (directory) + strlen (hook_manifest.files[i].name) + 2;
      path = (char *)xmalloc (path_length);
      sprintf (path, "%s/%s", directory, hook_manifest.files[i].name);
      if (RASH_LSTAT (path, &st) < 0 ||
	  rash_hook_file_matches (&hook_manifest.files[i], &st) == 0)
	{
	  free (path);
	  return 0;
	}
      free (path);
    }
  return 1;
}

static int
rash_load_hook (const char *path, int allow_unowned)
{
  struct stat st;
  char *source;
  ssize_t got;
  size_t offset;
  int fd, flags, trusted, status;

  flags = O_RDONLY;
#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  fd = open (path, flags);
  if (fd < 0)
    {
      rash_hook_warning ("cannot open hook '", path);
      return 0;
    }
  if (fstat (fd, &st) < 0 || S_ISREG (st.st_mode) == 0)
    {
      close (fd);
      rash_hook_warning ("refusing non-regular hook '", path);
      return 0;
    }
  if (st.st_size < 0 || st.st_size > RASH_HOOK_MAX_BYTES)
    {
      close (fd);
      rash_hook_warning ("refusing oversized hook '", path);
      return 0;
    }

  trusted = st.st_uid == 0 && (st.st_mode & (S_IWGRP | S_IWOTH)) == 0;
  if (trusted == 0 && allow_unowned == 0)
    {
      close (fd);
      rash_hook_warning ("refusing unowned hook; set RASH_ALLOW_UNOWNED_HOOKS=1 to load mutable advisory code: ", path);
      return 0;
    }
  if (trusted == 0)
    rash_hook_warning ("WARNING: RASH_ALLOW_UNOWNED_HOOKS=1 loads mutable hook as advisory code: ", path);
  loading_file_enforcing = trusted || (trusted == 0 && hook_enforce_unowned);
  if (trusted == 0 && hook_enforce_unowned)
    rash_hook_warning ("WARNING: RASH_HOOK_ENFORCE_UNOWNED=1 treats unowned hooks as enforcing (test/dev only): ", path);

  source = (char *)xmalloc ((size_t)st.st_size + 1);
  offset = 0;
  while (offset < (size_t)st.st_size)
    {
      got = read (fd, source + offset, (size_t)st.st_size - offset);
      if (got <= 0)
	{
	  free (source);
	  close (fd);
	  rash_hook_warning ("cannot read hook '", path);
	  return 0;
	}
      offset += (size_t)got;
    }
  close (fd);
  source[offset] = '\0';

  status = luaL_loadbuffer (rash_lua, source, offset, path);
  free (source);
  if (status == 0)
    status = rash_lua_pcall (rash_lua, 0, 0);
  loading_file_enforcing = 0;
  if (status != 0)
    {
      lua_pop (rash_lua, 1);
      rash_hook_warning ("cannot load hook '", path);
      return 0;
    }
  return 1;
}

static int
rash_load_hooks (const char *directory, int allow_unowned)
{
  struct dirent *entry;
  char **names;
  char *path;
  DIR *dir;
  int result;
  size_t allocated, count, i, path_length;

  dir = opendir (directory);
  if (dir == 0)
    {
      rash_hook_warning ("cannot open hook directory '", directory);
      return 0;
    }

  names = 0;
  result = 1;
  allocated = count = 0;
  while ((entry = readdir (dir)) != 0)
    {
      if (rash_lua_name (entry->d_name) == 0)
	continue;
      if (count == allocated)
	{
	  allocated = allocated ? allocated * 2 : 8;
	  names = (char **)xrealloc (names, allocated * sizeof (*names));
	}
      names[count++] = savestring (entry->d_name);
    }
  closedir (dir);

  qsort (names, count, sizeof (*names), rash_hook_name_compare);
  for (i = 0; i < count; i++)
    {
      path_length = strlen (directory) + strlen (names[i]) + 2;
      path = (char *)xmalloc (path_length);
      sprintf (path, "%s/%s", directory, names[i]);
      if (rash_load_hook (path, allow_unowned) == 0)
	result = 0;
      free (path);
      free (names[i]);
    }
  free (names);
  return result;
}

static int
rash_hook_reload_mode_enabled (void)
{
  const char *mode;

  mode = getenv ("RASH_HOOK_RELOAD");
  return mode && strcmp (mode, "mtime") == 0;
}

static void
rash_hook_save_configuration (const char *directory, int allow_unowned, int enforce_unowned)
{
  free (hook_directory);
  hook_directory = savestring (directory);
  hook_allow_unowned = allow_unowned;
  hook_enforce_unowned = enforce_unowned;
}

/* Build a complete replacement state before discarding an active hook set.
   A bad edit stays observable, but cannot turn off the prior observers. */
static int
rash_hooks_load_configuration (const char *directory, int allow_unowned, int enforce_unowned)
{
  lua_State *previous_lua;
  int *previous_enforcing, *previous_before_enforcing, *previous_after_enforcing;
  int previous_count, previous_before_count, previous_after_count;
  int previous_ref, previous_before_ref, previous_after_ref, previous_state;
  int loaded, snapshotted;
  RASH_HOOK_MANIFEST manifest;

  memset (&manifest, 0, sizeof (manifest));
  previous_lua = rash_lua;
  previous_ref = hook_table_ref;
  previous_before_ref = before_table_ref;
  previous_after_ref = after_table_ref;
  previous_count = hook_count;
  previous_before_count = before_count;
  previous_after_count = after_count;
  previous_enforcing = hook_enforcing;
  previous_before_enforcing = before_enforcing;
  previous_after_enforcing = after_enforcing;
  previous_state = hook_state;
  rash_lua = 0;
  hook_table_ref = before_table_ref = after_table_ref = LUA_NOREF;
  hook_count = before_count = after_count = 0;
  hook_enforcing = before_enforcing = after_enforcing = 0;
  hook_enforce_unowned = enforce_unowned;

  loaded = rash_lua_ready () && rash_load_hooks (directory, allow_unowned);
  snapshotted = rash_hook_manifest_collect (directory, &manifest);
  if (snapshotted == 0)
    rash_hook_warning ("cannot snapshot hook directory '", directory);

  if (loaded && snapshotted)
    {
      hook_state = (hook_count > 0 || before_count > 0 || after_count > 0) ? 1 : -1;
      rash_hook_save_configuration (directory, allow_unowned, enforce_unowned);
      rash_hook_manifest_replace (&manifest);
      free (previous_enforcing);
      free (previous_before_enforcing);
      free (previous_after_enforcing);
      if (previous_lua)
	{
	  if (hook_execution_depth)
	    retired_lua = previous_lua;
	  else
	    lua_close (previous_lua);
	}
      return 1;
    }

  if (rash_lua)
    lua_close (rash_lua);
  free (hook_enforcing);
  free (before_enforcing);
  free (after_enforcing);
  rash_lua = previous_lua;
  hook_table_ref = previous_ref;
  before_table_ref = previous_before_ref;
  after_table_ref = previous_after_ref;
  hook_count = previous_count;
  before_count = previous_before_count;
  after_count = previous_after_count;
  hook_enforcing = previous_enforcing;
  before_enforcing = previous_before_enforcing;
  after_enforcing = previous_after_enforcing;
  hook_state = previous_lua ? previous_state : -1;
  rash_hook_save_configuration (directory, allow_unowned, enforce_unowned);
  if (snapshotted)
    rash_hook_manifest_replace (&manifest);
  return 0;
}

static int
rash_hooks_initialize (int force)
{
  const char *allow, *directory, *enforce;
  int allow_unowned, enforce_unowned, same_configuration;

  directory = getenv ("RASH_HOOK_DIR");
  if (directory == 0 || *directory == '\0')
    {
      if (force)
	rash_hook_warning ("cannot reload hooks because RASH_HOOK_DIR is not set", 0);
      return 0;
    }
  allow = getenv ("RASH_ALLOW_UNOWNED_HOOKS");
  allow_unowned = allow && strcmp (allow, "1") == 0;
  enforce = getenv ("RASH_HOOK_ENFORCE_UNOWNED");
  enforce_unowned = enforce && strcmp (enforce, "1") == 0;
  same_configuration = hook_directory && strcmp (hook_directory, directory) == 0 &&
    hook_allow_unowned == allow_unowned &&
    hook_enforce_unowned == enforce_unowned;

  /* The normal path is deliberately one-shot.  Development mtime mode may
     also switch to a newly configured directory in the same process. */
  if (force == 0 && hook_state == 1 && rash_hook_reload_mode_enabled () == 0)
    return 1;

  if (force == 0 && same_configuration)
    {
      if (rash_hook_reload_mode_enabled () == 0 ||
	  rash_hook_manifest_matches (directory))
	return 1;
    }
  return rash_hooks_load_configuration (directory, allow_unowned, enforce_unowned);
}

void
rash_hooks_command_begin (void)
{
  if (hook_command_depth++ == 0)
    rash_hooks_initialize (0);
}

void
rash_hooks_command_end (void)
{
  if (hook_command_depth > 0)
    hook_command_depth--;
  if (hook_command_depth == 0)
    rash_hooks_close_retired_lua ();
}

void
rash_hooks_command_unwind (void *ignored)
{
  (void)ignored;
  rash_hooks_command_end ();
}

int
rash_hooks_reload (void)
{
  return rash_hooks_initialize (1) ? EXECUTION_SUCCESS : EXECUTION_FAILURE;
}

static void
rash_hooks_remove_builtin (struct builtin *builtin)
{
  size_t index;

  index = (size_t)(builtin - shell_builtins);
  if (index >= (size_t)num_shell_builtins)
    return;
  memmove (builtin, builtin + 1,
	   ((size_t)num_shell_builtins - index) * sizeof (*builtin));
  num_shell_builtins--;
}

void
rash_hooks_configure_builtin (void)
{
  const char *enabled;
  struct builtin *builtin;

  enabled = getenv ("RASH_HOOK_RELOAD_BUILTIN");
  builtin = builtin_address_internal ("reloadhooks", 1);
  if (builtin == 0)
    return;
  if (enabled && strcmp (enabled, "1") == 0)
    {
      hook_reload_builtin_enabled = 1;
      builtin->flags &= ~BUILTIN_DELETED;
      builtin->flags |= BUILTIN_ENABLED;
    }
  else
    {
      hook_reload_builtin_enabled = 0;
      rash_hooks_remove_builtin (builtin);
    }
}

static int
rash_hook_is_reload_builtin_command (COMMAND *command)
{
  WORD_LIST *words;

  if (hook_reload_builtin_enabled == 0 || command == 0 ||
      command->type != cm_simple)
    return 0;
  words = command->value.Simple->words;
  return words && words->word && words->word->word &&
    strcmp (words->word->word, "reloadhooks") == 0;
}

int
rash_hooks_active (void)
{
  if (running_trap != 0 || hook_execution_depth != 0)
    return 0;
  if (hook_command_depth == 0)
    rash_hooks_initialize (0);
  return hook_state == 1;
}

int
rash_hooks_execute (COMMAND *command, int asynchronous, int pipe_in, int pipe_out,
		    struct fd_bitmap *fds_to_close)
{
  RASH_HOOK_CONTEXT *context;
  int base, result;

  /* reloadhooks swaps the Lua state.  It must not run while a callback from
     that state is still on the C stack. */
  if (rash_hook_is_reload_builtin_command (command))
    {
      RASH_HOOK_CONTEXT direct;

      memset (&direct, 0, sizeof (direct));
      direct.command = command;
      direct.asynchronous = asynchronous;
      direct.pipe_in = pipe_in;
      direct.pipe_out = pipe_out;
      direct.fds_to_close = fds_to_close;
      direct.result = EXECUTION_FAILURE;
      result = rash_execute_original (&direct);
      rash_hooks_close_retired_lua ();
      return result;
    }

  base = lua_gettop (rash_lua);
  context = (RASH_HOOK_CONTEXT *)lua_newuserdata (rash_lua, sizeof (*context));
  context->command = command;
  context->asynchronous = asynchronous;
  context->pipe_in = pipe_in;
  context->pipe_out = pipe_out;
  context->fds_to_close = fds_to_close;
  context->active = 1;
  context->executed = 0;
  context->denied = 0;
  context->result = EXECUTION_FAILURE;
  context->lua_stack_base = base;
  context->deny_reason[0] = '\0';

  begin_unwind_frame ("rash-hook-command");
  add_unwind_protect (rash_unwind_hook_context, context);
  unwind_protect_int (hook_execution_depth);
  unwind_protect_int (rash_lua_call_depth);
  unwind_protect_int (rash_lua_instruction_ticks);
  result = rash_invoke_hook (rash_lua, context, 1);
  context->active = 0;
  lua_settop (rash_lua, base);
  discard_unwind_frame ("rash-hook-command");
  return result;
}

static void
rash_push_expanded_ctx (lua_State *L, WORD_LIST *words, int status,
			const char *captured_stdout, size_t stdout_len,
			const char *captured_stderr, size_t stderr_len)
{
  char *line;

  lua_newtable (L);
  rash_push_words (L, words);
  lua_setfield (L, -2, "words");
  line = string_list (words);
  if (line)
    {
      lua_pushstring (L, line);
      free (line);
    }
  else
    lua_pushliteral (L, "");
  lua_setfield (L, -2, "line");
  lua_pushinteger (L, status);
  lua_setfield (L, -2, "status");
  if (captured_stdout)
    lua_pushlstring (L, captured_stdout, stdout_len);
  else
    lua_pushnil (L);
  lua_setfield (L, -2, "stdout");
  if (captured_stderr)
    lua_pushlstring (L, captured_stderr, stderr_len);
  else
    lua_pushnil (L);
  lua_setfield (L, -2, "stderr");
}

int
rash_hooks_want_stdio_capture (void)
{
  /* May run under hook_execution_depth while parse-stage run() is active. */
  if (running_trap != 0)
    return 0;
  if (hook_command_depth == 0)
    rash_hooks_initialize (0);
  return hook_state == 1 && after_count > 0 && rash_lua != 0;
}

int
rash_hooks_before_simple (WORD_LIST *words)
{
  int i, base, status;

  /* Do not use rash_hooks_active(): that is false while parse-stage run() has
     raised hook_execution_depth, which is exactly when simple commands run. */
  if (running_trap != 0)
    return 0;
  if (hook_command_depth == 0)
    rash_hooks_initialize (0);
  if (hook_state != 1 || before_count == 0 || rash_lua == 0)
    return 0;

  stage_denied = 0;
  stage_deny_reason[0] = '\0';
  in_before_stage = 1;
  base = lua_gettop (rash_lua);

  for (i = 1; i <= before_count; i++)
    {
      current_hook_enforcing = before_enforcing && before_enforcing[i - 1];
      lua_rawgeti (rash_lua, LUA_REGISTRYINDEX, before_table_ref);
      lua_rawgeti (rash_lua, -1, i);
      lua_remove (rash_lua, -2);
      rash_push_expanded_ctx (rash_lua, words, 0, 0, 0, 0, 0);
      status = rash_lua_pcall (rash_lua, 1, 0);
      if (status != 0)
	{
	  const char *lua_error;

	  lua_error = lua_tostring (rash_lua, -1);
	  if (current_hook_enforcing)
	    {
	      rash_hook_warning ("enforcing before-hook failed; denying command: ",
				 lua_error ? lua_error : "(no error object)");
	      lua_pop (rash_lua, 1);
	      stage_denied = 1;
	      fprintf (stderr, "rash: denied: enforcing before-hook failed\n");
	    }
	  else
	    {
	      rash_hook_warning ("advisory before-hook failed; continuing: ",
				 lua_error ? lua_error : "(no error object)");
	      lua_pop (rash_lua, 1);
	    }
	}
      if (stage_denied)
	break;
    }

  lua_settop (rash_lua, base);
  in_before_stage = 0;
  current_hook_enforcing = 0;
  return stage_denied ? EXECUTION_FAILURE : 0;
}

void
rash_hooks_after_simple (WORD_LIST *words, int status,
			 const char *captured_stdout, size_t stdout_len,
			 const char *captured_stderr, size_t stderr_len)
{
  int i, base, rc;

  if (running_trap != 0)
    return;
  if (hook_command_depth == 0)
    rash_hooks_initialize (0);
  if (hook_state != 1 || after_count == 0 || rash_lua == 0)
    return;

  base = lua_gettop (rash_lua);
  for (i = 1; i <= after_count; i++)
    {
      current_hook_enforcing = after_enforcing && after_enforcing[i - 1];
      lua_rawgeti (rash_lua, LUA_REGISTRYINDEX, after_table_ref);
      lua_rawgeti (rash_lua, -1, i);
      lua_remove (rash_lua, -2);
      rash_push_expanded_ctx (rash_lua, words, status,
			      captured_stdout, stdout_len,
			      captured_stderr, stderr_len);
      rc = rash_lua_pcall (rash_lua, 1, 0);
      if (rc != 0)
	{
	  const char *lua_error;

	  lua_error = lua_tostring (rash_lua, -1);
	  rash_hook_warning ("after-hook failed: ",
			     lua_error ? lua_error : "(no error object)");
	  lua_pop (rash_lua, 1);
	}
    }
  lua_settop (rash_lua, base);
  current_hook_enforcing = 0;
}
