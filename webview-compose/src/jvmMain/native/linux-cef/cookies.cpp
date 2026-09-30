#include "compose_cef_internal.h"

#include "include/cef_cookie.h"

/* Cookie access via CefCookieManager::GetGlobalManager. All callbacks arrive
 * on the CEF UI thread; results are pushed to Kotlin via jni_bridge. */

namespace {

std::string cookieToJson(const CefCookie &cookie) {
    const std::string name = CefString(&cookie.name).ToString();
    const std::string value = CefString(&cookie.value).ToString();
    const std::string domain = CefString(&cookie.domain).ToString();
    const std::string path = CefString(&cookie.path).ToString();
    std::string out = "{\"name\":\"" + compose_cef_json_escape(name) +
                      "\",\"value\":\"" + compose_cef_json_escape(value) +
                      "\",\"domain\":\"" + compose_cef_json_escape(domain) +
                      "\",\"path\":\"" + compose_cef_json_escape(path) + "\"";
    out += std::string(",\"secure\":") + (cookie.secure ? "true" : "false");
    out += std::string(",\"httpOnly\":") + (cookie.httponly ? "true" : "false");
    out += std::string(",\"sessionOnly\":") + (cookie.has_expires ? "false" : "true");
    out += ",\"expiresDate\":";
    if (cookie.has_expires) {
        // cef_basetime_t: microseconds since the Windows epoch (1601).
        out += std::to_string((cookie.expires.val - 11644473600000000LL) / 1000);
    } else {
        out += "0";
    }
    const char *same_site = "Lax";
    if (cookie.same_site == CEF_COOKIE_SAME_SITE_NO_RESTRICTION) same_site = "None";
    else if (cookie.same_site == CEF_COOKIE_SAME_SITE_STRICT_MODE) same_site = "Strict";
    out += std::string(",\"sameSite\":\"") + same_site + "\"}";
    return out;
}

class CookieVisitor final : public CefCookieVisitor {
public:
    explicit CookieVisitor(jlong handle) : handle_(handle) { json_ = "["; }

    bool Visit(const CefCookie &cookie, int count, int, bool &) override {
        if (count > 0) json_ += ",";
        json_ += cookieToJson(cookie);
        return true;
    }

    ~CookieVisitor() override {
        json_ += "]";
        compose_cef_call_on_cookies(handle_, json_);
    }

private:
    jlong handle_;
    std::string json_;
    IMPLEMENT_REFCOUNTING(CookieVisitor);
};

void withCookieManager(std::function<void(CefRefPtr<CefCookieManager>)> op) {
    compose_cef_post_to_ui([op = std::move(op)] {
        CefRefPtr<CefCookieManager> manager = CefCookieManager::GetGlobalManager(nullptr);
        if (manager.get() != nullptr) op(manager);
    });
}

}  // namespace

extern "C" {

JNIEXPORT void JNICALL
Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeGetCookies(
    JNIEnv *env, jclass, jlong handle, jstring url) {
    const std::string u = compose_cef_jstring_to_utf8(env, url);
    CefRefPtr<CookieVisitor> visitor = new CookieVisitor(handle);
    withCookieManager([u, visitor](CefRefPtr<CefCookieManager> manager) {
        manager->VisitUrlCookies(CefString(u), true, visitor);
    });
}

JNIEXPORT void JNICALL
Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeSetCookie(
    JNIEnv *env,
    jclass,
    jlong handle,
    jstring name,
    jstring value,
    jstring domain,
    jstring path,
    jboolean secure,
    jboolean httpOnly,
    jlong expiresMs,
    jstring sameSite) {
    CefCookie cookie;
    CefString(&cookie.name) = compose_cef_jstring_to_utf8(env, name);
    CefString(&cookie.value) = compose_cef_jstring_to_utf8(env, value);
    const std::string dom = compose_cef_jstring_to_utf8(env, domain);
    if (!dom.empty()) CefString(&cookie.domain) = dom;
    const std::string p = compose_cef_jstring_to_utf8(env, path);
    CefString(&cookie.path) = p.empty() ? "/" : p;
    cookie.secure = secure == JNI_TRUE;
    cookie.httponly = httpOnly == JNI_TRUE;
    if (expiresMs > 0) {
        cookie.has_expires = true;
        cookie.expires.val = 11644473600000000LL + expiresMs * 1000LL;
    }
    const std::string ss = compose_cef_jstring_to_utf8(env, sameSite);
    if (ss == "None") cookie.same_site = CEF_COOKIE_SAME_SITE_NO_RESTRICTION;
    else if (ss == "Strict") cookie.same_site = CEF_COOKIE_SAME_SITE_STRICT_MODE;
    else if (ss == "Lax") cookie.same_site = CEF_COOKIE_SAME_SITE_LAX_MODE;
    else cookie.same_site = CEF_COOKIE_SAME_SITE_UNSPECIFIED;

    const std::string url = dom.empty() ? "" : (cookie.secure ? "https://" : "http://") + dom;
    withCookieManager([url, cookie](CefRefPtr<CefCookieManager> manager) {
        manager->SetCookie(CefString(url), cookie, nullptr);
    });
}

JNIEXPORT void JNICALL
Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeRemoveAllCookies(
    JNIEnv *, jclass, jlong /*handle*/) {
    withCookieManager([](CefRefPtr<CefCookieManager> manager) {
        manager->DeleteCookies(CefString(), CefString(), nullptr);
    });
}

JNIEXPORT void JNICALL
Java_dev_nucleusframework_webview_web_linux_CefLinuxBridge_nativeRemoveCookiesForUrl(
    JNIEnv *env, jclass, jlong /*handle*/, jstring url) {
    const std::string u = compose_cef_jstring_to_utf8(env, url);
    withCookieManager([u](CefRefPtr<CefCookieManager> manager) {
        manager->DeleteCookies(CefString(u), CefString(), nullptr);
    });
}

}  // extern "C"
