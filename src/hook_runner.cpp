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

#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/stdio.hpp>
#include <ist_app.hpp>
#include <phosphor-logging/lg2.hpp>

#include <chrono>
#include <csignal>
#include <cstring>
#include <format>
#include <memory>
#include <string>
#include <vector>

namespace bpv2 = boost::process::v2;

static constexpr std::chrono::seconds k_grace_timeout{5};

// Boost.Process v2 extension: make the child its own process group leader
// so the hook runner can signal the entire group on timeout.
struct ProcessGroupSetup
{
    boost::system::error_code on_exec_setup(bpv2::posix::default_launcher&,
                                            const bpv2::filesystem::path&,
                                            const char* const*)
    {
        if (::setpgid(0, 0) != 0)
        {
            return {errno, boost::system::generic_category()};
        }
        return {};
    }
};

class HookProcess final : public std::enable_shared_from_this<HookProcess>
{
  public:
    HookProcess(boost::asio::io_context& io, std::string what,
                std::move_only_function<void(bool) const> done) :
        deadline_(io), grace_(io), drain_(io), what_(std::move(what)),
        done_(std::move(done))
    {}

    void start(std::shared_ptr<bpv2::process> proc,
               std::shared_ptr<OutputTee> tee, std::chrono::seconds timeout)
    {
        proc_ = std::move(proc);
        pid_ = proc_->id();
        tee_ = std::move(tee);

        if (timeout <= std::chrono::seconds{0})
        {
            lg2::warning("{WHAT} timeout {TIMEOUT}s is invalid, using "
                         "default {DEFAULT}s",
                         "WHAT", what_, "TIMEOUT", timeout.count(), "DEFAULT",
                         HookRunner::k_default_timeout.count());
            timeout = HookRunner::k_default_timeout;
        }
        deadline_.expires_after(timeout);
        deadline_.async_wait(
            [weak = weak_from_this()](const boost::system::error_code& ec) {
                if (ec)
                {
                    return;
                }
                if (auto self = weak.lock())
                {
                    self->on_timeout();
                }
            });

        proc_->async_wait(
            [weak = weak_from_this()](const boost::system::error_code& ec,
                                      int exit_code) {
                if (auto self = weak.lock())
                {
                    self->on_exit(ec, exit_code);
                }
            });
    }

  private:
    void on_timeout()
    {
        if (!proc_)
        {
            return;
        }
        log_to_results("ERR", std::format("{} exceeded timeout, sending "
                                          "SIGTERM",
                                          what_));
        lg2::error("{WHAT} exceeded timeout, sending SIGTERM", "WHAT", what_);
        timed_out_ = true;
        if (::kill(-pid_, SIGTERM) != 0)
        {
            lg2::error("{WHAT} kill(SIGTERM) failed: {ERRNO}", "WHAT", what_,
                       "ERRNO", errno);
        }

        grace_.expires_after(k_grace_timeout);
        grace_.async_wait(
            [weak = weak_from_this()](const boost::system::error_code& ec) {
                if (ec)
                {
                    return;
                }
                if (auto self = weak.lock())
                {
                    self->on_grace_expired();
                }
            });
    }

    void on_grace_expired()
    {
        if (!proc_)
        {
            return;
        }
        lg2::error("{WHAT} did not exit after SIGTERM, sending SIGKILL", "WHAT",
                   what_);
        if (::kill(-pid_, SIGKILL) != 0)
        {
            lg2::error("{WHAT} kill(SIGKILL) failed: {ERRNO}", "WHAT", what_,
                       "ERRNO", errno);
        }
    }

    void on_exit(const boost::system::error_code& ec, int exit_code)
    {
        deadline_.cancel();
        grace_.cancel();
        proc_.reset();

        if (timed_out_)
        {
            lg2::error("{WHAT} killed due to timeout", "WHAT", what_);
            ok_ = false;
        }
        else if (ec || exit_code != 0)
        {
            log_to_results("ERR",
                           std::format("Failed to execute {}: ec={} exit={}",
                                       what_, ec.message(), exit_code));
            lg2::error("Failed to execute {WHAT}: ec={ERROR} exit={EXIT_CODE}",
                       "WHAT", what_, "ERROR", ec.message(), "EXIT_CODE",
                       exit_code);
            ok_ = false;
        }
        else
        {
            ok_ = true;
        }

        exited_ = true;
        drain_.expires_after(k_drain_timeout);
        drain_.async_wait(
            [weak = weak_from_this()](const boost::system::error_code& tec) {
                if (tec)
                {
                    return;
                }
                if (auto self = weak.lock())
                {
                    self->on_drain_timeout();
                }
            });
        finish();
    }

    void on_drain_timeout()
    {
        lg2::warning("{WHAT} output did not drain, reporting anyway", "WHAT",
                     what_);
        drained_ = true;
        finish();
    }

    // Completion means exited and drained, so the caller cannot archive or
    // close the log while the hook's last lines are still in the pipe.
    void finish()
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
        std::move_only_function<void(bool) const> cb = std::move(done_);
        if (cb)
        {
            cb(ok_);
        }
    }

    boost::asio::steady_timer deadline_;
    boost::asio::steady_timer grace_;
    boost::asio::steady_timer drain_;
    std::shared_ptr<bpv2::process> proc_;
    std::shared_ptr<OutputTee> tee_;
    pid_t pid_{0};
    std::string what_;
    std::move_only_function<void(bool) const> done_;
    bool timed_out_{false};
    bool finished_{false};
    bool exited_{false};
    bool drained_{false};
    bool ok_{false};

  public:
    void on_drained()
    {
        drained_ = true;
        finish();
    }

    bool is_finished() const
    {
        return finished_;
    }
};

class HookRunnerImpl final : public HookRunner
{
  public:
    explicit HookRunnerImpl(boost::asio::io_context& io) : io_(io)
    {}
    void set_log_file(LogFile& log) override
    {
        log_ = &log;
    }
    void async_run(const std::string& cmd, std::string what,
                   std::move_only_function<void(bool) const> done,
                   std::vector<std::string> args = {},
                   std::chrono::seconds timeout = k_default_timeout) override;

  private:
    boost::asio::io_context& io_;
    // Hooks can run before the service has a run log and after that log is
    // closed; a LogFile that was never opened discards writes, so neither case
    // needs a special path.
    LogFile unopened_log_;
    LogFile* log_{&unopened_log_};
    std::vector<std::shared_ptr<HookProcess>> active_;
};

void HookRunnerImpl::async_run(const std::string& cmd, std::string what,
                               std::move_only_function<void(bool) const> done,
                               std::vector<std::string> args,
                               std::chrono::seconds timeout)
{
    std::erase_if(active_, [](const std::shared_ptr<HookProcess>& hp) {
        return hp->is_finished();
    });

    int pipe_fds[2];
    if (::pipe(pipe_fds) < 0)
    {
        lg2::error("Failed to create pipe for {WHAT} output: {ERRNO}", "WHAT",
                   what, "ERRNO", errno);
        done(false);
        return;
    }
    UniqueFd pipe_read(pipe_fds[0]);
    UniqueFd pipe_write(pipe_fds[1]);

    std::shared_ptr<bpv2::process> proc;
    try
    {
        proc = std::make_shared<bpv2::process>(
            io_, cmd, std::move(args),
            bpv2::process_stdio{.in = nullptr,
                                .out = pipe_write.get(),
                                .err = pipe_write.get()},
            ProcessGroupSetup{});
    }
    catch (const std::exception& e)
    {
        log_to_results("ERR",
                       std::format("Failed to execute {}: {}", what, e.what()));
        lg2::error("Failed to execute {WHAT}: {ERROR}", "WHAT", what, "ERROR",
                   e.what());
        done(false);
        return;
    }

    // Closed in the parent so the pump sees EOF when the hook exits.
    pipe_write = UniqueFd();

    std::shared_ptr<HookProcess> hp =
        std::make_shared<HookProcess>(io_, std::move(what), std::move(done));
    active_.push_back(hp);

    std::shared_ptr<OutputTee> tee =
        std::make_shared<OutputTee>(io_, pipe_read.release(), *log_);
    tee->set_eof_handler([weak = std::weak_ptr<HookProcess>(hp)]() {
        if (std::shared_ptr<HookProcess> hook = weak.lock())
        {
            hook->on_drained();
        }
    });
    tee->start();

    hp->start(std::move(proc), std::move(tee), timeout);
}

std::unique_ptr<HookRunner> make_hook_runner(boost::asio::io_context& io)
{
    return std::make_unique<HookRunnerImpl>(io);
}
