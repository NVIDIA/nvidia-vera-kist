/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <unistd.h>

#include <boost/asio/io_context.hpp>
#include <ist_app.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

class HookRunnerTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        tmp_dir_ = fs::temp_directory_path() /
                   ("hook_test_" + std::to_string(::getpid()));
        fs::create_directories(tmp_dir_);
        runner_ = make_hook_runner(io_);
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(tmp_dir_, ec);
    }

    boost::asio::io_context io_;
    std::unique_ptr<HookRunner> runner_;
    fs::path tmp_dir_;
};

TEST_F(HookRunnerTest, NormalExitReportsSuccess)
{
    bool result = false;
    bool called = false;
    runner_->async_run(
        "/bin/bash", "test-normal",
        [&](bool ok) {
            result = ok;
            called = true;
        },
        {"-c", "exit 0"}, 10s);
    io_.run();
    ASSERT_TRUE(called);
    EXPECT_TRUE(result);
}

TEST_F(HookRunnerTest, NonZeroExitReportsFailure)
{
    bool result = true;
    bool called = false;
    runner_->async_run(
        "/bin/bash", "test-fail",
        [&](bool ok) {
            result = ok;
            called = true;
        },
        {"-c", "exit 1"}, 10s);
    io_.run();
    ASSERT_TRUE(called);
    EXPECT_FALSE(result);
}

// A hook the service launches directly produces no other diagnostics, so both
// of its streams have to end up in the run log, not just the journal.
TEST_F(HookRunnerTest, HookOutputReachesRunLog)
{
    fs::path log_path = tmp_dir_ / "run.log";
    LogFile log;
    ASSERT_TRUE(log.open(log_path));
    runner_->set_log_file(log);

    bool called = false;
    runner_->async_run(
        "/bin/bash", "test-output",
        [&](bool ok) {
            (void)ok;
            called = true;
        },
        {"-c", "echo on-stdout; echo on-stderr >&2"}, 10s);
    io_.run();
    log.close();

    ASSERT_TRUE(called);
    std::ifstream in(log_path);
    std::stringstream contents;
    contents << in.rdbuf();
    EXPECT_NE(contents.str().find("on-stdout"), std::string::npos);
    EXPECT_NE(contents.str().find("on-stderr"), std::string::npos);
}

// Hooks run outside a test run too, when nothing owns a log to write to.
TEST_F(HookRunnerTest, OutputWithNoRunLogIsNotFatal)
{
    bool result = false;
    bool called = false;
    runner_->async_run(
        "/bin/bash", "test-no-log",
        [&](bool ok) {
            result = ok;
            called = true;
        },
        {"-c", "echo orphaned-output"}, 10s);
    io_.run();
    ASSERT_TRUE(called);
    EXPECT_TRUE(result);
}

TEST_F(HookRunnerTest, BadCommandReportsFailure)
{
    bool result = true;
    bool called = false;
    runner_->async_run("/nonexistent/command", "test-bad-cmd", [&](bool ok) {
        result = ok;
        called = true;
    });
    io_.run();
    ASSERT_TRUE(called);
    EXPECT_FALSE(result);
}

TEST_F(HookRunnerTest, ArgsArePassedToProcess)
{
    bool result = false;
    bool called = false;
    runner_->async_run("/bin/bash", "test-args",
                       [&](bool ok) {
                           result = ok;
                           called = true;
                       },
                       {"-c", "test \"$0\" = hello", "hello"});
    io_.run();
    ASSERT_TRUE(called);
    EXPECT_TRUE(result);
}

TEST_F(HookRunnerTest, TimeoutKillsProcess)
{
    bool result = true;
    bool called = false;
    runner_->async_run(
        "/bin/bash", "test-timeout",
        [&](bool ok) {
            result = ok;
            called = true;
        },
        {"-c", "sleep 999"}, 1s);
    io_.run();
    ASSERT_TRUE(called);
    EXPECT_FALSE(result);
}

TEST_F(HookRunnerTest, TimeoutAllowsTrapCleanup)
{
    fs::path marker = tmp_dir_ / "cleanup_ran";
    std::string script =
        "trap 'touch " + marker.string() + "; exit 0' TERM; sleep 999";

    bool called = false;
    runner_->async_run(
        "/bin/bash", "test-trap-cleanup",
        [&](bool ok) {
            (void)ok;
            called = true;
        },
        {"-c", script}, 1s);
    io_.run();
    ASSERT_TRUE(called);
    EXPECT_TRUE(fs::exists(marker))
        << "SIGTERM trap handler should have run before the process was killed";
}

// ~6s (1s timeout + 5s grace): script traps SIGTERM but hangs, so SIGKILL
// must be used.
TEST_F(HookRunnerTest, SIGKILLEscalationWhenSIGTERMIgnored)
{
    std::string script = "trap 'while true; do sleep 1; done' TERM; sleep 999";

    bool result = true;
    bool called = false;
    std::chrono::steady_clock::time_point start =
        std::chrono::steady_clock::now();
    runner_->async_run(
        "/bin/bash", "test-sigkill",
        [&](bool ok) {
            result = ok;
            called = true;
        },
        {"-c", script}, 1s);
    io_.run();
    std::chrono::steady_clock::duration elapsed =
        std::chrono::steady_clock::now() - start;

    ASSERT_TRUE(called);
    EXPECT_FALSE(result);
    EXPECT_GT(elapsed, 5s) << "Should have waited for grace period";
    EXPECT_LT(elapsed, 15s) << "Should not hang forever";
}

// ist_em.sh against a fake busctl serving one line per property:
// <interface less xyz.openbmc_project.Configuration.> <path> <property>
// <busctl-typed value>, or FAIL for a property that cannot be read
class IstEmTest : public HookRunnerTest
{
  protected:
    bool load(const std::string& records, const std::string& expect,
              const std::string& value = "$IST_CPU0_EID $IST_CPU1_EID|"
                                         "$IST_D0_CPUS")
    {
        std::ofstream(tmp_dir_ / "records") << board << records;
        std::ofstream(tmp_dir_ / "busctl") << R"(#!/bin/bash
db=$(dirname "$0")/records
cfg=xyz.openbmc_project.Configuration.
if [ "$1" = call ]; then
    i=${!#}
    p=$(awk -v i="${i#$cfg}" '$1 == i { print "\"" $2 "\"" }' "$db" | sort -u)
    echo "as $(echo "$p" | grep -c .) $p"
else
    v=$(awk -v p="$3" -v i="${4#$cfg}" -v k="$5" '$1 == i && $2 == p &&
        $3 == k { $1 = $2 = $3 = ""; sub(/^ +/, ""); print }' "$db")
    [ "$v" = FAIL ] && echo "Connection timed out" >&2 && exit 1
    [ -z "$v" ] && echo "Unknown interface $4 or property $5." >&2 && exit 1
    echo "$v"
fi
)";
        fs::permissions(tmp_dir_ / "busctl", fs::perms::owner_all);
        bool result = false;
        runner_->async_run(
            "/bin/bash", "test-ist-em", [&](bool ok) { result = ok; },
            {"-c", "PATH=" + tmp_dir_.string() +
                       ":$PATH; . " IST_HOOKS_DIR
                       "/ist_em.sh && ist_em_load && test \"" +
                       value + "\" = \"" + expect + "\""},
            10s);
        io_.restart();
        io_.run();
        return result;
    }

    static constexpr auto board = "ISTDomain /b/D0 Index t 0\n"
                                  "ISTDomain /b/D0 Cpus at 2 0 1\n";

    static std::string nv_device(const std::string& name,
                                 const std::string& kind, int eid)
    {
        const std::string p = " /b/" + name + " ";
        return "NvDevice" + p + "Name s \"" + name + "\"\nNvDevice" + p +
               "Kind s \"" + kind + "\"\nNvDevice" + p + "EID t " +
               std::to_string(eid) + "\n";
    }

    static std::string usbrcm(const std::string& name, int eid)
    {
        const std::string p = "USBRCMRecovery /b/" + name + " ";
        return p + "Name s \"" + name + "\"\n" + p + "MctpEID t " +
               std::to_string(eid) + "\n";
    }
};

// GPU_0 is not a CPU and must not take CPU 0's EID.
TEST_F(IstEmTest, CpusFromNvDevice)
{
    EXPECT_TRUE(load(nv_device("CPU_0", "cpu", 16) +
                         nv_device("CPU_1", "cpu", 17) +
                         nv_device("GPU_0", "gpu", 30),
                     "16 17|0 1"));
}

TEST_F(IstEmTest, CpusFromUSBRCMRecoveryWithoutCpuNvDevice)
{
    EXPECT_TRUE(load(usbrcm("FW_CPU_0", 16) + usbrcm("FW_CPU_1", 17) +
                         nv_device("GPU_0", "gpu", 30),
                     "16 17|0 1"));
}

TEST_F(IstEmTest, NvDeviceTakesPrecedenceOverUSBRCMRecovery)
{
    EXPECT_TRUE(load(nv_device("HGX_CPU_0", "cpu", 14) +
                         nv_device("HGX_CPU_1", "cpu", 15) +
                         usbrcm("HGX_FW_CPU_1", 24),
                     "14 15|0 1"));
}

// The domain lists two CPUs and the board fits one (MGX C1): CPU 1 is left
// out and the CAK bypass needs only CPU 0.
TEST_F(IstEmTest, UnfittedDomainCpuLeftOut)
{
    const std::string value = "$IST_CPU0_EID|$IST_D0_CPUS|$IST_REQUIRE_CPUS";
    EXPECT_TRUE(load(nv_device("CPU_0", "cpu", 16), "16|0|1", value));
    EXPECT_TRUE(load(usbrcm("FW_CPU_0", 16), "16|0|1", value));
}

// The CAK bypass DOT objects share the CPU records' platform prefix.
TEST_F(IstEmTest, DotPrefixFromCpuNames)
{
    const std::string value = "$IST_DOT_PREFIX $IST_REQUIRE_CPUS";
    EXPECT_TRUE(load(nv_device("HGX_CPU_0", "cpu", 14) +
                         nv_device("HGX_CPU_1", "cpu", 15),
                     "/xyz/openbmc_project/DOT/HGX_IRoT_CPU_ 2", value));
    EXPECT_TRUE(
        load(nv_device("CPU_0", "cpu", 16) + nv_device("CPU_1", "cpu", 17),
             "/xyz/openbmc_project/DOT/IRoT_CPU_ 2", value));
    EXPECT_TRUE(load(usbrcm("HGX_FW_CPU_0", 16) + usbrcm("HGX_FW_CPU_1", 17),
                     "/xyz/openbmc_project/DOT/HGX_IRoT_CPU_ 2", value));
}
