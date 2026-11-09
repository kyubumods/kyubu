#pragma once
#include "common.h"

#include <deque>
#include <mutex>

namespace kyubu {

struct LogLine {
    uint64_t seq;
    KyubuLogLevel level;
    std::string text;       // fully formatted line (without newline)
};

class Log {
public:
    static Log& get();

    // Opens kyubu/logs/latest.log, rotating up to `keep` previous logs (log-1.log .. log-N.log).
    void open(const std::string& logs_dir, int keep = 5);
    void set_min_level(KyubuLogLevel l) { min_level_ = l; }
    KyubuLogLevel min_level() const { return min_level_; }

    void write(KyubuLogLevel level, std::string_view source, KyubuSide side, std::string_view msg);

    std::vector<LogLine> tail(size_t n);        // most recent n lines (oldest first)
    uint64_t sequence() const { return seq_; }  // number of lines written so far
    std::vector<LogLine> since(uint64_t seq);   // lines with seq >= given (bounded by ring size)
    std::string path() const { return path_; }

    using Listener = std::function<void(const LogLine&)>;
    void add_listener(Listener l);

private:
    std::recursive_mutex mu_;
    HANDLE file_ = INVALID_HANDLE_VALUE;
    std::string path_;
    std::deque<LogLine> ring_;
    std::vector<Listener> listeners_;
    uint64_t seq_ = 0;
    KyubuLogLevel min_level_ = KY_LOG_INFO;
    static constexpr size_t kRing = 1000;
};

const char* level_name(KyubuLogLevel l);
const char* side_name(KyubuSide s);

template <typename... A>
std::string fmt(const char* f, A... a) {
    int n = snprintf(nullptr, 0, f, a...);
    std::string s(n > 0 ? n : 0, '\0');
    if (n > 0) snprintf(s.data(), n + 1, f, a...);
    return s;
}

}  // namespace kyubu

#define KLOG(level, ...) ::kyubu::Log::get().write(level, "kyubu", KY_SIDE_NONE, ::kyubu::fmt(__VA_ARGS__))
#define KDEBUG(...) KLOG(KY_LOG_DEBUG, __VA_ARGS__)
#define KINFO(...) KLOG(KY_LOG_INFO, __VA_ARGS__)
#define KWARN(...) KLOG(KY_LOG_WARN, __VA_ARGS__)
#define KERROR(...) KLOG(KY_LOG_ERROR, __VA_ARGS__)
