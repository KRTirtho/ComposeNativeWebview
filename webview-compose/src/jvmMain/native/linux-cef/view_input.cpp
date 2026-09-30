#include "compose_cef_internal.h"

#include "include/wrapper/cef_helpers.h"

/* GTK signal handlers forwarding mouse / keyboard / scroll / focus to CEF.
 * All run on the GTK main thread; CEF ops are posted to TID_UI. */

namespace {

guint cefModifiersFromGdk(guint state) {
    guint modifiers = 0;
    if (state & GDK_SHIFT_MASK) modifiers |= EVENTFLAG_SHIFT_DOWN;
    if (state & GDK_CONTROL_MASK) modifiers |= EVENTFLAG_CONTROL_DOWN;
    if (state & GDK_MOD1_MASK) modifiers |= EVENTFLAG_ALT_DOWN;
    if (state & GDK_META_MASK) modifiers |= EVENTFLAG_COMMAND_DOWN;
    if (state & GDK_LOCK_MASK) modifiers |= EVENTFLAG_CAPS_LOCK_ON;
    if (state & GDK_BUTTON1_MASK) modifiers |= EVENTFLAG_LEFT_MOUSE_BUTTON;
    if (state & GDK_BUTTON2_MASK) modifiers |= EVENTFLAG_MIDDLE_MOUSE_BUTTON;
    if (state & GDK_BUTTON3_MASK) modifiers |= EVENTFLAG_RIGHT_MOUSE_BUTTON;
    return modifiers;
}

void attachGtkPointerDevice(GdkEvent *event, GdkWindow *window) {
    if (event == nullptr || window == nullptr) return;
    GdkDevice *device = gdk_event_get_device(event);
    if (device == nullptr) {
        GdkSeat *seat = gdk_display_get_default_seat(gdk_window_get_display(window));
        if (seat != nullptr) device = gdk_seat_get_pointer(seat);
    }
    if (device != nullptr) {
        gdk_event_set_device(event, device);
        gdk_event_set_source_device(event, device);
    }
}

int cefWindowsKeyCodeFromGdk(guint keyval) {
    switch (keyval) {
        case GDK_KEY_BackSpace: return 0x08;
        case GDK_KEY_Tab:
        case GDK_KEY_ISO_Left_Tab: return 0x09;
        case GDK_KEY_Return:
        case GDK_KEY_KP_Enter: return 0x0D;
        case GDK_KEY_Shift_L:
        case GDK_KEY_Shift_R: return 0x10;
        case GDK_KEY_Control_L:
        case GDK_KEY_Control_R: return 0x11;
        case GDK_KEY_Alt_L:
        case GDK_KEY_Alt_R: return 0x12;
        case GDK_KEY_Pause: return 0x13;
        case GDK_KEY_Caps_Lock: return 0x14;
        case GDK_KEY_Escape: return 0x1B;
        case GDK_KEY_space: return 0x20;
        case GDK_KEY_Page_Up: return 0x21;
        case GDK_KEY_Page_Down: return 0x22;
        case GDK_KEY_End: return 0x23;
        case GDK_KEY_Home: return 0x24;
        case GDK_KEY_Left: return 0x25;
        case GDK_KEY_Up: return 0x26;
        case GDK_KEY_Right: return 0x27;
        case GDK_KEY_Down: return 0x28;
        case GDK_KEY_Insert: return 0x2D;
        case GDK_KEY_Delete: return 0x2E;
        case GDK_KEY_KP_0: return 0x60;
        case GDK_KEY_KP_1: return 0x61;
        case GDK_KEY_KP_2: return 0x62;
        case GDK_KEY_KP_3: return 0x63;
        case GDK_KEY_KP_4: return 0x64;
        case GDK_KEY_KP_5: return 0x65;
        case GDK_KEY_KP_6: return 0x66;
        case GDK_KEY_KP_7: return 0x67;
        case GDK_KEY_KP_8: return 0x68;
        case GDK_KEY_KP_9: return 0x69;
        case GDK_KEY_KP_Multiply: return 0x6A;
        case GDK_KEY_KP_Add: return 0x6B;
        case GDK_KEY_KP_Subtract: return 0x6D;
        case GDK_KEY_KP_Decimal: return 0x6E;
        case GDK_KEY_KP_Divide: return 0x6F;
        default: break;
    }
    if (keyval >= GDK_KEY_F1 && keyval <= GDK_KEY_F24) {
        return 0x70 + static_cast<int>(keyval - GDK_KEY_F1);
    }
    const guint lower = gdk_keyval_to_lower(keyval);
    const gunichar unicode = gdk_keyval_to_unicode(lower);
    if (unicode >= 'a' && unicode <= 'z') return static_cast<int>(unicode - 'a' + 'A');
    if (unicode >= '0' && unicode <= '9') return static_cast<int>(unicode);
    switch (unicode) {
        case '-':
        case '_': return 0xBD;
        case '=':
        case '+': return 0xBB;
        case '[':
        case '{': return 0xDB;
        case ']':
        case '}': return 0xDD;
        case '\\':
        case '|': return 0xDC;
        case ';':
        case ':': return 0xBA;
        case '\'':
        case '"': return 0xDE;
        case ',':
        case '<': return 0xBC;
        case '.':
        case '>': return 0xBE;
        case '/':
        case '?': return 0xBF;
        case '`':
        case '~': return 0xC0;
        default: return 0;
    }
}

CefRefPtr<CefBrowser> browserOf(const std::shared_ptr<ComposeCefViewState> &state) {
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->browser;
}

gboolean onMotion(GtkWidget *, GdkEventMotion *event, gpointer data) {
    auto state = *static_cast<std::shared_ptr<ComposeCefViewState> *>(data);
    CefRefPtr<CefBrowser> browser = browserOf(state);
    if (browser == nullptr) return FALSE;
    CefMouseEvent mouse;
    mouse.x = static_cast<int>(event->x);
    mouse.y = static_cast<int>(event->y);
    mouse.modifiers = cefModifiersFromGdk(event->state);
    compose_cef_post_to_ui([browser, mouse] {
        browser->GetHost()->SendMouseMoveEvent(mouse, false);
    });
    return TRUE;
}

gboolean onButton(GtkWidget *widget, GdkEventButton *event, gpointer data) {
    auto state = *static_cast<std::shared_ptr<ComposeCefViewState> *>(data);
    if (event->type == GDK_BUTTON_RELEASE && event->button == 3) {
        // While a GTK context menu is open for this view, the right-button
        // release must not reach CEF (it would dismiss the menu before an
        // item can be chosen). Swallow it.
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->active_menu_widget != nullptr) return TRUE;
    }
    if (event->type == GDK_BUTTON_PRESS && event->button == 3) {
        GdkEvent *copied = gdk_event_copy(reinterpret_cast<GdkEvent *>(event));
        attachGtkPointerDevice(copied, event->window);
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->last_context_event != nullptr) gdk_event_free(state->last_context_event);
        state->last_context_event = copied;
    }
    CefRefPtr<CefBrowser> browser = browserOf(state);
    if (browser == nullptr) return FALSE;

    if (event->type == GDK_BUTTON_PRESS && event->button != 3) {
        gtk_widget_grab_focus(widget);
    }
    CefMouseEvent mouse;
    mouse.x = static_cast<int>(event->x);
    mouse.y = static_cast<int>(event->y);
    mouse.modifiers = cefModifiersFromGdk(event->state);
    CefBrowserHost::MouseButtonType button = MBT_LEFT;
    guint button_flag = EVENTFLAG_LEFT_MOUSE_BUTTON;
    if (event->button == 2) {
        button = MBT_MIDDLE;
        button_flag = EVENTFLAG_MIDDLE_MOUSE_BUTTON;
    } else if (event->button == 3) {
        button = MBT_RIGHT;
        button_flag = EVENTFLAG_RIGHT_MOUSE_BUTTON;
    } else if (event->button != 1) {
        return FALSE;
    }
    const bool mouse_up = event->type == GDK_BUTTON_RELEASE;
    if (!mouse_up) mouse.modifiers |= button_flag;
    const bool grab_focus = event->type == GDK_BUTTON_PRESS && event->button != 3;
    compose_cef_post_to_ui([browser, mouse, button, mouse_up, grab_focus] {
        if (grab_focus) browser->GetHost()->SetFocus(true);
        browser->GetHost()->SendMouseClickEvent(mouse, button, mouse_up, 1);
    });
    return TRUE;
}

gboolean onScroll(GtkWidget *, GdkEventScroll *event, gpointer data) {
    auto state = *static_cast<std::shared_ptr<ComposeCefViewState> *>(data);
    CefRefPtr<CefBrowser> browser = browserOf(state);
    if (browser == nullptr) return FALSE;
    double delta_x = 0.0;
    double delta_y = 0.0;
    if (event->direction == GDK_SCROLL_SMOOTH) {
        gdk_event_get_scroll_deltas(reinterpret_cast<GdkEvent *>(event), &delta_x, &delta_y);
    } else if (event->direction == GDK_SCROLL_UP) {
        delta_y = -1.0;
    } else if (event->direction == GDK_SCROLL_DOWN) {
        delta_y = 1.0;
    } else if (event->direction == GDK_SCROLL_LEFT) {
        delta_x = -1.0;
    } else if (event->direction == GDK_SCROLL_RIGHT) {
        delta_x = 1.0;
    }
    CefMouseEvent mouse;
    mouse.x = static_cast<int>(event->x);
    mouse.y = static_cast<int>(event->y);
    mouse.modifiers = cefModifiersFromGdk(event->state);
    compose_cef_post_to_ui([browser, mouse, delta_x, delta_y] {
        browser->GetHost()->SendMouseWheelEvent(
            mouse,
            static_cast<int>(delta_x * 120.0),
            static_cast<int>(delta_y * 120.0));
    });
    return TRUE;
}

gboolean onKey(GtkWidget *, GdkEventKey *event, gpointer data) {
    auto state = *static_cast<std::shared_ptr<ComposeCefViewState> *>(data);
    CefRefPtr<CefBrowser> browser = browserOf(state);
    if (browser == nullptr) return FALSE;
    CefKeyEvent key;
    key.type = event->type == GDK_KEY_RELEASE ? KEYEVENT_KEYUP : KEYEVENT_RAWKEYDOWN;
    key.windows_key_code = cefWindowsKeyCodeFromGdk(event->keyval);
    key.native_key_code = static_cast<int>(event->hardware_keycode);
    key.modifiers = cefModifiersFromGdk(event->state);
    key.is_system_key = (key.modifiers & EVENTFLAG_ALT_DOWN) != 0;
    const char32_t codepoint =
        event->type == GDK_KEY_PRESS ? gdk_keyval_to_unicode(event->keyval) : 0;
    key.character = 0;
    key.unmodified_character = 0;
    compose_cef_post_to_ui([browser, key, codepoint, is_release = event->type == GDK_KEY_RELEASE] {
        browser->GetHost()->SendKeyEvent(key);
        const bool has_shortcut_modifier =
            (key.modifiers & (EVENTFLAG_CONTROL_DOWN | EVENTFLAG_ALT_DOWN | EVENTFLAG_COMMAND_DOWN)) != 0;
        if (!is_release && !has_shortcut_modifier && codepoint > 0 && codepoint < 0x10000 &&
            !g_unichar_iscntrl(codepoint)) {
            CefKeyEvent character = key;
            character.type = KEYEVENT_CHAR;
            character.windows_key_code = static_cast<int>(codepoint);
            character.native_key_code = 0;
            character.character = static_cast<char16_t>(codepoint);
            character.unmodified_character = static_cast<char16_t>(codepoint);
            browser->GetHost()->SendKeyEvent(character);
        }
    });
    return TRUE;
}

gboolean onFocusIn(GtkWidget *, GdkEventFocus *, gpointer data) {
    auto state = *static_cast<std::shared_ptr<ComposeCefViewState> *>(data);
    CefRefPtr<CefBrowser> browser = browserOf(state);
    if (browser.get() != nullptr) {
        compose_cef_post_to_ui([browser] { browser->GetHost()->SetFocus(true); });
    }
    return FALSE;
}

gboolean onFocusOut(GtkWidget *, GdkEventFocus *, gpointer data) {
    auto state = *static_cast<std::shared_ptr<ComposeCefViewState> *>(data);
    CefRefPtr<CefBrowser> browser = browserOf(state);
    if (browser.get() != nullptr) {
        compose_cef_post_to_ui([browser] { browser->GetHost()->SetFocus(false); });
    }
    return FALSE;
}

}  // namespace

void compose_cef_connect_input(
    GtkWidget *widget,
    const std::shared_ptr<ComposeCefViewState> &state) {
    auto *state_data = new std::shared_ptr<ComposeCefViewState>(state);
    g_object_set_data_full(
        G_OBJECT(widget),
        "compose-cef-input-state",
        state_data,
        [](gpointer data) { delete static_cast<std::shared_ptr<ComposeCefViewState> *>(data); });

    g_signal_connect(widget, "motion-notify-event", G_CALLBACK(onMotion), state_data);
    g_signal_connect(widget, "button-press-event", G_CALLBACK(onButton), state_data);
    g_signal_connect(widget, "button-release-event", G_CALLBACK(onButton), state_data);
    g_signal_connect(widget, "scroll-event", G_CALLBACK(onScroll), state_data);
    g_signal_connect(widget, "key-press-event", G_CALLBACK(onKey), state_data);
    g_signal_connect(widget, "key-release-event", G_CALLBACK(onKey), state_data);
    g_signal_connect(widget, "focus-in-event", G_CALLBACK(onFocusIn), state_data);
    g_signal_connect(widget, "focus-out-event", G_CALLBACK(onFocusOut), state_data);
}
