/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2024  The Project Peppercorn Authors.
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

// Runs the tests in parallel: after the warm-up (TestWarmUp), `--jobs=N` (or NEXTPNR_TEST_JOBS, default:
// the number of cores) worker processes, started from this executable, run the tests, each its share.
// The shares are balanced by the durations of the previous run (kept next to the executable, in
// `<executable>.times`), the slowest tests first. Only the output of failing tests is shown;
// NEXTPNR_TEST_KEEP_LOGS=1 keeps the logs of the workers and of the warm-up. `--jobs=1`, a single
// selected test, sharding and the gtest options that list tests or write reports run the tests in this
// process as usual.

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include "gtest/gtest.h"
#include "log.h"
#include "test_runner.h"

USING_NEXTPNR_NAMESPACE

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

struct WarmUp
{
    std::function<void(const std::string &)> make, load;
};

std::vector<WarmUp> &warm_ups()
{
    static std::vector<WarmUp> work;
    return work;
}

std::string executable_path(const char *argv0)
{
#if defined(_WIN32)
    char path[32768];
    DWORD length = GetModuleFileNameA(nullptr, path, sizeof(path));
    if (length > 0 && length < sizeof(path))
        return path;
#elif defined(__linux__)
    std::error_code error;
    fs::path path = fs::read_symlink("/proc/self/exe", error);
    if (!error)
        return path.string();
#endif
    return fs::absolute(argv0).string();
}

std::string quote(const std::string &arg)
{
#if defined(_WIN32)
    return "\"" + arg + "\"";
#else
    std::string quoted = "'";
    for (char c : arg)
        quoted += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return quoted + "'";
#endif
}

// Run a command line through the shell; whether it exited with 0
bool run_command(const std::string &command)
{
#if defined(_WIN32)
    // (cmd.exe takes off the first and last quote)
    return std::system(("\"" + command + "\"").c_str()) == 0;
#else
    int status = std::system(command.c_str());
    return status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}

fs::path make_temp_dir()
{
    std::random_device random;
    for (int attempt = 0; attempt < 100; attempt++) {
        fs::path dir = fs::temp_directory_path() / ("nextpnr-tests-" + std::to_string(random()));
        if (fs::create_directory(dir))
            return dir;
    }
    throw std::runtime_error("cannot create a temporary directory");
}

bool starts_with(const std::string &s, const char *prefix) { return s.rfind(prefix, 0) == 0; }

// Whether gtest skips a test unless asked to run disabled tests: its suite or its name starts with
// DISABLED_ (as "Prefix/DISABLED_Suite.test" or "Suite.DISABLED_test/1")
bool disabled(const std::string &test)
{
    for (size_t at = 0; at < test.size(); at = test.find_first_of("/.", at) + 1) {
        if (test.compare(at, 9, "DISABLED_") == 0)
            return true;
        if (test.find_first_of("/.", at) == std::string::npos)
            break;
    }
    return false;
}

// The tests the gtest options select, as gtest lists them (and runs them)
std::vector<std::string> list_tests(const std::string &executable, const fs::path &dir)
{
    fs::path list = dir / "list";
    std::string command = quote(executable) + " --gtest_list_tests --gtest_filter=" + quote(GTEST_FLAG_GET(filter)) +
                          " > " + quote(list.string()) + " 2>&1";
    if (!run_command(command)) {
        fprintf(stderr, "Listing the tests failed:\n");
        std::ifstream in(list);
        std::cerr << in.rdbuf();
        exit(1);
    }
    std::vector<std::string> tests;
    std::ifstream in(list);
    std::string line, suite;
    while (std::getline(in, line)) {
        if (line.empty() || starts_with(line, "Info:") || starts_with(line, "Warning:"))
            continue;
        std::istringstream words(line);
        std::string name;
        words >> name;
        if (line[0] != ' ')
            suite = name;
        else if (GTEST_FLAG_GET(also_run_disabled_tests) || !disabled(suite + name))
            tests.push_back(suite + name);
    }
    return tests;
}

std::map<std::string, double> read_times(const std::string &path)
{
    std::map<std::string, double> times;
    std::ifstream in(path);
    std::string name;
    double seconds;
    while (in >> seconds >> name)
        times[name] = seconds;
    return times;
}

void write_times(const std::string &path, const std::map<std::string, double> &times)
{
    std::ofstream out(path);
    for (auto &entry : times)
        out << entry.second << " " << entry.first << "\n";
}

// The outcome of one test in a worker's log
struct Outcome
{
    enum
    {
        PASSED,
        FAILED,
        STOPPED // the worker stopped during the test
    } result;
    double seconds = 0;
    std::vector<std::string> output; // for a failure: what the test printed
};

// From a gtest result line ("[  FAILED  ] Suite.test, where GetParam() = 1 (12 ms)"): the test and
// its duration; false for the other lines with the same tag (the summary)
bool parse_result(const std::string &line, std::string &test, double &seconds)
{
    if (line.size() < 18 || line.compare(line.size() - 4, 4, " ms)") != 0)
        return false;
    size_t open = line.rfind(" (");
    std::string rest = line.substr(13, open - 13);
    test = rest.substr(0, rest.find_first_of(", "));
    seconds = atof(line.c_str() + open + 2) / 1000;
    return true;
}

std::map<std::string, Outcome> read_outcomes(const fs::path &log)
{
    std::map<std::string, Outcome> outcomes;
    std::ifstream in(log);
    std::string line, current;
    std::vector<std::string> output;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        // A gtest tag can follow output without a line end on the same line
        size_t at = std::string::npos;
        for (const char *tag : {"[ RUN      ] ", "[       OK ] ", "[  SKIPPED ] ", "[  FAILED  ] "})
            at = std::min(at, line.find(tag));
        if (at != std::string::npos && at > 0) {
            if (!current.empty())
                output.push_back(line.substr(0, at));
            line = line.substr(at);
        }
        std::string test;
        double seconds;
        if (starts_with(line, "[ RUN      ] ")) {
            current = line.substr(13);
            output.clear();
        } else if ((starts_with(line, "[       OK ] ") || starts_with(line, "[  SKIPPED ] ")) &&
                   parse_result(line, test, seconds)) {
            outcomes[test] = Outcome{Outcome::PASSED, seconds, {}};
            current.clear();
        } else if (starts_with(line, "[  FAILED  ] ") && parse_result(line, test, seconds)) {
            outcomes[test] = Outcome{Outcome::FAILED, seconds, output};
            current.clear();
        } else if (!current.empty()) {
            output.push_back(line);
        }
    }
    if (!current.empty())
        outcomes[current] = Outcome{Outcome::STOPPED, 0, output};
    return outcomes;
}

// What a failed test printed, without the log messages of passes that went well
void print_output(const std::vector<std::string> &output, const fs::path &log)
{
    int lines = 0;
    for (auto &line : output) {
        if (line.empty() || starts_with(line, "Info:") || starts_with(line, "Warning:"))
            continue;
        if (++lines > 100) {
            printf("    ... (all of it in %s)\n", log.string().c_str());
            break;
        }
        printf("    %s\n", line.c_str());
    }
}

// The tests split into `jobs` shares of about the same duration, each the slowest first
std::vector<std::vector<std::string>> split_tests(std::vector<std::string> tests,
                                                  const std::map<std::string, double> &times, int jobs)
{
    std::vector<double> known;
    for (auto &test : tests)
        if (times.count(test))
            known.push_back(times.at(test));
    std::sort(known.begin(), known.end());
    // A new test takes as long as most
    double typical = known.empty() ? 1 : known[known.size() / 2];
    auto duration = [&](const std::string &test) { return times.count(test) ? times.at(test) : typical; };
    std::stable_sort(tests.begin(), tests.end(),
                     [&](const std::string &a, const std::string &b) { return duration(a) > duration(b); });
    std::vector<std::vector<std::string>> shares(jobs);
    std::vector<double> load(jobs, 0);
    for (auto &test : tests) {
        int least = std::min_element(load.begin(), load.end()) - load.begin();
        shares[least].push_back(test);
        load[least] += duration(test);
    }
    return shares;
}

int run_parallel(const std::string &executable, int jobs)
{
    const bool keep_logs = getenv("NEXTPNR_TEST_KEEP_LOGS") && atoi(getenv("NEXTPNR_TEST_KEEP_LOGS"));
    fs::path dir = make_temp_dir();
    std::vector<std::string> tests = list_tests(executable, dir);
    std::string times_path = executable + ".times";
    std::map<std::string, double> times = read_times(times_path);
    jobs = std::max(1, std::min(jobs, int(tests.size())));

    auto start = Clock::now();
    printf("[==========] Running %d tests in %d processes\n", int(tests.size()), jobs);
    fflush(stdout);
    {
        // The warm-up logs into a file
        std::ofstream log(dir / "warm-up.log");
        auto streams = log_streams;
        log_streams = {std::make_pair(&log, LogLevel::LOG_MSG)};
        for (size_t i = 0; i < warm_ups().size(); i++)
            warm_ups()[i].make((dir / ("warm-up-" + std::to_string(i))).string());
        log_streams = streams;
    }
    printf("[----------] Warm-up done after %.1fs\n", std::chrono::duration<double>(Clock::now() - start).count());
    fflush(stdout);

    auto shares = split_tests(tests, times, jobs);
    std::mutex mutex;
    std::vector<std::string> failed;
    std::vector<std::thread> workers;
    for (int w = 0; w < jobs; w++) {
        fs::path base = dir / ("worker-" + std::to_string(w));
        {
            std::ofstream list(base.string() + ".tests");
            for (auto &test : shares[w])
                list << test << "\n";
        }
        workers.emplace_back([&, w, base]() {
            fs::path log = base.string() + ".log";
            std::string command = quote(executable) + " --worker=" + quote(base.string());
            if (GTEST_FLAG_GET(also_run_disabled_tests))
                command += " --gtest_also_run_disabled_tests";
            bool exited_cleanly = run_command(command + " > " + quote(log.string()) + " 2>&1");
            auto outcomes = read_outcomes(log);
            std::lock_guard<std::mutex> lock(mutex);
            bool failures = false;
            for (auto &test : shares[w]) {
                auto found = outcomes.find(test);
                if (found == outcomes.end()) {
                    failed.push_back(test);
                    failures = true;
                    printf("[  FAILED  ] %s (not run: its worker stopped before, see %s)\n", test.c_str(),
                           log.string().c_str());
                    continue;
                }
                Outcome &outcome = found->second;
                if (outcome.result == Outcome::PASSED) {
                    times[test] = outcome.seconds;
                    continue;
                }
                failed.push_back(test);
                failures = true;
                if (outcome.result == Outcome::STOPPED)
                    printf("[  FAILED  ] %s (its worker stopped during the test)\n", test.c_str());
                else
                    printf("[  FAILED  ] %s (%.1fs)\n", test.c_str(), outcome.seconds);
                print_output(outcome.output, log);
            }
            if (!exited_cleanly && !failures) {
                failed.push_back("(worker " + std::to_string(w) + ")");
                printf("[  FAILED  ] worker %d failed after its tests, see %s\n", w, log.string().c_str());
            }
            fflush(stdout);
        });
    }
    for (auto &worker : workers)
        worker.join();
    write_times(times_path, times);
    // (only the logs are kept)
    for (size_t i = 0; i < warm_ups().size(); i++) {
        std::error_code error;
        fs::remove(dir / ("warm-up-" + std::to_string(i)), error);
    }

    printf("[==========] %d tests ran in %.1fs in %d processes\n", int(tests.size()),
           std::chrono::duration<double>(Clock::now() - start).count(), jobs);
    printf("[  PASSED  ] %d tests.\n", int(tests.size() - failed.size()));
    if (failed.empty() && !keep_logs) {
        std::error_code error;
        fs::remove_all(dir, error);
    } else {
        printf("[----------] Logs in %s\n", dir.string().c_str());
    }
    if (failed.empty())
        return 0;
    printf("[  FAILED  ] %d tests, listed below:\n", int(failed.size()));
    for (auto &test : failed)
        printf("[  FAILED  ] %s\n", test.c_str());
    printf("\n %d FAILED TEST%s\n", int(failed.size()), failed.size() == 1 ? "" : "S");
    return 1;
}

// In a worker: load what the warm-up left, and run the tests listed in `<base>.tests`
int run_worker(const std::string &base)
{
    fs::path dir = fs::path(base).parent_path();
    for (size_t i = 0; i < warm_ups().size(); i++)
        warm_ups()[i].load((dir / ("warm-up-" + std::to_string(i))).string());
    std::ifstream in(base + ".tests");
    std::string test, filter;
    while (std::getline(in, test))
        filter += (filter.empty() ? "" : ":") + test;
    if (filter.empty())
        return 0;
    GTEST_FLAG_SET(filter, filter);
    return RUN_ALL_TESTS();
}

} // namespace

NEXTPNR_NAMESPACE_BEGIN
TestWarmUp::TestWarmUp(std::function<void(const std::string &file)> make,
                       std::function<void(const std::string &file)> load)
{
    warm_ups().push_back(WarmUp{std::move(make), std::move(load)});
}
NEXTPNR_NAMESPACE_END

int main(int argc, char **argv)
{
    // TODO: Remove for delivery, useful while checking tests
    log_streams.push_back(std::make_pair(&std::cerr, LogLevel::LOG_MSG));
    ::testing::InitGoogleTest(&argc, argv);

    int jobs = std::max(1u, std::thread::hardware_concurrency());
    if (const char *env = getenv("NEXTPNR_TEST_JOBS"))
        jobs = atoi(env);
    std::string worker;
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--jobs=", 7) == 0)
            jobs = atoi(argv[i] + 7);
        else if (strcmp(argv[i], "-j") == 0 && i + 1 < argc)
            jobs = atoi(argv[++i]);
        else if (strncmp(argv[i], "--worker=", 9) == 0)
            worker = argv[i] + 9;
        else {
            fprintf(stderr, "Unknown option '%s'\n", argv[i]);
            return 1;
        }
    }
    if (!worker.empty())
        return run_worker(worker);

    bool in_process = jobs <= 1 || GTEST_FLAG_GET(list_tests) || !GTEST_FLAG_GET(output).empty() ||
                      getenv("GTEST_TOTAL_SHARDS") || GTEST_FLAG_GET(repeat) != 1;
    if (!in_process) {
        // A single test runs here, without the warm-up
        std::string filter = GTEST_FLAG_GET(filter);
        in_process = !filter.empty() && filter.find_first_of("*?:-") == std::string::npos;
    }
    if (in_process)
        return RUN_ALL_TESTS();
    return run_parallel(executable_path(argv[0]), jobs);
}
