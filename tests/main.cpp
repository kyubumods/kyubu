#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest.h"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#include "guard.h"

// Prints each test case name before it runs, and flushes, so a crash always shows which case was
// running. On by default; set KYUBU_TEST_TRACE=0 to turn it off.
struct TraceListener : doctest::IReporter {
    explicit TraceListener(const doctest::ContextOptions&) {}
    void report_query(const doctest::QueryData&) override {}
    void test_run_start() override {}
    void test_run_end(const doctest::TestRunStats&) override {}
    void test_case_start(const doctest::TestCaseData& tc) override {
        if (!enabled()) return;
        std::fprintf(stdout, "[ run  ] %s\n", tc.m_name);
        std::fflush(stdout);
    }
    void test_case_reenter(const doctest::TestCaseData&) override {}
    void test_case_end(const doctest::CurrentTestCaseStats& st) override {
        if (!enabled()) return;
        std::fprintf(stdout, "[ %s ] %.2f s\n", st.failure_flags ? "FAIL" : " ok ", st.seconds);
        std::fflush(stdout);
    }
    void test_case_exception(const doctest::TestCaseException&) override {}
    void subcase_start(const doctest::SubcaseSignature&) override {}
    void subcase_end() override {}
    void log_assert(const doctest::AssertData&) override {}
    void log_message(const doctest::MessageData&) override {}
    void test_case_skipped(const doctest::TestCaseData&) override {}
    static bool enabled() {
        const char* e = std::getenv("KYUBU_TEST_TRACE");
        return !e || std::string(e) != "0";
    }
};
REGISTER_LISTENER("kyubu-trace", 1, TraceListener);

int main(int argc, char** argv) {
    // Unbuffered output: a crash must not swallow the results printed so far.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::cout << std::unitbuf;
    kyubu::guard_init();
    doctest::Context ctx(argc, argv);
    return ctx.run();
}
