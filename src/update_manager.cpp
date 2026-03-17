#include <fcntl.h>
#include <linux/loop.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <unistd.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/asio/steady_timer.hpp>
#include <ist_app.hpp>
#include <sdbusplus/exception.hpp>

#include <array>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

static constexpr std::string_view k_image_file_name = "CPU-IST.img";
static constexpr std::string_view k_golden_res_file_name = "GOLDEN_RES.sqfs";
static constexpr size_t k_transfer_buf_size = 65536;

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
        [weak = weak_from_this()](bool ok) {
            if (auto self = weak.lock())
            {
                self->activeTransfer_.reset();
                if (ok)
                {
                    std::cout << "Image transfer succeeded\n";
                }
                else
                {
                    std::cerr << "Image transfer failed\n";
                }
            }
        });

    activeTransfer_ = session;
    session->start();

    return {write_fd.release()};
}
