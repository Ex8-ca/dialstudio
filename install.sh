#!/bin/bash
# Dial Studio installer for Omarchy / Arch. Safe to re-run: it upgrades in place.
#   curl -fsSL https://raw.githubusercontent.com/Ex8-ca/dialstudio/main/install.sh | bash
set -euo pipefail

REPO="${DIALSTUDIO_REPO:-https://github.com/Ex8-ca/dialstudio}"
SRC="${DIALSTUDIO_SRC:-$HOME/.local/share/dialstudio/src}"

say() { printf '\033[1m==> %s\033[0m\n' "$*"; }
die() { printf 'install.sh: %s\n' "$*" >&2; exit 1; }

command -v pacman >/dev/null || die "this installer needs pacman (Arch or Omarchy)"
[ "$(id -u)" -ne 0 ] || die "run as your normal user; makepkg refuses to run as root"

BUILD_DEPS=(base-devel git qt6-base qt6-declarative qt6-svg qt6-wayland libxkbcommon)
RUN_DEPS=(bluez bluez-utils wtype lua)
MISSING=()
for pkg in "${BUILD_DEPS[@]}" "${RUN_DEPS[@]}"; do
  pacman -Qq "$pkg" >/dev/null 2>&1 || MISSING+=("$pkg")
done
if [ "${#MISSING[@]}" -gt 0 ]; then
  say "Installing build dependencies: ${MISSING[*]}"
  if command -v omarchy-pkg-add >/dev/null; then
    omarchy-pkg-add "${MISSING[@]}"
  else
    sudo pacman -S --needed "${MISSING[@]}"
  fi
fi

if [ -d "$SRC/.git" ]; then
  say "Updating $SRC"
  git -C "$SRC" fetch --quiet origin
  git -C "$SRC" reset --hard --quiet origin/HEAD
else
  say "Cloning $REPO"
  mkdir -p "$(dirname "$SRC")"
  git clone --quiet "$REPO" "$SRC"
fi

say "Building and installing the package"
(cd "$SRC/pkgbuild" && makepkg -fsi --noconfirm)

say "Enabling the daemon"
systemctl --user daemon-reload
systemctl --user enable dialstudio
systemctl --user restart dialstudio

say "Checking the setup"
sleep 2
dialstudio doctor || echo "doctor found problems (see above). Pair the dial first if it is missing."
