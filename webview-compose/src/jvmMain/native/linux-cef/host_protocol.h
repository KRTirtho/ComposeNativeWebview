#pragma once

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>

namespace cefipc {
constexpr uint32_t kMagic = 0x43454632;
constexpr uint32_t kMaxPacket = 256 * 1024 * 1024;
enum class Op : uint32_t {
    Create = 1, Close, Stop, Resize, Navigate, Back, Forward, Reload, StopLoad,
    Eval, Script, Focus, MouseMove, MouseClick, Wheel, Key, Edit, Zoom,
    GetCookies, SetCookie, RemoveCookies, ClearCookies, MenuDone, NavigateReply, OpenTools, CloseTools,
    Ready = 100, Closed, Status, Paint, JsResult, Cookies, Ipc, NavigateRequest,
    Menu, ClipboardText, ClipboardImage, Error
};
struct Packet { Op op; uint64_t view; std::vector<uint8_t> data; };
struct Writer {
    std::vector<uint8_t> data;
    template<class T> void value(T v) {
        const auto *p = reinterpret_cast<const uint8_t *>(&v);
        data.insert(data.end(), p, p + sizeof(T));
    }
    void integer(int32_t v) { value(v); }
    void number(double v) { value(v); }
    void text(const std::string &s) {
        value<uint32_t>(static_cast<uint32_t>(s.size()));
        data.insert(data.end(), s.begin(), s.end());
    }
};
struct Reader {
    const std::vector<uint8_t> &data;
    size_t offset = 0;
    template<class T> T value() {
        if (sizeof(T) > data.size() - offset) throw std::runtime_error("truncated CEF packet");
        T v; std::memcpy(&v, data.data() + offset, sizeof(T)); offset += sizeof(T); return v;
    }
    int32_t integer() { return value<int32_t>(); }
    double number() { return value<double>(); }
    std::string text() {
        auto n = value<uint32_t>();
        if (n > data.size() - offset) throw std::runtime_error("invalid CEF string");
        std::string s(reinterpret_cast<const char *>(data.data() + offset), n); offset += n; return s;
    }
};
inline bool transfer(int fd, void *buffer, size_t size, bool writing) {
    auto *p = static_cast<uint8_t *>(buffer);
    while (size) {
        ssize_t n = writing ? send(fd, p, size, MSG_NOSIGNAL) : recv(fd, p, size, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n; size -= static_cast<size_t>(n);
    }
    return true;
}
inline bool sendPacket(int fd, Op op, uint64_t view, const std::vector<uint8_t> &data = {}) {
    uint32_t header[] = {kMagic, static_cast<uint32_t>(op), static_cast<uint32_t>(data.size())};
    if (data.size() > kMaxPacket) return false;
    return transfer(fd, header, sizeof(header), true) && transfer(fd, &view, sizeof(view), true) &&
           transfer(fd, const_cast<uint8_t *>(data.data()), data.size(), true);
}
inline bool receivePacket(int fd, Packet &p) {
    uint32_t header[3];
    if (!transfer(fd, header, sizeof(header), false)) return false;
    if (header[0] != kMagic || header[2] > kMaxPacket) return false;
    p.op = static_cast<Op>(header[1]);
    if (!transfer(fd, &p.view, sizeof(p.view), false)) return false;
    p.data.resize(header[2]);
    return transfer(fd, p.data.data(), p.data.size(), false);
}
struct MenuEntry {
    int type = 0, command = -1;
    bool enabled = true, checked = false;
    std::string label;
    std::vector<MenuEntry> children;
};
inline void encodeMenu(Writer &w, const std::vector<MenuEntry> &entries) {
    w.value<uint32_t>(entries.size());
    for (const auto &e : entries) {
        w.integer(e.type); w.integer(e.command); w.integer(e.enabled); w.integer(e.checked); w.text(e.label);
        encodeMenu(w, e.children);
    }
}
inline std::vector<MenuEntry> decodeMenu(Reader &r, unsigned depth = 0) {
    auto count = r.value<uint32_t>();
    if (count > 1024 || depth > 8) throw std::runtime_error("invalid CEF menu");
    std::vector<MenuEntry> entries;
    for (uint32_t i = 0; i < count; ++i) {
        MenuEntry e; e.type = r.integer(); e.command = r.integer(); e.enabled = r.integer();
        e.checked = r.integer(); e.label = r.text(); e.children = decodeMenu(r, depth + 1);
        entries.push_back(std::move(e));
    }
    return entries;
}
} // namespace cefipc
