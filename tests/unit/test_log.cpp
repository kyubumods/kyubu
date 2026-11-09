#include "doctest.h"

#include <regex>

#include "common.h"
#include "log.h"

using namespace kyubu;

TEST_CASE("loader-bootstrap: log rotation and line format") {
    // covers: loader-bootstrap/Mod log line
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::string dir = join_path(narrow(tmp), fmt("kyubu-logtest-%lu", GetCurrentProcessId()));
    Log& log = Log::get();
    for (int i = 1; i <= 7; ++i) {
        log.open(dir, 5);
        log.write(KY_LOG_INFO, "example", KY_SIDE_CLIENT, fmt("hello %d", i));
    }
    // latest + 5 rotated logs, nothing older
    CHECK(file_exists(join_path(dir, "latest.log")));
    for (int i = 1; i <= 5; ++i) CHECK(file_exists(join_path(dir, fmt("log-%d.log", i))));
    CHECK_FALSE(file_exists(join_path(dir, "log-6.log")));

    auto latest = read_file(join_path(dir, "latest.log"));
    REQUIRE(latest);
    std::string text(latest->begin(), latest->end());
    CHECK(text.find("hello 7") != std::string::npos);
    auto prev = read_file(join_path(dir, "log-1.log"));
    REQUIRE(prev);
    std::string ptext(prev->begin(), prev->end());
    CHECK(ptext.find("hello 6") != std::string::npos);
    CHECK(ptext.find("hello 7") == std::string::npos);

    // "2026-09-25 17:02:11.482 INFO  [example] [client] hello 7"
    std::regex line(R"(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3} INFO  \[example\] \[client\] hello 7)");
    CHECK(std::regex_search(text, line));
}
