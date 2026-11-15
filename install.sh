#!/usr/bin/env bash
# Build/install a checkout without sudo; upgrades are explicit and fast-forward only.
set -euo pipefail

usage() {
    cat <<'HELP'
PortTop installer
Usage: ./install.sh [--prefix ABSOLUTE_PATH] [--upgrade | --uninstall] [--system]

Default: build and install to $HOME/.local/bin without sudo.
  --upgrade    Fast-forward this clean Git checkout, rebuild, then replace binaries.
  --uninstall  Remove this installer's binaries after checking their hashes.
  --prefix     Set an installation prefix (the executables go in PREFIX/bin).
  --system     Explicit system install; default prefix /usr/local, requires root.
  -h, --help   Show this help.

Dependencies (Ubuntu): sudo apt install build-essential pkg-config libncursesw5-dev
Keep the checkout: porttop-update uses it for future upgrades.
HELP
}
fail() { printf 'porttop installer: %s\n' "$*" >&2; exit 1; }
mode=install
system_install=false
prefix_choice=''
while (($#)); do
    case "$1" in
        --prefix) (($#>=2)) || fail '--prefix requires a path'; [[ -z $prefix_choice ]] || fail 'duplicate --prefix'; [[ -n $2 ]] || fail '--prefix requires a nonempty path'; prefix_choice=$2; shift 2 ;;
        --upgrade|--uninstall) [[ $mode == install ]] || fail 'choose only one operation'; mode=${1#--}; shift ;;
        --system) system_install=true; shift ;;
        -h|--help) usage; exit 0 ;;
        *) fail "unknown argument: $1" ;;
    esac
done
[[ $(uname -s) == Linux ]] || fail 'Linux is required'
if $system_install; then
    ((EUID==0)) || fail 'use sudo ./install.sh --system for an explicit system install'
    prefix=${prefix_choice:-/usr/local}
else
    ((EUID!=0)) || fail 'run without sudo for a user install, or pass --system'
    prefix=${prefix_choice:-${HOME:?HOME is required}/.local}
fi
[[ $prefix == /* && $prefix != / && $prefix != *$'\n'* && $prefix != *$'\r'* ]] || fail 'prefix must be an absolute directory other than /'
command -v realpath >/dev/null || fail 'missing realpath (coreutils)'
prefix=$(realpath -m -- "$prefix")
[[ $prefix != / ]] || fail 'prefix must not resolve to /'
source_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
manifest="$prefix/share/porttop/install.manifest"
for dependency in install mktemp sha256sum; do
    command -v "$dependency" >/dev/null || fail "missing command: $dependency"
done

if [[ $mode == uninstall ]]; then
    [[ -f $manifest && ! -L $manifest ]] || fail "no installer manifest at $manifest"
    mapfile -t record < "$manifest"
    [[ ${#record[@]} == 3 && ${record[0]} == porttop-install-v1 ]] || fail 'invalid installation manifest'
    for i in 1 2; do
        if ((i==1)); then file="$prefix/bin/porttop"; else file="$prefix/bin/porttop-update"; fi
        [[ -f $file && ! -L $file ]] || fail "missing or replaced file: $file"
        digest=$(sha256sum < "$file"); digest=${digest%% *}
        [[ $digest == "${record[$i]}" ]] || fail "file changed since installation; leaving it in place: $file"
    done
    rm -- "$prefix/bin/porttop" "$prefix/bin/porttop-update" "$manifest"
    rmdir -- "$prefix/share/porttop" 2>/dev/null || true
    printf 'Removed PortTop from %s/bin\n' "$prefix"
    exit 0
fi

if [[ $mode == upgrade ]]; then
    command -v git >/dev/null || fail 'git is required for upgrades'
    [[ $(git -C "$source_root" rev-parse --show-toplevel 2>/dev/null) == "$source_root" ]] || fail 'upgrades need the original Git checkout; install new source archives by rerunning install.sh'
    [[ -z $(git -C "$source_root" status --porcelain --untracked-files=normal) ]] || fail 'checkout has local changes; commit or move them before upgrading'
    git -C "$source_root" rev-parse --verify '@{upstream}' >/dev/null 2>&1 || fail 'current branch has no upstream configured'
    git -C "$source_root" pull --ff-only
    args=(--prefix "$prefix")
    if $system_install; then args+=(--system); fi
    exec bash "$source_root/install.sh" "${args[@]}"
fi

for dependency in make "${CC:-cc}"; do
    command -v "$dependency" >/dev/null || fail "missing $dependency; on Ubuntu: sudo apt install build-essential pkg-config libncursesw5-dev"
done
# Isolated build: never reuse objects left by a sanitizer or debug build.
mkdir -p -- "$source_root/build"
build_stage=$(mktemp -d "$source_root/build/install.XXXXXX")
file_stage=''
cleanup() {
    rm -rf -- "$build_stage"
    if [[ -n $file_stage ]]; then rm -rf -- "$file_stage"; fi
}
trap cleanup EXIT
if ! make -C "$source_root" "BUILD_DIR=build/${build_stage##*/}" all; then
    fail 'build failed; on Ubuntu install build-essential pkg-config libncursesw5-dev; existing installation unchanged'
fi
"$build_stage/porttop" --version
mkdir -p -- "$prefix/bin" "$prefix/share/porttop"
file_stage=$(mktemp -d "$prefix/bin/.porttop-install.XXXXXX")
install -m755 -- "$build_stage/porttop" "$file_stage/porttop"
{
    printf '#!/usr/bin/env bash\nset -euo pipefail\n'
    printf 'exec bash %q --upgrade --prefix %q ' "$source_root/install.sh" "$prefix"
    if $system_install; then printf '%s ' --system; fi
    printf '"$@"\n'
} > "$file_stage/porttop-update"
chmod 755 -- "$file_stage/porttop-update"
{
    printf 'porttop-install-v1\n'
    digest=$(sha256sum < "$file_stage/porttop"); printf '%s\n' "${digest%% *}"
    digest=$(sha256sum < "$file_stage/porttop-update"); printf '%s\n' "${digest%% *}"
} > "$file_stage/install.manifest"
# Stage everything first; each rename replaces a complete file on the same filesystem.
mv -fT -- "$file_stage/porttop" "$prefix/bin/porttop"
mv -fT -- "$file_stage/porttop-update" "$prefix/bin/porttop-update"
install -m644 -- "$file_stage/install.manifest" "$manifest"
printf '\nInstalled: %s/bin/porttop\nUpgrade:   %s/bin/porttop-update\nUninstall: bash %q --prefix %q --uninstall' "$prefix" "$prefix" "$source_root/install.sh" "$prefix"
if $system_install; then printf ' --system'; fi
printf '\n'
case ":$PATH:" in
    *":$prefix/bin:"*) ;;
    *) printf '\nFor this shell, run: export PATH=%q:"$PATH"\nAdd the same line to your shell startup file to keep it.\n' "$prefix/bin" ;;
esac
