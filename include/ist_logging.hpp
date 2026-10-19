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
#pragma once

#include <fcntl.h>
#include <unistd.h>

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/system/error_code.hpp>

#include <array>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string_view>
#include <tuple>
#include <utility>

// ----------------
// File-descriptor wrapper
// ----------------

class UniqueFd
{
  public:
    explicit UniqueFd(int fd = -1) noexcept : fd_(fd)
    {}
    ~UniqueFd()
    {
        if (fd_ >= 0)
        {
            ::close(fd_);
        }
    }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& o) noexcept : fd_(o.fd_)
    {
        o.fd_ = -1;
    }
    UniqueFd& operator=(UniqueFd&& o) noexcept
    {
        if (this != &o)
        {
            if (fd_ >= 0)
            {
                ::close(fd_);
            }
            fd_ = o.fd_;
            o.fd_ = -1;
        }
        return *this;
    }
    int get() const noexcept
    {
        return fd_;
    }
    int release() noexcept
    {
        int fd = fd_;
        fd_ = -1;
        return fd;
    }

  private:
    int fd_ = -1;
};

// ----------------
// Run-log writer
// ----------------

// Hook output keeps arriving while the run is being torn down, so write()
// after close() has to be silent rather than an EBADF per line.
class LogFile
{
  public:
    bool open(const std::filesystem::path& path)
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        fd_ = UniqueFd(::open(path.c_str(),
                              O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644));
        if (fd_.get() < 0)
        {
            std::cerr << "Failed to open log file '" << path
                      << "': errno=" << errno << '\n';
            return false;
        }
        return true;
    }

    void write(std::string_view data)
    {
        if (fd_.get() < 0)
        {
            return;
        }
        const char* p = data.data();
        std::size_t remaining = data.size();
        while (remaining > 0)
        {
            ssize_t written = ::write(fd_.get(), p, remaining);
            if (written <= 0)
            {
                std::cerr << "Write to log file failed: errno=" << errno
                          << '\n';
                return;
            }
            p += written;
            remaining -= static_cast<std::size_t>(written);
        }
    }

    void close()
    {
        fd_ = UniqueFd();
    }

    bool is_open() const noexcept
    {
        return fd_.get() >= 0;
    }

    // For TeeStreambuf, which writes to the same file until the lg2 migration
    // retires it.
    int fd() const noexcept
    {
        return fd_.get();
    }

  private:
    UniqueFd fd_;
};

// The log files a single run writes, opened after the results directory is
// wiped and closed once the archive has been taken.
struct RunLogs
{
    LogFile service;
};

// ----------------
// Child output pump
// ----------------

// A page. Output is forwarded verbatim, so this only bounds how much one read
// can carry; it does not split or truncate the child's lines.
static constexpr std::size_t k_output_buf_size = 4096;

// Reads from a pipe fd asynchronously and writes each chunk to both a log file
// and stdout (which journald captures). Chunks go out as they arrive with no
// line assembly: a read can land mid-line, and if a child wedges mid-line
// those bytes are the diagnostic worth having.
class OutputTee final : public std::enable_shared_from_this<OutputTee>
{
  public:
    OutputTee(boost::asio::io_context& io, int pipe_read_fd, LogFile& log) :
        stream_(io, pipe_read_fd), log_(log)
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
                self->log_.write(std::string_view(self->buf_.data(), n));
                std::ignore = ::write(STDOUT_FILENO, self->buf_.data(), n);
                self->read_next();
            });
    }

    boost::asio::posix::stream_descriptor stream_;
    LogFile& log_;
    std::array<char, k_output_buf_size> buf_{};
};
