# Installation, upgrades and removal

**English** | [简体中文](install.md) · [Home](../README.md)

The recommended installation builds from source into `~/.local/bin`. It needs Linux, Bash, coreutils, Make, a C11 compiler and ncursesw development files. Git is needed for checkout-based upgrades.

## Ubuntu

```sh
sudo apt install git build-essential pkg-config libncursesw5-dev
git clone https://github.com/cossoledad/PortTop.git
cd PortTop
./install.sh
export PATH="$HOME/.local/bin:$PATH"
porttop
```

The installer prints PATH advice without editing your shell configuration. Add the export line to `~/.bashrc` or `~/.zshrc` to keep it. Installation creates `bin/porttop`, `bin/porttop-update` and `share/porttop/install.manifest` beneath the prefix. The manifest records hashes for guarded removal.

To view other users, run `sudo "$HOME/.local/bin/porttop"`; sudo may omit your user bin directory from PATH.

## Upgrade

```sh
porttop-update
# Or, from the original checkout:
./install.sh --upgrade
```

Keep the source checkout in place. The helper updates its current branch from its upstream with `git pull --ff-only`. It rejects local modifications, untracked files, missing upstreams and diverged branches. It does not reset, clean or automatically merge your work.

Each installation builds in a fresh directory, verifies `--version`, stages the result, and replaces each installed file atomically. A failed build leaves the installed binary intact; the source checkout may already have updated. Replacement is per file, not a transaction across all installed files.

For a source archive, download the new source and run `./install.sh` again. Running it from a new checkout also repoints the upgrade helper. Updates are explicit; there are no background downloads.

## Custom prefix and system installation

```sh
./install.sh --prefix "$HOME/tools/porttop"
sudo ./install.sh --system                  # /usr/local by default
```

The prefix must be an absolute path other than `/`; spaces are supported. Keep it outside the source checkout. Root installation requires explicit `--system`; subsequent upgrades use `sudo /usr/local/bin/porttop-update`.

`make install` / `make uninstall` are binary-only packaging interfaces supporting `PREFIX` and `DESTDIR`. They do not create an upgrade helper or manifest. Use the matching Make target to remove such an installation.

## Uninstall

```sh
/path/to/PortTop/install.sh --uninstall
# For a custom prefix, repeat --prefix; for a system install, use sudo and --system.
```

Removal checks the manifest and hashes first, then removes only the two installed programs and the manifest. Modified files are preserved with an error. Source files, other programs and shell configuration are kept.

## Troubleshooting

- Missing compiler or ncurses headers: install the dependencies above and retry; build diagnostics are preserved.
- Moved checkout: run `./install.sh` from its new location to recreate the helper.
- Uncommitted development build: use `./install.sh` directly; only `--upgrade` requires a clean checkout.
- Incorrect `TERM`: set a valid terminal type, or use `--once` / `--json` for batch output.

CI archive contents and compatibility limits are documented in the [distribution guide](distribution.md).
