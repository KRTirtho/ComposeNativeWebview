#include "compose_webview_internal.h"

typedef struct {
    WebKitWebView *web_view;
    GWeakRef root_popover;
    GAction *source;
    GVariant *target;
    WebKitContextMenuAction stock_action;
    gchar *link_uri;
    gchar *image_uri;
    gchar *media_uri;
    gchar *insert_text;
    gint x;
    gint y;
} ComposeWebKitMenuAction;

static void compose_webkit_menu_action_free(gpointer data, GClosure *closure) {
    (void)closure;
    ComposeWebKitMenuAction *action = data;
    g_clear_object(&action->web_view);
    g_clear_object(&action->source);
    g_clear_pointer(&action->target, g_variant_unref);
    g_weak_ref_clear(&action->root_popover);
    g_free(action->link_uri);
    g_free(action->image_uri);
    g_free(action->media_uri);
    g_free(action->insert_text);
    g_free(action);
}

static void compose_webkit_insert_text(WebKitWebView *view, const gchar *text) {
    if (view == NULL || text == NULL) return;
    gchar *encoded = g_base64_encode((const guchar *)text, strlen(text));
    gchar *script = g_strdup_printf(
        "document.execCommand('insertText',false,decodeURIComponent(escape(atob('%s'))));", encoded);
    webkit_web_view_evaluate_javascript(view, script, -1, NULL, NULL, NULL, NULL, NULL);
    g_free(script);
    g_free(encoded);
}

static void compose_webkit_media_action(ComposeWebKitMenuAction *action, const gchar *operation) {
    const gchar *uri = action->media_uri != NULL ? action->media_uri : "";
    gchar *encoded_uri = g_base64_encode((const guchar *)uri, strlen(uri));
    gchar *script = g_strdup_printf(
        "(()=>{const u=decodeURIComponent(escape(atob('%s')));let e=document.elementFromPoint(%d,%d);"
        "while(e&&!(e instanceof HTMLMediaElement))e=e.parentElement;"
        "if(!e&&u)e=[...document.querySelectorAll('audio,video')].find(m=>m.currentSrc===u||m.src===u);"
        "if(e){%s}})()",
        encoded_uri, action->x, action->y, operation);
    webkit_web_view_evaluate_javascript(action->web_view, script, -1, NULL, NULL, NULL, NULL, NULL);
    g_free(script);
    g_free(encoded_uri);
}

static void compose_webkit_activate_stock_action(ComposeWebKitMenuAction *action) {
    WebKitWebView *view = action->web_view;
    const gchar *uri = action->link_uri != NULL ? action->link_uri : action->image_uri;
    const gchar *name = action->source != NULL ? g_action_get_name(action->source) : "";

    if (action->insert_text != NULL) {
        compose_webkit_insert_text(view, action->insert_text);
        return;
    }

    if (g_ascii_strcasecmp(name, "undo") == 0) {
        webkit_web_view_execute_editing_command(view, WEBKIT_EDITING_COMMAND_UNDO);
        return;
    }
    if (g_ascii_strcasecmp(name, "redo") == 0) {
        webkit_web_view_execute_editing_command(view, WEBKIT_EDITING_COMMAND_REDO);
        return;
    }

    switch (action->stock_action) {
    case WEBKIT_CONTEXT_MENU_ACTION_OPEN_LINK:
    case WEBKIT_CONTEXT_MENU_ACTION_OPEN_LINK_IN_NEW_WINDOW:
    case WEBKIT_CONTEXT_MENU_ACTION_OPEN_IMAGE_IN_NEW_WINDOW:
    case WEBKIT_CONTEXT_MENU_ACTION_OPEN_FRAME_IN_NEW_WINDOW:
        if (uri != NULL) webkit_web_view_load_uri(view, uri);
        break;
    case WEBKIT_CONTEXT_MENU_ACTION_DOWNLOAD_LINK_TO_DISK:
        if (action->link_uri != NULL) webkit_web_view_download_uri(view, action->link_uri);
        break;
    case WEBKIT_CONTEXT_MENU_ACTION_DOWNLOAD_IMAGE_TO_DISK:
        if (action->image_uri != NULL) webkit_web_view_download_uri(view, action->image_uri);
        break;
    case WEBKIT_CONTEXT_MENU_ACTION_DOWNLOAD_VIDEO_TO_DISK:
    case WEBKIT_CONTEXT_MENU_ACTION_DOWNLOAD_AUDIO_TO_DISK:
        if (action->media_uri != NULL) webkit_web_view_download_uri(view, action->media_uri);
        break;
    case WEBKIT_CONTEXT_MENU_ACTION_COPY_LINK_TO_CLIPBOARD:
        if (action->link_uri != NULL)
            gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), action->link_uri, -1);
        break;
    case WEBKIT_CONTEXT_MENU_ACTION_COPY_IMAGE_URL_TO_CLIPBOARD:
        if (action->image_uri != NULL)
            gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), action->image_uri, -1);
        break;
    case WEBKIT_CONTEXT_MENU_ACTION_COPY_VIDEO_LINK_TO_CLIPBOARD:
    case WEBKIT_CONTEXT_MENU_ACTION_COPY_AUDIO_LINK_TO_CLIPBOARD:
        if (action->media_uri != NULL)
            gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), action->media_uri, -1);
        break;
    case WEBKIT_CONTEXT_MENU_ACTION_COPY_IMAGE_TO_CLIPBOARD:
        if (action->image_uri != NULL)
            gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), action->image_uri, -1);
        break;
    case WEBKIT_CONTEXT_MENU_ACTION_GO_BACK: webkit_web_view_go_back(view); break;
    case WEBKIT_CONTEXT_MENU_ACTION_GO_FORWARD: webkit_web_view_go_forward(view); break;
    case WEBKIT_CONTEXT_MENU_ACTION_STOP: webkit_web_view_stop_loading(view); break;
    case WEBKIT_CONTEXT_MENU_ACTION_RELOAD: webkit_web_view_reload(view); break;
    case WEBKIT_CONTEXT_MENU_ACTION_COPY:
        webkit_web_view_execute_editing_command(view, WEBKIT_EDITING_COMMAND_COPY); break;
    case WEBKIT_CONTEXT_MENU_ACTION_CUT:
        webkit_web_view_execute_editing_command(view, WEBKIT_EDITING_COMMAND_CUT); break;
    case WEBKIT_CONTEXT_MENU_ACTION_PASTE:
        webkit_web_view_execute_editing_command(view, WEBKIT_EDITING_COMMAND_PASTE); break;
    case WEBKIT_CONTEXT_MENU_ACTION_PASTE_AS_PLAIN_TEXT:
        webkit_web_view_execute_editing_command(view, WEBKIT_EDITING_COMMAND_PASTE_AS_PLAIN_TEXT); break;
    case WEBKIT_CONTEXT_MENU_ACTION_DELETE:
        webkit_web_view_execute_editing_command(view, "Delete"); break;
    case WEBKIT_CONTEXT_MENU_ACTION_SELECT_ALL:
        webkit_web_view_execute_editing_command(view, WEBKIT_EDITING_COMMAND_SELECT_ALL); break;
    case WEBKIT_CONTEXT_MENU_ACTION_SPELLING_GUESS:
        webkit_web_view_execute_editing_command_with_argument(view, "ReplaceMisspelled",
            action->insert_text != NULL ? action->insert_text : ""); break;
    case WEBKIT_CONTEXT_MENU_ACTION_BOLD:
        webkit_web_view_execute_editing_command(view, "Bold"); break;
    case WEBKIT_CONTEXT_MENU_ACTION_ITALIC:
        webkit_web_view_execute_editing_command(view, "Italic"); break;
    case WEBKIT_CONTEXT_MENU_ACTION_UNDERLINE:
        webkit_web_view_execute_editing_command(view, "Underline"); break;
    case WEBKIT_CONTEXT_MENU_ACTION_INSPECT_ELEMENT: {
        WebKitWebInspector *inspector = webkit_web_view_get_inspector(view);
        if (inspector != NULL) webkit_web_inspector_show(inspector);
        break;
    }
    case WEBKIT_CONTEXT_MENU_ACTION_OPEN_VIDEO_IN_NEW_WINDOW:
    case WEBKIT_CONTEXT_MENU_ACTION_OPEN_AUDIO_IN_NEW_WINDOW:
        if (action->media_uri != NULL) webkit_web_view_load_uri(view, action->media_uri);
        break;
    case WEBKIT_CONTEXT_MENU_ACTION_TOGGLE_MEDIA_CONTROLS:
        compose_webkit_media_action(action, "e.controls=!e.controls;"); break;
    case WEBKIT_CONTEXT_MENU_ACTION_TOGGLE_MEDIA_LOOP:
        compose_webkit_media_action(action, "e.loop=!e.loop;"); break;
    case WEBKIT_CONTEXT_MENU_ACTION_ENTER_VIDEO_FULLSCREEN:
        compose_webkit_media_action(action, "if(e.requestFullscreen)e.requestFullscreen();"); break;
    case WEBKIT_CONTEXT_MENU_ACTION_MEDIA_PLAY:
        compose_webkit_media_action(action, "e.play();"); break;
    case WEBKIT_CONTEXT_MENU_ACTION_MEDIA_PAUSE:
        compose_webkit_media_action(action, "e.pause();"); break;
    case WEBKIT_CONTEXT_MENU_ACTION_MEDIA_MUTE:
        compose_webkit_media_action(action, "e.muted=!e.muted;"); break;
    case WEBKIT_CONTEXT_MENU_ACTION_INSERT_EMOJI:
        compose_webkit_insert_text(view, action->insert_text != NULL ? action->insert_text : "🙂");
        break;
    case WEBKIT_CONTEXT_MENU_ACTION_CUSTOM:
    default:
        if (action->source != NULL &&
            (g_action_get_parameter_type(action->source) == NULL || action->target != NULL))
            g_action_activate(action->source, action->target);
        break;
    }
}

static void compose_webkit_menu_button_clicked(GtkButton *button, gpointer user_data) {
    ComposeWebKitMenuAction *action = user_data;
    compose_webkit_activate_stock_action(action);
    GtkWidget *popover = g_weak_ref_get(&action->root_popover);
    if (popover != NULL) {
        gtk_popover_popdown(GTK_POPOVER(popover));
        g_object_unref(popover);
    }
    (void)button;
}

static gchar *compose_webkit_plain_menu_label(const gchar *label) {
    GString *plain = g_string_new(NULL);
    for (const gchar *p = label != NULL ? label : ""; *p != '\0'; ++p) {
        if (*p != '_') g_string_append_c(plain, *p);
    }
    return g_string_free(plain, FALSE);
}

static void compose_webkit_add_menu_row(GtkWidget *box, GtkWidget *root_popover, WebKitWebView *view,
    const gchar *label, WebKitContextMenuAction stock_action, GAction *source,
    GVariant *target, const gchar *link_uri, const gchar *image_uri,
    const gchar *media_uri, const gchar *insert_text, gint x, gint y, gboolean enabled)
{
    gchar *plain_label = compose_webkit_plain_menu_label(label);
    GtkWidget *button = gtk_button_new_with_label(plain_label);
    g_free(plain_label);
    gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
    gtk_widget_set_halign(button, GTK_ALIGN_FILL);
    gtk_widget_set_hexpand(button, TRUE);
    gtk_widget_set_can_focus(button, FALSE);
    gtk_style_context_add_class(gtk_widget_get_style_context(button), "modelbutton");
    gtk_widget_set_sensitive(button, enabled);

    ComposeWebKitMenuAction *action = g_new0(ComposeWebKitMenuAction, 1);
    action->web_view = g_object_ref(view);
    g_weak_ref_init(&action->root_popover, root_popover);
    action->source = source != NULL ? g_object_ref(source) : NULL;
    action->target = target != NULL ? g_variant_ref(target) : NULL;
    action->stock_action = stock_action;
    action->link_uri = g_strdup(link_uri);
    action->image_uri = g_strdup(image_uri);
    action->media_uri = g_strdup(media_uri);
    action->insert_text = g_strdup(insert_text);
    action->x = x;
    action->y = y;

    gtk_container_add(GTK_CONTAINER(box), button);
    g_signal_connect_data(button, "clicked", G_CALLBACK(compose_webkit_menu_button_clicked),
                          action, compose_webkit_menu_action_free, 0);
}

static void compose_webkit_add_heading(GtkWidget *box, const gchar *title) {
    gchar *plain_title = compose_webkit_plain_menu_label(title);
    GtkWidget *label = gtk_label_new(plain_title);
    g_free(plain_title);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_widget_set_margin_start(label, 12);
    gtk_widget_set_margin_end(label, 12);
    gtk_widget_set_margin_top(label, 6);
    gtk_widget_set_margin_bottom(label, 4);
    gtk_style_context_add_class(gtk_widget_get_style_context(label), "dim-label");
    gtk_container_add(GTK_CONTAINER(box), label);
}

static GtkWidget *compose_webkit_add_submenu(GtkWidget *box, const gchar *title) {
    gchar *plain_title = compose_webkit_plain_menu_label(title);
    GtkWidget *button = gtk_menu_button_new();
    gtk_button_set_label(GTK_BUTTON(button), plain_title);
    gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
    gtk_widget_set_halign(button, GTK_ALIGN_FILL);
    gtk_widget_set_hexpand(button, TRUE);
    gtk_widget_set_can_focus(button, FALSE);
    gtk_style_context_add_class(gtk_widget_get_style_context(button), "modelbutton");

    GtkWidget *submenu = gtk_popover_new(button);
    GtkWidget *submenu_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(submenu), GTK_STYLE_CLASS_MENU);
    gtk_popover_set_modal(GTK_POPOVER(submenu), FALSE);
    gtk_widget_set_can_focus(submenu, FALSE);
    gtk_container_add(GTK_CONTAINER(submenu), submenu_box);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(button), submenu);
    gtk_container_add(GTK_CONTAINER(box), button);
    g_free(plain_title);
    return submenu_box;
}

static void compose_webkit_add_text_items(GtkWidget *box, GtkWidget *root_popover,
    WebKitWebView *view, const gchar *title, const gchar * const (*items)[2],
    gsize count, gint x, gint y)
{
    GtkWidget *submenu_box = compose_webkit_add_submenu(box, title);
    for (gsize i = 0; i < count; ++i)
        compose_webkit_add_menu_row(submenu_box, root_popover, view, items[i][0], WEBKIT_CONTEXT_MENU_ACTION_CUSTOM,
            NULL, NULL, NULL, NULL, NULL, items[i][1], x, y, TRUE);
    gtk_widget_show_all(submenu_box);
}

static void compose_webkit_append_menu_items(GtkWidget *box, GtkWidget *root_popover, WebKitContextMenu *menu,
    WebKitWebView *view, const gchar *link_uri, const gchar *image_uri,
    const gchar *media_uri, gint x, gint y)
{
    for (GList *link = webkit_context_menu_get_items(menu); link != NULL; link = link->next) {
        WebKitContextMenuItem *item = WEBKIT_CONTEXT_MENU_ITEM(link->data);
        if (webkit_context_menu_item_is_separator(item)) {
            gtk_container_add(GTK_CONTAINER(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
            continue;
        }

        const gchar *title = webkit_context_menu_item_get_title(item);
        WebKitContextMenuAction stock = webkit_context_menu_item_get_stock_action(item);
        if (stock == WEBKIT_CONTEXT_MENU_ACTION_INSERT_EMOJI) {
            static const gchar *emojis[][2] = {
                {"😀", "😀"}, {"😃", "😃"}, {"😄", "😄"}, {"😊", "😊"},
                {"😂", "😂"}, {"🥰", "🥰"}, {"👍", "👍"}, {"❤️", "❤️"}, {"🎵", "🎵"},
            };
            compose_webkit_add_text_items(box, root_popover, view, title != NULL ? title : "Insert Emoji",
                                          emojis, G_N_ELEMENTS(emojis), x, y);
            continue;
        }
        if (stock == WEBKIT_CONTEXT_MENU_ACTION_UNICODE) {
            static const gchar *controls[][2] = {
                {"Zero Width Space (U+200B)", "​"}, {"Zero Width Non-Joiner (U+200C)", "‌"},
                {"Zero Width Joiner (U+200D)", "‍"}, {"Left-to-Right Mark (U+200E)", "‎"},
                {"Right-to-Left Mark (U+200F)", "‏"}, {"Non-Breaking Space (U+00A0)", " "},
            };
            compose_webkit_add_text_items(box, root_popover, view,
                title != NULL ? title : "Insert Unicode Control Character",
                controls, G_N_ELEMENTS(controls), x, y);
            continue;
        }

        WebKitContextMenu *submenu = webkit_context_menu_item_get_submenu(item);
        if (submenu != NULL) {
            GtkWidget *submenu_box = compose_webkit_add_submenu(box, title);
            compose_webkit_append_menu_items(submenu_box, root_popover, submenu, view, link_uri, image_uri,
                                              media_uri, x, y);
            gtk_widget_show_all(submenu_box);
            continue;
        }

        GAction *source = webkit_context_menu_item_get_gaction(item);
        GVariant *target = NULL;
#if WEBKIT_CHECK_VERSION(2, 52, 0)
        if (source != NULL) target = webkit_context_menu_item_get_gaction_target(item);
#endif
        GVariant *source_state = source != NULL ? g_action_get_state(source) : NULL;
        gboolean enabled = source == NULL || g_action_get_enabled(source);
        gchar *display_title = NULL;
        if (source_state != NULL && g_variant_is_of_type(source_state, G_VARIANT_TYPE_BOOLEAN) &&
            g_variant_get_boolean(source_state))
            display_title = g_strdup_printf("✓ %s", title != NULL ? title : "");
        compose_webkit_add_menu_row(box, root_popover, view, display_title != NULL ? display_title : title,
            stock, source, target, link_uri, image_uri, media_uri,
            stock == WEBKIT_CONTEXT_MENU_ACTION_SPELLING_GUESS ? title : NULL,
            x, y, enabled);
        g_free(display_title);
        if (source_state != NULL) g_variant_unref(source_state);
    }
}

static gboolean compose_webkit_destroy_popover(gpointer data) {
    GtkWidget *popover = GTK_WIDGET(data);
    if (!gtk_widget_in_destruction(popover)) gtk_widget_destroy(popover);
    g_object_unref(popover);
    return G_SOURCE_REMOVE;
}

static void compose_webkit_context_popover_closed(GtkWidget *popover, gpointer user_data) {
    ComposeWebViewState *state = user_data;
    if (state->context_popover != popover) return;
    state->context_popover = NULL;
    g_idle_add_full(G_PRIORITY_DEFAULT, compose_webkit_destroy_popover,
                    g_object_ref(popover), NULL);
    g_object_unref(popover); // state-owned reference
}

static gboolean compose_webkit_event_is_inside_popover(GdkEvent *event, GtkWidget *popover) {
    GtkWidget *widget = gtk_get_event_widget(event);
    for (; widget != NULL; widget = gtk_widget_get_parent(widget)) {
        if (widget == popover) return TRUE;
        if (GTK_IS_POPOVER(widget)) {
            GtkWidget *relative = gtk_popover_get_relative_to(GTK_POPOVER(widget));
            for (GtkWidget *parent = relative; parent != NULL; parent = gtk_widget_get_parent(parent))
                if (parent == popover) return TRUE;
        }
    }
    return FALSE;
}

static GdkFilterReturn compose_webkit_outside_click_filter(GdkXEvent *, GdkEvent *event, gpointer data) {
    ComposeWebViewState *state = data;
    if (event == NULL || event->type != GDK_BUTTON_PRESS || state->context_popover == NULL)
        return GDK_FILTER_CONTINUE;
    if (!compose_webkit_event_is_inside_popover(event, state->context_popover))
        gtk_popover_popdown(GTK_POPOVER(state->context_popover));
    return GDK_FILTER_CONTINUE;
}

static WebKitWebView *compose_webkit_focused_view(GtkWidget *host) {
    GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(host));
    for (GtkWidget *widget = focus; widget != NULL; widget = gtk_widget_get_parent(widget)) {
        if (WEBKIT_IS_WEB_VIEW(widget)) return WEBKIT_WEB_VIEW(widget);
    }
    return NULL;
}

gboolean compose_webview_on_context_menu(
    WebKitWebView *web_view,
    WebKitContextMenu *context_menu,
    GdkEvent *event,
    WebKitHitTestResult *hit_test_result,
    gpointer user_data)
{
    ComposeWebViewState *state = user_data;
    if (state == NULL || state->web_view != web_view || state->context_popover != NULL)
        return TRUE;

    gint x = 0, y = 0;
    if (!webkit_context_menu_get_position(context_menu, &x, &y) && event != NULL) {
        gdouble event_x = 0, event_y = 0;
        if (gdk_event_get_coords(event, &event_x, &event_y)) {
            x = (gint)event_x;
            y = (gint)event_y;
        }
    }

    const gchar *link_uri = hit_test_result != NULL ? webkit_hit_test_result_get_link_uri(hit_test_result) : NULL;
    const gchar *image_uri = hit_test_result != NULL ? webkit_hit_test_result_get_image_uri(hit_test_result) : NULL;
    const gchar *media_uri = hit_test_result != NULL ? webkit_hit_test_result_get_media_uri(hit_test_result) : NULL;
    GtkWidget *popover = gtk_popover_new(GTK_WIDGET(web_view));
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(popover), GTK_STYLE_CLASS_MENU);
    compose_webkit_append_menu_items(box, popover, context_menu, web_view, link_uri, image_uri,
                                     media_uri, x, y);
    gtk_container_add(GTK_CONTAINER(popover), box);
    gtk_widget_show_all(box);

    GList *children = gtk_container_get_children(GTK_CONTAINER(box));
    gboolean empty = children == NULL;
    g_list_free(children);
    if (empty) {
        gtk_widget_destroy(popover);
        return TRUE;
    }

    GdkRectangle anchor = { x, y, 1, 1 };
    gtk_popover_set_pointing_to(GTK_POPOVER(popover), &anchor);
    gtk_popover_set_position(GTK_POPOVER(popover), GTK_POS_BOTTOM);
    gtk_popover_set_modal(GTK_POPOVER(popover), FALSE);
    gtk_widget_set_can_focus(popover, FALSE);
    state->context_popover = GTK_WIDGET(g_object_ref_sink(popover));
    g_signal_connect(popover, "closed", G_CALLBACK(compose_webkit_context_popover_closed), state);
    gtk_popover_popup(GTK_POPOVER(popover));
    return TRUE;
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

    if (event->type == GDK_BUTTON_PRESS) {
        if (state->context_popover != NULL &&
            !compose_webkit_event_is_inside_popover(event, state->context_popover))
            gtk_popover_popdown(GTK_POPOVER(state->context_popover));
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

    if (state->context_popover != NULL) {
        GdkEvent *event = gtk_get_current_event();
        gboolean inside_popover = event != NULL &&
            compose_webkit_event_is_inside_popover(event, state->context_popover);
        if (event != NULL) gdk_event_free(event);
        if (!inside_popover) gtk_popover_popdown(GTK_POPOVER(state->context_popover));
    }

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
    if (!state->event_filter_installed) {
        gdk_window_add_filter(NULL, compose_webkit_outside_click_filter, state);
        state->event_filter_installed = TRUE;
    }
    state->hierarchy_handler = g_signal_connect(state->web_view, "hierarchy-changed",
        G_CALLBACK(compose_webview_on_hierarchy_changed), state);
    compose_webview_on_hierarchy_changed(GTK_WIDGET(state->web_view), NULL, state);
}

void compose_webview_disconnect_input_routing(ComposeWebViewState *state) {
    if (state == NULL) return;
    if (state->event_filter_installed) {
        gdk_window_remove_filter(NULL, compose_webkit_outside_click_filter, state);
        state->event_filter_installed = FALSE;
    }
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
