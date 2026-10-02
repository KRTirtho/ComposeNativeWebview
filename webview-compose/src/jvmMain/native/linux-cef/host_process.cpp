#include "cef_host_internal.h"

#include <spawn.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <signal.h>
#include <chrono>
#include <thread>
#include <cstdio>
#include <filesystem>

extern char **environ;
using namespace cefipc;

namespace {
std::mutex pool_mutex;
std::map<std::string, std::shared_ptr<CefHostProcess>> pool;
pid_t waitChild(pid_t pid, int *status, int options) {
    pid_t result;
    do { result = waitpid(pid, status, options); } while (result < 0 && errno == EINTR);
    return result;
}
void clipboard(const std::string &text) {
    if (text.empty()) return;
    auto *p = new std::string(text);
    g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer p) -> gboolean {
        const auto &s = *static_cast<std::string *>(p);
        gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), s.data(), s.size());
        return G_SOURCE_REMOVE;
    }, p, [](gpointer p) { delete static_cast<std::string *>(p); });
}
}

class CefHostProcess {
public:
    std::string key;
    int fd = -1;
    pid_t pid = -1;
    std::mutex write_mutex, views_mutex, ready_mutex;
    std::condition_variable ready_condition;
    std::map<uint64_t, std::weak_ptr<ComposeCefViewState>> views;
    std::thread reader;
    bool ready = false, failed = false;
    std::atomic<bool> stopped{false};

    ~CefHostProcess() { stop(); }
    bool healthy() {
        std::lock_guard lock(ready_mutex);
        return ready && !failed && !stopped;
    }
    bool start(const std::string &runtime, const std::string &cache) {
        int sockets[2];
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) != 0) return false;
        std::string executable = runtime + "/cef_subprocess";
        // Extracted sidecars have no executable bit. No shell, socket path,
        // TCP listener, or inherited descriptor is needed for this private IPC.
        chmod(executable.c_str(), 0755);
        std::string arg = "--compose-cef-host-fd=3";
        char *args[] = {executable.data(), arg.data(), const_cast<char *>(runtime.c_str()),
                        const_cast<char *>(cache.c_str()), nullptr};
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, sockets[1], 3);
        if (sockets[0] != 3) posix_spawn_file_actions_addclose(&actions, sockets[0]);
        if (sockets[1] != 3) posix_spawn_file_actions_addclose(&actions, sockets[1]);
        posix_spawnattr_t attributes;
        posix_spawnattr_init(&attributes);
        posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
        posix_spawnattr_setpgroup(&attributes, 0);
        int error = posix_spawn(&pid, executable.c_str(), &actions, &attributes, args, environ);
        posix_spawnattr_destroy(&attributes);
        posix_spawn_file_actions_destroy(&actions);
        close(sockets[1]);
        if (error) { close(sockets[0]); pid = -1; return false; }
        fd = sockets[0];
        reader = std::thread([this] { read(); });
        std::unique_lock lock(ready_mutex);
        ready_condition.wait_for(lock, std::chrono::seconds(30), [this] { return ready || failed; });
        return ready && !failed;
    }
    void send(Op op, uint64_t id, const Writer &w = {}) {
        std::lock_guard lock(write_mutex);
        if (!stopped && fd >= 0 && !sendPacket(fd, op, id, w.data)) {
            ::shutdown(fd, SHUT_RDWR);
        }
    }
    void add(const std::shared_ptr<ComposeCefViewState> &state, const ComposeCefCreateOptions &o) {
        { std::lock_guard lock(views_mutex); views[state->handle] = state; }
        Writer w;
        w.text(o.initialUrl); w.text(o.userAgent); w.text(o.initScript); w.text(o.jsBridgeScript);
        w.integer(o.incognito); w.integer(o.javascriptEnabled); w.number(o.zoomLevel);
        w.integer(o.transparent); w.number(o.bgR); w.number(o.bgG); w.number(o.bgB); w.number(o.bgA);
        send(Op::Create, state->handle, w);
    }
    void release(uint64_t id) {
        std::shared_ptr<ComposeCefViewState> state;
        { std::lock_guard lock(views_mutex); auto i = views.find(id); if (i != views.end()) state = i->second.lock(); }
        send(Op::Close, id);
        if (state) {
            std::unique_lock lock(state->mutex);
            if (!state->closed_condition.wait_for(lock, std::chrono::seconds(10), [&] { return state->closed; })) {
                g_printerr("CEF host: browser close timed out; terminating host\n");
                lock.unlock(); stop();
            }
        }
        bool empty;
        { std::lock_guard lock(views_mutex); views.erase(id); empty = views.empty(); }
        if (empty) {
            // Serialize the last release with acquisition: a newly-opened view
            // either joins a live host or launches a fresh process after reaping.
            std::lock_guard lock(pool_mutex);
            { std::lock_guard vlock(views_mutex); empty = views.empty(); }
            if (empty) {
                stop();
                auto i = pool.find(key);
                if (i != pool.end() && i->second.get() == this) pool.erase(i);
            }
        }
    }
    void stop() {
        if (stopped.exchange(true)) return;
        {
            std::lock_guard lock(write_mutex);
            if (fd >= 0) sendPacket(fd, Op::Stop, 0);
        }
        if (pid > 0) {
            int status = 0;
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (waitChild(pid, &status, WNOHANG) == 0) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    // Kill only this host's process group, never other apps'
                    // Chromium processes. Always reap our direct child.
                    kill(-pid, SIGKILL); waitChild(pid, &status, 0); break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            // CefShutdown normally reaps its children. Clean up any remaining
            // members of our dedicated group before declaring shutdown done.
            if (kill(-pid, 0) == 0) kill(-pid, SIGKILL);
            if (g_getenv("COMPOSE_CEF_DEBUG_PROCESS")) g_printerr("CEF host stopped and reaped: pid=%d\n", pid);
            pid = -1;
        }
        if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
        if (reader.joinable()) reader.join();
        if (fd >= 0) { close(fd); fd = -1; }
    }
    void read() {
        Packet p;
        try {
            while (receivePacket(fd, p)) {
                Reader r{p.data};
                if (p.op == Op::Ready) {
                    std::lock_guard lock(ready_mutex);
                    ready = r.integer() != 0; failed = !ready; ready_condition.notify_all(); continue;
                }
                std::shared_ptr<ComposeCefViewState> state;
                { std::lock_guard lock(views_mutex); auto i = views.find(p.view); if (i != views.end()) state = i->second.lock(); }
                if (!state) continue;
                if (p.op == Op::Closed) {
                    std::lock_guard lock(state->mutex); state->closed = true; state->closed_condition.notify_all(); continue;
                }
                if (p.op == Op::Status) {
                    auto url = r.text(); auto title = r.text();
                    std::lock_guard lock(state->mutex);
                    state->current_url = std::move(url); state->title = std::move(title);
                    state->is_loading = r.integer(); state->can_go_back = r.integer(); state->can_go_forward = r.integer();
                } else if (p.op == Op::Paint) {
                    int width = r.integer(), height = r.integer();
                    if (width > 0 && height > 0 && width <= 16384 && height <= 16384 &&
                        uint64_t(width) * height * 4 == p.data.size() - r.offset)
                        compose_cef_on_paint(state, p.data.data() + r.offset, width, height);
                } else if (p.op == Op::NavigateRequest) {
                    auto token = r.value<uint64_t>(); auto url = r.text();
                    bool closing;
                    { std::lock_guard lock(state->mutex); closing = state->closing; }
                    Writer reply; reply.value(token); reply.integer(!closing && compose_cef_call_on_navigate(state->handle, url));
                    send(Op::NavigateReply, state->handle, reply);
                } else if (p.op == Op::JsResult) compose_cef_call_on_js_result(state->handle, r.text());
                else if (p.op == Op::Cookies) compose_cef_call_on_cookies(state->handle, r.text());
                else if (p.op == Op::Ipc) compose_cef_call_on_ipc(state->handle, r.text());
                else if (p.op == Op::Menu) {
                    auto token = r.value<uint64_t>(); int x = r.integer(), y = r.integer();
                    compose_cef_show_context_menu(state, token, x, y, decodeMenu(r));
                } else if (p.op == Op::ClipboardText) clipboard(r.text());
                else if (p.op == Op::ClipboardImage) {
                    auto *image = new std::vector<uint8_t>(std::move(p.data));
                    g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer p) -> gboolean {
                        auto &bytes = *static_cast<std::vector<uint8_t> *>(p);
                        auto *loader = gdk_pixbuf_loader_new();
                        if (gdk_pixbuf_loader_write(loader, bytes.data(), bytes.size(), nullptr) &&
                            gdk_pixbuf_loader_close(loader, nullptr)) {
                            gtk_clipboard_set_image(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), gdk_pixbuf_loader_get_pixbuf(loader));
                        }
                        g_object_unref(loader); return G_SOURCE_REMOVE;
                    }, image, [](gpointer p) { delete static_cast<std::vector<uint8_t> *>(p); });
                } else if (p.op == Op::Error) g_printerr("CEF host: %s\n", r.text().c_str());
            }
        } catch (const std::exception &e) { g_printerr("CEF host protocol: %s\n", e.what()); }
        { std::lock_guard lock(ready_mutex); failed = true; ready_condition.notify_all(); }
        std::lock_guard lock(views_mutex);
        for (auto &[id, weak] : views) if (auto state = weak.lock()) {
            std::lock_guard slock(state->mutex); state->closed = true; state->closed_condition.notify_all();
        }
    }
};

ComposeCefBrowserRef compose_cef_start_host(const std::shared_ptr<ComposeCefViewState> &state,
                                         const std::string &runtime, const ComposeCefCreateOptions &options) {
    std::lock_guard lock(pool_mutex);
    auto cache = options.dataDirectory.empty() ? "/tmp/composewebview-cef" : options.dataDirectory;
    std::error_code error;
    auto canonical = std::filesystem::weakly_canonical(cache, error);
    if (!error) cache = canonical.string();
    auto key = runtime + "\n" + cache;
    auto &process = pool[key];
    if (process && !process->healthy()) { process->stop(); process.reset(); }
    if (!process) {
        process = std::make_shared<CefHostProcess>(); process->key = key;
        if (!process->start(runtime, cache)) { process->stop(); process.reset(); pool.erase(key); return nullptr; }
        if (g_getenv("COMPOSE_CEF_DEBUG_PROCESS")) g_printerr("CEF host started: pid=%d\n", process->pid);
    }
    auto browser = std::make_shared<CefRemoteBrowser>(process, state->handle);
    process->add(state, options);
    return browser;
}
void compose_cef_release_host(const ComposeCefBrowserRef &browser) { if (browser) browser->close(); }
void compose_cef_post_to_ui(std::function<void()> fn) { fn(); }
CefRemoteBrowser::CefRemoteBrowser(std::shared_ptr<CefHostProcess> process, uint64_t id)
    : process_(std::move(process)), id_(id) {}
void CefRemoteBrowser::send(Op op, const Writer &w) { process_->send(op, id_, w); }
void CefRemoteBrowser::close() { process_->release(id_); }
void CefRemoteBrowser::ExecuteJavaScript(const std::string &s, const std::string &, int) { Writer w; w.text(s); send(Op::Script, w); }
void CefRemoteBrowser::SetFocus(bool v) { Writer w; w.integer(v); send(Op::Focus, w); }
static Writer mouseData(const CefMouseEvent &m) { Writer w; w.integer(m.x); w.integer(m.y); w.value<uint32_t>(m.modifiers); return w; }
void CefRemoteBrowser::SendMouseMoveEvent(const CefMouseEvent &m, bool leave) { auto w = mouseData(m); w.integer(leave); send(Op::MouseMove, w); }
void CefRemoteBrowser::SendMouseClickEvent(const CefMouseEvent &m, CefBrowserHost::MouseButtonType b, bool up, int count) {
    auto w = mouseData(m); w.integer(b); w.integer(up); w.integer(count); send(Op::MouseClick, w);
}
void CefRemoteBrowser::SendMouseWheelEvent(const CefMouseEvent &m, int x, int y) { auto w = mouseData(m); w.integer(x); w.integer(y); send(Op::Wheel, w); }
void CefRemoteBrowser::SendKeyEvent(const CefKeyEvent &k) {
    Writer w; w.integer(k.type); w.value<uint32_t>(k.modifiers); w.integer(k.windows_key_code);
    w.integer(k.native_key_code); w.integer(k.is_system_key); w.integer(k.character); w.integer(k.unmodified_character); send(Op::Key, w);
}
void CefRemoteBrowser::Undo() { Writer w; w.integer(MENU_ID_UNDO); send(Op::Edit, w); }
void CefRemoteBrowser::Redo() { Writer w; w.integer(MENU_ID_REDO); send(Op::Edit, w); }
void CefRemoteBrowser::SelectAll() { Writer w; w.integer(MENU_ID_SELECT_ALL); send(Op::Edit, w); }
