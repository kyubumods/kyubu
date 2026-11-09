// ModManager (change add-mod-manager): the package. It checks at load that every game entry the page needs
// resolves, and logs the ones that do not (then there is no Mods row). The Mods row (D11): the mod rewrites the game's "Exit" label
// to "Mods", so the game draws, hovers (hover 2) and clicks its third row; a mid hook at drawEndSite turns that hover into 3, draws
// Exit again below it (or in the bottom-left corner of a short window) as hover 4, and acts on both clicks.
//
// The page shell (design D9): a click on Mods opens a page of nodes cloned from the game's own (blackwidget frames, the
// character-creation Back button for every button; the way of the dev mod start_menu_row). The clones are made once at the
// update site `startMenuShownSite`, which also hides the logo, places the clones for the window size, tests the queued clicks with
// getCursor and Escape, and keeps `page::State` (page.h). The page's text is drawn by the title menu's own draw, which stays
// visible while the page is open: its three labels are blanked in .rdata (restored on close), its hover is held at -1 so a click
// cannot start the game, and `start_menu_node` is moved to the end of `start_scene` so its text is drawn over the clones.
// The settings forms (form.h) are read with mods_option_get when a mod is first selected and are drawn under its details.
// The Actions menu and the change bar (actions.h): Enable/Disable (with the mods that need it), Reset options, Delete (after a
// box) and Undo last apply; APPLY writes the option drafts at once (mods_option_set), queues the other changes (mods_queue) and restarts.
// Downloads (download.h): the Download and Update rows plan the dependencies (actions::download), the worker stages the
// zips in kyubu\staging\, a failure drops that download with its dependencies, and Apply & restart queues the installs.
// Modpacks (packs.h, profile.h): Apply modpack downloads the pack's pinned versions as one group, saves the local
// profile "Before <pack>", queues the installs, enables, disables and the options of downloaded mods, sets the other options at once
// and restarts; a profile's Restore puts its mods (from kyubu\backup\ when another version is installed) and options back.
// Console: mod_manager:state, mod_manager:pending <n> (a stand-in for collected changes), mod_manager:texttest.
#include <windows.h>
#include <shellapi.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "actions.h"
#include "browse.h"
#include "download.h"
#include "catalog.h"
#include "form.h"
#include "hand.h"
#include "kyubu.hpp"
#include "list.h"
#include "packs.h"
#include "page.h"
#include "plan.h"
#include "profile.h"
#include "row.h"
#include "style.h"
#include "text.h"

namespace {

// Every game entry that the Mods row and the page call or hook.
const char* const kSymbols[] = {
    "cube::StartMenuWidget::drawEndSite",
    "cube::GameController::startMenuShownSite",
    "cube::game_controller",
    "plasma::FontEngine::getFont",
    "plasma::ScalableFont::drawText",
    "plasma::ScalableFont::measureText",
    "plasma::Widget::getCursor",
    "plasma::Widget::getWidth",
    "plasma::Widget::setPosition",
    "plasma::Widget::setSize",
    "plasma::Widget::getSizeWidth",
    "plasma::Widget::getSizeHeight",
    "plasma::Node::clone",
    "plasma::Node::setText",
    "plasma::Node::findChild",
    "plasma::Node::setParent",
};

kyubu::Mod* g_mod = nullptr;
text::Font g_fns;   // the function pointers; `font` is set at each draw

using GetCursorFn = void(__attribute__((thiscall)) *)(void* widget, float* out_xy);
using GetWidthFn = float(__attribute__((thiscall)) *)(void* widget);
GetCursorFn g_get_cursor = nullptr;
GetWidthFn g_get_width = nullptr;

constexpr uint32_t kHoverOffset = 0x160;   // cube::StartMenuWidget.hover
constexpr uint32_t kQuitOffset = 0x1A0;    // cube::GameController.quit_requested (u8)
constexpr uint32_t kWindowW = 0x11C, kWindowH = 0x120;   // cube::GameController window size
constexpr uint32_t kExitLabelRva = 0x71A204 - 0x400000;  // the wide string literal "Exit", used only by the start menu's draw
constexpr DWORD kRecentMs = 500;           // a draw this long ago still counts as "the menu is showing"
constexpr int kHoverMods = 3, kHoverExit = 4;

volatile uint32_t g_draws = 0, g_mods_clicks = 0, g_exit_clicks = 0, g_page_requests = 0;
volatile int g_hover_seen = -1;
volatile DWORD g_last_draw_tick = 0;
volatile uintptr_t g_widget = 0;
volatile int g_corner = 0;
uintptr_t g_gc_global = 0;

uintptr_t game_controller() {
    uint32_t gc = 0;
    if (!g_gc_global || g_mod->api()->mem_read(g_gc_global, &gc, sizeof gc) != KY_OK) return 0;
    return gc;
}

// --- the page ---------------------------------------------------------------------------------------------------------

constexpr uint32_t kStartScene = 0x800874;      // cube::GameController.start_scene
constexpr uint32_t kLogo = 0x800878;            // cube::GameController.logo
constexpr uint32_t kStartMenuNode = 0x800918;   // cube::GameController.start_menu_node
constexpr uint32_t kBlackwidget = 0x8008C8;     // cube::GameController.blackwidget_template
constexpr uint32_t kCreationBack = 0x80099C;    // cube::GameController.creation_back_button
constexpr uint32_t kNodeDisplay = 0x3C, kNodeWidget = 0x40;   // plasma::Node.display, widget
constexpr uint32_t kDisplayKey = 0x68, kDisplayValues = 0x94; // plasma::Display.key, visible_values
// The three wide literals the title menu draws ("Start Game", "Options", and "Exit" which row.h's rewrite made "Mods"): their first
// character is zeroed while the page is open, so the menu draws nothing of its own.
constexpr uint32_t kLabelRvas[3] = {0x71A1DC - 0x400000, 0x71A1F4 - 0x400000, kExitLabelRva};
constexpr int kMenuRows = 4;
constexpr float kButtonTextSize = 22.0f;   // DISCARD and APPLY, drawn by the page
constexpr float kFilterArrowZone = 56.0f;  // the filter picker's arrows: a click this near the left or right end steps it
constexpr float kInfoPitch = 26.0f;        // one line of the details

using CloneFn = void*(__attribute__((thiscall)) *)(void* node, void* parent);
using SetTextFn = void(__attribute__((thiscall)) *)(void* node, const void* text);
using SetParentFn = void(__attribute__((thiscall)) *)(void* node, void* parent);
using SetXYFn = void(__attribute__((thiscall)) *)(void* widget, float x, float y, int update);
using GetSizeFn = float(__attribute__((thiscall)) *)(void* widget);

CloneFn g_clone = nullptr;
SetTextFn g_set_text = nullptr;
SetParentFn g_set_parent = nullptr;
SetXYFn g_set_position = nullptr, g_set_size = nullptr;
GetSizeFn g_size_w = nullptr, g_size_h = nullptr;

struct Nodes {
    void *back = nullptr, *frame = nullptr, *tab[2] = {nullptr, nullptr}, *sub[2] = {nullptr, nullptr}, *list = nullptr, *search = nullptr, *details = nullptr, *bar = nullptr;
    void *discard = nullptr, *apply = nullptr, *actions = nullptr, *menu = nullptr, *confirm = nullptr, *yes = nullptr, *no = nullptr;
};
Nodes g_n;
page::State g_state;
page::Layout g_layout;
list::View g_view;   // the My mods list; filled at each open of the page
list::View g_cat_view;   // the Install tab's list: the catalog's mods
list::View g_pack_view;   // Install / Modpacks: the signed index's packs
// The list the page shows: My mods, Install / Mods or Install / Modpacks.
packs::Plan g_pack_plan;      // the plan of the selected pack, made again each frame it is drawn
std::string g_pack_planned;   // what the log last said about it: the log line comes once per change
// Apply modpack and Restore: the local profiles in kyubu\data\mod_manager\profiles\, the pack whose downloads run, and
// the reason the last apply of a pack stopped (shown in its details until the page reads the mods again).
std::vector<cindex::Pack> g_profiles;
struct PackApply { bool on = false; std::string pack_id; int group = 0; };
PackApply g_papply;
std::map<std::string, std::wstring> g_pack_stopped;
bool g_delete_profile = false;   // the delete box asks about the profile g_delete_id
volatile uint32_t g_pack_applies = 0, g_restores = 0;
list::View& cur_view() { return g_state.tab == 0 ? g_view : (g_state.sub == 1 ? g_pack_view : g_cat_view); }
cindex::Loaded g_loaded;   // the catalog result the lists were last built from
std::map<std::string, std::string> g_hashes;   // installed id -> package hash in hex
void apply_catalog();
struct ChipHit { page::Rect r; std::wstring id; };
struct FormHit { page::Rect r; form::Form* fm; int field; };
std::map<std::string, form::Form> g_forms;   // the drafts of every mod looked at since the page opened
std::vector<FormHit> g_form_hits;            // the option rows drawn last frame
int g_tip_row = -1;                          // the row (index among the details lines) whose tooltip was drawn last frame, or -1
page::Rect g_tip_rect;                       // where that row was drawn and where the cursor found it: the cursor stays on it anywhere in here
volatile uint32_t g_form_clicks = 0, g_apply_blocked = 0;
std::vector<ChipHit> g_chip_hits;   // the dependency chips drawn last frame that a click selects
volatile unsigned g_chars[32];
volatile int g_char_head = 0, g_char_tail = 0;   // the typed characters waiting for the update hook
volatile int g_backspaces = 0, g_wheel = 0;
bool g_page_ok = false;   // every page symbol resolved and the update site is hooked
bool g_built = false;
bool g_labels_blank = false;
wchar_t g_third_char = L'E';   // the third label's real first character: 'M' once the row rewrote it, else 'E'
volatile bool g_open_request = false, g_click_pending = false, g_escape_pending = false;
volatile int g_build_failed = 0;
bool g_options_invalid = false;
volatile uint32_t g_page_frames = 0, g_opens = 0, g_closes = 0, g_back_clicks = 0, g_tab_clicks = 0, g_actions_clicks = 0, g_menu_rows_clicked = 0;
volatile uint32_t g_row_clicks = 0, g_chip_clicks = 0;
volatile uint32_t g_discard_clicks = 0, g_apply_clicks = 0, g_confirm_yes = 0, g_confirm_no = 0, g_escapes = 0, g_relayouts = 0;
volatile float g_name_w = 160.0f;   // measured by the draw, used by the next layout
int g_laid_w = -1, g_laid_h = -1, g_laid_menu_n = -1;
float g_laid_name_w = -1.0f;

// The collected changes of the page: the installed mods as the planner sees them, the changes, the Actions menu of the
// selected mod, the delete box, and the line the bar shows after an action that was refused or that failed.
plan::World g_world;
std::map<std::string, actions::Pkg> g_pkgs;
actions::Collected g_coll;
std::vector<actions::Row> g_menu;
bool g_confirm_delete = false;
std::string g_delete_id;
std::wstring g_delete_name;
std::string g_notice;
int g_stub_pending = 0;   // mod_manager:pending, a stand-in for more changes
volatile uint32_t g_applies = 0, g_undos = 0;

// Mods put into mods\ by hand: packages the rescan found (state New) wait for the restart; the ids seen arriving by hand stay
// marked Unverified. The page rescans when it opens and when the game window gets the focus back.
int g_new_count = 0;
volatile bool g_rescan_request = false;
bool g_was_active = false;
volatile uint32_t g_rescans = 0, g_folder_clicks = 0;
page::Rect g_open_rect;   // the Open mods folder text drawn last frame

int32_t* visible_word(void* node) {
    const uintptr_t display = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uintptr_t>(node) + kNodeDisplay);
    if (!display) return nullptr;
    const int32_t key = *reinterpret_cast<int32_t*>(display + kDisplayKey);
    int32_t* values = *reinterpret_cast<int32_t**>(display + kDisplayValues);
    return values ? values + key : nullptr;
}
void set_visible(void* node, bool on) {
    if (int32_t* w = node ? visible_word(node) : nullptr) *w = on ? 1 : 0;
}
void* widget_of(void* node) { return node ? *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(node) + kNodeWidget) : nullptr; }
uintptr_t field(uintptr_t gc, uint32_t off) { return *reinterpret_cast<uintptr_t*>(gc + off); }

bool cursor_in(void* node) {
    void* w = widget_of(node);
    if (!w) return false;
    float c[2] = {-1.0f, -1.0f};
    g_get_cursor(w, c);
    return c[0] >= 0.0f && c[1] >= 0.0f && c[0] < g_size_w(w) && c[1] < g_size_h(w);
}

// The cursor in window pixels: the page's frame node sits at a known place under the scene.
bool screen_cursor(float& x, float& y) {
    void* w = widget_of(g_n.frame);
    if (!w) return false;
    float c[2] = {-1.0f, -1.0f};
    g_get_cursor(w, c);
    x = c[0] + g_layout.frame.x;
    y = c[1] + g_layout.frame.y;
    return true;
}

void set_label(void* node, const wchar_t* s) {
    text::GameString g(s, std::wcslen(s));
    g_set_text(node, &g);
}

void place(void* node, const page::Rect& r) {
    void* w = widget_of(node);
    if (!w) return;
    g_set_size(w, r.w, r.h, 1);
    g_set_position(w, r.x, r.y, 1);
}

std::wstring widen(const char* s) {
    if (!s || !*s) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 1) return L"";
    std::wstring w(static_cast<size_t>(n), L' ');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    w.resize(static_cast<size_t>(n - 1));
    return w;
}

// Reads every package of the mods folder (mods_get) into the list. Called when the page opens.
std::string hand_path() {
    char dir[300] = {};
    if (g_mod->api()->mod_data_dir(g_mod->handle(), dir, sizeof dir) != KY_OK) return "";
    CreateDirectoryA(dir, nullptr);
    return std::string(dir) + "\\by-hand.txt";
}

std::set<std::string> read_hand() {
    const std::string path = hand_path();
    std::string text;
    if (FILE* f = path.empty() ? nullptr : std::fopen(path.c_str(), "rb")) {
        char buf[512];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
        std::fclose(f);
    }
    return hand::parse(text);
}

void write_hand(const std::set<std::string>& ids) {
    const std::string path = hand_path();
    const std::string text = hand::write(ids);
    if (FILE* f = path.empty() ? nullptr : std::fopen(path.c_str(), "wb")) { std::fwrite(text.data(), 1, text.size(), f); std::fclose(f); }
    else g_mod->warn("mod_manager: cannot write the list of mods installed by hand");
}

void load_profiles();

// `rescan` first asks the loader for the packages added to or removed from mods\ since boot; `keep_changes` keeps what the page collected
// (a rescan while the page is open).
void refresh_mods(bool rescan = true, bool keep_changes = false) {
    const KyubuApi* api = g_mod->api();
    if (rescan) {
        const KyubuResult rr = api->mods_rescan(g_mod->handle());
        g_rescans = g_rescans + 1;
        if (rr != KY_OK) g_mod->warn("mod_manager: mods_rescan failed (result %d)", static_cast<int>(rr));
    }
    g_hashes.clear();
    std::set<std::string> by_hand = read_hand();
    const size_t by_hand_before = by_hand.size();
    g_new_count = 0;
    const uint32_t count = api->mods_count(g_mod->handle());
    std::vector<list::Mod> mods;
    std::vector<KyubuModInfo> info(1);
    g_world = plan::World();
    g_world.api = "1.2.0";
    g_pkgs.clear();
    if (!keep_changes) { download::cancel(-1); g_coll.clear(); g_notice.clear(); g_papply.on = false; }
    for (uint32_t i = 0; i < count; ++i) {
        KyubuModInfo& in = info[0];
        std::memset(&in, 0, sizeof in);
        in.size = sizeof in;
        if (api->mods_get(g_mod->handle(), i, &in) != KY_OK) continue;
        {
            uint8_t h[32];
            if (api->mods_package_hash(g_mod->handle(), in.id, h) == KY_OK) g_hashes[in.id] = browse::hex(h);
        }
        list::Mod m;
        m.id = widen(in.id);
        m.name = in.name[0] ? widen(in.name) : m.id;
        m.version = widen(in.version);
        m.authors = widen(in.authors);
        m.description = widen(in.description);
        m.reason = widen(in.reason);
        m.state = in.state <= static_cast<uint32_t>(list::State::Missing) ? static_cast<list::State>(in.state) : list::State::Failed;
        if (m.state == list::State::New) { ++g_new_count; by_hand.insert(in.id); }   // arrived by hand: Unverified from now on
        if (by_hand.count(in.id)) { m.mark = list::Mark::Unverified; m.mark_reason = widen(hand::kReason); }
        const uint32_t nd = in.dependency_count < KY_MOD_DEPS_MAX ? in.dependency_count : KY_MOD_DEPS_MAX;
        const uint32_t nb = in.dependent_count < KY_MOD_DEPS_MAX ? in.dependent_count : KY_MOD_DEPS_MAX;
        for (uint32_t d = 0; d < nd; ++d) m.needs.push_back({widen(in.dependencies[d].id), widen(in.dependencies[d].req), in.dependencies[d].optional != 0, false});
        for (uint32_t d = 0; d < nb; ++d) m.needed_by.push_back({widen(in.dependents[d].id), widen(in.dependents[d].req), in.dependents[d].optional != 0, false});
        plan::Installed pi;
        pi.id = in.id;
        pi.version = in.version;
        pi.enabled = in.state != KY_MOD_STATE_DISABLED && in.state != KY_MOD_STATE_MISSING;
        for (uint32_t d = 0; d < nd; ++d)
            if (!in.dependencies[d].optional) pi.needs[in.dependencies[d].id] = in.dependencies[d].req[0] ? in.dependencies[d].req : "*";
        g_world.installed.push_back(pi);
        actions::Pkg pk;
        pk.version = in.version;
        pk.zip = in.is_zip != 0;
        pk.problem = list::is_problem(m.state);
        g_pkgs[in.id] = pk;
        mods.push_back(std::move(m));
    }
    if (by_hand.size() != by_hand_before) write_hand(by_hand);
    g_view.set_mods(std::move(mods));
    load_profiles();
    if (!keep_changes) g_pack_stopped.clear();
    apply_catalog();
    g_mod->info("mod_manager: list read: %u package(s), %d new, %u shown", count, g_new_count, static_cast<unsigned>(g_view.shown.size()));
}

// Why a draft fails mods_option_check, in the words of the declaration.
std::string reason_for(const form::Field& f) {
    if (f.type == form::Type::Bool) return "Must be ON or OFF.";
    if (f.type == form::Type::String) return f.choices.empty() ? "This text is not allowed." : "Not one of the choices.";
    if (f.draft.empty() || form::num(f.draft, 1e300) > 1e200) return f.type == form::Type::Int ? "Type a whole number." : "Type a number.";
    if (!f.min.empty() && !f.max.empty()) return "Allowed: " + f.min + " to " + f.max + ".";
    if (!f.min.empty()) return "At least " + f.min + ".";
    if (!f.max.empty()) return "At most " + f.max + ".";
    return "This number is not allowed.";
}

std::string utf8_of(const std::wstring& w) {
    if (w.empty()) return "";
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), ' ');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

// The form of a package: read once with mods_option_get, then kept (its drafts too) until the page opens again.
form::Form& form_of(const std::wstring& wide_id) {
    const std::string id = utf8_of(wide_id);
    auto it = g_forms.find(id);
    if (it != g_forms.end()) return it->second;
    form::Form& fm = g_forms[id];
    fm.id = id;
    const KyubuApi* api = g_mod->api();
    const uint32_t n = api->mods_option_count(g_mod->handle(), id.c_str());
    for (uint32_t i = 0; i < n; ++i) {
        KyubuOptionInfo oi;
        std::memset(&oi, 0, sizeof oi);
        if (api->mods_option_get(g_mod->handle(), id.c_str(), i, &oi) != KY_OK) continue;
        fm.fields.push_back(form::make_field(oi.key, oi.type, oi.label, oi.group, oi.description, oi.def, oi.value, oi.min, oi.max, oi.choices, oi.restart != 0));
    }
    fm.check = [id](const form::Field& f, std::string& why) {
        const bool ok = g_mod->api()->mods_option_check(g_mod->handle(), id.c_str(), f.key.c_str(), f.draft.c_str()) == KY_OK;
        if (!ok) why = reason_for(f);
        return ok;
    };
    g_mod->info("mod_manager: form of %s: %u option(s)", id.c_str(), static_cast<unsigned>(fm.fields.size()));
    return fm;
}

form::Form* editing_form() {
    for (auto& kv : g_forms) if (kv.second.editing >= 0) return &kv.second;
    return nullptr;
}
void blur_forms() { for (auto& kv : g_forms) kv.second.blur(); }

// The page's count of changed options, and whether one of them is not allowed (that blocks APPLY).
void count_options() {
    int n = 0;
    bool bad = false;
    for (auto& kv : g_forms) { n += kv.second.changes(); bad = bad || kv.second.invalid(); }
    g_state.options = n;
    g_options_invalid = bad;
}

// --- the collected changes --------------------------------------------------------------------------------------------

std::string record_path() {
    char dir[300] = {};
    if (g_mod->api()->mod_data_dir(g_mod->handle(), dir, sizeof dir) != KY_OK) return "";
    CreateDirectoryA(dir, nullptr);
    return std::string(dir) + "\\last-apply.txt";
}

std::string read_record_file() {
    const std::string path = record_path();
    std::string text;
    if (FILE* f = path.empty() ? nullptr : std::fopen(path.c_str(), "rb")) {
        char buf[512];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
        std::fclose(f);
    }
    return text;
}

KyubuModOp make_op(const actions::Queued& q) {
    KyubuModOp op;
    std::memset(&op, 0, sizeof op);
    op.kind = q.kind == actions::Queued::Enable ? KY_MOD_OP_ENABLE : q.kind == actions::Queued::Disable ? KY_MOD_OP_DISABLE
            : q.kind == actions::Queued::Delete ? KY_MOD_OP_DELETE : q.kind == actions::Queued::Config ? KY_MOD_OP_CONFIG : KY_MOD_OP_INSTALL;
    std::snprintf(op.id, sizeof op.id, "%s", q.id.c_str());
    std::snprintf(op.version, sizeof op.version, "%s", q.version.c_str());
    std::snprintf(op.from, sizeof op.from, "%s", q.from.c_str());
    std::snprintf(op.key, sizeof op.key, "%s", q.key.c_str());
    std::snprintf(op.value, sizeof op.value, "%s", q.value.c_str());
    op.keep_settings = 1;
    return op;
}

// Queues the ops in a fresh queue. An optional config op that is refused is logged and left out; any other refusal
// empties the queue again and returns false.
bool queue_all(const std::vector<actions::Queued>& q) {
    const KyubuApi* api = g_mod->api();
    api->mods_queue_clear(g_mod->handle());
    int failed = 0;
    for (const actions::Queued& item : q) {
        const KyubuModOp op = make_op(item);
        const KyubuResult r = api->mods_queue(g_mod->handle(), &op);
        if (r == KY_OK) continue;
        if (item.kind == actions::Queued::Config && item.optional) {
            g_mod->info("mod_manager: option %s.%s left out: the package does not take it (result %d)", item.id.c_str(), item.key.c_str(), static_cast<int>(r));
            continue;
        }
        ++failed;
        g_mod->warn("mod_manager: queue %s%s%s failed (result %d)", item.id.c_str(), item.key.empty() ? "" : ".", item.key.c_str(), static_cast<int>(r));
    }
    if (failed) api->mods_queue_clear(g_mod->handle());
    return failed == 0;
}

void restart_now(size_t queued) {
    download::forget_all();   // the staged zips are the queue's now: the loader installs them at boot
    const KyubuResult r = g_mod->api()->game_restart(g_mod->handle());
    g_mod->info("mod_manager: restart requested (%u queued): result %d", static_cast<unsigned>(queued), static_cast<int>(r));
    if (r != KY_OK) g_notice = "Your changes are saved. Restart the game to apply them.";
}

// Queues the ops, then restarts; the notice says what happened when something is refused.
void queue_and_restart(const std::vector<actions::Queued>& q) {
    const KyubuApi* api = g_mod->api();
    api->mods_queue_clear(g_mod->handle());
    int failed = 0;
    for (const actions::Queued& item : q) {
        const KyubuModOp op = make_op(item);
        const KyubuResult r = api->mods_queue(g_mod->handle(), &op);
        if (r != KY_OK) { ++failed; g_mod->warn("mod_manager: queue %s failed (result %d)", item.id.c_str(), static_cast<int>(r)); }
    }
    if (failed) { g_notice = "Some changes could not be queued, see the log."; return; }
    download::forget_all();   // the staged zips are the queue's now: the loader installs them at boot
    const KyubuResult r = api->game_restart(g_mod->handle());
    g_mod->info("mod_manager: restart requested (%u queued): result %d", static_cast<unsigned>(q.size()), static_cast<int>(r));
    if (r != KY_OK) g_notice = "Your changes are saved. Restart the game to apply them.";
}

form::Form* current_form() {
    const list::Mod* cur = g_view.current();
    return cur ? &form_of(cur->id) : nullptr;
}

// Install / Modpacks: the selected pack and its plan against the installed mods. The option fields of a mod that stays come
// from its form (read once with mods_option_get).
const cindex::Pack* current_pack() {
    const list::Mod* m = g_pack_view.current();
    return m ? packs::find_pack(g_loaded.index, utf8_of(m->id)) : nullptr;
}

// The selected local profile (its row id is "profile:<id>"), or null.
const cindex::Pack* current_profile() {
    const list::Mod* m = g_pack_view.current();
    const std::wstring pre = profile::kRowPrefix;
    if (!m || m->id.compare(0, pre.size(), pre) != 0) return nullptr;
    const std::string id = utf8_of(m->id.substr(pre.size()));
    for (const cindex::Pack& p : g_profiles) if (p.id == id) return &p;
    return nullptr;
}

// The option fields of an installed mod (its form, read once), or null for a mod that is not installed.
const std::vector<form::Field>* installed_fields(const std::string& id) {
    if (!g_view.find(widen(id.c_str()))) return nullptr;
    return &form_of(widen(id.c_str())).fields;
}

std::string profiles_dir() {
    char dir[300] = {};
    if (g_mod->api()->mod_data_dir(g_mod->handle(), dir, sizeof dir) != KY_OK) return "";
    CreateDirectoryA(dir, nullptr);
    const std::string p = std::string(dir) + "\\profiles";
    CreateDirectoryA(p.c_str(), nullptr);
    return p;
}

// Reads every profile file; a file that does not parse is logged and not listed.
void load_profiles() {
    g_profiles.clear();
    const std::string dir = profiles_dir();
    if (dir.empty()) return;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*.json").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::string text;
        if (FILE* f = std::fopen((dir + "\\" + fd.cFileName).c_str(), "rb")) {
            char buf[4096];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
            std::fclose(f);
        }
        cindex::Pack p;
        if (profile::from_json(text, p)) g_profiles.push_back(std::move(p));
        else g_mod->warn("mod_manager: profile %s is not readable, not listed", fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

bool save_profile(const cindex::Pack& p) {
    const std::string dir = profiles_dir();
    if (dir.empty()) return false;
    const std::string path = dir + "\\" + p.id + ".json", text = profile::to_json(p);
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    return std::fclose(f) == 0 && ok;
}

// The package of `id` at `version` that the loader kept in kyubu\backup\ (a zip or a folder), as a queue path; "" when none.
std::string backup_of(const std::string& id, const std::string& version) {
    char dir[300] = {};
    if (g_mod->api()->game_dir(dir, sizeof dir) != KY_OK) return "";
    const std::string base = std::string(dir) + "\\kyubu\\backup\\" + id + "-" + version;
    const DWORD z = GetFileAttributesA((base + ".zip").c_str());
    if (z != INVALID_FILE_ATTRIBUTES && !(z & FILE_ATTRIBUTE_DIRECTORY)) return "kyubu/backup/" + id + "-" + version + ".zip";
    const DWORD d = GetFileAttributesA(base.c_str());
    if (d != INVALID_FILE_ATTRIBUTES && (d & FILE_ATTRIBUTE_DIRECTORY)) return "kyubu/backup/" + id + "-" + version;
    return "";
}

std::wstring pack_mod_name(const std::string& id) {
    if (const cindex::Mod* m = browse::find_mod(g_loaded.index, id)) return m->name.empty() ? widen(id.c_str()) : browse::utf8_wide(m->name);
    if (const list::Mod* m = g_view.find(widen(id.c_str()))) return m->name;
    return widen(id.c_str());
}

const packs::Plan& pack_plan(const cindex::Pack& p) {
    g_pack_plan = packs::plan_of(p, g_world, installed_fields);
    std::string said = p.id + (g_pack_plan.result.ok ? ": ok" : ": refused, " + g_pack_plan.result.reason);
    for (const plan::Op& op : g_pack_plan.result.ops) {
        static const char* kinds[] = {"keep", "download", "enable", "disable"};
        said += std::string(", ") + kinds[static_cast<int>(op.kind)] + " " + op.id + " " + (op.from.empty() ? "" : op.from + ">") + op.version;
    }
    said += ", settings " + std::to_string(g_pack_plan.settings) + (g_pack_plan.settings_exact ? "" : "+");
    if (said != g_pack_planned) {
        g_pack_planned = said;
        g_mod->info("mod_manager: pack plan %s", said.c_str());
    }
    return g_pack_plan;
}

bool downloads_running();

// The Actions menu of the selected mod; always four rows.
void rebuild_menu() {
    if (g_state.packs()) {
        const bool busy = g_papply.on || downloads_running();
        const bool changes = g_coll.count() > 0 || g_state.options > 0;
        if (const cindex::Pack* pr = current_profile()) {
            actions::Row r, d;
            r.act = actions::Act::Restore;
            r.label = "Restore";
            d.act = actions::Act::DeleteProfile;
            d.label = "Delete profile";
            d.kind = 2;
            const profile::Work w = profile::restore_work(*pr, profile::restore_plan(*pr, g_world, backup_of), g_world, installed_fields);
            if (w.empty()) { r.kind = 1; r.tip = "This setup is the one in use."; }
            else if (changes) { r.kind = 1; r.tip = "Apply or discard the changes first."; }
            else if (busy) { r.kind = 1; r.tip = "Wait for the downloads to finish."; }
            g_menu = {r, d};
            return;
        }
        actions::Row a;
        a.act = actions::Act::ApplyPack;
        a.label = "Apply modpack";
        const cindex::Pack* p = current_pack();
        if (!p) { a.kind = 1; a.tip = "No modpack is selected."; }
        else {
            const packs::Plan& pl = pack_plan(*p);
            if (!pl.result.ok) { a.kind = 1; a.tip = "It cannot be applied: " + pl.result.reason + "."; }
            else if (profile::in_use(pl.result, profile::pack_work(*p, pl.result, g_world, installed_fields))) { a.kind = 1; a.tip = "This modpack is the setup in use."; }
            else if (g_papply.on) { a.kind = 1; a.tip = "The modpack is downloading."; }
            else if (changes) { a.kind = 1; a.tip = "Apply or discard the changes first."; }
            else if (busy) { a.kind = 1; a.tip = "Wait for the downloads to finish."; }
            else if (g_loaded.state != cindex::State::Online) {
                for (const plan::Op& op : pl.result.ops)
                    if (op.kind == plan::Kind::Download) { a.kind = 1; a.tip = "The catalog is offline. Downloads wait for it."; break; }
            }
        }
        g_menu = {a};
        return;
    }
    if (g_state.tab == 1) {
        g_menu.clear();
        const list::Mod* c = g_cat_view.current();
        actions::Row d, s;
        d.act = actions::Act::Download;
        s.act = actions::Act::ShowInMyMods;
        s.label = "Show in My mods";
        if (!c) {
            d.label = "Download";
            d.kind = s.kind = 1;
            d.tip = s.tip = "No mod is selected.";
        } else {
            const browse::Menu bm = browse::menu_of(*c, g_loaded.state == cindex::State::Online);
            d.label = utf8_of(bm.download_label);
            d.tip = utf8_of(bm.tip);
            if (g_coll.dl.count(utf8_of(c->id))) { d.label = "Downloaded"; d.kind = 1; d.tip = "It is installed by Apply & restart."; }
            else if (!bm.download_ok) d.kind = 1;
            if (c->have.empty()) { s.kind = 1; s.tip = "It is not installed."; }
        }
        g_menu = {d, s};
        return;
    }
    const list::Mod* cur = g_view.current();
    const bool undo = !read_record_file().empty();
    if (!cur) {
        g_menu = actions::menu(g_world, g_coll, "", actions::Pkg(), undo);
        for (int i = 0; i < 3; ++i) { g_menu[i].kind = 1; g_menu[i].tip = "No mod is selected."; }
        return;
    }
    const std::string id = utf8_of(cur->id);
    actions::Pkg pk = g_pkgs[id];
    if (form::Form* fm = current_form()) {
        pk.has_options = !fm->fields.empty();
        for (const form::Field& f : fm->fields) if (form::differs_from_default(f)) pk.options_changed = true;
    }
    g_menu = actions::menu(g_world, g_coll, id, pk, undo);
    actions::Row u;
    u.act = actions::Act::Download;
    if (g_coll.dl.count(id)) { u.label = "Update downloaded"; u.kind = 1; u.tip = "It is installed by Apply & restart."; }
    else if (cur->update_to.empty()) { u.label = "Update"; u.kind = 1; u.tip = g_loaded.state == cindex::State::None ? "No catalog is loaded." : "No newer version in the catalog."; }
    else {
        u.label = "Update to " + utf8_of(cur->update_to);
        if (g_loaded.state != cindex::State::Online) { u.kind = 1; u.tip = "The catalog is offline. Downloads wait for it."; }
    }
    g_menu.push_back(u);
}

// --- Downloads -----------------------------------------------------------------------------------------------------------------

std::string catalog_url();
volatile uint32_t g_dl_started = 0, g_dl_staged = 0, g_dl_failed = 0;
std::set<std::string> g_dl_logged;   // "<id>-<version>" whose end was logged

bool downloads_running() {
    for (const download::Item& it : download::snapshot())
        if (it.st == download::St::Waiting || it.st == download::St::Running) return true;
    return false;
}

std::string staging_dir() {
    char dir[300] = {};
    if (g_mod->api()->game_dir(dir, sizeof dir) != KY_OK) return "";
    return std::string(dir) + "\\kyubu\\staging";
}

const cindex::Version* catalog_version(const std::string& id, const std::string& version) {
    if (g_loaded.state != cindex::State::Online && g_loaded.state != cindex::State::Offline) return nullptr;
    const cindex::Mod* m = browse::find_mod(g_loaded.index, id);
    if (!m) return nullptr;
    for (const cindex::Version& v : m->versions) if (v.version == version) return &v;
    return nullptr;
}

// Plans the download of `id` with its missing dependencies and hands them to the worker.
void start_download(const std::string& id) {
    if (g_loaded.state != cindex::State::Online) { g_notice = "The catalog is offline. Downloads wait for it."; return; }
    const std::string dir = staging_dir();
    if (dir.empty()) { g_notice = "Cannot find the game folder."; return; }
    download::init(dir);
    std::vector<plan::Op> fetch;
    actions::Collected before = g_coll;
    const actions::Outcome o = actions::download(g_world, g_coll, id, fetch);
    if (!o.ok) { g_notice = o.reason; g_mod->info("mod_manager: download of %s refused: %s", id.c_str(), o.reason.c_str()); return; }
    const std::string base = fetch::with_slash(catalog_url());
    for (const plan::Op& op : fetch) {
        const cindex::Version* v = catalog_version(op.id, op.version);
        if (!v) { g_coll = before; g_notice = "The catalog has no " + op.id + " " + op.version + "."; return; }
        download::Item it;
        it.id = op.id;
        it.version = op.version;
        it.url = base + v->url;
        it.sha256 = v->sha256;
        it.size = v->size;
        it.group = g_coll.dl[op.id].group;
        g_dl_logged.erase(op.id + "-" + op.version);
        if (!download::add(it)) { g_coll = before; download::cancel(it.group); g_notice = "The download could not start."; return; }
        g_dl_started = g_dl_started + 1;
        g_mod->info("mod_manager: download %s %s (%ld bytes) from %s", op.id.c_str(), op.version.c_str(), v->size, it.url.c_str());
    }
    if (!o.also.empty()) {
        g_notice = "Also: ";
        for (size_t i = 0; i < o.also.size(); ++i) g_notice += (i ? ", " : "") + o.also[i];
    }
}

// Each frame: a failed item drops its whole download (the mod and the dependencies it brought), with the reason on the bar.
void poll_downloads() {
    for (const download::Item& it : download::snapshot()) {
        const std::string key = it.id + "-" + it.version;
        if (it.st == download::St::Staged && !g_dl_logged.count(key)) {
            g_dl_logged.insert(key);
            g_dl_staged = g_dl_staged + 1;
            g_mod->info("mod_manager: download %s staged in kyubu\\staging", key.c_str());
        } else if (it.st == download::St::Failed && !g_dl_logged.count(key)) {
            g_dl_logged.insert(key);
            g_dl_failed = g_dl_failed + 1;
            g_mod->warn("mod_manager: download %s failed: %s", key.c_str(), it.reason.c_str());
            const std::vector<std::string> dropped = actions::drop_group(g_coll, it.group);
            download::cancel(it.group);
            g_notice = it.id + ": " + it.reason;
            if (g_papply.on && it.group == g_papply.group) {   // Apply modpack stops here: nothing is queued
                g_papply.on = false;
                g_pack_stopped[g_papply.pack_id] = browse::wide(it.id + " " + it.version) + L": " + browse::utf8_wide(it.reason) + L" Nothing was changed.";
                g_mod->warn("mod_manager: apply modpack %s stopped: %s %s failed", g_papply.pack_id.c_str(), it.id.c_str(), it.version.c_str());
            }
            (void)dropped;
        }
    }
}

// The progress text of a collected download: "QUEUED" once staged, else the percentage.
std::wstring download_tag(const std::string& id) {
    for (const download::Item& it : download::snapshot()) {
        if (it.id != id) continue;
        if (it.st == download::St::Staged) return L"QUEUED";
        if (it.st == download::St::Waiting) return L"WAITING";
        wchar_t b[16];
        std::swprintf(b, 16, L"%ld%%", it.size > 0 ? it.got * 100 / it.size : 0L);
        return b;
    }
    return L"QUEUED";
}

// The bar's download line: "DOWNLOADING cat_lib 40%", empty when nothing runs.
std::wstring download_line() {
    for (const download::Item& it : download::snapshot())
        if (it.st == download::St::Running || it.st == download::St::Waiting)
            return L"DOWNLOADING " + browse::wide(it.id) + L" " + download_tag(it.id);
    return L"";
}

void do_apply() {
    count_options();
    if (g_options_invalid) { g_apply_blocked = g_apply_blocked + 1; g_mod->info("mod_manager: APPLY blocked: an option has a value that is not allowed"); return; }
    if (g_state.changes() == 0) { g_mod->info("mod_manager: APPLY clicked: no changes"); return; }
    if (downloads_running()) { g_apply_blocked = g_apply_blocked + 1; g_notice = "Wait for the downloads to finish."; g_mod->info("mod_manager: APPLY blocked: a download is running"); return; }
    const KyubuApi* api = g_mod->api();
    g_notice.clear();
    std::vector<actions::OptionUndo> undo_options;
    int failed = 0;
    for (auto& kv : g_forms)
        for (form::Field& f : kv.second.fields) {
            if (!form::dirty(f)) continue;
            const KyubuResult r = api->mods_option_set(g_mod->handle(), kv.first.c_str(), f.key.c_str(), f.draft.c_str());
            if (r != KY_OK) { ++failed; g_mod->warn("mod_manager: option %s.%s was refused (result %d)", kv.first.c_str(), f.key.c_str(), static_cast<int>(r)); continue; }
            undo_options.push_back({kv.first, f.key, f.value});
            f.value = f.draft;
        }
    const std::vector<actions::Queued> q = actions::ops(g_world, g_coll, g_pkgs);
    const std::string record = actions::write_record(actions::reverse(q, g_pkgs), undo_options);
    const std::string path = record_path();
    if (FILE* f = path.empty() ? nullptr : std::fopen(path.c_str(), "wb")) { std::fwrite(record.data(), 1, record.size(), f); std::fclose(f); }
    else g_mod->warn("mod_manager: cannot write the undo record");
    g_applies = g_applies + 1;
    g_mod->info("mod_manager: APPLY: %u option(s) written, %u change(s) queued, %d refused", static_cast<unsigned>(undo_options.size()), static_cast<unsigned>(q.size()), failed);
    if (failed) g_notice = "Some options were refused, see the log.";
    g_coll.clear();
    count_options();
    if ((!q.empty() || g_new_count > 0) && !failed) queue_and_restart(q);   // a new package loads only after a restart
}

void do_undo() {
    if (g_state.changes() > 0) { g_notice = "Apply or discard the changes first."; return; }
    std::vector<actions::Queued> ops;
    std::vector<actions::OptionUndo> options;
    actions::read_record(read_record_file(), ops, options);
    if (ops.empty() && options.empty()) { g_notice = "Nothing to undo."; return; }
    for (const actions::OptionUndo& o : options)
        if (g_mod->api()->mods_option_set(g_mod->handle(), o.id.c_str(), o.key.c_str(), o.value.c_str()) != KY_OK)
            g_mod->warn("mod_manager: undo: option %s.%s was refused", o.id.c_str(), o.key.c_str());
    const std::string path = record_path();
    if (!path.empty()) DeleteFileA(path.c_str());
    g_forms.clear();
    g_undos = g_undos + 1;
    g_mod->info("mod_manager: UNDO: %u option(s) restored, %u change(s) queued", static_cast<unsigned>(options.size()), static_cast<unsigned>(ops.size()));
    if (ops.empty()) g_notice = "Options undone.";
    else queue_and_restart(ops);
}

// --- Apply modpack and Restore ----------------------------------------------------------------------------------------------------

// Queues a pack's or a profile's work, sets the options of the mods that stay, drops the undo record (the profile is the way back
// now) and restarts. False, with the notice set, when the queue refused an op: then nothing has changed.
bool run_work(const profile::Work& w, const char* what) {
    if (!queue_all(w.queue)) { g_notice = "The changes could not be queued, see the log. Nothing was changed."; return false; }
    int refused = 0;
    for (const profile::OptSet& o : w.live)
        if (g_mod->api()->mods_option_set(g_mod->handle(), o.id.c_str(), o.key.c_str(), o.value.c_str()) != KY_OK) {
            ++refused;
            g_mod->warn("mod_manager: %s: option %s.%s = %s was refused", what, o.id.c_str(), o.key.c_str(), o.value.c_str());
        }
    const std::string rec = record_path();
    if (!rec.empty()) DeleteFileA(rec.c_str());
    g_forms.clear();
    g_mod->info("mod_manager: %s: %u op(s) queued, %u option(s) set, %d refused", what, static_cast<unsigned>(w.queue.size()),
                static_cast<unsigned>(w.live.size()), refused);
    restart_now(w.queue.size());
    return true;
}

// The last step of Apply modpack, once every download is staged: save "Before <pack>", then queue and restart.
void finish_pack_apply() {
    g_papply.on = false;
    const cindex::Pack* p = packs::find_pack(g_loaded.index, g_papply.pack_id);
    if (!p) { g_notice = "The modpack is no longer in the catalog."; download::cancel(g_papply.group); return; }
    const packs::Plan pl = packs::plan_of(*p, g_world, installed_fields);
    if (!pl.result.ok) { g_notice = "It cannot be applied: " + pl.result.reason + "."; download::cancel(g_papply.group); return; }
    const profile::Work w = profile::pack_work(*p, pl.result, g_world, installed_fields);
    const std::string name = p->name.empty() ? p->id : p->name;
    const cindex::Pack before = profile::capture(g_world, installed_fields, profile::before_id(p->id), "Before " + name,
                                                 "Your mods and options from before " + name + " was applied.");
    if (!save_profile(before)) {
        g_notice = "Cannot save the profile \"Before " + name + "\". Nothing was changed.";
        g_mod->warn("mod_manager: apply %s stopped: cannot write the profile %s", p->id.c_str(), before.id.c_str());
        download::cancel(g_papply.group);
        return;
    }
    g_mod->info("mod_manager: profile %s saved: %u mod(s)", before.id.c_str(), static_cast<unsigned>(before.mods.size()));
    load_profiles();
    g_pack_applies = g_pack_applies + 1;
    if (!run_work(w, ("apply modpack " + p->id).c_str())) download::cancel(g_papply.group);
}

// Apply modpack: the downloads first (one group; a failure stops it with nothing queued), then finish_pack_apply.
void start_pack_apply() {
    const cindex::Pack* p = current_pack();
    if (!p) return;
    const packs::Plan pl = packs::plan_of(*p, g_world, installed_fields);
    if (!pl.result.ok) { g_notice = "It cannot be applied: " + pl.result.reason + "."; return; }
    g_pack_stopped.erase(p->id);
    g_papply = PackApply();
    g_papply.pack_id = p->id;
    g_papply.group = g_coll.next_group++;
    std::vector<plan::Op> fetch;
    for (const plan::Op& op : pl.result.ops) if (op.kind == plan::Kind::Download) fetch.push_back(op);
    g_mod->info("mod_manager: apply modpack %s: %u download(s)", p->id.c_str(), static_cast<unsigned>(fetch.size()));
    if (fetch.empty()) { finish_pack_apply(); return; }
    const std::string dir = staging_dir();
    if (dir.empty()) { g_notice = "Cannot find the game folder."; return; }
    download::init(dir);
    const std::string base = fetch::with_slash(catalog_url());
    for (const plan::Op& op : fetch) {
        const cindex::Version* v = catalog_version(op.id, op.version);
        if (!v) { download::cancel(g_papply.group); g_notice = "The catalog has no " + op.id + " " + op.version + "."; return; }
        download::Item it;
        it.id = op.id;
        it.version = op.version;
        it.url = base + v->url;
        it.sha256 = v->sha256;
        it.size = v->size;
        it.group = g_papply.group;
        g_dl_logged.erase(op.id + "-" + op.version);
        if (!download::add(it)) { download::cancel(g_papply.group); g_notice = "The download could not start."; return; }
        g_dl_started = g_dl_started + 1;
        g_mod->info("mod_manager: download %s %s (%ld bytes) from %s", op.id.c_str(), op.version.c_str(), v->size, it.url.c_str());
    }
    g_papply.on = true;
    g_notice = "Downloading the modpack...";
}

// Each frame while a pack downloads: finish when every item of its group is staged. poll_downloads stops it on a failure.
void poll_pack_apply() {
    if (!g_papply.on) return;
    int left = 0, staged = 0;
    for (const download::Item& it : download::snapshot()) {
        if (it.group != g_papply.group) continue;
        if (it.st == download::St::Staged) ++staged;
        else ++left;
    }
    if (left > 0) return;
    if (staged == 0) { g_papply.on = false; g_notice = "The modpack's downloads were cancelled."; return; }
    finish_pack_apply();
}

void do_restore() {
    const cindex::Pack* pr = current_profile();
    if (!pr) return;
    const std::vector<profile::Step> steps = profile::restore_plan(*pr, g_world, backup_of);
    const profile::Work w = profile::restore_work(*pr, steps, g_world, installed_fields);
    if (w.empty()) { g_notice = "This setup is the one in use."; return; }
    g_restores = g_restores + 1;
    run_work(w, ("restore " + pr->id).c_str());
}

void do_action(actions::Act act) {
    const list::Mod* cur = g_view.current();
    g_notice.clear();
    if (act == actions::Act::ShowInMyMods) {
        const list::Mod* c = g_cat_view.current();
        if (c && g_view.jump_to(c->id)) { g_state.tab = 0; g_state.typing = false; }
        return;
    }
    if (act == actions::Act::Download) {
        const list::Mod* m = g_state.tab == 1 ? g_cat_view.current() : cur;
        if (m) start_download(utf8_of(m->id));
        return;
    }
    if (act == actions::Act::Undo) { do_undo(); return; }
    if (act == actions::Act::ApplyPack) { start_pack_apply(); return; }
    if (act == actions::Act::Restore) { do_restore(); return; }
    if (act == actions::Act::DeleteProfile) {
        if (const cindex::Pack* pr = current_profile()) {
            g_confirm_delete = true;
            g_delete_profile = true;
            g_delete_id = pr->id;
            g_delete_name = browse::utf8_wide(pr->name);
            g_state.confirm = true;
            g_state.menu = false;
        }
        return;
    }
    if (!cur) return;
    const std::string id = utf8_of(cur->id);
    if (act == actions::Act::Toggle) {
        const actions::Outcome o = actions::toggle(g_world, g_coll, id);
        if (!o.ok) g_notice = o.reason;
        else if (!o.also.empty()) {
            g_notice = "Also switches: ";
            for (size_t i = 0; i < o.also.size(); ++i) g_notice += (i ? ", " : "") + o.also[i];
        }
    } else if (act == actions::Act::ResetOptions) {
        if (form::Form* fm = current_form()) {
            for (size_t i = 0; i < fm->fields.size(); ++i) fm->to_default(static_cast<int>(i));
            count_options();
        }
    } else if (act == actions::Act::Delete) {
        if (g_coll.del.count(id)) actions::cancel_delete(g_coll, id);
        else {
            g_confirm_delete = true;
            g_delete_profile = false;
            g_delete_id = id;
            g_delete_name = cur->name;
            g_state.confirm = true;
            g_state.menu = false;
        }
    }
}

bool build_page(uintptr_t gc) {
    void* scene = reinterpret_cast<void*>(field(gc, kStartScene));
    void* black = reinterpret_cast<void*>(field(gc, kBlackwidget));
    void* plate = reinterpret_cast<void*>(field(gc, kCreationBack));
    void* menu_node = reinterpret_cast<void*>(field(gc, kStartMenuNode));
    if (!scene || !black || !plate || !menu_node) return false;
    // creation order is draw order: frames first, then what sits on them, the menu's overlays last
    Nodes n;
    n.back = g_clone(plate, scene);
    n.frame = g_clone(black, scene);
    n.tab[0] = g_clone(black, scene);   // tabs and ACTIONS are frames: a plate is never shorter than 67 px, a frame can be any height
    n.tab[1] = g_clone(black, scene);
    n.sub[0] = g_clone(black, scene);
    n.sub[1] = g_clone(black, scene);
    n.list = g_clone(black, scene);
    n.search = g_clone(black, scene);
    n.details = g_clone(black, scene);
    n.bar = g_clone(black, scene);
    n.discard = g_clone(plate, scene);
    n.apply = g_clone(plate, scene);
    n.actions = g_clone(black, scene);
    n.menu = g_clone(black, scene);
    n.confirm = g_clone(black, scene);
    n.yes = g_clone(plate, scene);
    n.no = g_clone(plate, scene);
    void* all[] = {n.back, n.frame, n.tab[0], n.tab[1], n.sub[0], n.sub[1], n.list, n.search, n.details, n.bar, n.discard, n.apply, n.actions, n.menu, n.confirm, n.yes, n.no};
    for (void* node : all)
        if (!node || !widget_of(node)) return false;
    g_n = n;
    set_label(n.discard, L"");   // DISCARD and APPLY are drawn by the page, larger than the plate's own text
    set_label(n.apply, L"");
    set_label(n.yes, L"YES");
    set_label(n.no, L"NO");
    // the text is drawn by the title menu's draw: its node goes last, so the clones are drawn before it
    g_set_parent(menu_node, scene);
    return true;
}

void write_labels(bool blank) {
    const wchar_t orig[3] = {L'S', L'O', g_third_char};
    for (int i = 0; i < 3; ++i) {
        const uintptr_t at = g_mod->api()->rva_to_addr(kLabelRvas[i]);
        const wchar_t c = blank ? 0 : orig[i];
        if (at) g_mod->api()->mem_write(g_mod->handle(), at, &c, sizeof c);
    }
    g_labels_blank = blank;
}

void close_page() { g_state.open = false; }

void open_mods_folder() {
    char dir[300] = {};
    if (g_mod->api()->game_dir(dir, sizeof dir) != KY_OK) { g_notice = "Cannot find the mods folder."; return; }
    const std::string path = std::string(dir) + "\\mods";
    g_folder_clicks = g_folder_clicks + 1;
    const INT_PTR r = reinterpret_cast<INT_PTR>(ShellExecuteA(nullptr, "open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    g_mod->info("mod_manager: Open mods folder: %s (result %d)", path.c_str(), static_cast<int>(r));
    if (r <= 32) g_notice = "The folder could not be opened.";
}

void handle_click(const page::Layout& l) {
    page::State& s = g_state;
    if (s.confirm && g_confirm_delete) {
        if (cursor_in(g_n.yes) && g_delete_profile) {
            const std::string dir = profiles_dir();
            const bool ok = !dir.empty() && DeleteFileA((dir + "\\" + g_delete_id + ".json").c_str());
            g_notice = ok ? "" : "The profile could not be deleted.";
            g_confirm_delete = g_delete_profile = false;
            s.confirm = false;
            g_mod->info("mod_manager: profile %s deleted: %s", g_delete_id.c_str(), ok ? "yes" : "no");
            load_profiles();
            apply_catalog();
        } else if (cursor_in(g_n.yes)) {
            const actions::Outcome o = actions::remove(g_world, g_coll, g_delete_id);
            g_notice = o.ok ? "" : o.reason;
            g_confirm_delete = false;
            s.confirm = false;
            g_mod->info("mod_manager: delete confirmed for %s: %s", g_delete_id.c_str(), o.ok ? "marked" : o.reason.c_str());
        } else if (cursor_in(g_n.no)) { g_confirm_delete = false; s.confirm = false; }
        return;
    }
    if (s.confirm) {
        if (cursor_in(g_n.yes)) { download::cancel(-1); g_coll.clear(); g_notice.clear(); s.confirm_yes(); g_confirm_yes = g_confirm_yes + 1; g_mod->info("mod_manager: confirm yes: changes discarded, page closed"); }
        else if (cursor_in(g_n.no)) { s.confirm_no(); g_confirm_no = g_confirm_no + 1; g_mod->info("mod_manager: confirm no"); }
        return;
    }
    if (s.menu) {
        float sx = -1, sy = -1;
        const bool got = screen_cursor(sx, sy);
        if (cursor_in(g_n.actions)) { s.actions_click(); g_actions_clicks = g_actions_clicks + 1; return; }
        const int row = got ? page::menu_row_at(l, static_cast<int>(g_menu.size()), sx, sy) : -1;
        s.menu_done();
        if (row >= 0 && row < static_cast<int>(g_menu.size()) && g_menu[row].kind != 1) {
            g_menu_rows_clicked = g_menu_rows_clicked + 1;
            g_mod->info("mod_manager: Actions menu row %d clicked (%s)", row, g_menu[row].label.c_str());
            do_action(g_menu[row].act);
        }
        return;
    }
    if (!cursor_in(g_n.search)) { s.typing = false; blur_forms(); }
    if (cursor_in(g_n.back)) { s.back_click(); g_back_clicks = g_back_clicks + 1; g_mod->info("mod_manager: BACK clicked (pending %d, confirm %d, open %d)", s.changes(), s.confirm, s.open); }
    else if (cursor_in(g_n.tab[0])) { s.tab_click(0); g_tab_clicks = g_tab_clicks + 1; }
    else if (cursor_in(g_n.tab[1])) { s.tab_click(1); g_tab_clicks = g_tab_clicks + 1; }
    else if (s.tab == 1 && cursor_in(g_n.sub[0])) { s.sub_click(0); g_tab_clicks = g_tab_clicks + 1; }
    else if (s.tab == 1 && cursor_in(g_n.sub[1])) { s.sub_click(1); g_tab_clicks = g_tab_clicks + 1; g_mod->info("mod_manager: MODPACKS tab, %u pack(s)", static_cast<unsigned>(g_pack_view.mods.size())); }
    else if (cursor_in(g_n.actions)) { s.actions_click(); g_actions_clicks = g_actions_clicks + 1; rebuild_menu(); }
    else if (cursor_in(g_n.discard)) { g_discard_clicks = g_discard_clicks + 1; s.pending = 0; download::cancel(-1); g_coll.clear(); g_notice.clear(); for (auto& kv : g_forms) kv.second.discard(); count_options(); g_mod->info("mod_manager: DISCARD clicked"); }
    else if (cursor_in(g_n.apply)) {
        g_apply_clicks = g_apply_clicks + 1;
        do_apply();
    }
    else if (cursor_in(g_n.search)) { s.typing = true; }
    else if (s.tab == 1) {
        float sx = -1, sy = -1;
        if (!screen_cursor(sx, sy)) return;
        list::View& cv = cur_view();
        if (l.filter.has(sx, sy)) {
            if (sx < l.filter.x + kFilterArrowZone) cv.step_filter(-1);
            else if (sx >= l.filter.right() - kFilterArrowZone) cv.step_filter(1);
            return;
        }
        const int row = page::row_at(l, sx, sy, cv.first, static_cast<int>(cv.shown.size()));
        if (row >= 0) { cv.select_row(row); g_row_clicks = g_row_clicks + 1; }
    }
    else if (s.tab == 0) {
        float sx = -1, sy = -1;
        if (!screen_cursor(sx, sy)) return;
        for (const FormHit& h : g_form_hits) {
            if (!h.r.has(sx, sy)) continue;
            const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (form::click(*h.fm, h.field, {h.r.x, h.r.y, h.r.w, h.r.h}, sx, sy, ctrl)) {
                g_form_clicks = g_form_clicks + 1;
                s.typing = h.fm->editing >= 0;
                if (!s.typing) blur_forms();
                count_options();
                g_mod->info("mod_manager: option %s = %s (changes %d)", h.fm->fields[h.field].key.c_str(), h.fm->fields[h.field].draft.c_str(), s.changes());
            }
            return;
        }
        if (g_open_rect.has(sx, sy)) { open_mods_folder(); return; }
        if (l.filter.has(sx, sy)) {
            if (sx < l.filter.x + kFilterArrowZone) g_view.step_filter(-1);
            else if (sx >= l.filter.right() - kFilterArrowZone) g_view.step_filter(1);
            return;
        }
        const int row = page::row_at(l, sx, sy, g_view.first, static_cast<int>(g_view.shown.size()));
        if (row >= 0) { g_view.select_row(row); g_row_clicks = g_row_clicks + 1; return; }
        for (const ChipHit& c : g_chip_hits)
            if (c.r.has(sx, sy) && g_view.jump_to(c.id)) { g_chip_clicks = g_chip_clicks + 1; return; }
    }
}

// The catalog: fetched on a worker thread when the page opens, and by the console aid mod_manager:catalog. `catalog_url`
// is the option; the aid takes a file:/// address, or mods:<folder> for a folder shipped inside an installed mod (replays).
std::string g_catalog_override;

std::string catalog_url() {
    if (!g_catalog_override.empty()) return g_catalog_override;
    return g_mod->config_string("catalog_url");
}

void start_catalog() {
    const std::string url = catalog_url();
    if (url.empty()) return;
    char dir[300] = {};
    if (g_mod->api()->mod_data_dir(g_mod->handle(), dir, sizeof dir) != KY_OK) return;
    CreateDirectoryA(dir, nullptr);
    if (catalog::start(url, dir)) g_mod->info("mod_manager: fetching the catalog %s", url.c_str());
}

const char* catalog_state_name(cindex::State st) {
    switch (st) {
        case cindex::State::Online: return "online";
        case cindex::State::Offline: return "offline";
        case cindex::State::Rejected: return "rejected";
        default: return "none";
    }
}

// Builds everything the catalog decides from the last result: the marks and updates of the installed mods, the planner's
// catalog and the Install list. Called when the page reads the mods and when a fetch finishes. A rejected or missing index leaves the
// marks to the by-hand rule and the Install list empty.
void apply_catalog() {
    g_loaded = catalog::get();
    const bool known = g_loaded.state == cindex::State::Online || g_loaded.state == cindex::State::Offline;
    const cindex::Index* ix = known ? &g_loaded.index : nullptr;
    g_world.catalog = ix ? browse::plan_catalog(*ix) : std::vector<plan::CatalogMod>();
    std::map<std::string, std::string> have;
    for (list::Mod& m : g_view.mods) {
        const std::string id = utf8_of(m.id), ver = utf8_of(m.version);
        have[id] = ver;
        m.update_to.clear();
        if (!ix) continue;
        auto h = g_hashes.find(id);
        const browse::Verdict v = browse::mark_of(ix, id, h == g_hashes.end() ? "" : h->second);
        m.mark = v.mark;
        m.mark_reason = v.reason;
        m.update_to = widen(browse::update_for(ix, id, ver, g_world.api).c_str());
    }
    g_view.rebuild();
    for (plan::Installed& pi : g_world.installed) {   // a pack keeps an installed pinned version only when it is Verified (D7)
        const list::Mod* m = g_view.find(widen(pi.id.c_str()));
        pi.verified = m && m->mark == list::Mark::Verified;
    }
    g_cat_view.catalog = true;
    g_cat_view.set_mods(ix ? browse::catalog_mods(*ix, have, g_world.api) : std::vector<list::Mod>());
    g_pack_view.catalog = true;
    g_pack_view.packs = true;
    std::vector<list::Mod> pack_list = ix ? packs::pack_rows(*ix) : std::vector<list::Mod>();
    for (list::Mod& m : profile::profile_rows(g_profiles)) pack_list.push_back(std::move(m));   // local profiles, under the packs
    g_pack_view.set_mods(std::move(pack_list));
    g_pack_planned.clear();
}

// Logs each finished fetch once, from the frame.
void log_catalog() {
    bool fresh = false;
    const cindex::Loaded l = catalog::get(&fresh);
    if (!fresh) return;
    apply_catalog();
    size_t versions = 0;
    for (const auto& m : l.index.mods) versions += m.versions.size();
    g_mod->info("mod_manager: catalog %s, %u mods, %u versions, %u packs%s%s", catalog_state_name(l.state), static_cast<unsigned>(l.index.mods.size()),
                static_cast<unsigned>(versions), static_cast<unsigned>(l.index.packs.size()), l.message.empty() ? "" : ": ", l.message.c_str());
}

// The update site: the game has just shown the logo and the menu again. Everything that touches the scene happens here.
void KYUBU_CALL shown_site(KyubuCpuContext*, void*) {
    const uintptr_t gc = game_controller();
    if (!gc) return;
    void* scene = reinterpret_cast<void*>(field(gc, kStartScene));
    int32_t* scene_shown = scene ? visible_word(scene) : nullptr;
    if (!scene_shown) return;

    if (g_open_request) {
        g_open_request = false;
        if (!g_built && !g_build_failed) {
            if (build_page(gc)) { g_built = true; g_mod->info("mod_manager: page built"); }
            else { g_build_failed = 1; g_mod->warn("mod_manager: could not build the page"); }
        }
        if (g_built && *scene_shown && !g_state.open) {
            g_state.open_page();
            g_forms.clear();
            g_state.options = 0;
            g_options_invalid = false;
            refresh_mods();
            start_catalog();
            g_opens = g_opens + 1;
            g_laid_w = -1;   // force a layout
            g_mod->info("mod_manager: page opened");
        }
    }
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        const bool active = pid == GetCurrentProcessId();
        if (active && !g_was_active) g_rescan_request = true;   // the game window got the focus back
        g_was_active = active;
    }
    log_catalog();
    if (g_rescan_request) {
        g_rescan_request = false;
        if (g_state.open) refresh_mods(true, true);
    }
    if (g_state.open && !*scene_shown) close_page();   // the start scene went away (should not happen from the page)

    if (g_state.open && !g_state.confirm) {
        while (g_char_tail != g_char_head) {
            const unsigned cp = g_chars[g_char_tail];
            g_char_tail = (g_char_tail + 1) % 32;
            if (!g_state.typing) continue;
            if (form::Form* fm = editing_form()) { fm->type(cp); count_options(); }
            else cur_view().type_char(cp);
        }
        for (int b = g_backspaces; b > 0; --b)
            if (g_state.typing) {
                if (form::Form* fm = editing_form()) { fm->back(); count_options(); }
                else cur_view().backspace();
            }
        g_backspaces = 0;
        if (g_wheel != 0) {
            list::View& wv = cur_view();
            float sx = -1, sy = -1;
            const int steps = g_wheel / 120 != 0 ? g_wheel / 120 : (g_wheel > 0 ? 1 : -1);
            if (screen_cursor(sx, sy)) {
                if (g_layout.rows.has(sx, sy) || g_layout.list.has(sx, sy)) wv.scroll_rows(-steps);
                else if (g_layout.details.has(sx, sy)) { wv.info_first -= steps; wv.clamp(); }
            }
        }
        g_wheel = 0;
    } else {
        g_char_tail = g_char_head;
        g_backspaces = 0;
        g_wheel = 0;
    }
    if (g_state.open) {
        if (g_escape_pending) { g_escape_pending = false; g_escapes = g_escapes + 1; g_state.escape(); }
        if (g_click_pending) { g_click_pending = false; handle_click(g_layout); }
    }
    g_click_pending = false;
    g_escape_pending = false;
    if (!g_state.typing) blur_forms();
    if (!g_state.confirm) g_confirm_delete = false;   // Escape closed the delete box
    poll_downloads();
    poll_pack_apply();
    if (g_state.open) {
        count_options();
        g_state.pending = g_coll.count() + g_stub_pending + g_new_count;
        if (g_state.menu) rebuild_menu();
    }

    if (g_state.open) {
        const int w = *reinterpret_cast<int32_t*>(gc + kWindowW), h = *reinterpret_cast<int32_t*>(gc + kWindowH);
        const float name_w = g_name_w;
        const int menu_n = g_menu.empty() ? kMenuRows : static_cast<int>(g_menu.size());
        if (w != g_laid_w || h != g_laid_h || name_w != g_laid_name_w || menu_n != g_laid_menu_n) {
            g_laid_w = w; g_laid_h = h; g_laid_name_w = name_w; g_laid_menu_n = menu_n;
            g_layout = page::compute(w, h, g_size_w(widget_of(g_n.back)), g_size_h(widget_of(g_n.back)), name_w, menu_n);
            place(g_n.back, g_layout.back);
            place(g_n.frame, g_layout.frame);
            place(g_n.tab[0], g_layout.tab[0]);
            place(g_n.tab[1], g_layout.tab[1]);
            place(g_n.sub[0], g_layout.sub[0]);
            place(g_n.sub[1], g_layout.sub[1]);
            place(g_n.list, g_layout.list);
            place(g_n.search, g_layout.search);
            g_view.rows = std::max(1, page::rows_visible(g_layout));
            g_view.clamp();
            g_cat_view.rows = g_view.rows;
            g_cat_view.clamp();
            g_pack_view.rows = g_view.rows;
            g_pack_view.clamp();
            place(g_n.details, g_layout.details);
            place(g_n.bar, g_layout.bar);
            place(g_n.discard, g_layout.discard);
            place(g_n.apply, g_layout.apply);
            place(g_n.actions, g_layout.actions);
            place(g_n.menu, g_layout.menu);
            place(g_n.confirm, g_layout.confirm);
            place(g_n.yes, g_layout.yes);
            place(g_n.no, g_layout.no);
            g_relayouts = g_relayouts + 1;
            g_mod->info("mod_manager: layout %dx%d: back %.0fx%.0f, frame %.0f,%.0f %.0fx%.0f, tabs %.0f,%.0f, actions %.0f,%.0f %.0fx%.0f, discard %.0f,%.0f, apply %.0f,%.0f, "
                        "yes %.0f,%.0f, no %.0f,%.0f; read back: discard %.0fx%.0f, tab %.0fx%.0f, actions %.0fx%.0f", w, h, g_layout.back.w, g_layout.back.h, g_layout.frame.x, g_layout.frame.y, g_layout.frame.w,
                        g_layout.frame.h, g_layout.tab[1].x, g_layout.tab[1].y, g_layout.actions.x, g_layout.actions.y, g_layout.actions.w, g_layout.actions.h,
                        g_layout.discard.x, g_layout.discard.y, g_layout.apply.x, g_layout.apply.y, g_layout.yes.x, g_layout.yes.y, g_layout.no.x, g_layout.no.y,
                        g_size_w(widget_of(g_n.discard)), g_size_h(widget_of(g_n.discard)), g_size_w(widget_of(g_n.tab[0])), g_size_h(widget_of(g_n.tab[0])),
                        g_size_w(widget_of(g_n.actions)), g_size_h(widget_of(g_n.actions)));
        }
        if (!g_labels_blank) write_labels(true);
        // the menu's own hover must not act on a click while the page is open
        if (const uintptr_t widget = field(gc, kStartMenuNode) ? reinterpret_cast<uintptr_t>(widget_of(reinterpret_cast<void*>(field(gc, kStartMenuNode)))) : 0) {
            const int32_t none = -1;
            g_mod->api()->mem_write(g_mod->handle(), widget + kHoverOffset, &none, sizeof none);
        }
        set_visible(reinterpret_cast<void*>(field(gc, kLogo)), false);
        g_page_frames = g_page_frames + 1;
    } else if (g_labels_blank) {
        write_labels(false);
    }
    static bool was_open = false;
    if (was_open && !g_state.open) {
        g_closes = g_closes + 1;
        g_mod->info("mod_manager: page closed");
    }
    was_open = g_state.open;
    const page::State& s = g_state;
    set_visible(g_n.back, s.open);
    set_visible(g_n.frame, s.open);
    set_visible(g_n.tab[0], s.open);
    set_visible(g_n.tab[1], s.open);
    set_visible(g_n.sub[0], s.open && s.tab == 1);
    set_visible(g_n.sub[1], s.open && s.tab == 1);
    set_visible(g_n.list, s.open);
    set_visible(g_n.search, s.open);
    set_visible(g_n.details, s.open);
    set_visible(g_n.bar, s.open);
    set_visible(g_n.discard, s.open);
    set_visible(g_n.apply, s.open);
    set_visible(g_n.actions, s.open);
    set_visible(g_n.menu, s.open && s.menu);
    set_visible(g_n.confirm, s.open && s.confirm);
    set_visible(g_n.yes, s.open && s.confirm);
    set_visible(g_n.no, s.open && s.confirm);
}

unsigned kind_rgb(int kind) {
    switch (kind) {
        case 1: return style::kTextSecondaryRgb;
        case 2: return style::kWarningRgb;
        case 3: return style::kErrorRgb;
        case 4: return style::kVerifiedRgb;
        default: return style::kTextRgb;
    }
}

// One row of a settings form: a header, a note, or an option with its control. `tip_hot` says the cursor is on this option
// although its tooltip moved the row away from the cursor; `cy` is then the cursor's height as if the row had not moved.
template <typename LineFn, typename BlockedFn>
void draw_form_row(const text::Font& f, LineFn&& line, form::Form& fm, const form::Row& row, float x, float y, float w, float cx, float cy,
                   BlockedFn&& blocked, bool tip_hot) {
    const float size = text::kBodySize;
    auto wide = [&](const std::wstring& t) { return f.width(t, size); };
    if (blocked(x, y, w)) return;
    if (row.kind == form::Row::Header) {
        line(widen(row.text.c_str()).c_str(), x, y, size, style::kTextSecondaryRgb);
        return;
    }
    if (row.kind == form::Row::Note) {
        const unsigned rgb = row.tone == 1 ? style::kErrorRgb : (row.tone == 2 ? style::kWarningRgb : style::kTextSecondaryRgb);
        const std::wstring t = text::fit(widen(row.text.c_str()), w - 16.0f, wide);
        line(t.c_str(), x + 16.0f, y, size, rgb);
        return;
    }
    form::Field& fd = fm.fields[row.field];
    const form::Kind kind = fd.kind();
    const form::Rect rect = {x, y - 19.0f, w, kInfoPitch};
    g_form_hits.push_back({{rect.x, rect.y, rect.w, rect.h}, &fm, row.field});
    const bool hot = !g_state.confirm && !g_state.menu && (tip_hot || rect.has(cx, cy));
    const form::Geometry g = form::geometry(rect, kind);
    const bool off_default = form::differs_from_default(fd);
    const unsigned val_rgb = fd.bad ? style::kErrorRgb : (form::dirty(fd) ? style::kWarningRgb : style::kTextRgb);
    const std::wstring label = text::fit(widen((form::label_of(fd)).c_str()), g.control.x - x - 12.0f, wide);
    line(label.c_str(), x, y, size, hot ? style::kTextHoverRgb : style::kTextRgb);
    auto zone_hot = [&](const form::Rect& z) { return hot && z.has(cx, cy); };
    if (kind != form::Kind::Text) {
        line(kind == form::Kind::Number ? L"-" : L"<", g.left.x + 10.0f, y, size, zone_hot(g.left) ? style::kTextHoverRgb : style::kTextRgb);
        line(kind == form::Kind::Number ? L"+" : L">", g.right.x + 10.0f, y, size, zone_hot(g.right) ? style::kTextHoverRgb : style::kTextRgb);
    }
    const bool typing = fm.editing == row.field;
    std::string val = typing ? fd.draft : form::shown(fd);
    if (typing && ((GetTickCount() / 500) & 1) == 0) val += "|";
    const std::wstring wv = text::fit(widen(val.c_str()), g.value.w - 8.0f, wide);
    if (kind == form::Kind::Slider) {
        line(wv.c_str(), g.value.x + 4.0f, y, size, val_rgb);
        const std::wstring bar = L"|";
        const float gw = std::max(1.0f, wide(bar));
        const int cells = std::max(2, static_cast<int>((g.track.w - 12.0f) / gw));
        const int filled = static_cast<int>(form::fraction(fd) * cells + 0.5);
        std::wstring on(static_cast<size_t>(filled), L'|'), rest(static_cast<size_t>(cells - filled), L'|');
        line(on.c_str(), g.track.x + 6.0f, y, size, style::kTextHoverRgb);
        line(rest.c_str(), g.track.x + 6.0f + gw * filled, y, size, style::kTextSecondaryRgb);
    } else if (kind == form::Kind::Text) {
        line(wv.c_str(), g.value.x + 6.0f, y, size, typing ? style::kTextHoverRgb : val_rgb);
    } else {
        line(wv.c_str(), g.value.x + g.value.w * 0.5f, y, size, val_rgb, 1);
    }
    if (off_default) line(L"[R]", g.reset.x + 4.0f, y, size, zone_hot(g.reset) ? style::kTextHoverRgb : style::kTextSecondaryRgb);
}

// The My mods tab: the search box, the filter picker, the count, the rows, and the selected mod's header and details.
// `line(text, x, y, size, rgb, flags)` draws one line of window pixels; (cx, cy) is the cursor in window pixels.
template <typename LineFn>
void draw_list_tab(const text::Font& f, LineFn&& line, float cx, float cy, bool shop, bool pack_list) {
    const page::Layout& l = g_layout;
    const page::State& s = g_state;
    list::View& v = pack_list ? g_pack_view : shop ? g_cat_view : g_view;
    const float size = text::kBodySize;
    auto wide = [&](const std::wstring& t) { return f.width(t, size); };
    auto mid = [&](const page::Rect& r) { return r.y + r.h * 0.5f + size * 0.38f; };
    auto blocked = [&](float x, float y, float w) {   // the open Actions menu hides what is under it (its frame cannot cover the text)
        return s.menu && x < l.menu.right() && x + w > l.menu.x && y - 20.0f < l.menu.bottom() && y + 6.0f > l.menu.y;
    };

    // search box
    {
        std::wstring q = v.query;
        const bool search_typing = s.typing && !editing_form();   // a form field may have the keyboard instead
        const bool caret = search_typing && ((GetTickCount() / 500) & 1) == 0;
        const float room = l.search.w - 2 * page::kTextIndent + 10.0f;
        if (q.empty() && !search_typing) {
            line(L"SEARCH", l.search.x + page::kTextIndent, mid(l.search), size, style::kTextSecondaryRgb);
        } else {
            while (!q.empty() && wide(q + L"|") > room) q.erase(0, 1);   // the end of what was typed stays in view
            line((q + (caret ? L"|" : L"")).c_str(), l.search.x + page::kTextIndent, mid(l.search), size, style::kTextHoverRgb);
        }
    }
    // filter picker: < NAME >
    {
        const bool left_hot = !s.confirm && l.filter.has(cx, cy) && cx < l.filter.x + kFilterArrowZone;
        const bool right_hot = !s.confirm && l.filter.has(cx, cy) && cx >= l.filter.right() - kFilterArrowZone;
        line(L"<", l.filter.x + 18.0f, mid(l.filter), size, left_hot ? style::kTextHoverRgb : style::kTextRgb, 1);
        line(L">", l.filter.right() - 18.0f, mid(l.filter), size, right_hot ? style::kTextHoverRgb : style::kTextRgb, 1);
        line(list::filter_name(v.filter), l.filter.x + l.filter.w * 0.5f, mid(l.filter), size, style::kTextRgb, 1);
    }
    {
        const std::wstring c = v.count_text();
        line(c.c_str(), l.count.x + 4.0f, mid(l.count), size, style::kTextSecondaryRgb);
        if (shop) g_open_rect = page::Rect();
        else {
            const wchar_t* open = L"OPEN MODS FOLDER";
            g_open_rect = l.folder;
            line(open, l.folder.x + 4.0f, mid(l.folder), size, (!s.confirm && !s.menu && g_open_rect.has(cx, cy)) ? style::kTextHoverRgb : style::kTextRgb);
        }
    }
    // rows
    if (v.shown.empty()) {
        const wchar_t* none = v.mods.empty() ? L"NO MODS INSTALLED" : pack_list ? L"NO MODPACK MATCHES" : L"NO MOD MATCHES";
        if (shop && v.mods.empty()) {
            bool running = false;
            catalog::get(nullptr, nullptr, &running);
            if (g_loaded.state == cindex::State::Rejected) {
                none = browse::kRejectedText;
            } else none = running ? L"LOADING THE CATALOG..." : (g_loaded.state == cindex::State::None ? L"NO CATALOG IS SET" : pack_list ? L"THE CATALOG HAS NO MODPACKS" : L"THE CATALOG IS EMPTY");
        }
        const bool rejected = shop && g_loaded.state == cindex::State::Rejected;
        float ny = l.rows.y + 28.0f;
        for (const std::wstring& part : text::wrap(none, l.rows.w, wide)) {
            line(part.c_str(), l.rows.x + l.rows.w * 0.5f, ny, size, rejected ? style::kErrorRgb : style::kTextSecondaryRgb, 1);
            ny += kInfoPitch;
        }
        if (rejected) line(L"NOTHING IS OFFERED FROM IT.", l.rows.x + l.rows.w * 0.5f, ny, size, style::kTextSecondaryRgb, 1);
    }
    const int visible = page::rows_visible(l);
    const int sel = v.selected_row();
    // The tag column fits its widest label, so a long tag such as INSTALLED never runs into the name.
    float tag_w = 0.0f;
    if (shop) {
        for (const wchar_t* t : {L"GET", L"UPDATE", L"INSTALLED", L"WAITING", L"QUEUED", L"100%"}) tag_w = std::max(tag_w, wide(t));
    } else {
        for (const wchar_t* t : {L"[DEL]", L"[UPD]", L"[ON ]", L"[OFF]", L"[ ! ]"}) tag_w = std::max(tag_w, wide(t));
    }
    tag_w = std::max(shop ? 128.0f : 72.0f, tag_w + 4.0f + 14.0f);
    for (int r = 0; r < visible && v.first + r < static_cast<int>(v.shown.size()); ++r) {
        const list::Mod& m = v.mods[v.shown[v.first + r]];
        const page::Rect row = {l.rows.x, l.rows.y + page::kRowH * r, l.rows.w, page::kRowH};
        const float y = row.y + row.h * 0.5f + size * 0.38f;
        const bool hot = !s.confirm && !s.menu && row.has(cx, cy);
        const bool chosen = v.first + r == sel;
        const unsigned tag_rgb = list::is_problem(m.state) ? style::kErrorRgb : (m.state == list::State::Disabled ? style::kTextSecondaryRgb : style::kVerifiedRgb);
        const std::string rid = utf8_of(m.id);
        const bool marked_del = !shop && g_coll.del.count(rid) != 0, marked_switch = !shop && g_coll.enable.count(rid) != 0;   // a collected change shows in warning colour
        const bool marked_dl = g_coll.dl.count(rid) != 0;
        const std::wstring dl_tag = marked_dl ? (shop ? download_tag(rid) : std::wstring(L"[UPD]")) : L"";
        const wchar_t* tag = pack_list ? m.size_text.c_str() : marked_dl ? dl_tag.c_str() : shop ? browse::offer_label(m.offer)
                                  : (marked_del ? L"[DEL]" : (marked_switch ? (g_coll.enable[rid] ? L"[ON ]" : L"[OFF]") : list::switch_label(m.state)));
        unsigned tag_color = (marked_del || marked_switch) ? style::kWarningRgb : tag_rgb;
        if (shop) tag_color = m.offer == list::Offer::Update ? style::kWarningRgb : (m.offer == list::Offer::Installed ? style::kVerifiedRgb : (m.offer == list::Offer::None ? style::kTextSecondaryRgb : style::kTextRgb));
        if (marked_dl) tag_color = style::kWarningRgb;
        if (pack_list) tag_color = style::kTextSecondaryRgb;
        line(tag, row.x + 4.0f, y, size, tag_color);
        std::wstring ver = m.version;
        if (shop && !pack_list && !m.size_text.empty()) ver += L"  " + m.size_text;
        if (!shop && !m.update_to.empty()) ver += L" > " + m.update_to;
        const float ver_w = wide(ver);
        const float name_w = std::max(40.0f, row.w - tag_w - ver_w - 16.0f);
        const std::wstring name = text::fit(m.name, name_w, wide);
        line(name.c_str(), row.x + tag_w, y, size, (chosen || hot) ? style::kTextHoverRgb : style::kTextRgb);
        if (shop && !m.authors.empty()) {   // the author follows the name when there is room
            const float used = wide(name) + 14.0f;
            if (name_w - used > 70.0f) {
                const std::wstring by = text::fit(L"by " + m.authors, name_w - used, wide);
                line(by.c_str(), row.x + tag_w + used, y, size, style::kTextSecondaryRgb);
            }
        }
        line(ver.c_str(), row.right() - 4.0f - ver_w, y, size, (!shop && !m.update_to.empty()) ? style::kWarningRgb : style::kTextSecondaryRgb);
    }
    // header of the details
    const list::Mod* cur = v.current();
    if (!cur) {
        const wchar_t* none = L"NONE SELECTED";
        g_name_w = f.width(none, std::wcslen(none), size);
        line(none, l.details.x + page::kTextIndent, mid(l.actions), size, style::kTextRgb);
        return;
    }
    const float name_room = std::max(120.0f, l.details.w - page::kTextIndent - page::kActionsW - 28.0f - 150.0f - page::kPad);   // the room left of the clamped Actions button (page.h)
    const std::wstring name = text::fit(cur->name, name_room, wide);
    g_name_w = wide(name);
    line(name.c_str(), l.details.x + page::kTextIndent, mid(l.actions), size, style::kTextRgb);
    if (!cur->version.empty())
        line(cur->version.c_str(), l.details.right() - page::kPad - wide(cur->version), mid(l.actions), size, style::kTextSecondaryRgb);
    // the details, scrolled by whole lines
    std::vector<list::InfoRow> rows;
    if (pack_list) {
        if (const cindex::Pack* p = current_pack()) {
            auto st = g_pack_stopped.find(p->id);
            rows = packs::info_rows(*p, pack_plan(*p), pack_mod_name, l.info.w, wide, st == g_pack_stopped.end() ? std::wstring() : st->second);
        } else if (const cindex::Pack* pr = current_profile()) {
            const std::vector<profile::Step> steps = profile::restore_plan(*pr, g_world, backup_of);
            rows = profile::info_rows(*pr, steps, profile::restore_work(*pr, steps, g_world, installed_fields), pack_mod_name, l.info.w, wide);
        }
    } else rows = shop ? browse::info_rows(*cur, l.info.w, wide) : list::info_rows(v, *cur, l.info.w, 22.0f, wide);
    form::Form no_form;
    form::Form& fm = shop ? no_form : form_of(cur->id);
    const std::vector<form::Row> frows = shop ? std::vector<form::Row>() : form::rows(fm);
    const int info_n = static_cast<int>(rows.size());
    const int total = info_n + static_cast<int>(frows.size());
    const int per_page = std::max(1, static_cast<int>(l.info.h / kInfoPitch));
    v.info_first = std::clamp(v.info_first, 0, std::max(0, total - per_page));
    g_form_hits.clear();
    auto slot_y = [&](int slot) { return l.info.y + 18.0f + kInfoPitch * slot; };
    // The hovered option with a long description gets a tooltip drawn over the last lines of the details (at most half of them).
    // The cursor stays on the option while it is in the rect the option was last drawn in or in the line it is found on.
    auto has_tip = [&](int i) {
        if (i < info_n || i >= total || frows[i - info_n].kind != form::Row::Option) return false;
        return fm.fields[frows[i - info_n].field].description.size() > form::kShortDescription;
    };
    int hov = -1;
    if (!frows.empty() && !s.confirm && !s.menu) {
        const int last = std::min(total, v.info_first + per_page);
        if (g_tip_row >= v.info_first && g_tip_row < last && has_tip(g_tip_row) && g_tip_rect.has(cx, cy)) hov = g_tip_row;
        for (int i = v.info_first; hov < 0 && i < last; ++i)
            if (has_tip(i) && page::Rect{l.info.x, slot_y(i - v.info_first) - 19.0f, l.info.w, kInfoPitch}.has(cx, cy)) hov = i;
    }
    std::vector<std::wstring> tip;
    int tip_n = 0, hov_slot = 0, draw_slot = 0;
    if (hov >= 0) {
        const std::wstring text_w = widen(fm.fields[frows[hov - info_n].field].description.c_str());
        const int wanted = static_cast<int>(text::wrap(text_w, l.info.w, wide).size());
        tip_n = form::tip_lines(per_page, wanted);
        tip = text::wrap_capped(text_w, l.info.w, static_cast<size_t>(tip_n), wide);
        tip_n = static_cast<int>(tip.size());
        hov_slot = hov - v.info_first;
        draw_slot = form::tip_row_slot(hov_slot, per_page, tip_n);
    }
    for (int slot = 0; slot < per_page && v.info_first + slot < total; ++slot) {
        int i = v.info_first + slot;
        if (hov >= 0) {
            if (slot >= per_page - tip_n) break;   // under the tooltip
            if (slot == draw_slot) i = hov;        // the hovered option stands here, above the tooltip, when its own line is covered
            else if (i == hov) continue;
        }
        const float y = slot_y(slot);
        if (i >= info_n) {
            const bool tip_hot = i == hov;
            const float shift = tip_hot ? slot_y(hov_slot) - y : 0.0f;   // from where the row is drawn to where the cursor found it
            const bool from_old_spot = tip_hot && page::Rect{l.info.x, y + shift - 19.0f, l.info.w, kInfoPitch}.has(cx, cy);
            draw_form_row(f, line, fm, frows[i - info_n], l.info.x, y, l.info.w, cx, from_old_spot ? cy - shift : cy, blocked, tip_hot);
            if (tip_hot && !g_form_hits.empty()) {
                page::Rect& r = g_form_hits.back().r;   // a click anywhere in the moved row's old spot or new one is a click on it
                const float top = std::min(r.y, r.y + shift), bottom = std::max(r.bottom(), r.bottom() + shift);
                r.y = top;
                r.h = bottom - top;
                g_tip_rect = r;
            }
            continue;
        }
        const list::InfoRow& row = rows[i];
        float x = l.info.x;
        if (!row.label.empty() || !row.chips.empty()) {
            if (!row.label.empty() && !blocked(x, y, wide(row.label)))
                line(row.label.c_str(), x, y, size, style::kTextSecondaryRgb);
            x += list::kChipIndent;
            for (const list::Chip& chip : row.chips) {
                const list::Chip c = {text::fit(chip.text, std::max(60.0f, l.info.w - list::kChipIndent), wide), chip.id, chip.installed, chip.kind};
                const float w = wide(c.text);
                const page::Rect r = {x, y - 18.0f, w, 24.0f};
                const bool hot = c.installed && !s.confirm && !s.menu && r.has(cx, cy);
                if (!blocked(x, y, w)) line(c.text.c_str(), x, y, size, hot ? style::kTextHoverRgb : (c.installed ? style::kTextRgb : kind_rgb(c.kind)));
                if (c.installed) g_chip_hits.push_back({r, c.id});
                x += w + 22.0f;
            }
            continue;
        }
        for (const list::Seg& seg : row.line.segs) {
            const float w = wide(seg.text);
            if (!blocked(x, y, w)) line(seg.text.c_str(), x, y, size, kind_rgb(seg.kind));
            x += w;
        }
    }
    g_tip_row = hov;
    for (int k = 0; k < tip_n; ++k)
        line(tip[k].c_str(), l.info.x, slot_y(per_page - tip_n + k), size, style::kTextSecondaryRgb);
}

// The page's text, drawn from the title menu's draw (the font is bound, the transform is the widget's). Window pixels in, widget
// pixels out. y is the text's baseline, which is about 7 px below the middle of an 18 px line (9 below at 24).
void draw_page(const text::Font& f, void* widget, int win_w, int win_h) {
    const float width = g_get_width(widget);
    const float ox = (static_cast<float>(win_w) - width) * 0.5f, oy = static_cast<float>(win_h) * 0.5f + 50.0f;
    const page::Layout& l = g_layout;
    const page::State& s = g_state;
    float cx = -1, cy = -1;
    {
        float c[2] = {-1.0f, -1.0f};
        g_get_cursor(widget, c);
        if (c[0] >= 0.0f) { cx = c[0] + ox; cy = c[1] + oy; }
    }
    auto line = [&](const wchar_t* t, float x, float y, float size, unsigned rgb, int flags = 0) {
        f.draw_line(t, std::wcslen(t), x - ox, y - oy, size, rgb, flags);
    };
    auto mid = [](const page::Rect& r, float size) { return r.y + r.h * 0.5f + size * 0.38f; };

    line(L"MODS", l.title_x, l.title_y, text::kTitleSize, style::kTextRgb, 1);
    for (int i = 0; i < 2; ++i) {
        const bool hot = !s.confirm && l.tab[i].has(cx, cy);
        line(i == 0 ? L"MY MODS" : L"INSTALL", l.tab[i].x + l.tab[i].w * 0.5f, mid(l.tab[i], text::kBodySize), text::kBodySize,
             (s.tab == i || hot) ? style::kTextHoverRgb : style::kTextRgb, 1);
    }
    if (s.tab == 1)
        for (int i = 0; i < 2; ++i) {
            const bool hot = !s.confirm && l.sub[i].has(cx, cy);
            line(i == 0 ? L"MODS" : L"MODPACKS", l.sub[i].x + l.sub[i].w * 0.5f, mid(l.sub[i], text::kBodySize), text::kBodySize,
                 (s.sub == i || hot) ? style::kTextHoverRgb : style::kTextRgb, 1);
        }
    if (s.confirm && g_confirm_delete) {
        const std::wstring name = text::fit(g_delete_name, l.confirm.w - 60.0f, [&](const std::wstring& t) { return f.width(t, text::kBodySize); });
        line((L"DELETE " + name + L"?").c_str(), l.confirm.x + l.confirm.w * 0.5f, l.confirm.y + 52.0f, text::kBodySize, style::kTextRgb, 1);
        line(g_delete_profile ? L"THE PROFILE FILE IS REMOVED." : L"IT MOVES TO THE BACKUP. SETTINGS STAY.", l.confirm.x + l.confirm.w * 0.5f, l.confirm.y + 84.0f, text::kBodySize, style::kTextSecondaryRgb, 1);
        return;
    }
    if (s.confirm) {
        wchar_t msg[64];
        std::swprintf(msg, 64, L"DISCARD %d CHANGE%ls?", s.changes(), s.changes() == 1 ? L"" : L"S");
        line(msg, l.confirm.x + l.confirm.w * 0.5f, l.confirm.y + 52.0f, text::kBodySize, style::kTextRgb, 1);
        line(L"THEY ARE NOT SAVED.", l.confirm.x + l.confirm.w * 0.5f, l.confirm.y + 84.0f, text::kBodySize, style::kTextSecondaryRgb, 1);
        return;
    }
    if (cur_view().current())
        line(L"ACTIONS v", l.actions.x + l.actions.w * 0.5f, mid(l.actions, text::kBodySize), text::kBodySize,
         l.actions.has(cx, cy) ? style::kTextHoverRgb : style::kTextRgb, 1);
    line(L"DISCARD", l.discard.x + l.discard.w * 0.5f, mid(l.discard, kButtonTextSize), kButtonTextSize, style::kTextRgb, 1);
    line(page::State::apply_label(g_coll.count() > 0 || g_new_count > 0), l.apply.x + l.apply.w * 0.5f, mid(l.apply, kButtonTextSize), kButtonTextSize,
         (g_options_invalid || downloads_running()) ? style::kTextSecondaryRgb : style::kTextRgb, 1);
    g_chip_hits.clear();
    draw_list_tab(f, line, cx, cy, s.tab == 1, s.packs());
    {
        const std::wstring cat = browse::banner(g_loaded.state);
        line(cat.c_str(), l.frame.right() - page::kPad - f.width(cat, text::kBodySize), l.title_y, text::kBodySize,
             g_loaded.state == cindex::State::Online ? style::kVerifiedRgb : (g_loaded.state == cindex::State::None ? style::kTextSecondaryRgb : style::kWarningRgb));
    }
    wchar_t bar[128];
    const std::wstring dl_line = download_line();
    std::swprintf(bar, 128, L"%d CHANGE%ls%ls%ls", s.changes(), s.changes() == 1 ? L"" : L"S", dl_line.empty() ? L"" : L", ", dl_line.c_str());
    line(s.changes() ? bar : L"NO CHANGES", l.bar.x + page::kTextIndent, mid(l.bar, text::kBodySize), text::kBodySize,
         s.changes() ? style::kWarningRgb : style::kTextSecondaryRgb);
    if (!g_notice.empty()) {
        const float x = l.bar.x + page::kTextIndent + f.width(std::wstring(s.changes() ? bar : L"NO CHANGES"), text::kBodySize) + 28.0f;
        const float room = l.discard.x - x - 12.0f;
        const std::wstring t = text::fit(widen(g_notice.c_str()), room, [&](const std::wstring& u) { return f.width(u, text::kBodySize); });
        line(t.c_str(), x, mid(l.bar, text::kBodySize), text::kBodySize, style::kErrorRgb);
    }
    if (s.menu) {
        const int hot = page::menu_row_at(l, static_cast<int>(g_menu.size()), cx, cy);
        for (int i = 0; i < static_cast<int>(g_menu.size()); ++i) {
            const actions::Row& r = g_menu[i];
            unsigned rgb = style::kTextRgb;
            if (r.kind == 1) rgb = style::kTextSecondaryRgb;
            else if (i == hot) rgb = r.kind == 2 ? style::kErrorRgb : style::kTextHoverRgb;
            line(widen(r.label.c_str()).c_str(), l.menu.x + page::kPad, l.menu.y + page::kMenuPad + page::kMenuRowH * i + 21.0f, text::kBodySize, rgb);
        }
        if (hot >= 0 && hot < static_cast<int>(g_menu.size()) && !g_menu[hot].tip.empty())   // the tooltip of a grey row, under the menu
            line(text::fit(widen(g_menu[hot].tip.c_str()), l.details.right() - page::kPad - l.menu.x - page::kPad,
                           [&](const std::wstring& t) { return f.width(t, text::kBodySize); }).c_str(),
                 l.menu.x + page::kPad, l.menu.bottom() + 24.0f, text::kBodySize, style::kTextSecondaryRgb);
    }
}

// The row's hook: after the game's three rows, with the font in edi and the widget in esi.
void KYUBU_CALL row_site(KyubuCpuContext* ctx, void*) {
    void* widget = reinterpret_cast<void*>(ctx->esi);
    void* font = reinterpret_cast<void*>(ctx->edi);
    const uintptr_t gc = game_controller();
    if (!widget || !font || !gc) return;
    int32_t* hover = reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(widget) + kHoverOffset);
    const float width = g_get_width(widget);
    const int win_w = *reinterpret_cast<int32_t*>(gc + kWindowW), win_h = *reinterpret_cast<int32_t*>(gc + kWindowH);
    if (g_state.open) {   // the page: no title rows, no hover
        *hover = -1;
        text::Font pf = g_fns;
        pf.font = font;
        draw_page(pf, widget, win_w, win_h);
        g_widget = reinterpret_cast<uintptr_t>(widget);
        g_last_draw_tick = GetTickCount();
        return;
    }
    const row::Layout lay = row::exit_layout(win_w, win_h, width);
    float cursor[2] = {-1.0f, -1.0f};
    g_get_cursor(widget, cursor);
    const bool over = cursor[0] >= 0.0f && row::inside(lay, cursor[0], cursor[1]);
    if (*hover == 2) *hover = kHoverMods;   // the game's third row now reads Mods: its click must not quit
    else if (over) *hover = kHoverExit;

    text::Font f = g_fns;
    f.font = font;
    f.draw_line(L"Exit", 4, lay.x, lay.y, text::kBodySize, over ? style::kTextHoverRgb : style::kTextRgb, lay.flags);

    g_widget = reinterpret_cast<uintptr_t>(widget);
    g_hover_seen = *hover;
    g_corner = lay.corner ? 1 : 0;
    g_last_draw_tick = GetTickCount();
    g_draws = g_draws + 1;
}

void on_mouse(KyubuEvent& ev) {
    const KyubuMouseButtonData* d = kyubu::Mod::data<KyubuMouseButtonData>(ev);
    if (!d || d->button != 0 || !d->down) return;
    if (g_state.open) {   // the page's buttons are tested in the update hook
        g_click_pending = true;
        return;
    }
    // the menu is showing when the draw ran a moment ago; hover is the widget's own word
    if (GetTickCount() - g_last_draw_tick > kRecentMs || !g_widget) return;
    int32_t hover = -1;
    if (g_mod->api()->mem_read(g_widget + kHoverOffset, &hover, sizeof hover) != KY_OK) return;
    if (hover == kHoverMods) {
        g_mods_clicks = g_mods_clicks + 1;
        g_page_requests = g_page_requests + 1;
        if (g_page_ok) g_open_request = true;
        g_mod->info("mod_manager: click on Mods at %d,%d", d->x, d->y);
    } else if (hover == kHoverExit) {
        g_exit_clicks = g_exit_clicks + 1;
        g_mod->info("mod_manager: click on Exit at %d,%d: quit requested", d->x, d->y);
        if (const uintptr_t gc = game_controller()) {
            const uint8_t one = 1;
            g_mod->api()->mem_write(g_mod->handle(), gc + kQuitOffset, &one, 1);
        }
    }
}

void on_key(KyubuEvent& ev) {
    const KyubuKeyData* d = kyubu::Mod::data<KyubuKeyData>(ev);
    if (!d || !g_state.open) return;
    if (d->vk == VK_ESCAPE && d->down && !d->repeat) g_escape_pending = true;
    else if (d->vk == VK_BACK && d->down) g_backspaces = g_backspaces + 1;
}

void on_char(KyubuEvent& ev) {
    const KyubuCharData* d = kyubu::Mod::data<KyubuCharData>(ev);
    if (!d || !g_state.open) return;
    const int next = (g_char_head + 1) % 32;
    if (next == g_char_tail) return;   // full: drop
    g_chars[g_char_head] = d->codepoint;
    g_char_head = next;
}

void on_wheel(KyubuEvent& ev) {
    const KyubuMouseWheelData* d = kyubu::Mod::data<KyubuMouseWheelData>(ev);
    if (d && g_state.open) g_wheel = g_wheel + d->delta;
}

void KYUBU_CALL cmd_pending(const char* args, KyubuPrintFn print, void* ctx, void*) {
    int n = 0;
    if (args) n = std::atoi(args);
    g_stub_pending = n < 0 ? 0 : n;
    char line[96];
    std::snprintf(line, sizeof line, "mod_manager: pending changes set to %d", g_stub_pending);
    print(ctx, line);
}

void KYUBU_CALL cmd_rescan(const char*, KyubuPrintFn print, void* ctx, void*) {
    g_rescan_request = true;   // what the focus coming back does, for a replay (no window focus under Xvfb)
    print(ctx, "mod_manager: rescan requested");
}

void KYUBU_CALL cmd_catalog(const char* args, KyubuPrintFn print, void* ctx, void*) {
    std::string a = args ? args : "";
    while (!a.empty() && (a.front() == ' ' || a.front() == '\t')) a.erase(a.begin());
    while (!a.empty() && (a.back() == ' ' || a.back() == '\r' || a.back() == '\n')) a.pop_back();
    if (a.compare(0, 5, "mods:") == 0) {
        char dir[300] = {};
        if (g_mod->api()->game_dir(dir, sizeof dir) != KY_OK) { print(ctx, "mod_manager: no game folder"); return; }
        std::string d = dir;
        for (char& c : d) if (c == '\\') c = '/';
        a = "file:///" + d + "/mods/" + a.substr(5);
    }
    if (!a.empty()) g_catalog_override = a;
    start_catalog();
    print(ctx, ("mod_manager: catalog fetch requested for " + catalog_url()).c_str());
}

unsigned mark_count(list::Mark k) {
    unsigned n = 0;
    for (const list::Mod& m : g_view.mods) n += m.mark == k ? 1 : 0;
    return n;
}
unsigned update_count() {
    unsigned n = 0;
    for (const list::Mod& m : g_view.mods) n += m.update_to.empty() ? 0 : 1;
    return n;
}

void KYUBU_CALL cmd_state(const char*, KyubuPrintFn print, void* ctx, void*) {
    char line[1400];
    char sel_id[40] = "";
    for (size_t i = 0; i < g_view.selected.size() && i < sizeof sel_id - 1; ++i) sel_id[i] = static_cast<char>(g_view.selected[i]);
    std::snprintf(line, sizeof line,
                  "mod_manager: draws=%u hover=%d corner=%d last_draw_ms_ago=%lu mods_clicks=%u exit_clicks=%u page_requests=%u "
                  "page_open=%d tab=%d menu=%d confirm=%d pending=%d options=%d invalid=%d form_clicks=%u apply_blocked=%u opens=%u closes=%u page_frames=%u relayouts=%u back=%u tabs=%u actions=%u rows=%u "
                  "discard=%u apply=%u yes=%u no=%u escapes=%u build_failed=%d labels_blank=%d mods=%u shown=%u filter=%d first=%d selected=%s row_clicks=%u chip_clicks=%u catalog=%s cat_mods=%u cat_shown=%u cat_filter=%d unverified=%u verified=%u updates=%u menu_rows=%u sub=%d packs=%u pack_shown=%u profiles=%u pack_applying=%d pack_applies=%u restores=%u",
                  static_cast<unsigned>(g_draws), static_cast<int>(g_hover_seen), static_cast<int>(g_corner),
                  static_cast<unsigned long>(GetTickCount() - g_last_draw_tick), static_cast<unsigned>(g_mods_clicks),
                  static_cast<unsigned>(g_exit_clicks), static_cast<unsigned>(g_page_requests), g_state.open ? 1 : 0, g_state.tab, g_state.menu ? 1 : 0,
                  g_state.confirm ? 1 : 0, g_state.pending, g_state.options, g_options_invalid ? 1 : 0,
                  static_cast<unsigned>(g_form_clicks), static_cast<unsigned>(g_apply_blocked), static_cast<unsigned>(g_opens), static_cast<unsigned>(g_closes),
                  static_cast<unsigned>(g_page_frames), static_cast<unsigned>(g_relayouts), static_cast<unsigned>(g_back_clicks),
                  static_cast<unsigned>(g_tab_clicks), static_cast<unsigned>(g_actions_clicks), static_cast<unsigned>(g_menu_rows_clicked),
                  static_cast<unsigned>(g_discard_clicks), static_cast<unsigned>(g_apply_clicks), static_cast<unsigned>(g_confirm_yes),
                  static_cast<unsigned>(g_confirm_no), static_cast<unsigned>(g_escapes), static_cast<int>(g_build_failed), g_labels_blank ? 1 : 0,
                  static_cast<unsigned>(g_view.mods.size()), static_cast<unsigned>(g_view.shown.size()), static_cast<int>(g_view.filter), g_view.first,
                  sel_id, static_cast<unsigned>(g_row_clicks), static_cast<unsigned>(g_chip_clicks), catalog_state_name(g_loaded.state),
                  static_cast<unsigned>(g_cat_view.mods.size()), static_cast<unsigned>(g_cat_view.shown.size()), static_cast<int>(g_cat_view.filter), mark_count(list::Mark::Unverified),
                  mark_count(list::Mark::Verified), update_count(), static_cast<unsigned>(g_menu.size()), g_state.sub,
                  static_cast<unsigned>(g_pack_view.mods.size()), static_cast<unsigned>(g_pack_view.shown.size()),
                  static_cast<unsigned>(g_profiles.size()), g_papply.on ? 1 : 0, static_cast<unsigned>(g_pack_applies), static_cast<unsigned>(g_restores));
    print(ctx, line);
    g_mod->info("%s", line);
    unsigned waiting = 0, running = 0, staged = 0, failed = 0;
    for (const download::Item& it : download::snapshot()) {
        waiting += it.st == download::St::Waiting;
        running += it.st == download::St::Running;
        staged += it.st == download::St::Staged;
        failed += it.st == download::St::Failed;
    }
    std::snprintf(line, sizeof line, "mod_manager: downloads collected=%u waiting=%u running=%u staged=%u failed=%u started=%u staged_total=%u failed_total=%u",
                  static_cast<unsigned>(g_coll.dl.size()), waiting, running, staged, failed, static_cast<unsigned>(g_dl_started),
                  static_cast<unsigned>(g_dl_staged), static_cast<unsigned>(g_dl_failed));
    print(ctx, line);
    g_mod->info("%s", line);
}

// Hooks the row and rewrites the Exit label; false (with a logged reason) leaves the title menu as the game has it.
bool install_row(kyubu::Mod& m) {
    const uintptr_t site = m.symbol("cube::StartMenuWidget::drawEndSite");
    g_gc_global = m.symbol("cube::game_controller");
    // the exe may be relocated: go through the RVA
    const uintptr_t literal = m.api()->rva_to_addr(kExitLabelRva);
    static const wchar_t kExit[5] = {L'E', L'x', L'i', L't', 0};
    static const wchar_t kMods[5] = {L'M', L'o', L'd', L's', 0};
    wchar_t now[5] = {};
    if (!literal || m.api()->mem_read(literal, now, sizeof now) != KY_OK || std::memcmp(now, kExit, sizeof now) != 0) {
        m.warn("mod_manager: the Exit label is not where the map says: no Mods row");
        return false;
    }
    KyubuId id = 0;
    const KyubuResult r = m.api()->hook_mid(m.handle(), site, &row_site, nullptr, &id);
    if (r != KY_OK) {
        m.warn("mod_manager: could not hook the menu's end site (result %d): no Mods row", static_cast<int>(r));
        return false;
    }
    KyubuId patch_id = 0;
    if (m.api()->patch_bytes(m.handle(), literal, kMods, sizeof kMods, &patch_id) != KY_OK) {
        m.api()->unhook(m.handle(), id);
        m.warn("mod_manager: could not rewrite the Exit label: no Mods row");
        return false;
    }
    return true;
}

// Text test (console mod_manager:texttest): under the title menu, a long name cut to 200 px, the whole name with a bar at its measured end, and a short one.
void KYUBU_CALL text_test_site(KyubuCpuContext* ctx, void*) {
    void* font = reinterpret_cast<void*>(ctx->edi);
    if (!font) return;
    text::Font f = g_fns;
    f.font = font;
    const std::wstring name = L"A VERY LONG MOD NAME THAT MUST BE CLIPPED";
    const std::wstring shortname = L"SHORT";
    const float size = text::kBodySize;
    const std::wstring cut = text::fit(name, 200.0f, [&](const std::wstring& s) { return f.width(s, size); });
    const float w_name = f.width(name, size), w_short = f.width(shortname, size), w_cut = f.width(cut, size);
    f.draw_line(cut.data(), cut.size(), 0.0f, 240.0f, size, style::kTextRgb);
    f.draw_line(L"|", 1, 200.0f, 240.0f, size, style::kWarningRgb);
    f.draw_line(name.data(), name.size(), 0.0f, 280.0f, size, style::kTextSecondaryRgb);
    f.draw_line(L"|", 1, w_name, 280.0f, size, style::kErrorRgb);
    f.draw_line(shortname.data(), shortname.size(), 0.0f, 320.0f, size, style::kTextRgb);
    f.draw_line(L"|", 1, w_short, 320.0f, size, style::kErrorRgb);
    static bool logged = false;
    if (!logged && g_mod) {
        logged = true;
        g_mod->info("mod_manager: text widths at %.0f: name %.1f, short %.1f, cut %.1f (%d chars of %d)", size, w_name, w_short, w_cut,
                    static_cast<int>(cut.size()), static_cast<int>(name.size()));
    }
}

// mod_manager:texttest hooks the title menu's end site once and draws the test text from then on.
void KYUBU_CALL cmd_texttest(const char*, KyubuPrintFn print, void* ctx, void*) {
    static bool on = false;
    if (on) { print(ctx, "mod_manager: text test already on"); return; }
    KyubuId id = 0;
    const KyubuResult r = g_mod->api()->hook_mid(g_mod->handle(), g_mod->symbol("cube::StartMenuWidget::drawEndSite"), &text_test_site, nullptr, &id);
    on = r == KY_OK;
    print(ctx, on ? "mod_manager: text test on" : "mod_manager: text test: hook failed");
    g_mod->info("mod_manager: text test hook result %d", static_cast<int>(r));
}

}  // namespace

struct ModManager {
    explicit ModManager(kyubu::Mod& m) {
        if (m.side() != KY_SIDE_CLIENT) return;
        g_mod = &m;
        int missing = 0;
        for (const char* name : kSymbols) {
            if (m.symbol(name)) continue;
            ++missing;
            m.warn("mod_manager: symbol %s does not resolve", name);
        }
        if (missing) {
            m.warn("mod_manager: %d symbol(s) missing: no Mods row", missing);
            return;
        }
        m.info("mod_manager: loaded, %d symbols resolve", static_cast<int>(sizeof kSymbols / sizeof kSymbols[0]));
        g_fns.draw = reinterpret_cast<text::DrawTextFn>(m.symbol("plasma::ScalableFont::drawText"));
        g_fns.measure = reinterpret_cast<text::MeasureTextFn>(m.symbol("plasma::ScalableFont::measureText"));
        g_get_cursor = reinterpret_cast<GetCursorFn>(m.symbol("plasma::Widget::getCursor"));
        g_get_width = reinterpret_cast<GetWidthFn>(m.symbol("plasma::Widget::getWidth"));
        g_clone = reinterpret_cast<CloneFn>(m.symbol("plasma::Node::clone"));
        g_set_text = reinterpret_cast<SetTextFn>(m.symbol("plasma::Node::setText"));
        g_set_parent = reinterpret_cast<SetParentFn>(m.symbol("plasma::Node::setParent"));
        g_set_position = reinterpret_cast<SetXYFn>(m.symbol("plasma::Widget::setPosition"));
        g_set_size = reinterpret_cast<SetXYFn>(m.symbol("plasma::Widget::setSize"));
        g_size_w = reinterpret_cast<GetSizeFn>(m.symbol("plasma::Widget::getSizeWidth"));
        g_size_h = reinterpret_cast<GetSizeFn>(m.symbol("plasma::Widget::getSizeHeight"));
        m.on("input.mouse_button", on_mouse);
        m.on("input.key", on_key);
        m.on("input.char", on_char);
        m.on("input.mouse_wheel", on_wheel);
        if (install_row(m)) {
            g_third_char = L'M';
            m.info("mod_manager: Mods row installed");
            KyubuId shown_id = 0;
            const KyubuResult rs = m.api()->hook_mid(m.handle(), m.symbol("cube::GameController::startMenuShownSite"), &shown_site, nullptr, &shown_id);
            if (rs != KY_OK) m.warn("mod_manager: could not hook the update site (result %d): no page", static_cast<int>(rs));
            else g_page_ok = true;
        }
        if (m.api()->register_command(m.handle(), "mod_manager:pending", "mod_manager:pending <n>: pretend n changes are collected (page shell, task 5.5)",
                                      cmd_pending, nullptr) != KY_OK)
            m.warn("mod_manager: could not register the pending command");
        if (m.api()->register_command(m.handle(), "mod_manager:rescan", "mod_manager:rescan: rescan the mods folder as when the game window gets the focus back", cmd_rescan, nullptr) != KY_OK)
            m.warn("mod_manager: could not register the rescan command");
        if (m.api()->register_command(m.handle(), "mod_manager:catalog", "mod_manager:catalog [file:///... | mods:<folder>]: fetch the catalog now", cmd_catalog, nullptr) != KY_OK)
            m.warn("mod_manager: could not register the catalog command");
        if (m.api()->register_command(m.handle(), "mod_manager:state", "mod_manager:state: print the Mods row counters", cmd_state, nullptr) != KY_OK)
            m.warn("mod_manager: could not register the state command");
        if (m.api()->register_command(m.handle(), "mod_manager:texttest", "mod_manager:texttest: draw test text under the title menu (task 5.3)", cmd_texttest,
                                      nullptr) != KY_OK)
            m.warn("mod_manager: could not register the text test command");
    }
};

KYUBU_MOD("mod_manager", KY_SIDE_CLIENT, ModManager)
