#include "compose_cef_internal.h"

#include "include/cef_image.h"
#include "include/wrapper/cef_helpers.h"

/* Native GTK context menu built from CEF's menu model, filtered to the
 * Chromium essentials. Selecting an item runs its browser/frame action on
 * CEF's UI thread. The right-button release that follows the press is
 * swallowed in view_input.cpp so the menu survives. */

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
        if (!closing && command_id >= 0) {
            compose_cef_execute_context_menu_command(
                menu->owner, menu->browser, menu->frame,
                menu->link_url, menu->source_url, command_id);
        }
    });
    if (widget != nullptr) {
        // GtkMenu retains an input grab until it is destroyed. Defer that
        // destruction until the current activate/deactivate signal returns.
        g_object_ref(widget);
        g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer data) -> gboolean {
            auto *menu_widget = GTK_WIDGET(data);
            if (!gtk_widget_in_destruction(menu_widget)) {
                gtk_menu_popdown(GTK_MENU(menu_widget));
                gtk_widget_destroy(menu_widget);
            }
            g_object_unref(menu_widget);
            return G_SOURCE_REMOVE;
        }, widget, nullptr);
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
    if (g_getenv("COMPOSE_CEF_DEBUG_MENU")) {
        g_printerr("CEF context menu: %zu items, widget=%p, at %d,%d\n",
                   state->entries.size(), state->parent_widget, state->x, state->y);
    }
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
            if (g_getenv("COMPOSE_CEF_DEBUG_MENU")) g_printerr("CEF context menu deactivated\n");
            completeContextMenu(*menu_state, -1);
        }),
        deactivate_payload,
        [](gpointer value, GClosure *) {
            delete static_cast<std::shared_ptr<GtkContextMenuState> *>(value);
        },
        G_CONNECT_DEFAULT);
    gtk_menu_attach_to_widget(GTK_MENU(gtk_menu), state->parent_widget, nullptr);
    gtk_widget_show_all(gtk_menu);
    // CefContextMenuParams coordinates are local to the view; the widget's
    // GdkWindow is also local to the view. Adding its parent allocation here
    // moves the menu away from the click (and often off-screen).
    GdkRectangle anchor{state->x, state->y, 1, 1};
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
    menu->browser = browser;
    menu->frame = frame;
    menu->owner = state;
    menu->link_url = params->GetUnfilteredLinkUrl().ToString();
    menu->source_url = params->GetSourceUrl().ToString();
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
