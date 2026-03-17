#include <fcntl.h>
#include <linux/loop.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <unistd.h>

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
#include <vector>

namespace fs = std::filesystem;

static constexpr std::string_view k_image_file_name = "CPU-IST.img";
static constexpr std::string_view k_golden_res_file_name = "GOLDEN_RES.sqfs";
static constexpr size_t k_transfer_buf_size = 65536;

static constexpr std::string_view k_activation_activating =
    "xyz.openbmc_project.Software.Activation.Activations.Activating";
static constexpr std::string_view k_activation_active =
    "xyz.openbmc_project.Software.Activation.Activations.Active";
static constexpr std::string_view k_activation_failed =
    "xyz.openbmc_project.Software.Activation.Activations.Failed";

// Image file names managed by this service; used to identify orphaned
// loop devices that need cleanup before a new image is written.
static constexpr std::array k_managed_image_names = {
    k_image_file_name,
    k_golden_res_file_name,
};

// ----------------------------------------------------------------
// File-descriptor wrapper
// ----------------------------------------------------------------

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
    int fd_;
};

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

struct PldmComponentInfo
{
    uint32_t offset;
    uint32_t size;
    uint32_t payloadCrc;
};

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

static std::string strip_deleted_suffix(std::string path)
{
    constexpr std::string_view suffix = " (deleted)";
    if (path.size() >= suffix.size() &&
        path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0)
    {
        path.resize(path.size() - suffix.size());
    }
    return path;
}

// Detach a single loop device by path (e.g. "/dev/loop3").
static void teardown_loop_device(const std::string& loop_dev)
{
    UniqueFd fd(::open(loop_dev.c_str(), O_RDWR | O_CLOEXEC));
    if (fd.get() < 0)
    {
        std::cerr << "open(" << loop_dev << "): " << errno << '\n';
        return;
    }
    if (::ioctl(fd.get(), LOOP_CLR_FD) != 0)
    {
        std::cerr << "ioctl(LOOP_CLR_FD) on " << loop_dev << ": " << errno
                  << '\n';
    }
}

// Scan sysfs for loop devices whose backing file is under `dir` and
// matches one of kManagedImageNames, then detach them.
static void teardown_loop_devices_under(const fs::path& dir)
{
    // The ec overload avoids throwing if /sys/block is unreadable;
    // the iterator is simply empty and we skip teardown — best-effort cleanup.
    std::error_code ec;
    for (auto& entry : fs::directory_iterator("/sys/block", ec))
    {
        const std::string name = entry.path().filename().string();
        if (name.compare(0, 4, "loop") != 0)
        {
            continue;
        }
        fs::path backing_path = entry.path() / "loop" / "backing_file";
        std::ifstream backing(backing_path);
        if (!backing)
        {
            continue;
        }
        std::string backing_file;
        std::getline(backing, backing_file);
        backing_file = strip_deleted_suffix(backing_file);

        fs::path bp(backing_file);
        if (bp.parent_path() != dir)
        {
            continue;
        }
        std::string fname = bp.filename().string();
        bool managed = false;
        for (auto& img : k_managed_image_names)
        {
            if (fname == img)
            {
                managed = true;
                break;
            }
        }
        if (!managed)
        {
            continue;
        }

        std::string dev_path = "/dev/" + name;
        std::cout << "Tearing down orphaned loop device " << dev_path
                  << " (backing " << backing_file << ")\n";
        teardown_loop_device(dev_path);
    }
}

// Allocate a free loop device, attach `image_path` as its backing file,
// and return the device path (e.g. "/dev/loop3").  Returns std::nullopt
// on failure.
static std::optional<std::string> setup_loop_device(const fs::path& image_path,
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
    return loop_dev;
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
// PldmStripper — async in-place PLDM header removal
//
// Opens the image file O_RDWR, copies payload bytes forward using
// pread/pwrite in 64 KiB chunks (each posted to the io_context),
// verifies the payload CRC (revision >= 4), and ftruncates the file.
// On destruction, removes the image file unless committed.
// ----------------------------------------------------------------

class PldmStripper : public std::enable_shared_from_this<PldmStripper>
{
  public:
    PldmStripper(boost::asio::io_context& io, fs::path image_path,
                 PldmComponentInfo comp,
                 std::move_only_function<void(bool ok) const> on_complete,
                 std::move_only_function<void(uint8_t pct) const> on_progress) :
        io_(io), imagePath_(std::move(image_path)), comp_(comp),
        onComplete_(std::move(on_complete)),
        onProgress_(std::move(on_progress)),
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
        remaining_ = comp_.size;
        readPos_ = comp_.offset;
        writePos_ = 0;
        crcState_ = 0xFFFFFFFF;
        strip_next_chunk();
    }

  private:
    // post() defers execution via the io_context event loop, not recursion.
    // NOLINTNEXTLINE(misc-no-recursion)
    void strip_next_chunk()
    {
        if (remaining_ == 0)
        {
            uint32_t computed = ~crcState_;
            if (computed != comp_.payloadCrc)
            {
                std::cerr << "PLDM payload CRC mismatch: stored 0x" << std::hex
                          << comp_.payloadCrc << " computed 0x" << computed
                          << std::dec << '\n';
                finish(false);
                return;
            }
            std::cerr << "PLDM payload CRC verified OK\n";

            if (::ftruncate(fd_.get(), static_cast<off_t>(comp_.size)) != 0)
            {
                std::cerr << "ftruncate failed: " << errno << '\n';
                finish(false);
                return;
            }

            std::cerr << "PLDM header stripped, image ready (" << comp_.size
                      << " bytes)\n";
            committed_ = true;
            finish(true);
            return;
        }

        size_t to_read =
            std::min(static_cast<size_t>(remaining_), k_transfer_buf_size);
        ssize_t nread = ::pread(fd_.get(), buf_.data(), to_read, readPos_);
        if (nread < 0)
        {
            std::cerr << "pread failed during PLDM stripping: " << errno
                      << '\n';
            finish(false);
            return;
        }
        if (nread == 0)
        {
            std::cerr << "Unexpected EOF during PLDM stripping"
                         " (file truncated?)\n";
            finish(false);
            return;
        }

        crcState_ = crc32_update(crcState_,
                                 reinterpret_cast<const uint8_t*>(buf_.data()),
                                 static_cast<size_t>(nread));

        ssize_t nwritten = ::pwrite(fd_.get(), buf_.data(),
                                    static_cast<size_t>(nread), writePos_);
        if (nwritten != nread)
        {
            std::cerr << "pwrite failed during PLDM stripping: " << errno
                      << '\n';
            finish(false);
            return;
        }

        readPos_ += nread;
        writePos_ += nread;
        remaining_ -= static_cast<uint32_t>(nread);

        uint8_t pct =
            (comp_.size == 0)
                ? 100
                : static_cast<uint8_t>(100ULL * (comp_.size - remaining_) /
                                       comp_.size);
        if (pct >= lastPct_ + 20)
        {
            lastPct_ = pct;
            if (onProgress_)
            {
                onProgress_(pct);
            }
        }

        boost::asio::post(
            io_,
            [weak = weak_from_this()]() // NOLINT(misc-no-recursion)
                                        // deferred, not recursive
            {
                if (auto self = weak.lock())
                {
                    self->strip_next_chunk();
                }
            });
    }

    void finish(bool ok)
    {
        if (finished_)
        {
            return;
        }
        finished_ = true;
        auto cb = std::move(onComplete_);
        if (cb)
        {
            cb(ok);
        }
    }

    boost::asio::io_context& io_;
    fs::path imagePath_;
    PldmComponentInfo comp_;
    std::move_only_function<void(bool ok) const> onComplete_;
    std::move_only_function<void(uint8_t pct) const> onProgress_;
    UniqueFd fd_;
    std::array<char, k_transfer_buf_size> buf_{};
    uint32_t remaining_{0};
    off_t readPos_{0};
    off_t writePos_{0};
    uint32_t crcState_{0xFFFFFFFF};
    uint8_t lastPct_{0};
    bool finished_{false};
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
    if (activeTransfer_)
    {
        throw sdbusplus::exception::SdBusError(EBUSY,
                                               "Transfer already in progress");
    }
    if (activeStripper_)
    {
        throw sdbusplus::exception::SdBusError(EBUSY, "PLDM strip in progress");
    }
    if (platformCfg_.storage.vectorStoragePath.empty())
    {
        throw sdbusplus::exception::SdBusError(
            EINVAL, "vectorStoragePath not configured");
    }

    const fs::path& storage_path = platformCfg_.storage.vectorStoragePath;
    const fs::path& mount_path = platformCfg_.storage.vectorMountPath;

    // Tear down loop devices first so lazy unmounts can fully release.
    // Order matters: squashfs loop (under vectorMountPath) must go before
    // the ext4 loop (under vectorStoragePath), because the squashfs backing
    // file lives inside the ext4 mount.
    if (!mount_path.empty())
    {
        teardown_loop_devices_under(mount_path);
    }
    teardown_loop_devices_under(storage_path);

    if (!mount_path.empty())
    {
        fs::path golden_mount = mount_path / "GOLDEN_RES";
        if (umount2(golden_mount.c_str(), MNT_DETACH) != 0 && errno != EINVAL &&
            errno != ENOENT)
        {
            std::cerr << "Failed to unmount " << golden_mount << ": " << errno
                      << '\n';
        }
        if (umount2(mount_path.c_str(), MNT_DETACH) != 0 && errno != EINVAL &&
            errno != ENOENT)
        {
            std::cerr << "Failed to unmount " << mount_path << ": " << errno
                      << '\n';
        }
    }

    // Create socketpair for the FD-based transfer.
    int fds[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) != 0)
    {
        throw sdbusplus::exception::SdBusError(errno, "socketpair failed");
    }
    UniqueFd read_fd(fds[0]);
    UniqueFd write_fd(fds[1]);

    fs::path image_path = storage_path / k_image_file_name;
    auto session = std::make_shared<TransferSession>(
        io_, std::move(read_fd), image_path,
        platformCfg_.transferInactivityTimeout,
        [weak = weak_from_this(), image_path](bool ok) {
            if (auto self = weak.lock())
            {
                self->onTransferComplete(ok, image_path);
            }
        });

    publisher_->publishActivation(k_activation_activating);
    publisher_->createActivationProgress();
    publisher_->publishActivationProgress(0);

    activeTransfer_ = session;
    session->start();

    return {write_fd.release()};
}

void IstService::onTransferComplete(bool ok, const fs::path& image_path)
{
    activeTransfer_.reset();
    if (!ok)
    {
        std::cerr << "Image transfer failed\n";
        publisher_->publishActivation(k_activation_failed);
        publisher_->removeActivationProgress();
        return;
    }

    std::cout << "Image transfer succeeded, processing PLDM package\n";
    std::optional<PldmComponentInfo> comp = process_pldm_package(image_path);
    if (!comp)
    {
        std::cerr << "PLDM processing failed, removing image\n";
        std::error_code ec;
        fs::remove(image_path, ec);
        publisher_->publishActivation(k_activation_failed);
        publisher_->removeActivationProgress();
        return;
    }

    try
    {
        auto stripper = std::make_shared<PldmStripper>(
            io_, image_path, *comp,
            [weak = weak_from_this()](bool strip_ok) {
                if (auto self = weak.lock())
                {
                    self->onStripComplete(strip_ok);
                }
            },
            [weak = weak_from_this()](uint8_t pct) {
                if (auto self = weak.lock())
                {
                    self->publisher_->publishActivationProgress(pct);
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
        publisher_->publishActivation(k_activation_failed);
        publisher_->removeActivationProgress();
    }
}

void IstService::onStripComplete(bool ok)
{
    activeStripper_.reset();
    if (!ok)
    {
        std::cerr << "PLDM strip failed\n";
        publisher_->publishActivation(k_activation_failed);
        publisher_->removeActivationProgress();
        return;
    }

    std::cout << "PLDM strip succeeded\n";
    publisher_->removeActivationProgress();

    if (!mountImages())
    {
        std::cerr << "Failed to mount images after PLDM strip\n";
        publisher_->publishActivation(k_activation_failed);
        return;
    }

    publisher_->publishActivation(k_activation_active);
    readAndPublishVersion();
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

    std::optional<std::string> loop_dev =
        setup_loop_device(image_path, O_RDONLY);
    if (!loop_dev)
    {
        return false;
    }

    if (::mount(loop_dev->c_str(), mount_path.c_str(), "ext4",
                MS_RDONLY | MS_NOATIME, nullptr) != 0)
    {
        if (errno == EBUSY)
        {
            // EBUSY means mount_path is already an active mount point.
            // Detach the loop device we just created (it's unused) and
            // return success — the existing mount is what we want.
            std::cout << "Vectors already mounted at " << mount_path << '\n';
            teardown_loop_device(*loop_dev);
            return true;
        }
        std::cerr << "mount ext4 at " << mount_path << ": " << errno << '\n';
        teardown_loop_device(*loop_dev);
        return false;
    }
    std::cout << "Mounted ext4 image at " << mount_path << '\n';

    fs::path sqsh_path = mount_path / "GOLDEN_RES.sqfs";
    exists = fs::exists(sqsh_path, ec);
    if (ec)
    {
        std::cerr << "Failed to stat " << sqsh_path << ": " << ec.message()
                  << '\n';
        return false;
    }
    if (!exists)
    {
        std::cerr << "Golden results image not found: " << sqsh_path << '\n';
        return false;
    }

    fs::path golden_mount = mount_path / "GOLDEN_RES";
    fs::create_directories(golden_mount, ec);
    if (ec)
    {
        std::cerr << "Failed to create " << golden_mount << ": " << ec.message()
                  << '\n';
        return false;
    }

    std::optional<std::string> sqsh_loop =
        setup_loop_device(sqsh_path, O_RDONLY);
    if (!sqsh_loop)
    {
        std::cerr << "Failed to set up loop device for squashfs\n";
        return false;
    }

    if (::mount(sqsh_loop->c_str(), golden_mount.c_str(), "squashfs", MS_RDONLY,
                nullptr) != 0)
    {
        std::cerr << "mount squashfs at " << golden_mount << ": " << errno
                  << '\n';
        teardown_loop_device(*sqsh_loop);
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
