// The Animator's editing model: see model.h. File access is Windows-only (MoveFileExW replaces the clip file in one step).
#include "model.h"

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace kanimator {

namespace {

const char* kSuffix = ".anim.toml";

std::wstring to_wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::filesystem::path to_path(const std::string& s) { return std::filesystem::path(to_wide(s)); }

bool read_file(const std::string& path, std::string& out, std::string& err) {
    std::ifstream in(to_path(path), std::ios::binary);
    if (!in) {
        err = "cannot open " + path;
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

bool write_file(const std::string& path, const std::string& text, std::string& err) {
    std::ofstream out(to_path(path), std::ios::binary | std::ios::trunc);
    if (!out) {
        err = "cannot write " + path;
        return false;
    }
    out.write(text.data(), (std::streamsize)text.size());
    out.close();
    if (!out) {
        err = "cannot write " + path;
        return false;
    }
    return true;
}

bool replace_file(const std::string& from, const std::string& to, std::string& err) {
    if (MoveFileExW(to_wide(from).c_str(), to_wide(to).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return true;
    err = "cannot replace " + to + " (error " + std::to_string(GetLastError()) + ")";
    return false;
}

}  // namespace

bool valid_clip_name(const std::string& name) {
    if (name.empty() || name.size() > kMaxNameLen) return false;
    for (char c : name) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (!ok) return false;
    }
    return true;
}

bool name_from_path(const std::string& path, std::string& name) {
    size_t slash = path.find_last_of("/\\");
    std::string file = slash == std::string::npos ? path : path.substr(slash + 1);
    std::string suffix = kSuffix;
    if (file.size() <= suffix.size() || file.compare(file.size() - suffix.size(), suffix.size(), suffix) != 0) return false;
    name = file.substr(0, file.size() - suffix.size());
    return valid_clip_name(name);
}

std::string new_clip_text(const std::string& name) {
    return "# " + name + ": a new clip, made in the Animator. Times: ms for duration, fractions of it for t.\n"
           "duration = 1000\n"
           "\n"
           "[[track]]\n"
           "part = \"hand_r\"\n"
           "channel = \"rot\"\n"
           "mode = \"add\"\n"
           "keys = [\n"
           "  { t = 0.00, v = [0, 0, 0] },\n"
           "  { t = 1.00, v = [0, 0, 0] },\n"
           "]\n";
}

FileStamp stamp_of(const std::string& path) {
    FileStamp s;
    std::error_code ec;
    std::filesystem::path p = to_path(path);
    uintmax_t size = std::filesystem::file_size(p, ec);
    if (ec) return s;
    auto t = std::filesystem::last_write_time(p, ec);
    s.size = (int64_t)size;
    s.mtime = ec ? 0 : (int64_t)t.time_since_epoch().count();
    return s;
}

std::string autosave_path(const std::string& clip_path) { return clip_path + ".autosave"; }

void ClipDoc::forget_autosave_state() {
    autosaved_text_.clear();
    autosave_written_ = false;
    dirty_since_ms_ = -1;
    last_autosave_ms_ = -1;
}

bool ClipDoc::open(const std::string& path, std::string& err) {
    std::string text;
    if (!read_file(path, text, err)) return false;
    path_ = path;
    text_ = text;
    saved_text_ = text;
    undo_.clear();
    known_ = stamp_of(path);
    forget_autosave_state();
    return true;
}

void ClipDoc::start_new(const std::string& path, const std::string& text) {
    path_ = path;
    text_ = text;
    saved_text_.clear();
    undo_.clear();
    known_ = stamp_of(path);
    forget_autosave_state();
}

bool ClipDoc::save(std::string& err) {
    if (path_.empty()) {
        err = "no file: New or Open a clip first";
        return false;
    }
    std::string tmp = path_ + ".tmp";
    if (!write_file(tmp, text_, err)) return false;
    if (!replace_file(tmp, path_, err)) {
        std::error_code ec;
        std::filesystem::remove(to_path(tmp), ec);
        return false;
    }
    saved_text_ = text_;
    known_ = stamp_of(path_);
    dirty_since_ms_ = -1;
    drop_autosave();
    return true;
}

bool ClipDoc::reload(std::string& err) {
    std::string text;
    if (!read_file(path_, text, err)) return false;
    known_ = stamp_of(path_);
    dirty_since_ms_ = -1;
    if (text == text_) {
        saved_text_ = text;   // nothing to show: the undo stack still fits the text
        return true;
    }
    text_ = text;
    saved_text_ = text;
    undo_.clear();
    return true;
}

void ClipDoc::set_text(const std::string& text) {
    if (text == text_) return;
    undo_.push_back(text_);
    if (undo_.size() > kUndoDepth) undo_.erase(undo_.begin());
    text_ = text;
}

bool ClipDoc::undo() {
    if (undo_.empty()) return false;
    text_ = undo_.back();
    undo_.pop_back();
    return true;
}

bool ClipDoc::disk_changed() const {
    if (path_.empty()) return false;
    return stamp_of(path_) != known_;
}

void ClipDoc::accept_disk() { known_ = stamp_of(path_); }

bool ClipDoc::autosave_tick(int64_t now_ms, int64_t interval_ms, std::string& err) {
    if (path_.empty()) return false;
    if (!dirty()) {
        dirty_since_ms_ = -1;
        if (autosave_written_) drop_autosave();
        return false;
    }
    if (dirty_since_ms_ < 0) dirty_since_ms_ = now_ms;
    if (autosave_written_ && text_ == autosaved_text_) return false;
    const int64_t since = std::max(dirty_since_ms_, last_autosave_ms_);
    if (now_ms - since < interval_ms) return false;
    const std::string target = autosave_path(path_);
    const std::string tmp = target + ".tmp";
    if (!write_file(tmp, text_, err)) return false;
    if (!replace_file(tmp, target, err)) {
        std::error_code ec;
        std::filesystem::remove(to_path(tmp), ec);
        return false;
    }
    autosaved_text_ = text_;
    autosave_written_ = true;
    last_autosave_ms_ = now_ms;
    return true;
}

bool ClipDoc::find_autosave(std::string& text) {
    if (path_.empty()) return false;
    std::string found, err;
    if (!read_file(autosave_path(path_), found, err)) return false;
    if (found == text_) {
        drop_autosave();
        return false;
    }
    text = found;
    return true;
}

void ClipDoc::drop_autosave() {
    if (!path_.empty()) {
        std::error_code ec;
        std::filesystem::remove(to_path(autosave_path(path_)), ec);
    }
    autosaved_text_.clear();
    autosave_written_ = false;
    last_autosave_ms_ = -1;
}

}  // namespace kanimator
