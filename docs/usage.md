# Usage

**English** | [简体中文](usage.zh-CN.md) · [Home](../README.md)

See [installation](install.en.md) for setup and upgrades. Use a terminal of at least 80 × 24, preferably 120 × 36, with a valid `TERM`. `NO_COLOR=1` or `--no-color` disables colors; `LC_ALL=C` selects ASCII drawing.

## Command line

```sh
porttop --port 18080       # Exact local port
porttop --filter python    # Command, PID, user, address or port
porttop --all              # Include connected TCP/UDP sockets
porttop --sort cpu         # Descending CPU; requires two interactive samples
porttop --once             # Text snapshot
porttop --json             # JSON snapshot
porttop --help
```

Non-terminal stdin or stdout automatically selects a text snapshot. JSON stdout contains only JSON. A single snapshot has no CPU difference sample; it reports `cpu_sampled: false`. An unidentifiable socket owner is `null`.

## Controls

| Key | Action |
| --- | --- |
| ↑ / ↓, `j` down | Select a port/process; lowercase `k` is reserved for termination |
| PgUp / PgDn, Home / End | Page or jump to the beginning/end |
| `/` | Live filter; Enter applies, Esc cancels editing |
| Esc | Clear an applied filter; a CLI `--port` constraint remains |
| `s` / `r` | Cycle port/PID/CPU/RSS sorting / reverse order |
| `a` | Toggle listeners + unconnected UDP / all sockets |
| Space / `R` | Pause sampling / refresh now |
| `c` / `g` | Page logical-core meters / switch graph scales |
| Enter / `i` | Open process information |
| `k` / `t` | Open the SIGTERM confirmation dialog |
| `K` | Open the SIGKILL confirmation dialog |
| `?` / `q` | Help / quit |

## Process information

Enter or `i` opens a read-only snapshot in a preferred 110 × 28 dialog, capped to the terminal size. Long content wraps. Scroll with the mouse wheel (three lines per event), arrows, `j`, PgUp/PgDn or Home/End. Esc, `q`, `i` or Enter closes it.

The dialog contains PID/PPID, UID/user, CPU/RSS, state, command, working directory, executable, process group, session, TTY, start ticks, visible ports in the current collection mode and the parent chain. Sampling pauses while open; the captured identity remains readable if the process exits. No termination keys operate inside this dialog. `i` also opens it from the termination preview and returns to the previous confirmation state.

## Graphs

System CPU is the average across logical cores, 0–100%; each core meter uses the same range. Process CPU follows top: one fully used core is 100%, so a multithreaded process can exceed 100%.

System memory is `MemTotal − MemAvailable`; process memory is RSS. Host histories always use a 0–100% scale. By default, the selected process uses independent axes: CPU starts at 100%, RSS at 32 MiB, doubling to fit retained peaks. The legend shows these limits. Press `g` for shared host-capacity axes: process CPU divided by the core count and RSS divided by total memory.

The history retains 240 successful samples per PID + start time, with up to two samples per character in UTF-8 mode. The purple line represents the selected process; grid lines mark host percentages. Missing process samples leave gaps. Switching rows never joins different process histories. Initial CPU differences are unavailable; pauses, dialogs and failed collections add no samples. The horizontal axis is a sample sequence, with the visible samples' elapsed span shown in the title.

## Stop a service launched by a script

For `launcher → supervisor → server:18080`, killing the server alone may trigger a restart. Select the port and press `k`:

1. The initial target is the port owner, scope **Process only**, with **Cancel** focused.
2. Use ↑ / ↓ to choose an ancestor or return to its child. Selecting an ancestor defaults to **Subtree**; `s` toggles the scope.
3. Review affected processes and visible ports, including descendants without ports. Scroll the preview with the wheel or page keys; `i` shows the selected target's full details. A supervisor's subtree may include its other services.
4. Choose **Terminate** with Tab or ← / → and press Enter, or confirm with `y`. Esc, `n`, `q` or Enter on Cancel aborts.

The preview freezes a finite set of PID/start-time identities. All targets must pass pidfd preflight before sending begins. PID 1, PortTop and its own ancestors are protected. Signals go parent-first to the reviewed processes, without process-group kills. Successful sending does not prove exit.

New descendants, detached processes and supervisors outside the selected tree are outside that finite set. To stop one service while preserving its siblings under a restarting supervisor, use that supervisor's own control interface.

## Visibility and data limits

- Non-root users see only identified owners with their effective UID; root sees all visible owners, including unknown ones. `--all` changes socket states, not this scope. System graphs remain system-wide. UID comes from `/proc/<pid>` ownership, not historical launcher tracking. Root remains subject to namespaces, LSM and proc mount restrictions.
- Collection covers TCP/TCP6/UDP/UDP6 in the current network namespace, excluding Unix sockets. Snapshots are non-atomic; processes and sockets can disappear during a scan. Diagnostics count the raw collection, including rows hidden by filtering.
- Command text joins argv with spaces; it is not a lossless shell command. Collection is capped at 16 KiB with a truncation marker. Control and non-ASCII bytes in user data become `?`. Executed-away commands and original script text cannot be recovered.
- A process's RSS repeats across its port rows and must not be summed. The ancestor chain contains the owner and up to 11 ancestors, stopping at protected ancestors or a terminal session leader. Subtree planning rejects scans beyond 16,384 processes and reports unreadable records.
- Monitoring remains available without pidfd support; termination reports the reason it cannot proceed. There is no automatic escalation or automatic termination of parents.

The interface keeps selection by socket/process identity and uses ncurses differential updates to reduce redraw flicker. Resizing recomputes the layout and centers dialogs; a terminal too small for confirmation disables that action until resized.
