#!/usr/bin/env bash
#
# Install Dori (this Nautilus fork) over the system GNOME Files.
#
# Run it WITHOUT sudo:
#
#     ./install.sh
#
# It builds as your user (no root), then asks for your password ONCE for the
# privileged part: install into /usr, refresh the GLib/icon/desktop caches,
# and pin the distro `nautilus` package so a `dnf upgrade` won't revert it.
#
# The fork keeps Nautilus' own identity (binary `nautilus`, app id
# `org.gnome.Nautilus`) on purpose, so it replaces GNOME Files cleanly and
# nothing in GNOME (Shell, file handlers, search provider) breaks.
#
set -euo pipefail

REPO_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$REPO_DIR"

BUILD_DIR="_build_install"

# --- Build as the current user (no sudo) ------------------------------------
if [ ! -d "$BUILD_DIR" ]; then
    meson setup "$BUILD_DIR" -Dprefix=/usr -Dtests=none \
        -Dintrospection=false -Ddocs=false -Dextensions=false
fi
ninja -C "$BUILD_DIR"

if [ ! -x "$BUILD_DIR/src/nautilus" ]; then
    echo "error: build did not produce src/nautilus." >&2
    exit 1
fi

# --- One privileged block: install + refresh caches + pin updates -----------
echo ">> Installing over /usr — you'll be asked for your password once."
sudo sh -s "$REPO_DIR/$BUILD_DIR" <<'ROOT'
set -e
BUILD="$1"
# --no-rebuild: install only, never recompile as root (otherwise root-owned
# artifacts land in the build dir and break later user builds).
meson install --no-rebuild -C "$BUILD"
glib-compile-schemas /usr/share/glib-2.0/schemas
gtk-update-icon-cache -f /usr/share/icons/hicolor 2>/dev/null || true
update-desktop-database /usr/share/applications 2>/dev/null || true
# Block dnf from updating (and reverting) nautilus. Idempotent.
if ! grep -q '^exclude=nautilus$' /etc/dnf/dnf.conf 2>/dev/null; then
    echo 'exclude=nautilus' >> /etc/dnf/dnf.conf
    echo ">> Pinned: added 'exclude=nautilus' to /etc/dnf/dnf.conf"
fi
ROOT

# --- Restart the running instance so the next launch uses the fork ----------
nautilus -q 2>/dev/null || true

echo ">> Done. GNOME Files is now the Dori fork; 'nautilus' updates are pinned."
echo "   To undo: remove 'exclude=nautilus' from /etc/dnf/dnf.conf and"
echo "   run 'sudo dnf reinstall nautilus'."
