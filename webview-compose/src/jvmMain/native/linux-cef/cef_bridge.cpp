// Direct CEF (Chromium) Linux backend for webview-compose.
//
// Windowless (OSR) CEF rendered into a GTK DrawingArea that is handed to
// Tao's NativeView. Context menus are built natively with GTK from CEF's
// menu model, filtered to Chromium essentials (Cut/Copy/Paste/Select All/
// Undo/Redo/Copy Link/…). JNI surface mirrors the WebKit2GTK backend so the
// Kotlin side can treat both engines interchangeably.

#include <jni.h>

#include <sys/stat.h>
#include <unistd.h>

#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_command_ids.h"
#include "include/cef_command_line.h"
#include "include/cef_client.h"
#include "include/cef_context_menu_handler.h"
#include "include/cef_cookie.h"
#include "include/cef_display_handler.h"
#include "include/cef_load_handler.h"
#include "include/cef_menu_model.h"
#include "include/cef_render_handler.h"
#include "include/cef_request_handler.h"
#include "include/cef_resource_request_handler.h"
#include "include/internal/cef_string_wrappers.h"
#include "include/cef_task.h"
#include "include/wrapper/cef_helpers.h"

namespace {

// ── JNI callback dispatch ───────────────────────────────────────────────────

JavaVM* g_jvm = nullptr;
jclass g_bridge_class = nullptr;
jmethodID g_on_navigate = nullptr;
jmethodID g_on_ipc = nullptr;
jmethodID g_on_js_result = nullptr;
jmethodID g_on_cookies = nullptr;
jmethodID g_on_screenshot = nullptr;

JNIEnv* getEnv() {
    if (g_jvm == nullptr) return nullptr;
    JNIEnv* env = nullptr;
    jint status = g_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_8);
    if (status == JNI_EDETACHED) {
        if (g_jvm->AttachCurrentThread(reinterpret_cast<void**>(&env), nullptr) != 0) {
            return nullptr;
        }
    } else if (status != JNI_OK) {
        return nullptr;
    }
    return env;
}

void ensureBridgeMethods(JNIEnv* env) {
    if (g_bridge_class != nullptr) return;
    jclass local = env->FindClass("dev/nucleusframework/webview/web/linux/CefLinuxBridge");
    if (local == nullptr) return;
    g_bridge_class = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    g_on_navigate = env->GetStaticMethodID(
        g_bridge_class, "nativeOnNavigate", "(JLjava/lang/String;)Z");
    g_on_ipc = env->GetStaticMethodID(
        g_bridge_class, "nativeOnIpcMessage", "(JLjava/lang/String;)V");
    g_on_js_result = env->GetStaticMethodID(
        g_bridge_class, "nativeOnJsResult", "(JLjava/lang/String;)V");
    g_on_cookies = env->GetStaticMethodID(
        g_bridge_class, "nativeOnCookiesResult", "(JLjava/lang/String;)V");
    g_on_screenshot = env->GetStaticMethodID(
        g_bridge_class, "nativeOnScreenshotResult", "(J[B)V");
}

bool callNavigate(jlong handle, const CefString& url) {
    JNIEnv* env = getEnv();
    if (env == nullptr || g_bridge_class == nullptr || g_on_navigate == nullptr) return true;
    jstring jurl = env->NewStringUTF(url.ToString().c_str());
    jboolean allow = env->CallStaticBooleanMethod(g_bridge_class, g_on_navigate, handle, jurl);
    env->DeleteLocalRef(jurl);
    return allow == JNI_TRUE;
}

void callJsResult(jlong handle, const CefString& result) {
    JNIEnv* env = getEnv();
    if (env == nullptr || g_bridge_class == nullptr || g_on_js_result == nullptr) return;
    jstring jresult = env->NewStringUTF(result.ToString().c_str());
    env->CallStaticVoidMethod(g_bridge_class, g_on_js_result, handle, jresult);
    env->DeleteLocalRef(jresult);
}

void callCookiesResult(jlong handle, const CefString& json) {
    JNIEnv* env = getEnv();
    if (env == nullptr || g_bridge_class == nullptr || g_on_cookies == nullptr) return;
    jstring jjson = env->NewStringUTF(json.ToString().c_str());
    env->CallStaticVoidMethod(g_bridge_class, g_on_cookies, handle, jjson);
    env->DeleteLocalRef(jjson);
}

void callScreenshotResult(jlong handle, const std::vector<uint8_t>& png) {
    JNIEnv* env = getEnv();
    if (env == nullptr || g_bridge_class == nullptr || g_on_screenshot == nullptr) return;
    jbyteArray jbytes = nullptr;
    if (!png.empty()) {
        jbytes = env->NewByteArray(static_cast<jsize>(png.size()));
        env->SetByteArrayRegion(jbytes, 0, static_cast<jsize>(png.size()),
                                reinterpret_cast<const jbyte*>(png.data()));
    }
    env->CallStaticVoidMethod(g_bridge_class, g_on_screenshot, handle, jbytes);
    if (jbytes != nullptr) env->DeleteLocalRef(jbytes);
}

// ── Per-view state ──────────────────────────────────────────────────────────

struct GtkContextMenuState;

struct CefViewState {
    std::mutex mutex;
    std::condition_variable closed_condition;
    jlong handle = 0;
    GtkWidget* widget = nullptr;
    CefRefPtr<CefBrowser> browser;
    std::vector<uint8_t> pixels;
    std::atomic<bool> gtk_draw_pending{false};
    std::atomic<bool> cef_resize_pending{false};
    int width = 0;
    int height = 0;
    int frame_width = 0;
    int frame_height = 0;
    bool first_frame_logged = false;
    bool browser_creation_requested = false;
    bool browser_creation_failed = false;
    int browser_creation_width = 0;
    int browser_creation_height = 0;
    bool closing = false;
    bool closed = false;
    std::string initial_url;
    std::string current_url;
    std::string title;
    bool is_loading = false;
    std::string init_script;
    std::string js_bridge_script;
    GdkEvent* last_context_event = nullptr;
    GtkWidget* active_menu_widget = nullptr;
    std::weak_ptr<GtkContextMenuState> active_context_menu;
};

struct CefViewHandle {
    std::shared_ptr<CefViewState> state;
};

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
    std::shared_ptr<CefViewState> owner;
    GtkWidget* parent_widget = nullptr;
    GtkWidget* menu_widget = nullptr;
    GdkEvent* trigger_event = nullptr;
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

std::mutex g_cef_mutex;
bool g_cef_initialized = false;
CefRefPtr<CefApp> g_cef_app;
int g_live_views = 0;
char g_cef_argv0[] = "composewebview-cef";
char* g_cef_argv[] = {g_cef_argv0, nullptr};

class FunctionTask final : public CefTask {
public:
    explicit FunctionTask(std::function<void()> fn) : fn_(std::move(fn)) {}

    void Execute() override { fn_(); }

private:
    std::function<void()> fn_;
    IMPLEMENT_REFCOUNTING(FunctionTask);
};

void postToCefUi(std::function<void()> fn) {
    CefPostTask(TID_UI, new FunctionTask(std::move(fn)));
}

void createBrowserForView(const std::shared_ptr<CefViewState>& state);

void scheduleCefResize(const std::shared_ptr<CefViewState>& state) {
    bool expected = false;
    if (!state->cef_resize_pending.compare_exchange_strong(expected, true)) return;
    CefPostDelayedTask(TID_UI, new FunctionTask([state] {
        CefRefPtr<CefBrowser> browser;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->closing) browser = state->browser;
        }
        if (browser.get() != nullptr) browser->GetHost()->WasResized();
        state->cef_resize_pending.store(false);
    }), 32);
}

guint cefModifiersFromGdk(guint state) {
    guint modifiers = 0;
    if (state & GDK_SHIFT_MASK) modifiers |= EVENTFLAG_SHIFT_DOWN;
    if (state & GDK_CONTROL_MASK) modifiers |= EVENTFLAG_CONTROL_DOWN;
    if (state & GDK_MOD1_MASK) modifiers |= EVENTFLAG_ALT_DOWN;
    if (state & GDK_META_MASK) modifiers |= EVENTFLAG_COMMAND_DOWN;
    if (state & GDK_BUTTON1_MASK) modifiers |= EVENTFLAG_LEFT_MOUSE_BUTTON;
    if (state & GDK_BUTTON2_MASK) modifiers |= EVENTFLAG_MIDDLE_MOUSE_BUTTON;
    if (state & GDK_BUTTON3_MASK) modifiers |= EVENTFLAG_RIGHT_MOUSE_BUTTON;
    return modifiers;
}

void attachGtkPointerDevice(GdkEvent* event, GdkWindow* window) {
    if (event == nullptr || window == nullptr) return;
    GdkDevice* device = gdk_event_get_device(event);
    if (device == nullptr) {
        GdkSeat* seat = gdk_display_get_default_seat(gdk_window_get_display(window));
        if (seat != nullptr) device = gdk_seat_get_pointer(seat);
    }
    if (device != nullptr) {
        gdk_event_set_device(event, device);
        gdk_event_set_source_device(event, device);
    }
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

void queueDrawOnGtkThread(const std::shared_ptr<CefViewState>& state) {
    bool expected = false;
    if (!state->gtk_draw_pending.compare_exchange_strong(expected, true)) return;

    auto* payload = new std::shared_ptr<CefViewState>(state);
    g_idle_add_full(
        G_PRIORITY_DEFAULT,
        [](gpointer data) -> gboolean {
            auto* state_ref = static_cast<std::shared_ptr<CefViewState>*>(data);
            GtkWidget* widget = nullptr;
            {
                std::lock_guard<std::mutex> lock((*state_ref)->mutex);
                if (!(*state_ref)->closing) widget = (*state_ref)->widget;
            }
            if (widget != nullptr) gtk_widget_queue_draw(widget);
            (*state_ref)->gtk_draw_pending.store(false);
            return G_SOURCE_REMOVE;
        },
        payload,
        [](gpointer data) {
            delete static_cast<std::shared_ptr<CefViewState>*>(data);
        });
}

gboolean onDraw(GtkWidget*, cairo_t* cairo, gpointer data) {
    auto state = *static_cast<std::shared_ptr<CefViewState>*>(data);
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->pixels.empty() || state->width <= 0 || state->height <= 0) {
        cairo_set_source_rgb(cairo, 1.0, 1.0, 1.0);
        cairo_paint(cairo);
        return FALSE;
    }

    cairo_surface_t* surface = cairo_image_surface_create_for_data(
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

void resizeView(const std::shared_ptr<CefViewState>& state, int width, int height) {
    width = std::max(1, width);
    height = std::max(1, height);
    CefRefPtr<CefBrowser> browser;
    bool create_browser = false;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->closing || (state->width == width && state->height == height)) return;
        state->width = width;
        state->height = height;
        browser = state->browser;
        if (browser.get() == nullptr &&
            !state->browser_creation_requested &&
            width >= 16 && height >= 16) {
            state->browser_creation_requested = true;
            state->browser_creation_width = width;
            state->browser_creation_height = height;
            create_browser = true;
        }
    }
    if (create_browser) {
        createBrowserForView(state);
        return;
    }
    if (browser.get() != nullptr) {
        scheduleCefResize(state);
    }
}

void onSizeAllocate(GtkWidget*, GtkAllocation* allocation, gpointer data) {
    auto state = *static_cast<std::shared_ptr<CefViewState>*>(data);
    resizeView(state, allocation->width, allocation->height);
}

gboolean onMotion(GtkWidget*, GdkEventMotion* event, gpointer data) {
    auto state = *static_cast<std::shared_ptr<CefViewState>*>(data);
    CefRefPtr<CefBrowser> browser;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        browser = state->browser;
    }
    if (browser == nullptr) return FALSE;

    CefMouseEvent mouse;
    mouse.x = static_cast<int>(event->x);
    mouse.y = static_cast<int>(event->y);
    mouse.modifiers = cefModifiersFromGdk(event->state);
    postToCefUi([browser, mouse] { browser->GetHost()->SendMouseMoveEvent(mouse, false); });
    return TRUE;
}

gboolean onButton(GtkWidget* widget, GdkEventButton* event, gpointer data) {
    auto state = *static_cast<std::shared_ptr<CefViewState>*>(data);
    if (event->type == GDK_BUTTON_RELEASE && event->button == 3) {
        // While a GTK context menu is open for this view, the right-button
        // release must not reach CEF (it would dismiss the menu before an
        // item can be chosen). Swallow it.
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->active_context_menu.expired() || state->active_menu_widget != nullptr) {
            return TRUE;
        }
    }
    if (event->type == GDK_BUTTON_PRESS && event->button == 3) {
        GdkEvent* copied = gdk_event_copy(reinterpret_cast<GdkEvent*>(event));
        attachGtkPointerDevice(copied, event->window);
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->last_context_event != nullptr) gdk_event_free(state->last_context_event);
        state->last_context_event = copied;
    }
    CefRefPtr<CefBrowser> browser;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        browser = state->browser;
    }
    if (browser == nullptr) return FALSE;

    if (event->type == GDK_BUTTON_PRESS && event->button != 3) {
        gtk_widget_grab_focus(widget);
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
    const bool grab_focus = event->type == GDK_BUTTON_PRESS && event->button != 3;
    postToCefUi([browser, mouse, button, mouse_up, grab_focus] {
        if (grab_focus) browser->GetHost()->SetFocus(true);
        browser->GetHost()->SendMouseClickEvent(mouse, button, mouse_up, 1);
    });
    return TRUE;
}

gboolean onScroll(GtkWidget*, GdkEventScroll* event, gpointer data) {
    auto state = *static_cast<std::shared_ptr<CefViewState>*>(data);
    CefRefPtr<CefBrowser> browser;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        browser = state->browser;
    }
    if (browser == nullptr) return FALSE;

    double delta_x = 0.0;
    double delta_y = 0.0;
    if (event->direction == GDK_SCROLL_SMOOTH) {
        gdk_event_get_scroll_deltas(reinterpret_cast<GdkEvent*>(event), &delta_x, &delta_y);
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
    postToCefUi([browser, mouse, delta_x, delta_y] {
        browser->GetHost()->SendMouseWheelEvent(
            mouse,
            static_cast<int>(delta_x * 120.0),
            static_cast<int>(delta_y * 120.0));
    });
    return TRUE;
}

gboolean onKey(GtkWidget*, GdkEventKey* event, gpointer data) {
    auto state = *static_cast<std::shared_ptr<CefViewState>*>(data);
    CefRefPtr<CefBrowser> browser;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        browser = state->browser;
    }
    if (browser == nullptr) return FALSE;

    CefKeyEvent key;
    key.type = event->type == GDK_KEY_RELEASE ? KEYEVENT_KEYUP : KEYEVENT_RAWKEYDOWN;
    key.windows_key_code = cefWindowsKeyCodeFromGdk(event->keyval);
    key.native_key_code = static_cast<int>(event->hardware_keycode);
    key.modifiers = cefModifiersFromGdk(event->state);
    key.is_system_key = false;
    const char32_t codepoint =
        event->type == GDK_KEY_PRESS ? gdk_keyval_to_unicode(event->keyval) : 0;
    key.character = 0;
    key.unmodified_character = 0;
    postToCefUi([browser, key, codepoint, is_release = event->type == GDK_KEY_RELEASE] {
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

gboolean onFocusIn(GtkWidget*, GdkEventFocus*, gpointer data) {
    auto state = *static_cast<std::shared_ptr<CefViewState>*>(data);
    CefRefPtr<CefBrowser> browser;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        browser = state->browser;
    }
    if (browser.get() != nullptr) postToCefUi([browser] { browser->GetHost()->SetFocus(true); });
    return FALSE;
}

gboolean onFocusOut(GtkWidget*, GdkEventFocus*, gpointer data) {
    auto state = *static_cast<std::shared_ptr<CefViewState>*>(data);
    CefRefPtr<CefBrowser> browser;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        browser = state->browser;
    }
    if (browser.get() != nullptr) postToCefUi([browser] { browser->GetHost()->SetFocus(false); });
    return FALSE;
}

void completeContextMenu(
    const std::shared_ptr<GtkContextMenuState>& menu,
    int command_id) {
    bool expected = false;
    if (!menu->completed.compare_exchange_strong(expected, true)) return;
    if (menu->owner != nullptr) {
        std::lock_guard<std::mutex> lock(menu->owner->mutex);
        if (menu->owner->active_context_menu.lock() == menu) {
            menu->owner->active_context_menu.reset();
            menu->owner->active_menu_widget = nullptr;
        }
    }
    auto callback = menu->callback;
    postToCefUi([callback, command_id] {
        if (command_id >= 0) callback->Continue(command_id, EVENTFLAG_NONE);
        else callback->Cancel();
    });
}

std::string gtkMenuLabel(const std::string& cef_label) {
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

void onGtkMenuItemActivated(GtkMenuItem*, gpointer data) {
    auto* action = static_cast<std::shared_ptr<GtkContextMenuAction>*>(data);
    completeContextMenu((*action)->menu, (*action)->command_id);
}

void appendGtkMenuEntries(
    GtkWidget* menu_widget,
    const std::vector<GtkMenuEntry>& entries,
    const std::shared_ptr<GtkContextMenuState>& menu_state) {
    for (const GtkMenuEntry& entry : entries) {
        if (entry.type == MENUITEMTYPE_SEPARATOR) {
            gtk_menu_shell_append(GTK_MENU_SHELL(menu_widget), gtk_separator_menu_item_new());
            continue;
        }

        GtkWidget* item = nullptr;
        if (entry.type == MENUITEMTYPE_CHECK || entry.type == MENUITEMTYPE_RADIO) {
            item = gtk_check_menu_item_new_with_label(entry.label.c_str());
            gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), entry.checked);
        } else {
            item = gtk_menu_item_new_with_label(entry.label.c_str());
        }
        gtk_widget_set_sensitive(item, entry.enabled);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu_widget), item);

        if (entry.type == MENUITEMTYPE_SUBMENU) {
            GtkWidget* submenu = gtk_menu_new();
            appendGtkMenuEntries(submenu, entry.children, menu_state);
            gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), submenu);
        } else if (entry.command_id >= 0) {
            auto* action = new std::shared_ptr<GtkContextMenuAction>(
                std::make_shared<GtkContextMenuAction>(GtkContextMenuAction{menu_state, entry.command_id}));
            g_signal_connect_data(
                item,
                "activate",
                G_CALLBACK(onGtkMenuItemActivated),
                action,
                [](gpointer data, GClosure*) {
                    delete static_cast<std::shared_ptr<GtkContextMenuAction>*>(data);
                },
                G_CONNECT_DEFAULT);
        }
    }
}

gboolean showGtkContextMenu(gpointer data) {
    auto* menu_ref = static_cast<std::shared_ptr<GtkContextMenuState>*>(data);
    const auto& state = *menu_ref;
    if (state->completed) return G_SOURCE_REMOVE;
    if (state->entries.empty() || state->parent_widget == nullptr ||
        !gtk_widget_get_realized(state->parent_widget)) {
        completeContextMenu(state, -1);
        return G_SOURCE_REMOVE;
    }

    GtkWidget* gtk_menu = gtk_menu_new();
    bool owner_closing = false;
    {
        std::lock_guard<std::mutex> lock(state->owner->mutex);
        owner_closing = state->owner->closing;
        if (!owner_closing) {
            state->owner->active_context_menu = state;
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
    auto* deactivate_payload = new std::shared_ptr<GtkContextMenuState>(state);
    g_signal_connect_data(
        gtk_menu,
        "deactivate",
        G_CALLBACK(+[](GtkMenuShell* shell, gpointer context) {
            auto* menu_state = static_cast<std::shared_ptr<GtkContextMenuState>*>(context);
            (void) shell;
            completeContextMenu(*menu_state, -1);
        }),
        deactivate_payload,
        [](gpointer value, GClosure*) {
            delete static_cast<std::shared_ptr<GtkContextMenuState>*>(value);
        },
        G_CONNECT_DEFAULT);
    gtk_menu_attach_to_widget(GTK_MENU(gtk_menu), state->parent_widget, nullptr);
    gtk_widget_show_all(gtk_menu);
    GtkAllocation allocation;
    gtk_widget_get_allocation(state->parent_widget, &allocation);
    GdkRectangle anchor{allocation.x + state->x, allocation.y + state->y, 1, 1};
    GdkWindow* parent_window = gtk_widget_get_window(state->parent_widget);
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

bool runGtkContextMenu(
    CefRefPtr<CefMenuModel> model,
    CefRefPtr<CefRunContextMenuCallback> callback,
    const std::shared_ptr<CefViewState>& owner,
    GtkWidget* parent_widget,
    GdkEvent* trigger_event,
    int x,
    int y) {
    auto menu = std::make_shared<GtkContextMenuState>();
    menu->callback = callback;
    menu->owner = owner;
    menu->entries = copyMenuModel(model);
    menu->parent_widget = parent_widget;
    menu->trigger_event = trigger_event;
    menu->x = x;
    menu->y = y;
    auto* payload = new std::shared_ptr<GtkContextMenuState>(menu);
    g_idle_add_full(
        G_PRIORITY_DEFAULT,
        showGtkContextMenu,
        payload,
        [](gpointer value) {
            delete static_cast<std::shared_ptr<GtkContextMenuState>*>(value);
        });
    return true;
}

// ── CEF callbacks → Kotlin ──────────────────────────────────────────────────

// JS results are returned via console messages with this reserved prefix.
// The JNI section below wraps evaluated scripts to console.log(prefix + result).
constexpr char kJsResultPrefix[] = "__compose_cef_js:";

class WebViewApp final : public CefApp, public CefBrowserProcessHandler {
public:
    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }

    void OnBeforeCommandLineProcessing(
        const CefString& process_type,
        CefRefPtr<CefCommandLine> command_line) override {
        command_line->AppendSwitch("disable-gpu");
        command_line->AppendSwitchWithValue("disable-features", "Vulkan");
    }

private:
    IMPLEMENT_REFCOUNTING(WebViewApp);
};

class WebViewClient final : public CefClient,
                            public CefRenderHandler,
                            public CefLifeSpanHandler,
                            public CefContextMenuHandler,
                            public CefDisplayHandler,
                            public CefLoadHandler,
                            public CefRequestHandler {
public:
    explicit WebViewClient(std::shared_ptr<CefViewState> state) : state_(std::move(state)) {}

    CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override { return this; }
    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
    CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }

    // Context menu — Chromium essentials only (Cut/Copy/Paste/…).
    bool RunContextMenu(
        CefRefPtr<CefBrowser>,
        CefRefPtr<CefFrame>,
        CefRefPtr<CefContextMenuParams> params,
        CefRefPtr<CefMenuModel> model,
        CefRefPtr<CefRunContextMenuCallback> callback) override {
        GtkWidget* widget = nullptr;
        GdkEvent* trigger_event = nullptr;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            widget = state_->widget;
            if (widget != nullptr) g_object_ref(widget);
            if (state_->last_context_event != nullptr) {
                trigger_event = gdk_event_copy(state_->last_context_event);
            }
        }
        return runGtkContextMenu(
            model,
            callback,
            state_,
            widget,
            trigger_event,
            params->GetXCoord(),
            params->GetYCoord());
    }

    // Lifecycle
    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
        std::fprintf(stderr, "[cef] browser created\n");
        bool should_close = false;
        bool resize_after_create = false;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->browser = browser;
            should_close = state_->closing;
            resize_after_create =
                state_->width != state_->browser_creation_width ||
                state_->height != state_->browser_creation_height;
        }
        if (should_close) browser->GetHost()->CloseBrowser(true);
        else if (resize_after_create) scheduleCefResize(state_);
    }

    void OnBeforeClose(CefRefPtr<CefBrowser>) override {
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->browser = nullptr;
            state_->closed = true;
        }
        state_->closed_condition.notify_all();
    }

    // Rendering
    void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        rect = CefRect(0, 0, std::max(1, state_->width), std::max(1, state_->height));
    }

    void OnPaint(
        CefRefPtr<CefBrowser>,
        PaintElementType type,
        const RectList&,
        const void* buffer,
        int width,
        int height) override {
        if (type != PET_VIEW || buffer == nullptr || width <= 0 || height <= 0) return;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (state_->closing) return;
            if (!state_->first_frame_logged) {
                std::fprintf(stderr, "[cef] first OSR frame %dx%d\n", width, height);
                state_->first_frame_logged = true;
            }
            const size_t size = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
            state_->pixels.resize(size);
            std::memcpy(state_->pixels.data(), buffer, size);
            state_->frame_width = width;
            state_->frame_height = height;
        }
        queueDrawOnGtkThread(state_);
    }

    // Navigation interception + URL/title/loading tracking
    bool OnBeforeBrowse(
        CefRefPtr<CefBrowser> browser,
        CefRefPtr<CefFrame> frame,
        CefRefPtr<CefRequest> request,
        bool user_gesture,
        bool is_redirect) override {
        if (!frame->IsMain()) return false;
        const CefString url = request->GetURL();
        const jlong handle = state_->handle;
        return !callNavigate(handle, url);
    }

    void OnAddressChange(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, const CefString& url) override {
        if (!frame->IsMain()) return;
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->current_url = url.ToString();
    }

    void OnTitleChange(CefRefPtr<CefBrowser>, const CefString& title) override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->title = title.ToString();
    }

    void OnLoadingStateChange(
        CefRefPtr<CefBrowser>,
        bool isLoading,
        bool,
        bool) override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->is_loading = isLoading;
    }

    bool OnConsoleMessage(
        CefRefPtr<CefBrowser>,
        cef_log_severity_t,
        const CefString& message,
        const CefString&,
        int) override {
        const std::string text = message.ToString();
        const std::string prefix = kJsResultPrefix;
        if (text.compare(0, prefix.size(), prefix) == 0) {
            callJsResult(state_->handle, CefString(text.substr(prefix.size())));
            return true;
        }
        std::fprintf(stderr, "[cef:console] %s\n", text.c_str());
        return false;
    }

    void OnLoadEnd(
        CefRefPtr<CefBrowser> browser,
        CefRefPtr<CefFrame> frame,
        int) override {
        if (!frame->IsMain()) return;
        std::string init_script;
        std::string bridge_script;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            init_script = state_->init_script;
            bridge_script = state_->js_bridge_script;
        }
        if (!init_script.empty()) frame->ExecuteJavaScript(init_script, frame->GetURL(), 0);
        if (!bridge_script.empty()) frame->ExecuteJavaScript(bridge_script, frame->GetURL(), 0);
    }

private:
    std::shared_ptr<CefViewState> state_;
    IMPLEMENT_REFCOUNTING(WebViewClient);
};

void createBrowserForView(const std::shared_ptr<CefViewState>& state) {
    std::string url;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->closing || state->browser_creation_failed) return;
        url = state->initial_url;
        state->browser_creation_width = state->width;
        state->browser_creation_height = state->height;
    }

    CefRefPtr<CefClient> client(static_cast<CefClient*>(new WebViewClient(state)));
    CefWindowInfo window_info;
    window_info.SetAsWindowless(0);
    CefBrowserSettings browser_settings;
    browser_settings.windowless_frame_rate = 60;
    browser_settings.background_color = CefColorSetARGB(0xFF, 0xFF, 0xFF, 0xFF);
    const bool created = CefBrowserHost::CreateBrowser(
        window_info,
        client,
        CefString(url.empty() ? "about:blank" : url),
        browser_settings,
        nullptr,
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

// NativeLibraryLoader extracts sidecars from the JAR without Unix mode bits,
// so cef_subprocess may not be executable. CEF only execs the subprocess
// path, never dlopens it, so point it at a tiny +x shim that execs the real
// helper. Created once per runtime dir.
std::string ensureSubprocessShim(const std::string& runtime_dir) {
    const std::string real = runtime_dir + "/cef_subprocess";
    const std::string shim = runtime_dir + "/cef_subprocess.sh";
    if (g_file_test(shim.c_str(), G_FILE_TEST_EXISTS)) return shim;
    const std::string contents =
        "#!/bin/sh\nchmod 0755 \"" + real + "\" 2>/dev/null\nexec \"" + real + "\" \"$@\"\n";
    if (g_file_set_contents(shim.c_str(), contents.data(),
                            static_cast<gssize>(contents.size()), nullptr)) {
        chmod(shim.c_str(), 0755);
    }
    return shim;
}

bool initializeCef(const std::string& runtime_dir, const std::string& cache_dir) {
    std::lock_guard<std::mutex> lock(g_cef_mutex);
    if (g_cef_initialized) return true;

    // NativeLibraryLoader extracts sidecars as flat files; CEF expects the
    // locale pack at <locales_dir>/en-US.pak. Hard-link (fallback: copy) it.
    const std::string locales_dir = runtime_dir + "/locales";
    g_mkdir_with_parents(locales_dir.c_str(), 0755);
    const std::string locale_src = runtime_dir + "/en-US.pak";
    const std::string locale_dst = locales_dir + "/en-US.pak";
    if (g_file_test(locale_src.c_str(), G_FILE_TEST_EXISTS) &&
        !g_file_test(locale_dst.c_str(), G_FILE_TEST_EXISTS)) {
        if (link(locale_src.c_str(), locale_dst.c_str()) != 0) {
            std::string contents;
            char* data = nullptr;
            gsize length = 0;
            if (g_file_get_contents(locale_src.c_str(), &data, &length, nullptr)) {
                contents.assign(data, length);
                g_free(data);
                g_file_set_contents(locale_dst.c_str(), contents.data(), static_cast<gssize>(contents.size()), nullptr);
            }
        }
    }

    CefMainArgs main_args(1, g_cef_argv);
    CefSettings settings;
    settings.no_sandbox = true;
    settings.multi_threaded_message_loop = true;
    settings.windowless_rendering_enabled = true;
    CefString(&settings.browser_subprocess_path).FromString(ensureSubprocessShim(runtime_dir));
    CefString(&settings.resources_dir_path).FromString(runtime_dir);
    CefString(&settings.locales_dir_path).FromString(runtime_dir + "/locales");
    CefString(&settings.root_cache_path).FromString(cache_dir);
    CefString(&settings.cache_path).FromString(cache_dir + "/profile");
    CefString(&settings.log_file).FromString(cache_dir + "/cef.log");

    g_mkdir_with_parents(cache_dir.c_str(), 0700);
    g_cef_app = new WebViewApp();
    g_cef_initialized = CefInitialize(main_args, settings, g_cef_app, nullptr);
    return g_cef_initialized;
}

void destroyGtkState(const std::shared_ptr<CefViewState>& state) {
    GtkWidget* widget = nullptr;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        widget = state->widget;
        state->widget = nullptr;
    }
    if (widget != nullptr) {
        gtk_widget_destroy(widget);
        g_object_unref(widget);
    }
}

std::string jstringToStd(JNIEnv* env, jstring value) {
    if (value == nullptr) return {};
    const char* chars = env->GetStringUTFChars(value, nullptr);
    if (chars == nullptr) return {};
    std::string out(chars);
    env->ReleaseStringUTFChars(value, chars);
    return out;
}

CefRefPtr<CefBrowser> browserFromHandle(jlong handle) {
    if (handle == 0) return nullptr;
    auto* view = reinterpret_cast<CefViewHandle*>(static_cast<uintptr_t>(handle));
    std::lock_guard<std::mutex> lock(view->state->mutex);
    return view->state->browser;
}

void postBrowserOp(jlong handle, std::function<void(CefRefPtr<CefBrowser>)> op) {
    CefRefPtr<CefBrowser> browser = browserFromHandle(handle);
    if (browser.get() == nullptr) return;
    postToCefUi([browser, op = std::move(op)] { op(browser); });
}

}  // namespace

extern "C" {

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    g_jvm = vm;
    return JNI_VERSION_1_8;
}

JNIEXPORT jlong JNICALL
Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeCreate(
    JNIEnv* env,
    jclass,
    jstring runtime_dir_value,
    jstring cache_dir_value,
    jstring user_agent_value,
    jstring init_script_value,
    jstring js_bridge_script_value,
    jboolean incognito,
    jboolean enable_devtools,
    jboolean javascript_enabled,
    jdouble zoom_level,
    jboolean transparent,
    jfloat bg_r,
    jfloat bg_g,
    jfloat bg_b,
    jfloat bg_a,
    jstring initial_url_value) {
    ensureBridgeMethods(env);
    const std::string runtime_dir = jstringToStd(env, runtime_dir_value);
    const std::string cache_dir = jstringToStd(env, cache_dir_value);
    if (!initializeCef(runtime_dir, cache_dir)) return 0;

    auto state = std::make_shared<CefViewState>();
    state->initial_url = jstringToStd(env, initial_url_value);
    state->init_script = jstringToStd(env, init_script_value);
    state->js_bridge_script = jstringToStd(env, js_bridge_script_value);

    GtkWidget* widget = gtk_drawing_area_new();
    g_object_ref_sink(widget);
    gtk_widget_set_can_focus(widget, TRUE);
    gtk_widget_set_hexpand(widget, TRUE);
    gtk_widget_set_vexpand(widget, TRUE);
    gtk_widget_add_events(
        widget,
        GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK |
            GDK_SCROLL_MASK | GDK_KEY_PRESS_MASK | GDK_KEY_RELEASE_MASK | GDK_FOCUS_CHANGE_MASK);
    state->widget = widget;
    auto* state_data = new std::shared_ptr<CefViewState>(state);
    g_object_set_data_full(
        G_OBJECT(widget),
        "compose-cef-view-state",
        state_data,
        [](gpointer data) { delete static_cast<std::shared_ptr<CefViewState>*>(data); });

    g_signal_connect(widget, "draw", G_CALLBACK(onDraw), state_data);
    g_signal_connect(widget, "size-allocate", G_CALLBACK(onSizeAllocate), state_data);
    g_signal_connect(widget, "motion-notify-event", G_CALLBACK(onMotion), state_data);
    g_signal_connect(widget, "button-press-event", G_CALLBACK(onButton), state_data);
    g_signal_connect(widget, "button-release-event", G_CALLBACK(onButton), state_data);
    g_signal_connect(widget, "scroll-event", G_CALLBACK(onScroll), state_data);
    g_signal_connect(widget, "key-press-event", G_CALLBACK(onKey), state_data);
    g_signal_connect(widget, "key-release-event", G_CALLBACK(onKey), state_data);
    g_signal_connect(widget, "focus-in-event", G_CALLBACK(onFocusIn), state_data);
    g_signal_connect(widget, "focus-out-event", G_CALLBACK(onFocusOut), state_data);

    gtk_widget_show(widget);

    auto* view = new CefViewHandle{state};
    const jlong handle = static_cast<jlong>(reinterpret_cast<uintptr_t>(view));
    state->handle = handle;
    {
        std::lock_guard<std::mutex> lock(g_cef_mutex);
        g_live_views++;
    }
    return handle;
}

JNIEXPORT jlong JNICALL
Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeGetGtkWidget(
    JNIEnv*,
    jclass,
    jlong handle) {
    if (handle == 0) return 0;
    auto* view = reinterpret_cast<CefViewHandle*>(static_cast<uintptr_t>(handle));
    std::lock_guard<std::mutex> lock(view->state->mutex);
    return static_cast<jlong>(reinterpret_cast<uintptr_t>(view->state->widget));
}

JNIEXPORT void JNICALL
Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeRelease(
    JNIEnv*,
    jclass,
    jlong handle) {
    if (handle == 0) return;
    auto* view = reinterpret_cast<CefViewHandle*>(static_cast<uintptr_t>(handle));
    std::shared_ptr<CefViewState> state = view->state;
    CefRefPtr<CefBrowser> browser;
    std::shared_ptr<GtkContextMenuState> active_menu;
    GtkWidget* active_menu_widget = nullptr;
    GdkEvent* last_context_event = nullptr;
    bool should_wait = false;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->closing = true;
        browser = state->browser;
        active_menu = state->active_context_menu.lock();
        active_menu_widget = state->active_menu_widget;
        state->active_context_menu.reset();
        state->active_menu_widget = nullptr;
        last_context_event = state->last_context_event;
        state->last_context_event = nullptr;
        should_wait = state->browser_creation_requested && !state->browser_creation_failed;
        if (!should_wait) state->closed = true;
    }
    state->closed_condition.notify_all();
    if (active_menu != nullptr) completeContextMenu(active_menu, -1);
    if (active_menu_widget != nullptr) gtk_widget_destroy(active_menu_widget);
    if (last_context_event != nullptr) gdk_event_free(last_context_event);
    if (browser.get() != nullptr) {
        postToCefUi([browser] { browser->GetHost()->CloseBrowser(true); });
    }
    if (should_wait) {
        std::unique_lock<std::mutex> lock(state->mutex);
        state->closed_condition.wait_for(
            lock,
            std::chrono::seconds(10),
            [&] { return state->closed; });
    }

    // Do not keep a CEF browser reference alive across potential CefShutdown().
    browser = nullptr;
    active_menu.reset();
    destroyGtkState(state);
    delete view;

    bool do_shutdown = false;
    {
        std::lock_guard<std::mutex> lock(g_cef_mutex);
        g_live_views--;
        if (g_cef_initialized && g_live_views <= 0) {
            do_shutdown = true;
        }
    }
    if (do_shutdown) {
        // Synchronous shutdown on the releasing thread, before the JVM starts
        // tearing down native libraries. This mirrors the proven prototype
        // shutdown path and avoids calling into CEF from a JVM-exit hook after
        // its threads are gone (which segfaulted in anonymous memory).
        std::lock_guard<std::mutex> lock(g_cef_mutex);
        if (g_cef_initialized && g_live_views <= 0) {
            CefShutdown();
            g_cef_initialized = false;
            g_cef_app = nullptr;
        }
    }
}

// ── Navigation ──────────────────────────────────────────────────────────────

JNIEXPORT void JNICALL
Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeLoadUrl(
    JNIEnv* env,
    jclass,
    jlong handle,
    jstring url_value) {
    const std::string url = jstringToStd(env, url_value);
    if (url.empty()) return;
    postBrowserOp(handle, [url](CefRefPtr<CefBrowser> b) {
        b->GetMainFrame()->LoadURL(CefString(url));
    });
}

JNIEXPORT void JNICALL
Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeLoadUrlWithHeaders(
    JNIEnv* env,
    jclass,
    jlong handle,
    jstring url_value,
    jobjectArray header_names,
    jobjectArray header_values) {
    const std::string url = jstringToStd(env, url_value);
    if (url.empty()) return;
    CefRequest::HeaderMap headers;
    const jsize count = env->GetArrayLength(header_names);
    for (jsize i = 0; i < count; ++i) {
        auto name = static_cast<jstring>(env->GetObjectArrayElement(header_names, i));
        auto value = static_cast<jstring>(env->GetObjectArrayElement(header_values, i));
        headers.insert(std::make_pair(jstringToStd(env, name), jstringToStd(env, value)));
        env->DeleteLocalRef(name);
        env->DeleteLocalRef(value);
    }
    postBrowserOp(handle, [url, headers](CefRefPtr<CefBrowser> b) {
        CefRefPtr<CefRequest> request = CefRequest::Create();
        request->SetURL(CefString(url));
        request->SetMethod("GET");
        CefRequest::HeaderMap map = headers;
        request->SetHeaderMap(map);
        b->GetMainFrame()->LoadRequest(request);
    });
}

JNIEXPORT void JNICALL
Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeLoadHtml(
    JNIEnv* env,
    jclass,
    jlong handle,
    jstring html_value,
    jstring base_uri_value) {
    const std::string html = jstringToStd(env, html_value);
    // CefFrame::LoadString was removed in CEF 139+; load via a data: URL.
    std::string url = "data:text/html;charset=utf-8,";
    for (const unsigned char c : html) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            url.push_back(static_cast<char>(c));
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            url += buf;
        }
    }
    postBrowserOp(handle, [url](CefRefPtr<CefBrowser> b) {
        b->GetMainFrame()->LoadURL(CefString(url));
    });
}

JNIEXPORT void JNICALL
Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeGoBack(
    JNIEnv*, jclass, jlong handle) {
    postBrowserOp(handle, [](CefRefPtr<CefBrowser> b) { b->GoBack(); });
}

JNIEXPORT void JNICALL
Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeGoForward(
    JNIEnv*, jclass, jlong handle) {
    postBrowserOp(handle, [](CefRefPtr<CefBrowser> b) { b->GoForward(); });
}

JNIEXPORT void JNICALL
Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeReload(
    JNIEnv*, jclass, jlong handle) {
    postBrowserOp(handle, [](CefRefPtr<CefBrowser> b) { b->Reload(); });
}

JNIEXPORT void JNICALL
Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeStopLoading(
    JNIEnv*, jclass, jlong handle) {
    postBrowserOp(handle, [](CefRefPtr<CefBrowser> b) { b->StopLoad(); });
}

JNIEXPORT jboolean JNICALL
Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeCanGoBack(
    JNIEnv*, jclass, jlong handle) {
    CefRefPtr<CefBrowser> browser = browserFromHandle(handle);
    if (browser.get() == nullptr) return JNI_FALSE;
    return browser->CanGoBack() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeCanGoForward(
    JNIEnv*, jclass, jlong handle) {
    CefRefPtr<CefBrowser> browser = browserFromHandle(handle);
    if (browser.get() == nullptr) return JNI_FALSE;
    return browser->CanGoForward() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL
Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeCurrentUrl(
    JNIEnv* env, jclass, jlong handle) {
    if (handle == 0) return nullptr;
    auto* view = reinterpret_cast<CefViewHandle*>(static_cast<uintptr_t>(handle));
    std::lock_guard<std::mutex> lock(view->state->mutex);
    if (view->state->current_url.empty()) return nullptr;
    return env->NewStringUTF(view->state->current_url.c_str());
}

JNIEXPORT jstring JNICALL
Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeGetTitle(
    JNIEnv* env, jclass, jlong handle) {
    if (handle == 0) return nullptr;
    auto* view = reinterpret_cast<CefViewHandle*>(static_cast<uintptr_t>(handle));
    std::lock_guard<std::mutex> lock(view->state->mutex);
    if (view->state->title.empty()) return nullptr;
    return env->NewStringUTF(view->state->title.c_str());
}

JNIEXPORT jboolean JNICALL
Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeIsLoading(
    JNIEnv*, jclass, jlong handle) {
    if (handle == 0) return JNI_FALSE;
    auto* view = reinterpret_cast<CefViewHandle*>(static_cast<uintptr_t>(handle));
    std::lock_guard<std::mutex> lock(view->state->mutex);
    return view->state->is_loading ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeSetZoomLevel(
    JNIEnv*, jclass, jlong handle, jdouble zoom) {
    postBrowserOp(handle, [zoom](CefRefPtr<CefBrowser> b) {
        b->GetHost()->SetZoomLevel(zoom == 1.0 ? 0.0 : zoom);
    });
}

JNIEXPORT void JNICALL
Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeFocus(
    JNIEnv*, jclass, jlong handle) {
    postBrowserOp(handle, [](CefRefPtr<CefBrowser> b) { b->GetHost()->SetFocus(true); });
}

JNIEXPORT void JNICALL
Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeOpenDevTools(
    JNIEnv*, jclass, jlong handle) {
    postBrowserOp(handle, [](CefRefPtr<CefBrowser> b) {
        CefWindowInfo window_info;
        window_info.SetAsWindowless(0);
        CefBrowserSettings settings;
        b->GetHost()->ShowDevTools(window_info, nullptr, settings, CefPoint(0, 0));
    });
}

JNIEXPORT void JNICALL
Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeCloseDevTools(
    JNIEnv*, jclass, jlong handle) {
    postBrowserOp(handle, [](CefRefPtr<CefBrowser> b) { b->GetHost()->CloseDevTools(); });
}

// ── JavaScript ──────────────────────────────────────────────────────────────
//
// Results are returned via console messages with a reserved prefix; see
// WebViewClient::OnConsoleMessage.

std::string jsonEncodeString(const std::string& value) {
    std::string out = "\"";
    for (const char c : value) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    out += "\"";
    return out;
}

std::string base64Encode(const std::string& in) {
    static const char* table =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    while (i < in.size()) {
        const uint32_t a = in[i++];
        const uint32_t b = i < in.size() ? in[i++] : 0;
        const uint32_t c = i < in.size() ? in[i++] : 0;
        const uint32_t triple = (a << 16) | (b << 8) | c;
        out.push_back(table[(triple >> 18) & 0x3F]);
        out.push_back(table[(triple >> 12) & 0x3F]);
        out.push_back(i - 1 < in.size() ? table[(triple >> 6) & 0x3F] : '=');
        out.push_back(i < in.size() ? table[triple & 0x3F] : '=');
    }
    return out;
}

JNIEXPORT void JNICALL
Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeEvaluateJavaScript(
    JNIEnv* env,
    jclass,
    jlong handle,
    jstring script_value) {
    const std::string script = jstringToStd(env, script_value);
    if (script.empty()) return;
    // Decode + eval the script, then ship the result back via console.log with
    // a reserved prefix (OnConsoleMessage → nativeOnJsResult).
    const std::string wrapped =
        "(function(){var s=decodeURIComponent(escape(atob(\"" + base64Encode(script) +
        "\")));var r;try{r=String(eval(s));}catch(e){r='';}"
        "console.log(\"" + std::string(kJsResultPrefix) + "\"+r);})();";
    postBrowserOp(handle, [wrapped](CefRefPtr<CefBrowser> b) {
        b->GetMainFrame()->ExecuteJavaScript(CefString(wrapped), b->GetMainFrame()->GetURL(), 0);
    });
}

// ── Cookies ─────────────────────────────────────────────────────────────────

std::string cookieToJson(const CefCookie& cookie) {
    const std::string name = CefString(&cookie.name).ToString();
    const std::string value = CefString(&cookie.value).ToString();
    const std::string domain = CefString(&cookie.domain).ToString();
    const std::string path = CefString(&cookie.path).ToString();
    std::string out = "{\"name\":";
    out += "\"" + name + "\",\"value\":\"" + value + "\"";
    out += ",\"domain\":\"" + domain + "\"";
    out += ",\"path\":\"" + path + "\"";
    out += std::string(",\"secure\":") + (cookie.secure ? "true" : "false");
    out += std::string(",\"httpOnly\":") + (cookie.httponly ? "true" : "false");
    out += std::string(",\"sessionOnly\":") + (cookie.has_expires ? "false" : "true");
    if (cookie.has_expires) {
        // cef_basetime_t is microseconds since the Windows epoch (1601).
        out += ",\"expiresDate\":";
        const int64_t epoch_us = (cookie.expires.val - 11644473600000000LL);
        out += std::to_string(epoch_us / 1000);
    }
    out += "}";
    return out;
}

class CookieVisitor final : public CefCookieVisitor {
public:
    explicit CookieVisitor(jlong handle) : handle_(handle) { json_ = "["; }

    bool Visit(
        const CefCookie& cookie,
        int count,
        int total,
        bool& deleteCookie) override {
        if (count > 0) json_ += ",";
        json_ += cookieToJson(cookie);
        return true;
    }

    ~CookieVisitor() override {
        json_ += "]";
        callCookiesResult(handle_, CefString(json_));
    }

private:
    jlong handle_;
    std::string json_;
    IMPLEMENT_REFCOUNTING(CookieVisitor);
};

JNIEXPORT void JNICALL
Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeGetCookies(
    JNIEnv* env,
    jclass,
    jlong handle,
    jstring url_value) {
    const std::string url = jstringToStd(env, url_value);
    CefRefPtr<CefCookieManager> manager = CefCookieManager::GetGlobalManager(nullptr);
    if (manager.get() == nullptr) {
        callCookiesResult(handle, CefString("[]"));
        return;
    }
    CefRefPtr<CookieVisitor> visitor = new CookieVisitor(handle);
    CefPostTask(TID_IO, new FunctionTask([manager, url, visitor] {
        manager->VisitUrlCookies(CefString(url), true, visitor);
    }));
}

JNIEXPORT void JNICALL
Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeSetCookie(
    JNIEnv* env,
    jclass,
    jlong handle,
    jstring name_value,
    jstring value_value,
    jstring domain_value,
    jstring path_value,
    jboolean secure,
    jboolean http_only,
    jlong expires_ms,
    jstring same_site_value) {
    CefCookie cookie;
    CefString(&cookie.name) = jstringToStd(env, name_value);
    CefString(&cookie.value) = jstringToStd(env, value_value);
    const std::string domain = jstringToStd(env, domain_value);
    if (!domain.empty()) CefString(&cookie.domain) = domain;
    const std::string path = jstringToStd(env, path_value);
    CefString(&cookie.path) = path.empty() ? "/" : path;
    cookie.secure = secure == JNI_TRUE;
    cookie.httponly = http_only == JNI_TRUE;
    if (expires_ms > 0) {
        cookie.has_expires = true;
        // cef_basetime_t: microseconds since the Windows epoch (1601).
        cookie.expires.val = 11644473600000000LL + expires_ms * 1000LL;
    }
    const std::string same_site = jstringToStd(env, same_site_value);
    if (same_site == "None") cookie.same_site = CEF_COOKIE_SAME_SITE_NO_RESTRICTION;
    else if (same_site == "Strict") cookie.same_site = CEF_COOKIE_SAME_SITE_STRICT_MODE;
    else if (same_site == "Lax") cookie.same_site = CEF_COOKIE_SAME_SITE_LAX_MODE;
    else cookie.same_site = CEF_COOKIE_SAME_SITE_UNSPECIFIED;

    CefRefPtr<CefCookieManager> manager = CefCookieManager::GetGlobalManager(nullptr);
    if (manager.get() == nullptr) return;
    std::string url = domain.empty() ? "" : (cookie.secure ? "https://" : "http://") + domain;
    CefPostTask(TID_IO, new FunctionTask([manager, url, cookie] {
        manager->SetCookie(CefString(url), cookie, nullptr);
    }));
}

JNIEXPORT void JNICALL
Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeRemoveAllCookies(
    JNIEnv*,
    jclass,
    jlong handle) {
    CefRefPtr<CefCookieManager> manager = CefCookieManager::GetGlobalManager(nullptr);
    if (manager.get() == nullptr) return;
    CefPostTask(TID_IO, new FunctionTask([manager] {
        manager->DeleteCookies(CefString(), CefString(), nullptr);
    }));
}

JNIEXPORT void JNICALL
Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeRemoveCookiesForUrl(
    JNIEnv* env,
    jclass,
    jlong handle,
    jstring url_value) {
    const std::string url = jstringToStd(env, url_value);
    CefRefPtr<CefCookieManager> manager = CefCookieManager::GetGlobalManager(nullptr);
    if (manager.get() == nullptr) return;
    CefPostTask(TID_IO, new FunctionTask([manager, url] {
        manager->DeleteCookies(CefString(url), CefString(), nullptr);
    }));
}

// ── Screenshot ──────────────────────────────────────────────────────────────

JNIEXPORT void JNICALL
Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeCaptureScreenshot(
    JNIEnv*,
    jclass,
    jlong handle) {
    if (handle == 0) {
        callScreenshotResult(handle, {});
        return;
    }
    auto* view = reinterpret_cast<CefViewHandle*>(static_cast<uintptr_t>(handle));
    std::vector<uint8_t> bgra;
    int width = 0;
    int height = 0;
    {
        std::lock_guard<std::mutex> lock(view->state->mutex);
        bgra = view->state->pixels;
        width = view->state->frame_width;
        height = view->state->frame_height;
    }
    if (bgra.empty() || width <= 0 || height <= 0) {
        callScreenshotResult(handle, {});
        return;
    }
    // Convert BGRA to PNG via cairo.
    cairo_surface_t* surface = cairo_image_surface_create_for_data(
        bgra.data(), CAIRO_FORMAT_ARGB32, width, height, width * 4);
    std::vector<uint8_t> png;
    cairo_write_func_t write_fn = [](void* closure, const unsigned char* data, unsigned int length) {
        auto* out = static_cast<std::vector<uint8_t>*>(closure);
        out->insert(out->end(), data, data + length);
        return CAIRO_STATUS_SUCCESS;
    };
    cairo_surface_write_to_png_stream(surface, write_fn, &png);
    cairo_surface_destroy(surface);
    callScreenshotResult(handle, png);
}

JNIEXPORT void JNICALL
Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeResize(
    JNIEnv*,
    jclass,
    jlong handle,
    jint width,
    jint height) {
    if (handle == 0 || width <= 0 || height <= 0) return;
    auto* view = reinterpret_cast<CefViewHandle*>(static_cast<uintptr_t>(handle));
    resizeView(view->state, width, height);
}

}  // extern "C"
