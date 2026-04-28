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
#include <archive.h>
#include <archive_entry.h>
#include <fcntl.h>
#include <sys/stat.h>

#include <ist_app.hpp>

#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

inline constexpr std::string_view k_archive_name = "ist_results.tar.gz";
static constexpr size_t k_archive_buf_size = 65536;
static constexpr off_t k_max_result_size = 50LL * 1024 * 1024;

static bool add_file_to_archive(struct archive* ar, const fs::path& file_path,
                                const fs::path& entry_name)
{
    struct stat st;
    if (::stat(file_path.c_str(), &st) != 0)
    {
        std::cerr << "IST: stat failed for " << file_path << ": " << errno
                  << '\n';
        return false;
    }

    struct archive_entry* entry = archive_entry_new();
    archive_entry_set_pathname(entry, entry_name.c_str());
    archive_entry_copy_stat(entry, &st);
    archive_entry_set_filetype(entry, AE_IFREG);
    archive_entry_set_perm(entry, st.st_mode & 0777);

    if (archive_write_header(ar, entry) != ARCHIVE_OK)
    {
        std::cerr << "IST: archive_write_header failed: "
                  << archive_error_string(ar) << '\n';
        archive_entry_free(entry);
        return false;
    }

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    int fd = ::open(file_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
    {
        std::cerr << "IST: open failed for " << file_path << ": " << errno
                  << '\n';
        archive_entry_free(entry);
        return false;
    }

    std::array<char, k_archive_buf_size> buf{};
    bool ok = true;
    while (true)
    {
        ssize_t nread = ::read(fd, buf.data(), buf.size());
        if (nread < 0)
        {
            std::cerr << "IST: read failed for " << file_path << ": " << errno
                      << '\n';
            ok = false;
            break;
        }
        if (nread == 0)
        {
            break;
        }
        if (archive_write_data(ar, buf.data(), static_cast<size_t>(nread)) < 0)
        {
            std::cerr << "IST: archive_write_data failed: "
                      << archive_error_string(ar) << '\n';
            ok = false;
            break;
        }
    }

    ::close(fd);
    archive_entry_free(entry);
    return ok;
}

bool archiveResults(const fs::path& results_dir)
{
    fs::path archive_path = results_dir / k_archive_name;
    std::error_code ec;

    // Collect files first so we don't iterate while modifying the directory.
    std::vector<fs::path> files;
    for (const fs::directory_entry& entry :
         fs::directory_iterator(results_dir, ec))
    {
        if (entry.path().filename() == k_archive_name)
        {
            continue;
        }
        if (!entry.is_regular_file(ec))
        {
            continue;
        }
        files.push_back(entry.path());
    }

    if (files.empty())
    {
        std::cerr << "IST: no result files to archive in " << results_dir
                  << '\n';
        return false;
    }

    struct archive* ar = archive_write_new();
    archive_write_add_filter_gzip(ar);
    archive_write_set_format_pax_restricted(ar);

    if (archive_write_open_filename(ar, archive_path.c_str()) != ARCHIVE_OK)
    {
        std::cerr << "IST: archive_write_open_filename failed: "
                  << archive_error_string(ar) << '\n';
        archive_write_free(ar);
        return false;
    }

    bool all_ok = true;
    for (const fs::path& file : files)
    {
        if (!add_file_to_archive(ar, file, file.filename()))
        {
            all_ok = false;
            break;
        }
        // Delete each file immediately after archiving to stay within
        // XMC storage limits.
        fs::remove(file, ec);
        if (ec)
        {
            std::cerr << "IST: failed to remove " << file << ": "
                      << ec.message() << '\n';
        }
    }

    if (archive_write_close(ar) != ARCHIVE_OK)
    {
        std::cerr << "IST: archive_write_close failed: "
                  << archive_error_string(ar) << '\n';
        all_ok = false;
    }
    archive_write_free(ar);

    if (!all_ok)
    {
        fs::remove(archive_path, ec);
    }

    return all_ok;
}

sdbusplus::message::unix_fd IstService::getResultsFd()
{
    if (state_.stage != IstStage::idle)
    {
        throw sdbusplus::exception::SdBusError(
            EBUSY, "Cannot retrieve results while IST is in progress");
    }

    const fs::path& results_dir = platformCfg_.storage.resultStoragePath;
    if (results_dir.empty())
    {
        std::cerr << "IST: resultStoragePath not configured\n";
        throw sdbusplus::exception::SdBusError(
            ENOENT, "resultStoragePath not configured");
    }

    fs::path archive_path = results_dir / k_archive_name;

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    int fd = ::open(archive_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
    {
        std::cerr << "IST: no results archive at " << archive_path << ": "
                  << errno << '\n';
        throw sdbusplus::exception::SdBusError(ENOENT,
                                               "No IST results available");
    }

    struct stat st{};
    if (::fstat(fd, &st) < 0)
    {
        int err = errno;
        std::cerr << "IST: fstat failed on results archive: " << err << '\n';
        ::close(fd);
        throw sdbusplus::exception::SdBusError(
            err, "Failed to stat IST results archive");
    }
    if (st.st_size > k_max_result_size)
    {
        std::cerr << "IST: results archive too large (" << st.st_size
                  << " bytes, max " << k_max_result_size << ")\n";
        ::close(fd);
        throw sdbusplus::exception::SdBusError(
            EFBIG, "IST results archive exceeds size limit");
    }

    return {fd};
}
