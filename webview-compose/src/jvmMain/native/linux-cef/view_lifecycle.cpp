#include "compose_cef_internal.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <unordered_map>

#include "include/wrapper/cef_helpers.h"

/* ── Handle map ─────────────────────────────────────────────────────────── */

namespace {

std::mutex g_handles_mutex;
std::unordered_map<jlong, std::shared_ptr<ComposeCefViewState>> g_handles;
std::atomic<jlong> g_next_handle{1};

gboolean onDraw(GtkWidget *, cairo_t *cairo, gpointer data) {
    auto state = *static_cast<std::shared_ptr<ComposeCefViewState> *>(data);
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->pixels.empty() || state->width <= 0 || state->height <= 0) {
        cairo_set_source_rgb(cairo, 1.0, 1.0, 1.0);
        cairo_paint(cairo);
        return FALSE;
    }
    cairo_surface_t *surface = cairo_image_surface_create_for_data(
        state->pixels.data(),
        CAIRO_FORMAT_ARGB32,
        state->frame_width,
        state->frame_height,
        state->frame_width * 4);
    cairo_save(cairo);
    cairo_scale(
        cairo,
        static_cast<double>(state->width) / state->frame_width,
        static_cast<double>(state->height) / state->frame_height);
    cairo_set_source_surface(cairo, surface, 0, 0);
    cairo_paint(cairo);
    cairo_restore(cairo);
    cairo_surface_destroy(surface);
    return FALSE;
}

void onSizeAllocate(GtkWidget *, GtkAllocation *allocation, gpointer data) {
    auto state = *static_cast<std::shared_ptr<ComposeCefViewState> *>(data);
    compose_cef_resize(state, allocation->width, allocation->height);
}

void createBrowserForView(const std::shared_ptr<ComposeCefViewState> &state) {
    std::string url;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->closing || state->browser_creation_failed) return;
        url = state->initial_url;
        state->browser_creation_width = state->width;
        state->browser_creation_height = state->height;
    }

    CefRefPtr<CefClient> client = compose_cef_create_client(state);
    CefWindowInfo window_info;
    window_info.SetAsWindowless(0);
    CefBrowserSettings browser_settings;
    browser_settings.windowless_frame_rate = 60;
    browser_settings.background_color = CefColorSetARGB(0xFF, 0xFF, 0xFF, 0xFF);
    CefRefPtr<CefDictionaryValue> extra_info = CefDictionaryValue::Create();
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        extra_info->SetString("bridge_script", state->js_bridge_script);
    }
    const bool created = CefBrowserHost::CreateBrowser(
        window_info,
        client,
        CefString(url.empty() ? "about:blank" : url),
        browser_settings,
        extra_info,
        nullptr);
    if (!created) {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->browser_creation_failed = true;
            state->closed = true;
        }
        state->closed_condition.notify_all();
    }
}

}  // namespace

/* ── Helpers ────────────────────────────────────────────────────────────── */

std::string compose_cef_jstring_to_utf8(JNIEnv *env, jstring s) {
    if (s == nullptr) return {};
    const char *chars = env->GetStringUTFChars(s, nullptr);
    if (chars == nullptr) return {};
    std::string out(chars);
    env->ReleaseStringUTFChars(s, chars);
    return out;
}

jstring compose_cef_utf8_to_jstring(JNIEnv *env, const std::string &s) {
    return env->NewStringUTF(s.c_str());
}

std::string compose_cef_json_escape(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (const unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

ComposeCefViewState *compose_cef_state_from_handle(jlong handle) {
    std::lock_guard<std::mutex> lock(g_handles_mutex);
    auto it = g_handles.find(handle);
    return it == g_handles.end() ? nullptr : it->second.get();
}

std::shared_ptr<ComposeCefViewState> compose_cef_shared_from_handle(jlong handle) {
    std::lock_guard<std::mutex> lock(g_handles_mutex);
    auto it = g_handles.find(handle);
    return it == g_handles.end() ? nullptr : it->second;
}

jlong compose_cef_register(std::shared_ptr<ComposeCefViewState> s) {
    jlong handle = g_next_handle.fetch_add(1, std::memory_order_relaxed);
    s->handle = handle;
    std::lock_guard<std::mutex> lock(g_handles_mutex);
    g_handles[handle] = std::move(s);
    return handle;
}

std::shared_ptr<ComposeCefViewState> compose_cef_unregister(jlong handle) {
    std::lock_guard<std::mutex> lock(g_handles_mutex);
    auto it = g_handles.find(handle);
    if (it == g_handles.end()) return nullptr;
    std::shared_ptr<ComposeCefViewState> s = it->second;
    g_handles.erase(it);
    return s;
}

/* ── Create / release / resize ──────────────────────────────────────────── */

std::shared_ptr<ComposeCefViewState> compose_cef_create(const ComposeCefCreateOptions &opts) {
    const std::string cache_dir =
        opts.dataDirectory.empty() ? "/tmp/composewebview-cef" : opts.dataDirectory;
    // runtime_dir comes from the JNI wrapper (system property / loader).
    extern const char *g_compose_cef_runtime_dir;
    if (g_compose_cef_runtime_dir == nullptr) return nullptr;
    if (!compose_cef_initialize(g_compose_cef_runtime_dir, cache_dir)) return nullptr;

    auto state = std::make_shared<ComposeCefViewState>();
    state->initial_url = opts.initialUrl;
    state->init_script = opts.initScript;
    state->js_bridge_script = opts.jsBridgeScript;

    GtkWidget *widget = gtk_drawing_area_new();
    g_object_ref_sink(widget);
    gtk_widget_set_can_focus(widget, TRUE);
    gtk_widget_set_hexpand(widget, TRUE);
    gtk_widget_set_vexpand(widget, TRUE);
    gtk_widget_add_events(
        widget,
        GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK |
            GDK_SCROLL_MASK | GDK_KEY_PRESS_MASK | GDK_KEY_RELEASE_MASK | GDK_FOCUS_CHANGE_MASK);
    state->widget = widget;
    auto *state_data = new std::shared_ptr<ComposeCefViewState>(state);
    g_object_set_data_full(
        G_OBJECT(widget),
        "compose-cef-view-state",
        state_data,
        [](gpointer data) { delete static_cast<std::shared_ptr<ComposeCefViewState> *>(data); });

    g_signal_connect(widget, "draw", G_CALLBACK(onDraw), state_data);
    g_signal_connect(widget, "size-allocate", G_CALLBACK(onSizeAllocate), state_data);
    compose_cef_connect_input(widget, state);

    gtk_widget_show(widget);
    compose_cef_note_view_created();
    return state;
}

void compose_cef_release(const std::shared_ptr<ComposeCefViewState> &s) {
    if (s == nullptr) return;
    CefRefPtr<CefBrowser> browser;
    GtkWidget *widget = nullptr;
    GtkWidget *active_menu = nullptr;
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        s->closing = true;
        browser = s->browser;
        widget = s->widget;
        s->widget = nullptr;
        active_menu = s->active_menu_widget;
        s->active_menu_widget = nullptr;
    }
    if (active_menu != nullptr) gtk_widget_destroy(active_menu);
    if (browser.get() != nullptr) {
        compose_cef_post_to_ui([browser] { browser->GetHost()->CloseBrowser(true); });
    }
    bool closed = false;
    {
        std::unique_lock<std::mutex> lock(s->mutex);
        // Browser creation is asynchronous. OnAfterCreated closes a browser
        // whose view was released while CreateBrowser was still in flight.
        if (!s->browser_creation_requested) s->closed = true;
        closed = s->closed_condition.wait_for(lock, std::chrono::seconds(10),
                                               [&] { return s->closed; });
    }
    // OnBeforeClose must run before CefShutdown. Do not tear CEF down under a
    // still-live browser if it fails to close within the deadline.
    if (!closed) fprintf(stderr, "CEF browser did not close within 10s; leaving CEF running\n");
    browser = nullptr;
    if (widget != nullptr) {
        compose_cef_disconnect_input(s);
        gtk_widget_destroy(widget);
        g_object_unref(widget);
    }
    if (closed) compose_cef_note_view_released();
}

void compose_cef_resize(
    const std::shared_ptr<ComposeCefViewState> &s,
    int widthPx,
    int heightPx) {
    if (s == nullptr) return;
    widthPx = std::max(1, widthPx);
    heightPx = std::max(1, heightPx);
    CefRefPtr<CefBrowser> browser;
    bool create_browser = false;
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        if (s->closing || (s->width == widthPx && s->height == heightPx)) return;
        s->width = widthPx;
        s->height = heightPx;
        browser = s->browser;
        if (browser.get() == nullptr && !s->browser_creation_requested &&
            widthPx >= 16 && heightPx >= 16) {
            s->browser_creation_requested = true;
            s->browser_creation_width = widthPx;
            s->browser_creation_height = heightPx;
            create_browser = true;
        }
    }
    if (create_browser) {
        createBrowserForView(s);
        return;
    }
    if (browser.get() != nullptr) {
        bool expected = false;
        if (!s->cef_resize_pending.compare_exchange_strong(expected, true)) return;
        compose_cef_post_to_ui([browser, s] {
            browser->GetHost()->WasResized();
            s->cef_resize_pending.store(false);
        });
    }
}

/* runtime dir set once by nativeCreate before compose_cef_create. */
const char *g_compose_cef_runtime_dir = nullptr;

extern "C" {

JNIEXPORT jlong JNICALL
Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeCreate(
    JNIEnv *env,
    jclass,
    jstring runtimeDir,
    jstring cacheDir,
    jstring userAgent,
    jstring initScript,
    jstring jsBridgeScript,
    jboolean incognito,
    jboolean enableDevtools,
    jboolean javascriptEnabled,
    jdouble zoomLevel,
    jboolean transparent,
    jfloat bgR,
    jfloat bgG,
    jfloat bgB,
    jfloat bgA,
    jstring initialUrl) {
    compose_cef_ensure_bridge_methods(env);

    static std::string runtime_dir_storage;
    runtime_dir_storage = compose_cef_jstring_to_utf8(env, runtimeDir);
    g_compose_cef_runtime_dir = runtime_dir_storage.c_str();

    ComposeCefCreateOptions opts;
    opts.userAgent = compose_cef_jstring_to_utf8(env, userAgent);
    opts.dataDirectory = compose_cef_jstring_to_utf8(env, cacheDir);
    opts.initScript = compose_cef_jstring_to_utf8(env, initScript);
    opts.jsBridgeScript = compose_cef_jstring_to_utf8(env, jsBridgeScript);
    opts.incognito = incognito == JNI_TRUE;
    opts.enableDevtools = enableDevtools == JNI_TRUE;
    opts.javascriptEnabled = javascriptEnabled == JNI_TRUE;
    opts.zoomLevel = zoomLevel;
    opts.transparent = transparent == JNI_TRUE;
    opts.bgR = bgR;
    opts.bgG = bgG;
    opts.bgB = bgB;
    opts.bgA = bgA;
    opts.initialUrl = compose_cef_jstring_to_utf8(env, initialUrl);

    std::shared_ptr<ComposeCefViewState> s = compose_cef_create(opts);
    if (s == nullptr) return 0;
    return compose_cef_register(std::move(s));
}

JNIEXPORT jlong JNICALL
Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeGetGtkWidget(
    JNIEnv *, jclass, jlong handle) {
    ComposeCefViewState *raw = compose_cef_state_from_handle(handle);
    if (raw == nullptr) return 0;
    std::lock_guard<std::mutex> lock(raw->mutex);
    return static_cast<jlong>(reinterpret_cast<uintptr_t>(raw->widget));
}

JNIEXPORT void JNICALL
Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeRelease(
    JNIEnv *, jclass, jlong handle) {
    compose_cef_release(compose_cef_unregister(handle));
}

JNIEXPORT void JNICALL
Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeResize(
    JNIEnv *, jclass, jlong handle, jint widthPx, jint heightPx) {
    std::lock_guard<std::mutex> lock(g_handles_mutex);
    auto it = g_handles.find(handle);
    if (it != g_handles.end()) {
        compose_cef_resize(it->second, widthPx, heightPx);
    }
}

}  // extern "C"
