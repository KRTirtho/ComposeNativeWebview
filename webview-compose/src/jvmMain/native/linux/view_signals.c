#include "compose_webview_internal.h"

static WebKitWebView *compose_webkit_focused_view(GtkWidget *host) {
    GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(host));
    for (GtkWidget *widget = focus; widget != NULL; widget = gtk_widget_get_parent(widget)) {
        if (WEBKIT_IS_WEB_VIEW(widget)) return WEBKIT_WEB_VIEW(widget);
    }
    return NULL;
}

static gboolean compose_webkit_native_context_menu_is_open(WebKitWebView *web_view) {
    // WebKit attaches its native GTK3 context menu to the WebView. The list is
    // borrowed from GTK, so only inspect it; never free or modify it.
    GList *menus = gtk_menu_get_for_attach_widget(GTK_WIDGET(web_view));
    for (GList *link = menus; link != NULL; link = link->next) {
        GtkWidget *menu = GTK_WIDGET(link->data);
        if (GTK_IS_MENU(menu) && gtk_widget_get_visible(menu) && gtk_widget_get_mapped(menu))
            return TRUE;
    }
    return FALSE;
}

static gboolean compose_webkit_host_event(GtkWidget *host, GdkEvent *event, gpointer) {
    if (event->any.send_event) return FALSE;

    // Tao's top-level event hook must not treat GTK popup surface geometry or
    // focus events as changes to the decorated window itself.
    if ((event->type == GDK_CONFIGURE || event->type == GDK_FOCUS_CHANGE) &&
        event->any.window != gtk_widget_get_window(host))
        return TRUE;

    WebKitWebView *web_view = compose_webkit_focused_view(host);
    if (web_view == NULL) return FALSE;
    ComposeWebViewState *state = g_object_get_data(G_OBJECT(web_view), "compose-webview-state");
    if (state == NULL || state->web_view != web_view) return FALSE;

    // The native GTK menu owns menu navigation and Escape while it is open.
    if ((event->type == GDK_KEY_PRESS || event->type == GDK_KEY_RELEASE) &&
        compose_webkit_native_context_menu_is_open(web_view))
        return FALSE;

    if (event->type == GDK_BUTTON_PRESS) {
        GtkWidget *source = gtk_get_event_widget(event);
        gint x = 0, y = 0;
        if (source != NULL && gtk_widget_translate_coordinates(source, GTK_WIDGET(web_view),
                (gint)event->button.x, (gint)event->button.y, &x, &y)) {
            GtkAllocation allocation;
            gtk_widget_get_allocation(GTK_WIDGET(web_view), &allocation);
            if (x < 0 || y < 0 || x >= allocation.width || y >= allocation.height)
                gtk_window_set_focus(GTK_WINDOW(host), NULL);
        }
        return FALSE;
    }

    if (event->type != GDK_KEY_PRESS && event->type != GDK_KEY_RELEASE) return FALSE;
    GdkWindow *target_window = gtk_widget_get_window(GTK_WIDGET(web_view));
    if (target_window == NULL) return FALSE;

    // Tao 2.5.18 runs its top-level IM filter before GTK propagates the key to
    // the focused child. Retarget the original key to WebKit so its own IM
    // context handles text entry without also sending the key to Compose.
    GdkEvent *forwarded = gdk_event_copy(event);
    GdkWindow *old_window = forwarded->key.window;
    forwarded->key.window = g_object_ref(target_window);
    if (old_window != NULL) g_object_unref(old_window);
    forwarded->key.send_event = TRUE;
    gtk_widget_event(GTK_WIDGET(web_view), forwarded);
    gdk_event_free(forwarded);
    return TRUE;
}

static void compose_webkit_host_pointer_press(GtkGestureMultiPress *gesture, gint, gdouble x, gdouble y, gpointer user_data) {
    ComposeWebViewState *state = user_data;
    WebKitWebView *web_view = state->web_view;
    GtkWidget *host = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    if (web_view == NULL || !GTK_IS_WINDOW(host)) return;

    gint view_x = -1, view_y = -1;
    GtkAllocation view_allocation;
    gtk_widget_get_allocation(GTK_WIDGET(web_view), &view_allocation);
    const gboolean inside_web_view =
        gtk_widget_translate_coordinates(host, GTK_WIDGET(web_view), (gint)x, (gint)y,
                                         &view_x, &view_y) &&
        view_x >= 0 && view_y >= 0 &&
        view_x < view_allocation.width && view_y < view_allocation.height;
    if (!inside_web_view && gtk_window_get_focus(GTK_WINDOW(host)) != NULL)
        gtk_window_set_focus(GTK_WINDOW(host), NULL);
}

static void compose_webview_on_hierarchy_changed(GtkWidget *widget, GtkWidget *, gpointer user_data) {
    ComposeWebViewState *state = user_data;
    if (state->host_window != NULL) {
        if (state->host_event_handler != 0 &&
            g_signal_handler_is_connected(state->host_window, state->host_event_handler))
            g_signal_handler_disconnect(state->host_window, state->host_event_handler);
        g_object_unref(state->host_window);
        state->host_window = NULL;
        state->host_event_handler = 0;
    }
    if (state->host_pointer_gesture != NULL) {
        g_signal_handlers_disconnect_by_data(state->host_pointer_gesture, state);
        g_object_unref(state->host_pointer_gesture);
        state->host_pointer_gesture = NULL;
    }
    GtkWidget *top = gtk_widget_get_toplevel(widget);
    if (!GTK_IS_WINDOW(top) || top == widget) return;
    state->host_window = g_object_ref(top);
    state->host_event_handler = g_signal_connect(top, "event", G_CALLBACK(compose_webkit_host_event), NULL);
    state->host_pointer_gesture = gtk_gesture_multi_press_new(top);
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(state->host_pointer_gesture), 0);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(state->host_pointer_gesture), GTK_PHASE_CAPTURE);
    g_signal_connect(state->host_pointer_gesture, "pressed",
                     G_CALLBACK(compose_webkit_host_pointer_press), state);
}

void compose_webview_connect_input_routing(ComposeWebViewState *state) {
    if (state == NULL || state->web_view == NULL) return;
    state->hierarchy_handler = g_signal_connect(state->web_view, "hierarchy-changed",
        G_CALLBACK(compose_webview_on_hierarchy_changed), state);
    compose_webview_on_hierarchy_changed(GTK_WIDGET(state->web_view), NULL, state);
}

void compose_webview_disconnect_input_routing(ComposeWebViewState *state) {
    if (state == NULL) return;
    if (state->web_view != NULL && state->hierarchy_handler != 0 &&
        g_signal_handler_is_connected(state->web_view, state->hierarchy_handler))
        g_signal_handler_disconnect(state->web_view, state->hierarchy_handler);
    state->hierarchy_handler = 0;
    if (state->host_pointer_gesture != NULL) {
        g_signal_handlers_disconnect_by_data(state->host_pointer_gesture, state);
        g_object_unref(state->host_pointer_gesture);
        state->host_pointer_gesture = NULL;
    }
    if (state->host_window != NULL) {
        if (state->host_event_handler != 0 &&
            g_signal_handler_is_connected(state->host_window, state->host_event_handler))
            g_signal_handler_disconnect(state->host_window, state->host_event_handler);
        g_object_unref(state->host_window);
        state->host_window = NULL;
        state->host_event_handler = 0;
    }
}

gboolean compose_webview_on_decide_policy(
    WebKitWebView *web_view,
    WebKitPolicyDecision *decision,
    WebKitPolicyDecisionType type,
    gpointer user_data)
{
    (void) user_data;
    if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION &&
        type != WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) {
        return FALSE;
    }

    WebKitNavigationPolicyDecision *nav =
        WEBKIT_NAVIGATION_POLICY_DECISION(decision);
    WebKitNavigationAction *action =
        webkit_navigation_policy_decision_get_navigation_action(nav);
    WebKitURIRequest *request = webkit_navigation_action_get_request(action);
    const gchar *uri = webkit_uri_request_get_uri(request);
    if (uri == NULL) {
        webkit_policy_decision_use(decision);
        return TRUE;
    }

    if (g_str_has_prefix(uri, "about:") ||
        g_str_has_prefix(uri, "data:") ||
        g_str_has_prefix(uri, "blob:")) {
        webkit_policy_decision_use(decision);
        return TRUE;
    }

    JNIEnv *env = compose_webview_get_env();
    if (env == NULL) {
        webkit_policy_decision_use(decision);
        return TRUE;
    }
    compose_webview_ensure_bridge_methods(env);
    if (compose_webview_bridge_class() == NULL || compose_webview_on_navigate() == NULL) {
        webkit_policy_decision_use(decision);
        return TRUE;
    }

    jlong handle = compose_webview_handle_from_view(web_view);
    jstring juri = (*env)->NewStringUTF(env, uri);
    jboolean allow = (*env)->CallStaticBooleanMethod(
        env, compose_webview_bridge_class(), compose_webview_on_navigate(), handle, juri);
    (*env)->DeleteLocalRef(env, juri);

    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        webkit_policy_decision_use(decision);
        return TRUE;
    }

    if (allow) {
        webkit_policy_decision_use(decision);
    } else {
        webkit_policy_decision_ignore(decision);
    }
    return TRUE;
}

void compose_webview_on_script_message(
    WebKitUserContentManager *manager,
    WebKitJavascriptResult *js_result,
    gpointer user_data)
{
    (void) manager;
    ComposeWebViewState *state = (ComposeWebViewState *) user_data;
    if (state == NULL || state->web_view == NULL || js_result == NULL) return;

    /* WebKitGTK 4.1 still delivers WebKitJavascriptResult on this signal
     * (not a bare JSCValue*). Extract the JSCValue first. */
    JSCValue *value = webkit_javascript_result_get_js_value(js_result);
    if (value == NULL || !JSC_IS_VALUE(value)) return;

    gchar *message = NULL;
    if (jsc_value_is_string(value)) {
        message = jsc_value_to_string(value);
    } else {
        message = jsc_value_to_json(value, 0);
    }
    if (message == NULL) return;

    JNIEnv *env = compose_webview_get_env();
    if (env != NULL) {
        compose_webview_ensure_bridge_methods(env);
        if (compose_webview_bridge_class() != NULL && compose_webview_on_ipc() != NULL) {
            jlong handle = (jlong) (uintptr_t) state;
            jstring jmsg = (*env)->NewStringUTF(env, message);
            (*env)->CallStaticVoidMethod(
                env, compose_webview_bridge_class(), compose_webview_on_ipc(), handle, jmsg);
            (*env)->DeleteLocalRef(env, jmsg);
            if ((*env)->ExceptionCheck(env)) {
                (*env)->ExceptionClear(env);
            }
        }
    }
    g_free(message);
}
