# Terminator

Terminator is a small X11 terminal project written in C++17.

## Build and run

XQuartz must be installed and running on macOS. The Makefile uses `/opt/X11` by
default; override `X11_PREFIX` if XQuartz is installed somewhere else.

```sh
make
./terminator
./terminator useProcess=True
```

The default launch mode creates a worker thread for each tab. The optional
`useProcess=True` argument creates one shell worker process per tab; the parent
process owns the X11 window and exchanges commands and output with tab workers
through pipes.

Type a command in the window and press Return. Use Shift+Return to insert a
newline. An unfinished quote or trailing backslash also continues onto another
line when Return is pressed.

Browse command history with Up/Down. Scroll through output with the mouse wheel
or Page Up/Down; Page Up/Down moves by most of a screen. Use horizontal wheel buttons 6/7, Shift+wheel,
or Ctrl+Page Up/Down to scroll sideways across long lines. Scroll position is
kept separately for each tab. While typing a long command, the view follows the
input cursor so the newest character stays visible.

Drag with the left mouse button to select output text. The selection is copied
to the X11 clipboard on release; Ctrl+Shift+C copies again, and Ctrl+Shift+V
pastes at the current prompt. Middle-click pastes the PRIMARY selection.

## Implemented features

1. X11 window with clickable tabs.
2. External command execution with `fork`, `execvp`, and `waitpid`.
3. Quoted arguments, escaped characters, multiline commands, environment
   variable expansion, backtick substitution, and `$(...)` command substitution.
4. Input and output redirection with `<`, `>`, `>>`, and `2>`.
5. Pipelines with `|`.
6. Background jobs provide concurrent live output. `multiWatch ["cmd1",
   "cmd2"]` runs and repeats commands concurrently every two seconds, using
   the required timestamp and separator format until Ctrl+C.
7. Default hotkeys can be changed in
   `~/.config/terminator/hotkeys.conf`. Each line uses `action=modifiers+key`,
   for example `new_tab=ctrl+shift+t` or `quit=ctrl+q`. Editing actions are
   configurable there too; invalid or conflicting bindings are reported in
   the window and fall back to their defaults. Defaults include Left/Right,
   Ctrl+A/E, Up/Down history, Ctrl+U/K/W/L/D, and Ctrl+Shift+C/V.
8. `Ctrl+C` sends SIGINT and `Ctrl+Z` sends SIGTSTP to the foreground process
   group. The `jobs`, `fg`, and `bg` commands manage jobs; `fg` and `bg` accept
   job numbers in the form shown by `jobs` (for example `fg %1`).
9. Tabs share a working directory. `cd` changes the directory used by every
   tab, and child processes start there.
10. `history` displays commands entered during the current run. Tabs share this
    in-memory history.

The shell includes `exit`, `history`, `source` for scripts made from supported
commands, and `$?`. Input supports cursor movement, history browsing, line
editing, clipboard shortcuts, tab close, and tab switching. Tabs share a
working directory and keep their own active jobs. Closing a tab sends
SIGHUP only to that tab's jobs.

This repository targets Tasks 1–8 in the PDF. Task 9's persistent searchable
history, Task 10's filename completion, and Task 11's conditional bash-script
interpreter are outside this scope. Known limitations include full `if`/`elif`
script interpretation and deeply nested backtick substitution. The command parser also does not support shell operators
such as `;`, `&&`, or `||`, glob expansion, or a pseudo-terminal for interactive
full-screen programs.

## Source layout

- `src/app/` — X11 lifecycle, event loop, and application entry point.
- `src/terminal/` — tab state, rendering, scrolling, and clipboard support.
- `src/shell/` — command parsing, input submission, and shell built-ins.
- `src/jobs/` — process creation, job management, and per-tab worker processes.
- `src/config/` — user-configurable hotkeys.
- `src/history/` — shared, semaphore-protected command history.

Commands run in child processes, with process groups for pipelines and job
signals. Tabs use worker threads by default or isolated shell worker processes
when launched with `useProcess=True`.
