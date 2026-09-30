#include "compose_cef_internal.h"

#include "include/wrapper/cef_helpers.h"

/* Native GTK context menu built from CEF's menu model, filtered to the
 * Chromium essentials. Selecting an item forwards the real CEF command id back
 * to Chromium via CefRunContextMenuCallback::Continue. The right-button release
 * that follows the press is swallowed in view_input.cpp so the menu survives. */

namespace {

struct GtkMenuEntry {
    cef_menu_item_type_t type = MENUITEMTYPE_NONE;
    int command_id = -1;
    std::string label;
    bool enabled = true;
    bool checked = false;
    std::vector<GtkMenuEntry> children;
};

struct GtkContextMenuState {
    CefRefPtr<CefRunContextMenuCallback> callback;
    std::shared_ptr<ComposeCefViewState> owner;
    GtkWidget *parent_widget = nullptr;
    GtkWidget *menu_widget = nullptr;
    GdkEvent *trigger_event = nullptr;
    std::atomic<bool> completed{false};
    std::vector<GtkMenuEntry> entries;
    int x = 0;
    int y = 0;

    ~GtkContextMenuState() {
        if (parent_widget != nullptr) g_object_unref(parent_widget);
        if (trigger_event != nullptr) gdk_event_free(trigger_event);
    }
};

struct GtkContextMenuAction {
    std::shared_ptr<GtkContextMenuState> menu;
    int command_id = -1;
};

void completeContextMenu(const std::shared_ptr<GtkContextMenuState> &menu, int command_id) {
    bool expected = false;
    if (!menu->completed.compare_exchange_strong(expected, true)) return;
    if (menu->owner != nullptr) {
        std::lock_guard<std::mutex> lock(menu->owner->mutex);
        if (menu->owner->active_menu_widget == menu->menu_widget) {
            menu->owner->active_menu_widget = nullptr;
        }
    }
    auto callback = menu->callback;
    compose_cef_post_to_ui([callback, command_id] {
        if (command_id >= 0) callback->Continue(command_id, EVENTFLAG_NONE);
        else callback->Cancel();
    });
}

std::string gtkMenuLabel(const std::string &cef_label) {
    std::string label;
    label.reserve(cef_label.size());
    for (size_t index = 0; index < cef_label.size(); ++index) {
        if (cef_label[index] == '&') {
            if (index + 1 < cef_label.size() && cef_label[index + 1] == '&') {
                label.push_back('&');
                ++index;
            }
            continue;
        }
        label.push_back(cef_label[index]);
    }
    // Chromium sentence-cases menu labels; Chrome uses title case.
    if (label == "Select all") label = "Select All";
    return label;
}

bool isAllowedContextMenuCommand(int command_id) {
    switch (command_id) {
        case IDC_CONTENT_CONTEXT_UNDO:
        case IDC_CONTENT_CONTEXT_REDO:
        case IDC_CONTENT_CONTEXT_CUT:
        case IDC_CONTENT_CONTEXT_COPY:
        case IDC_CONTENT_CONTEXT_PASTE:
        case IDC_CONTENT_CONTEXT_PASTE_AND_MATCH_STYLE:
        case IDC_CONTENT_CONTEXT_DELETE:
        case IDC_CONTENT_CONTEXT_SELECTALL:
        case IDC_BACK:
        case IDC_FORWARD:
        case IDC_RELOAD:
        case IDC_CONTENT_CONTEXT_OPENLINKNEWTAB:
        case IDC_CONTENT_CONTEXT_COPYLINKLOCATION:
        case IDC_CONTENT_CONTEXT_COPYIMAGELOCATION:
        case IDC_CONTENT_CONTEXT_COPYIMAGE:
            return true;
        default:
            return false;
    }
}

std::vector<GtkMenuEntry> copyMenuModel(CefRefPtr<CefMenuModel> model) {
    std::vector<GtkMenuEntry> entries;
    if (model.get() == nullptr) return entries;
    const size_t count = model->GetCount();
    entries.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        if (!model->IsVisibleAt(index)) continue;
        GtkMenuEntry entry;
        entry.type = model->GetTypeAt(index);
        entry.command_id = model->GetCommandIdAt(index);
        entry.label = gtkMenuLabel(model->GetLabelAt(index).ToString());
        entry.enabled = model->IsEnabledAt(index);
        entry.checked = model->IsCheckedAt(index);
        if (entry.type == MENUITEMTYPE_SUBMENU) {
            entry.children = copyMenuModel(model->GetSubMenuAt(index));
            if (entry.children.empty()) continue;
        } else if (entry.type == MENUITEMTYPE_SEPARATOR) {
            if (entries.empty() || entries.back().type == MENUITEMTYPE_SEPARATOR) continue;
        } else if (!isAllowedContextMenuCommand(entry.command_id)) {
            continue;
        }
        entries.push_back(std::move(entry));
    }
    while (!entries.empty() && entries.back().type == MENUITEMTYPE_SEPARATOR) {
        entries.pop_back();
    }
    return entries;
}

void onGtkMenuItemActivated(GtkMenuItem *, gpointer data) {
    auto *action = static_cast<std::shared_ptr<GtkContextMenuAction> *>(data);
    completeContextMenu((*action)->menu, (*action)->command_id);
}

void appendGtkMenuEntries(
    GtkWidget *menu_widget,
    const std::vector<GtkMenuEntry> &entries,
    const std::shared_ptr<GtkContextMenuState> &menu_state) {
    for (const GtkMenuEntry &entry : entries) {
        if (entry.type == MENUITEMTYPE_SEPARATOR) {
            gtk_menu_shell_append(GTK_MENU_SHELL(menu_widget), gtk_separator_menu_item_new());
            continue;
        }
        GtkWidget *item = nullptr;
        if (entry.type == MENUITEMTYPE_CHECK || entry.type == MENUITEMTYPE_RADIO) {
            item = gtk_check_menu_item_new_with_label(entry.label.c_str());
            gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), entry.checked);
        } else {
            item = gtk_menu_item_new_with_label(entry.label.c_str());
        }
        gtk_widget_set_sensitive(item, entry.enabled);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu_widget), item);

        if (entry.type == MENUITEMTYPE_SUBMENU) {
            GtkWidget *submenu = gtk_menu_new();
            appendGtkMenuEntries(submenu, entry.children, menu_state);
            gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), submenu);
        } else if (entry.command_id >= 0) {
            auto *action = new std::shared_ptr<GtkContextMenuAction>(
                std::make_shared<GtkContextMenuAction>(
                    GtkContextMenuAction{menu_state, entry.command_id}));
            g_signal_connect_data(
                item,
                "activate",
                G_CALLBACK(onGtkMenuItemActivated),
                action,
                [](gpointer data, GClosure *) {
                    delete static_cast<std::shared_ptr<GtkContextMenuAction> *>(data);
                },
                G_CONNECT_DEFAULT);
        }
    }
}

gboolean showGtkContextMenu(gpointer data) {
    auto *menu_ref = static_cast<std::shared_ptr<GtkContextMenuState> *>(data);
    const auto &state = *menu_ref;
    if (state->completed) return G_SOURCE_REMOVE;
    if (state->entries.empty() || state->parent_widget == nullptr ||
        !gtk_widget_get_realized(state->parent_widget)) {
        completeContextMenu(state, -1);
        return G_SOURCE_REMOVE;
    }

    GtkWidget *gtk_menu = gtk_menu_new();
    bool owner_closing = false;
    {
        std::lock_guard<std::mutex> lock(state->owner->mutex);
        owner_closing = state->owner->closing;
        if (!owner_closing) {
            state->owner->active_menu_widget = gtk_menu;
            state->menu_widget = gtk_menu;
        }
    }
    if (owner_closing) {
        completeContextMenu(state, -1);
        gtk_widget_destroy(gtk_menu);
        return G_SOURCE_REMOVE;
    }

    appendGtkMenuEntries(gtk_menu, state->entries, state);
    auto *deactivate_payload = new std::shared_ptr<GtkContextMenuState>(state);
    g_signal_connect_data(
        gtk_menu,
        "deactivate",
        G_CALLBACK(+[](GtkMenuShell *shell, gpointer context) {
            auto *menu_state = static_cast<std::shared_ptr<GtkContextMenuState> *>(context);
            (void)shell;
            completeContextMenu(*menu_state, -1);
        }),
        deactivate_payload,
        [](gpointer value, GClosure *) {
            delete static_cast<std::shared_ptr<GtkContextMenuState> *>(value);
        },
        G_CONNECT_DEFAULT);
    gtk_menu_attach_to_widget(GTK_MENU(gtk_menu), state->parent_widget, nullptr);
    gtk_widget_show_all(gtk_menu);
    GtkAllocation allocation;
    gtk_widget_get_allocation(state->parent_widget, &allocation);
    GdkRectangle anchor{allocation.x + state->x, allocation.y + state->y, 1, 1};
    GdkWindow *parent_window = gtk_widget_get_window(state->parent_widget);
    if (parent_window == nullptr) {
        completeContextMenu(state, -1);
        gtk_widget_destroy(gtk_menu);
        return G_SOURCE_REMOVE;
    }
    gtk_menu_popup_at_rect(
        GTK_MENU(gtk_menu),
        parent_window,
        &anchor,
        GDK_GRAVITY_SOUTH_WEST,
        GDK_GRAVITY_NORTH_WEST,
        state->trigger_event);
    if (state->trigger_event != nullptr) gdk_event_free(state->trigger_event);
    state->trigger_event = nullptr;
    return G_SOURCE_REMOVE;
}

}  // namespace

bool compose_cef_run_context_menu(
    const std::shared_ptr<ComposeCefViewState> &state,
    CefRefPtr<CefMenuModel> model,
    CefRefPtr<CefRunContextMenuCallback> callback,
    CefRefPtr<CefContextMenuParams> params) {
    GtkWidget *widget = nullptr;
    GdkEvent *trigger_event = nullptr;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        widget = state->widget;
        if (widget != nullptr) g_object_ref(widget);
        if (state->last_context_event != nullptr) {
            trigger_event = gdk_event_copy(state->last_context_event);
        }
    }
    auto menu = std::make_shared<GtkContextMenuState>();
    menu->callback = callback;
    menu->owner = state;
    menu->entries = copyMenuModel(model);
    menu->parent_widget = widget;
    menu->trigger_event = trigger_event;
    menu->x = params->GetXCoord();
    menu->y = params->GetYCoord();
    auto *payload = new std::shared_ptr<GtkContextMenuState>(menu);
    g_idle_add_full(
        G_PRIORITY_DEFAULT,
        showGtkContextMenu,
        payload,
        [](gpointer value) {
            delete static_cast<std::shared_ptr<GtkContextMenuState> *>(value);
        });
    return true;
}
