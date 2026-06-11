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

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <ist_app.hpp>
#include <ist_errors.hpp>
#include <ist_results.hpp>

#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <vector>

namespace fs = std::filesystem;
namespace ist_err = sdbusplus::error::com::nvidia::vera::ist;

inline constexpr std::string_view k_archive_name = "ist_results.tar.gz";
static constexpr size_t k_archive_buf_size = 65536;
static constexpr off_t k_max_result_size = 50LL * 1024 * 1024;

// Collected up front so the drivers never iterate the directory while
// removing entries from it.
static std::vector<fs::path> collect_result_files(const fs::path& results_dir)
{
    std::vector<fs::path> files;
    std::error_code ec;
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
    return files;
}

// Incremental gzip/pax archive writer, driven one ~64 KB block per pump() so
// it can run either in a tight sync loop or one step per io_context turn. The
// destructor frees the handle and removes a partial archive unless finish()
// succeeded, so dropping the writer early is safe.
class ArchiveWriter
{
  public:
    ArchiveWriter() = default;

    ~ArchiveWriter()
    {
        if (ar_ != nullptr)
        {
            archive_write_free(ar_);
            ar_ = nullptr;
        }
        if (opened_ && !finishedOk_)
        {
            std::error_code ec;
            fs::remove(archivePath_, ec);
        }
    }

    ArchiveWriter(const ArchiveWriter&) = delete;
    ArchiveWriter& operator=(const ArchiveWriter&) = delete;
    ArchiveWriter(ArchiveWriter&&) = delete;
    ArchiveWriter& operator=(ArchiveWriter&&) = delete;

    bool open(const fs::path& archive_path)
    {
        archivePath_ = archive_path;
        ar_ = archive_write_new();
        archive_write_add_filter_gzip(ar_);
        archive_write_set_format_pax_restricted(ar_);

        if (archive_write_open_filename(ar_, archivePath_.c_str()) !=
            ARCHIVE_OK)
        {
            std::cerr << "IST: archive_write_open_filename failed: "
                      << archive_error_string(ar_) << '\n';
            archive_write_free(ar_);
            ar_ = nullptr;
            return false;
        }
        opened_ = true;
        return true;
    }

    bool begin_entry(const fs::path& file)
    {
        struct stat st;
        if (::stat(file.c_str(), &st) != 0)
        {
            std::cerr << "IST: stat failed for " << file << ": " << errno
                      << '\n';
            failed_ = true;
            return false;
        }

        struct archive_entry* entry = archive_entry_new();
        archive_entry_set_pathname(entry, file.filename().c_str());
        archive_entry_copy_stat(entry, &st);
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, st.st_mode & 0777);

        int rc = archive_write_header(ar_, entry);
        archive_entry_free(entry);
        if (rc != ARCHIVE_OK)
        {
            std::cerr << "IST: archive_write_header failed: "
                      << archive_error_string(ar_) << '\n';
            failed_ = true;
            return false;
        }

        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        int fd = ::open(file.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0)
        {
            std::cerr << "IST: open failed for " << file << ": " << errno
                      << '\n';
            failed_ = true;
            return false;
        }
        fd_ = UniqueFd(fd);
        return true;
    }

    enum class Pump
    {
        More,
        Done,
        Error,
    };

    Pump pump()
    {
        ssize_t nread = ::read(fd_.get(), buf_.data(), buf_.size());
        if (nread < 0)
        {
            std::cerr << "IST: read failed: " << errno << '\n';
            failed_ = true;
            return Pump::Error;
        }
        if (nread == 0)
        {
            fd_ = UniqueFd();
            return Pump::Done;
        }
        if (archive_write_data(ar_, buf_.data(), static_cast<size_t>(nread)) <
            0)
        {
            std::cerr << "IST: archive_write_data failed: "
                      << archive_error_string(ar_) << '\n';
            failed_ = true;
            return Pump::Error;
        }
        return Pump::More;
    }

    bool finish()
    {
        bool close_ok = true;
        if (ar_ != nullptr)
        {
            if (archive_write_close(ar_) != ARCHIVE_OK)
            {
                std::cerr << "IST: archive_write_close failed: "
                          << archive_error_string(ar_) << '\n';
                close_ok = false;
            }
            archive_write_free(ar_);
            ar_ = nullptr;
        }

        bool ok = close_ok && !failed_;
        if (opened_ && !ok)
        {
            std::error_code ec;
            fs::remove(archivePath_, ec);
        }
        finishedOk_ = ok;
        return ok;
    }

  private:
    struct archive* ar_ = nullptr;
    UniqueFd fd_;
    fs::path archivePath_;
    bool opened_ = false;
    bool failed_ = false;
    bool finishedOk_ = false;
    std::array<char, k_archive_buf_size> buf_{};
};

bool archiveResults(const fs::path& results_dir)
{
    std::vector<fs::path> files = collect_result_files(results_dir);
    if (files.empty())
    {
        std::cerr << "IST: no result files to archive in " << results_dir
                  << '\n';
        return false;
    }

    ArchiveWriter writer;
    if (!writer.open(results_dir / k_archive_name))
    {
        return false;
    }

    for (const fs::path& file : files)
    {
        if (!writer.begin_entry(file))
        {
            break;
        }
        ArchiveWriter::Pump p = ArchiveWriter::Pump::More;
        while ((p = writer.pump()) == ArchiveWriter::Pump::More)
        {
        }
        if (p == ArchiveWriter::Pump::Error)
        {
            break;
        }
        // Delete each file as it is archived to stay within XMC storage limits.
        std::error_code ec;
        fs::remove(file, ec);
        if (ec)
        {
            std::cerr << "IST: failed to remove " << file << ": "
                      << ec.message() << '\n';
        }
    }

    return writer.finish();
}

// Kept alive by the strong `self` capture in the post chain; self-destructs
// once finish() fires the callback, so no IstService member is needed.
class ArchiveSession : public std::enable_shared_from_this<ArchiveSession>
{
  public:
    ArchiveSession(boost::asio::io_context& io, fs::path results_dir,
                   std::move_only_function<void(bool ok) const> on_complete) :
        io_(io), resultsDir_(std::move(results_dir)),
        onComplete_(std::move(on_complete))
    {}

    ArchiveSession(const ArchiveSession&) = delete;
    ArchiveSession& operator=(const ArchiveSession&) = delete;

    void start()
    {
        post_step();
    }

  private:
    // post() re-arms step() on the next io_context turn: deferred execution,
    // not recursion.
    // NOLINTNEXTLINE(misc-no-recursion)
    void post_step()
    {
        // NOLINTNEXTLINE(misc-no-recursion)
        boost::asio::post(io_, [self = shared_from_this()]() { self->step(); });
    }

    // NOLINTNEXTLINE(misc-no-recursion)
    void step()
    {
        if (finished_)
        {
            return;
        }

        // Collect the file list and open the archive on the first turn so
        // completion is always delivered asynchronously, never inline from
        // start().
        if (!started_)
        {
            files_ = collect_result_files(resultsDir_);
            if (files_.empty())
            {
                std::cerr << "IST: no result files to archive in "
                          << resultsDir_ << '\n';
                finish(false);
                return;
            }
            if (!writer_.open(resultsDir_ / k_archive_name))
            {
                finish(false);
                return;
            }
            started_ = true;
            post_step();
            return;
        }

        if (!entryOpen_)
        {
            if (index_ >= files_.size())
            {
                finish(true);
                return;
            }
            if (!writer_.begin_entry(files_[index_]))
            {
                finish(false);
                return;
            }
            entryOpen_ = true;
            post_step();
            return;
        }

        ArchiveWriter::Pump p = writer_.pump();
        if (p == ArchiveWriter::Pump::Error)
        {
            finish(false);
            return;
        }
        if (p == ArchiveWriter::Pump::More)
        {
            post_step();
            return;
        }

        // Delete each file as it is archived to stay within XMC storage limits.
        std::error_code ec;
        fs::remove(files_[index_], ec);
        if (ec)
        {
            std::cerr << "IST: failed to remove " << files_[index_] << ": "
                      << ec.message() << '\n';
        }
        ++index_;
        entryOpen_ = false;
        post_step();
    }

    void finish(bool ok)
    {
        if (finished_)
        {
            return;
        }
        finished_ = true;
        bool writer_ok = writer_.finish();
        auto cb = std::move(onComplete_);
        if (cb)
        {
            cb(ok && writer_ok);
        }
    }

    boost::asio::io_context& io_;
    fs::path resultsDir_;
    std::move_only_function<void(bool ok) const> onComplete_;
    ArchiveWriter writer_;
    std::vector<fs::path> files_;
    size_t index_ = 0;
    bool started_ = false;
    bool entryOpen_ = false;
    bool finished_ = false;
};

void archiveResultsAsync(
    boost::asio::io_context& io, fs::path results_dir,
    std::move_only_function<void(bool ok) const> on_complete)
{
    std::make_shared<ArchiveSession>(io, std::move(results_dir),
                                     std::move(on_complete))
        ->start();
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
        throw ist_err::InternalFailure{};
    }

    fs::path archive_path = results_dir / k_archive_name;

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    int fd = ::open(archive_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
    {
        std::cerr << "IST: no results archive at " << archive_path << ": "
                  << errno << '\n';
        throw ist_err::ResourceNotFound{};
    }

    struct stat st{};
    if (::fstat(fd, &st) < 0)
    {
        int err = errno;
        std::cerr << "IST: fstat failed on results archive: " << err << '\n';
        ::close(fd);
        throw ist_err::InternalFailure{};
    }
    if (st.st_size > k_max_result_size)
    {
        std::cerr << "IST: results archive too large (" << st.st_size
                  << " bytes, max " << k_max_result_size << ")\n";
        ::close(fd);
        throw ist_err::InternalFailure{};
    }

    return {fd};
}
