#include "cef_host_internal.h"

namespace {

JavaVM *g_jvm = nullptr;
jclass g_bridge_class = nullptr;
jmethodID g_on_navigate = nullptr;
jmethodID g_on_ipc = nullptr;
jmethodID g_on_js_result = nullptr;
jmethodID g_on_cookies = nullptr;
jmethodID g_on_screenshot = nullptr;
jmethodID g_on_pointer_focus = nullptr;

}  // namespace

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *) {
    g_jvm = vm;
    return JNI_VERSION_1_8;
}

JNIEnv *compose_cef_get_env() {
    if (g_jvm == nullptr) return nullptr;
    JNIEnv *env = nullptr;
    jint status = g_jvm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_8);
    if (status == JNI_EDETACHED) {
        // CEF's native threads live with the process, not a screen. Their JNI
        // callbacks must not keep the JVM alive after the application exits.
        if (g_jvm->AttachCurrentThreadAsDaemon(reinterpret_cast<void **>(&env), nullptr) != 0) {
            return nullptr;
        }
    } else if (status != JNI_OK) {
        return nullptr;
    }
    return env;
}

void compose_cef_ensure_bridge_methods(JNIEnv *env) {
    if (g_bridge_class != nullptr || env == nullptr) return;
    jclass local = env->FindClass("dev/nucleusframework/webview/web/linux/CefLinuxBridge");
    if (local == nullptr) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return;
    }
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
    g_on_pointer_focus = env->GetStaticMethodID(
        g_bridge_class, "nativeOnPointerFocus", "(J)V");
}

void compose_cef_call_on_pointer_focus(jlong handle) {
    JNIEnv *env = compose_cef_get_env();
    if (env == nullptr || g_bridge_class == nullptr || g_on_pointer_focus == nullptr) return;
    env->CallStaticVoidMethod(g_bridge_class, g_on_pointer_focus, handle);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

bool compose_cef_call_on_navigate(jlong handle, const std::string &url) {
    JNIEnv *env = compose_cef_get_env();
    if (env == nullptr) return true;
    compose_cef_ensure_bridge_methods(env);
    if (g_bridge_class == nullptr || g_on_navigate == nullptr) return true;
    jstring jurl = env->NewStringUTF(url.c_str());
    jboolean allow = env->CallStaticBooleanMethod(g_bridge_class, g_on_navigate, handle, jurl);
    env->DeleteLocalRef(jurl);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return true;
    }
    return allow == JNI_TRUE;
}

void compose_cef_call_on_ipc(jlong handle, const std::string &utf8) {
    JNIEnv *env = compose_cef_get_env();
    if (env == nullptr) return;
    compose_cef_ensure_bridge_methods(env);
    if (g_bridge_class == nullptr || g_on_ipc == nullptr) return;
    jstring j = env->NewStringUTF(utf8.c_str());
    env->CallStaticVoidMethod(g_bridge_class, g_on_ipc, handle, j);
    env->DeleteLocalRef(j);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

void compose_cef_call_on_js_result(jlong handle, const std::string &utf8) {
    JNIEnv *env = compose_cef_get_env();
    if (env == nullptr) return;
    compose_cef_ensure_bridge_methods(env);
    if (g_bridge_class == nullptr || g_on_js_result == nullptr) return;
    jstring j = env->NewStringUTF(utf8.c_str());
    env->CallStaticVoidMethod(g_bridge_class, g_on_js_result, handle, j);
    env->DeleteLocalRef(j);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

void compose_cef_call_on_cookies(jlong handle, const std::string &json) {
    JNIEnv *env = compose_cef_get_env();
    if (env == nullptr) return;
    compose_cef_ensure_bridge_methods(env);
    if (g_bridge_class == nullptr || g_on_cookies == nullptr) return;
    jstring j = env->NewStringUTF(json.c_str());
    env->CallStaticVoidMethod(g_bridge_class, g_on_cookies, handle, j);
    env->DeleteLocalRef(j);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

void compose_cef_call_on_screenshot(jlong handle, const std::vector<uint8_t> &png) {
    JNIEnv *env = compose_cef_get_env();
    if (env == nullptr) return;
    compose_cef_ensure_bridge_methods(env);
    if (g_bridge_class == nullptr || g_on_screenshot == nullptr) return;
    jbyteArray arr = nullptr;
    if (!png.empty()) {
        arr = env->NewByteArray(static_cast<jsize>(png.size()));
        if (arr != nullptr) {
            env->SetByteArrayRegion(
                arr, 0, static_cast<jsize>(png.size()),
                reinterpret_cast<const jbyte *>(png.data()));
        }
    }
    env->CallStaticVoidMethod(g_bridge_class, g_on_screenshot, handle, arr);
    if (arr != nullptr) env->DeleteLocalRef(arr);
    if (env->ExceptionCheck()) env->ExceptionClear();
}
