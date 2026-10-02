#include "cef_host_internal.h"
using namespace cefipc;

namespace {
struct Menu {
    std::shared_ptr<ComposeCefViewState> owner;
    ComposeCefBrowserRef browser;
    uint64_t token;
    GtkWidget *widget = nullptr, *host = nullptr;
    std::vector<gulong> handlers;
    std::atomic<bool> completed{false};
};
void sendChoice(const std::shared_ptr<Menu> &menu, int command, const std::string &paste = {}) {
    Writer w; w.value(menu->token); w.integer(command); w.text(paste);
    menu->browser->send(Op::MenuDone, w);
}
void complete(const std::shared_ptr<Menu> &menu, int command) {
    if (menu->completed.exchange(true)) return;
    {
        std::lock_guard lock(menu->owner->mutex);
        if (menu->owner->active_menu_widget == menu->widget) menu->owner->active_menu_widget = nullptr;
    }
    if (menu->owner->right_button_pending.exchange(false)) {
        auto mouse = menu->owner->last_context_mouse;
        mouse.modifiers &= ~EVENTFLAG_RIGHT_MOUSE_BUTTON;
        menu->browser->SendMouseClickEvent(mouse, MBT_RIGHT, true, 1);
    }
    if (command == MENU_ID_PASTE || command == MENU_ID_PASTE_MATCH_STYLE ||
        command == IDC_CONTENT_CONTEXT_PASTE || command == IDC_CONTENT_CONTEXT_PASTE_AND_MATCH_STYLE) {
        auto *p = new std::shared_ptr<Menu>(menu);
        gtk_clipboard_request_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD),
            [](GtkClipboard *, const gchar *text, gpointer p) {
                auto menu = *static_cast<std::shared_ptr<Menu> *>(p);
                sendChoice(menu, MENU_ID_PASTE, text ? text : "");
                delete static_cast<std::shared_ptr<Menu> *>(p);
            }, p);
    } else sendChoice(menu, command);
    auto *p = new std::shared_ptr<Menu>(menu);
    g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer p) -> gboolean {
        auto menu = *static_cast<std::shared_ptr<Menu> *>(p);
        if (!gtk_widget_in_destruction(menu->widget)) {
            gtk_widget_hide(menu->widget); gtk_widget_destroy(menu->widget);
        }
        for (auto h : menu->handlers) if (g_signal_handler_is_connected(menu->host, h)) g_signal_handler_unblock(menu->host, h);
        compose_cef_finish_context_input(menu->owner);
        if (menu->owner->widget) menu->browser->SetFocus(gtk_widget_has_focus(menu->owner->widget));
        return G_SOURCE_REMOVE;
    }, p, [](gpointer p) {
        auto menu = *static_cast<std::shared_ptr<Menu> *>(p);
        g_object_unref(menu->widget); g_object_unref(menu->host);
        delete static_cast<std::shared_ptr<Menu> *>(p);
    });
}
struct Action { std::shared_ptr<Menu> menu; int command; };
GMenu *model(const std::vector<MenuEntry> &entries, GSimpleActionGroup *actions,
             const std::shared_ptr<Menu> &menu, int &index) {
    GMenu *root = g_menu_new(), *section = g_menu_new();
    for (const auto &entry : entries) {
        if (entry.type == MENUITEMTYPE_SEPARATOR) {
            g_menu_append_section(root, nullptr, G_MENU_MODEL(section)); g_object_unref(section); section = g_menu_new(); continue;
        }
        if (entry.type == MENUITEMTYPE_SUBMENU) {
            auto *child = model(entry.children, actions, menu, index);
            g_menu_append_submenu(section, entry.label.c_str(), G_MENU_MODEL(child)); g_object_unref(child); continue;
        }
        auto name = "command" + std::to_string(index++);
        auto *action = entry.type == MENUITEMTYPE_CHECK
            ? g_simple_action_new_stateful(name.c_str(), nullptr, g_variant_new_boolean(entry.checked))
            : entry.type == MENUITEMTYPE_RADIO
            ? g_simple_action_new_stateful(name.c_str(), G_VARIANT_TYPE_STRING, g_variant_new_string(entry.checked ? "selected" : ""))
            : g_simple_action_new(name.c_str(), nullptr);
        g_simple_action_set_enabled(action, entry.enabled);
        g_signal_connect_data(action, "activate", G_CALLBACK(+[](GSimpleAction *, GVariant *, gpointer p) {
            auto *a = static_cast<Action *>(p); complete(a->menu, a->command);
        }), new Action{menu, entry.command}, [](gpointer p, GClosure *) { delete static_cast<Action *>(p); }, G_CONNECT_DEFAULT);
        g_action_map_add_action(G_ACTION_MAP(actions), G_ACTION(action)); g_object_unref(action);
        auto detailed = "context." + name;
        auto *item = g_menu_item_new(entry.label.c_str(), nullptr);
        g_menu_item_set_action_and_target_value(item, detailed.c_str(),
            entry.type == MENUITEMTYPE_RADIO ? g_variant_new_string("selected") : nullptr);
        g_menu_append_item(section, item); g_object_unref(item);
    }
    g_menu_append_section(root, nullptr, G_MENU_MODEL(section)); g_object_unref(section); return root;
}
struct Request { std::shared_ptr<ComposeCefViewState> state; uint64_t token; int x, y; std::vector<MenuEntry> entries; };
}
void compose_cef_show_context_menu(const std::shared_ptr<ComposeCefViewState> &state, uint64_t token,
                                  int x, int y, std::vector<MenuEntry> entries) {
    auto *p = new Request{state, token, x, y, std::move(entries)};
    g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer p) -> gboolean {
        auto &r = *static_cast<Request *>(p);
        GtkWidget *view;
        { std::lock_guard lock(r.state->mutex); view = r.state->closing ? nullptr : r.state->widget; }
        if (!view) { Writer w; w.value(r.token); w.integer(-1); w.text(""); if (r.state->browser) r.state->browser->send(Op::MenuDone, w); return G_SOURCE_REMOVE; }
        auto menu = std::make_shared<Menu>(); menu->owner = r.state; menu->browser = r.state->browser; menu->token = r.token;
        menu->host = GTK_WIDGET(g_object_ref(gtk_widget_get_toplevel(view)));
        auto *actions = g_simple_action_group_new(); int index = 0;
        auto *items = model(r.entries, actions, menu, index);
        menu->widget = gtk_popover_new_from_model(view, G_MENU_MODEL(items)); g_object_ref(menu->widget);
        gtk_widget_insert_action_group(menu->widget, "context", G_ACTION_GROUP(actions));
        g_object_unref(items); g_object_unref(actions);
        const char *signals[] = {"button-press-event", "button-release-event", "motion-notify-event", "scroll-event", "key-press-event", "key-release-event"};
        for (const char *s : signals) {
            auto id = g_signal_lookup(s, GTK_TYPE_WIDGET); gulong h;
            while ((h = g_signal_handler_find(menu->host, static_cast<GSignalMatchType>(G_SIGNAL_MATCH_ID | G_SIGNAL_MATCH_UNBLOCKED), id, 0, nullptr, nullptr, nullptr))) {
                menu->handlers.push_back(h); g_signal_handler_block(menu->host, h);
            }
        }
        { std::lock_guard lock(r.state->mutex); r.state->active_menu_widget = menu->widget; }
        g_signal_connect_data(menu->widget, "closed", G_CALLBACK(+[](GtkWidget *, gpointer p) {
            auto *ref = new std::shared_ptr<Menu>(*static_cast<std::shared_ptr<Menu> *>(p));
            g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer p) -> gboolean {
                complete(*static_cast<std::shared_ptr<Menu> *>(p), -1); return G_SOURCE_REMOVE;
            }, ref, [](gpointer p) { delete static_cast<std::shared_ptr<Menu> *>(p); });
        }), new std::shared_ptr<Menu>(menu), [](gpointer p, GClosure *) { delete static_cast<std::shared_ptr<Menu> *>(p); }, G_CONNECT_DEFAULT);
        GdkRectangle rect{r.x, r.y, 1, 1};
        gtk_popover_set_pointing_to(GTK_POPOVER(menu->widget), &rect);
        gtk_popover_set_position(GTK_POPOVER(menu->widget), GTK_POS_BOTTOM);
        gtk_widget_show(menu->widget);
        return G_SOURCE_REMOVE;
    }, p, [](gpointer p) { delete static_cast<Request *>(p); });
}
