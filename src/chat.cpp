#include "chat.h"

#include <windows.h>
#include <winhttp.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <safetyhook.hpp>

#include "fault_handler.h"
#include "log.h"
#include "memory.h"
#include "text.h"
#include "world.h"

namespace livemap::chat {

namespace {

using ChatPostFn = void* (*)(void* out, uintptr_t chat, uint8_t type, const void* text, uint32_t loca1,
                             uint32_t loca2, uint32_t player, uint64_t account, const void* opt);
constexpr uint8_t kSystemLine = 4;
constexpr const char* kSiteChatPost = "chat_post";

struct InboundMessage {
    std::string sender;
    std::string text;
    uint64_t queued_ms = 0;
};

struct OutboundMessage {
    std::string sender;
    std::string text;
};

struct RecordedMessage {
    uint64_t timestamp = 0;
    std::string sender;
    std::string text;
    uint8_t type = 0;
};

Config g_config;
ChatPostFn g_post_fn = nullptr;
fault::Hook g_chat_post_hook;

std::mutex g_inbound_mutex;
std::deque<InboundMessage> g_inbound_queue;

std::mutex g_outbound_mutex;
std::condition_variable g_outbound_cv;
std::deque<OutboundMessage> g_outbound_queue;
std::atomic<bool> g_worker_running{false};
std::thread g_worker_thread;

std::mutex g_recent_mutex;
std::deque<RecordedMessage> g_recent_messages;
constexpr size_t kMaxRecentMessages = 50;

thread_local bool g_is_injected = false;

std::wstring to_wide(std::string_view s) {
    std::wstring out;
    out.reserve(s.size());
    for (char c : s) {
        out += static_cast<wchar_t>(static_cast<unsigned char>(c));
    }
    return out;
}

std::string escape_json(std::string_view in) {
    std::string out;
    out.reserve(in.size() + 16);
    for (char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
                break;
        }
    }
    return out;
}

void record_message(const std::string& sender, const std::string& text, uint8_t type) {
    std::lock_guard lock(g_recent_mutex);
    RecordedMessage rec;
    rec.timestamp = static_cast<uint64_t>(GetTickCount64());
    rec.sender = sender;
    rec.text = text;
    rec.type = type;
    g_recent_messages.push_back(std::move(rec));
    while (g_recent_messages.size() > kMaxRecentMessages) {
        g_recent_messages.pop_front();
    }
}

void send_discord_webhook(const std::string& webhook_url, const std::string& bot_name,
                          const std::string& sender, const std::string& content) {
    if (webhook_url.empty() || content.empty()) {
        return;
    }
    std::string_view url = webhook_url;
    if (url.starts_with("https://")) {
        url.remove_prefix(8);
    } else if (url.starts_with("http://")) {
        url.remove_prefix(7);
    }
    size_t slash = url.find('/');
    if (slash == std::string_view::npos) {
        return;
    }
    std::string host(url.substr(0, slash));
    std::string path(url.substr(slash));

    std::wstring host_w = to_wide(host);
    std::wstring path_w = to_wide(path);

    HINTERNET session = WinHttpOpen(L"EnshroudedLiveMap-Discord/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        return;
    }
    WinHttpSetTimeouts(session, 5000, 5000, 5000, 5000);
    HINTERNET conn = WinHttpConnect(session, host_w.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!conn) {
        WinHttpCloseHandle(session);
        return;
    }
    HINTERNET req = WinHttpOpenRequest(conn, L"POST", path_w.c_str(), nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (req) {
        std::wstring headers = L"Content-Type: application/json\r\n";
        std::string formatted = "**" + sender + "**: " + content;
        std::string payload = "{\"username\":\"" + escape_json(bot_name) +
                              "\",\"content\":\"" + escape_json(formatted) + "\"}";

        if (WinHttpSendRequest(req, headers.c_str(), static_cast<DWORD>(headers.size()),
                               payload.data(), static_cast<DWORD>(payload.size()),
                               static_cast<DWORD>(payload.size()), 0)) {
            WinHttpReceiveResponse(req, nullptr);
        }
        WinHttpCloseHandle(req);
    }
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
}

void outbound_worker_loop() {
    while (g_worker_running.load(std::memory_order_relaxed)) {
        OutboundMessage msg;
        {
            std::unique_lock lock(g_outbound_mutex);
            g_outbound_cv.wait(lock, [] {
                return !g_outbound_queue.empty() || !g_worker_running.load(std::memory_order_relaxed);
            });
            if (!g_worker_running.load(std::memory_order_relaxed) && g_outbound_queue.empty()) {
                break;
            }
            if (g_outbound_queue.empty()) {
                continue;
            }
            msg = std::move(g_outbound_queue.front());
            g_outbound_queue.pop_front();
        }
        send_discord_webhook(g_config.webhook_url, g_config.bot_name, msg.sender, msg.text);
        Sleep(100);  // Polite spacing against Discord rate-limits
    }
}

void on_chat_post_hook(SafetyHookContext& ctx) {
    fault::SiteScope scope(kSiteChatPost);
    if (g_is_injected) {
        return;
    }
    uint8_t type = static_cast<uint8_t>(ctx.r8 & 0xff);
    uintptr_t text_desc = ctx.r9;
    if (text_desc == 0) {
        return;
    }
    uintptr_t str_ptr = 0;
    uint64_t str_len = 0;
    if (!read_ptr(text_desc, str_ptr) || !read_u64(text_desc + 8, str_len) || str_ptr == 0 ||
        str_len == 0 || str_len > 4000) {
        return;
    }
    std::string msg(static_cast<size_t>(str_len), '\0');
    auto byte_span = std::span<std::byte>(reinterpret_cast<std::byte*>(msg.data()), msg.size());
    if (!read_bytes(str_ptr, byte_span)) {
        return;
    }
    msg = text::clean_utf8(msg);

    // Skip echoed messages and announce strings
    if (msg.rfind("[Discord]", 0) == 0) {
        return;
    }

    uint32_t handle = 0;
    read_u32(ctx.rsp + 0x38, handle);

    std::string sender = world::player_name_by_handle(handle);
    if (sender.empty()) {
        sender = (handle == 0) ? "Server" : ("Player#" + std::to_string(handle));
    }

    record_message(sender, msg, type);

    if (g_config.enabled && !g_config.webhook_url.empty()) {
        std::lock_guard lock(g_outbound_mutex);
        if (g_outbound_queue.size() < 100) {
            OutboundMessage out_msg;
            out_msg.sender = sender;
            out_msg.text = msg;
            g_outbound_queue.push_back(std::move(out_msg));
            g_outbound_cv.notify_one();
        }
    }
}

}  // namespace

void init(const Config& config) {
    g_config = config;
    if (g_config.enabled && !g_config.webhook_url.empty() && !g_worker_running.load()) {
        g_worker_running.store(true);
        g_worker_thread = std::thread(outbound_worker_loop);
        log("chat: Discord bridge outbound worker active (webhook configured)");
    } else if (g_config.enabled) {
        log("chat: REST chat bridge active on /api/chat");
    }
}

bool install(const Targets& targets) {
    if (targets.chat_post == 0) {
        log("chat: chat_post site not found in target build; bridge disabled");
        return false;
    }
    g_post_fn = reinterpret_cast<ChatPostFn>(targets.chat_post);
    auto hook = fault::Hook::create(kSiteChatPost, targets.chat_post, on_chat_post_hook);
    if (!hook) {
        log("chat: could not hook chat_post");
        return false;
    }
    g_chat_post_hook = std::move(*hook);
    log("hooked %-12s at %llx (Discord & REST chat bridge)", kSiteChatPost,
        static_cast<unsigned long long>(targets.chat_post));
    return true;
}

void shutdown() {
    if (g_worker_running.exchange(false)) {
        g_outbound_cv.notify_all();
        if (g_worker_thread.joinable()) {
            g_worker_thread.join();
        }
    }
}

void queue_inbound(std::string sender, std::string message) {
    std::lock_guard lock(g_inbound_mutex);
    if (g_inbound_queue.size() < 100) {
        InboundMessage in_msg;
        in_msg.sender = std::move(sender);
        in_msg.text = std::move(message);
        in_msg.queued_ms = static_cast<uint64_t>(GetTickCount64());
        g_inbound_queue.push_back(std::move(in_msg));
    }
}

void on_tick(uintptr_t manager) {
    if (g_post_fn == nullptr || manager == 0) {
        return;
    }
    InboundMessage msg;
    {
        std::lock_guard lock(g_inbound_mutex);
        if (g_inbound_queue.empty()) {
            return;
        }
        msg = std::move(g_inbound_queue.front());
        g_inbound_queue.pop_front();
    }

    uint32_t handle = 0;
    uint64_t account = 0;
    if (!world::get_active_player(handle, account)) {
        // No players online to receive chat; discard stale messages older than 30s
        if (GetTickCount64() - msg.queued_ms > 30000) {
            return;
        }
        // Requeue to deliver once a player enters
        std::lock_guard lock(g_inbound_mutex);
        g_inbound_queue.push_front(std::move(msg));
        return;
    }

    uintptr_t chat = 0;
    if (!read_ptr(manager + 0x60, chat) || chat == 0) {
        return;
    }

    std::string line = msg.sender.empty() ? msg.text : ("[" + msg.sender + "] " + msg.text);
    struct {
        const char* ptr;
        uint64_t length;
    } text_arg{line.c_str(), line.size()};

    struct {
        uint64_t value;
        uint8_t has;
        std::array<uint8_t, 7> pad;
    } opt{};
    static_assert(sizeof(opt) == 16);
    alignas(8) uint8_t out[16] = {};

    g_is_injected = true;
    g_post_fn(out, chat, kSystemLine, &text_arg, 0, 0, handle, account, &opt);
    g_is_injected = false;

    record_message(msg.sender.empty() ? "Discord" : msg.sender, msg.text, kSystemLine);
    log("chat: broadcast \"%s\" in-game (result %u)", line.c_str(), out[0]);
}

std::string recent_messages_json() {
    std::lock_guard lock(g_recent_mutex);
    std::string json = "[";
    for (size_t i = 0; i < g_recent_messages.size(); ++i) {
        const auto& m = g_recent_messages[i];
        if (i > 0) {
            json += ",";
        }
        json += "{\"time\":" + std::to_string(m.timestamp) +
                ",\"sender\":\"" + escape_json(m.sender) +
                "\",\"text\":\"" + escape_json(m.text) +
                "\",\"type\":" + std::to_string(m.type) + "}";
    }
    json += "]";
    return json;
}

void parse_chat_payload(std::string_view json, std::string& sender, std::string& message) {
    auto extract = [](std::string_view src, std::initializer_list<std::string_view> keys) -> std::string {
        for (auto key : keys) {
            std::string pattern = "\"" + std::string(key) + "\"";
            size_t pos = src.find(pattern);
            if (pos == std::string_view::npos) {
                continue;
            }
            pos += pattern.size();
            while (pos < src.size() && (src[pos] == ' ' || src[pos] == ':' || src[pos] == '\t')) {
                ++pos;
            }
            if (pos >= src.size()) {
                continue;
            }
            if (src[pos] == '"') {
                ++pos;
                std::string val;
                while (pos < src.size() && src[pos] != '"') {
                    if (src[pos] == '\\' && pos + 1 < src.size()) {
                        ++pos;
                        if (src[pos] == 'n') val += '\n';
                        else if (src[pos] == 'r') val += '\r';
                        else if (src[pos] == 't') val += '\t';
                        else val += src[pos];
                    } else {
                        val += src[pos];
                    }
                    ++pos;
                }
                return val;
            }
        }
        return {};
    };

    std::string s = extract(json, {"sender", "user", "username", "author"});
    if (!s.empty()) {
        sender = s;
    }
    std::string m = extract(json, {"message", "content", "text"});
    if (!m.empty()) {
        message = m;
    }
}

}  // namespace livemap::chat
