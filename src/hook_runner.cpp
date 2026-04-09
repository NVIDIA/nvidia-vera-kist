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
        deadline_(io), grace_(io), what_(std::move(what)),
        done_(std::move(done))
    {}

    void start(std::shared_ptr<bpv2::process> proc,
               std::chrono::seconds timeout)
    {
        proc_ = std::move(proc);
        pid_ = proc_->id();

        if (timeout <= std::chrono::seconds{0})
        {
            std::cerr << what_ << " timeout " << timeout.count()
                      << "s is invalid, using default "
                      << HookRunner::defaultTimeout.count() << "s\n";
            timeout = HookRunner::defaultTimeout;
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
        timedOut_ = true;
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

        if (timedOut_)
        {
            std::cerr << what_ << " killed due to timeout\n";
            finish(false);
            return;
        }
        if (ec || exit_code != 0)
        {
            std::cerr << "Failed to execute " << what_
                      << ": ec=" << ec.message() << " exit=" << exit_code
                      << '\n';
            finish(false);
            return;
        }
        finish(true);
    }

    void finish(bool ok)
    {
        if (finished_)
        {
            return;
        }
        finished_ = true;
        std::move_only_function<void(bool) const> cb = std::move(done_);
        if (cb)
        {
            cb(ok);
        }
    }

    boost::asio::steady_timer deadline_;
    boost::asio::steady_timer grace_;
    std::shared_ptr<bpv2::process> proc_;
    pid_t pid_{0};
    std::string what_;
    std::move_only_function<void(bool) const> done_;
    bool timedOut_{false};
    bool finished_{false};

  public:
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
    void asyncRun(const std::string& cmd, std::string what,
                  std::move_only_function<void(bool) const> done,
                  std::vector<std::string> args = {},
                  std::chrono::seconds timeout = defaultTimeout) override;

  private:
    boost::asio::io_context& io_;
    std::vector<std::shared_ptr<HookProcess>> active_;
};

void HookRunnerImpl::asyncRun(const std::string& cmd, std::string what,
                              std::move_only_function<void(bool) const> done,
                              std::vector<std::string> args,
                              std::chrono::seconds timeout)
{
    std::erase_if(active_, [](const std::shared_ptr<HookProcess>& hp) {
        return hp->is_finished();
    });

    std::shared_ptr<bpv2::process> proc;
    try
    {
        proc = std::make_shared<bpv2::process>(
            io_, cmd, std::move(args),
            bpv2::process_stdio{.in = nullptr, .out = nullptr, .err = nullptr},
            ProcessGroupSetup{});
    }
    catch (const std::exception& e)
    {
        std::cerr << "Failed to execute " << what << ": " << e.what() << '\n';
        done(false);
        return;
    }

    std::shared_ptr<HookProcess> hp =
        std::make_shared<HookProcess>(io_, std::move(what), std::move(done));
    active_.push_back(hp);
    hp->start(std::move(proc), timeout);
}

std::unique_ptr<HookRunner> makeHookRunner(boost::asio::io_context& io)
{
    return std::make_unique<HookRunnerImpl>(io);
}
