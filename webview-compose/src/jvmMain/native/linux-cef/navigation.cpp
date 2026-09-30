#include "compose_cef_internal.h"

#include <cctype>
#include <cstdio>
#include <utility>

#include "include/cef_request.h"

namespace {

void postBrowserOp(jlong handle, std::function<void(CefRefPtr<CefBrowser>)> op) {
    std::shared_ptr<ComposeCefViewState> state = compose_cef_shared_from_handle(handle);
    if (state == nullptr) return;
    CefRefPtr<CefBrowser> browser;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        browser = state->browser;
    }
    if (browser.get() == nullptr) return;
    compose_cef_post_to_ui([browser, op = std::move(op)] { op(browser); });
}

/* Percent-encode UTF-8 for a data: URL body (CEF 139+ removed LoadString). */
std::string percentEncodeUtf8(const std::string &input) {
    static const char *hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(input.size() * 3);
    for (const unsigned char c : input) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

}  // namespace

extern "C" {

JNIEXPORT void JNICALL
Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeLoadUrl(
    JNIEnv *env, jclass, jlong handle, jstring url) {
    const std::string u = compose_cef_jstring_to_utf8(env, url);
    if (u.empty()) return;
    postBrowserOp(handle, [u](CefRefPtr<CefBrowser> b) {
        b->GetMainFrame()->LoadURL(CefString(u));
    });
}

JNIEXPORT void JNICALL
Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeLoadUrlWithHeaders(
    JNIEnv *env,
    jclass,
    jlong handle,
    jstring url,
    jobjectArray headerNames,
    jobjectArray headerValues) {
    const std::string u = compose_cef_jstring_to_utf8(env, url);
    if (u.empty()) return;
    CefRequest::HeaderMap headers;
    if (headerNames != nullptr && headerValues != nullptr) {
        const jsize count = env->GetArrayLength(headerNames);
        const jsize vcount = env->GetArrayLength(headerValues);
        const jsize n = count < vcount ? count : vcount;
        for (jsize i = 0; i < n; ++i) {
            auto jn = static_cast<jstring>(env->GetObjectArrayElement(headerNames, i));
            auto jv = static_cast<jstring>(env->GetObjectArrayElement(headerValues, i));
            headers.insert(std::make_pair(
                compose_cef_jstring_to_utf8(env, jn),
                compose_cef_jstring_to_utf8(env, jv)));
            env->DeleteLocalRef(jn);
            env->DeleteLocalRef(jv);
        }
    }
    postBrowserOp(handle, [u, headers](CefRefPtr<CefBrowser> b) {
        CefRefPtr<CefRequest> request = CefRequest::Create();
        request->SetURL(CefString(u));
        request->SetMethod("GET");
        CefRequest::HeaderMap map = headers;
        request->SetHeaderMap(map);
        b->GetMainFrame()->LoadRequest(request);
    });
}

JNIEXPORT void JNICALL
Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeLoadHtml(
    JNIEnv *env, jclass, jlong handle, jstring html, jstring /*baseUri*/) {
    const std::string markup = compose_cef_jstring_to_utf8(env, html);
    // data: URL so back/forward history works (CefFrame::LoadString removed in CEF 139+).
    const std::string url = "data:text/html;charset=utf-8," + percentEncodeUtf8(markup);
    postBrowserOp(handle, [url](CefRefPtr<CefBrowser> b) {
        b->GetMainFrame()->LoadURL(CefString(url));
    });
}

JNIEXPORT void JNICALL
Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeGoBack(
    JNIEnv *, jclass, jlong handle) {
    postBrowserOp(handle, [](CefRefPtr<CefBrowser> b) { b->GoBack(); });
}

JNIEXPORT void JNICALL
Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeGoForward(
    JNIEnv *, jclass, jlong handle) {
    postBrowserOp(handle, [](CefRefPtr<CefBrowser> b) { b->GoForward(); });
}

JNIEXPORT void JNICALL
Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeReload(
    JNIEnv *, jclass, jlong handle) {
    postBrowserOp(handle, [](CefRefPtr<CefBrowser> b) { b->Reload(); });
}

JNIEXPORT void JNICALL
Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeStopLoading(
    JNIEnv *, jclass, jlong handle) {
    postBrowserOp(handle, [](CefRefPtr<CefBrowser> b) { b->StopLoad(); });
}

JNIEXPORT jboolean JNICALL
Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeCanGoBack(
    JNIEnv *, jclass, jlong handle) {
    ComposeCefViewState *s = compose_cef_state_from_handle(handle);
    return (s != nullptr && s->can_go_back.load(std::memory_order_acquire)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeCanGoForward(
    JNIEnv *, jclass, jlong handle) {
    ComposeCefViewState *s = compose_cef_state_from_handle(handle);
    return (s != nullptr && s->can_go_forward.load(std::memory_order_acquire)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL
Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeCurrentUrl(
    JNIEnv *env, jclass, jlong handle) {
    ComposeCefViewState *s = compose_cef_state_from_handle(handle);
    if (s == nullptr) return nullptr;
    std::lock_guard<std::mutex> lock(s->mutex);
    if (s->current_url.empty()) return nullptr;
    return compose_cef_utf8_to_jstring(env, s->current_url);
}

JNIEXPORT jstring JNICALL
Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeGetTitle(
    JNIEnv *env, jclass, jlong handle) {
    ComposeCefViewState *s = compose_cef_state_from_handle(handle);
    if (s == nullptr) return nullptr;
    std::lock_guard<std::mutex> lock(s->mutex);
    if (s->title.empty()) return nullptr;
    return compose_cef_utf8_to_jstring(env, s->title);
}

JNIEXPORT jboolean JNICALL
Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeIsLoading(
    JNIEnv *, jclass, jlong handle) {
    ComposeCefViewState *s = compose_cef_state_from_handle(handle);
    return (s != nullptr && s->is_loading.load(std::memory_order_acquire)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeSetZoomLevel(
    JNIEnv *, jclass, jlong handle, jdouble zoom) {
    postBrowserOp(handle, [zoom](CefRefPtr<CefBrowser> b) {
        b->GetHost()->SetZoomLevel(zoom == 1.0 ? 0.0 : zoom);
    });
}

JNIEXPORT void JNICALL
Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeFocus(
    JNIEnv *, jclass, jlong handle) {
    postBrowserOp(handle, [](CefRefPtr<CefBrowser> b) { b->GetHost()->SetFocus(true); });
}

JNIEXPORT void JNICALL
Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeOpenDevTools(
    JNIEnv *, jclass, jlong handle) {
    postBrowserOp(handle, [](CefRefPtr<CefBrowser> b) {
        CefWindowInfo window_info;
        window_info.SetAsWindowless(0);
        CefBrowserSettings settings;
        b->GetHost()->ShowDevTools(window_info, nullptr, settings, CefPoint(0, 0));
    });
}

JNIEXPORT void JNICALL
Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeCloseDevTools(
    JNIEnv *, jclass, jlong handle) {
    postBrowserOp(handle, [](CefRefPtr<CefBrowser> b) { b->GetHost()->CloseDevTools(); });
}

}  // extern "C"
