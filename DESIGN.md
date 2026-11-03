# Terminator design

## GUI and event loop

`src/app/app.cpp` owns the X11 display, window, event loop, and active-tab index.
The loop polls the X connection with a short timeout, dispatches keyboard and
mouse events, updates jobs, and redraws through `TerminalView`. Terminal output
is captured from child-process pipes and stored as lines in each `Tab`; only the
GUI process calls Xlib.
Tabs can be created with the configured hotkey or `+`, switched by mouse or
hotkey, and closed with the `x` on the tab. Closing a tab sends SIGHUP to only
that tab's jobs.
## Tab execution modes

The default mode starts one worker thread per tab. The GUI sends command
submissions to the matching worker; workers run shell built-ins and launch
command child processes. The GUI remains responsible for X events and drawing.
The shared command history is protected by a process-shared semaphore.

With `useProcess=True`, the GUI forks one shell worker process per tab. It
sends framed command and signal messages to each worker and receives framed
output over pipes. Each worker has its own tab state and `JobManager`; the GUI
parent alone uses Xlib. Command history lives in an anonymous shared mapping
and a named process-shared semaphore protects updates across tab processes.

## Working directories

Each `Tab` stores its title, output, input, scroll state, and working directory.
`cd` updates the shared directory used by every tab. `process::launch` changes
to that directory in each command child before applying redirections and
executing the command. In process mode the GUI synchronizes directory changes
with each tab worker so command substitutions and later commands use the same
directory.

## Process and job management

`shell::parse` turns input into a pipeline of commands and redirections.
`process::launch` creates one child per pipeline stage, connects the stages with
pipes, places them in a process group, and redirects their output to a capture
pipe. `JobManager` polls child status, drains output without blocking, and
implements `jobs`, `fg`, and `bg`. Foreground signals target the process group.

## History and configuration

`history_store` keeps commands entered during the current run in a shared
memory ring buffer. The `history` built-in prints the most recent entries, and
tabs share that list in either execution mode.
Hotkeys, including editing keys, are loaded from
`~/.config/terminator/hotkeys.conf`; invalid or conflicting bindings are
reported in the window and use defaults.

## Current limits

This implementation targets Tasks 1–8. It does not provide full Task 11 script
interpretation. Task 9 persistent searchable history and Task 10 filename
completion are out of scope. It also lacks deeply nested backtick substitution,
shell control operators, globbing, and a pseudo-terminal for interactive
full-screen programs. Commands run in child processes in both modes.
