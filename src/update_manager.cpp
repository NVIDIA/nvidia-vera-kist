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
#include <tuple>
#include <vector>

namespace fs = std::filesystem;

namespace
{

struct InvalidArgument final : sdbusplus::exception::generated_exception
{
    const char* name() const noexcept override
    {
        return "xyz.openbmc_project.Common.Error.InvalidArgument";
    }
    const char* description() const noexcept override
    {
        return "Invalid argument was given.";
    }
    const char* what() const noexcept override
    {
        return "xyz.openbmc_project.Common.Error.InvalidArgument: "
               "Invalid argument was given.";
    }
};

struct Unavailable final : sdbusplus::exception::generated_exception
{
    const char* name() const noexcept override
    {
        return "xyz.openbmc_project.Common.Error.Unavailable";
    }
    const char* description() const noexcept override
    {
        return "The service is temporarily unavailable.";
    }
    const char* what() const noexcept override
    {
        return "xyz.openbmc_project.Common.Error.Unavailable: "
               "The service is temporarily unavailable.";
    }
};

} // namespace

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
static constexpr size_t k_pldm_uuid_size = 16;
static constexpr size_t k_pldm_header_size_field_offset = 17;
static constexpr size_t k_pldm_header_prefix_size = 19;
static constexpr size_t k_pldm_max_header_size = 4096;
static constexpr uint16_t k_pldm_descriptor_type_uuid = 0x0002;

static constexpr std::array<uint8_t, 16> k_ist_vectors_component_uuid = {
    0x8f, 0x1b, 0x9b, 0x0e, 0x3c, 0x6d, 0x4a, 0x8c,
    0x9f, 0x32, 0x12, 0xe7, 0xb4, 0xd9, 0xc5, 0xa1,
};

static bool header_contains_ist_component_uuid(const uint8_t* hdr,
                                               size_t header_size)
{
    if (header_size < k_pldm_min_header_size)
    {
        return false;
    }

    const uint8_t descriptor_prefix[] = {
        static_cast<uint8_t>(k_pldm_descriptor_type_uuid & 0xFF),
        static_cast<uint8_t>((k_pldm_descriptor_type_uuid >> 8) & 0xFF),
        static_cast<uint8_t>(k_pldm_uuid_size & 0xFF),
        static_cast<uint8_t>((k_pldm_uuid_size >> 8) & 0xFF),
    };

    for (size_t i = 0;
         i + sizeof(descriptor_prefix) + k_pldm_uuid_size <= header_size; ++i)
    {
        if (!std::equal(std::begin(descriptor_prefix),
                        std::end(descriptor_prefix), hdr + i))
        {
            continue;
        }
        const uint8_t* uuid = hdr + i + sizeof(descriptor_prefix);
        if (std::equal(k_ist_vectors_component_uuid.begin(),
                       k_ist_vectors_component_uuid.end(), uuid))
        {
            return true;
        }
    }
    return false;
}

enum class PldmHeaderCheck
{
    need_more_data,
    invalid_header,
    component_uuid_mismatch,
    ok,
};

static PldmHeaderCheck check_ist_component_uuid(const uint8_t* data, size_t len)
{
    if (len < k_pldm_header_prefix_size)
    {
        return PldmHeaderCheck::need_more_data;
    }

    uint16_t header_size = read_u16_le(data + k_pldm_header_size_field_offset);
    if (header_size < k_pldm_min_header_size ||
        header_size > k_pldm_max_header_size)
    {
        return PldmHeaderCheck::invalid_header;
    }

    if (len < header_size)
    {
        return PldmHeaderCheck::need_more_data;
    }

    if (!header_contains_ist_component_uuid(data, header_size))
    {
        return PldmHeaderCheck::component_uuid_mismatch;
    }

    return PldmHeaderCheck::ok;
}

// Validate a fully-buffered PLDM package header and extract the payload
// offset (== header size) and the stored payload CRC.  This runs entirely
// in memory during the upload "peek" phase, before any data is written to
// disk, so a malformed header never disturbs the existing on-disk image.
//
// Precondition: check_ist_component_uuid(hdr.data(), hdr.size()) == ok,
// i.e. the buffer already contains the complete header and the IST vectors
// component UUID has been matched.
static bool validate_pldm_header(const std::vector<uint8_t>& hdr,
                                 PldmComponentInfo& comp)
{
    if (hdr.size() < k_pldm_min_header_size)
    {
        std::cerr << "validate_pldm_header called with short buffer ("
                  << hdr.size() << " bytes)\n";
        return false;
    }

    uint8_t revision = hdr[16];
    uint16_t header_size = read_u16_le(&hdr[17]);

    if (hdr.size() < header_size)
    {
        std::cerr << "PLDM header truncated: have " << hdr.size()
                  << " bytes, header declares " << header_size << '\n';
        return false;
    }

    // Per DSP0267 v1.3.0 Table 8: revision 0x04 is the first format
    // that includes PackagePayloadChecksum.
    if (revision < 4)
    {
        std::cerr << "PLDM revision 0x" << std::hex
                  << static_cast<unsigned>(revision) << std::dec
                  << " does not include payload checksum; rejecting\n";
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
    comp.offset = header_size;

    std::cerr << "PLDM revision 0x" << std::hex
              << static_cast<unsigned>(revision) << std::dec << ", header size "
              << header_size << "; payload starts at offset " << comp.offset
              << '\n';
    return true;
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
// PldmHeaderPeekSession — read full PLDM header before touching disk
// ----------------------------------------------------------------

class PldmHeaderPeekSession :
    public std::enable_shared_from_this<PldmHeaderPeekSession>
{
  public:
    PldmHeaderPeekSession(boost::asio::io_context& io, UniqueFd read_fd,
                          std::chrono::seconds timeout,
                          std::move_only_function<void(
                              bool ok, std::vector<uint8_t> prefix,
                              UniqueFd read_fd, PldmComponentInfo comp) const>
                              on_complete) :
        stream_(io, read_fd.release()), deadline_(io), timeout_(timeout),
        onComplete_(std::move(on_complete))
    {}

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
            if (prefix_.empty())
            {
                std::cerr << "Transfer aborted before PLDM header received\n";
                finish(false);
            }
            else
            {
                std::cerr << "Upload ended before full PLDM header received\n";
                finish(false);
            }
            return;
        }

        prefix_.insert(prefix_.end(), buf_.data(), buf_.data() + n);

        switch (check_ist_component_uuid(prefix_.data(), prefix_.size()))
        {
            case PldmHeaderCheck::need_more_data:
                reset_deadline();
                read_next();
                return;
            case PldmHeaderCheck::invalid_header:
                std::cerr << "Invalid PLDM header size in upload\n";
                finish(false);
                return;
            case PldmHeaderCheck::component_uuid_mismatch:
                std::cerr << "PLDM component UUID mismatch (expected IST "
                             "vectors component)\n";
                finish(false);
                return;
            case PldmHeaderCheck::ok:
                if (!validate_pldm_header(prefix_, comp_))
                {
                    finish(false);
                    return;
                }
                std::cout << "PLDM header verified\n";
                finish(true);
                return;
        }
    }

    void on_timeout()
    {
        if (finished_)
        {
            return;
        }
        std::cerr << "Timed out waiting for PLDM header\n";
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

        if (!ok)
        {
            boost::system::error_code ec;
            // Return value discarded; errors are reported via ec
            std::ignore = stream_.close(ec);
            if (ec)
            {
                std::cerr << "Failed to close read stream: " << ec.message()
                          << '\n';
            }
        }

        UniqueFd read_fd(-1);
        if (ok)
        {
            read_fd = UniqueFd(stream_.release());
        }

        auto cb = std::move(onComplete_);
        if (cb)
        {
            cb(ok, std::move(prefix_), std::move(read_fd), comp_);
        }
    }

    boost::asio::posix::stream_descriptor stream_;
    boost::asio::steady_timer deadline_;
    std::chrono::seconds timeout_;
    std::move_only_function<void(bool ok, std::vector<uint8_t> prefix,
                                 UniqueFd read_fd, PldmComponentInfo comp)
                                const>
        onComplete_;
    std::array<char, k_transfer_buf_size> buf_{};
    std::vector<uint8_t> prefix_;
    PldmComponentInfo comp_{};
    bool finished_{false};
};

// ----------------------------------------------------------------
// TransferSession — async image transfer from the upload stream
//
// Streams the PLDM component payload straight to the image file on the
// io_context thread, skipping the PLDM package header (which is never
// persisted) and verifying the payload CRC as bytes arrive.  The expected
// payload offset and CRC come from the header validated during the peek
// phase, so there is no second pass over the file.
//
// Owns: stream_descriptor (read-end FD), ofstream, deadline timer.
// On destruction, removes the image file unless committed (a transfer is
// committed only once the full payload arrives and its CRC matches).
// ----------------------------------------------------------------

class TransferSession : public std::enable_shared_from_this<TransferSession>
{
  public:
    TransferSession(boost::asio::io_context& io, UniqueFd read_fd,
                    fs::path image_path, std::chrono::seconds timeout,
                    std::vector<uint8_t> prefix, PldmComponentInfo comp,
                    std::move_only_function<void(bool ok) const> on_complete) :
        stream_(io, read_fd.release()), deadline_(io),
        imagePath_(std::move(image_path)), timeout_(timeout),
        prefix_(std::move(prefix)), payloadOffset_(comp.offset),
        expectedCrc_(comp.payloadCrc), onComplete_(std::move(on_complete))
    {
        // Overwrite in place (no room for a temp copy): once the validated
        // header gets us here, a payload-level failure intentionally leaves no
        // image. Header/UUID failures are rejected earlier and never reach
        // this.
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
        // The buffered prefix holds the full PLDM header plus, usually, the
        // first payload bytes.  Write only the payload portion; the header is
        // intentionally never persisted.
        if (prefix_.size() > payloadOffset_)
        {
            const uint8_t* payload = prefix_.data() + payloadOffset_;
            size_t payload_len = prefix_.size() - payloadOffset_;
            if (!write_payload(payload, payload_len))
            {
                return;
            }
        }
        prefix_.clear();
        reset_deadline();
        read_next();
    }

  private:
    // Append `len` payload bytes to the image file and fold them into the
    // running CRC.  Returns false (and finishes the session) on write error.
    bool write_payload(const uint8_t* data, size_t len)
    {
        output_.write(reinterpret_cast<const char*>(data),
                      static_cast<std::streamsize>(len));
        if (!output_)
        {
            std::cerr << "Write to image file failed\n";
            finish(false);
            return false;
        }
        crcState_ = crc32_update(crcState_, data, len);
        payloadBytes_ += len;
        return true;
    }

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
            if (ec != boost::asio::error::eof)
            {
                std::cerr << "Image transfer read error: " << ec.message()
                          << '\n';
            }
            verify_and_finish();
            return;
        }

        if (!write_payload(reinterpret_cast<const uint8_t*>(buf_.data()), n))
        {
            return;
        }

        reset_deadline();
        read_next();
    }

    void verify_and_finish()
    {
        uint32_t computed = ~crcState_;
        if (computed != expectedCrc_)
        {
            std::cerr << "PLDM payload CRC mismatch: stored 0x" << std::hex
                      << expectedCrc_ << " computed 0x" << computed << std::dec
                      << " (" << payloadBytes_
                      << " payload bytes received; truncated transfer?)\n";
            finish(false);
            return;
        }

        std::cout << "Image transfer complete: " << payloadBytes_
                  << " payload bytes, CRC verified\n";
        committed_ = true;
        finish(true);
    }

    void on_timeout()
    {
        if (finished_)
        {
            return;
        }
        std::cerr << "Transfer timed out after " << timeout_.count()
                  << "s of inactivity (" << payloadBytes_
                  << " payload bytes received)\n";
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
    std::vector<uint8_t> prefix_;
    size_t payloadOffset_;
    uint32_t expectedCrc_;
    std::move_only_function<void(bool ok) const> onComplete_;
    std::array<char, k_transfer_buf_size> buf_{};
    uint32_t crcState_{0xFFFFFFFF};
    size_t payloadBytes_{0};
    bool finished_{false};
    bool committed_{false};
};

// ----------------------------------------------------------------
// IstService::startUpdate
// ----------------------------------------------------------------

std::string IstService::startUpdate(int image_fd, std::string_view apply_time)
{
    // Take ownership immediately so the fd is closed on any early return/throw
    // below; on success it is moved into the peek session.
    UniqueFd image(image_fd);
    if (!initialized_)
    {
        throw Unavailable{};
    }
    if (state_.stage != IstStage::idle)
    {
        throw Unavailable{};
    }
    if (updateInProgress_)
    {
        throw Unavailable{};
    }
    if (platformCfg_.storage.vectorStoragePath.empty())
    {
        throw InvalidArgument{};
    }
    if (image.get() < 0)
    {
        throw InvalidArgument{};
    }
    if (!isAllowedApplyTime(apply_time))
    {
        throw InvalidArgument{};
    }
    (void)apply_time; // Immediate and OnReset are accepted; apply is always
                      // immediate.

    const fs::path& storage_path = platformCfg_.storage.vectorStoragePath;
    fs::path image_path = storage_path / k_image_file_name;

    updateInProgress_ = true;

    std::shared_ptr<PldmHeaderPeekSession> peek =
        std::make_shared<PldmHeaderPeekSession>(
            io_, std::move(image), platformCfg_.transferInactivityTimeout,
            [weak = weak_from_this(),
             image_path](bool ok, std::vector<uint8_t> prefix, UniqueFd read_fd,
                         PldmComponentInfo comp) {
                if (auto self = weak.lock())
                {
                    self->onHeaderPeekComplete(ok, std::move(prefix),
                                               std::move(read_fd), comp,
                                               image_path);
                }
            });
    activeHeaderPeek_ = peek;
    peek->start();

    return swObjectPath_;
}

void IstService::onTransferComplete(bool ok)
{
    activeTransfer_.reset();
    if (!ok)
    {
        std::cerr << "Image transfer failed\n";
        finishUpdate(false);
        return;
    }

    std::cout << "Image transfer succeeded (payload verified); mounting\n";
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

void IstService::onHeaderPeekComplete(bool ok, std::vector<uint8_t> prefix,
                                      UniqueFd read_fd, PldmComponentInfo comp,
                                      const fs::path& image_path)
{
    activeHeaderPeek_.reset();
    if (!ok)
    {
        finishUpdate(false);
        return;
    }

    auto prefix_buf = std::make_shared<std::vector<uint8_t>>(std::move(prefix));
    auto read_fd_holder = std::make_shared<UniqueFd>(std::move(read_fd));

    asyncTeardownMounts([weak = weak_from_this(), image_path, prefix_buf,
                         read_fd_holder, comp](bool tear_ok) {
        if (auto self = weak.lock())
        {
            self->onTeardownComplete(tear_ok, std::move(*read_fd_holder),
                                     std::move(*prefix_buf), comp, image_path);
        }
    });
}

void IstService::onTeardownComplete(bool ok, UniqueFd read_fd,
                                    std::vector<uint8_t> prefix,
                                    PldmComponentInfo comp,
                                    const fs::path& image_path)
{
    if (!ok)
    {
        std::cerr << "Failed to unmount existing images; rejecting update\n";
        publisher_->publishActivation(k_activation_failed);
        updateInProgress_ = false;
        return;
    }

    if (read_fd.get() < 0)
    {
        std::cerr << "Failed to start image transfer\n";
        publisher_->publishActivation(k_activation_failed);
        updateInProgress_ = false;
        return;
    }

    try
    {
        std::shared_ptr<TransferSession> session =
            std::make_shared<TransferSession>(
                io_, std::move(read_fd), image_path,
                platformCfg_.transferInactivityTimeout, std::move(prefix), comp,
                [weak = weak_from_this()](bool xfer_ok) {
                    if (auto self = weak.lock())
                    {
                        self->onTransferComplete(xfer_ok);
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
        std::cerr << "Failed to mount images after transfer\n";
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
