// The shared main() of every doctest executable (tests/CMakeLists.txt, ADR 0005).
//
// With MT_DOCTEST_EXPECT_CASES=<n> in the environment (every discovered CTest sets it to 1,
// tools/mtport/doctestctest.py), a run whose filters matched a different number of test cases
// fails: a CTest whose --test-case filter selects nothing would otherwise pass without running
// anything (#171).
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <cstdio>
#include <cstdlib>

namespace {

bool g_ran = false;
unsigned g_matched = 0;

// Records how many test cases passed the filters; reports nothing.
struct CaseCount : doctest::IReporter {
    explicit CaseCount(const doctest::ContextOptions &) {}
    void report_query(const doctest::QueryData &) override {}
    void test_run_start() override {}
    void test_run_end(const doctest::TestRunStats &stats) override {
        g_ran = true;
        g_matched = stats.numTestCasesPassingFilters;
    }
    void test_case_start(const doctest::TestCaseData &) override {}
    void test_case_reenter(const doctest::TestCaseData &) override {}
    void test_case_end(const doctest::CurrentTestCaseStats &) override {}
    void test_case_exception(const doctest::TestCaseException &) override {}
    void subcase_start(const doctest::SubcaseSignature &) override {}
    void subcase_end() override {}
    void log_assert(const doctest::AssertData &) override {}
    void log_message(const doctest::MessageData &) override {}
    void test_case_skipped(const doctest::TestCaseData &) override {}
};

} // namespace

DOCTEST_REGISTER_LISTENER("mt_case_count", 1, CaseCount);

int main(int argc, char **argv) {
    const int result = doctest::Context(argc, argv).run();
    const char *expect = std::getenv("MT_DOCTEST_EXPECT_CASES");
    if (g_ran && expect != nullptr && *expect != '\0') {
        const unsigned long want = std::strtoul(expect, nullptr, 10);
        if (g_matched != want) {
            std::fprintf(stderr,
                         "doctest_main: the filters matched %u test cases, MT_DOCTEST_EXPECT_CASES "
                         "is %lu (#171)\n",
                         g_matched, want);
            return 1;
        }
    }
    return result;
}
