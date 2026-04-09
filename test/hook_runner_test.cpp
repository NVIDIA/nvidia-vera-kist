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
#include <string>

#include <gtest/gtest.h>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

class HookRunnerTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        tmpDir_ = fs::temp_directory_path() /
                  ("hook_test_" + std::to_string(::getpid()));
        fs::create_directories(tmpDir_);
        runner_ = makeHookRunner(io_);
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(tmpDir_, ec);
    }

    boost::asio::io_context io_;
    std::unique_ptr<HookRunner> runner_;
    fs::path tmpDir_;
};

TEST_F(HookRunnerTest, NormalExitReportsSuccess)
{
    bool result = false;
    bool called = false;
    runner_->asyncRun(
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
    runner_->asyncRun(
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

TEST_F(HookRunnerTest, TimeoutKillsProcess)
{
    bool result = true;
    bool called = false;
    runner_->asyncRun(
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
    fs::path marker = tmpDir_ / "cleanup_ran";
    std::string script =
        "trap 'touch " + marker.string() + "; exit 0' TERM; sleep 999";

    bool called = false;
    runner_->asyncRun(
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
    runner_->asyncRun(
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
