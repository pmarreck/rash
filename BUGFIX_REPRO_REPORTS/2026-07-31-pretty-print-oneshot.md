# `--pretty-print` executes the command instead of printing it when `-c` is used

Reproduced on bash 5.3.15(1)-release, x86_64-pc-linux-gnu.

## Summary

`--pretty-print` writes a parsed command to standard output instead of running
it. That works when input comes from a script file or standard input, but with
`-c` the option has no effect at all: the command is executed and never
printed. The difference is not intentional — it is a side effect of the
`ONESHOT` compile-time optimization.

## Reproduction

```sh
$ bash --pretty-print -c 'echo hi > /tmp/canary'
$ cat /tmp/canary
hi                          # the command ran; nothing was printed

$ echo 'echo hi > /tmp/canary2' | bash --pretty-print
echo hi > /tmp/canary2      # printed, not executed

$ printf '%s\n' 'echo hi > /tmp/canary3' > /tmp/s.sh; bash --pretty-print /tmp/s.sh
echo hi > /tmp/canary3      # printed, not executed
```

Only the `-c` form executes.

## Cause

`config-top.h` defines `ONESHOT` as an efficiency measure only:

```c
/* Define ONESHOT if you want sh -c 'command' to avoid forking to execute
   `command' whenever possible.  This is a big efficiency improvement. */
#define ONESHOT
```

With `ONESHOT` defined, the `-c` branch of `main` in `shell.c` runs the command
and exits before `pretty_print_mode` is ever consulted:

```c
  if (command_execution_string)
    {
      ...
#if defined (ONESHOT)
      executing = shell_initialized = 1;
      run_one_command (command_execution_string);
      exit_shell (last_command_exit_value);
#else /* ONESHOT */
      with_input_from_string (command_execution_string, "-c");
      goto read_and_execute;
#endif /* !ONESHOT */
    }
```

`pretty_print_mode` is only tested further down, after the `read_and_execute`
label, which the `ONESHOT` path never reaches. Building the identical source
with `ONESHOT` undefined makes `--pretty-print -c` print without executing, so
the two build configurations disagree on whether a command runs.

An optimization documented purely as fork avoidance should not determine
whether a command is executed.

## Why `-c` should not be exempt

`bash -n -c 'echo hi > /tmp/canary'` does not execute the command. `-n` is
enforced at both parse loops — `builtins/evalstring.c` for the `-c` path and
`eval.c` for the reader loop — so it covers every input route regardless of
`ONESHOT`. `pretty_print_mode` is enforced at one site only. The precedent that
a do-not-execute mode overrides `-c` is therefore already established in the
tree; `--pretty-print` simply misses an enforcement site.

Separately, the printed representation and the command's own output both go to
standard output, so printing and executing cannot meaningfully be combined.

## Suggested fix

Skip the `ONESHOT` fast path when a printing mode is active, so `-c` uses the
same reader-loop path a non-`ONESHOT` build already uses. This adds no new
printing or parsing machinery:

```c
#if defined (ONESHOT)
      if (pretty_print_mode == 0)
	{
	  executing = shell_initialized = 1;
	  run_one_command (command_execution_string);
	  exit_shell (last_command_exit_value);
	}
#endif /* ONESHOT */
      with_input_from_string (command_execution_string, "-c");
      goto read_and_execute;
```

The `read_and_execute:` label is currently inside `#if !defined (ONESHOT)` and
must become unconditional for the `goto` to compile in both configurations.

## Two related documentation observations

`--pretty-print` appears in the `--help` usage output but is documented in
neither `doc/bash.1` nor `doc/bashref.texi`.

When `--pretty-print` is ignored because the shell is interactive, bash emits
`pretty-printing mode ignored in interactive shells`. When it is ignored
because `-c` was used, it emits nothing. If the `-c` case is intended to remain
as-is, a matching diagnostic would at least make the behavior discoverable.
