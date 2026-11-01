/*
 * GTK4 window-lifecycle guard for the Buoy 0.72 Linux UI bridge (buoy_ui.so).
 *
 * The installed bridge ships without source, and has four defects this file
 * corrects at the GTK call boundary, without reading any bridge-private
 * memory:
 *
 * 1. bui_ui_window_new connects the GTK3-only "focus-out" signal to a GTK4
 *    GtkEventControllerKey. GTK4 rejects it with a GLib CRITICAL, so the
 *    handler that clears the held-key tracker never runs. GTK4 delivers focus
 *    loss through GtkEventControllerFocus::leave instead. When the bridge makes
 *    that connection, the same handler is connected to "leave" on a new focus
 *    controller, which is attached alongside the key controller when the
 *    bridge adds it to the window. The bridge's handler ignores its first
 *    argument, so the different controller type is harmless.
 *
 * 2. Window.Close calls gtk_window_close. When nothing vetoes, GTK runs
 *    gtk_window_destroy, which unrealizes the window, removes it from the
 *    toplevel list and drops its last reference, so the GObject is finalized.
 *    The bridge keeps the raw pointer. Buoy's Window destructor later passes it
 *    to bui_ui_window_set_menubar and bui_ui_window_free, which read and
 *    destroy freed memory. Every window the bridge creates therefore gets one
 *    owned reference here. A closed window stays a valid, unrealized object
 *    outside the toplevel list until the bridge's own teardown calls
 *    gtk_window_destroy from bui_ui_window_free. GTK documents a repeated
 *    gtk_window_destroy as a no-op; the owned reference is then dropped, so
 *    the window is disposed and finalized there. That happens before the
 *    bridge frees its own window record, so signal handlers carrying the
 *    record as user data never see freed memory. A gtk_window_destroy from
 *    any other caller only destroys; the owned reference stays until the
 *    bridge's teardown.
 *
 * 3. The bridge's close-request handler quits the Application.Run main loop
 *    whenever a non-modal window closes, even with other windows still open.
 *    Application.Quit is documented as equivalent to closing the last window.
 *    The main loop is the one bui_ui_app_run creates (its caller, found by
 *    dladdr on the bridge's exported symbols). A quit of that loop from the
 *    bridge, made while a guarded window is emitting close-request, is skipped
 *    when another guarded window is still visible. Quits from
 *    bui_ui_app_quit, of modal or other nested loops, and the close of the
 *    last visible window all pass through.
 *
 * 4. ListBox factory cells are owned by GTK, but the bridge caches their raw
 *    widgets and reads them again in bui_ui_listbox_free after GTK teardown.
 *    Hold a reference when the bridge attaches each factory cell. At bridge
 *    teardown, its existing unparented-widget cleanup consumes that reference;
 *    if the cell still has a parent, release it here instead. No bridge-private
 *    records are read, and all cell references end at bridge teardown.
 *
 * Only calls made from buoy_ui.so are changed, identified by return address
 * or the scoped public factory callback registered by the bridge.
 * All other GTK callers go straight to the real functions. GTK is single
 * threaded, so the guard table is only touched on the GTK main thread. A
 * missing symbol is a hard failure, not a silent pass-through.
 *
 * This object must be searched before libgtk/libgobject. Link it into the
 * executable (tools/build_gtklifecycle.py --integrate) so that holds without
 * environment variables.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned long GType;
typedef unsigned long gulong;
typedef void *gpointer;
typedef void (*GCallback)(void);
typedef void (*GClosureNotify)(gpointer data, gpointer closure);
typedef void (*GDestroyNotify)(gpointer data);
typedef int GConnectFlags;
typedef struct _GtkWidget GtkWidget;
typedef struct _GtkWindow GtkWindow;
typedef struct _GtkEventController GtkEventController;
typedef struct _GMainLoop GMainLoop;
typedef struct _GMainContext GMainContext;
typedef struct {
    unsigned int signal_id;
    unsigned int detail;
    int run_type;
} GSignalInvocationHint;

#define BRIDGE_NAME "buoy_ui.so"
#define FOCUS_KEY "buoy-gtklifecycle-focus-controller"

static GtkWidget *(*real_gtk_window_new)(void);
static void (*real_gtk_window_destroy)(GtkWindow *);
static gulong (*real_g_signal_connect_data)(gpointer, const char *, GCallback, gpointer,
                                            GClosureNotify, GConnectFlags);
static void (*real_gtk_widget_add_controller)(GtkWidget *, GtkEventController *);
typedef void (*GWeakNotify)(gpointer data, gpointer where_the_object_was);
static gpointer (*p_g_object_ref)(gpointer);
static void (*p_g_object_weak_ref)(gpointer, GWeakNotify, gpointer);
static void (*p_g_object_unref)(gpointer);
static void (*p_g_object_set_data_full)(gpointer, const char *, gpointer, GDestroyNotify);
static gpointer (*p_g_object_steal_data)(gpointer, const char *);
static int (*p_g_type_check_instance_is_a)(gpointer, GType);
static GType (*p_gtk_event_controller_key_get_type)(void);
static GtkEventController *(*p_gtk_event_controller_focus_new)(void);
static int (*p_gtk_window_is_active)(GtkWindow *);
static GMainLoop *(*real_g_main_loop_new)(GMainContext *, int);
static void (*real_g_main_loop_quit)(GMainLoop *);
static void (*real_g_main_loop_unref)(GMainLoop *);
static unsigned int (*p_g_signal_lookup)(const char *, GType);
static GSignalInvocationHint *(*p_g_signal_get_invocation_hint)(gpointer);
static GType (*p_gtk_window_get_type)(void);
static GType (*p_gtk_signal_list_item_factory_get_type)(void);
static int (*p_gtk_widget_get_visible)(GtkWidget *);

static void *bridge_base;
static int tracing;
static pthread_once_t resolve_once = PTHREAD_ONCE_INIT;

static int strict;

/* With the bridge loaded every symbol is required; without it, only the
 * pass-through targets that exist in the process are needed. */
static void *must(void *handle, const char *name)
{
    void *symbol = dlsym(handle, name);
    if (symbol == NULL && strict) {
        fprintf(stderr, "buoy-gtklifecycle: cannot resolve %s: %s\n", name, dlerror());
        abort();
    }
    return symbol;
}

static void resolve_symbols(void)
{
    const char *trace = getenv("BUOY_GTKLIFECYCLE_TRACE");
    tracing = trace != NULL && trace[0] != '\0' && strcmp(trace, "0") != 0;
    Dl_info info;
    void *bridge_symbol = dlsym(RTLD_DEFAULT, "bui_ui_window_new");
    if (bridge_symbol != NULL) {
        if (dladdr(bridge_symbol, &info) == 0 || info.dli_fbase == NULL) {
            fprintf(stderr, "buoy-gtklifecycle: cannot locate %s\n", BRIDGE_NAME);
            abort();
        }
        strict = 1;
    }
    real_gtk_window_new = must(RTLD_NEXT, "gtk_window_new");
    real_gtk_window_destroy = must(RTLD_NEXT, "gtk_window_destroy");
    real_g_signal_connect_data = must(RTLD_NEXT, "g_signal_connect_data");
    real_gtk_widget_add_controller = must(RTLD_NEXT, "gtk_widget_add_controller");
    real_g_main_loop_new = must(RTLD_NEXT, "g_main_loop_new");
    real_g_main_loop_quit = must(RTLD_NEXT, "g_main_loop_quit");
    real_g_main_loop_unref = must(RTLD_NEXT, "g_main_loop_unref");
    p_g_object_ref = must(RTLD_DEFAULT, "g_object_ref");
    p_g_object_unref = must(RTLD_DEFAULT, "g_object_unref");
    p_g_object_weak_ref = must(RTLD_DEFAULT, "g_object_weak_ref");
    p_g_object_set_data_full = must(RTLD_DEFAULT, "g_object_set_data_full");
    p_g_object_steal_data = must(RTLD_DEFAULT, "g_object_steal_data");
    p_g_type_check_instance_is_a = must(RTLD_DEFAULT, "g_type_check_instance_is_a");
    p_gtk_event_controller_key_get_type = must(RTLD_DEFAULT, "gtk_event_controller_key_get_type");
    p_gtk_event_controller_focus_new = must(RTLD_DEFAULT, "gtk_event_controller_focus_new");
    p_gtk_window_is_active = must(RTLD_DEFAULT, "gtk_window_is_active");
    p_g_signal_lookup = must(RTLD_DEFAULT, "g_signal_lookup");
    p_g_signal_get_invocation_hint = must(RTLD_DEFAULT, "g_signal_get_invocation_hint");
    p_gtk_window_get_type = must(RTLD_DEFAULT, "gtk_window_get_type");
    p_gtk_signal_list_item_factory_get_type = must(RTLD_DEFAULT, "gtk_signal_list_item_factory_get_type");
    p_gtk_widget_get_visible = must(RTLD_DEFAULT, "gtk_widget_get_visible");
    if (!strict) {
        /* A program without the UI bridge has nothing to correct. */
        if (tracing)
            fprintf(stderr, "buoy-gtklifecycle: %s not loaded; guard inactive\n", BRIDGE_NAME);
        return;
    }
    bridge_base = info.dli_fbase;
    if (tracing)
        fprintf(stderr, "buoy-gtklifecycle: active for %s\n", info.dli_fname);
}

/* g_signal_connect_data is also reached from GIO worker threads. */
static void resolve(void)
{
    pthread_once(&resolve_once, resolve_symbols);
}

/* True when the instruction at `address` belongs to the Buoy UI bridge. */
static int from_bridge(void *address)
{
    Dl_info info;
    return bridge_base != NULL && dladdr(address, &info) != 0 && info.dli_fbase == bridge_base;
}

/* True when `address` lies in the bridge's exported function `name`. */
static int in_bridge_function(void *address, const char *name)
{
    Dl_info info;
    return bridge_base != NULL && dladdr(address, &info) != 0 && info.dli_fbase == bridge_base
           && info.dli_sname != NULL && strcmp(info.dli_sname, name) == 0;
}

/* Windows created by the bridge that hold an owned reference. Keyed by pointer
 * value only; an entry's window is always alive because the entry owns a ref. */
static GtkWindow **guarded;
static size_t guarded_count, guarded_capacity;

static long guard_index(const void *window)
{
    for (size_t i = 0; i < guarded_count; i++)
        if (guarded[i] == window)
            return (long)i;
    return -1;
}

static void on_traced_unrealize(GtkWidget *widget, gpointer unused)
{
    (void)unused;
    fprintf(stderr, "buoy-gtklifecycle: window %p unrealized; owned reference held for bridge teardown\n",
            (void *)widget);
}

static void on_traced_active(GtkWindow *window, gpointer pspec, gpointer unused)
{
    (void)pspec;
    (void)unused;
    fprintf(stderr, "buoy-gtklifecycle: window %p active=%d\n", (void *)window, p_gtk_window_is_active(window));
}

static void on_traced_finalize(gpointer unused, gpointer where_the_object_was)
{
    (void)unused;
    fprintf(stderr, "buoy-gtklifecycle: window %p finalized\n", where_the_object_was);
}

static void on_traced_focus_leave(gpointer controller, gpointer unused)
{
    (void)unused;
    fprintf(stderr, "buoy-gtklifecycle: focus left window (controller %p)\n", controller);
}

static void guard_window(GtkWindow *window)
{
    if (guarded_count == guarded_capacity) {
        size_t capacity = guarded_capacity ? guarded_capacity * 2 : 8;
        GtkWindow **grown = realloc(guarded, capacity * sizeof *grown);
        if (grown == NULL) {
            fprintf(stderr, "buoy-gtklifecycle: out of memory guarding a window\n");
            abort();
        }
        guarded = grown;
        guarded_capacity = capacity;
    }
    p_g_object_ref(window);
    guarded[guarded_count++] = window;
    if (tracing) {
        real_g_signal_connect_data(window, "unrealize", (GCallback)on_traced_unrealize, NULL, NULL, 0);
        real_g_signal_connect_data(window, "notify::is-active", (GCallback)on_traced_active, NULL, NULL, 0);
        p_g_object_weak_ref(window, on_traced_finalize, NULL);
        fprintf(stderr, "buoy-gtklifecycle: guarding window %p\n", (void *)window);
    }
}

GtkWidget *gtk_window_new(void)
{
    resolve();
    GtkWidget *window = real_gtk_window_new();
    if (window != NULL && from_bridge(__builtin_return_address(0)))
        guard_window((GtkWindow *)window);
    return window;
}

void gtk_window_destroy(GtkWindow *window)
{
    resolve();
    long i = guard_index(window);
    if (i < 0) {
        real_gtk_window_destroy(window);
        return;
    }
    if (!from_bridge(__builtin_return_address(0))) {
        /* Another caller (program code through a Declare, another library)
         * destroys a bridge window. GTK unrealizes it and drops its own
         * reference; the owned reference keeps it valid for the bridge, which
         * still holds the pointer and tears it down later. */
        if (tracing)
            fprintf(stderr, "buoy-gtklifecycle: external destroy of window %p; owned reference kept\n",
                    (void *)window);
        real_gtk_window_destroy(window);
        return;
    }
    guarded[i] = guarded[--guarded_count];
    if (tracing)
        fprintf(stderr, "buoy-gtklifecycle: bridge teardown of window %p\n", (void *)window);
    real_gtk_window_destroy(window);
    p_g_object_unref(window);
}


typedef struct _GtkListItem GtkListItem;
static _Thread_local GtkListItem *active_cell_item;
typedef struct {
    GCallback handler;
    gpointer data;
    GClosureNotify destroy;
} CellSetup;
static void cell_setup_callback(gpointer factory, gpointer item, gpointer user)
{
    CellSetup *setup = user;
    GtkListItem *previous = active_cell_item;
    active_cell_item = item;
    ((void (*)(gpointer, gpointer, gpointer))setup->handler)(factory, item, setup->data);
    active_cell_item = previous;
}
static void cell_setup_destroy(gpointer user, gpointer closure)
{
    CellSetup *setup = user;
    if (setup->destroy) setup->destroy(setup->data, closure);
    free(setup);
}

gulong g_signal_connect_data(gpointer instance, const char *detailed_signal, GCallback handler,
                             gpointer data, GClosureNotify destroy_data, GConnectFlags flags)
{
    resolve();
    /* The factory tail-calls gtk_list_item_set_child, so its return address
     * belongs to GTK. Scope that boundary by wrapping only the bridge's
     * public setup signal, preserving callback data and closure destruction. */
    if (instance && detailed_signal && strcmp(detailed_signal, "setup") == 0
        && from_bridge(__builtin_return_address(0)) && flags == 0
        && p_g_type_check_instance_is_a(instance, p_gtk_signal_list_item_factory_get_type())) {
        CellSetup *setup = malloc(sizeof *setup);
        if (!setup) abort();
        *setup = (CellSetup){handler, data, destroy_data};
        return real_g_signal_connect_data(instance, detailed_signal, (GCallback)cell_setup_callback,
                                          setup, cell_setup_destroy, flags);
    }
    if (instance != NULL && detailed_signal != NULL && strcmp(detailed_signal, "focus-out") == 0
        && from_bridge(__builtin_return_address(0))
        && p_g_type_check_instance_is_a(instance, p_gtk_event_controller_key_get_type())) {
        GtkEventController *focus = p_gtk_event_controller_focus_new();
        gulong id = real_g_signal_connect_data(focus, "leave", handler, data, destroy_data, flags);
        p_g_object_set_data_full(instance, FOCUS_KEY, focus, p_g_object_unref);
        if (tracing) {
            real_g_signal_connect_data(focus, "leave", (GCallback)on_traced_focus_leave, NULL, NULL, 0);
            fprintf(stderr, "buoy-gtklifecycle: focus-out redirected to GtkEventControllerFocus::leave\n");
        }
        return id;
    }
    return real_g_signal_connect_data(instance, detailed_signal, handler, data, destroy_data, flags);
}

void gtk_widget_add_controller(GtkWidget *widget, GtkEventController *controller)
{
    resolve();
    GtkEventController *focus = NULL;
    if (controller != NULL && from_bridge(__builtin_return_address(0)))
        focus = p_g_object_steal_data(controller, FOCUS_KEY);
    real_gtk_widget_add_controller(widget, controller);
    if (focus != NULL)
        real_gtk_widget_add_controller(widget, focus);
}

/* The Application.Run main loop, created by bui_ui_app_run. */
static GMainLoop *app_loop;

void g_main_loop_unref(GMainLoop *loop)
{
    resolve();
    /* Stop tracking before the bridge releases its loop reference. */
    if (loop == app_loop) app_loop = NULL;
    real_g_main_loop_unref(loop);
}

GMainLoop *g_main_loop_new(GMainContext *context, int is_running)
{
    resolve();
    GMainLoop *loop = real_g_main_loop_new(context, is_running);
    if (in_bridge_function(__builtin_return_address(0), "bui_ui_app_run"))
        app_loop = loop;
    return loop;
}

static int emitting_close_request(GtkWindow *window, unsigned int close_request)
{
    GSignalInvocationHint *hint = p_g_signal_get_invocation_hint(window);
    return hint != NULL && hint->signal_id == close_request;
}

void g_main_loop_quit(GMainLoop *loop)
{
    resolve();
    void *caller = __builtin_return_address(0);
    if (loop == NULL || loop != app_loop || !from_bridge(caller) || in_bridge_function(caller, "bui_ui_app_quit")) {
        real_g_main_loop_quit(loop);
        return;
    }
    unsigned int close_request = p_g_signal_lookup("close-request", p_gtk_window_get_type());
    size_t closing = 0, visible = 0;
    for (size_t i = 0; i < guarded_count; i++) {
        if (emitting_close_request(guarded[i], close_request))
            closing++;
        else if (p_gtk_widget_get_visible((GtkWidget *)guarded[i]))
            visible++;
    }
    if (closing == 0 || visible == 0) {
        real_g_main_loop_quit(loop);
        return;
    }
    if (tracing)
        fprintf(stderr, "buoy-gtklifecycle: window close kept Application.Run going; %zu window(s) still visible\n",
                visible);
}

/* Factory-created cells are cached by the bridge after GTK drops
 * its ownership. Keep one reference until the bridge's cell teardown. */
static GtkWidget **guarded_cells;
static size_t cell_count, cell_capacity;
static void (*real_gtk_list_item_set_child)(GtkListItem *, GtkWidget *);
static GtkWidget *(*real_gtk_widget_get_parent)(GtkWidget *);
static size_t cells_created, cells_released, cells_finalized;
static void cell_finalized(gpointer unused, gpointer object)
{
    (void)unused;
    cells_finalized++;
    if (tracing)
        fprintf(stderr, "cell-guard: finalized %p created=%zu released=%zu finalized=%zu\n",
                object, cells_created, cells_released, cells_finalized);
}
void gtk_list_item_set_child(GtkListItem *item, GtkWidget *child)
{
    resolve();
    if (!real_gtk_list_item_set_child)
        real_gtk_list_item_set_child = must(RTLD_NEXT, "gtk_list_item_set_child");
    if (child && item == active_cell_item) {
        size_t i;
        for (i = 0; i < cell_count; i++)
            if (guarded_cells[i] == child) break;
        if (i == cell_count) {
            if (cell_count == cell_capacity) {
                size_t capacity = cell_capacity ? cell_capacity * 2 : 32;
                GtkWidget **grown = realloc(guarded_cells, capacity * sizeof *grown);
                if (!grown) abort();
                guarded_cells = grown;
                cell_capacity = capacity;
            }
            p_g_object_ref(child);
            guarded_cells[cell_count++] = child;
            cells_created++;
            p_g_object_weak_ref(child, cell_finalized, NULL);
            if (tracing) fprintf(stderr, "cell-guard: retained %p\n", (void *)child);
        }
    }
    real_gtk_list_item_set_child(item, child);
}
GtkWidget *gtk_widget_get_parent(GtkWidget *widget)
{
    resolve();
    if (!real_gtk_widget_get_parent)
        real_gtk_widget_get_parent = must(RTLD_NEXT, "gtk_widget_get_parent");
    GtkWidget *parent = real_gtk_widget_get_parent(widget);
    if (in_bridge_function(__builtin_return_address(0), "bui_ui_listbox_free")) {
        for (size_t i = 0; i < cell_count; i++) {
            if (guarded_cells[i] != widget) continue;
            guarded_cells[i] = guarded_cells[--cell_count];
            cells_released++;
            /* With no parent the bridge's existing cleanup consumes this
             * reference. Otherwise GTK still owns it and the bridge skips
             * cleanup, so release our reference here. */
            if (parent) p_g_object_unref(widget);
            if (tracing)
                fprintf(stderr, "cell-guard: released %p parent=%p outstanding=%zu\n",
                        (void *)widget, (void *)parent, cell_count);
            if (!cell_count) {
                free(guarded_cells);
                guarded_cells = NULL;
                cell_capacity = 0;
            }
            break;
        }
    }
    return parent;
}
