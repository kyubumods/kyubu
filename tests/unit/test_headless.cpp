// headless mode rules (add-native-replay 2.2): the flag, the dropped messages, the placement, the synthetic state.
#include "doctest.h"

#include <dinput.h>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <vector>

#include "headless.h"

using namespace kyubu::headless;

TEST_CASE("headless: the flag needs dev mode") {
    CHECK(resolve_flag(false, true) == FlagResult::Off);
    CHECK(resolve_flag(false, false) == FlagResult::Off);
    CHECK(resolve_flag(true, true) == FlagResult::On);
    CHECK(resolve_flag(true, false) == FlagResult::Refused);
}

TEST_CASE("headless: real input and focus loss are dropped, the rest passes") {
    for (UINT m : {WM_KEYDOWN, WM_KEYUP, WM_SYSKEYDOWN, WM_CHAR, WM_MOUSEMOVE, WM_MOUSEWHEEL, WM_LBUTTONDOWN, WM_RBUTTONUP,
                   WM_MBUTTONDOWN, WM_XBUTTONUP, WM_KILLFOCUS})
        CHECK(drops_message(m, 0));
    CHECK(drops_message(WM_ACTIVATE, WA_INACTIVE));
    CHECK_FALSE(drops_message(WM_ACTIVATE, WA_ACTIVE));
    CHECK_FALSE(drops_message(WM_ACTIVATE, WA_CLICKACTIVE));
    CHECK(drops_message(WM_ACTIVATEAPP, FALSE));
    CHECK_FALSE(drops_message(WM_ACTIVATEAPP, TRUE));
    // The device reset needs these two, and the engine must still see paint, size and close.
    CHECK_FALSE(drops_message(WM_SETFOCUS, 0));
    CHECK_FALSE(drops_message(WM_SIZE, 0));
    CHECK_FALSE(drops_message(WM_CLOSE, 0));
    CHECK_FALSE(drops_message(WM_PAINT, 0));
}

TEST_CASE("headless: window style and place") {
    const DWORD ex = window_ex_style(WS_EX_WINDOWEDGE);
    CHECK((ex & WS_EX_NOACTIVATE) != 0);
    CHECK((ex & WS_EX_TOOLWINDOW) != 0);
    CHECK((ex & WS_EX_WINDOWEDGE) != 0);
    const POINT o = window_origin(-1920, 0, 4480);
    CHECK(o.x == 2760);   // right of the virtual screen (-1920 + 4480 = 2560) plus 200
    CHECK(o.y == 0);
}

TEST_CASE("headless: synthetic keyboard and mouse state") {
    set_scan(DIK_W, true);
    set_scan(DIK_4, true);
    unsigned char k[256];
    keyboard_state(k);
    CHECK(k[DIK_W] == 0x80);
    CHECK(k[DIK_4] == 0x80);
    CHECK(k[DIK_A] == 0);
    set_scan(DIK_W, false);
    keyboard_state(k);
    CHECK(k[DIK_W] == 0);
    set_scan(DIK_4, false);

    cursor_reset(1024, 768);
    CHECK(cursor_get().x == 512);
    CHECK(cursor_get().y == 384);
    DIMOUSESTATE2 m{};
    fill_mouse(&m, sizeof m);   // first read: no motion
    CHECK(m.lX == 0);
    cursor_set(520, 380);
    set_button(0, true);
    add_wheel(120);
    fill_mouse(&m, sizeof m);
    CHECK(m.lX == 8);
    CHECK(m.lY == -4);
    CHECK(m.lZ == 120);
    CHECK(m.rgbButtons[0] == 0x80);
    fill_mouse(&m, sizeof m);   // a second read: no new motion, the wheel is spent, the button stays down
    CHECK(m.lX == 0);
    CHECK(m.lZ == 0);
    CHECK(m.rgbButtons[0] == 0x80);
    cursor_warp(512, 384);   // a warp by the game (SetCursorPos) is no mouse motion
    fill_mouse(&m, sizeof m);
    CHECK(m.lX == 0);
    CHECK(m.lY == 0);
    CHECK(cursor_get().x == 512);
    DIMOUSESTATE m4{};
    set_button(0, false);
    fill_mouse(&m4, sizeof m4);
    CHECK(m4.rgbButtons[0] == 0);
}

TEST_CASE("headless: key table") {
    int vk = 0, dik = 0;
    REQUIRE(key_lookup("w", vk, dik)); CHECK(vk == 'W'); CHECK(dik == DIK_W);
    REQUIRE(key_lookup("W", vk, dik)); CHECK(dik == DIK_W);
    REQUIRE(key_lookup("4", vk, dik)); CHECK(vk == '4'); CHECK(dik == DIK_4);
    REQUIRE(key_lookup("F12", vk, dik)); CHECK(vk == VK_F12); CHECK(dik == DIK_F12);
    REQUIRE(key_lookup("f1", vk, dik)); CHECK(vk == VK_F1);
    REQUIRE(key_lookup("grave", vk, dik)); CHECK(vk == VK_OEM_3); CHECK(dik == DIK_GRAVE);
    REQUIRE(key_lookup("Return", vk, dik)); CHECK(vk == VK_RETURN); CHECK(dik == DIK_RETURN);
    REQUIRE(key_lookup("space", vk, dik)); CHECK(dik == DIK_SPACE);
    REQUIRE(key_lookup("shift", vk, dik)); CHECK(vk == VK_SHIFT); CHECK(dik == DIK_LSHIFT);
    REQUIRE(key_lookup("ctrl", vk, dik)); CHECK(dik == DIK_LCONTROL);
    CHECK_FALSE(key_lookup("F13", vk, dik));
    CHECK_FALSE(key_lookup("F0", vk, dik));
    CHECK_FALSE(key_lookup("nonsense", vk, dik));
    CHECK_FALSE(key_lookup("", vk, dik));
}

TEST_CASE("headless: input parser") {
    std::vector<std::string> errs;
    auto ev = parse_input("\xEF\xBB\xBF# comment\r\ndown w\r\n\r\nup W\nbutton down 1\nbutton up 3\nmove 512 384\nwheel down 12\nwheel up 2\ntext hello world\n", &errs);
    CHECK(errs.empty());
    REQUIRE(ev.size() == 8);
    CHECK(ev[0].kind == EvKind::KeyDown); CHECK(ev[0].dik == DIK_W);
    CHECK(ev[1].kind == EvKind::KeyUp);
    CHECK(ev[2].kind == EvKind::ButtonDown); CHECK(ev[2].button == 0);
    CHECK(ev[3].kind == EvKind::ButtonUp); CHECK(ev[3].button == 2);
    CHECK(ev[4].kind == EvKind::Move); CHECK(ev[4].x == 512); CHECK(ev[4].y == 384);
    CHECK(ev[5].ticks == -12);
    CHECK(ev[6].ticks == 2);
    CHECK(ev[7].kind == EvKind::Text); CHECK(ev[7].text == "hello world");

    errs.clear();
    ev = parse_input("down nonsense\ndown w\nmove 1\nwheel sideways 2\nbutton down 4\nfoo\ntext\nwheel up 0\n", &errs);
    CHECK(ev.size() == 1);
    REQUIRE(errs.size() == 7);
    CHECK(errs[0].find("line 1") == 0);
    CHECK(errs[0].find("nonsense") != std::string::npos);
    CHECK(errs[1].find("line 3") == 0);
}

TEST_CASE("headless: one-frame rule") {
    std::vector<Event> in = parse_input("down 4\nup 4\ndown w\nup w\n", nullptr);
    InputQueue q;
    q.push(in);
    auto f1 = q.next_frame();
    REQUIRE(f1.size() == 1);   // down 4 only: its up waits
    CHECK(f1[0].kind == EvKind::KeyDown);
    CHECK(q.size() == 3);
    auto f2 = q.next_frame();
    REQUIRE(f2.size() == 2);   // up 4, down w
    CHECK(f2[0].kind == EvKind::KeyUp);
    CHECK(f2[1].kind == EvKind::KeyDown);
    auto f3 = q.next_frame();
    REQUIRE(f3.size() == 1);
    CHECK(f3[0].kind == EvKind::KeyUp);
    CHECK(q.next_frame().empty());

    // Different keys in one frame are fine; buttons follow the same rule.
    q.push(parse_input("down a\ndown d\nup a\nbutton down 1\nbutton up 1\nmove 3 4\n", nullptr));
    auto g1 = q.next_frame();
    REQUIRE(g1.size() == 2);   // down a, down d; up a would see its own down
    auto g2 = q.next_frame();
    REQUIRE(g2.size() == 2);   // up a, button down 1; button up waits
    auto g3 = q.next_frame();
    REQUIRE(g3.size() == 2);   // button up, move
}

TEST_CASE("headless: apply_state and text units") {
    cursor_reset(1024, 768);
    std::vector<Event> ev = parse_input("down w\nbutton down 1\nmove 10 20\nwheel up 2\n", nullptr);
    for (auto& e : ev) apply_state(e);
    unsigned char k[256];
    keyboard_state(k);
    CHECK(k[DIK_W] == 0x80);
    CHECK(cursor_get().x == 10);
    long m[5] = {};
    fill_mouse(m, 20);
    CHECK(m[2] == 240);
    CHECK(((unsigned char*)(m + 3))[0] == 0x80);
    apply_state(parse_input("up w\n", nullptr)[0]);
    keyboard_state(k);
    CHECK(k[DIK_W] == 0);

    auto u = utf8_to_units("a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80");
    REQUIRE(u.size() == 5);
    CHECK(u[0] == 'a'); CHECK(u[1] == 0xE9); CHECK(u[2] == 0x20AC); CHECK(u[3] == 0xD83D); CHECK(u[4] == 0xDE00);
    CHECK(utf8_to_units("\xFF" "b").size() == 1);
}

TEST_CASE("headless: checksums and the stored zlib stream") {
    const unsigned char abc[3] = {'a', 'b', 'c'};
    CHECK(crc32_of(abc, 3) == 0x352441C2u);
    CHECK(adler32_of(abc, 3) == 0x024D0127u);
    std::vector<unsigned char> z;
    CHECK(stored_zlib(abc, 3, z));
    CHECK(z.size() == 2 + 5 + 3 + 4);
    CHECK(z[0] == 0x78);
    CHECK(z[2] == 1);   // last block
    std::vector<unsigned char> big(70000, 7), zb;
    CHECK(stored_zlib(big.data(), big.size(), zb));
    CHECK(zb[2] == 0);                           // first of two blocks
    CHECK(zb.size() == 2 + 5 + 65535 + 5 + 4465 + 4);
}

TEST_CASE("headless: PNG writer and frame hash") {
    unsigned char px[4 * 2 * 2] = {1, 2, 3, 0, 10, 20, 30, 0, 4, 5, 6, 0, 7, 8, 9, 0};   // B,G,R,X
    auto png = png_from_bgra(px, 2, 2, 8, nullptr);
    REQUIRE(png.size() > 8 + 25);
    CHECK(png[1] == 'P');
    CHECK(png[8 + 3] == 13);          // IHDR length
    CHECK(png[8 + 4 + 4 + 3] == 2);   // width 2
    CHECK(png[png.size() - 1 - 11] == 0);  // IEND length 0
    CHECK(png[png.size() - 8] == 'I');   // IEND type
    const unsigned h1 = frame_hash(px, 2, 2, 8);
    px[3] = 99;   // the X byte is ignored
    CHECK(frame_hash(px, 2, 2, 8) == h1);
    px[0] = 2;
    CHECK(frame_hash(px, 2, 2, 8) != h1);
    if (const char* out = getenv("KYUBU_TEST_PNG_OUT")) {
        std::vector<unsigned char> img(64 * 48 * 4);
        for (int y = 0; y < 48; ++y)
            for (int x = 0; x < 64; ++x) {
                img[(y * 64 + x) * 4 + 0] = (unsigned char)(x * 4);
                img[(y * 64 + x) * 4 + 1] = (unsigned char)(y * 5);
                img[(y * 64 + x) * 4 + 2] = 200;
            }
        auto p = png_from_bgra(img.data(), 64, 48, 256, nullptr);
        FILE* f = fopen(out, "wb");
        REQUIRE(f);
        fwrite(p.data(), 1, p.size(), f);
        fclose(f);
    }
}

TEST_CASE("headless: shot names are file safe") {
    CHECK(shot_name("title\n", "x") == "title");
    CHECK(shot_name("  a b/c..d\r\nsecond", "x") == "abc..d");
    CHECK(shot_name("../..", "fb") == "fb");
    CHECK(shot_name("", "") == "shot");
    CHECK(shot_name(std::string(200, 'a'), "x").size() == 80);
}

TEST_CASE("headless: fps meter reports every period") {
    FpsMeter m;
    double fps = 0, lo = 0;
    CHECK(!m.tick(1000, fps, lo));   // first tick only starts the clock
    bool got = false;
    unsigned long long t = 1000;
    for (int i = 0; i < 1000 && !got; ++i) {
        t += (i == 100) ? 40 : 10;   // 100 fps with one 40 ms gap
        got = m.tick(t, fps, lo);
    }
    REQUIRE(got);
    CHECK(fps > 95.0);
    CHECK(fps < 101.0);
    CHECK(lo > 24.9);
    CHECK(lo < 25.1);
    CHECK(!m.tick(t + 10, fps, lo));   // a new period started
}
