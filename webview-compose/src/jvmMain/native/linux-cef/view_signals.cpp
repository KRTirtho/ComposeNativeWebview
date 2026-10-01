#include "compose_cef_internal.h"

#include "include/wrapper/cef_helpers.h"

/* CEF client: implements the handlers that surface navigation / loading /
 * title / IPC / context-menu state to Kotlin. All callbacks arrive on the CEF
 * UI thread (multi_threaded_message_loop); Kotlin-facing JNI calls are
 * dispatched directly (jni_bridge attaches the thread). */

namespace {

class NavigationHeadersHandler final : public CefResourceRequestHandler {
public:
    explicit NavigationHeadersHandler(CefRequest::HeaderMap headers)
        : headers_(std::move(headers)) {}

    ReturnValue OnBeforeResourceLoad(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                                     CefRefPtr<CefRequest> request,
                                     CefRefPtr<CefCallback>) override {
        CefRequest::HeaderMap merged;
        request->GetHeaderMap(merged);
        merged.insert(headers_.begin(), headers_.end());
        request->SetHeaderMap(merged);
        return RV_CONTINUE;
    }

private:
    CefRequest::HeaderMap headers_;
    IMPLEMENT_REFCOUNTING(NavigationHeadersHandler);
};

class ComposeCefClient final : public CefClient,
                               public CefRenderHandler,
                               public CefLifeSpanHandler,
                               public CefContextMenuHandler,
                               public CefDisplayHandler,
                               public CefLoadHandler,
                               public CefRequestHandler {
public:
    explicit ComposeCefClient(std::shared_ptr<ComposeCefViewState> state)
        : state_(std::move(state)) {
        CefMessageRouterConfig config;
        message_router_ = CefMessageRouterBrowserSide::Create(config);
    }

    CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override { return this; }
    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
    CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }

    /* ── CefRenderHandler (UI thread) ─────────────────────────────────── */
    void GetViewRect(CefRefPtr<CefBrowser>, CefRect &rect) override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        rect = CefRect(0, 0, std::max(1, state_->width), std::max(1, state_->height));
    }

    void OnPaint(
        CefRefPtr<CefBrowser>,
        PaintElementType type,
        const RectList &dirty_rects,
        const void *buffer,
        int width,
        int height) override {
        if (type != PET_VIEW || buffer == nullptr) return;
        compose_cef_on_paint(state_, buffer, width, height);
    }

    /* ── CefLifeSpanHandler ───────────────────────────────────────────── */
    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
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
        if (should_close) {
            browser->GetHost()->CloseBrowser(true);
        } else if (resize_after_create) {
            browser->GetHost()->WasResized();
        }
    }

    bool OnBeforePopup(
        CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame>, int,
        const CefString &target_url, const CefString &,
        CefLifeSpanHandler::WindowOpenDisposition,
        bool, const CefPopupFeatures &, CefWindowInfo &,
        CefRefPtr<CefClient> &, CefBrowserSettings &,
        CefRefPtr<CefDictionaryValue> &, bool *) override {
        // A popup needs its own OSR surface and GTK host. Letting CEF create
        // one without either produces unowned Chrome windows (often black).
        // Until popup hosting exists, keep the destination in this view.
        const std::string url = target_url.ToString();
        if (!url.empty() && url != "about:blank") {
            compose_cef_post_to_ui([browser, url] {
                browser->GetMainFrame()->LoadURL(CefString(url));
            });
        }
        return true;
    }

    void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
        message_router_->OnBeforeClose(browser);
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->browser = nullptr;
            state_->closed = true;
        }
        state_->closed_condition.notify_all();
    }

    /* ── CefContextMenuHandler ────────────────────────────────────────── */
    void OnBeforeContextMenu(
        CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
        CefRefPtr<CefContextMenuParams> params,
        CefRefPtr<CefMenuModel> model) override {
        compose_cef_prepare_context_menu(params, model);
    }

    bool RunContextMenu(
        CefRefPtr<CefBrowser> browser,
        CefRefPtr<CefFrame> frame,
        CefRefPtr<CefContextMenuParams> params,
        CefRefPtr<CefMenuModel> model,
        CefRefPtr<CefRunContextMenuCallback> callback) override {
        return compose_cef_run_context_menu(state_, browser, frame, model, callback, params);
    }

    bool OnContextMenuCommand(
        CefRefPtr<CefBrowser> browser,
        CefRefPtr<CefFrame> frame,
        CefRefPtr<CefContextMenuParams> params,
        int command_id, EventFlags) override {
        return compose_cef_handle_context_menu_command(state_, browser, frame, params, command_id);
    }

    /* ── CefDisplayHandler ────────────────────────────────────────────── */
    void OnAddressChange(
        CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, const CefString &url) override {
        if (!frame->IsMain()) return;
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->current_url = url.ToString();
    }

    void OnTitleChange(CefRefPtr<CefBrowser>, const CefString &title) override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->title = title.ToString();
    }

    bool OnConsoleMessage(
        CefRefPtr<CefBrowser>,
        cef_log_severity_t,
        const CefString &message,
        const CefString &,
        int) override {
        const std::string text = message.ToString();
        if (text.starts_with("__compose_cef_paste_ok:")) {
            if (g_getenv("COMPOSE_CEF_DEBUG_INPUT")) g_printerr("CEF paste result: %s\n", text.c_str());
            return true;
        }
        if (compose_cef_handle_clipboard_console(state_, text)) return true;
        static const std::string prefix = "__compose_cef_js:";
        if (text.compare(0, prefix.size(), prefix) == 0) {
            compose_cef_call_on_js_result(state_->handle, text.substr(prefix.size()));
            return true;
        }
        return false;
    }

    bool OnCursorChange(
        CefRefPtr<CefBrowser>,
        CefCursorHandle,
        cef_cursor_type_t,
        const CefCursorInfo &) override {
        // TODO: map the X11 cursor handle to a GdkCursor on the view window.
        return false;
    }

    /* ── CefLoadHandler ───────────────────────────────────────────────── */
    void OnLoadingStateChange(
        CefRefPtr<CefBrowser> browser,
        bool isLoading,
        bool canGoBack,
        bool canGoForward) override {
        state_->is_loading.store(isLoading, std::memory_order_release);
        state_->can_go_back.store(canGoBack, std::memory_order_release);
        state_->can_go_forward.store(canGoForward, std::memory_order_release);
    }

    void OnLoadEnd(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, int) override {
        if (!frame->IsMain()) return;
        std::string init_script;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            init_script = state_->init_script;
        }
        if (!init_script.empty()) frame->ExecuteJavaScript(init_script, frame->GetURL(), 0);
    }

    /* ── CefRequestHandler ────────────────────────────────────────────── */
    bool OnBeforeBrowse(
        CefRefPtr<CefBrowser> browser,
        CefRefPtr<CefFrame> frame,
        CefRefPtr<CefRequest> request,
        bool,
        bool) override {
        if (!frame->IsMain()) return false;
        message_router_->OnBeforeBrowse(browser, frame);
        return !compose_cef_call_on_navigate(
            state_->handle, request->GetURL().ToString());
    }

    CefRefPtr<CefResourceRequestHandler> GetResourceRequestHandler(
        CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame,
        CefRefPtr<CefRequest> request, bool is_navigation, bool,
        const CefString &, bool &) override {
        if (!is_navigation || frame == nullptr || !frame->IsMain()) return nullptr;
        CefRequest::HeaderMap headers;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            auto it = state_->navigation_headers.find(request->GetURL().ToString());
            if (it == state_->navigation_headers.end()) return nullptr;
            headers = std::move(it->second);
            state_->navigation_headers.erase(it);
        }
        return new NavigationHeadersHandler(std::move(headers));
    }

    bool OnProcessMessageReceived(
        CefRefPtr<CefBrowser> browser,
        CefRefPtr<CefFrame> frame,
        CefProcessId source_process,
        CefRefPtr<CefProcessMessage> message) override {
        if (source_process == PID_RENDERER && message->GetName() == "compose_cef_ipc") {
            compose_cef_call_on_ipc(state_->handle, message->GetArgumentList()->GetString(0));
            return true;
        }
        return message_router_->OnProcessMessageReceived(
            browser, frame, source_process, message);
    }

    void OnRenderProcessTerminated(
        CefRefPtr<CefBrowser> browser,
        TerminationStatus,
        int,
        const CefString &) override {
        message_router_->OnRenderProcessTerminated(browser);
    }

private:
    std::shared_ptr<ComposeCefViewState> state_;
    CefRefPtr<CefMessageRouterBrowserSide> message_router_;
    IMPLEMENT_REFCOUNTING(ComposeCefClient);
};

}  // namespace

CefRefPtr<CefClient> compose_cef_create_client(std::shared_ptr<ComposeCefViewState> state) {
    return static_cast<CefClient *>(new ComposeCefClient(std::move(state)));
}
