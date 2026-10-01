#include "compose_cef_internal.h"

#include "include/cef_image.h"
#include "include/wrapper/cef_helpers.h"

#include <utility>

/* Native GTK context menu built from CEF's menu model, filtered to the
 * Chromium essentials. Selecting an item runs its browser/frame action on
 * CEF's UI thread. If the popover takes the right-button release, menu
 * completion sends that missing release to CEF so its mouse state is balanced. */

namespace {

constexpr int kCopyLinkAddress = MENU_ID_USER_FIRST;
constexpr int kCopyImageAddress = MENU_ID_USER_FIRST + 1;
constexpr int kCopyImage = MENU_ID_USER_FIRST + 2;

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
    CefRefPtr<CefBrowser> browser;
    CefRefPtr<CefFrame> frame;
    std::shared_ptr<ComposeCefViewState> owner;
    std::string link_url;
    std::string source_url;
    GtkWidget *parent_widget = nullptr;
    GtkWidget *menu_widget = nullptr;
    GtkWidget *host_widget = nullptr;
    std::vector<gulong> host_input_handlers;
    std::atomic<bool> completed{false};
    std::vector<GtkMenuEntry> entries;
    int x = 0;
    int y = 0;

    ~GtkContextMenuState() {
        // The last shared reference can be released by the CEF UI task.
        // Keep widget finalization on the GTK main thread.
        auto *widgets = new std::pair<GtkWidget *, GtkWidget *>(parent_widget, host_widget);
        g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer data) -> gboolean {
            auto *widgets = static_cast<std::pair<GtkWidget *, GtkWidget *> *>(data);
            if (widgets->first != nullptr) g_object_unref(widgets->first);
            if (widgets->second != nullptr) g_object_unref(widgets->second);
            return G_SOURCE_REMOVE;
        }, widgets, [](gpointer data) {
            delete static_cast<std::pair<GtkWidget *, GtkWidget *> *>(data);
        });
    }
};

struct GtkContextMenuAction {
    std::shared_ptr<GtkContextMenuState> menu;
    int command_id = -1;
};

void completeContextMenu(const std::shared_ptr<GtkContextMenuState> &menu, int command_id) {
    bool expected = false;
    if (!menu->completed.compare_exchange_strong(expected, true)) return;
    GtkWidget *widget = menu->menu_widget;
    if (menu->owner != nullptr) {
        std::lock_guard<std::mutex> lock(menu->owner->mutex);
        if (menu->owner->active_menu_widget == menu->menu_widget) {
            menu->owner->active_menu_widget = nullptr;
        }
    }
    compose_cef_post_to_ui([menu, command_id] {
        // Complete CEF's pending menu first, then execute our selected action
        // directly. Continue(IDC_*) is not reliable with Chrome-style OSR
        // menus and silently ignores navigation/editing on some builds.
        menu->callback->Cancel();
        bool closing = false;
        {
            std::lock_guard<std::mutex> lock(menu->owner->mutex);
            closing = menu->owner->closing;
        }
        if (menu->owner->right_button_pending.exchange(false) && !closing &&
            menu->browser != nullptr) {
            if (g_getenv("COMPOSE_CEF_DEBUG_MENU")) {
                g_printerr("CEF menu: sending missing right-button release\n");
            }
            CefMouseEvent mouse;
            {
                std::lock_guard<std::mutex> lock(menu->owner->mutex);
                mouse = menu->owner->last_context_mouse;
            }
            mouse.modifiers &= ~EVENTFLAG_RIGHT_MOUSE_BUTTON;
            menu->browser->GetHost()->SendMouseClickEvent(mouse, MBT_RIGHT, true, 1);
        }
        if (!closing && command_id >= 0) {
            compose_cef_execute_context_menu_command(
                menu->owner, menu->browser, menu->frame,
                menu->link_url, menu->source_url, command_id);
        }
    });
    if (widget != nullptr) {
        menu->menu_widget = nullptr;
        // Defer popdown/destruction until the current clicked/closed signal
        // returns; the popover's own handlers are still on the stack.
        g_object_ref(widget);
        auto *cleanup = new std::pair<GtkWidget *, std::shared_ptr<GtkContextMenuState>>(
            widget, menu);
        g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer data) -> gboolean {
            auto *cleanup = static_cast<std::pair<GtkWidget *, std::shared_ptr<GtkContextMenuState>> *>(data);
            auto *popover = cleanup->first;
            if (!gtk_widget_in_destruction(popover)) {
                // Hide synchronously: release modality and restore the GTK
                // focus chain before destruction, without a frame-clock race.
                gtk_widget_hide(popover);
                gtk_widget_destroy(popover);
            }
            const auto &menu = cleanup->second;
            // Restore exactly the handlers we suspended (not handlers that
            // the host had already blocked for its own reasons).
            if (menu->host_widget != nullptr) {
                for (gulong handler : menu->host_input_handlers) {
                    if (g_signal_handler_is_connected(menu->host_widget, handler)) {
                        g_signal_handler_unblock(menu->host_widget, handler);
                    }
                }
                menu->host_input_handlers.clear();
                // Publish subsurface teardown before accepting the next
                // pointer sequence, even when Compose has no redraw pending.
                gtk_widget_queue_draw(menu->host_widget);
                gdk_display_flush(gtk_widget_get_display(menu->host_widget));
            }
            compose_cef_finish_context_input(menu->owner);
            CefRefPtr<CefBrowser> browser;
            bool focused = false;
            {
                std::lock_guard<std::mutex> lock(menu->owner->mutex);
                if (!menu->owner->closing) {
                    browser = menu->owner->browser;
                    focused = menu->owner->widget != nullptr &&
                              gtk_widget_has_focus(menu->owner->widget);
                }
            }
            // GTK may restore focus while the menu is still marked active,
            // so reconcile CEF after all popover focus bookkeeping is done.
            if (browser != nullptr) {
                compose_cef_post_to_ui([browser, focused] { browser->GetHost()->SetFocus(focused); });
            }
            if (g_getenv("COMPOSE_CEF_DEBUG_MENU")) {
                g_printerr("CEF menu cleanup: focused=%d grab=%p\n", focused, gtk_grab_get_current());
            }
            return G_SOURCE_REMOVE;
        }, cleanup, [](gpointer data) {
            auto *cleanup = static_cast<std::pair<GtkWidget *, std::shared_ptr<GtkContextMenuState>> *>(data);
            g_object_unref(cleanup->first);
            delete cleanup;
        });
    }
}

void copyTextToClipboard(const std::string &text) {
    auto *payload = new std::string(text);
    g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer data) -> gboolean {
        const auto &value = *static_cast<std::string *>(data);
        gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD),
                               value.c_str(), static_cast<gint>(value.size()));
        return G_SOURCE_REMOVE;
    }, payload, [](gpointer data) { delete static_cast<std::string *>(data); });
}

class CopyImageCallback final : public CefDownloadImageCallback {
public:
    void OnDownloadImageFinished(const CefString &, int status,
                                 CefRefPtr<CefImage> image) override {
        if ((status != 0 && (status < 200 || status >= 300)) ||
            image == nullptr || image->IsEmpty()) return;
        int width = 0, height = 0;
        CefRefPtr<CefBinaryValue> png = image->GetAsPNG(1.f, true, width, height);
        if (png == nullptr || png->GetSize() == 0) return;
        auto *bytes = new std::vector<uint8_t>(png->GetSize());
        png->GetData(bytes->data(), bytes->size(), 0);
        g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer data) -> gboolean {
            const auto &png_data = *static_cast<std::vector<uint8_t> *>(data);
            GdkPixbufLoader *loader = gdk_pixbuf_loader_new();
            if (gdk_pixbuf_loader_write(loader, png_data.data(), png_data.size(), nullptr) &&
                gdk_pixbuf_loader_close(loader, nullptr)) {
                GdkPixbuf *pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);
                if (pixbuf != nullptr) {
                    gtk_clipboard_set_image(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), pixbuf);
                }
            }
            g_object_unref(loader);
            return G_SOURCE_REMOVE;
        }, bytes, [](gpointer data) { delete static_cast<std::vector<uint8_t> *>(data); });
    }

private:
    IMPLEMENT_REFCOUNTING(CopyImageCallback);
};

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
        case MENU_ID_UNDO:
        case MENU_ID_REDO:
        case MENU_ID_CUT:
        case MENU_ID_COPY:
        case MENU_ID_PASTE:
        case MENU_ID_PASTE_MATCH_STYLE:
        case MENU_ID_DELETE:
        case MENU_ID_SELECT_ALL:
        case MENU_ID_BACK:
        case MENU_ID_FORWARD:
        case MENU_ID_RELOAD:
        case kCopyLinkAddress:
        case kCopyImageAddress:
        case kCopyImage:
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

void onGtkMenuItemActivated(GSimpleAction *, GVariant *, gpointer data) {
    auto *action = static_cast<std::shared_ptr<GtkContextMenuAction> *>(data);
    completeContextMenu((*action)->menu, (*action)->command_id);
}

GMenu *createGtkMenuModel(
    const std::vector<GtkMenuEntry> &entries,
    const std::shared_ptr<GtkContextMenuState> &menu_state,
    GSimpleActionGroup *actions,
    int &action_index) {
    GMenu *model = g_menu_new();
    GMenu *section = g_menu_new();
    for (const GtkMenuEntry &entry : entries) {
        if (entry.type == MENUITEMTYPE_SEPARATOR) {
            g_menu_append_section(model, nullptr, G_MENU_MODEL(section));
            g_object_unref(section);
            section = g_menu_new();
            continue;
        }
        if (entry.type == MENUITEMTYPE_SUBMENU) {
            GMenu *submenu = createGtkMenuModel(entry.children, menu_state, actions, action_index);
            g_menu_append_submenu(section, entry.label.c_str(), G_MENU_MODEL(submenu));
            g_object_unref(submenu);
            continue;
        }
        const std::string name = "command" + std::to_string(action_index++);
        GSimpleAction *action = nullptr;
        if (entry.type == MENUITEMTYPE_RADIO) {
            action = g_simple_action_new_stateful(name.c_str(), G_VARIANT_TYPE_STRING,
                g_variant_new_string(entry.checked ? "selected" : ""));
        } else if (entry.type == MENUITEMTYPE_CHECK) {
            action = g_simple_action_new_stateful(name.c_str(), nullptr,
                                                  g_variant_new_boolean(entry.checked));
        } else {
            action = g_simple_action_new(name.c_str(), nullptr);
        }
        g_simple_action_set_enabled(action, entry.enabled);
        if (entry.command_id >= 0) {
            auto *payload = new std::shared_ptr<GtkContextMenuAction>(
                std::make_shared<GtkContextMenuAction>(
                    GtkContextMenuAction{menu_state, entry.command_id}));
            g_signal_connect_data(
                action,
                "activate",
                G_CALLBACK(onGtkMenuItemActivated),
                payload,
                [](gpointer data, GClosure *) {
                    delete static_cast<std::shared_ptr<GtkContextMenuAction> *>(data);
                },
                G_CONNECT_DEFAULT);
        }
        g_action_map_add_action(G_ACTION_MAP(actions), G_ACTION(action));
        g_object_unref(action);
        const std::string detailed_action = "context." + name;
        GMenuItem *item = g_menu_item_new(entry.label.c_str(), nullptr);
        g_menu_item_set_action_and_target_value(item, detailed_action.c_str(),
            entry.type == MENUITEMTYPE_RADIO ? g_variant_new_string("selected") : nullptr);
        g_menu_append_item(section, item);
        g_object_unref(item);
    }
    g_menu_append_section(model, nullptr, G_MENU_MODEL(section));
    g_object_unref(section);
    return model;
}

gboolean showGtkContextMenu(gpointer data) {
    auto *menu_ref = static_cast<std::shared_ptr<GtkContextMenuState> *>(data);
    const auto &state = *menu_ref;
    if (state->completed) return G_SOURCE_REMOVE;
    if (g_getenv("COMPOSE_CEF_DEBUG_MENU")) {
        g_printerr("CEF context menu: %zu items, widget=%p, at %d,%d\n",
                   state->entries.size(), state->parent_widget, state->x, state->y);
    }
    if (state->entries.empty() || state->parent_widget == nullptr ||
        !gtk_widget_get_realized(state->parent_widget)) {
        completeContextMenu(state, -1);
        return G_SOURCE_REMOVE;
    }

    // Unlike GtkMenu's separate popup surface and seat grab, a GtkPopover
    // stays inside the host GTK window. Window activation is not transferred
    // to another surface; GTK owns the temporary widget focus and restores it.
    GtkWidget *top = gtk_widget_get_toplevel(state->parent_widget);
    if (GTK_IS_WINDOW(top)) {
        state->host_widget = GTK_WIDGET(g_object_ref(top));
        // GTK's WM-event precheck emits input signals on GtkWindow BEFORE
        // routing to the modal grab. Tao consumes those signals, preventing
        // the popover's buttons and outside-click dismissal from seeing them.
        // While this native menu is modal, let GTK route input to its grab
        // instead of forwarding it to the Compose scene behind the menu.
        const char *signals[] = {
            "button-press-event", "button-release-event", "motion-notify-event",
            "scroll-event", "key-press-event", "key-release-event"
        };
        for (const char *signal : signals) {
            const guint id = g_signal_lookup(signal, GTK_TYPE_WIDGET);
            gulong handler = 0;
            while ((handler = g_signal_handler_find(top,
                       static_cast<GSignalMatchType>(G_SIGNAL_MATCH_ID | G_SIGNAL_MATCH_UNBLOCKED),
                       id, 0, nullptr, nullptr, nullptr)) != 0) {
                state->host_input_handlers.push_back(handler);
                g_signal_handler_block(top, handler);
            }
        }
    }
    GSimpleActionGroup *actions = g_simple_action_group_new();
    int action_index = 0;
    GMenu *model = createGtkMenuModel(state->entries, state, actions, action_index);
    GtkWidget *popover = gtk_popover_new_from_model(state->parent_widget, G_MENU_MODEL(model));
    gtk_widget_insert_action_group(popover, "context", G_ACTION_GROUP(actions));
    g_object_unref(model);
    g_object_unref(actions);
    bool owner_closing = false;
    {
        std::lock_guard<std::mutex> lock(state->owner->mutex);
        owner_closing = state->owner->closing;
        state->menu_widget = popover;
        if (!owner_closing) {
            state->owner->active_menu_widget = popover;
        }
    }
    if (owner_closing) {
        completeContextMenu(state, -1);
        return G_SOURCE_REMOVE;
    }

    // "closed" fires on outside-click dismissal, Escape, and after an item's
    // action handler completes the menu. The CAS in
    // completeContextMenu makes double-completion harmless.
    auto *closed_payload = new std::shared_ptr<GtkContextMenuState>(state);
    g_signal_connect_data(
        popover,
        "closed",
        G_CALLBACK(+[](GtkWidget *, gpointer context) {
            auto *menu_state = static_cast<std::shared_ptr<GtkContextMenuState> *>(context);
            if (g_getenv("COMPOSE_CEF_DEBUG_MENU")) g_printerr("CEF context menu closed\n");
            // GTK's model button can emit "closed" before activating its
            // GAction. Allow that action to complete first; only treat this
            // as cancellation if no action was selected in this event turn.
            auto *payload = new std::shared_ptr<GtkContextMenuState>(*menu_state);
            g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer data) -> gboolean {
                completeContextMenu(*static_cast<std::shared_ptr<GtkContextMenuState> *>(data), -1);
                return G_SOURCE_REMOVE;
            }, payload, [](gpointer data) {
                delete static_cast<std::shared_ptr<GtkContextMenuState> *>(data);
            });
        }),
        closed_payload,
        [](gpointer value, GClosure *) {
            delete static_cast<std::shared_ptr<GtkContextMenuState> *>(value);
        },
        G_CONNECT_DEFAULT);

    // CefContextMenuParams coordinates are local to the view, and the popover
    // is relative to the view — direct mapping, no translation needed.
    GdkRectangle anchor{state->x, state->y, 1, 1};
    gtk_popover_set_pointing_to(GTK_POPOVER(popover), &anchor);
    gtk_popover_set_position(GTK_POPOVER(popover), GTK_POS_BOTTOM);
    // No popup animation: Nucleus drives its own render loop, not GTK's.
    // Showing directly avoids relying on frame-clock transition completion
    // for the popover's input region or dismissal.
    gtk_widget_show(popover);
    // A modal popover holds a GTK grab while visible; if the grab failed
    // (e.g. another grab is active) no "closed" signal will come either.
    if (gtk_grab_get_current() != popover) {
        if (g_getenv("COMPOSE_CEF_DEBUG_MENU")) {
            g_printerr("CEF context menu: GTK grab failed, cancelling\n");
        }
        completeContextMenu(state, -1);
    }
    return G_SOURCE_REMOVE;
}

}  // namespace

void compose_cef_prepare_context_menu(
    CefRefPtr<CefContextMenuParams> params,
    CefRefPtr<CefMenuModel> model) {
    if (g_getenv("COMPOSE_CEF_DEBUG_MENU")) {
        g_printerr("CEF OnBeforeContextMenu: %zu items, editable=%d, link=%d, media=%d\n",
                   model->GetCount(), params->IsEditable(),
                   !params->GetUnfilteredLinkUrl().empty(), params->GetMediaType());
    }
    if (!copyMenuModel(model).empty()) return;

    // The Chrome runtime can return an empty/default-only model for OSR.
    // Supply Chromium command IDs so the normal CEF callback still performs
    // editing, link and image actions instead of displaying an empty menu.
    model->Clear();
    const auto flags = params->GetEditStateFlags();
    if (params->IsEditable()) {
        const auto addEdit = [model, flags](int id, const char *label, int flag) {
            model->AddItem(id, label);
            model->SetEnabled(id, (flags & flag) != 0);
        };
        addEdit(MENU_ID_UNDO, "Undo", CM_EDITFLAG_CAN_UNDO);
        addEdit(MENU_ID_REDO, "Redo", CM_EDITFLAG_CAN_REDO);
        model->AddSeparator();
        addEdit(MENU_ID_CUT, "Cut", CM_EDITFLAG_CAN_CUT);
        addEdit(MENU_ID_COPY, "Copy", CM_EDITFLAG_CAN_COPY);
        addEdit(MENU_ID_PASTE, "Paste", CM_EDITFLAG_CAN_PASTE);
        addEdit(MENU_ID_DELETE, "Delete", CM_EDITFLAG_CAN_DELETE);
        model->AddSeparator();
        addEdit(MENU_ID_SELECT_ALL, "Select All", CM_EDITFLAG_CAN_SELECT_ALL);
    } else if ((params->GetTypeFlags() & CM_TYPEFLAG_SELECTION) != 0) {
        model->AddItem(MENU_ID_COPY, "Copy");
    }
    if (!params->GetUnfilteredLinkUrl().empty()) {
        if (model->GetCount() != 0) model->AddSeparator();
        model->AddItem(kCopyLinkAddress, "Copy link address");
    }
    if (params->GetMediaType() == CM_MEDIATYPE_IMAGE) {
        if (model->GetCount() != 0) model->AddSeparator();
        model->AddItem(kCopyImageAddress, "Copy image address");
        model->AddItem(kCopyImage, "Copy image");
        model->SetEnabled(kCopyImage, params->HasImageContents());
    }
    if (model->GetCount() == 0) {
        model->AddItem(MENU_ID_BACK, "Back");
        model->AddItem(MENU_ID_FORWARD, "Forward");
        model->AddItem(MENU_ID_RELOAD, "Reload");
    }
}

bool compose_cef_handle_context_menu_command(
    const std::shared_ptr<ComposeCefViewState> &state,
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefFrame> frame,
    CefRefPtr<CefContextMenuParams> params,
    int command_id) {
    if (params == nullptr) return false;
    return compose_cef_execute_context_menu_command(
        state, browser, frame, params->GetUnfilteredLinkUrl().ToString(),
        params->GetSourceUrl().ToString(), command_id);
}

bool compose_cef_execute_context_menu_command(
    const std::shared_ptr<ComposeCefViewState> &state,
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefFrame> frame,
    const std::string &link_url,
    const std::string &source_url,
    int command_id) {
    if (g_getenv("COMPOSE_CEF_DEBUG_MENU")) {
        g_printerr("CEF context menu command: %d\n", command_id);
    }
    if (browser == nullptr) return false;
    if (frame == nullptr) frame = browser->GetMainFrame();
    if (frame == nullptr) return false;
    switch (command_id) {
        case MENU_ID_UNDO: case IDC_CONTENT_CONTEXT_UNDO: frame->Undo(); return true;
        case MENU_ID_REDO: case IDC_CONTENT_CONTEXT_REDO: frame->Redo(); return true;
        case MENU_ID_CUT: case IDC_CONTENT_CONTEXT_CUT:
            compose_cef_copy_selection(state, browser, frame, true); return true;
        case MENU_ID_COPY: case IDC_CONTENT_CONTEXT_COPY:
            compose_cef_copy_selection(state, browser, frame, false); return true;
        case MENU_ID_PASTE: case IDC_CONTENT_CONTEXT_PASTE:
            compose_cef_paste_system_clipboard(browser); return true;
        case MENU_ID_PASTE_MATCH_STYLE:
        case IDC_CONTENT_CONTEXT_PASTE_AND_MATCH_STYLE:
            compose_cef_paste_system_clipboard(browser); return true;
        case MENU_ID_DELETE: case IDC_CONTENT_CONTEXT_DELETE: frame->Delete(); return true;
        case MENU_ID_SELECT_ALL: case IDC_CONTENT_CONTEXT_SELECTALL: frame->SelectAll(); return true;
        case MENU_ID_BACK: case IDC_BACK: browser->GoBack(); return true;
        case MENU_ID_FORWARD: case IDC_FORWARD: browser->GoForward(); return true;
        case MENU_ID_RELOAD: case IDC_RELOAD: browser->Reload(); return true;
        case IDC_CONTENT_CONTEXT_OPENLINKNEWTAB:
            browser->GetMainFrame()->LoadURL(CefString(link_url));
            return true;
        case kCopyLinkAddress:
        case IDC_CONTENT_CONTEXT_COPYLINKLOCATION:
            copyTextToClipboard(link_url);
            return true;
        case kCopyImageAddress:
        case IDC_CONTENT_CONTEXT_COPYIMAGELOCATION:
            copyTextToClipboard(source_url);
            return true;
        case kCopyImage:
        case IDC_CONTENT_CONTEXT_COPYIMAGE:
            browser->GetHost()->DownloadImage(CefString(source_url), false, 0, false,
                                               new CopyImageCallback());
            return true;
        default: return false;
    }
}

bool compose_cef_run_context_menu(
    const std::shared_ptr<ComposeCefViewState> &state,
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefFrame> frame,
    CefRefPtr<CefMenuModel> model,
    CefRefPtr<CefRunContextMenuCallback> callback,
    CefRefPtr<CefContextMenuParams> params) {
    if (g_getenv("COMPOSE_CEF_DEBUG_MENU")) {
        g_printerr("CEF RunContextMenu: %zu model items\n", model->GetCount());
    }
    GtkWidget *widget = nullptr;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        widget = state->widget;
        if (widget != nullptr) g_object_ref(widget);
    }
    auto menu = std::make_shared<GtkContextMenuState>();
    menu->callback = callback;
    menu->browser = browser;
    menu->frame = frame;
    menu->owner = state;
    menu->link_url = params->GetUnfilteredLinkUrl().ToString();
    menu->source_url = params->GetSourceUrl().ToString();
    menu->entries = copyMenuModel(model);
    menu->parent_widget = widget;
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
