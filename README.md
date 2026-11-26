# kisslock

A screen locker for Wayland and wlroots-based compositors, styled after Suckless' [slock](https://tools.suckless.org/slock/). It asks the compositor to lock with `ext-session-lock-v1`, then fills every output with one solid color.

It is a Wayland client of `ext-session-lock-v1`, the session lock protocol wlroots compositors implement.

The screen is black when idle, `#005577` while a password is being typed, and `#CC3333` for a second after a failed attempt. Enter submits the password. Backspace deletes one character. Escape, Ctrl-u, and Ctrl-Backspace clear it. Enter on an empty password is ignored.

A clock is drawn at the top of each output. `clock off` in the config file hides it. `clock on` is the default. `hours 24` prints 00-23. `hours 12` prints AM/PM.

Those colors, the fail timeout, the clock, and the PAM service name come from a config file when one is present. `kisslock -c file` reads that file. With no `-c`, it reads `$XDG_CONFIG_HOME/kisslock/config`, or `~/.config/kisslock/config`. A missing file keeps the defaults above. A bad line is skipped. See `example.conf`.

Authentication is PAM, service name `kisslock`.

## Requirements

A compositor that implements `ext-session-lock-v1`, `wp_viewporter`, and `wp_single_pixel_buffer_manager_v1`. Current sway, Hyprland, river, wayfire, niri, and other wlroots compositors do.

Build dependencies: a C23 compiler, Make, `wayland-client`, `wayland-scanner`, `wayland-protocols`, `libxkbcommon`, and PAM.

## Build and check

```sh
make
make check
```

`make check` covers usage errors and a missing display.

## Setup

`setup.sh` installs the build packages, builds, and installs. It covers Arch, Void, and Artix (pacman, including runit Artix). The locker is not a service: start it from the compositor.

```sh
./setup.sh
```

## Install

```sh
make install PREFIX="$HOME/.local"
```

The binary goes to `PREFIX/bin`. The default prefix is `/usr/local`. The PAM file is installed to `/etc/pam.d/kisslock` and needs root:

```
#%PAM-1.0
auth    required  pam_unix.so
account required  pam_unix.so
```

`DESTDIR` stages the binary, the PAM stack, and that example. `make uninstall` removes them. Use the same prefix you installed with.
