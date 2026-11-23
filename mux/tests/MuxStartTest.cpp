// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// The start of the daemon through the systemd user manager, with a user
// manager of the test (UserManager): its run function answers for
// systemd-run and systemctl, and runs neither. No test reaches the user's
// manager, its unit aglow-mux.service or the user's daemon (TestDaemon.h).

#include "TestDaemon.h"

#include "MuxStart.h"

using namespace MuxTest;

namespace {

/// The environment of the clients of @p daemon, as NAME=VALUE entries.
Environment environmentOf(const TestDaemon& daemon)
{
    Environment environment;
    for (const auto& [name, value] : daemon.cleanEnvironment())
        environment.push_back(name + "=" + value);
    return environment;
}

/// The property that a `systemctl show` call asks for, or empty.
std::string propertyOf(const std::vector<std::string>& argv)
{
    constexpr std::string_view Option = "--property=";
    for (const std::string& word : argv) {
        if (word.starts_with(Option))
            return word.substr(Option.size());
    }
    return {};
}

bool contains(const std::vector<std::string>& argv, const std::string& word)
{
    return std::find(argv.begin(), argv.end(), word) != argv.end();
}

/// A user manager of the test: it runs, and @p answer answers each call
/// that connectOrStart() makes, which it keeps in @p calls. A call of
/// another program fails the test.
UserManager fakeManager(std::vector<std::vector<std::string>>& calls,
                        std::function<int(const std::vector<std::string>& argv, std::string* output)> answer)
{
    UserManager manager;
    manager.runs = true;
    manager.run = [&calls, answer = std::move(answer)](const std::vector<std::string>& argv,
                                                       std::chrono::steady_clock::time_point, std::string* output) {
        calls.push_back(argv);
        if (argv.empty() || (argv.front() != "systemd-run" && argv.front() != "systemctl")) {
            ADD_FAILURE() << "a call of another program: " << (argv.empty() ? std::string() : argv.front());
            return -1;
        }
        return answer(argv, output);
    };
    return manager;
}

/// A daemon path that cannot run: a start outside the user manager fails
/// with "cannot run", so a test sees it, and no stray daemon stays.
std::string missingDaemon(const TestDaemon& daemon)
{
    return daemon.dir() + "/no-aglow-mux";
}

constexpr const char* NoAnswer = "aglow-mux did not start: the systemd user manager does not answer";

} // namespace

TEST(MuxStart, TheUserManagerOfATestIsNeverTheUsers)
{
    TestDaemon daemon;
    // The runtime directory of the test is not /run/user/UID.
    EXPECT_FALSE(userManagerOf(environmentOf(daemon)).runs);
}

TEST(MuxStart, TheRunOfTheUserManagerTellsATimeoutFromAFailure)
{
    TestDaemon daemon;
    const UserManager manager = userManagerOf(environmentOf(daemon));
    const auto later = [] {
        return std::chrono::steady_clock::now() + 5s;
    };
    EXPECT_EQ(manager.run({"/bin/sh", "-c", "exit 3"}, later(), nullptr), 3);
    std::string output;
    EXPECT_EQ(manager.run({"/bin/sh", "-c", "echo loaded"}, later(), &output), 0);
    EXPECT_EQ(output, "loaded\n");
    // A program that does not run, or that a signal ends: -1.
    EXPECT_EQ(manager.run({missingDaemon(daemon)}, later(), nullptr), -1);
    EXPECT_EQ(manager.run({"/bin/sh", "-c", "kill -KILL $$"}, later(), nullptr), -1);
    // A program that does not end by the deadline: killed, and TimedOut,
    // with its output read or not.
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(manager.run({"/bin/sh", "-c", "exec sleep 30"}, start + 200ms, nullptr), UserManager::TimedOut);
    output.clear();
    EXPECT_EQ(manager.run({"/bin/sh", "-c", "exec sleep 30"}, std::chrono::steady_clock::now() + 200ms, &output),
              UserManager::TimedOut);
    EXPECT_LT(std::chrono::steady_clock::now() - start, 10s);
}

TEST(MuxStart, AUnitOfTheUserManagerStartsTheDaemon)
{
    TestDaemon daemon;
    std::vector<std::vector<std::string>> calls;
    const UserManager manager =
        fakeManager(calls, [&daemon](const std::vector<std::string>& argv, std::string* output) {
            // The unit's daemon: the test's, in the test's directories.
            if (argv.front() == "systemd-run")
                return daemon.spawnDaemon() > 0 ? 0 : 1;
            if (output && propertyOf(argv) == "LoadState")
                *output = daemon.alive() ? "loaded\n" : "not-found\n";
            return 0;
        });
    std::string error;
    const Reached reached = connectOrStart(daemon.sockets(), AFTERGLOW_MUX, environmentOf(daemon), manager, &error);
    ASSERT_TRUE(reached.connection) << error << daemon.errors();
    EXPECT_EQ(error, "");
    ASSERT_FALSE(calls.empty());
    const std::vector<std::string>& start = calls.front();
    EXPECT_EQ(start.front(), "systemd-run");
    EXPECT_TRUE(contains(start, "--user"));
    EXPECT_TRUE(contains(start, "--unit=aglow-mux"));
    EXPECT_TRUE(contains(start, "--setenv=XDG_STATE_HOME=" + daemon.stateHome()));
    EXPECT_EQ(start.back(), AFTERGLOW_MUX);
    // One start: the unit's.
    EXPECT_EQ(std::count_if(calls.begin(), calls.end(),
                            [](const auto& argv) {
                                return argv.front() == "systemd-run";
                            }),
              1);
}

TEST(MuxStart, TheUnitGetsTheStateDirectoryOfTheClientWithoutXdgStateHome)
{
    TestDaemon daemon;
    Environment environment;
    for (const std::string& entry : environmentOf(daemon)) {
        if (!entry.starts_with("XDG_STATE_HOME="))
            environment.push_back(entry);
    }
    std::vector<std::vector<std::string>> calls;
    // A unit that starts and is gone at once: no daemon answers.
    const UserManager manager = fakeManager(calls, [](const std::vector<std::string>& argv, std::string* output) {
        if (output && argv.front() == "systemctl")
            *output = propertyOf(argv) == "LoadState" ? "not-found\n" : "inactive\n";
        return 0;
    });
    std::string error;
    const Reached reached = connectOrStart(daemon.sockets(), missingDaemon(daemon), environment, manager, &error);
    EXPECT_FALSE(reached.connection);
    EXPECT_EQ(error, "aglow-mux did not start: see journalctl --user -u aglow-mux");
    ASSERT_FALSE(calls.empty());
    // muxSockets() finds the state directory under HOME: so must the daemon,
    // whose manager can have another XDG_STATE_HOME.
    EXPECT_TRUE(contains(calls.front(), "--setenv=XDG_STATE_HOME=" + daemon.dir() + "/home/.local/state"));
    EXPECT_TRUE(contains(calls.front(), "--setenv=HOME=" + daemon.dir() + "/home"));
}

TEST(MuxStart, AUnitFileThatDoesNotRunStartsNoDaemon)
{
    TestDaemon daemon;
    std::vector<std::vector<std::string>> calls;
    const UserManager manager = fakeManager(calls, [](const std::vector<std::string>& argv, std::string* output) {
        if (argv.front() == "systemd-run")
            return 1;
        const std::string property = propertyOf(argv);
        if (output)
            *output = property == "LoadState" ? "loaded\n" : property == "Transient" ? "no\n" : "inactive\n";
        return 0;
    });
    std::string error;
    const Reached reached =
        connectOrStart(daemon.sockets(), missingDaemon(daemon), environmentOf(daemon), manager, &error);
    EXPECT_FALSE(reached.connection);
    EXPECT_EQ(error,
              "aglow-mux did not start: the user unit aglow-mux.service exists and does not run. Start it with "
              "systemctl --user "
              "start aglow-mux.service, or remove it");
    EXPECT_FALSE(exists(daemon.socket()));
}

TEST(MuxStart, AStartThatTheUserManagerDoesNotAnswerStartsNoSecondDaemon)
{
    TestDaemon daemon;
    std::vector<std::vector<std::string>> calls;
    // systemd-run, killed at the deadline: the unit can still come.
    const UserManager manager = fakeManager(calls, [](const std::vector<std::string>& argv, std::string* output) {
        if (argv.front() == "systemd-run")
            return UserManager::TimedOut;
        if (output)
            *output = "not-found\n";
        return 0;
    });
    std::string error;
    const Reached reached =
        connectOrStart(daemon.sockets(), missingDaemon(daemon), environmentOf(daemon), manager, &error);
    EXPECT_FALSE(reached.connection);
    // Not the "cannot run" of a start outside the user manager.
    EXPECT_EQ(error, NoAnswer);
    EXPECT_EQ(calls.size(), 1u);
    EXPECT_FALSE(exists(daemon.socket()));
}

TEST(MuxStart, ALookAtTheUnitThatTimesOutIsNotAMissingUnit)
{
    TestDaemon daemon;
    std::vector<std::vector<std::string>> calls;
    // systemd-run fails (the unit exists), and systemctl does not answer:
    // whether the unit exists is not known.
    const UserManager manager = fakeManager(calls, [](const std::vector<std::string>& argv, std::string*) {
        return argv.front() == "systemd-run" ? 1 : UserManager::TimedOut;
    });
    std::string error;
    const Reached reached =
        connectOrStart(daemon.sockets(), missingDaemon(daemon), environmentOf(daemon), manager, &error);
    EXPECT_FALSE(reached.connection);
    EXPECT_EQ(error, NoAnswer);
    EXPECT_FALSE(exists(daemon.socket()));
}

TEST(MuxStart, ADaemonThatAnotherCommandStartedAnswersAfterALookThatTimesOut)
{
    TestDaemon daemon;
    std::vector<std::vector<std::string>> calls;
    // systemd-run fails: another command started the unit's daemon at the
    // same time, which listens before the look at the unit, which the
    // manager does not answer.
    const UserManager manager = fakeManager(calls, [&daemon](const std::vector<std::string>& argv, std::string*) {
        if (argv.front() != "systemd-run")
            return UserManager::TimedOut;
        return daemon.start(false) ? 1 : -1;
    });
    std::string error;
    const Reached reached =
        connectOrStart(daemon.sockets(), missingDaemon(daemon), environmentOf(daemon), manager, &error);
    ASSERT_TRUE(daemon.alive()) << daemon.errors();
    // Not the message that the manager does not answer: the last look at
    // the sockets finds the other command's daemon.
    EXPECT_TRUE(reached.connection) << error << daemon.errors();
    EXPECT_EQ(error, "");
    EXPECT_EQ(std::count_if(calls.begin(), calls.end(),
                            [](const auto& argv) {
                                return argv.front() == "systemd-run";
                            }),
              1);
}
