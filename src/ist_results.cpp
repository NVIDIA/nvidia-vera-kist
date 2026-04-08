/*
 * SPDX-FileCopyrightText: Copyright (c) 2024-2026 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
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
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/stdio.hpp>
#include <ist_app.hpp>

#include <cerrno>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace bpv2 = boost::process::v2;
namespace fs = std::filesystem;

static void on_tar_exit(const std::shared_ptr<bpv2::process>& /*proc*/,
                        const boost::system::error_code& ec, int exit_code)
{
    if (ec || exit_code != 0)
    {
        std::cerr << "IST: tar exited: ec=" << ec.message()
                  << " exit=" << exit_code << '\n';
    }
}

sdbusplus::message::unix_fd IstService::getResultsFd()
{
    const fs::path& results_dir = platformCfg_.storage.resultStoragePath;
    if (results_dir.empty())
    {
        std::cerr << "IST: resultStoragePath not configured\n";
        throw sdbusplus::exception::SdBusError(
            ENOENT, "resultStoragePath not configured");
    }

    std::error_code ec;
    bool dir_exists = fs::exists(results_dir, ec);
    if (ec)
    {
        std::cerr << "IST: cannot stat " << results_dir << ": " << ec.message()
                  << '\n';
        throw sdbusplus::exception::SdBusError(EIO, ec.message().c_str());
    }
    if (!dir_exists)
    {
        std::cerr << "IST: no results available in " << results_dir << '\n';
        throw sdbusplus::exception::SdBusError(ENOENT,
                                               "No IST results available");
    }

    bool dir_empty = fs::is_empty(results_dir, ec);
    if (ec)
    {
        std::cerr << "IST: cannot check " << results_dir << ": " << ec.message()
                  << '\n';
        throw sdbusplus::exception::SdBusError(EIO, ec.message().c_str());
    }
    if (dir_empty)
    {
        std::cerr << "IST: no results available in " << results_dir << '\n';
        throw sdbusplus::exception::SdBusError(ENOENT,
                                               "No IST results available");
    }

    int pipe_fds[2];
    if (::pipe(pipe_fds) < 0)
    {
        std::cerr << "IST: pipe() failed: " << errno << '\n';
        throw sdbusplus::exception::SdBusError(errno, "pipe failed");
    }

    UniqueFd read_end(pipe_fds[0]);
    UniqueFd write_end(pipe_fds[1]);

    std::shared_ptr<bpv2::process> proc = std::make_shared<bpv2::process>(
        io_, "/bin/tar",
        std::vector<std::string>{"czf", "-", "-C", results_dir, "."},
        bpv2::process_stdio{
            .in = nullptr, .out = write_end.get(), .err = stderr});

    // tar owns a dup of writeEnd; close ours so bmcweb sees EOF when tar exits.
    write_end = UniqueFd();

    proc->async_wait(
        [proc](const boost::system::error_code& ec, int exit_code) {
            on_tar_exit(proc, ec, exit_code);
        });

    return {read_end.release()};
}
