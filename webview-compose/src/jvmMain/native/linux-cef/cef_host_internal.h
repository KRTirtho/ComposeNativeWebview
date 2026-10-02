#pragma once

#include <jni.h>
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "include/cef_browser.h"
#include "include/cef_command_ids.h"
#include "host_protocol.h"

struct ComposeCefViewState;
class CefHostProcess;
struct ComposeCefCreateOptions {
    std::string userAgent, dataDirectory, initScript, jsBridgeScript, initialUrl;
    bool incognito = false, enableDevtools = false, javascriptEnabled = true, transparent = false;
    double zoomLevel = 1.0;
    float bgR = 1, bgG = 1, bgB = 1, bgA = 1;
};

// A transport facade, NOT a CefBrowser. The JVM library never initializes or
// links libcef; all Chromium objects and their UI thread belong to the worker.
class CefRemoteBrowser : public std::enable_shared_from_this<CefRemoteBrowser> {
public:
    CefRemoteBrowser(std::shared_ptr<CefHostProcess> process, uint64_t id);
    void send(cefipc::Op op, const cefipc::Writer &w = {});
    CefRemoteBrowser *GetHost() { return this; }
    std::shared_ptr<CefRemoteBrowser> GetFocusedFrame() { return shared_from_this(); }
    std::shared_ptr<CefRemoteBrowser> GetMainFrame() { return shared_from_this(); }
    std::string GetURL() { return {}; }
    void ExecuteJavaScript(const std::string &script, const std::string &, int);
    void SetFocus(bool focus);
    void SendMouseMoveEvent(const CefMouseEvent &mouse, bool leave);
    void SendMouseClickEvent(const CefMouseEvent &mouse, CefBrowserHost::MouseButtonType button, bool up, int count);
    void SendMouseWheelEvent(const CefMouseEvent &mouse, int x, int y);
    void SendKeyEvent(const CefKeyEvent &key);
    void Undo(); void Redo(); void SelectAll();
    void close();
private:
    std::shared_ptr<CefHostProcess> process_;
    uint64_t id_;
};
using ComposeCefBrowserRef = std::shared_ptr<CefRemoteBrowser>;
using ComposeCefFrameRef = ComposeCefBrowserRef;

struct ComposeCefViewState : public std::enable_shared_from_this<ComposeCefViewState> {
    std::mutex mutex;
    std::condition_variable closed_condition;
    jlong handle = 0;
    GtkWidget *widget = nullptr;
    GtkGesture *outside_press_gesture = nullptr;
    ComposeCefBrowserRef browser;
    std::vector<uint8_t> pixels;
    int frame_width = 0, frame_height = 0, width = 800, height = 600;
    std::atomic<bool> gtk_draw_pending{false};
    bool closing = false, closed = false;
    std::string current_url, title;
    std::atomic<bool> is_loading{false}, can_go_back{false}, can_go_forward{false};
    GtkWidget *active_menu_widget = nullptr;
    GdkEvent *context_press_event = nullptr;
    GtkWidget *context_input_widget = nullptr;
    gulong context_input_handler = 0;
    bool context_release_pending = false;
    CefMouseEvent last_context_mouse;
    std::atomic<bool> right_button_pending{false};
};

JNIEnv *compose_cef_get_env();
void compose_cef_ensure_bridge_methods(JNIEnv *env);
bool compose_cef_call_on_navigate(jlong, const std::string &);
void compose_cef_call_on_ipc(jlong, const std::string &);
void compose_cef_call_on_js_result(jlong, const std::string &);
void compose_cef_call_on_cookies(jlong, const std::string &);
void compose_cef_call_on_screenshot(jlong, const std::vector<uint8_t> &);
void compose_cef_call_on_pointer_focus(jlong);
std::string compose_cef_jstring_to_utf8(JNIEnv *, jstring);
jstring compose_cef_utf8_to_jstring(JNIEnv *, const std::string &);
std::string compose_cef_json_escape(const std::string &);
std::shared_ptr<ComposeCefViewState> compose_cef_shared_from_handle(jlong);
void compose_cef_post_to_ui(std::function<void()> fn);
void compose_cef_on_paint(const std::shared_ptr<ComposeCefViewState> &, const void *, int, int);
void compose_cef_queue_draw(const std::shared_ptr<ComposeCefViewState> &);
void compose_cef_connect_input(GtkWidget *, const std::shared_ptr<ComposeCefViewState> &);
void compose_cef_disconnect_input(const std::shared_ptr<ComposeCefViewState> &);
void compose_cef_finish_context_input(const std::shared_ptr<ComposeCefViewState> &);
void compose_cef_copy_selection(const std::shared_ptr<ComposeCefViewState> &, ComposeCefBrowserRef, ComposeCefFrameRef, bool);
void compose_cef_paste_system_clipboard(ComposeCefBrowserRef);
void compose_cef_show_context_menu(const std::shared_ptr<ComposeCefViewState> &, uint64_t token,
                                  int x, int y, std::vector<cefipc::MenuEntry> entries);
ComposeCefBrowserRef compose_cef_start_host(const std::shared_ptr<ComposeCefViewState> &,
                                         const std::string &runtime, const ComposeCefCreateOptions &);
void compose_cef_release_host(const ComposeCefBrowserRef &);
