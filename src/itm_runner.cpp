#include <boost/asio/steady_timer.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/stdio.hpp>
#include <ist_app.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace bpv2 = boost::process::v2;
namespace fs = std::filesystem;

// Lifetime note: ProgressPoller is stopped in the proc->async_wait
// callback which fires before io.run() returns.
class ProgressPoller final : public std::enable_shared_from_this<ProgressPoller>
{
  public:
    ProgressPoller(boost::asio::io_context& io, std::string path,
                   std::move_only_function<void(uint8_t) const> on_progress) :
        timer_(io), path_(std::move(path)), onProgress_(std::move(on_progress))
    {}

    ~ProgressPoller()
    {
        stopped_ = true;
        timer_.cancel();
    }

    void start()
    {
        poll();
    }
    void stop()
    {
        stopped_ = true;
        timer_.cancel();
        read_progress();
    }

  private:
    // Progress file format: "<completed> <total>\n" (two decimal integers).
    void read_progress()
    {
        std::ifstream input_file(path_);
        if (!input_file.is_open())
        {
            return; // File not yet created; normal during early IST stages
        }

        std::string line;
        if (!std::getline(input_file, line))
        {
            std::cerr << "Failed to read progress file '" << path_ << "'\n";
            return;
        }

        int completed = 0;
        int total = 0;
        try
        {
            std::size_t pos = 0;
            completed = std::stoi(line, &pos);
            total = std::stoi(line.substr(pos));
        }
        catch (const std::exception& e)
        {
            std::cerr << "Failed to parse progress file '" << path_
                      << "': " << e.what() << '\n';
            return;
        }

        if (total <= 0)
        {
            std::cerr << "Invalid total in progress file: " << total << '\n';
            return;
        }

        int pct =
            std::clamp(static_cast<int>(100.0 * completed / total), 0, 100);
        uint8_t new_progress = static_cast<uint8_t>(pct);
        if (new_progress != lastProgress_)
        {
            lastProgress_ = new_progress;
            onProgress_(new_progress);
        }
    }

    void poll()
    {
        using namespace std::chrono_literals;
        if (stopped_)
        {
            return; // Poller was stopped; normal shutdown path
        }

        read_progress();

        timer_.expires_after(1s);
        timer_.async_wait(
            [weak = weak_from_this()](const boost::system::error_code& ec) {
                if (ec) // Timer cancelled — poller stopped
                {
                    return;
                }
                std::shared_ptr<ProgressPoller> self = weak.lock();
                if (!self)
                {
                    return; // Poller was destroyed; normal during shutdown
                }
                self->poll();
            });
    }

    boost::asio::steady_timer timer_;
    std::string path_;
    std::move_only_function<void(uint8_t) const> onProgress_;
    uint8_t lastProgress_{0};
    bool stopped_ = false;
};

class ItmProcess final : public std::enable_shared_from_this<ItmProcess>
{
  public:
    ItmProcess(boost::asio::io_context& io,
               std::move_only_function<void(int) const> done) :
        deadline_(io), done_(std::move(done))
    {}

    void start(std::shared_ptr<bpv2::process> proc,
               std::shared_ptr<ProgressPoller> poller, int timeout_sec);

  private:
    void on_deadline_expired(int timeout_sec);
    void on_process_exit(const boost::system::error_code& ec, int exit_code);

    std::shared_ptr<bpv2::process> proc_;
    std::shared_ptr<ProgressPoller> poller_;
    boost::asio::steady_timer deadline_;
    std::move_only_function<void(int) const> done_;
    bool timedOut_{false};
};

void ItmProcess::start(std::shared_ptr<bpv2::process> proc,
                       std::shared_ptr<ProgressPoller> poller, int timeout_sec)
{
    proc_ = std::move(proc);
    poller_ = std::move(poller);

    deadline_.expires_after(std::chrono::seconds(timeout_sec));
    deadline_.async_wait([weak = weak_from_this(),
                          timeout_sec](const boost::system::error_code& ec) {
        if (ec) // Timer cancelled — process exited before deadline
        {
            return;
        }
        std::shared_ptr<ItmProcess> self = weak.lock();
        if (!self)
        {
            return; // ItmProcess was destroyed; normal during shutdown
        }
        self->on_deadline_expired(timeout_sec);
    });

    proc_->async_wait([weak = weak_from_this()](
                          const boost::system::error_code& ec, int exit_code) {
        std::shared_ptr<ItmProcess> self = weak.lock();
        if (!self)
        {
            return; // ItmProcess was destroyed; normal during shutdown
        }
        self->on_process_exit(ec, exit_code);
    });
}

void ItmProcess::on_deadline_expired(int timeout_sec)
{
    if (!proc_)
    {
        return; // Process already cleaned up after exit
    }
    std::cerr << "kist_itm exceeded SW timeout (" << timeout_sec
              << "s), terminating\n";
    timedOut_ = true;
    proc_->terminate();
}

void ItmProcess::on_process_exit(const boost::system::error_code& ec,
                                 int exit_code)
{
    if (poller_)
    {
        poller_->stop();
        poller_.reset();
    }
    deadline_.cancel();
    proc_.reset();

    if (timedOut_)
    {
        std::cerr << "kist_itm killed due to SW timeout\n";
        done_(-1);
        return;
    }
    if (ec)
    {
        std::cerr << "kist_itm wait failed: ec=" << ec.message()
                  << " exit=" << exit_code << '\n';
        done_(-1);
        return;
    }
    if (exit_code != 0)
    {
        std::cerr << "kist_itm exited with code " << exit_code << '\n';
    }
    done_(exit_code);
}

class ItmRunnerImpl final : public ItmRunner
{
  public:
    explicit ItmRunnerImpl(boost::asio::io_context& io) : io_(io)
    {}
    void asyncRun(
        const IstTestConfig& cfg, const IstPlatformConfig& platform_cfg,
        std::move_only_function<void(int) const> done,
        std::move_only_function<void(uint8_t) const> on_progress) override;

  private:
    boost::asio::io_context& io_;
    std::shared_ptr<ItmProcess> active_;
};

// ---------------------
// ITM process helpers
// ---------------------

static std::vector<std::string>
    build_itm_args(const IstTestConfig& cfg,
                   const IstPlatformConfig& platform_cfg)
{
    std::vector<std::string> args;
    args.emplace_back(platform_cfg.itmBinaryPath);

    if (!platform_cfg.storage.vectorMountPath.empty())
    {
        args.emplace_back("--ist_package_path");
        args.emplace_back(platform_cfg.storage.vectorMountPath);
    }
    if (!platform_cfg.storage.resultStoragePath.empty())
    {
        args.emplace_back("--ist_results_path");
        args.emplace_back(platform_cfg.storage.resultStoragePath);
    }
    if (!platform_cfg.hookDir.empty())
    {
        args.emplace_back("--ist_hook_path");
        args.emplace_back(platform_cfg.hookDir);
    }

    // Optional test params
    if (cfg.customTestList)
    {
        args.emplace_back("--custom_test_list");
        args.emplace_back(*cfg.customTestList);
    }
    if (cfg.customSocketList)
    {
        args.emplace_back("--custom_socket_list");
        args.emplace_back(*cfg.customSocketList);
    }
    if (cfg.continueOnFail)
    {
        args.emplace_back("--ist_continueonfail");
        args.emplace_back(*cfg.continueOnFail ? "yes" : "no");
    }
    if (cfg.saveResOnPass)
    {
        args.emplace_back("--ist_save_res_on_pass");
        args.emplace_back(*cfg.saveResOnPass ? "enable" : "disable");
    }
    if (cfg.saveResOnFail)
    {
        args.emplace_back("--ist_save_res_on_fail");
        args.emplace_back(*cfg.saveResOnFail ? "enable" : "disable");
    }

    return args;
}

void ItmRunnerImpl::asyncRun(
    const IstTestConfig& cfg, const IstPlatformConfig& platform_cfg,
    std::move_only_function<void(int) const> done,
    std::move_only_function<void(uint8_t) const> on_progress)
{
    if (platform_cfg.storage.resultStoragePath.empty())
    {
        std::cerr << "resultStoragePath not found in platform config\n";
        done(-1);
        return;
    }
    fs::path progress_path =
        platform_cfg.storage.resultStoragePath / "progress.txt";

    // Remove stale progress file from a previous run so the poller
    // doesn't read old data and immediately report 100%.
    std::error_code rm_ec;
    fs::remove(progress_path, rm_ec); // May not exist yet; failure is harmless

    std::vector<std::string> args = build_itm_args(cfg, platform_cfg);

    std::shared_ptr<bpv2::process> proc;
    try
    {
        proc = std::make_shared<bpv2::process>(
            io_, args[0],
            std::vector<std::string>(args.begin() + 1, args.end()),
            bpv2::process_stdio{.in = nullptr, .out = stdout, .err = stderr});
    }
    catch (const std::exception& e)
    {
        std::cerr << "Failed to execute kist_itm: " << e.what() << '\n';
        done(-1);
        return;
    }

    std::shared_ptr<ProgressPoller> poller = std::make_shared<ProgressPoller>(
        io_, progress_path, std::move(on_progress));
    poller->start();

    int timeout_sec = cfg.swTimeoutSec.value_or(15 * 60);

    active_ = std::make_shared<ItmProcess>(io_, std::move(done));
    active_->start(std::move(proc), std::move(poller), timeout_sec);
}

std::unique_ptr<ItmRunner> makeItmRunner(boost::asio::io_context& io)
{
    return std::make_unique<ItmRunnerImpl>(io);
}
