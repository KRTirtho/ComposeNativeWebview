#include "cef_host_internal.h"

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

ComposeCefBrowserRef browserOf(const std::shared_ptr<ComposeCefViewState> &state) {
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->browser;
}

gboolean onMotion(GtkWidget *, GdkEventMotion *event, gpointer data) {
    auto state = *static_cast<std::shared_ptr<ComposeCefViewState> *>(data);
    ComposeCefBrowserRef browser = browserOf(state);
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
        // The GTK popover may grab the right-button release. Defer it until the
        // menu closes, but never leave CEF with an unmatched button-down.
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->active_menu_widget != nullptr) return TRUE;
    }
    if (event->type == GDK_BUTTON_PRESS && event->button == 3) {
        if (g_getenv("COMPOSE_CEF_DEBUG_MENU")) {
            g_printerr("CEF right-button press at %.0f,%.0f\n", event->x, event->y);
        }
        if (state->context_input_widget != nullptr) {
            if (g_signal_handler_is_connected(state->context_input_widget, state->context_input_handler)) {
                g_signal_handler_disconnect(state->context_input_widget, state->context_input_handler);
            }
            g_object_unref(state->context_input_widget);
        }
        if (state->context_press_event != nullptr) gdk_event_free(state->context_press_event);
        // gtk_widget_event() may have received a retargeted copy from Nucleus.
        // gtk_get_current_event() retains the real EventBox press on the GTK
        // event stack, whose release also needs to reach Compose.
        state->context_press_event = gtk_get_current_event();
        if (state->context_press_event == nullptr ||
            state->context_press_event->type != GDK_BUTTON_PRESS) {
            if (state->context_press_event != nullptr) gdk_event_free(state->context_press_event);
            state->context_press_event = gdk_event_copy(reinterpret_cast<GdkEvent *>(event));
        }
        GtkWidget *source = gtk_get_event_widget(state->context_press_event);
        state->context_input_widget = source != nullptr ? GTK_WIDGET(g_object_ref(source)) : nullptr;
        state->context_input_handler = 0;
        state->context_release_pending = true;
        if (source != nullptr) {
            // Observe the generic signal, before the EventBox's specific
            // button-release handler consumes it. Avoid duplicate releases
            // when the physical release arrived before the popover opened.
            state->context_input_handler = g_signal_connect(source, "event",
                G_CALLBACK(+[](GtkWidget *, GdkEvent *event, gpointer data) -> gboolean {
                    if (event->type == GDK_BUTTON_RELEASE && event->button.button == 3) {
                        static_cast<ComposeCefViewState *>(data)->context_release_pending = false;
                    }
                    return FALSE;
                }), state.get());
        }
    }
    ComposeCefBrowserRef browser = browserOf(state);
    if (browser == nullptr) return FALSE;

    if (event->type == GDK_BUTTON_PRESS) {
        gtk_widget_grab_focus(widget);
        compose_cef_call_on_pointer_focus(state->handle);
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
    if (button == MBT_RIGHT) {
        if (mouse_up) {
            // The popup may already have sent a synthetic release; don't send
            // another if GTK also delivers the physical one later.
            if (!state->right_button_pending.exchange(false)) return TRUE;
        } else {
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->last_context_mouse = mouse;
            }
            state->right_button_pending.store(true);
        }
    }
    const bool grab_focus = event->type == GDK_BUTTON_PRESS;
    compose_cef_post_to_ui([browser, mouse, button, mouse_up, grab_focus] {
        if (grab_focus) browser->GetHost()->SetFocus(true);
        browser->GetHost()->SendMouseClickEvent(mouse, button, mouse_up, 1);
    });
    return TRUE;
}

gboolean onScroll(GtkWidget *, GdkEventScroll *event, gpointer data) {
    auto state = *static_cast<std::shared_ptr<ComposeCefViewState> *>(data);
    ComposeCefBrowserRef browser = browserOf(state);
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
            static_cast<int>(-delta_y * 120.0));
    });
    return TRUE;
}

gboolean onKey(GtkWidget *, GdkEventKey *event, gpointer data) {
    auto state = *static_cast<std::shared_ptr<ComposeCefViewState> *>(data);
    ComposeCefBrowserRef browser = browserOf(state);
    if (browser == nullptr) return FALSE;
    const bool control = (event->state & GDK_CONTROL_MASK) != 0;
    const bool other_modifier = (event->state & (GDK_MOD1_MASK | GDK_META_MASK)) != 0;
    if (control && !other_modifier) {
        const guint keyval = gdk_keyval_to_lower(event->keyval);
        const bool shift = (event->state & GDK_SHIFT_MASK) != 0;
        if (keyval == GDK_KEY_c || keyval == GDK_KEY_x || keyval == GDK_KEY_v ||
            keyval == GDK_KEY_a || keyval == GDK_KEY_z || keyval == GDK_KEY_y) {
            if (event->type == GDK_KEY_PRESS) {
                if (g_getenv("COMPOSE_CEF_DEBUG_INPUT")) {
                    g_printerr("CEF edit shortcut: %c (shift=%d)\n",
                               static_cast<char>(keyval), shift);
                }
                if (keyval == GDK_KEY_v) {
                    compose_cef_paste_system_clipboard(browser);
                } else {
                    compose_cef_post_to_ui([state, browser, keyval, shift] {
                        ComposeCefFrameRef frame = browser->GetFocusedFrame();
                        if (frame == nullptr) frame = browser->GetMainFrame();
                        if (frame == nullptr) return;
                        switch (keyval) {
                            case GDK_KEY_c: compose_cef_copy_selection(state, browser, frame, false); break;
                            case GDK_KEY_x: compose_cef_copy_selection(state, browser, frame, true); break;
                            case GDK_KEY_a: frame->SelectAll(); break;
                            case GDK_KEY_z:
                                if (shift) frame->Redo();
                                else frame->Undo();
                                break;
                            case GDK_KEY_y: frame->Redo(); break;
                        }
                    });
                }
            }
            return TRUE;
        }
    }
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
    if (g_getenv("COMPOSE_CEF_DEBUG_INPUT")) g_printerr("CEF GTK view focus in\n");
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->active_menu_widget != nullptr) return FALSE;
    }
    ComposeCefBrowserRef browser = browserOf(state);
    if (browser.get() != nullptr) {
        compose_cef_post_to_ui([browser] { browser->GetHost()->SetFocus(true); });
    }
    return FALSE;
}

gboolean onFocusOut(GtkWidget *, GdkEventFocus *, gpointer data) {
    auto state = *static_cast<std::shared_ptr<ComposeCefViewState> *>(data);
    if (g_getenv("COMPOSE_CEF_DEBUG_INPUT")) g_printerr("CEF GTK view focus out\n");
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        // Modal popover focus is temporary. Blurring CEF here cancels its
        // pending context menu before the user can choose an action.
        if (state->active_menu_widget != nullptr) return FALSE;
    }
    ComposeCefBrowserRef browser = browserOf(state);
    if (browser.get() != nullptr) {
        compose_cef_post_to_ui([browser] { browser->GetHost()->SetFocus(false); });
    }
    return FALSE;
}

void blurIfOutsideView(GtkWidget *top, gdouble x, gdouble y, ComposeCefViewState *state) {
    GtkWidget *widget = state->widget;
    if (widget == nullptr || !GTK_IS_WINDOW(top)) return;
    // GtkOverlay gives each child its own GdkWindow. GTK's widget-coordinate
    // translation reports (0,0) for the embed even when Nucleus positions it
    // at (0,120) in the content area. Use the same rect that Nucleus passes to
    // GtkOverlay::get-child-position for pointer hit testing.
    struct NucleusRect { gint x, y, width, height, valid; };
    const auto *rect = static_cast<const NucleusRect *>(
        g_object_get_data(G_OBJECT(widget), "nucleus_tao_widget_rect"));
    if (rect != nullptr && rect->valid) {
        GtkWidget *content = gtk_bin_get_child(GTK_BIN(top));
        int cx = static_cast<int>(x), cy = static_cast<int>(y);
        if (content != nullptr) {
            gtk_widget_translate_coordinates(top, content, static_cast<int>(x),
                                             static_cast<int>(y), &cx, &cy);
        }
        if (cx >= rect->x && cy >= rect->y &&
            cx < rect->x + rect->width && cy < rect->y + rect->height) return;
    } else {
        // Fallback for GTK hosts without Nucleus's cached overlay rectangle.
        int wx = 0, wy = 0;
        if (gtk_widget_translate_coordinates(top, widget, static_cast<int>(x),
                                             static_cast<int>(y), &wx, &wy)) {
            GtkAllocation allocation;
            gtk_widget_get_allocation(widget, &allocation);
            if (wx >= 0 && wy >= 0 && wx < allocation.width && wy < allocation.height) return;
        }
    }
    // Nucleus paints Compose over the same GTK window; moving Compose focus
    // does not always change GTK's focused widget. Capture the actual GTK
    // press before a child consumes it and relinquish CEF's keyboard focus.
    if (g_getenv("COMPOSE_CEF_DEBUG_INPUT")) {
        GdkEvent *event = gtk_get_current_event();
        GtkWidget *target = event != nullptr ? gtk_get_event_widget(event) : nullptr;
        g_printerr("CEF outside press: blur (focus=%d target=%s)\n",
                   gtk_widget_has_focus(widget), target != nullptr ? G_OBJECT_TYPE_NAME(target) : "none");
        if (event != nullptr) gdk_event_free(event);
    }
    if (gtk_widget_has_focus(widget)) gtk_window_set_focus(GTK_WINDOW(top), nullptr);
    ComposeCefBrowserRef browser;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        browser = state->browser;
    }
    if (browser != nullptr) {
        compose_cef_post_to_ui([browser] { browser->GetHost()->SetFocus(false); });
    }
}

void onOutsidePress(GtkGestureMultiPress *gesture, gint, gdouble x, gdouble y,
                    gpointer data) {
    blurIfOutsideView(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture)),
                      x, y, static_cast<ComposeCefViewState *>(data));
}

void onHierarchyChanged(GtkWidget *widget, GtkWidget *, gpointer data) {
    auto *state = static_cast<ComposeCefViewState *>(data);
    if (state->outside_press_gesture != nullptr) {
        g_signal_handlers_disconnect_by_data(state->outside_press_gesture, state);
        g_object_unref(state->outside_press_gesture);
        state->outside_press_gesture = nullptr;
    }
    GtkWidget *top = gtk_widget_get_toplevel(widget);
    if (state->closing || !GTK_IS_WINDOW(top)) return;
    if (g_object_get_data(G_OBJECT(top), "compose-cef-host-event-filter") == nullptr) {
        // GTK registers popover subsurface windows on the host GtkWindow.
        // Its class handler ignores their configure events, but Tao's user
        // handlers run first and otherwise mistake popover size/focus for
        // the app window's. Filter before the specific event signals fire.
        // The connection belongs to the host window, not this view, so it
        // also covers late events after a popover/view has been destroyed.
        g_signal_connect(top, "event", G_CALLBACK(+[](GtkWidget *host, GdkEvent *event, gpointer) -> gboolean {
            if (event->type == GDK_BUTTON_PRESS && event->any.window == gtk_widget_get_window(host)) {
                GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(host));
                auto *input = focus != nullptr
                    ? static_cast<std::shared_ptr<ComposeCefViewState> *>(
                        g_object_get_data(G_OBJECT(focus), "compose-cef-input-state"))
                    : nullptr;
                if (input != nullptr) {
                    bool can_blur = false;
                    {
                        std::lock_guard<std::mutex> lock((*input)->mutex);
                        can_blur = !(*input)->closing && (*input)->active_menu_widget == nullptr;
                    }
                    // Tao may consume a toplevel press during GTK's WM-event
                    // precheck, before capture-phase gestures see it. Update
                    // native focus here as well, but let the actual press
                    // continue to Compose for normal hit testing.
                    if (can_blur) blurIfOutsideView(host, event->button.x, event->button.y, input->get());
                }
            }
            if (event->type == GDK_KEY_PRESS || event->type == GDK_KEY_RELEASE) {
                GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(host));
                auto *input = focus != nullptr
                    ? static_cast<std::shared_ptr<ComposeCefViewState> *>(
                        g_object_get_data(G_OBJECT(focus), "compose-cef-input-state"))
                    : nullptr;
                if (input != nullptr) {
                    {
                        std::lock_guard<std::mutex> lock((*input)->mutex);
                        if ((*input)->closing || (*input)->active_menu_widget != nullptr) return FALSE;
                    }
                    // Nucleus 2.5.18 filters keys through Tao's Compose IME
                    // in GtkWindow::key-press-event, before GTK's class
                    // handler propagates them to the focused native widget.
                    // Route native CEF focus at the generic event boundary,
                    // before that filter can consume text or leak shortcuts
                    // into Compose. Popovers and Compose retain normal GTK
                    // key routing when their widgets own keyboard focus.
                    return onKey(focus, &event->key, input);
                }
            }
            if ((event->type == GDK_CONFIGURE || event->type == GDK_FOCUS_CHANGE) &&
                event->any.window != gtk_widget_get_window(host)) {
                return TRUE;
            }
            return FALSE;
        }), nullptr);
        g_object_set_data(G_OBJECT(top), "compose-cef-host-event-filter", GINT_TO_POINTER(1));
    }
    if (g_getenv("COMPOSE_CEF_DEBUG_INPUT")) g_printerr("CEF GTK focus capture attached\n");
    GtkGesture *gesture = gtk_gesture_multi_press_new(top);
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gesture), 0);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(gesture), GTK_PHASE_CAPTURE);
    g_signal_connect(gesture, "pressed", G_CALLBACK(onOutsidePress), state);
    state->outside_press_gesture = gesture;
}

}  // namespace

void compose_cef_finish_context_input(const std::shared_ptr<ComposeCefViewState> &state) {
    if (!state->context_release_pending || state->context_press_event == nullptr) return;
    state->context_release_pending = false;
    GdkEvent *release = gdk_event_copy(state->context_press_event);
    release->type = GDK_BUTTON_RELEASE;
    release->button.state &= ~GDK_BUTTON3_MASK;
    release->button.send_event = TRUE;
    if (g_getenv("COMPOSE_CEF_DEBUG_MENU")) {
        g_printerr("CEF menu: balancing original GTK/Compose right-button release\n");
    }
    // Use normal GTK dispatch after removing the modal grab. This balances
    // both GTK capture gestures and Nucleus's Compose pointer capture, which
    // would otherwise keep forwarding outside clicks to the WebView.
    gtk_main_do_event(release);
    gdk_event_free(release);
}

void compose_cef_disconnect_input(const std::shared_ptr<ComposeCefViewState> &state) {
    compose_cef_finish_context_input(state);
    if (state->context_input_widget != nullptr) {
        if (g_signal_handler_is_connected(state->context_input_widget, state->context_input_handler)) {
            g_signal_handler_disconnect(state->context_input_widget, state->context_input_handler);
        }
        g_object_unref(state->context_input_widget);
        state->context_input_widget = nullptr;
        state->context_input_handler = 0;
    }
    if (state->context_press_event != nullptr) {
        gdk_event_free(state->context_press_event);
        state->context_press_event = nullptr;
    }
    GtkGesture *gesture = state->outside_press_gesture;
    state->outside_press_gesture = nullptr;
    if (gesture != nullptr) {
        g_signal_handlers_disconnect_by_data(gesture, state.get());
        g_object_unref(gesture);
    }
}

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
    g_signal_connect(widget, "hierarchy-changed", G_CALLBACK(onHierarchyChanged), state.get());
}
