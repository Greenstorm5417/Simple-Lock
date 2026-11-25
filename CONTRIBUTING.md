# Contributing

kisslock is a small C program. Patches that keep it small are the ones that fit.

## Build

You need a C23 compiler, Make, `wayland-client`, `wayland-scanner`, `wayland-protocols`, `libxkbcommon`, and PAM.

```sh
make
make check
```

`make check` covers usage errors and a missing Wayland display. It does not lock the screen.

Do not run `kisslock` against your real compositor while you are trying things. If the process dies while the session is locked, the session stays locked. Recover from another virtual terminal by restarting the compositor.

## Changes

Match the C that is already in `src/`. The build uses `-std=c23` and the warning flags in the `Makefile`. A change should stay clean under `make`.

In the pull request, say what you changed and why. Run `make check` before you ask for review.

## Reports

Use the bug or feature templates. Include the compositor, the config you used, and what you expected the screen to do.
