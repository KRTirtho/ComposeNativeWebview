// Shared state and helpers for the Linux direct-CEF (Chromium) backend.
//
// Mirrors the Windows WebView2 backend layout (compose_webview_internal.h).
// Not a public API — only used by the compose_cef_*.cpp units.
//
// Pattern: CEF windowless (OSR) rendered into a GTK DrawingArea that is handed
// to Tao's NativeView. CEF runs with multi_threaded_message_loop=true and owns
// its UI thread; all CEF browser ops are posted to TID_UI, all GTK mutations are
// marshalled to the GTK main thread via g_idle_add. Rendering copies the BGRA
// OnPaint buffer into a cairo_image_surface_t (same little-endian layout).
#ifndef COMPOSE_CEF_INTERNAL_H
#define COMPOSE_CEF_INTERNAL_H

#include <jni.h>

#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_command_ids.h"
#include "include/cef_command_line.h"
#include "include/cef_context_menu_handler.h"
#include "include/cef_cookie.h"
#include "include/cef_display_handler.h"
#include "include/cef_life_span_handler.h"
#include "include/cef_load_handler.h"
#include "include/cef_menu_model.h"
#include "include/cef_render_handler.h"
#include "include/cef_resource_request_handler.h"
#include "include/cef_request_handler.h"
#include "include/cef_task.h"
#include "include/wrapper/cef_message_router.h"

struct ComposeCefViewState;

/* cef_app.cpp — process-level CEF lifecycle (multi_threaded_message_loop). */
bool compose_cef_initialize(const std::string &runtime_dir, const std::string &cache_dir);
void compose_cef_shutdown_if_idle();
void compose_cef_note_view_created();
void compose_cef_note_view_released();
void compose_cef_post_to_ui(std::function<void()> fn);

/* jni_bridge.cpp */
JNIEnv *compose_cef_get_env();
void compose_cef_ensure_bridge_methods(JNIEnv *env);
void compose_cef_call_on_navigate_result(jlong handle, bool allow);
bool compose_cef_call_on_navigate(jlong handle, const std::string &url);
void compose_cef_call_on_ipc(jlong handle, const std::string &utf8);
void compose_cef_call_on_js_result(jlong handle, const std::string &utf8);
void compose_cef_call_on_cookies(jlong handle, const std::string &json);
void compose_cef_call_on_screenshot(jlong handle, const std::vector<uint8_t> &png);

/* view_lifecycle.cpp */
std::string compose_cef_jstring_to_utf8(JNIEnv *env, jstring s);
jstring compose_cef_utf8_to_jstring(JNIEnv *env, const std::string &s);
std::string compose_cef_json_escape(const std::string &s);
ComposeCefViewState *compose_cef_state_from_handle(jlong handle);
std::shared_ptr<ComposeCefViewState> compose_cef_shared_from_handle(jlong handle);
jlong compose_cef_register(std::shared_ptr<ComposeCefViewState> s);
std::shared_ptr<ComposeCefViewState> compose_cef_unregister(jlong handle);

struct ComposeCefCreateOptions {
    std::string userAgent;
    std::string dataDirectory;
    std::string initScript;
    std::string jsBridgeScript;
    bool incognito = false;
    bool enableDevtools = false;
    bool javascriptEnabled = true;
    double zoomLevel = 1.0;
    bool transparent = false;
    float bgR = 1.f;
    float bgG = 1.f;
    float bgB = 1.f;
    float bgA = 1.f;
    std::string initialUrl;
};

std::shared_ptr<ComposeCefViewState> compose_cef_create(const ComposeCefCreateOptions &opts);
void compose_cef_release(const std::shared_ptr<ComposeCefViewState> &s);
void compose_cef_resize(const std::shared_ptr<ComposeCefViewState> &s, int widthPx, int heightPx);

/* view_signals.cpp — CEF client (Render/Display/Load/LifeSpan/ContextMenu/Request). */
CefRefPtr<CefClient> compose_cef_create_client(std::shared_ptr<ComposeCefViewState> state);

/* view_render.cpp — OSR OnPaint → cairo surface on the GTK widget. */
void compose_cef_on_paint(
    const std::shared_ptr<ComposeCefViewState> &state,
    const void *buffer,
    int width,
    int height);
void compose_cef_queue_draw(const std::shared_ptr<ComposeCefViewState> &state);

/* view_input.cpp — GTK signal handlers forwarding input to CEF. */
void compose_cef_connect_input(GtkWidget *widget, const std::shared_ptr<ComposeCefViewState> &state);

/* context_menu.cpp — native GTK menu built from CEF's menu model. */
bool compose_cef_run_context_menu(
    const std::shared_ptr<ComposeCefViewState> &state,
    CefRefPtr<CefMenuModel> model,
    CefRefPtr<CefRunContextMenuCallback> callback,
    CefRefPtr<CefContextMenuParams> params);

/* The per-view shared state (allocated as shared_ptr, owned by handle + widget). */
struct ComposeCefViewState : public std::enable_shared_from_this<ComposeCefViewState> {
    std::mutex mutex;
    std::condition_variable closed_condition;
    jlong handle = 0;
    GtkWidget *widget = nullptr;
    CefRefPtr<CefBrowser> browser;

    /* Latest full OSR frame (BGRA premultiplied), drawn by view_render. */
    std::vector<uint8_t> pixels;
    int frame_width = 0;
    int frame_height = 0;

    std::atomic<bool> gtk_draw_pending{false};
    std::atomic<bool> cef_resize_pending{false};

    int width = 0;
    int height = 0;
    bool browser_creation_requested = false;
    bool browser_creation_failed = false;
    int browser_creation_width = 0;
    int browser_creation_height = 0;
    bool closing = false;
    bool closed = false;

    /* Tracked navigation state (written by CEF handlers on the UI thread). */
    std::string current_url;
    std::string title;
    std::atomic<bool> is_loading{false};
    std::atomic<bool> can_go_back{false};
    std::atomic<bool> can_go_forward{false};

    std::string initial_url;
    std::string init_script;
    std::string js_bridge_script;

    /* One-shot headers for LoadURL navigations, consumed on CEF's IO thread. */
    std::map<std::string, CefRequest::HeaderMap> navigation_headers;

    /* Context-menu plumbing (context_menu.cpp). */
    GdkEvent *last_context_event = nullptr;
    GtkWidget *active_menu_widget = nullptr;
};

#endif /* COMPOSE_CEF_INTERNAL_H */
