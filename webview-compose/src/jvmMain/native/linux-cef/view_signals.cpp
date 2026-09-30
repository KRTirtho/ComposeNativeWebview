#include "compose_cef_internal.h"

#include "include/wrapper/cef_helpers.h"

/* CEF client: implements the handlers that surface navigation / loading /
 * title / IPC / context-menu state to Kotlin. All callbacks arrive on the CEF
 * UI thread (multi_threaded_message_loop); Kotlin-facing JNI calls are
 * dispatched directly (jni_bridge attaches the thread). */

namespace {

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

    void OnBeforeClose(CefRefPtr<CefBrowser>) override {
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->browser = nullptr;
            state_->closed = true;
        }
        state_->closed_condition.notify_all();
    }

    /* ── CefContextMenuHandler ────────────────────────────────────────── */
    bool RunContextMenu(
        CefRefPtr<CefBrowser>,
        CefRefPtr<CefFrame>,
        CefRefPtr<CefContextMenuParams> params,
        CefRefPtr<CefMenuModel> model,
        CefRefPtr<CefRunContextMenuCallback> callback) override {
        return compose_cef_run_context_menu(state_, model, callback, params);
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
        std::string bridge_script;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            init_script = state_->init_script;
            bridge_script = state_->js_bridge_script;
        }
        if (!init_script.empty()) frame->ExecuteJavaScript(init_script, frame->GetURL(), 0);
        if (!bridge_script.empty()) frame->ExecuteJavaScript(bridge_script, frame->GetURL(), 0);
    }

    /* ── CefRequestHandler ────────────────────────────────────────────── */
    bool OnBeforeBrowse(
        CefRefPtr<CefBrowser>,
        CefRefPtr<CefFrame> frame,
        CefRefPtr<CefRequest> request,
        bool,
        bool) override {
        if (!frame->IsMain()) return false;
        message_router_->OnBeforeBrowse(nullptr, frame);
        return !compose_cef_call_on_navigate(
            state_->handle, request->GetURL().ToString());
    }

    bool OnProcessMessageReceived(
        CefRefPtr<CefBrowser> browser,
        CefRefPtr<CefFrame> frame,
        CefProcessId source_process,
        CefRefPtr<CefProcessMessage> message) override {
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
