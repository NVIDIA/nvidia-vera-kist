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

#include <boost/algorithm/string/trim.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/asio/steady_timer.hpp>
#include <ist_app.hpp>
#include <ist_errors.hpp>
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/exception.hpp>
#include <vector_manager.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

namespace fs = std::filesystem;
namespace ist_err = sdbusplus::error::com::nvidia::vera::ist;

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

// DSP0267 v1.3.0 Table 20 (component image information).
static constexpr size_t k_comp_info_fixed_size = 22;
static constexpr size_t k_comp_info_location_offset = 12;
static constexpr size_t k_comp_info_size_offset = 16;
static constexpr size_t k_comp_info_version_len_offset = 21;
static constexpr size_t k_comp_info_opaque_len_size = 4;
static constexpr size_t k_pldm_package_version_len_offset = 35;

// Package size from the component image information area, or 0 if the header
// cannot be walked.  0 is not a reject; progress then uses a heartbeat.
static uint64_t package_size_from_header(const std::vector<uint8_t>& hdr,
                                         size_t header_size)
{
    size_t offset =
        k_pldm_min_header_size + hdr[k_pldm_package_version_len_offset];

    if (offset >= header_size)
    {
        return 0;
    }
    uint8_t device_records = hdr[offset];
    offset += sizeof(uint8_t);

    while (device_records-- > 0)
    {
        if (offset + sizeof(uint16_t) > header_size)
        {
            return 0;
        }
        uint16_t record_length = read_u16_le(&hdr[offset]);
        if (record_length == 0)
        {
            return 0;
        }
        offset += record_length;
    }

    if (offset >= header_size || hdr[offset] != 0)
    {
        return 0;
    }
    offset += sizeof(uint8_t); // DownstreamDeviceIDRecordCount

    if (offset + sizeof(uint16_t) > header_size)
    {
        return 0;
    }
    uint16_t component_count = read_u16_le(&hdr[offset]);
    offset += sizeof(uint16_t);

    uint64_t package_end = 0;
    while (component_count-- > 0)
    {
        if (offset + k_comp_info_fixed_size > header_size)
        {
            return 0;
        }
        uint64_t location =
            read_u32_le(&hdr[offset + k_comp_info_location_offset]);
        uint64_t size = read_u32_le(&hdr[offset + k_comp_info_size_offset]);
        package_end = std::max(package_end, location + size);
        offset += k_comp_info_fixed_size +
                  hdr[offset + k_comp_info_version_len_offset] +
                  k_comp_info_opaque_len_size;
    }

    return package_end;
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
        lg2::error("validate_pldm_header called with short buffer "
                   "({SIZE} bytes)",
                   "SIZE", hdr.size());
        return false;
    }

    uint8_t revision = hdr[16];
    uint16_t header_size = read_u16_le(&hdr[17]);

    if (hdr.size() < header_size)
    {
        lg2::error("PLDM header truncated: have {HAVE} bytes, header "
                   "declares {DECLARED}",
                   "HAVE", hdr.size(), "DECLARED", header_size);
        return false;
    }

    // Per DSP0267 v1.3.0 Table 8: revision 0x04 is the first format
    // that includes PackagePayloadChecksum.
    if (revision < 4)
    {
        lg2::error("PLDM revision {REVISION} does not include payload "
                   "checksum; rejecting",
                   "REVISION", lg2::hex, static_cast<unsigned>(revision));
        return false;
    }

    // Revision >= 4 trailer: [header CRC (4)] [payload CRC (4)]
    constexpr size_t trailer_size = 8;
    if (header_size < k_pldm_min_header_size + trailer_size)
    {
        lg2::error("PLDM header too small for trailer");
        return false;
    }

    size_t crc_offset = header_size - trailer_size;
    uint32_t stored_crc = read_u32_le(&hdr[crc_offset]);
    uint32_t computed_crc = compute_crc32(hdr.data(), crc_offset);
    if (stored_crc != computed_crc)
    {
        lg2::error("PLDM header CRC mismatch: stored {STORED} computed "
                   "{COMPUTED}",
                   "STORED", lg2::hex, stored_crc, "COMPUTED", lg2::hex,
                   computed_crc);
        return false;
    }

    comp.payload_crc = read_u32_le(&hdr[header_size - 4]);
    comp.offset = header_size;

    uint64_t package_size = package_size_from_header(hdr, header_size);
    comp.payload_size =
        package_size > header_size ? package_size - header_size : 0;

    lg2::info("PLDM revision {REVISION}, header size {HEADER_SIZE}; payload "
              "starts at offset {OFFSET} and spans {SPAN} bytes",
              "REVISION", lg2::hex, static_cast<unsigned>(revision),
              "HEADER_SIZE", header_size, "OFFSET", comp.offset, "SPAN",
              comp.payload_size);
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
        lg2::error("open loop-control: {ERRNO}", "ERRNO", errno);
        return std::nullopt;
    }
    int loop_num = ::ioctl(ctl_fd.get(), LOOP_CTL_GET_FREE);
    if (loop_num < 0)
    {
        lg2::error("LOOP_CTL_GET_FREE: {ERRNO}", "ERRNO", errno);
        return std::nullopt;
    }

    std::string loop_dev = "/dev/loop" + std::to_string(loop_num);

    UniqueFd loop_fd(::open(loop_dev.c_str(), open_flags | O_CLOEXEC));
    if (loop_fd.get() < 0)
    {
        lg2::error("open {PATH}: {ERRNO}", "PATH", loop_dev, "ERRNO", errno);
        return std::nullopt;
    }
    UniqueFd img_fd(::open(image_path.c_str(), open_flags | O_CLOEXEC));
    if (img_fd.get() < 0)
    {
        lg2::error("open {PATH}: {ERRNO}", "PATH", image_path, "ERRNO", errno);
        return std::nullopt;
    }
    if (::ioctl(loop_fd.get(), LOOP_SET_FD, img_fd.get()) < 0)
    {
        lg2::error("LOOP_SET_FD for {PATH}: {ERRNO}", "PATH", image_path,
                   "ERRNO", errno);
        return std::nullopt;
    }

    struct loop_info64 info = {};
    info.lo_flags = LO_FLAGS_AUTOCLEAR;
    if (::ioctl(loop_fd.get(), LOOP_SET_STATUS64, &info) < 0)
    {
        lg2::error("LOOP_SET_STATUS64 for {PATH}: {ERRNO}", "PATH", loop_dev,
                   "ERRNO", errno);
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
        on_complete_(std::move(on_complete))
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
                lg2::error("Transfer aborted before PLDM header received");
                finish(false);
            }
            else
            {
                lg2::error("Upload ended before full PLDM header "
                           "received");
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
                lg2::error("Invalid PLDM header size in upload");
                finish(false);
                return;
            case PldmHeaderCheck::component_uuid_mismatch:
                lg2::error("PLDM component UUID mismatch (expected IST "
                           "vectors component)");
                finish(false);
                return;
            case PldmHeaderCheck::ok:
                if (!validate_pldm_header(prefix_, comp_))
                {
                    finish(false);
                    return;
                }
                lg2::info("PLDM header verified");
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
        lg2::error("Timed out waiting for PLDM header");
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
                lg2::error("Failed to close read stream: {ERROR}", "ERROR",
                           ec.message());
            }
        }

        UniqueFd read_fd(-1);
        if (ok)
        {
            read_fd = UniqueFd(stream_.release());
        }

        auto cb = std::move(on_complete_);
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
        on_complete_;
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
// Owns: stream_descriptor (read-end FD), ofstream, deadline timer, progress
// timer.  On destruction, removes the image file unless committed (a transfer
// is committed only once the full payload arrives and its CRC matches).
// ----------------------------------------------------------------

// 100 is reserved for a committed image.
static constexpr uint8_t k_transfer_progress_max = 99;

class TransferSession : public std::enable_shared_from_this<TransferSession>
{
  public:
    TransferSession(
        boost::asio::io_context& io, UniqueFd read_fd, fs::path image_path,
        std::chrono::seconds timeout, std::chrono::seconds progress_interval,
        std::vector<uint8_t> prefix, PldmComponentInfo comp,
        std::move_only_function<void(uint8_t progress) const> on_progress,
        std::move_only_function<void(bool ok) const> on_complete) :
        stream_(io, read_fd.release()), deadline_(io), progress_timer_(io),
        image_path_(std::move(image_path)), timeout_(timeout),
        progress_interval_(progress_interval), prefix_(std::move(prefix)),
        payload_offset_(comp.offset), expected_crc_(comp.payload_crc),
        payload_total_(comp.payload_size), on_progress_(std::move(on_progress)),
        on_complete_(std::move(on_complete))
    {
        // Overwrite in place (no room for a temp copy): once the validated
        // header gets us here, a payload-level failure intentionally leaves no
        // image. Header/UUID failures are rejected earlier and never reach
        // this.
        output_.open(image_path_, std::ios::binary | std::ios::trunc);
        if (!output_)
        {
            throw std::system_error(errno, std::generic_category(),
                                    "Failed to open " + image_path_.string());
        }
    }

    ~TransferSession()
    {
        discard_if_uncommitted();
    }

    TransferSession(const TransferSession&) = delete;
    TransferSession& operator=(const TransferSession&) = delete;

    void start()
    {
        // The buffered prefix holds the full PLDM header plus, usually, the
        // first payload bytes.  Write only the payload portion; the header is
        // intentionally never persisted.
        if (prefix_.size() > payload_offset_)
        {
            const uint8_t* payload = prefix_.data() + payload_offset_;
            size_t payload_len = prefix_.size() - payload_offset_;
            if (!write_payload(payload, payload_len))
            {
                return;
            }
        }
        prefix_.clear();
        reset_deadline();
        schedule_progress_tick();
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
            lg2::error("Write to image file failed");
            finish(false);
            return false;
        }
        crc_state_ = crc32_update(crc_state_, data, len);
        payload_bytes_ += len;
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

    void schedule_progress_tick()
    {
        progress_timer_.expires_after(progress_interval_);
        progress_timer_.async_wait(
            [weak = weak_from_this()](const boost::system::error_code& ec) {
                if (ec)
                {
                    return;
                }
                if (std::shared_ptr<TransferSession> self = weak.lock())
                {
                    self->on_progress_tick();
                }
            });
    }

    void on_progress_tick()
    {
        if (finished_)
        {
            return;
        }

        if (payload_bytes_ != reported_bytes_)
        {
            reported_bytes_ = payload_bytes_;
            uint8_t progress = next_progress();
            if (progress != last_progress_ && on_progress_)
            {
                last_progress_ = progress;
                on_progress_(progress);
            }
        }

        schedule_progress_tick();
    }

    uint8_t next_progress() const
    {
        if (payload_total_ == 0)
        {
            return static_cast<uint8_t>(std::min<unsigned>(
                last_progress_ + 1U, k_transfer_progress_max));
        }
        uint64_t pct = static_cast<uint64_t>(payload_bytes_) *
                       k_transfer_progress_max / payload_total_;
        return static_cast<uint8_t>(
            std::min<uint64_t>(pct, k_transfer_progress_max));
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
                lg2::error("Image transfer read error: {ERROR}", "ERROR",
                           ec.message());
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
        uint32_t computed = ~crc_state_;
        if (computed != expected_crc_)
        {
            lg2::error("PLDM payload CRC mismatch: stored {STORED} computed "
                       "{COMPUTED} ({BYTES} payload bytes received; truncated "
                       "transfer?)",
                       "STORED", lg2::hex, expected_crc_, "COMPUTED", lg2::hex,
                       computed, "BYTES", payload_bytes_);
            finish(false);
            return;
        }

        lg2::info("Image transfer complete: {BYTES} payload bytes, CRC "
                  "verified",
                  "BYTES", payload_bytes_);
        committed_ = true;
        finish(true);
    }

    void on_timeout()
    {
        if (finished_)
        {
            return;
        }
        lg2::error("Transfer timed out after {TIMEOUT}s of inactivity "
                   "({BYTES} payload bytes received)",
                   "TIMEOUT", timeout_.count(), "BYTES", payload_bytes_);
        stream_.close();
        finish(false);
    }

    void discard_if_uncommitted()
    {
        output_.close();
        if (!committed_)
        {
            std::error_code ec;
            fs::remove(image_path_, ec);
            if (ec)
            {
                lg2::error("Failed to remove the partially written image "
                           "{PATH}: {ERROR}; a later mount may find it",
                           "PATH", image_path_, "ERROR", ec.message());
            }
        }
    }

    void finish(bool ok)
    {
        if (finished_)
        {
            return;
        }
        finished_ = true;
        deadline_.cancel();
        progress_timer_.cancel();
        discard_if_uncommitted();
        auto cb = std::move(on_complete_);
        if (cb)
        {
            cb(ok);
        }
    }

    boost::asio::posix::stream_descriptor stream_;
    boost::asio::steady_timer deadline_;
    boost::asio::steady_timer progress_timer_;
    std::ofstream output_;
    fs::path image_path_;
    std::chrono::seconds timeout_;
    std::chrono::seconds progress_interval_;
    std::vector<uint8_t> prefix_;
    size_t payload_offset_;
    uint32_t expected_crc_;
    uint64_t payload_total_;
    std::move_only_function<void(uint8_t progress) const> on_progress_;
    std::move_only_function<void(bool ok) const> on_complete_;
    std::array<char, k_transfer_buf_size> buf_{};
    uint32_t crc_state_{0xFFFFFFFF};
    size_t payload_bytes_{0};
    size_t reported_bytes_{0};
    uint8_t last_progress_{0};
    bool finished_{false};
    bool committed_{false};
};

// ----------------------------------------------------------------
// VectorManager
// ----------------------------------------------------------------

std::shared_ptr<VectorManager>
    VectorManager::create(boost::asio::io_context& io,
                          StatePublisher& publisher,
                          const IstPlatformConfig& cfg)
{
    return std::shared_ptr<VectorManager>(
        new VectorManager(io, publisher, cfg));
}

VectorManager::VectorManager(boost::asio::io_context& io,
                             StatePublisher& publisher,
                             const IstPlatformConfig& cfg) :
    io_(io), publisher_(publisher), platform_cfg_(cfg)
{}

void VectorManager::mount_vectors_on_startup()
{
    const bool mounted = mount_images();
    read_and_publish_version();
    publisher_.publish_activation(mounted ? k_activation_active
                                          : k_activation_failed);
}

std::string VectorManager::start_update(UniqueFd image,
                                        std::string_view apply_time)
{
    if (update_in_progress_)
    {
        lg2::error("StartUpdate rejected: an update is already in "
                   "progress");
        throw ist_err::Unavailable{};
    }
    if (platform_cfg_.storage.vector_storage_path.empty())
    {
        lg2::error("StartUpdate rejected: vector storage path is empty");
        throw ist_err::InternalFailure{};
    }
    if (image.get() < 0)
    {
        lg2::error("StartUpdate rejected: invalid image fd");
        throw ist_err::InternalFailure{};
    }
    if (!is_allowed_apply_time(apply_time))
    {
        lg2::error("StartUpdate rejected: unsupported apply time "
                   "'{APPLY_TIME}'",
                   "APPLY_TIME", apply_time);
        throw ist_err::InvalidArgument{};
    }
    (void)apply_time; // Immediate and OnReset are accepted; apply is always
                      // immediate.

    const fs::path& storage_path = platform_cfg_.storage.vector_storage_path;
    fs::path image_path = storage_path / k_image_file_name;

    update_in_progress_ = true;

    std::shared_ptr<PldmHeaderPeekSession> peek =
        std::make_shared<PldmHeaderPeekSession>(
            io_, std::move(image), platform_cfg_.transfer_inactivity_timeout,
            [weak = weak_from_this(),
             image_path](bool ok, std::vector<uint8_t> prefix, UniqueFd read_fd,
                         PldmComponentInfo comp) {
                if (auto self = weak.lock())
                {
                    self->on_header_peek_complete(ok, std::move(prefix),
                                                  std::move(read_fd), comp,
                                                  image_path);
                }
            });
    active_header_peek_ = peek;
    peek->start();

    return std::format("/xyz/openbmc_project/inventory_software/{}",
                       platform_cfg_.software_inventory_id);
}

void VectorManager::on_transfer_complete(bool ok)
{
    active_transfer_.reset();
    if (!ok)
    {
        lg2::error("Image transfer failed");
        finish_update(false);
        return;
    }

    lg2::info("Image transfer succeeded (payload verified); mounting");
    publisher_.publish_activation_progress(100);
    publisher_.remove_activation_progress();

    on_mount_complete(mount_images());
}

bool VectorManager::teardown_mounts()
{
    const fs::path& mount_path = platform_cfg_.storage.vector_mount_path;

    if (mount_path.empty())
    {
        return true;
    }

    bool ok = true;
    fs::path golden_mount = mount_path / "GOLDEN_RES";
    if (umount2(golden_mount.c_str(), 0) != 0 && errno != EINVAL &&
        errno != ENOENT && errno != EPERM)
    {
        lg2::error("Failed to unmount {PATH}: {ERRNO}", "PATH", golden_mount,
                   "ERRNO", errno);
        ok = false;
    }

    if (umount2(mount_path.c_str(), 0) != 0 && errno != EINVAL &&
        errno != ENOENT && errno != EPERM)
    {
        lg2::error("Failed to unmount {PATH}: {ERRNO}", "PATH", mount_path,
                   "ERRNO", errno);
        ok = false;
    }

    return ok;
}

bool VectorManager::mount_images()
{
    const fs::path& storage_path = platform_cfg_.storage.vector_storage_path;
    const fs::path& mount_path = platform_cfg_.storage.vector_mount_path;

    if (storage_path.empty() || mount_path.empty())
    {
        lg2::error("vectorStoragePath or vectorMountPath not configured");
        return false;
    }

    if (!teardown_mounts())
    {
        lg2::error("Cannot mount: failed to unmount existing images");
        return false;
    }

    fs::path image_path = storage_path / k_image_file_name;
    std::error_code ec;
    bool exists = fs::exists(image_path, ec);
    if (ec)
    {
        lg2::error("Failed to stat {PATH}: {ERROR}", "PATH", image_path,
                   "ERROR", ec.message());
        return false;
    }
    if (!exists)
    {
        lg2::info("No IST image at {PATH}; nothing to mount", "PATH",
                  image_path);
        return false;
    }

    fs::create_directories(mount_path, ec);
    if (ec)
    {
        lg2::error("Failed to create {PATH}: {ERROR}", "PATH", mount_path,
                   "ERROR", ec.message());
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
        lg2::error("mount ext4 at {PATH}: {ERRNO}", "PATH", mount_path, "ERRNO",
                   errno);
        return false;
    }
    lg2::info("Mounted ext4 image at {PATH}", "PATH", mount_path);

    auto undo_ext4 = [&mount_path]() { ::umount2(mount_path.c_str(), 0); };

    fs::path sqsh_path = mount_path / "GOLDEN_RES.sqfs";
    exists = fs::exists(sqsh_path, ec);
    if (ec)
    {
        lg2::error("Failed to stat {PATH}: {ERROR}", "PATH", sqsh_path, "ERROR",
                   ec.message());
        undo_ext4();
        return false;
    }
    if (!exists)
    {
        lg2::error("Golden results image not found: {PATH}", "PATH", sqsh_path);
        undo_ext4();
        return false;
    }

    fs::path golden_mount = mount_path / "GOLDEN_RES";
    fs::create_directories(golden_mount, ec);
    if (ec)
    {
        lg2::error("Failed to create {PATH}: {ERROR}", "PATH", golden_mount,
                   "ERROR", ec.message());
        undo_ext4();
        return false;
    }

    std::optional<LoopDevice> sqsh_loop =
        setup_loop_device(sqsh_path, O_RDONLY);
    if (!sqsh_loop)
    {
        lg2::error("Failed to set up loop device for squashfs");
        undo_ext4();
        return false;
    }

    if (::mount(sqsh_loop->path.c_str(), golden_mount.c_str(), "squashfs",
                MS_RDONLY, nullptr) != 0)
    {
        lg2::error("mount squashfs at {PATH}: {ERRNO}", "PATH", golden_mount,
                   "ERRNO", errno);
        undo_ext4();
        return false;
    }
    lg2::info("Mounted golden results at {PATH}", "PATH", golden_mount);

    return true;
}

void VectorManager::read_and_publish_version()
{
    const fs::path& mount_path = platform_cfg_.storage.vector_mount_path;
    if (mount_path.empty())
    {
        lg2::error("vectorMountPath not configured; skipping version "
                   "read");
        return;
    }

    fs::path version_file = mount_path / "version.txt";
    std::ifstream f(version_file);
    if (!f.is_open())
    {
        lg2::info("No version.txt at {PATH}; version unavailable", "PATH",
                  version_file);
        return;
    }

    std::string version;
    if (!std::getline(f, version))
    {
        lg2::error("Failed to read {PATH}", "PATH", version_file);
        return;
    }

    boost::algorithm::trim(version);

    constexpr size_t k_max_version_length = 256;
    if (version.size() > k_max_version_length)
    {
        lg2::error("Version string too long ({SIZE} chars), truncating to "
                   "{LIMIT}",
                   "SIZE", version.size(), "LIMIT", k_max_version_length);
        version.resize(k_max_version_length);
    }

    if (version.empty())
    {
        lg2::error("version.txt at {PATH} is empty", "PATH", version_file);
        return;
    }

    publisher_.publish_version(version);
    lg2::info("IST vector version: {VERSION}", "VERSION", version);
}

void VectorManager::finish_update(bool ok)
{
    publisher_.publish_activation(ok ? k_activation_active
                                     : k_activation_failed);
    publisher_.remove_activation_progress();
    update_in_progress_ = false;
}

void VectorManager::on_header_peek_complete(bool ok,
                                            std::vector<uint8_t> prefix,
                                            UniqueFd read_fd,
                                            PldmComponentInfo comp,
                                            const fs::path& image_path)
{
    active_header_peek_.reset();
    if (!ok)
    {
        finish_update(false);
        return;
    }

    bool tear_ok = teardown_mounts();
    on_teardown_complete(tear_ok, std::move(read_fd), std::move(prefix), comp,
                         image_path);
}

void VectorManager::on_teardown_complete(bool ok, UniqueFd read_fd,
                                         std::vector<uint8_t> prefix,
                                         PldmComponentInfo comp,
                                         const fs::path& image_path)
{
    if (!ok)
    {
        lg2::error("Failed to unmount existing images; rejecting update");
        finish_update(false);
        return;
    }

    if (read_fd.get() < 0)
    {
        lg2::error("Failed to start image transfer");
        finish_update(false);
        return;
    }

    try
    {
        std::shared_ptr<TransferSession> session =
            std::make_shared<TransferSession>(
                io_, std::move(read_fd), image_path,
                platform_cfg_.transfer_inactivity_timeout,
                platform_cfg_.transfer_progress_interval, std::move(prefix),
                comp,
                [weak = weak_from_this()](uint8_t progress) {
                    std::shared_ptr<VectorManager> self = weak.lock();
                    if (self)
                    {
                        self->publisher_.publish_activation_progress(progress);
                    }
                },
                [weak = weak_from_this()](bool xfer_ok) {
                    if (auto self = weak.lock())
                    {
                        self->on_transfer_complete(xfer_ok);
                    }
                });

        publisher_.publish_activation(k_activation_activating);
        publisher_.create_activation_progress();
        publisher_.publish_activation_progress(0);

        active_transfer_ = session;
        session->start();
    }
    catch (const std::system_error& e)
    {
        lg2::error("Failed to start transfer: {ERROR}", "ERROR", e.what());
        finish_update(false);
    }
}

void VectorManager::on_mount_complete(bool ok)
{
    if (!ok)
    {
        lg2::error("Failed to mount images after transfer");
        finish_update(false);
        return;
    }
    read_and_publish_version();
    finish_update(true);
}

void VectorManager::ensure_mounted()
{
    const fs::path& mount_path = platform_cfg_.storage.vector_mount_path;
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

    lg2::info("Test vectors not mounted; attempting mount");
    if (mount_images())
    {
        read_and_publish_version();
    }
}

// ----------------------------------------------------------------
// IstService::startUpdate — service-level guard, then delegate
// ----------------------------------------------------------------

std::string IstService::start_update(int image_fd, std::string_view apply_time)
{
    // Take ownership immediately so the fd is closed on any early return/throw
    // below; on success it is moved into the update flow.
    UniqueFd image(image_fd);
    if (!initialized_)
    {
        lg2::error("StartUpdate rejected: service not initialized");
        throw ist_err::Unavailable{};
    }
    if (state_.stage != IstStage::idle)
    {
        lg2::error("StartUpdate rejected: IST is running "
                   "(stage={STAGE})",
                   "STAGE", ist_stage_to_string(state_.stage));
        throw ist_err::Unavailable{};
    }
    return vector_manager_->start_update(std::move(image), apply_time);
}
