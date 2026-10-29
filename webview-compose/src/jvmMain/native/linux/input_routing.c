/**
 * Keyboard / pointer routing between the embedded WebKitWebView and the
 * Nucleus Tao host window it is reparented into.
 *
 * Tao handles input at the GtkApplicationWindow level, which collides with
 * GTK's own focus model for the embedded view:
 *
 *  - Keys: Tao's `key-press-event` handler runs its window-level IM filter
 *    and stops propagation for every key that filter consumes (Nucleus
 *    >= 2.5.6), so GTK never forwards the key to the focused WebKitWebView
 *    and typing into the page does nothing. Older Tao proceeds instead and
 *    the key reaches Compose *and* WebKit. The host's generic `event` signal
 *    is emitted before `key-press-event`, so keys are delivered to WebKit
 *    from there and stopped while the view owns GTK focus.
 *
 *  - Focus: a click on Compose UI never reaches a GTK widget that takes
 *    focus, so WebKit kept GTK focus and swallowed the keys meant for a
 *    Compose text field. A press that bubbles up to the host window is a
 *    Compose click: drop GTK focus so keys go back to Tao.
 *
 *  - Stolen release: Nucleus captures presses over the view with a
 *    GtkEventBox, dispatches them to Compose and forwards them to WebKit.
 *    When WebKit answers a press with a GTK popup that grabs the pointer
 *    (context menu, <select> dropdown), the matching release goes to the
 *    popup, Compose keeps the button pressed on the view node and routes
 *    every later pointer event to it — Compose looks dead while the page
 *    keeps working. When such a grab ends, the release is replayed on the
 *    EventBox that delivered the press.
 */
#include "compose_webview_internal.h"

static gboolean view_owns_focus(GtkWidget *host, GtkWidget *view) {
    GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(host));
    return focus != NULL && (focus == view || gtk_widget_is_ancestor(focus, view));
}

static gboolean on_host_event(GtkWidget *host, GdkEvent *event, gpointer user_data) {
    ComposeWebViewState *state = user_data;
    GtkWidget *view = GTK_WIDGET(state->web_view);

    switch (event->type) {
    case GDK_KEY_PRESS:
    case GDK_KEY_RELEASE:
        if (!view_owns_focus(host, view)) return FALSE;
        gtk_window_propagate_key_event(GTK_WINDOW(host), &event->key);
        return TRUE;
    case GDK_BUTTON_PRESS:
        /* Presses over the view are taken by Nucleus' input box or by the
         * view itself and never bubble up here; anything else is Compose. */
        if (view_owns_focus(host, view)) gtk_window_set_focus(GTK_WINDOW(host), NULL);
        return FALSE;
    default:
        return FALSE;
    }
}

static void clear_pending_press(ComposeWebViewState *state) {
    if (state->press_source != NULL) {
        g_object_unref(state->press_source);
        state->press_source = NULL;
    }
    state->press_button = 0;
    state->press_shadowed = FALSE;
}

static gboolean on_view_button_press(GtkWidget *view, GdkEventButton *event, gpointer user_data) {
    ComposeWebViewState *state = user_data;
    if (event->type != GDK_BUTTON_PRESS) return FALSE;
    clear_pending_press(state);

    /* Nucleus forwards a copy retargeted at the view; the event being
     * dispatched is still the original one on its input box. */
    GdkEvent *current = gtk_get_current_event();
    GtkWidget *source = current != NULL ? gtk_get_event_widget(current) : NULL;
    if (source != NULL && source != view && GTK_IS_EVENT_BOX(source)) {
        state->press_source = g_object_ref(source);
        state->press_button = event->button;
    }
    if (current != NULL) gdk_event_free(current);
    return FALSE;
}

static gboolean on_view_button_release(GtkWidget *view, GdkEventButton *event, gpointer user_data) {
    (void) view;
    ComposeWebViewState *state = user_data;
    if (event->button == state->press_button) clear_pending_press(state);
    return FALSE;
}

static gboolean replay_release(gpointer user_data) {
    ComposeWebViewState *state = user_data;
    state->replay_source_id = 0;
    GtkWidget *box = state->press_source;
    guint button = state->press_button;
    if (box == NULL || button == 0) return G_SOURCE_REMOVE;

    GdkWindow *window = gtk_widget_get_window(box);
    if (window != NULL && gtk_widget_get_mapped(box)) {
        GdkDevice *pointer = gdk_seat_get_pointer(
            gdk_display_get_default_seat(gdk_window_get_display(window)));
        gint wx = 0, wy = 0;
        GdkModifierType mods = 0;
        if (pointer != NULL) gdk_window_get_device_position(window, pointer, &wx, &wy, &mods);
        gint rx = 0, ry = 0;
        gdk_window_get_root_coords(window, wx, wy, &rx, &ry);

        /* EventBox is window-less: its coords are its parent window's minus
         * its allocation, which is what Nucleus' release handler expects. */
        GtkAllocation allocation;
        gtk_widget_get_allocation(box, &allocation);

        GdkEvent *release = gdk_event_new(GDK_BUTTON_RELEASE);
        release->button.window = g_object_ref(window);
        release->button.send_event = TRUE;
        release->button.time = GDK_CURRENT_TIME;
        release->button.x = wx - allocation.x;
        release->button.y = wy - allocation.y;
        release->button.x_root = rx;
        release->button.y_root = ry;
        release->button.button = button;
        release->button.state = mods;
        if (pointer != NULL) gdk_event_set_device(release, pointer);
        gtk_widget_event(box, release);
        gdk_event_free(release);
    }
    clear_pending_press(state);
    return G_SOURCE_REMOVE;
}

static void on_view_grab_notify(GtkWidget *view, gboolean was_grabbed, gpointer user_data) {
    (void) view;
    ComposeWebViewState *state = user_data;
    if (state->press_source == NULL) return;
    if (!was_grabbed) {
        state->press_shadowed = TRUE;
    } else if (state->press_shadowed && state->replay_source_id == 0) {
        /* Grab teardown runs inside the popup's own event handling; replay
         * once GTK is back in the main loop. */
        state->replay_source_id = g_idle_add(replay_release, state);
    }
}

static void disconnect_host(ComposeWebViewState *state) {
    if (state->host_window == NULL) return;
    if (state->host_event_handler != 0 &&
        g_signal_handler_is_connected(state->host_window, state->host_event_handler)) {
        g_signal_handler_disconnect(state->host_window, state->host_event_handler);
    }
    state->host_event_handler = 0;
    g_object_unref(state->host_window);
    state->host_window = NULL;
}

static void on_view_hierarchy_changed(GtkWidget *view, GtkWidget *previous, gpointer user_data) {
    (void) previous;
    ComposeWebViewState *state = user_data;
    GtkWidget *top = gtk_widget_get_toplevel(view);
    if (top == state->host_window) return;
    disconnect_host(state);
    if (top == view || !GTK_IS_WINDOW(top)) return;
    state->host_window = g_object_ref(top);
    state->host_event_handler =
        g_signal_connect(top, "event", G_CALLBACK(on_host_event), state);
}

void compose_webview_connect_input_routing(ComposeWebViewState *state) {
    GtkWidget *view = GTK_WIDGET(state->web_view);
    state->input_handlers[0] = g_signal_connect(
        view, "hierarchy-changed", G_CALLBACK(on_view_hierarchy_changed), state);
    state->input_handlers[1] = g_signal_connect(
        view, "button-press-event", G_CALLBACK(on_view_button_press), state);
    state->input_handlers[2] = g_signal_connect(
        view, "button-release-event", G_CALLBACK(on_view_button_release), state);
    state->input_handlers[3] = g_signal_connect(
        view, "grab-notify", G_CALLBACK(on_view_grab_notify), state);
    on_view_hierarchy_changed(view, NULL, state);
}

void compose_webview_disconnect_input_routing(ComposeWebViewState *state) {
    if (state->replay_source_id != 0) {
        g_source_remove(state->replay_source_id);
        state->replay_source_id = 0;
    }
    for (gsize i = 0; i < G_N_ELEMENTS(state->input_handlers); i++) {
        if (state->input_handlers[i] != 0) {
            g_signal_handler_disconnect(state->web_view, state->input_handlers[i]);
            state->input_handlers[i] = 0;
        }
    }
    disconnect_host(state);
    clear_pending_press(state);
}
