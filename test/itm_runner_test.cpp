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
#include <string>

#include <gtest/gtest.h>

namespace fs = std::filesystem;

class ItmRunnerTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        tmpDir_ = fs::temp_directory_path() /
                  ("itm_test_" + std::to_string(::getpid()));
        fs::create_directories(tmpDir_);
        runner_ = makeItmRunner(io_);

        platform_.storage.resultStoragePath = tmpDir_;
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(tmpDir_, ec);
    }

    // Write a small shell script and return its path.
    fs::path write_script(const std::string& name, const std::string& body)
    {
        fs::path path = tmpDir_ / name;
        std::ofstream f(path);
        f << "#!/bin/bash\n" << body << '\n';
        f.close();
        fs::permissions(path, fs::perms::owner_all);
        return path;
    }

    // Run the ITM runner with a script as the "binary".
    // The script receives build_itm_args flags (--ist_results_path etc.)
    // as positional args, which it can ignore.
    void run(const std::string& script_body, int timeout_sec = 30)
    {
        platform_.itmBinaryPath = write_script("itm_stub.sh", script_body);

        IstTestConfig cfg;
        cfg.swTimeoutSec = timeout_sec;
        runner_->asyncRun(
            cfg, platform_,
            [&](int code) {
                exitCode_ = code;
                called_ = true;
            },
            [&](uint8_t p) { lastProgress_ = p; });
        io_.run();
    }

    boost::asio::io_context io_;
    std::unique_ptr<ItmRunner> runner_;
    fs::path tmpDir_;
    IstPlatformConfig platform_;

    int exitCode_{-999};
    bool called_{false};
    uint8_t lastProgress_{0};
};

TEST_F(ItmRunnerTest, NormalExitReportsZero)
{
    run("exit 0");
    ASSERT_TRUE(called_);
    EXPECT_EQ(exitCode_, 0);
}

TEST_F(ItmRunnerTest, NonZeroExitReportsExitCode)
{
    run("exit 42");
    ASSERT_TRUE(called_);
    EXPECT_EQ(exitCode_, 42);
}

TEST_F(ItmRunnerTest, LogOpenFailureReportsError)
{
    platform_.storage.resultStoragePath = "/nonexistent/path";
    platform_.itmBinaryPath = "/bin/true";

    IstTestConfig cfg;
    cfg.swTimeoutSec = 30;
    runner_->asyncRun(
        cfg, platform_,
        [&](int code) {
            exitCode_ = code;
            called_ = true;
        },
        [&](uint8_t) {});
    io_.run();

    ASSERT_TRUE(called_);
    EXPECT_NE(exitCode_, 0) << "Log open failure must not report success";
}

TEST_F(ItmRunnerTest, EmptyResultStoragePathReportsError)
{
    platform_.storage.resultStoragePath.clear();

    IstTestConfig cfg;
    runner_->asyncRun(
        cfg, platform_,
        [&](int code) {
            exitCode_ = code;
            called_ = true;
        },
        [&](uint8_t) {});
    io_.run();

    ASSERT_TRUE(called_);
    EXPECT_NE(exitCode_, 0);
}

TEST_F(ItmRunnerTest, BadBinaryReportsError)
{
    platform_.storage.resultStoragePath = tmpDir_;
    platform_.itmBinaryPath = "/nonexistent/binary";

    IstTestConfig cfg;
    cfg.swTimeoutSec = 30;
    runner_->asyncRun(
        cfg, platform_,
        [&](int code) {
            exitCode_ = code;
            called_ = true;
        },
        [&](uint8_t) {});
    io_.run();

    ASSERT_TRUE(called_);
    EXPECT_NE(exitCode_, 0);
}

TEST_F(ItmRunnerTest, TimeoutReportsError)
{
    run("sleep 999", /*timeout_sec=*/1);
    ASSERT_TRUE(called_);
    EXPECT_NE(exitCode_, 0) << "Timed-out process must report error";
}

TEST_F(ItmRunnerTest, ProgressIsReported)
{
    // The script writes the progress file after a brief delay so the poller
    // (which runs on a 1-second timer) has time to pick it up. We cannot
    // pre-write the file because asyncRun removes stale progress files.
    std::string progress_path = (tmpDir_ / "progress.txt").string();
    run("sleep 1; echo '50 100' > " + progress_path + "; sleep 3", 10);
    ASSERT_TRUE(called_);
    EXPECT_EQ(exitCode_, 0);
    EXPECT_GE(lastProgress_, 50)
        << "Progress poller should have reported at least 50%";
}
