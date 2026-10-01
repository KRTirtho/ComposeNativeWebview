#include "compose_cef_internal.h"

#include <cstring>

namespace {

constexpr char kClipboardPrefix[] = "__compose_cef_system_clipboard:";

struct PasteRequest {
    CefRefPtr<CefBrowser> browser;
};

void setSystemClipboard(const std::string &text) {
    auto *payload = new std::string(text);
    g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer data) -> gboolean {
        const auto &value = *static_cast<std::string *>(data);
        gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD),
                               value.c_str(), static_cast<gint>(value.size()));
        return G_SOURCE_REMOVE;
    }, payload, [](gpointer data) { delete static_cast<std::string *>(data); });
}

}  // namespace

void compose_cef_copy_selection(const std::shared_ptr<ComposeCefViewState> &state,
                                CefRefPtr<CefBrowser> browser,
                                CefRefPtr<CefFrame> frame, bool cut) {
    if (browser == nullptr) return;
    if (frame == nullptr) frame = browser->GetFocusedFrame();
    if (frame == nullptr) frame = browser->GetMainFrame();
    if (frame == nullptr) return;
    state->clipboard_copies_pending.fetch_add(1);
    // In OSR, CefFrame::Copy() writes Chromium's private clipboard. Nucleus
    // Compose uses GTK's system clipboard. Extract the focused DOM selection
    // before editing and send it through the existing console-message channel.
    const std::string script =
        "(function(){let el=document.activeElement;let text='';"
        "if(el && typeof el.selectionStart==='number' && typeof el.value==='string')"
        "text=el.value.slice(el.selectionStart,el.selectionEnd);"
        "else text=String(window.getSelection()||'');"
        "try{console.log('" + std::string(kClipboardPrefix) +
        "'+btoa(unescape(encodeURIComponent(text))));}catch(e){}" +
        (cut ? "if(text)document.execCommand('delete');" : "") +
        "})();";
    frame->ExecuteJavaScript(script, frame->GetURL(), 0);
}

bool compose_cef_handle_clipboard_console(const std::shared_ptr<ComposeCefViewState> &state,
                                          const std::string &message) {
    if (message.compare(0, sizeof(kClipboardPrefix) - 1, kClipboardPrefix) != 0) return false;
    int pending = state->clipboard_copies_pending.load();
    while (pending > 0 && !state->clipboard_copies_pending.compare_exchange_weak(pending, pending - 1)) {}
    if (pending <= 0) return true;
    const std::string encoded = message.substr(sizeof(kClipboardPrefix) - 1);
    gsize size = 0;
    guchar *bytes = g_base64_decode(encoded.c_str(), &size);
    if (bytes != nullptr) {
        if (g_getenv("COMPOSE_CEF_DEBUG_INPUT")) {
            g_printerr("CEF system clipboard copy: %zu bytes selected\n", size);
        }
        if (size != 0 && g_utf8_validate(reinterpret_cast<const char *>(bytes), size, nullptr)) {
            setSystemClipboard(std::string(reinterpret_cast<const char *>(bytes), size));
        }
        g_free(bytes);
    }
    return true;
}

void compose_cef_paste_system_clipboard(CefRefPtr<CefBrowser> browser) {
    if (browser == nullptr) return;
    auto *request = new PasteRequest{browser};
    // This function is also used by CEF's menu callback (TID_UI). GTK's
    // clipboard must only be accessed on the GTK main thread.
    g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer data) -> gboolean {
        gtk_clipboard_request_text(
            gtk_clipboard_get(GDK_SELECTION_CLIPBOARD),
            [](GtkClipboard *, const gchar *text, gpointer data) {
                auto *request = static_cast<PasteRequest *>(data);
                CefRefPtr<CefBrowser> browser = request->browser;
                if (g_getenv("COMPOSE_CEF_DEBUG_INPUT")) {
                    g_printerr("CEF system clipboard paste: %zu bytes read\n",
                               text == nullptr ? 0 : std::strlen(text));
                }
                if (text != nullptr && *text != '\0') {
                    gchar *utf8 = g_utf8_make_valid(text, -1);
                    gchar *encoded = g_base64_encode(
                        reinterpret_cast<const guchar *>(utf8), std::strlen(utf8));
                    const std::string script =
                        "console.log('__compose_cef_paste_ok:'+"
                        "document.execCommand('insertText',false,"
                        "decodeURIComponent(escape(atob('" + std::string(encoded) + "')))));";
                    g_free(encoded);
                    g_free(utf8);
                    compose_cef_post_to_ui([browser, script] {
                        CefRefPtr<CefFrame> frame = browser->GetFocusedFrame();
                        if (frame == nullptr) frame = browser->GetMainFrame();
                        if (frame != nullptr) frame->ExecuteJavaScript(script, frame->GetURL(), 0);
                    });
                }
                delete request;
            }, data);
        return G_SOURCE_REMOVE;
    }, request, nullptr);
}
