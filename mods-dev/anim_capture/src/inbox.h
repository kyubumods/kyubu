// The dev-mode file inbox of the capture mod (openspec add-ability-animation, task 7.6; design decision 7). A request file is a text file
// in <game>/kyubu/inbox/ whose first line is one console line; the mod runs it as if the author typed it. Pure: no game and no loader, so
// a unit test builds it (tests/unit/test_anim_inbox.cpp).
//
// Only the words this mod itself answers run from the inbox: `anim preview`, `anim capture`, `anim rig`, `anim bake`. Anything else is
// refused with a reply, so a file dropped there cannot reach another command (decision 7: "console commands the player could type", and
// only the dev tool's own).
#pragma once
#include <string>

namespace kyubu::anim_capture {

constexpr size_t kInboxMaxBytes = 1024;   // a request file larger than this is consumed and refused

struct InboxRequest {
    bool ok = false;
    std::string line;    // the console line, trimmed
    std::string sub;     // preview, capture, rig or bake
    std::string args;    // what follows the subcommand, trimmed
    std::string error;   // when !ok: the line to write in the reply
};

// `text` is the content of the request file. The first line that is not blank is the console line; later lines are ignored (a text editor
// adds a newline, and a note below the line is allowed). A byte order mark and a carriage return are dropped.
inline InboxRequest parse_inbox_request(const std::string& text) {
    InboxRequest r;
    if (text.size() > kInboxMaxBytes) {
        r.error = "inbox: the request file is larger than " + std::to_string(kInboxMaxBytes) + " bytes";
        return r;
    }
    size_t at = 0;
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) at = 3;
    std::string line;
    while (at <= text.size()) {
        const size_t end = text.find('\n', at);
        std::string raw = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
        const size_t a = raw.find_first_not_of(" \t\r");
        if (a != std::string::npos) {
            const size_t b = raw.find_last_not_of(" \t\r");
            line = raw.substr(a, b - a + 1);
            break;
        }
        if (end == std::string::npos) break;
        at = end + 1;
    }
    if (line.empty()) {
        r.error = "inbox: the request file has no console line";
        return r;
    }
    r.line = line;
    for (char c : line)
        if (static_cast<unsigned char>(c) < 0x20 && c != '\t') {
            r.error = "inbox: the console line has a control character";
            return r;
        }
    auto word = [&line](size_t from, std::string* out) {
        const size_t a = line.find_first_not_of(" \t", from);
        if (a == std::string::npos) {
            out->clear();
            return line.size();
        }
        const size_t b = line.find_first_of(" \t", a);
        *out = line.substr(a, b == std::string::npos ? std::string::npos : b - a);
        return b == std::string::npos ? line.size() : b;
    };
    std::string first, second;
    const size_t after_first = word(0, &first);
    if (first != "anim") {
        r.error = "inbox: only `anim preview`, `anim capture`, `anim rig` and `anim bake` run from the inbox, not `" + first + "`";
        return r;
    }
    const size_t after_second = word(after_first, &second);
    if (second != "preview" && second != "capture" && second != "rig" && second != "bake") {
        r.error = "inbox: `anim " + second + "` does not run from the inbox (only preview, capture, rig and bake)";
        return r;
    }
    r.sub = second;
    const size_t a = line.find_first_not_of(" \t", after_second);
    r.args = a == std::string::npos ? std::string() : line.substr(a);
    r.ok = true;
    return r;
}

// A request file is `<name>.txt`; the mod's reply is `<name>.reply` beside it, so a reply is never taken for a request.
inline bool is_inbox_request_name(const std::string& file) {
    return file.size() > 4 && file.compare(file.size() - 4, 4, ".txt") == 0 && file[0] != '.';
}

inline std::string inbox_reply_name(const std::string& request_file) {
    return request_file.substr(0, request_file.size() - 4) + ".reply";
}

}  // namespace kyubu::anim_capture
