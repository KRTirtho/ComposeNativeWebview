#include "include/cef_app.h"

namespace {
class WebViewCefApp final : public CefApp {
 public:
  WebViewCefApp() = default;

 private:
  IMPLEMENT_REFCOUNTING(WebViewCefApp);
};
}  // namespace

int main(int argc, char* argv[]) {
  CefMainArgs main_args(argc, argv);
  CefRefPtr<CefApp> app = new WebViewCefApp();
  return CefExecuteProcess(main_args, app, nullptr);
}
