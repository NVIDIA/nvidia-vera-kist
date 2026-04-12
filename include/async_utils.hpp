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

#include <boost/asio/io_context.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <thread>

/**
 * Run a blocking callable on a detached worker thread and deliver
 * the bool result back to the io_context event loop via a pipe.
 *
 * The work function must be self-contained — it must not reference
 * any state that the io_context thread may concurrently access.
 */
inline void runOffThread(boost::asio::io_context& io,
                         std::move_only_function<bool()> work,
                         std::move_only_function<void(bool) const> done)
{
    int fds[2];
    if (::pipe2(fds, O_CLOEXEC) < 0)
    {
        done(false);
        return;
    }

    auto stream =
        std::make_shared<boost::asio::posix::stream_descriptor>(io, fds[0]);
    std::shared_ptr<uint8_t> result = std::make_shared<uint8_t>(0);

    stream->async_read_some(boost::asio::buffer(result.get(), 1),
                            [stream, result, done = std::move(done)](
                                const boost::system::error_code& ec, size_t) {
                                done(!ec && *result != 0);
                            });

    std::thread([work = std::move(work), write_fd = fds[1]]() mutable {
        bool ok = work();
        uint8_t val = ok ? 1 : 0;
        std::ignore = ::write(write_fd, &val, 1);
        ::close(write_fd);
    }).detach();
}
