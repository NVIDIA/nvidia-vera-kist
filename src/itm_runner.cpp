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
#include <fcntl.h>
#include <unistd.h>

#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/stdio.hpp>
#include <ist_app.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace bpv2 = boost::process::v2;
namespace fs = std::filesystem;

// Lifetime note: ProgressPoller is stopped in the proc->async_wait
// callback which fires before io.run() returns.
//
// Each poll() reads the progress file on a detached worker thread
// and sends the percentage byte back via a pipe so the main
// io_context thread is never blocked by filesystem I/O.
class ProgressPoller final : public std::enable_shared_from_this<ProgressPoller>
{
  public:
    ProgressPoller(boost::asio::io_context& io, std::string path,
                   std::move_only_function<void(uint8_t) const> on_progress) :
        io_(io), timer_(io), path_(std::move(path)),
        onProgress_(std::move(on_progress))
    {}

    ~ProgressPoller()
    {
        stopped_ = true;
        timer_.cancel();
    }

    void start()
    {
        poll();
    }
    void stop()
    {
        stopped_ = true;
        timer_.cancel();
        read_progress_sync();
    }

  private:
    static constexpr uint8_t k_no_update = 0xFF;

    // Progress file format: "<completed> <total>\n" (two decimal integers).
    static uint8_t parse_progress_file(const std::string& path)
    {
        std::ifstream input_file(path);
        if (!input_file.is_open())
        {
            return k_no_update;
        }

        std::string line;
        if (!std::getline(input_file, line))
        {
            return k_no_update;
        }

        try
        {
            std::size_t pos = 0;
            int completed = std::stoi(line, &pos);
            int total = std::stoi(line.substr(pos));
            if (total <= 0)
            {
                return k_no_update;
            }
            return static_cast<uint8_t>(std::clamp(
                static_cast<int>(100.0 * completed / total), 0, 100));
        }
        catch (const std::exception&)
        {
            return k_no_update;
        }
    }

    void report(uint8_t pct)
    {
        if (pct != k_no_update && pct != lastProgress_)
        {
            lastProgress_ = pct;
            onProgress_(pct);
        }
    }

    // Synchronous final read — used only in stop()
    void read_progress_sync()
    {
        report(parse_progress_file(path_));
    }

    void poll()
    {
        if (stopped_)
        {
            return;
        }

        int fds[2];
        if (::pipe2(fds, O_CLOEXEC) < 0)
        {
            schedule_next_poll();
            return;
        }

        auto stream = std::make_shared<boost::asio::posix::stream_descriptor>(
            io_, fds[0]);
        std::shared_ptr<uint8_t> result =
            std::make_shared<uint8_t>(k_no_update);

        stream->async_read_some(
            boost::asio::buffer(result.get(), 1),
            [weak = weak_from_this(), stream,
             result](const boost::system::error_code& ec, size_t) {
                std::shared_ptr<ProgressPoller> self = weak.lock();
                if (!self || ec || self->stopped_)
                {
                    return;
                }
                self->report(*result);
                self->schedule_next_poll();
            });

        std::string path = path_;
        std::thread([path, write_fd = fds[1]]() {
            uint8_t pct = parse_progress_file(path);
            std::ignore = ::write(write_fd, &pct, 1);
            ::close(write_fd);
        }).detach();
    }

    void schedule_next_poll()
    {
        using namespace std::chrono_literals;
        if (stopped_)
        {
            return;
        }

        timer_.expires_after(1s);
        timer_.async_wait(
            [weak = weak_from_this()](const boost::system::error_code& ec) {
                if (ec)
                {
                    return;
                }
                std::shared_ptr<ProgressPoller> self = weak.lock();
                if (!self)
                {
                    return;
                }
                self->poll();
            });
    }

    boost::asio::io_context& io_;
    boost::asio::steady_timer timer_;
    std::string path_;
    std::move_only_function<void(uint8_t) const> onProgress_;
    uint8_t lastProgress_{0};
    bool stopped_ = false;
};

// Reads from a pipe fd asynchronously and writes each chunk to both a log
// file and stderr (which journald captures).  Stops automatically on EOF.
class OutputTee final : public std::enable_shared_from_this<OutputTee>
{
  public:
    OutputTee(boost::asio::io_context& io, int pipe_read_fd, UniqueFd log_fd) :
        stream_(io, pipe_read_fd), logFd_(std::move(log_fd))
    {}

    void start()
    {
        read_next();
    }

    void stop()
    {
        boost::system::error_code ec;
        // Return value discarded; errors are reported via ec
        std::ignore = stream_.close(ec);
        if (ec)
        {
            std::cerr << "OutputTee: failed to close stream: " << ec.message()
                      << "\n";
        }
    }

  private:
    void read_next()
    {
        stream_.async_read_some(
            boost::asio::buffer(buf_),
            [self = shared_from_this()](const boost::system::error_code& ec,
                                        std::size_t n) {
                if (ec)
                {
                    return; // EOF or pipe closed
                }
                // Write to log file
                const char* data = self->buf_.data();
                std::size_t remaining = n;
                while (remaining > 0)
                {
                    ssize_t written =
                        ::write(self->logFd_.get(), data, remaining);
                    if (written <= 0)
                    {
                        std::cerr << "OutputTee: write to log failed: " << errno
                                  << "\n";
                        break;
                    }
                    data += written;
                    remaining -= static_cast<std::size_t>(written);
                }
                // Write to stderr for journald
                std::ignore = ::write(STDERR_FILENO, self->buf_.data(), n);

                self->read_next();
            });
    }

    boost::asio::posix::stream_descriptor stream_;
    UniqueFd logFd_;
    std::array<char, 4096> buf_{};
};

class ItmProcess final : public std::enable_shared_from_this<ItmProcess>
{
  public:
    ItmProcess(boost::asio::io_context& io,
               std::move_only_function<void(int) const> done) :
        deadline_(io), done_(std::move(done))
    {}

    void start(std::shared_ptr<bpv2::process> proc,
               std::shared_ptr<ProgressPoller> poller,
               std::shared_ptr<OutputTee> tee, int timeout_sec);

  private:
    void on_deadline_expired(int timeout_sec);
    void on_process_exit(const boost::system::error_code& ec, int exit_code);

    std::shared_ptr<bpv2::process> proc_;
    std::shared_ptr<ProgressPoller> poller_;
    std::shared_ptr<OutputTee> tee_;
    boost::asio::steady_timer deadline_;
    std::move_only_function<void(int) const> done_;
    bool timedOut_{false};
};

void ItmProcess::start(std::shared_ptr<bpv2::process> proc,
                       std::shared_ptr<ProgressPoller> poller,
                       std::shared_ptr<OutputTee> tee, int timeout_sec)
{
    proc_ = std::move(proc);
    poller_ = std::move(poller);
    tee_ = std::move(tee);

    deadline_.expires_after(std::chrono::seconds(timeout_sec));
    deadline_.async_wait([weak = weak_from_this(),
                          timeout_sec](const boost::system::error_code& ec) {
        if (ec) // Timer cancelled — process exited before deadline
        {
            return;
        }
        std::shared_ptr<ItmProcess> self = weak.lock();
        if (!self)
        {
            return; // ItmProcess was destroyed; normal during shutdown
        }
        self->on_deadline_expired(timeout_sec);
    });

    proc_->async_wait([weak = weak_from_this()](
                          const boost::system::error_code& ec, int exit_code) {
        std::shared_ptr<ItmProcess> self = weak.lock();
        if (!self)
        {
            return; // ItmProcess was destroyed; normal during shutdown
        }
        self->on_process_exit(ec, exit_code);
    });
}

void ItmProcess::on_deadline_expired(int timeout_sec)
{
    if (!proc_)
    {
        return; // Process already cleaned up after exit
    }
    std::cerr << "kist_itm exceeded SW timeout (" << timeout_sec
              << "s), terminating\n";
    timedOut_ = true;
    proc_->terminate();
}

void ItmProcess::on_process_exit(const boost::system::error_code& ec,
                                 int exit_code)
{
    if (poller_)
    {
        poller_->stop();
        poller_.reset();
    }
    if (tee_)
    {
        tee_->stop();
        tee_.reset();
    }
    deadline_.cancel();
    proc_.reset();

    if (timedOut_)
    {
        std::cerr << "kist_itm killed due to SW timeout\n";
        done_(-1);
        return;
    }
    if (ec)
    {
        std::cerr << "kist_itm wait failed: ec=" << ec.message()
                  << " exit=" << exit_code << '\n';
        done_(-1);
        return;
    }
    done_(exit_code);
}

class ItmRunnerImpl final : public ItmRunner
{
  public:
    explicit ItmRunnerImpl(boost::asio::io_context& io) : io_(io)
    {}
    void asyncRun(
        const IstTestConfig& cfg, const IstPlatformConfig& platform_cfg,
        std::move_only_function<void(int) const> done,
        std::move_only_function<void(uint8_t) const> on_progress) override;

  private:
    boost::asio::io_context& io_;
    std::shared_ptr<ItmProcess> active_;
};

// ---------------------
// ITM process helpers
// ---------------------

static std::vector<std::string>
    build_itm_args(const IstTestConfig& cfg,
                   const IstPlatformConfig& platform_cfg)
{
    std::vector<std::string> args;

    if (!platform_cfg.itmLibDir.empty())
    {
        args.emplace_back("/usr/bin/env");
        args.emplace_back("LD_LIBRARY_PATH=" + platform_cfg.itmLibDir.string());
    }

    args.emplace_back(platform_cfg.itmBinaryPath);

    if (!platform_cfg.storage.vectorMountPath.empty())
    {
        args.emplace_back("--ist_package_path");
        args.emplace_back(platform_cfg.storage.vectorMountPath);
    }
    if (!platform_cfg.storage.resultStoragePath.empty())
    {
        args.emplace_back("--ist_results_path");
        args.emplace_back(platform_cfg.storage.resultStoragePath);
    }
    if (!platform_cfg.hookDir.empty())
    {
        args.emplace_back("--ist_hook_path");
        args.emplace_back(platform_cfg.hookDir);
    }

    // Optional test params
    if (cfg.customTestList)
    {
        args.emplace_back("--custom_test_list");
        args.emplace_back(*cfg.customTestList);
    }
    if (cfg.customSocketList)
    {
        args.emplace_back("--custom_socket_list");
        args.emplace_back(*cfg.customSocketList);
    }
    if (cfg.continueOnFail)
    {
        args.emplace_back("--ist_continueonfail");
        args.emplace_back(*cfg.continueOnFail ? "yes" : "no");
    }
    if (cfg.saveResOnPass)
    {
        args.emplace_back("--ist_save_res_on_pass");
        args.emplace_back(*cfg.saveResOnPass ? "enable" : "disable");
    }
    if (cfg.saveResOnFail)
    {
        args.emplace_back("--ist_save_res_on_fail");
        args.emplace_back(*cfg.saveResOnFail ? "enable" : "disable");
    }

    return args;
}

void ItmRunnerImpl::asyncRun(
    const IstTestConfig& cfg, const IstPlatformConfig& platform_cfg,
    std::move_only_function<void(int) const> done,
    std::move_only_function<void(uint8_t) const> on_progress)
{
    if (platform_cfg.storage.resultStoragePath.empty())
    {
        std::cerr << "resultStoragePath not found in platform config\n";
        done(-1);
        return;
    }
    fs::path progress_path =
        platform_cfg.storage.resultStoragePath / "progress.txt";

    // Remove stale progress file from a previous run so the poller
    // doesn't read old data and immediately report 100%.
    std::error_code rm_ec;
    fs::remove(progress_path, rm_ec); // May not exist yet; failure is harmless

    std::vector<std::string> args = build_itm_args(cfg, platform_cfg);

    fs::path log_path = platform_cfg.storage.resultStoragePath /
                        "execute_ist_IST_Package_Summary.log";
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    UniqueFd log_fd(::open(log_path.c_str(),
                           O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644));
    if (log_fd.get() < 0)
    {
        std::cerr << "Failed to open ITM log file '" << log_path
                  << "': " << strerror(errno) << '\n';
        done(-1);
        return;
    }

    int pipe_fds[2];
    if (::pipe(pipe_fds) < 0)
    {
        std::cerr << "Failed to create pipe for ITM output: " << strerror(errno)
                  << '\n';
        done(-1);
        return;
    }
    UniqueFd pipe_read(pipe_fds[0]);
    UniqueFd pipe_write(pipe_fds[1]);

    std::shared_ptr<bpv2::process> proc;
    try
    {
        proc = std::make_shared<bpv2::process>(
            io_, args[0],
            std::vector<std::string>(args.begin() + 1, args.end()),
            bpv2::process_stdio{.in = nullptr,
                                .out = pipe_write.get(),
                                .err = pipe_write.get()});
    }
    catch (const std::exception& e)
    {
        std::cerr << "Failed to execute kist_itm: " << e.what() << '\n';
        done(-1);
        return;
    }

    // Close write-end in parent so OutputTee sees EOF when kist_itm exits
    pipe_write = UniqueFd();

    auto tee = std::make_shared<OutputTee>(io_, pipe_read.release(),
                                           std::move(log_fd));
    tee->start();

    std::shared_ptr<ProgressPoller> poller = std::make_shared<ProgressPoller>(
        io_, progress_path, std::move(on_progress));
    poller->start();

    int timeout_sec = cfg.swTimeoutSec.value_or(45 * 60);

    active_ = std::make_shared<ItmProcess>(io_, std::move(done));
    active_->start(std::move(proc), std::move(poller), std::move(tee),
                   timeout_sec);
}

std::unique_ptr<ItmRunner> makeItmRunner(boost::asio::io_context& io)
{
    return std::make_unique<ItmRunnerImpl>(io);
}
