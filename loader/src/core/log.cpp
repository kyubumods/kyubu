#include "log.h"

#include <cstdio>

namespace kyubu {

Log& Log::get() {
    static Log* instance = new Log();   // never destroyed: logging must work during shutdown
    return *instance;
}

const char* level_name(KyubuLogLevel l) {
    switch (l) {
        case KY_LOG_DEBUG: return "DEBUG";
        case KY_LOG_INFO: return "INFO";
        case KY_LOG_WARN: return "WARN";
        case KY_LOG_ERROR: return "ERROR";
    }
    return "?";
}

const char* side_name(KyubuSide s) {
    switch (s) {
        case KY_SIDE_CLIENT: return "client";
        case KY_SIDE_SERVER: return "server";
        case KY_SIDE_BOTH: return "both";
        default: return "-";
    }
}

void Log::open(const std::string& dir, int keep) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    if (file_ != INVALID_HANDLE_VALUE) { CloseHandle(file_); file_ = INVALID_HANDLE_VALUE; }
    make_dirs(dir);
    path_ = join_path(dir, "latest.log");
    // rotate: log-(keep-1) -> log-keep, ..., latest -> log-1
    for (int i = keep - 1; i >= 1; --i) {
        std::string from = join_path(dir, fmt("log-%d.log", i));
        std::string to = join_path(dir, fmt("log-%d.log", i + 1));
        MoveFileExW(widen(from).c_str(), widen(to).c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    if (keep >= 1)
        MoveFileExW(widen(path_).c_str(), widen(join_path(dir, "log-1.log")).c_str(), MOVEFILE_REPLACE_EXISTING);
    file_ = CreateFileW(widen(path_).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_ != INVALID_HANDLE_VALUE) {
        static const unsigned char bom[] = {0xEF, 0xBB, 0xBF};
        DWORD w;
        WriteFile(file_, bom, 3, &w, nullptr);
        // flush lines logged before the file existed
        for (auto& l : ring_) {
            std::string s = l.text + "\r\n";
            WriteFile(file_, s.data(), (DWORD)s.size(), &w, nullptr);
        }
    }
}

void Log::write(KyubuLogLevel level, std::string_view source, KyubuSide side, std::string_view msg) {
    if (level < min_level_) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    char head[96];
    snprintf(head, sizeof head, "%04d-%02d-%02d %02d:%02d:%02d.%03d %-5s ", t.wYear, t.wMonth, t.wDay, t.wHour,
             t.wMinute, t.wSecond, t.wMilliseconds, level_name(level));
    std::string line = head;
    line += "[";
    line.append(source.data(), source.size());
    line += "] [";
    line += side_name(side);
    line += "] ";
    line.append(msg.data(), msg.size());

    std::vector<Listener> ls;
    LogLine ll;
    {
        std::lock_guard<std::recursive_mutex> g(mu_);
        ll = LogLine{seq_++, level, line};
        ring_.push_back(ll);
        if (ring_.size() > kRing) ring_.pop_front();
        if (file_ != INVALID_HANDLE_VALUE) {
            std::string s = line + "\r\n";
            DWORD w;
            WriteFile(file_, s.data(), (DWORD)s.size(), &w, nullptr);
        }
        ls = listeners_;
    }
    OutputDebugStringA((line + "\n").c_str());
    for (auto& l : ls) l(ll);
}

std::vector<LogLine> Log::tail(size_t n) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    size_t start = ring_.size() > n ? ring_.size() - n : 0;
    return std::vector<LogLine>(ring_.begin() + start, ring_.end());
}

std::vector<LogLine> Log::since(uint64_t seq) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    std::vector<LogLine> out;
    for (auto& l : ring_) if (l.seq >= seq) out.push_back(l);
    return out;
}

void Log::add_listener(Listener l) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    listeners_.push_back(std::move(l));
}

}  // namespace kyubu
