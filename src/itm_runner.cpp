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
#include <phosphor-logging/lg2.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
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
        on_progress_(std::move(on_progress))
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
        if (pct != k_no_update && pct != last_progress_)
        {
            last_progress_ = pct;
            on_progress_(pct);
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
    std::move_only_function<void(uint8_t) const> on_progress_;
    uint8_t last_progress_{0};
    bool stopped_ = false;
};

class ItmProcess final : public std::enable_shared_from_this<ItmProcess>
{
  public:
    ItmProcess(boost::asio::io_context& io,
               std::move_only_function<void(int) const> done) :
        deadline_(io), drain_(io), done_(std::move(done))
    {}

    void start(std::shared_ptr<bpv2::process> proc,
               std::shared_ptr<ProgressPoller> poller,
               std::shared_ptr<OutputTee> tee, int timeout_sec);

    void on_drained()
    {
        drained_ = true;
        finish();
    }

  private:
    void on_deadline_expired(int timeout_sec);
    void on_process_exit(const boost::system::error_code& ec, int exit_code);
    void on_drain_timeout();
    void finish();

    std::shared_ptr<bpv2::process> proc_;
    std::shared_ptr<ProgressPoller> poller_;
    std::shared_ptr<OutputTee> tee_;
    boost::asio::steady_timer deadline_;
    boost::asio::steady_timer drain_;
    std::move_only_function<void(int) const> done_;
    bool timed_out_{false};
    bool exited_{false};
    bool drained_{false};
    bool finished_{false};
    int exit_code_{-1};
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
    log_to_results("ERR", std::format("kist_itm exceeded SW timeout ({}s), "
                                      "terminating",
                                      timeout_sec));
    lg2::error("kist_itm exceeded SW timeout ({TIMEOUT}s), terminating",
               "TIMEOUT", timeout_sec);
    timed_out_ = true;
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
    deadline_.cancel();
    proc_.reset();

    if (timed_out_)
    {
        lg2::error("kist_itm killed due to SW timeout");
        exit_code_ = itm_exit_sw_timeout;
    }
    else if (ec)
    {
        log_to_results("ERR", std::format("kist_itm wait failed: ec={} exit={}",
                                          ec.message(), exit_code));
        lg2::error("kist_itm wait failed: ec={ERROR} exit={EXIT_CODE}", "ERROR",
                   ec.message(), "EXIT_CODE", exit_code);
        exit_code_ = -1;
    }
    else
    {
        exit_code_ = exit_code;
    }

    exited_ = true;
    drain_.expires_after(k_drain_timeout);
    drain_.async_wait(
        [weak = weak_from_this()](const boost::system::error_code& tec) {
            if (tec)
            {
                return;
            }
            if (std::shared_ptr<ItmProcess> self = weak.lock())
            {
                self->on_drain_timeout();
            }
        });
    finish();
}

void ItmProcess::on_drain_timeout()
{
    lg2::warning("kist_itm output did not drain, reporting anyway");
    drained_ = true;
    finish();
}

// Closing the read stream here is what used to discard kist_itm's closing
// lines, so completion waits for the pump's EOF instead.
void ItmProcess::finish()
{
    if (finished_ || !exited_ || !drained_)
    {
        return;
    }
    finished_ = true;
    drain_.cancel();
    if (tee_)
    {
        tee_->stop();
        tee_.reset();
    }
    done_(exit_code_);
}

class ItmRunnerImpl final : public ItmRunner
{
  public:
    explicit ItmRunnerImpl(boost::asio::io_context& io) : io_(io)
    {}
    void async_run(
        const IstTestConfig& cfg, const IstPlatformConfig& platform_cfg,
        std::move_only_function<void(int) const> done,
        std::move_only_function<void(uint8_t) const> on_progress) override;

  private:
    boost::asio::io_context& io_;
    std::shared_ptr<ItmProcess> active_;
    // Outlives the OutputTee that borrows it, and is closed when kist_itm
    // completes so the archive does not tar a file still being written.
    LogFile itm_log_;
};

// ---------------------
// ITM process helpers
// ---------------------

static std::vector<std::string>
    build_itm_args(const IstTestConfig& cfg,
                   const IstPlatformConfig& platform_cfg)
{
    std::vector<std::string> args;

    if (!platform_cfg.itm_lib_dir.empty())
    {
        args.emplace_back("/usr/bin/env");
        args.emplace_back("LD_LIBRARY_PATH=" +
                          platform_cfg.itm_lib_dir.string());
    }

    args.emplace_back(platform_cfg.itm_binary_path);

    if (!platform_cfg.storage.vector_mount_path.empty())
    {
        args.emplace_back("--ist_package_path");
        args.emplace_back(platform_cfg.storage.vector_mount_path);
    }
    if (!platform_cfg.storage.result_storage_path.empty())
    {
        args.emplace_back("--ist_results_path");
        args.emplace_back(platform_cfg.storage.result_storage_path);
    }
    if (!platform_cfg.hook_dir.empty())
    {
        args.emplace_back("--ist_hook_path");
        args.emplace_back(platform_cfg.hook_dir);
    }

    // Optional test params
    if (cfg.custom_test_list)
    {
        args.emplace_back("--custom_test_list");
        args.emplace_back(*cfg.custom_test_list);
    }
    if (cfg.custom_socket_list)
    {
        args.emplace_back("--custom_socket_list");
        args.emplace_back(*cfg.custom_socket_list);
    }
    if (cfg.continue_on_fail)
    {
        args.emplace_back("--ist_continueonfail");
        args.emplace_back(*cfg.continue_on_fail ? "yes" : "no");
    }
    if (cfg.save_res_on_pass)
    {
        args.emplace_back("--ist_save_res_on_pass");
        args.emplace_back(*cfg.save_res_on_pass ? "enable" : "disable");
    }
    if (cfg.save_res_on_fail)
    {
        args.emplace_back("--ist_save_res_on_fail");
        args.emplace_back(*cfg.save_res_on_fail ? "enable" : "disable");
    }

    return args;
}

void ItmRunnerImpl::async_run(
    const IstTestConfig& cfg, const IstPlatformConfig& platform_cfg,
    std::move_only_function<void(int) const> done,
    std::move_only_function<void(uint8_t) const> on_progress)
{
    if (platform_cfg.storage.result_storage_path.empty())
    {
        log_to_results("ERR", "resultStoragePath not found in platform "
                              "config");
        lg2::error("resultStoragePath not found in platform config");
        done(-1);
        return;
    }
    fs::path progress_path =
        platform_cfg.storage.result_storage_path / "progress.txt";

    // Remove stale progress file from a previous run so the poller
    // doesn't read old data and immediately report 100%.
    std::error_code rm_ec;
    fs::remove(progress_path, rm_ec); // May not exist yet; failure is harmless

    std::vector<std::string> args = build_itm_args(cfg, platform_cfg);

    fs::path log_path = platform_cfg.storage.result_storage_path /
                        "execute_ist_IST_Package_Summary.log";
    if (!itm_log_.open(log_path))
    {
        done(-1);
        return;
    }

    int pipe_fds[2];
    if (::pipe(pipe_fds) < 0)
    {
        int err = errno;
        log_to_results(
            "ERR",
            std::format("Failed to create pipe for ITM output: {}", err));
        lg2::error("Failed to create pipe for ITM output: {ERRNO}", "ERRNO",
                   err);
        itm_log_.close();
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
        log_to_results("ERR",
                       std::format("Failed to execute kist_itm: {}", e.what()));
        lg2::error("Failed to execute kist_itm: {ERROR}", "ERROR", e.what());
        itm_log_.close();
        done(-1);
        return;
    }

    // Close write-end in parent so OutputTee sees EOF when kist_itm exits
    pipe_write = UniqueFd();

    std::shared_ptr<ProgressPoller> poller = std::make_shared<ProgressPoller>(
        io_, progress_path, std::move(on_progress));
    poller->start();

    int timeout_sec = cfg.sw_timeout_sec.value_or(45 * 60);

    std::move_only_function<void(int) const> on_done =
        [this, done = std::move(done)](int exit_code) {
            itm_log_.close();
            done(exit_code);
        };

    active_ = std::make_shared<ItmProcess>(io_, std::move(on_done));

    std::shared_ptr<OutputTee> tee =
        std::make_shared<OutputTee>(io_, pipe_read.release(), itm_log_);
    tee->set_eof_handler([weak = std::weak_ptr<ItmProcess>(active_)]() {
        if (std::shared_ptr<ItmProcess> itm = weak.lock())
        {
            itm->on_drained();
        }
    });
    tee->start();

    active_->start(std::move(proc), std::move(poller), std::move(tee),
                   timeout_sec);
}

std::unique_ptr<ItmRunner> make_itm_runner(boost::asio::io_context& io)
{
    return std::make_unique<ItmRunnerImpl>(io);
}
