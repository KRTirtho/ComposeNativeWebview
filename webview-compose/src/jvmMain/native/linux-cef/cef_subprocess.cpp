#include "include/cef_app.h"
#include "include/cef_values.h"
#include "include/cef_v8.h"
#include "include/wrapper/cef_message_router.h"

#include <unordered_map>

int compose_cef_worker_main(int fd, const std::string &runtime, const std::string &cache);

/* CEF subprocess (renderer / GPU / utility processes). Hosts the renderer-side
 * message router that injects window.cefQuery into every frame and forwards
 * page queries to the browser process. */

namespace {

class IpcHandler final : public CefV8Handler {
public:
    bool Execute(const CefString &, CefRefPtr<CefV8Value>,
                 const CefV8ValueList &arguments, CefRefPtr<CefV8Value> &retval,
                 CefString &) override {
        if (arguments.size() != 1 || !arguments[0]->IsString()) return false;
        CefRefPtr<CefV8Context> context = CefV8Context::GetCurrentContext();
        if (context == nullptr) return false;
        CefRefPtr<CefProcessMessage> message = CefProcessMessage::Create("compose_cef_ipc");
        message->GetArgumentList()->SetString(0, arguments[0]->GetStringValue());
        context->GetFrame()->SendProcessMessage(PID_BROWSER, message);
        retval = CefV8Value::CreateUndefined();
        return true;
    }

private:
    IMPLEMENT_REFCOUNTING(IpcHandler);
};

class ComposeCefRenderApp final : public CefApp, public CefRenderProcessHandler {
public:
    ComposeCefRenderApp() = default;

    CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override { return this; }

    void OnWebKitInitialized() override {
        message_router_ = CefMessageRouterRendererSide::Create(CefMessageRouterConfig{});
    }

    void OnBrowserCreated(CefRefPtr<CefBrowser> browser,
                          CefRefPtr<CefDictionaryValue> extra_info) override {
        if (extra_info != nullptr)
            bridge_scripts_[browser->GetIdentifier()] =
                extra_info->GetString("bridge_script").ToString();
    }

    void OnBrowserDestroyed(CefRefPtr<CefBrowser> browser) override {
        bridge_scripts_.erase(browser->GetIdentifier());
    }

    void OnContextCreated(
        CefRefPtr<CefBrowser> browser,
        CefRefPtr<CefFrame> frame,
        CefRefPtr<CefV8Context> context) override {
        message_router_->OnContextCreated(browser, frame, context);
        CefRefPtr<CefV8Value> ipc = CefV8Value::CreateObject(nullptr, nullptr);
        ipc->SetValue("postMessage", CefV8Value::CreateFunction("postMessage", new IpcHandler()),
                      V8_PROPERTY_ATTRIBUTE_NONE);
        context->GetGlobal()->SetValue("ipc", ipc, V8_PROPERTY_ATTRIBUTE_NONE);
        auto script = bridge_scripts_.find(browser->GetIdentifier());
        if (script != bridge_scripts_.end() && !script->second.empty()) {
            CefRefPtr<CefV8Value> result;
            CefRefPtr<CefV8Exception> exception;
            context->Eval(script->second, frame->GetURL(), 0, result, exception);
        }
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
    std::unordered_map<int, std::string> bridge_scripts_;
    IMPLEMENT_REFCOUNTING(ComposeCefRenderApp);
};

}  // namespace

int main(int argc, char *argv[]) {
    if (argc == 4 && std::string(argv[1]) == "--compose-cef-host-fd=3") {
        return compose_cef_worker_main(3, argv[2], argv[3]);
    }
    CefMainArgs main_args(argc, argv);
    CefRefPtr<CefApp> app = new ComposeCefRenderApp();
    return CefExecuteProcess(main_args, app, nullptr);
}
