#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")" && pwd)

as_root() {
    if [ "$(id -u)" -eq 0 ]; then
        "$@"
    else
        sudo "$@"
    fi
}

install_pacman() {
    as_root pacman -S --needed --noconfirm base-devel wayland wayland-protocols libxkbcommon pam
}

install_xbps() {
    as_root xbps-install -Sy base-devel wayland wayland-protocols libxkbcommon pam
}

if [ ! -r /etc/os-release ]; then
    echo "setup: cannot read /etc/os-release" >&2
    exit 1
fi

# shellcheck disable=SC1091
. /etc/os-release

case "${ID:-}" in
    arch)
        install_pacman
        ;;
    artix)
        install_pacman
        echo "No runit service is installed. Bind kisslock from the compositor."
        ;;
    void)
        install_xbps
        ;;
    *)
        echo "setup: unsupported system '${ID:-unknown}'" >&2
        echo "setup: this script covers Arch, Void, and Artix." >&2
        exit 1
        ;;
esac

make -C "$root"
as_root make -C "$root" install
