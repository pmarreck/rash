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
  int result;
  int lua_stack_base;
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
static int hook_count;
static int hook_state;
static int hook_execution_depth;
static int hook_command_depth;
static int hook_reload_builtin_enabled;
static char *hook_directory;
static int hook_allow_unowned;
static RASH_HOOK_MANIFEST hook_manifest;
static int rash_lua_call_depth;
static int rash_lua_instruction_ticks;

static int rash_lua_hook (lua_State *);
static int rash_lua_warn (lua_State *);
static int rash_command_index (lua_State *);
static int rash_hook_run (lua_State *);
static void rash_lua_instruction_limit (lua_State *, lua_Debug *);
static int rash_lua_pcall (lua_State *, int, int);
static void rash_unwind_hook_context (void *);
static int rash_hooks_initialize (int);

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
rash_hook_save_configuration (const char *directory, int allow_unowned)
{
  free (hook_directory);
  hook_directory = savestring (directory);
  hook_allow_unowned = allow_unowned;
}

/* Build a complete replacement state before discarding an active hook set.
   A bad edit stays observable, but cannot turn off the prior observers. */
static int
rash_hooks_load_configuration (const char *directory, int allow_unowned)
{
  lua_State *previous_lua;
  int previous_count, previous_ref, previous_state, loaded, snapshotted;
  RASH_HOOK_MANIFEST manifest;

  memset (&manifest, 0, sizeof (manifest));
  previous_lua = rash_lua;
  previous_ref = hook_table_ref;
  previous_count = hook_count;
  previous_state = hook_state;
  rash_lua = 0;
  hook_table_ref = LUA_NOREF;
  hook_count = 0;

  loaded = rash_lua_ready () && rash_load_hooks (directory, allow_unowned);
  snapshotted = rash_hook_manifest_collect (directory, &manifest);
  if (snapshotted == 0)
    rash_hook_warning ("cannot snapshot hook directory '", directory);

  if (loaded && snapshotted)
    {
      hook_state = hook_count > 0 ? 1 : -1;
      rash_hook_save_configuration (directory, allow_unowned);
      rash_hook_manifest_replace (&manifest);
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
  rash_lua = previous_lua;
  hook_table_ref = previous_ref;
  hook_count = previous_count;
  hook_state = previous_lua ? previous_state : -1;
  rash_hook_save_configuration (directory, allow_unowned);
  if (snapshotted)
    rash_hook_manifest_replace (&manifest);
  return 0;
}

static int
rash_hooks_initialize (int force)
{
  const char *allow, *directory;
  int allow_unowned, same_configuration;

  directory = getenv ("RASH_HOOK_DIR");
  if (directory == 0 || *directory == '\0')
    {
      if (force)
	rash_hook_warning ("cannot reload hooks because RASH_HOOK_DIR is not set", 0);
      return 0;
    }
  allow = getenv ("RASH_ALLOW_UNOWNED_HOOKS");
  allow_unowned = allow && strcmp (allow, "1") == 0;
  same_configuration = hook_directory && strcmp (hook_directory, directory) == 0 &&
    hook_allow_unowned == allow_unowned;

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
  return rash_hooks_load_configuration (directory, allow_unowned);
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
