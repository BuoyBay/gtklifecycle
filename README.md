# C guard for GTK window, ListBox, and focus lifetimes

This Linux C shim corrects lifecycle calls made by the 0.72 Linux GTK UI bridge.
It retains native windows and ListBox cells until bridge teardown, redirects the
focus-leave signal, and keeps closing one window from ending a multi-window app.

The object is linked into the application ahead of GTK. It is not a `.buipkg`,
adds no public Buoy API and requires no `LD_PRELOAD` setting.

## Build and link

From this checkout, with a Linux C compiler and compatible Buoy toolchain:

```sh
mkdir -p dist/gtklifecycle
cc -std=c11 -O2 -fPIC -Wall -Wextra -Werror -c src/gtk_lifecycle.c -o dist/gtklifecycle/gtk_lifecycle.o
buoy main.bui -o dist/app --extra-obj=dist/gtklifecycle/gtk_lifecycle.o
```

Replace `main.bui` with your application's entrypoint. The resulting application
still needs its normal GTK/Buoy runtime. Other operating systems do not use this
object. Recheck the guard when changing the toolchain's UI bridge version.

## Diagnostics and limits

`BUOY_GTKLIFECYCLE_TRACE=1` logs guard lifetime/focus events. Only calls identified
as coming from the expected Buoy bridge are changed; missing expected native
symbols fail explicitly. It is a targeted lifecycle workaround, not a general
GTK compatibility layer. See the [guard reference](docs/API.md)
for call ownership, native symbols, teardown and cross-build details.

## Local Buoy setup

On your own machine, install Buoy and run:

```sh
buoy ai init --both
```

[Contributing](CONTRIBUTING.md) · [License](LICENSE)
