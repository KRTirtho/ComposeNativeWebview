#include "compose_cef_internal.h"

#include "include/wrapper/cef_message_router.h"

/* JS bridge. The canonical CEF pattern is CefMessageRouterBrowserSide +
 * CefMessageRouterRendererSide (window.cefQuery). The renderer side runs in
 * the CEF subprocess (cef_subprocess.cpp). evaluateJavaScript results are
 * shipped back via a reserved console.log prefix (see
 * view_signals.cpp OnConsoleMessage). */

namespace {

std::string base64Encode(const std::string &in) {
    static const char *table =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    while (i < in.size()) {
        const size_t remaining = in.size() - i;
        const uint32_t a = static_cast<uint8_t>(in[i++]);
        const uint32_t b = remaining > 1 ? static_cast<uint8_t>(in[i++]) : 0;
        const uint32_t c = remaining > 2 ? static_cast<uint8_t>(in[i++]) : 0;
        const uint32_t triple = (a << 16) | (b << 8) | c;
        out.push_back(table[(triple >> 18) & 0x3F]);
        out.push_back(table[(triple >> 12) & 0x3F]);
        out.push_back(remaining > 1 ? table[(triple >> 6) & 0x3F] : '=');
        out.push_back(remaining > 2 ? table[triple & 0x3F] : '=');
    }
    return out;
}

constexpr char kJsResultPrefix[] = "__compose_cef_js:";

}  // namespace

extern "C" {

JNIEXPORT void JNICALL
Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeEvaluateJavaScript(
    JNIEnv *env, jclass, jlong handle, jstring script) {
    const std::string code = compose_cef_jstring_to_utf8(env, script);
    std::shared_ptr<ComposeCefViewState> state = compose_cef_shared_from_handle(handle);
    if (state == nullptr || code.empty()) {
        compose_cef_call_on_js_result(handle, "");
        return;
    }
    CefRefPtr<CefBrowser> browser;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        browser = state->browser;
    }
    if (browser.get() == nullptr) {
        compose_cef_call_on_js_result(handle, "");
        return;
    }
    const std::string wrapped =
        "(function(){var s=decodeURIComponent(escape(atob(\"" + base64Encode(code) +
        "\")));var r;try{r=String(eval(s));}catch(e){r='';}"
        "console.log(\"" + std::string(kJsResultPrefix) + "\"+r);})();";
    compose_cef_post_to_ui([browser, wrapped] {
        browser->GetMainFrame()->ExecuteJavaScript(
            CefString(wrapped), browser->GetMainFrame()->GetURL(), 0);
    });
}

}  // extern "C"
