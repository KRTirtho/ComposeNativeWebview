#include "include/cef_app.h"
#include "include/wrapper/cef_message_router.h"

/* CEF subprocess (renderer / GPU / utility processes). Hosts the renderer-side
 * message router that injects window.cefQuery into every frame and forwards
 * page queries to the browser process. */

namespace {

class ComposeCefRenderApp final : public CefApp, public CefRenderProcessHandler {
public:
    ComposeCefRenderApp() = default;

    CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override { return this; }

    void OnWebKitInitialized() override {
        message_router_ = CefMessageRouterRendererSide::Create(CefMessageRouterConfig{});
    }

    void OnContextCreated(
        CefRefPtr<CefBrowser> browser,
        CefRefPtr<CefFrame> frame,
        CefRefPtr<CefV8Context> context) override {
        message_router_->OnContextCreated(browser, frame, context);
    }

    void OnContextReleased(
        CefRefPtr<CefBrowser> browser,
        CefRefPtr<CefFrame> frame,
        CefRefPtr<CefV8Context> context) override {
        message_router_->OnContextReleased(browser, frame, context);
    }

    bool OnProcessMessageReceived(
        CefRefPtr<CefBrowser> browser,
        CefRefPtr<CefFrame> frame,
        CefProcessId source_process,
        CefRefPtr<CefProcessMessage> message) override {
        return message_router_->OnProcessMessageReceived(
            browser, frame, source_process, message);
    }

private:
    CefRefPtr<CefMessageRouterRendererSide> message_router_;
    IMPLEMENT_REFCOUNTING(ComposeCefRenderApp);
};

}  // namespace

int main(int argc, char *argv[]) {
    CefMainArgs main_args(argc, argv);
    CefRefPtr<CefApp> app = new ComposeCefRenderApp();
    return CefExecuteProcess(main_args, app, nullptr);
}
