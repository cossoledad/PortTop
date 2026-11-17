# Contributing to PortTop

**English** | [简体中文](docs/development.md) · [Home](README.md)

PortTop is a C11 / ncursesw application for Linux. Issues and pull requests are welcome. For bugs, include the version, distribution, kernel, terminal, locale and a minimal reproduction. Redact credentials or private paths from command lines and screenshots.

## Build and run

On Ubuntu:

```sh
sudo apt install build-essential pkg-config libncursesw5-dev python3 git
make -j2
./build/porttop
```

Use `CC=clang` to select Clang. Build products belong in the ignored `build/` directory; `BUILD_DIR` can isolate different compiler configurations. Python 3 is needed for tests and packaging, not at runtime.

## Verify a change

```sh
make test                 # C core checks, real sockets/process trees and PTY interaction
make test-install         # Isolated install, upgrade, failure recovery and uninstall
make sanitize             # Same core/integration tests with ASan and UBSan
make test-package         # Build, verify, extract and run the distribution archive
```

The sanitizer target uses `build/asan` and preserves the normal executable. Tests signal only their own fixtures. IPv6 socket cases are skipped when IPv6 is unavailable. Installer tests use a temporary HOME and a local Git remote; they do not modify your user installation or contact GitHub. Diagnostics are written under `build/`.

Run `make test` for behavior changes and the relevant installer/package checks for changes to those paths. For UI changes, inspect the terminal at 80 × 24 and 120 × 36 as well as the automated PTY checks. Describe what you tested in your pull request, including anything left unverified.

## Source map

| Path | Responsibility |
| --- | --- |
| `src/main.c` | CLI, terminal detection and entry point |
| `src/proc.c` | Socket ownership, process data, filtering and snapshots |
| `src/actions.c` | Process identity checks and finite subtree termination |
| `src/history.c` | Host and process history |
| `src/text_view.c`, `src/ui.c` | Scrollable text and ncurses rendering/input |
| `tests/` | Core, real process/socket and terminal acceptance |
| `install.sh`, `scripts/package.py` | User installation and distribution archives |

Keep process identity as PID plus start time. Preserve preflight checks, ancestor protection and explicit confirmation for termination. Compose each frame through the ncurses virtual screen with a single `doupdate()`; avoid clearing the physical screen on refresh.

Detailed [architecture](docs/architecture.md) and historical [validation notes](docs/validation.md) are currently in Chinese. The [distribution guide](docs/distribution.md) describes CI artifacts and the current release boundary.
