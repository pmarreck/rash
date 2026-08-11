/* hooks.c -- lazy LuaJIT lifecycle hooks over unexpanded shell commands. */

#include <config.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
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
#include "hooks.h"
#include "trap.h"
#include "unwind_prot.h"
#include "xmalloc.h"

#define RASH_COMMAND_METATABLE "rash.command"
#define RASH_HOOK_MAX_BYTES (1024 * 1024)
#define RASH_HOOK_INSTRUCTION_TICKS 100
#define RASH_HOOK_MAX_TICKS 1000

typedef struct {
  COMMAND *command;
  int asynchronous;
  int pipe_in;
  int pipe_out;
  struct fd_bitmap *fds_to_close;
  int active;
  int executed;
  int result;
  int lua_stack_base;
} RASH_HOOK_CONTEXT;

typedef struct {
  COMMAND *command;
} RASH_COMMAND_PROXY;

typedef struct {
  RASH_HOOK_CONTEXT *context;
  int next_hook;
  int called;
} RASH_HOOK_RUNNER;

static lua_State *rash_lua;
static int hook_table_ref = LUA_NOREF;
static int hook_count;
static int hook_state;
static int hook_execution_depth;
static char *hook_directory;
static int hook_allow_unowned;
static int rash_lua_call_depth;
static int rash_lua_instruction_ticks;

static int rash_lua_hook (lua_State *);
static int rash_lua_warn (lua_State *);
static int rash_command_index (lua_State *);
static int rash_hook_run (lua_State *);
static void rash_lua_instruction_limit (lua_State *, lua_Debug *);
static int rash_lua_pcall (lua_State *, int, int);
static void rash_unwind_hook_context (void *);

static void
rash_hook_warning (const char *prefix, const char *path)
{
  fprintf (stderr, "rash: warning: %s%s\n", prefix, path ? path : "");
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

  lua_newtable (L);
  for (redirect = redirects, index = 1; redirect; redirect = redirect->next, index++)
    {
      lua_newtable (L);
      lua_pushboolean (L, INPUT_REDIRECT (redirect->instruction));
      lua_setfield (L, -2, "is_input");
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
      if (command->value.Connection->connector == '|')
	lua_pushliteral (L, "|");
      else
	lua_pushnil (L);
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
rash_lua_hook (lua_State *L)
{
  size_t count;

  luaL_checktype (L, 1, LUA_TFUNCTION);
  lua_rawgeti (L, LUA_REGISTRYINDEX, hook_table_ref);
  count = lua_objlen (L, -1);
  lua_pushvalue (L, 1);
  lua_rawseti (L, -2, (int)count + 1);
  lua_pop (L, 1);
  hook_count++;
  return 0;
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

/* Invokes hooks as Rack-style middleware.  An advisory hook that errors or
   returns without run() cannot suppress later observers or the command. */
static int
rash_invoke_hook (lua_State *L, RASH_HOOK_CONTEXT *context, int index)
{
  RASH_HOOK_RUNNER *runner;
  int base, status;

  if (index > hook_count)
    return rash_execute_original (context);

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
      rash_hook_warning ("advisory hook failed; continuing command execution", 0);
      lua_pop (L, 1);
    }
  lua_settop (L, base);

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

  lua_newtable (L);
  lua_pushcfunction (L, rash_lua_hook);
  lua_setfield (L, -2, "hook");
  lua_pushcfunction (L, rash_lua_warn);
  lua_setfield (L, -2, "warn");
  lua_setglobal (L, "rash");

  luaL_newmetatable (L, RASH_COMMAND_METATABLE);
  lua_pushcfunction (L, rash_command_index);
  lua_setfield (L, -2, "__index");
  lua_pop (L, 1);

  lua_newtable (L);
  hook_table_ref = luaL_ref (L, LUA_REGISTRYINDEX);
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

static void
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
      return;
    }
  if (fstat (fd, &st) < 0 || S_ISREG (st.st_mode) == 0)
    {
      close (fd);
      rash_hook_warning ("refusing non-regular hook '", path);
      return;
    }
  if (st.st_size < 0 || st.st_size > RASH_HOOK_MAX_BYTES)
    {
      close (fd);
      rash_hook_warning ("refusing oversized hook '", path);
      return;
    }

  trusted = st.st_uid == 0 && (st.st_mode & (S_IWGRP | S_IWOTH)) == 0;
  if (trusted == 0 && allow_unowned == 0)
    {
      close (fd);
      rash_hook_warning ("refusing unowned hook; set RASH_ALLOW_UNOWNED_HOOKS=1 to load mutable advisory code: ", path);
      return;
    }
  if (trusted == 0)
    rash_hook_warning ("WARNING: RASH_ALLOW_UNOWNED_HOOKS=1 loads mutable hook as advisory code: ", path);

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
	  return;
	}
      offset += (size_t)got;
    }
  close (fd);
  source[offset] = '\0';

  status = luaL_loadbuffer (rash_lua, source, offset, path);
  free (source);
  if (status == 0)
    status = rash_lua_pcall (rash_lua, 0, 0);
  if (status != 0)
    {
      lua_pop (rash_lua, 1);
      rash_hook_warning ("cannot load hook '", path);
    }
}

static void
rash_load_hooks (const char *directory, int allow_unowned)
{
  struct dirent *entry;
  char **names;
  char *path;
  DIR *dir;
  size_t allocated, count, i, path_length;

  dir = opendir (directory);
  if (dir == 0)
    {
      rash_hook_warning ("cannot open hook directory '", directory);
      return;
    }

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
  for (i = 0; i < count; i++)
    {
      path_length = strlen (directory) + strlen (names[i]) + 2;
      path = (char *)xmalloc (path_length);
      sprintf (path, "%s/%s", directory, names[i]);
      rash_load_hook (path, allow_unowned);
      free (path);
      free (names[i]);
    }
  free (names);
}

static void
rash_hooks_initialize (void)
{
  const char *allow, *directory;
  int allow_unowned;

  directory = getenv ("RASH_HOOK_DIR");
  if (directory == 0 || *directory == '\0')
    return;
  allow = getenv ("RASH_ALLOW_UNOWNED_HOOKS");
  allow_unowned = allow && strcmp (allow, "1") == 0;
  if (hook_state == 1 ||
      (hook_state == -1 && hook_directory &&
       strcmp (hook_directory, directory) == 0 &&
       hook_allow_unowned == allow_unowned))
    return;

  hook_state = -1;
  if (rash_lua == 0 && rash_lua_ready () == 0)
    goto save_configuration;
  rash_load_hooks (directory, allow_unowned);
  if (hook_count > 0)
    hook_state = 1;

save_configuration:
  free (hook_directory);
  hook_directory = savestring (directory);
  hook_allow_unowned = allow_unowned;
}

int
rash_hooks_active (void)
{
  if (running_trap != 0)
    return 0;
  rash_hooks_initialize ();
  return hook_state == 1 && hook_execution_depth == 0;
}

int
rash_hooks_execute (COMMAND *command, int asynchronous, int pipe_in, int pipe_out,
		    struct fd_bitmap *fds_to_close)
{
  RASH_HOOK_CONTEXT *context;
  int base, result;

  base = lua_gettop (rash_lua);
  context = (RASH_HOOK_CONTEXT *)lua_newuserdata (rash_lua, sizeof (*context));
  context->command = command;
  context->asynchronous = asynchronous;
  context->pipe_in = pipe_in;
  context->pipe_out = pipe_out;
  context->fds_to_close = fds_to_close;
  context->active = 1;
  context->executed = 0;
  context->result = EXECUTION_FAILURE;
  context->lua_stack_base = base;

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
