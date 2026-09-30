#include "compose_cef_internal.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>

/* Process-level CEF lifecycle (multi_threaded_message_loop). CEF owns its UI
 * thread; we just initialize/shutdown once per process and expose helpers to
 * post work onto TID_UI. */

namespace {

class FunctionTask final : public CefTask {
public:
    explicit FunctionTask(std::function<void()> fn) : fn_(std::move(fn)) {}
    void Execute() override { fn_(); }

private:
    std::function<void()> fn_;
    IMPLEMENT_REFCOUNTING(FunctionTask);
};

class ComposeCefApp final : public CefApp, public CefBrowserProcessHandler {
public:
    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }

    void OnBeforeCommandLineProcessing(
        const CefString &process_type,
        CefRefPtr<CefCommandLine> command_line) override {
        command_line->AppendSwitch("disable-gpu");
        command_line->AppendSwitchWithValue("disable-features", "Vulkan");
    }

private:
    IMPLEMENT_REFCOUNTING(ComposeCefApp);
};

std::mutex g_cef_mutex;
bool g_cef_initialized = false;
CefRefPtr<CefApp> g_cef_app;
int g_live_views = 0;
char g_cef_argv0[] = "composewebview-cef";
char *g_cef_argv[] = {g_cef_argv0, nullptr};

/* NativeLibraryLoader extracts sidecars from the JAR without Unix mode bits,
 * so cef_subprocess may not be executable. CEF only execs the subprocess path,
 * never dlopens it, so point it at a tiny +x shim that execs the real helper. */
std::string ensureSubprocessShim(const std::string &runtime_dir) {
    const std::string real = runtime_dir + "/cef_subprocess";
    const std::string shim = runtime_dir + "/cef_subprocess.sh";
    if (g_file_test(shim.c_str(), G_FILE_TEST_EXISTS)) return shim;
    const std::string contents =
        "#!/bin/sh\nchmod 0755 \"" + real + "\" 2>/dev/null\nexec \"" + real + "\" \"$@\"\n";
    if (g_file_set_contents(
            shim.c_str(), contents.data(),
            static_cast<gssize>(contents.size()), nullptr)) {
        chmod(shim.c_str(), 0755);
    }
    return shim;
}

/* The locale pack is staged flat (en-US.pak) because JAR sidecars cannot be
 * nested; CEF expects <locales_dir>/en-US.pak. Hard-link (fallback: copy). */
void ensureLocales(const std::string &runtime_dir) {
    const std::string locales_dir = runtime_dir + "/locales";
    g_mkdir_with_parents(locales_dir.c_str(), 0755);
    const std::string src = runtime_dir + "/en-US.pak";
    const std::string dst = locales_dir + "/en-US.pak";
    if (g_file_test(src.c_str(), G_FILE_TEST_EXISTS) &&
        !g_file_test(dst.c_str(), G_FILE_TEST_EXISTS)) {
        if (link(src.c_str(), dst.c_str()) != 0) {
            char *data = nullptr;
            gsize length = 0;
            if (g_file_get_contents(src.c_str(), &data, &length, nullptr)) {
                g_file_set_contents(dst.c_str(), data, static_cast<gssize>(length), nullptr);
                g_free(data);
            }
        }
    }
}

}  // namespace

void compose_cef_post_to_ui(std::function<void()> fn) {
    CefPostTask(TID_UI, new FunctionTask(std::move(fn)));
}

bool compose_cef_initialize(const std::string &runtime_dir, const std::string &cache_dir) {
    std::lock_guard<std::mutex> lock(g_cef_mutex);
    if (g_cef_initialized) return true;

    ensureLocales(runtime_dir);
    g_mkdir_with_parents(cache_dir.c_str(), 0700);

    CefMainArgs main_args(1, g_cef_argv);
    CefSettings settings;
    settings.no_sandbox = true;
    // HotSpot uses SIGSEGV for implicit null checks. CEF must not replace the
    // JVM's signal handler, or ordinary Java null checks can kill the process.
    settings.disable_signal_handlers = true;
    settings.multi_threaded_message_loop = true;
    settings.windowless_rendering_enabled = true;
    CefString(&settings.browser_subprocess_path).FromString(ensureSubprocessShim(runtime_dir));
    CefString(&settings.resources_dir_path).FromString(runtime_dir);
    CefString(&settings.locales_dir_path).FromString(runtime_dir + "/locales");
    CefString(&settings.root_cache_path).FromString(cache_dir);
    CefString(&settings.cache_path).FromString(cache_dir + "/profile");
    CefString(&settings.log_file).FromString(cache_dir + "/cef.log");

    g_cef_app = new ComposeCefApp();
    g_cef_initialized = CefInitialize(main_args, settings, g_cef_app, nullptr);
    return g_cef_initialized;
}

void compose_cef_note_view_created() {
    std::lock_guard<std::mutex> lock(g_cef_mutex);
    g_live_views++;
}

void compose_cef_note_view_released() {
    bool do_shutdown = false;
    {
        std::lock_guard<std::mutex> lock(g_cef_mutex);
        g_live_views--;
        do_shutdown = g_cef_initialized && g_live_views <= 0;
    }
    if (!do_shutdown) return;
    /* Synchronous shutdown on the releasing thread, before the JVM starts
     * tearing down native libraries. Mirrors the proven prototype shutdown. */
    std::lock_guard<std::mutex> lock(g_cef_mutex);
    if (g_cef_initialized && g_live_views <= 0) {
        CefShutdown();
        g_cef_initialized = false;
        g_cef_app = nullptr;
    }
}
