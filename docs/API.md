# GTK window-lifecycle guard (Linux)

The Buoy 0.72 Linux UI bridge (`buoy_ui.so`, GTK 4) has four UI lifecycle
defects. This guard corrects them at the GTK call boundary. It is a C object
that you link into a Buoy executable. It is not a Buoy package and adds no
API.

| Defect | Symptom | Guard behavior |
| --- | --- | --- |
| `bui_ui_window_new` connects the GTK 3 `focus-out` signal to a GTK 4 `GtkEventControllerKey`. | A GLib `CRITICAL` when every window is created. The handler, which clears held-key state, is never connected. | Connects the same handler to `GtkEventControllerFocus::leave`. The focus controller is attached to the window together with the key controller. |
| `Window.Close` runs `gtk_window_close`. GTK then destroys the window and drops its last reference, but the bridge keeps the pointer. The `Window` destructor later passes it to `bui_ui_window_set_menubar` and `bui_ui_window_free`. | When ARC frees a closed `Window`, GTK reports invalid widget/window accesses, ending in `gtk_window_destroy`. These are reads of freed memory. | Holds one reference to each window the bridge creates. A closed window remains a valid, unrealized object until `bui_ui_window_free` calls `gtk_window_destroy`. GTK treats that repeated destroy as a no-op. The guard then drops its reference, so the window is finalized before the bridge frees its own window record. A `gtk_window_destroy` from any other caller (program code through a `Declare`, another library) only destroys the window. The reference stays until the bridge's teardown. |
| The bridge's `close-request` handler quits the `Application.Run` main loop when any non-modal window closes. | Closing one of several open windows ends `Application.Run`. `Application.Quit` is documented as equivalent to closing the *last* window. | Skips the bridge's quit of the main loop while another bridge window is still visible. Closing the last visible window, `Application.Quit`, and the end of a modal or other nested loop still quit as before. |

ListBox factory cells also need an owned reference: GTK can finalize them when
rows or their window are removed, while the bridge caches their raw pointers
until `bui_ui_listbox_free`. The guard retains cells attached by the bridge's
factory setup callback. The bridge's unparented-cell cleanup consumes that
reference at teardown; if GTK still parents a cell, the guard releases its
reference itself. Cell traces record retention, release and finalization.

The guard changes only calls that come from `buoy_ui.so`, identified by return
address with `dladdr`, or calls scoped to its public ListBox factory setup
callback (which tail-calls GTK). It recognizes the main loop by the bridge's exported
`bui_ui_app_run` symbol, which creates it, and lets quits from `bui_ui_app_quit`
through. It identifies the closing window with
`g_signal_get_invocation_hint`. It never reads memory private to the bridge. If an expected GTK or
GLib symbol is missing, it aborts. It does not fall back to unguarded
behavior. A program that does not load `buoy_ui.so` passes through unchanged.

The bridge already clears held keys when the window's `is-active` property
becomes false. The redirected `leave` handler adds the GTK 4 focus-loss
path that the bridge intended. Neither the bridge nor the guard changes
`Window` events.

## Build and link

From the repository root:

```sh
mkdir -p dist/gtklifecycle
cc -std=c11 -O2 -fPIC -Wall -Wextra -Werror -c src/gtk_lifecycle.c -o dist/gtklifecycle/gtk_lifecycle.o
buoy main.bui -o dist/app --extra-obj=dist/gtklifecycle/gtk_lifecycle.o
```

Replace `main.bui` with the application's entrypoint. Linking the object into the
executable places the guard ahead of GTK in symbol lookup. No `LD_PRELOAD`
setting or additional guard runtime file is needed. Build outputs stay in ignored
`dist/`. Only Linux applications use this object.

The retained `tools/build_gtklifecycle.py` wrapper is a compatibility fallback
for the assembled workspace's logical source layout. It is not required for the
normal direct C build above.

## Diagnostics

Set `BUOY_GTKLIFECYCLE_TRACE=1` to log each lifecycle event on stderr:
guarded windows, the focus redirect, activation changes, focus loss,
unrealize on close, bridge teardown, and finalization (through a GObject weak
reference).

## Limits

- Only x86_64 Linux has been built and run. An arm64 object needs a glibc
  sysroot: pass `--object-target arm64-linux --sysroot <dir>`. No arm64 sysroot
  or arm64 host was available, so arm64 is unbuilt and unverified.
- This is a workaround at the bridge boundary. The bridge's source is not
  shipped with Buoy 0.72, so the durable fix belongs upstream. Upstream should
  connect `GtkEventControllerFocus::leave` and clear its window pointer on
  `destroy`. Re-run the verifier after a Buoy upgrade. If an upgraded bridge
  stops reproducing the defects, the unguarded baseline check fails, which
  shows the guard can be retired.
- "Visible" means `gtk_widget_get_visible` on a window the bridge created. A
  hidden window (`Visible = False`, or never shown) does not keep
  `Application.Run` going. If a non-modal window closes while a modal window
  runs, the main loop is kept only when another window, such as the modal
  one, is still visible.
- Destroying an *open* window's `GtkWindow` behind Buoy's back sends no close
  event. The runtime still counts the window as open
  (`Application.WindowCount`) and keeps the `Window` object. The guard only
  ensures that no freed memory is used.
- `Declare` calls bind to the guard at link time. `Optional Declare` and other
  `dlsym` lookups in `libgtk-4` reach GTK directly. A direct destroy cannot
  drop the guard's reference, so those calls are safe too.
- macOS and Windows bridges are unaffected and untouched.

## Build compatibility

If a host C compiler enables glibc's `_FORTIFY_SOURCE` wrapper aliases, add
`-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0` when compiling this guard. Buoy 0.72's libc
link stubs omit `__fprintf_chk`, so a fortified object can compile successfully
and then fail to link. Scope these flags to the guard's C compilation.
