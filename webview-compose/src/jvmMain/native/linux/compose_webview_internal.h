/**
 * Shared state and JNI helpers for the Linux WebKit2GTK backend.
 * Not a public API — only used by the compose_webview_*.c units.
 */
#ifndef COMPOSE_WEBVIEW_INTERNAL_H
#define COMPOSE_WEBVIEW_INTERNAL_H

#include <jni.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <gtk/gtk.h>
#include <cairo.h>
#include <webkit2/webkit2.h>
#include <libsoup/soup.h>
#include <jsc/jsc.h>

typedef struct {
    WebKitWebView *web_view;
    WebKitUserContentManager *ucm;
    WebKitWebContext *context;
    gulong decide_policy_handler;
    gulong ipc_handler;
    gulong context_menu_handler;
    gulong hierarchy_handler;
    GtkWidget *host_window;
    gulong host_event_handler;
    GtkGesture *host_pointer_gesture;
    GtkWidget *context_popover;
    gboolean event_filter_installed;
} ComposeWebViewState;

/* jni_bridge.c */
JNIEnv *compose_webview_get_env(void);
void compose_webview_ensure_bridge_methods(JNIEnv *env);
jclass compose_webview_bridge_class(void);
jmethodID compose_webview_on_navigate(void);
jmethodID compose_webview_on_ipc(void);
jmethodID compose_webview_on_js_result(void);
jmethodID compose_webview_on_cookies(void);
jmethodID compose_webview_on_screenshot(void);

/* view_lifecycle.c */
ComposeWebViewState *compose_webview_state_from_handle(jlong handle);
jlong compose_webview_handle_from_view(WebKitWebView *view);

/* view_signals.c — wired during create */
gboolean compose_webview_on_decide_policy(
    WebKitWebView *web_view,
    WebKitPolicyDecision *decision,
    WebKitPolicyDecisionType type,
    gpointer user_data);
void compose_webview_on_script_message(
    WebKitUserContentManager *manager,
    WebKitJavascriptResult *js_result,
    gpointer user_data);
void compose_webview_connect_input_routing(ComposeWebViewState *state);
void compose_webview_disconnect_input_routing(ComposeWebViewState *state);
gboolean compose_webview_on_context_menu(
    WebKitWebView *web_view,
    WebKitContextMenu *context_menu,
    GdkEvent *event,
    WebKitHitTestResult *hit_test_result,
    gpointer user_data);

#endif /* COMPOSE_WEBVIEW_INTERNAL_H */
