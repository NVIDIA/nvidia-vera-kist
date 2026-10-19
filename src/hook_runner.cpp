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

#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
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
            std::cerr << what_ << " timeout " << timeout.count()
                      << "s is invalid, using default "
                      << HookRunner::k_default_timeout.count() << "s\n";
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
        std::cerr << what_ << " exceeded timeout, sending SIGTERM\n";
        timed_out_ = true;
        if (::kill(-pid_, SIGTERM) != 0)
        {
            std::cerr << what_ << " kill(SIGTERM) failed: " << strerror(errno)
                      << '\n';
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
        std::cerr << what_ << " did not exit after SIGTERM, sending SIGKILL\n";
        if (::kill(-pid_, SIGKILL) != 0)
        {
            std::cerr << what_ << " kill(SIGKILL) failed: " << strerror(errno)
                      << '\n';
        }
    }

    void on_exit(const boost::system::error_code& ec, int exit_code)
    {
        deadline_.cancel();
        grace_.cancel();
        proc_.reset();

        if (timed_out_)
        {
            std::cerr << what_ << " killed due to timeout\n";
            ok_ = false;
        }
        else if (ec || exit_code != 0)
        {
            std::cerr << "Failed to execute " << what_
                      << ": ec=" << ec.message() << " exit=" << exit_code
                      << '\n';
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
        std::cerr << what_ << " output did not drain, reporting anyway\n";
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
        std::cerr << "Failed to create pipe for " << what
                  << " output: " << strerror(errno) << '\n';
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
        std::cerr << "Failed to execute " << what << ": " << e.what() << '\n';
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
