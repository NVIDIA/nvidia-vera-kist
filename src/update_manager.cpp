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
#include <linux/loop.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <unistd.h>

#include <async_utils.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/asio/steady_timer.hpp>
#include <ist_app.hpp>
#include <sdbusplus/exception.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

static constexpr std::string_view k_image_file_name = "CPU-IST.img";
static constexpr size_t k_transfer_buf_size = 65536;

// ----------------------------------------------------------------
// PLDM package helpers
// ----------------------------------------------------------------

static uint16_t read_u16_le(const uint8_t* p)
{
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

static uint32_t read_u32_le(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

static uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t len)
{
    for (size_t i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
        {
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
        }
    }
    return crc;
}

static uint32_t compute_crc32(const uint8_t* data, size_t len)
{
    return ~crc32_update(0xFFFFFFFF, data, len);
}

static constexpr size_t k_pldm_min_header_size = 36;

static bool parse_pldm_header(const std::vector<uint8_t>& hdr, size_t file_size,
                              PldmComponentInfo& comp)
{
    if (hdr.size() < k_pldm_min_header_size)
    {
        std::cerr << "PLDM header too small\n";
        return false;
    }

    uint8_t revision = hdr[16];
    uint16_t header_size = read_u16_le(&hdr[17]);

    // Per DSP0267 v1.3.0 Table 8: revision 0x04 is the first format
    // that includes PackagePayloadChecksum.
    if (revision < 4)
    {
        std::cerr << "PLDM revision 0x" << std::hex
                  << static_cast<unsigned>(revision) << std::dec
                  << " does not include payload checksum; rejecting\n";
        return false;
    }

    // The header_size < min check overlaps with the trailer check below,
    // but gives a clearer error when the header size itself is nonsense.
    if (hdr.size() < header_size || header_size < k_pldm_min_header_size)
    {
        std::cerr << "PLDM header size invalid: " << header_size << '\n';
        return false;
    }

    // Revision >= 4 trailer: [header CRC (4)] [payload CRC (4)]
    constexpr size_t trailer_size = 8;
    if (header_size < k_pldm_min_header_size + trailer_size)
    {
        std::cerr << "PLDM header too small for trailer\n";
        return false;
    }

    size_t crc_offset = header_size - trailer_size;
    uint32_t stored_crc = read_u32_le(&hdr[crc_offset]);
    uint32_t computed_crc = compute_crc32(hdr.data(), crc_offset);
    if (stored_crc != computed_crc)
    {
        std::cerr << "PLDM header CRC mismatch: stored 0x" << std::hex
                  << stored_crc << " computed 0x" << computed_crc << std::dec
                  << '\n';
        return false;
    }

    comp.payloadCrc = read_u32_le(&hdr[header_size - 4]);

    std::cerr << "PLDM revision 0x" << std::hex
              << static_cast<unsigned>(revision) << std::dec << ", header size "
              << header_size << '\n';

    comp.offset = header_size;
    comp.size = static_cast<uint32_t>(file_size - header_size);
    return true;
}

static std::optional<PldmComponentInfo>
    process_pldm_package(const fs::path& file_path)
{
    std::ifstream file(file_path, std::ios::binary | std::ios::ate);
    if (!file)
    {
        std::cerr << "Cannot open PLDM package: " << file_path << '\n';
        return std::nullopt;
    }

    size_t file_size = static_cast<size_t>(file.tellg());
    if (file_size < k_pldm_min_header_size)
    {
        std::cerr << "File too small to be a PLDM package\n";
        return std::nullopt;
    }

    // Read the first 19 bytes to extract the header size field at offset 17.
    file.seekg(0);
    std::array<uint8_t, 19> prefix{};
    file.read(reinterpret_cast<char*>(prefix.data()), prefix.size());
    if (!file)
    {
        std::cerr << "Failed to read PLDM header prefix\n";
        return std::nullopt;
    }

    uint16_t header_size = read_u16_le(&prefix[17]);
    if (header_size < k_pldm_min_header_size || header_size > file_size)
    {
        std::cerr << "Invalid PLDM header size: " << header_size << '\n';
        return std::nullopt;
    }

    std::vector<uint8_t> header(header_size);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(header.data()), header_size);
    if (!file)
    {
        std::cerr << "Failed to read full PLDM header\n";
        return std::nullopt;
    }
    file.close();

    PldmComponentInfo comp{};
    if (!parse_pldm_header(header, file_size, comp))
    {
        return std::nullopt;
    }

    std::cerr << "PLDM header verified (CRC OK). Component at offset "
              << comp.offset << ", size " << comp.size << '\n';
    return comp;
}

// ----------------------------------------------------------------
// Loop-device / mount teardown helpers
// ----------------------------------------------------------------

struct LoopDevice
{
    std::string path;
    UniqueFd fd;
};

// Allocate a free loop device, attach `image_path` as its backing file,
// and return the device path + fd.  The caller must keep the fd alive
// until after mount() so that LO_FLAGS_AUTOCLEAR does not fire early.
static std::optional<LoopDevice> setup_loop_device(const fs::path& image_path,
                                                   int open_flags)
{
    UniqueFd ctl_fd(::open("/dev/loop-control", O_RDWR | O_CLOEXEC));
    if (ctl_fd.get() < 0)
    {
        std::cerr << "open loop-control: " << errno << '\n';
        return std::nullopt;
    }
    int loop_num = ::ioctl(ctl_fd.get(), LOOP_CTL_GET_FREE);
    if (loop_num < 0)
    {
        std::cerr << "LOOP_CTL_GET_FREE: " << errno << '\n';
        return std::nullopt;
    }

    std::string loop_dev = "/dev/loop" + std::to_string(loop_num);

    UniqueFd loop_fd(::open(loop_dev.c_str(), open_flags | O_CLOEXEC));
    if (loop_fd.get() < 0)
    {
        std::cerr << "open " << loop_dev << ": " << errno << '\n';
        return std::nullopt;
    }
    UniqueFd img_fd(::open(image_path.c_str(), open_flags | O_CLOEXEC));
    if (img_fd.get() < 0)
    {
        std::cerr << "open " << image_path << ": " << errno << '\n';
        return std::nullopt;
    }
    if (::ioctl(loop_fd.get(), LOOP_SET_FD, img_fd.get()) < 0)
    {
        std::cerr << "LOOP_SET_FD for " << image_path << ": " << errno << '\n';
        return std::nullopt;
    }

    struct loop_info64 info = {};
    info.lo_flags = LO_FLAGS_AUTOCLEAR;
    if (::ioctl(loop_fd.get(), LOOP_SET_STATUS64, &info) < 0)
    {
        std::cerr << "LOOP_SET_STATUS64 for " << loop_dev << ": " << errno
                  << '\n';
        ::ioctl(loop_fd.get(), LOOP_CLR_FD);
        return std::nullopt;
    }

    return LoopDevice{std::move(loop_dev), std::move(loop_fd)};
}

// ----------------------------------------------------------------
// TransferSession — async image transfer via socketpair
//
// Owns: stream_descriptor (read-end FD), ofstream, deadline timer.
// On destruction, removes the image file unless committed.
// ----------------------------------------------------------------

class TransferSession : public std::enable_shared_from_this<TransferSession>
{
  public:
    TransferSession(boost::asio::io_context& io, UniqueFd read_fd,
                    fs::path image_path, std::chrono::seconds timeout,
                    std::move_only_function<void(bool ok) const> on_complete) :
        stream_(io, read_fd.release()), deadline_(io),
        imagePath_(std::move(image_path)), timeout_(timeout),
        onComplete_(std::move(on_complete))
    {
        output_.open(imagePath_, std::ios::binary | std::ios::trunc);
        if (!output_)
        {
            throw std::system_error(errno, std::generic_category(),
                                    "Failed to open " + imagePath_.string());
        }
    }

    ~TransferSession()
    {
        output_.close();
        if (!committed_)
        {
            std::error_code ec;
            fs::remove(imagePath_, ec);
        }
    }

    TransferSession(const TransferSession&) = delete;
    TransferSession& operator=(const TransferSession&) = delete;

    void start()
    {
        reset_deadline();
        read_next();
    }

  private:
    void reset_deadline()
    {
        deadline_.expires_after(timeout_);
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
    }

    void read_next()
    {
        stream_.async_read_some(
            boost::asio::buffer(buf_),
            [weak = weak_from_this()](const boost::system::error_code& ec,
                                      size_t n) {
                if (auto self = weak.lock())
                {
                    self->on_read(ec, n);
                }
            });
    }

    void on_read(const boost::system::error_code& ec, size_t n)
    {
        if (finished_)
        {
            return;
        }

        if (ec)
        {
            if (bytesReceived_ == 0)
            {
                std::cerr << "Transfer aborted: received 0 bytes\n";
                finish(false);
            }
            else
            {
                std::cout << "Image transfer complete: " << bytesReceived_
                          << " bytes\n";
                committed_ = true;
                finish(true);
            }
            return;
        }

        output_.write(buf_.data(), static_cast<std::streamsize>(n));
        if (!output_)
        {
            std::cerr << "Write to image file failed\n";
            finish(false);
            return;
        }

        bytesReceived_ += n;
        reset_deadline();
        read_next();
    }

    void on_timeout()
    {
        if (finished_)
        {
            return;
        }
        std::cerr << "Transfer timed out after " << timeout_.count()
                  << "s of inactivity (" << bytesReceived_
                  << " bytes received)\n";
        stream_.close();
        finish(false);
    }

    void finish(bool ok)
    {
        if (finished_)
        {
            return;
        }
        finished_ = true;
        deadline_.cancel();
        output_.close();
        auto cb = std::move(onComplete_);
        if (cb)
        {
            cb(ok);
        }
    }

    boost::asio::posix::stream_descriptor stream_;
    boost::asio::steady_timer deadline_;
    std::ofstream output_;
    fs::path imagePath_;
    std::chrono::seconds timeout_;
    std::move_only_function<void(bool ok) const> onComplete_;
    std::array<char, k_transfer_buf_size> buf_{};
    size_t bytesReceived_{0};
    bool finished_{false};
    bool committed_{false};
};

// ----------------------------------------------------------------
// PLDM header stripping — runs entirely on a worker thread
// ----------------------------------------------------------------

static bool strip_pldm_payload(int fd, const PldmComponentInfo& comp)
{
    uint32_t remaining = comp.size;
    off_t read_pos = comp.offset;
    off_t write_pos = 0;
    uint32_t crc_state = 0xFFFFFFFF;
    std::array<char, k_transfer_buf_size> buf{};

    while (remaining > 0)
    {
        size_t to_read =
            std::min(static_cast<size_t>(remaining), k_transfer_buf_size);
        ssize_t nread = ::pread(fd, buf.data(), to_read, read_pos);
        if (nread < 0)
        {
            std::cerr << "pread failed during PLDM stripping: " << errno
                      << '\n';
            return false;
        }
        if (nread == 0)
        {
            std::cerr << "Unexpected EOF during PLDM stripping"
                         " (file truncated?)\n";
            return false;
        }

        crc_state = crc32_update(crc_state,
                                 reinterpret_cast<const uint8_t*>(buf.data()),
                                 static_cast<size_t>(nread));

        ssize_t nwritten =
            ::pwrite(fd, buf.data(), static_cast<size_t>(nread), write_pos);
        if (nwritten != nread)
        {
            std::cerr << "pwrite failed during PLDM stripping: " << errno
                      << '\n';
            return false;
        }

        read_pos += nread;
        write_pos += nread;
        remaining -= static_cast<uint32_t>(nread);
    }

    uint32_t computed = ~crc_state;
    if (computed != comp.payloadCrc)
    {
        std::cerr << "PLDM payload CRC mismatch: stored 0x" << std::hex
                  << comp.payloadCrc << " computed 0x" << computed << std::dec
                  << '\n';
        return false;
    }
    std::cerr << "PLDM payload CRC verified OK\n";

    if (::ftruncate(fd, static_cast<off_t>(comp.size)) != 0)
    {
        std::cerr << "ftruncate failed: " << errno << '\n';
        return false;
    }

    std::cerr << "PLDM header stripped, image ready (" << comp.size
              << " bytes)\n";
    return true;
}

class PldmStripper : public std::enable_shared_from_this<PldmStripper>
{
  public:
    PldmStripper(boost::asio::io_context& io, fs::path image_path,
                 PldmComponentInfo comp,
                 std::move_only_function<void(bool ok) const> on_complete) :
        io_(io), imagePath_(std::move(image_path)), comp_(comp),
        onComplete_(std::move(on_complete)),
        fd_(::open(imagePath_.c_str(), O_RDWR | O_CLOEXEC))
    {
        if (fd_.get() < 0)
        {
            throw std::system_error(errno, std::generic_category(),
                                    "Failed to open " + imagePath_.string());
        }
    }

    ~PldmStripper()
    {
        if (!committed_)
        {
            std::error_code ec;
            fs::remove(imagePath_, ec);
        }
    }

    PldmStripper(const PldmStripper&) = delete;
    PldmStripper& operator=(const PldmStripper&) = delete;

    void start()
    {
        std::shared_ptr<UniqueFd> fd_ptr =
            std::make_shared<UniqueFd>(std::move(fd_));
        PldmComponentInfo comp = comp_;

        // shared_from_this is intentional: the callback must set
        // committed_ before the destructor runs, otherwise it would
        // delete the image file.
        runOffThread(
            io_,
            [fd_ptr, comp]() {
                return strip_pldm_payload(fd_ptr->get(), comp);
            },
            [self = shared_from_this()](bool ok) {
                if (ok)
                {
                    self->committed_ = true;
                }
                if (self->onComplete_)
                {
                    self->onComplete_(ok);
                }
            });
    }

  private:
    boost::asio::io_context& io_;
    fs::path imagePath_;
    PldmComponentInfo comp_;
    std::move_only_function<void(bool ok) const> onComplete_;
    UniqueFd fd_;
    bool committed_{false};
};

// ----------------------------------------------------------------
// IstService::startUpdate
// ----------------------------------------------------------------

sdbusplus::message::unix_fd IstService::startUpdate()
{
    if (!initialized_)
    {
        throw sdbusplus::exception::SdBusError(EINVAL, "Not initialized");
    }
    if (state_.stage != IstStage::idle)
    {
        throw sdbusplus::exception::SdBusError(
            EBUSY, "Cannot update while IST is running");
    }
    if (updateInProgress_)
    {
        throw sdbusplus::exception::SdBusError(EBUSY,
                                               "Update already in progress");
    }
    if (platformCfg_.storage.vectorStoragePath.empty())
    {
        throw sdbusplus::exception::SdBusError(
            EINVAL, "vectorStoragePath not configured");
    }

    const fs::path& storage_path = platformCfg_.storage.vectorStoragePath;
    fs::path image_path = storage_path / k_image_file_name;

    int fds[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) != 0)
    {
        throw sdbusplus::exception::SdBusError(errno, "socketpair failed");
    }
    std::shared_ptr<UniqueFd> read_fd = std::make_shared<UniqueFd>(fds[0]);
    UniqueFd write_fd(fds[1]);

    updateInProgress_ = true;

    // Teardown runs on a worker thread so it does not block the event
    // loop.  The write_fd is returned to bmcweb immediately; any data it
    // writes goes into the socketpair buffer until we start the
    // TransferSession.  On teardown failure we close the read end so
    // bmcweb gets EPIPE on its next write.
    asyncTeardownMounts(
        [weak = weak_from_this(), read_fd, image_path](bool ok) {
            if (auto self = weak.lock())
            {
                self->onTeardownComplete(ok, read_fd, image_path);
            }
        });

    return {write_fd.release()};
}

void IstService::onTransferComplete(bool ok, const fs::path& image_path)
{
    activeTransfer_.reset();
    if (!ok)
    {
        std::cerr << "Image transfer failed\n";
        finishUpdate(false);
        return;
    }

    std::cout << "Image transfer succeeded, processing PLDM package\n";

    std::shared_ptr<std::optional<PldmComponentInfo>> comp =
        std::make_shared<std::optional<PldmComponentInfo>>();
    runOffThread(
        io_,
        [comp, image_path]() {
            *comp = process_pldm_package(image_path);
            return comp->has_value();
        },
        [weak = weak_from_this(), comp, image_path](bool ok) {
            if (auto self = weak.lock())
            {
                self->onPldmParseComplete(ok, comp, image_path);
            }
        });
}

void IstService::onPldmParseComplete(
    bool ok, const std::shared_ptr<std::optional<PldmComponentInfo>>& comp,
    const fs::path& image_path)
{
    if (!ok)
    {
        std::cerr << "PLDM processing failed, removing image\n";
        std::error_code ec;
        fs::remove(image_path, ec);
        finishUpdate(false);
        return;
    }
    try
    {
        std::shared_ptr<PldmStripper> stripper = std::make_shared<PldmStripper>(
            io_, image_path, **comp, [weak = weak_from_this()](bool strip_ok) {
                if (auto self = weak.lock())
                {
                    self->onStripComplete(strip_ok);
                }
            });
        activeStripper_ = stripper;
        stripper->start();
    }
    catch (const std::system_error& e)
    {
        std::cerr << "Failed to start PLDM strip: " << e.what() << '\n';
        std::error_code ec;
        fs::remove(image_path, ec);
        finishUpdate(false);
    }
}

void IstService::onStripComplete(bool ok)
{
    activeStripper_.reset();
    if (!ok)
    {
        std::cerr << "PLDM strip failed\n";
        finishUpdate(false);
        return;
    }

    std::cout << "PLDM strip succeeded\n";
    publisher_->publishActivationProgress(100);
    publisher_->removeActivationProgress();

    asyncMountImages([weak = weak_from_this()](bool mount_ok) {
        if (auto self = weak.lock())
        {
            self->onMountComplete(mount_ok);
        }
    });
}

bool IstService::teardownMounts()
{
    const fs::path& mount_path = platformCfg_.storage.vectorMountPath;

    if (mount_path.empty())
    {
        return true;
    }

    bool ok = true;
    fs::path golden_mount = mount_path / "GOLDEN_RES";
    if (umount2(golden_mount.c_str(), 0) != 0 && errno != EINVAL &&
        errno != ENOENT && errno != EPERM)
    {
        std::cerr << "Failed to unmount " << golden_mount << ": " << errno
                  << '\n';
        ok = false;
    }

    if (umount2(mount_path.c_str(), 0) != 0 && errno != EINVAL &&
        errno != ENOENT && errno != EPERM)
    {
        std::cerr << "Failed to unmount " << mount_path << ": " << errno
                  << '\n';
        ok = false;
    }

    return ok;
}

bool IstService::mountImages()
{
    const fs::path& storage_path = platformCfg_.storage.vectorStoragePath;
    const fs::path& mount_path = platformCfg_.storage.vectorMountPath;

    if (storage_path.empty() || mount_path.empty())
    {
        std::cerr << "vectorStoragePath or vectorMountPath not configured\n";
        return false;
    }

    if (!teardownMounts())
    {
        std::cerr << "Cannot mount: failed to unmount existing images\n";
        return false;
    }

    fs::path image_path = storage_path / k_image_file_name;
    std::error_code ec;
    bool exists = fs::exists(image_path, ec);
    if (ec)
    {
        std::cerr << "Failed to stat " << image_path << ": " << ec.message()
                  << '\n';
        return false;
    }
    if (!exists)
    {
        std::cout << "No IST image at " << image_path << "; nothing to mount\n";
        return false;
    }

    fs::create_directories(mount_path, ec);
    if (ec)
    {
        std::cerr << "Failed to create " << mount_path << ": " << ec.message()
                  << '\n';
        return false;
    }

    std::optional<LoopDevice> ext4_loop =
        setup_loop_device(image_path, O_RDONLY);
    if (!ext4_loop)
    {
        return false;
    }

    if (::mount(ext4_loop->path.c_str(), mount_path.c_str(), "ext4",
                MS_RDONLY | MS_NOATIME, nullptr) != 0)
    {
        std::cerr << "mount ext4 at " << mount_path << ": " << errno << '\n';
        return false;
    }
    std::cout << "Mounted ext4 image at " << mount_path << '\n';

    auto undo_ext4 = [&mount_path]() { ::umount2(mount_path.c_str(), 0); };

    fs::path sqsh_path = mount_path / "GOLDEN_RES.sqfs";
    exists = fs::exists(sqsh_path, ec);
    if (ec)
    {
        std::cerr << "Failed to stat " << sqsh_path << ": " << ec.message()
                  << '\n';
        undo_ext4();
        return false;
    }
    if (!exists)
    {
        std::cerr << "Golden results image not found: " << sqsh_path << '\n';
        undo_ext4();
        return false;
    }

    fs::path golden_mount = mount_path / "GOLDEN_RES";
    fs::create_directories(golden_mount, ec);
    if (ec)
    {
        std::cerr << "Failed to create " << golden_mount << ": " << ec.message()
                  << '\n';
        undo_ext4();
        return false;
    }

    std::optional<LoopDevice> sqsh_loop =
        setup_loop_device(sqsh_path, O_RDONLY);
    if (!sqsh_loop)
    {
        std::cerr << "Failed to set up loop device for squashfs\n";
        undo_ext4();
        return false;
    }

    if (::mount(sqsh_loop->path.c_str(), golden_mount.c_str(), "squashfs",
                MS_RDONLY, nullptr) != 0)
    {
        std::cerr << "mount squashfs at " << golden_mount << ": " << errno
                  << '\n';
        undo_ext4();
        return false;
    }
    std::cout << "Mounted golden results at " << golden_mount << '\n';

    return true;
}

void IstService::readAndPublishVersion()
{
    const fs::path& mount_path = platformCfg_.storage.vectorMountPath;
    if (mount_path.empty())
    {
        std::cerr << "vectorMountPath not configured; skipping version read\n";
        return;
    }

    fs::path version_file = mount_path / "version.txt";
    std::ifstream f(version_file);
    if (!f.is_open())
    {
        std::cout << "No version.txt at " << version_file
                  << "; version unavailable\n";
        return;
    }

    std::string version;
    if (!std::getline(f, version))
    {
        std::cerr << "Failed to read " << version_file << '\n';
        return;
    }

    boost::algorithm::trim(version);

    constexpr size_t k_max_version_length = 256;
    if (version.size() > k_max_version_length)
    {
        std::cerr << "Version string too long (" << version.size()
                  << " chars), truncating to " << k_max_version_length << '\n';
        version.resize(k_max_version_length);
    }

    if (version.empty())
    {
        std::cerr << "version.txt at " << version_file << " is empty\n";
        return;
    }

    publisher_->publishVersion(version);
    std::cout << "IST vector version: " << version << '\n';
}

void IstService::finishUpdate(bool ok)
{
    publisher_->publishActivation(ok ? k_activation_active
                                     : k_activation_failed);
    publisher_->removeActivationProgress();
    updateInProgress_ = false;
}

void IstService::onTeardownComplete(bool ok,
                                    const std::shared_ptr<UniqueFd>& read_fd,
                                    const fs::path& image_path)
{
    if (!ok)
    {
        std::cerr << "Failed to unmount existing images; rejecting update\n";
        publisher_->publishActivation(k_activation_failed);
        int fd = read_fd->release();
        if (fd >= 0)
        {
            ::close(fd);
        }
        updateInProgress_ = false;
        return;
    }

    try
    {
        std::shared_ptr<TransferSession> session =
            std::make_shared<TransferSession>(
                io_, UniqueFd(read_fd->release()), image_path,
                platformCfg_.transferInactivityTimeout,
                [weak = weak_from_this(), image_path](bool xfer_ok) {
                    if (auto self = weak.lock())
                    {
                        self->onTransferComplete(xfer_ok, image_path);
                    }
                });

        publisher_->publishActivation(k_activation_activating);
        publisher_->createActivationProgress();
        publisher_->publishActivationProgress(0);

        activeTransfer_ = session;
        session->start();
    }
    catch (const std::system_error& e)
    {
        std::cerr << "Failed to start transfer: " << e.what() << '\n';
        publisher_->publishActivation(k_activation_failed);
        updateInProgress_ = false;
    }
}

void IstService::onMountComplete(bool ok)
{
    if (!ok)
    {
        std::cerr << "Failed to mount images after PLDM strip\n";
        finishUpdate(false);
        return;
    }
    publisher_->publishActivation(k_activation_active);
    readAndPublishVersion();
    updateInProgress_ = false;
}

void IstService::asyncTeardownMounts(
    std::move_only_function<void(bool) const> done)
{
    auto self = shared_from_this();
    runOffThread(
        io_, [self]() { return self->teardownMounts(); }, std::move(done));
}

void IstService::asyncMountImages(
    std::move_only_function<void(bool) const> done)
{
    auto self = shared_from_this();
    runOffThread(
        io_, [self]() { return self->mountImages(); }, std::move(done));
}

void IstService::ensureMounted()
{
    const fs::path& mount_path = platformCfg_.storage.vectorMountPath;
    if (mount_path.empty())
    {
        return;
    }

    fs::path golden_mount = mount_path / "GOLDEN_RES";
    std::error_code ec;
    if (fs::exists(golden_mount, ec) && !ec &&
        !fs::is_empty(golden_mount, ec) && !ec)
    {
        return;
    }

    std::cout << "Test vectors not mounted; attempting mount\n";
    if (mountImages())
    {
        readAndPublishVersion();
    }
}
