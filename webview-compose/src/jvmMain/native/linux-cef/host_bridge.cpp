#include "cef_host_internal.h"
using namespace cefipc;
namespace {
void send(jlong id, Op op, const Writer &w = {}) {
    auto state = compose_cef_shared_from_handle(id);
    if (state && state->browser && !state->closing) state->browser->send(op, w);
}
std::string encode(const std::string &s) {
    static const char *hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (g_ascii_isalnum(c) || c == '-' || c == '_' || c == '.') out += c;
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}
}
extern "C" {
JNIEXPORT jboolean JNICALL Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeIsReady(
    JNIEnv *, jclass, jlong id) {
    auto state = compose_cef_shared_from_handle(id);
    if (!state) return JNI_FALSE;
    std::lock_guard lock(state->mutex);
    return state->browser && !state->closing && !state->closed ? JNI_TRUE : JNI_FALSE;
}
JNIEXPORT void JNICALL Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeLoadUrl(
    JNIEnv *env, jclass, jlong id, jstring url) { Writer w; w.text(compose_cef_jstring_to_utf8(env, url)); w.value<uint32_t>(0); send(id, Op::Navigate, w); }
JNIEXPORT void JNICALL Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeLoadHtml(
    JNIEnv *env, jclass, jlong id, jstring html, jstring) {
    Writer w; w.text("data:text/html;charset=utf-8," + encode(compose_cef_jstring_to_utf8(env, html)));
    w.value<uint32_t>(0); send(id, Op::Navigate, w);
}
JNIEXPORT void JNICALL Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeLoadUrlWithHeaders(
    JNIEnv *env, jclass, jlong id, jstring url, jobjectArray names, jobjectArray values) {
    Writer w; w.text(compose_cef_jstring_to_utf8(env, url));
    int n = names && values ? std::min(env->GetArrayLength(names), env->GetArrayLength(values)) : 0;
    w.value<uint32_t>(n);
    for (int i = 0; i < n; ++i) {
        auto name = static_cast<jstring>(env->GetObjectArrayElement(names, i));
        auto value = static_cast<jstring>(env->GetObjectArrayElement(values, i));
        w.text(compose_cef_jstring_to_utf8(env, name)); w.text(compose_cef_jstring_to_utf8(env, value));
        env->DeleteLocalRef(name); env->DeleteLocalRef(value);
    }
    send(id, Op::Navigate, w);
}
#define ACTION(Name, Operation) JNIEXPORT void JNICALL Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_native##Name(JNIEnv *, jclass, jlong id) { send(id, Op::Operation); }
ACTION(GoBack, Back) ACTION(GoForward, Forward) ACTION(Reload, Reload) ACTION(StopLoading, StopLoad)
ACTION(RemoveAllCookies, ClearCookies)
ACTION(OpenDevTools, OpenTools) ACTION(CloseDevTools, CloseTools)
#undef ACTION
JNIEXPORT void JNICALL Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeFocus(JNIEnv *, jclass, jlong id) {
    auto state = compose_cef_shared_from_handle(id); if (state && state->widget) gtk_widget_grab_focus(state->widget);
    Writer w; w.integer(1); send(id, Op::Focus, w);
}
JNIEXPORT void JNICALL Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeSetZoomLevel(JNIEnv *, jclass, jlong id, jdouble zoom) {
    Writer w; w.number(zoom); send(id, Op::Zoom, w);
}
#define BOOL_GETTER(Name, Field) JNIEXPORT jboolean JNICALL Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_native##Name(JNIEnv *, jclass, jlong id) { auto s = compose_cef_shared_from_handle(id); return s && s->Field.load() ? JNI_TRUE : JNI_FALSE; }
BOOL_GETTER(CanGoBack, can_go_back) BOOL_GETTER(CanGoForward, can_go_forward) BOOL_GETTER(IsLoading, is_loading)
#undef BOOL_GETTER
#define STRING_GETTER(Name, Field) JNIEXPORT jstring JNICALL Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_native##Name(JNIEnv *env, jclass, jlong id) { auto s = compose_cef_shared_from_handle(id); if (!s) return nullptr; std::lock_guard lock(s->mutex); return compose_cef_utf8_to_jstring(env, s->Field); }
STRING_GETTER(CurrentUrl, current_url) STRING_GETTER(GetTitle, title)
#undef STRING_GETTER
JNIEXPORT void JNICALL Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeEvaluateJavaScript(JNIEnv *env, jclass, jlong id, jstring script) {
    Writer w; w.text(compose_cef_jstring_to_utf8(env, script)); send(id, Op::Eval, w);
}
JNIEXPORT void JNICALL Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeGetCookies(JNIEnv *env, jclass, jlong id, jstring url) {
    Writer w; w.text(compose_cef_jstring_to_utf8(env, url)); send(id, Op::GetCookies, w);
}
JNIEXPORT void JNICALL Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeRemoveCookiesForUrl(JNIEnv *env, jclass, jlong id, jstring url) {
    Writer w; w.text(compose_cef_jstring_to_utf8(env, url)); send(id, Op::RemoveCookies, w);
}
JNIEXPORT void JNICALL Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeSetCookie(JNIEnv *env, jclass, jlong id,
    jstring name, jstring value, jstring domain, jstring path, jboolean secure, jboolean httpOnly, jlong expires, jstring sameSite) {
    Writer w; w.text(compose_cef_jstring_to_utf8(env, name)); w.text(compose_cef_jstring_to_utf8(env, value));
    w.text(compose_cef_jstring_to_utf8(env, domain)); w.text(compose_cef_jstring_to_utf8(env, path));
    w.integer(secure); w.integer(httpOnly); w.value<int64_t>(expires); w.text(compose_cef_jstring_to_utf8(env, sameSite));
    send(id, Op::SetCookie, w);
}
}
