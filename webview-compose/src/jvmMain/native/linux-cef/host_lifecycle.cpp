#include "cef_host_internal.h"
#include <unordered_map>
#include <cstdio>

namespace {
std::mutex handles_mutex;
std::map<jlong, std::shared_ptr<ComposeCefViewState>> handles;
std::atomic<jlong> next_handle{1};
gboolean draw(GtkWidget *, cairo_t *cr, gpointer data) {
    auto state = *static_cast<std::shared_ptr<ComposeCefViewState> *>(data);
    std::lock_guard lock(state->mutex);
    if (state->closing || !state->widget) return FALSE;
    if (state->pixels.empty()) { cairo_set_source_rgb(cr, 1, 1, 1); cairo_paint(cr); return FALSE; }
    auto *surface = cairo_image_surface_create_for_data(state->pixels.data(), CAIRO_FORMAT_ARGB32,
        state->frame_width, state->frame_height, state->frame_width * 4);
    GtkAllocation allocation; gtk_widget_get_allocation(state->widget, &allocation);
    cairo_save(cr);
    cairo_scale(cr, double(allocation.width) / state->frame_width, double(allocation.height) / state->frame_height);
    cairo_set_source_surface(cr, surface, 0, 0); cairo_paint(cr); cairo_restore(cr); cairo_surface_destroy(surface);
    return FALSE;
}
void resize(GtkWidget *, GtkAllocation *a, gpointer data) {
    auto state = *static_cast<std::shared_ptr<ComposeCefViewState> *>(data);
    if (state->browser && !state->closing) {
        cefipc::Writer w; w.integer(a->width); w.integer(a->height); state->browser->send(cefipc::Op::Resize, w);
    }
}
void release(const std::shared_ptr<ComposeCefViewState> &state) {
    ComposeCefBrowserRef browser; GtkWidget *widget; GtkWidget *menu;
    {
        std::lock_guard lock(state->mutex); state->closing = true;
        browser = state->browser; widget = state->widget; state->widget = nullptr;
        menu = state->active_menu_widget; state->active_menu_widget = nullptr;
    }
    if (menu) gtk_widget_destroy(menu);
    compose_cef_release_host(browser); // Includes CefShutdown + waitpid for the last view.
    { std::lock_guard lock(state->mutex); state->browser.reset(); }
    if (widget) {
        compose_cef_disconnect_input(state); gtk_widget_destroy(widget);
        g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer) -> gboolean { return G_SOURCE_REMOVE; },
                        widget, [](gpointer p) { g_object_unref(p); });
    }
}
}

std::shared_ptr<ComposeCefViewState> compose_cef_shared_from_handle(jlong id) {
    std::lock_guard lock(handles_mutex); auto i = handles.find(id);
    return i == handles.end() ? nullptr : i->second;
}
std::string compose_cef_jstring_to_utf8(JNIEnv *env, jstring s) {
    if (!s) return {};
    const char *p = env->GetStringUTFChars(s, nullptr);
    if (!p) return {};
    std::string result(p); env->ReleaseStringUTFChars(s, p); return result;
}
jstring compose_cef_utf8_to_jstring(JNIEnv *env, const std::string &s) { return env->NewStringUTF(s.c_str()); }
std::string compose_cef_json_escape(const std::string &s) {
    std::string out;
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break; case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break; case '\r': out += "\\r"; break; case '\t': out += "\\t"; break;
            default: if (c < 32) { char escaped[7]; snprintf(escaped, sizeof escaped, "\\u%04x", c); out += escaped; } else out += c;
        }
    }
    return out;
}

extern "C" {
JNIEXPORT jlong JNICALL Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeCreate(
    JNIEnv *env, jclass, jstring runtime, jstring cache, jstring ua, jstring init, jstring bridge,
    jboolean incognito, jboolean tools, jboolean js, jdouble zoom, jboolean transparent,
    jfloat r, jfloat g, jfloat b, jfloat a, jstring url) {
    compose_cef_ensure_bridge_methods(env);
    ComposeCefCreateOptions o;
    o.dataDirectory = compose_cef_jstring_to_utf8(env, cache); o.userAgent = compose_cef_jstring_to_utf8(env, ua);
    o.initScript = compose_cef_jstring_to_utf8(env, init); o.jsBridgeScript = compose_cef_jstring_to_utf8(env, bridge);
    o.initialUrl = compose_cef_jstring_to_utf8(env, url); o.incognito = incognito; o.enableDevtools = tools;
    o.javascriptEnabled = js; o.zoomLevel = zoom; o.transparent = transparent;
    o.bgR = r; o.bgG = g; o.bgB = b; o.bgA = a;
    auto state = std::make_shared<ComposeCefViewState>(); state->handle = next_handle.fetch_add(1);
    auto *widget = gtk_drawing_area_new(); g_object_ref_sink(widget); state->widget = widget;
    gtk_widget_set_can_focus(widget, TRUE); gtk_widget_set_hexpand(widget, TRUE); gtk_widget_set_vexpand(widget, TRUE);
    gtk_widget_add_events(widget, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK |
        GDK_SCROLL_MASK | GDK_KEY_PRESS_MASK | GDK_KEY_RELEASE_MASK | GDK_FOCUS_CHANGE_MASK);
    auto *data = new std::shared_ptr<ComposeCefViewState>(state);
    g_object_set_data_full(G_OBJECT(widget), "compose-cef-view-state", data,
                          [](gpointer p) { delete static_cast<std::shared_ptr<ComposeCefViewState> *>(p); });
    g_signal_connect(widget, "draw", G_CALLBACK(draw), data);
    g_signal_connect(widget, "size-allocate", G_CALLBACK(resize), data);
    compose_cef_connect_input(widget, state);
    { std::lock_guard lock(handles_mutex); handles[state->handle] = state; }
    auto browser = compose_cef_start_host(state, compose_cef_jstring_to_utf8(env, runtime), o);
    { std::lock_guard lock(state->mutex); state->browser = std::move(browser); }
    if (!state->browser) {
        { std::lock_guard lock(handles_mutex); handles.erase(state->handle); }
        release(state); return 0;
    }
    gtk_widget_show(widget); return state->handle;
}
JNIEXPORT jlong JNICALL Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeGetGtkWidget(
    JNIEnv *, jclass, jlong id) {
    auto state = compose_cef_shared_from_handle(id);
    return state ? reinterpret_cast<jlong>(state->widget) : 0;
}
JNIEXPORT void JNICALL Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeRelease(
    JNIEnv *, jclass, jlong id) {
    auto state = compose_cef_shared_from_handle(id); if (!state) return;
    { std::lock_guard lock(handles_mutex); handles.erase(id); }
    release(state);
}
JNIEXPORT void JNICALL Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeResize(
    JNIEnv *, jclass, jlong id, jint width, jint height) {
    auto state = compose_cef_shared_from_handle(id); if (!state || !state->browser) return;
    cefipc::Writer w; w.integer(width); w.integer(height); state->browser->send(cefipc::Op::Resize, w);
}
}
