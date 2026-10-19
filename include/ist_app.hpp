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

#include <unistd.h>

#include <boost/asio/steady_timer.hpp>
#include <ist_logging.hpp>
#include <sdbusplus/asio/object_server.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <streambuf>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

// ----------------
// Tee streambuf — duplicates writes to a log file while forwarding to the
// original streambuf (typically the journal fd under systemd).
// ----------------

class TeeStreambuf : public std::streambuf
{
  public:
    TeeStreambuf(std::streambuf* primary, int log_fd) :
        primary_(primary), log_fd_(log_fd)
    {}

  protected:
    // Single-char path; bulk writes go through xsputn.
    int overflow(int c) override
    {
        if (c == EOF)
        {
            return EOF;
        }
        char ch = static_cast<char>(c);
        if (primary_->sputc(ch) == EOF)
        {
            return EOF;
        }
        // Log file write is best-effort — don't fail the stream if it errors.
        std::ignore = ::write(log_fd_, &ch, 1);
        return c;
    }

    std::streamsize xsputn(const char* s, std::streamsize n) override
    {
        std::streamsize primary_written = primary_->sputn(s, n);
        // Best-effort write to log file regardless of primary result.
        const char* p = s;
        std::streamsize remaining = n;
        while (remaining > 0)
        {
            ssize_t w = ::write(log_fd_, p, static_cast<size_t>(remaining));
            if (w <= 0)
            {
                break;
            }
            p += w;
            remaining -= w;
        }
        return primary_written;
    }

  private:
    std::streambuf* primary_;
    int log_fd_;
};

// ----------------
// Type aliases
// ----------------

using IstParamVariant = std::variant<std::string, bool, int>;
using ParamMap = std::unordered_map<std::string, IstParamVariant>;

// ----------------
// ITM exit codes and error-marker path
// ----------------

inline constexpr char err_marker_dir[] = "/tmp/ist/err_marker";
inline constexpr int itm_exit_mismatch = 8;
inline constexpr int itm_exit_platform_error = 14;
inline constexpr int itm_exit_sw_timeout = -2;

// ----------------
// Software activation states (D-Bus enum values)
// ----------------

inline constexpr std::string_view k_activation_activating =
    "xyz.openbmc_project.Software.Activation.Activations.Activating";
inline constexpr std::string_view k_activation_active =
    "xyz.openbmc_project.Software.Activation.Activations.Active";
inline constexpr std::string_view k_activation_failed =
    "xyz.openbmc_project.Software.Activation.Activations.Failed";

inline constexpr std::string_view k_apply_time_immediate =
    "xyz.openbmc_project.Software.ApplyTime.RequestedApplyTimes.Immediate";
inline constexpr std::string_view k_apply_time_on_reset =
    "xyz.openbmc_project.Software.ApplyTime.RequestedApplyTimes.OnReset";

inline bool is_allowed_apply_time(std::string_view apply_time)
{
    return apply_time == k_apply_time_immediate ||
           apply_time == k_apply_time_on_reset;
}

// ----------------
// Configuration
// ----------------

struct HookPaths
{
    std::filesystem::path ist_boot_assert;
    std::filesystem::path ist_boot_deassert;
    std::filesystem::path reset_system;
    std::filesystem::path error_check;
};

struct StoragePaths
{
    std::filesystem::path vector_mount_path;
    std::filesystem::path vector_storage_path;
    std::filesystem::path result_storage_path;
};

struct IstPlatformConfig
{
    std::string software_inventory_id;
    std::filesystem::path hook_dir;
    std::filesystem::path itm_binary_path{"/bin/kist_itm"};
    std::filesystem::path itm_lib_dir;
    std::string arch_sub_dir{KIST_ARCH_SUBDIR};
    std::filesystem::path signing_key_path{
        "/usr/share/ist/kist_itm_verify_key.pem"};
    HookPaths hooks;
    StoragePaths storage;
    std::chrono::seconds transfer_inactivity_timeout{60};
    std::chrono::seconds transfer_progress_interval{120};
};

/**
 * Test configuration for the IST service.
 * These options come in via dbus.
 *
 * customTestList: A list of custom test names to run.
 * customSocketList: A list of custom socket names to run.
 * swTimeoutSec: The timeout in seconds for the IST run.
 * continueOnFail: Whether to continue on fail.
 * saveResOnFail: Whether to save results on fail.
 * saveResOnPass: Whether to save results on pass.
 * autoRebootOnComplete: Whether to auto reboot on complete.
 */
struct IstTestConfig
{
    std::optional<std::string> custom_test_list;
    std::optional<std::string> custom_socket_list;
    std::optional<int32_t> sw_timeout_sec;
    std::optional<bool> continue_on_fail;
    std::optional<bool> save_res_on_fail;
    std::optional<bool> save_res_on_pass;
    bool auto_reboot_on_complete{false};
};

// ----------------
// State enums
// ----------------

/**
 * The stages of the IST service.
 *
 * idle: The service is idle.
 * collateralVerification: The service is performing collateral verification.
 * pendingIstBoot: The service is waiting for the IST boot.
 * pendingPowerCycle: The service is waiting for the power cycle.
 * runningIst: The service is running the IST.
 * cleanup: The service is cleaning up after the IST.
 */
enum class IstStage
{
    idle,
    collateral_verification,
    pending_ist_boot,
    pending_power_cycle,
    running_ist,
    cleanup,
};

inline std::string ist_stage_to_string(IstStage s)
{
    switch (s)
    {
        case IstStage::idle:
            return "Idle";
        case IstStage::collateral_verification:
            return "CollateralVerification";
        case IstStage::pending_ist_boot:
            return "PendingISTBoot";
        case IstStage::pending_power_cycle:
            return "PendingPowerCycle";
        case IstStage::running_ist:
            return "RunningIST";
        case IstStage::cleanup:
            return "Cleanup";
        default:
            return "Unknown";
    }
}

/**
 * The status of the IST run.
 *
 * inProgress: The test is in progress.
 * completed: The test has completed.
 * failed: The test has failed.
 * aborted: The test has been aborted.
 */
enum class IstStatus
{
    in_progress,
    completed,
    failed,
    aborted,
};

inline std::string ist_status_to_string(IstStatus s)
{
    switch (s)
    {
        case IstStatus::in_progress:
            return "InProgress";
        case IstStatus::completed:
            return "Completed";
        case IstStatus::failed:
            return "Failed";
        case IstStatus::aborted:
            return "Aborted";
    }
    return "Unknown";
}

/**
 * The state of the IST run.
 *
 * progress: The progress of the run.
 * status: The status of the run.
 * stage: The stage of the run.
 */
struct IstState
{
    uint8_t progress{0};
    IstStatus status{IstStatus::completed};
    IstStage stage{IstStage::idle};
};

// ----------------
// Interfaces
// ----------------

class HookRunner
{
  public:
    static constexpr std::chrono::seconds k_default_timeout{120};

    // The reset hooks wait for the CPU MCTP endpoints and reset again if they
    // do not come back, so they need room for two resets and two waits: about
    // 30s per reset of both boards, plus IST_EID_TIMEOUT_S (45s) each time,
    // so 150s.
    //
    // The CAK bypass hook does that same recovery, which costs it a wait more
    // than the reset hooks because it starts with one: 120s before it even
    // begins waiting on IST_CAK_TIMEOUT_S. That wait can then overrun its own
    // 120s by one round of Bypass calls, since it only checks the deadline
    // between iterations. Budget 300s of hook.
    //
    // These are deliberately not the default, because a hook that resets
    // nothing but hangs for minutes should still be caught.
    static constexpr std::chrono::seconds k_reset_system_timeout{240};
    static constexpr std::chrono::seconds k_cak_bypass_timeout{360};

    virtual ~HookRunner() = default;

    // The run log belongs to the service, which does not exist yet when the
    // runner is constructed.
    virtual void set_log_file(LogFile& log) = 0;
    virtual void
        async_run(const std::string& cmd, std::string what,
                  std::move_only_function<void(bool ok) const> done,
                  std::vector<std::string> args = {},
                  std::chrono::seconds timeout = k_default_timeout) = 0;
};

class HostPowerMonitor
{
  public:
    virtual ~HostPowerMonitor() = default;
    virtual void async_wait_for_power_cycle(
        std::move_only_function<void(bool ok) const> done) = 0;
};

class ItmRunner
{
  public:
    virtual ~ItmRunner() = default;
    virtual void
        async_run(const IstTestConfig& cfg,
                  const IstPlatformConfig& platform_cfg,
                  std::move_only_function<void(int exit_code) const> done,
                  std::move_only_function<void(uint8_t) const> on_progress) = 0;
};

using ResultsFdCb = std::function<sdbusplus::message::unix_fd()>;

class StatePublisher
{
  public:
    virtual ~StatePublisher() = default;
    virtual void create_run_object(const std::string& run_path,
                                   ResultsFdCb results_fd_cb) = 0;
    virtual void remove_run_object() = 0;
    virtual void publish(const IstState& state) = 0;
    virtual void re_signal_stage() = 0;
    virtual void publish_progress(uint8_t progress) = 0;
    virtual void publish_version(const std::string& version) = 0;
    virtual void publish_activation(std::string_view state) = 0;
    virtual void create_activation_progress() = 0;
    virtual void publish_activation_progress(uint8_t progress) = 0;
    virtual void remove_activation_progress() = 0;
    virtual void emit_event_log(
        const std::string& message, const std::string& severity,
        const std::map<std::string, std::string>& additional_data) = 0;
};

// ----------------
// Factory functions
// ----------------

std::unique_ptr<HookRunner> make_hook_runner(boost::asio::io_context& io);
std::shared_ptr<HostPowerMonitor>
    make_host_power_monitor(boost::asio::io_context& io,
                            std::shared_ptr<sdbusplus::asio::connection> conn);
std::unique_ptr<ItmRunner> make_itm_runner(boost::asio::io_context& io);
std::unique_ptr<StatePublisher>
    make_dbus_state_publisher(sdbusplus::asio::object_server& server,
                              std::shared_ptr<sdbusplus::asio::connection> conn,
                              const std::string& sw_path,
                              const std::string& ist_path);

// ----------------
// IstService
//
// Must be created via IstService::create() so that async callbacks can
// safely capture shared_from_this().
// ----------------

inline constexpr std::string_view trusted_hook_root = "/usr/share/ist/hooks";

bool parse_platform_config(IstPlatformConfig& out, const std::string& path,
                           std::string_view hook_root = trusted_hook_root);
void resolve_itm_paths(IstPlatformConfig& cfg);

struct PldmComponentInfo;
class TransferSession;
class PldmHeaderPeekSession;
class VectorManager;

struct PldmComponentInfo
{
    // Offset of the component payload within the PLDM package, which equals
    // the PLDM package header size.  The header is never written to disk; the
    // payload is streamed straight to the image file starting at offset 0.
    uint32_t offset;
    // Stored CRC-32 of the component payload, taken from the package header.
    uint32_t payload_crc;
    // Declared payload length, or 0 if unknown.  Progress reporting only.
    uint64_t payload_size{0};
};

class IstService : public std::enable_shared_from_this<IstService>
{
  public:
    ~IstService();

    static std::shared_ptr<IstService>
        create(boost::asio::io_context& io,
               std::unique_ptr<StatePublisher> publisher,
               std::unique_ptr<HookRunner> hook_runner,
               std::shared_ptr<HostPowerMonitor> power_monitor,
               std::unique_ptr<ItmRunner> itm_runner);

    bool initialize(IstPlatformConfig cfg);
    void print_ist_platform_config() const;
    const IstState& state() const
    {
        return state_;
    }
    const std::string& software_inventory_id() const
    {
        return platform_cfg_.software_inventory_id;
    }
    const std::string& current_run_path() const
    {
        return current_run_path_;
    }
    /**
     * Override the function used to verify kist_itm binary and library
     * signatures before execution.  It is invoked with the platform config and
     * a completion callback that must be called once with the result.  Defaults
     * to verify_itm_signatures_async.  Exposed so unit tests can bypass crypto
     * without real signing keys.
     */
    using SignatureVerifier = std::function<void(
        const IstPlatformConfig&, std::move_only_function<void(bool) const>)>;
    void set_signature_verifier(SignatureVerifier fn);

    /**
     * Async callback that discovers the CPU sockets present on the system.
     * The result is a vector of D-Bus object paths (one per CPU).
     * Paths are expected to end in CPU_0 through CPU_N-1 with no gaps.
     * The default implementation queries Entity Manager via Object Mapper.
     * Exposed so unit tests can inject a canned response.
     */
    using CpuDiscoverer = std::function<void(
        std::move_only_function<void(const std::vector<std::string>&) const>)>;
    void set_cpu_discoverer(CpuDiscoverer fn);

    void set_results_fd_callback(ResultsFdCb cb);
    std::string start_ist(const ParamMap& test_params);
    std::string start_update(int image_fd, std::string_view apply_time);
    sdbusplus::message::unix_fd get_results_fd();

  private:
    IstService(boost::asio::io_context& io,
               std::unique_ptr<StatePublisher> publisher,
               std::unique_ptr<HookRunner> hook_runner,
               std::shared_ptr<HostPowerMonitor> power_monitor,
               std::unique_ptr<ItmRunner> itm_runner);
    bool get_ist_params(const ParamMap& test_params);
    bool collateral_verification();
    bool prepare_result_storage();

    void update_dbus_state();
    void transition_to(IstStage stage);
    void transition_to(IstStage stage, IstStatus status,
                       std::string_view error_info = {});

    void on_signature_verify_done(bool ok);
    void on_cpu_discovery_done(const std::vector<std::string>& cpu_paths);
    void launch_boot_assert();
    void on_ist_boot_assert_done(bool ok);
    void on_power_cycle_done(bool ok);
    void start_itm_run();
    void run_ist_cleanup(int itm_exit);
    void on_archive_done(int itm_exit, bool ok_deassert);
    void emit_ist_event_log(int itm_exit);
    void on_deassert_done(int itm_exit, bool ok_deassert);
    void on_reset_done(int itm_exit, bool ok_reset);
    void fail_run_with_archive(std::string_view error_info);

    boost::asio::io_context& io_;
    std::unique_ptr<StatePublisher> publisher_;

    IstPlatformConfig platform_cfg_;
    IstTestConfig test_;
    IstState state_;
    bool initialized_{false};
    uint64_t run_counter_{0};
    std::string current_run_path_;
    ResultsFdCb results_fd_cb_;

    std::unique_ptr<HookRunner> hook_runner_;
    std::shared_ptr<HostPowerMonitor> power_monitor_;
    std::unique_ptr<ItmRunner> itm_runner_;
    SignatureVerifier signature_verifier_;
    CpuDiscoverer cpu_discoverer_;
    std::shared_ptr<VectorManager> vector_manager_;
    boost::asio::steady_timer re_signal_timer_;

    std::vector<std::string> failure_info_;

    RunLogs run_logs_;
    std::unique_ptr<TeeStreambuf> cout_tee_;
    std::unique_ptr<TeeStreambuf> cerr_tee_;
    std::streambuf* orig_cout_buf_{nullptr};
    std::streambuf* orig_cerr_buf_{nullptr};

    void install_service_log_tee();
    void remove_service_log_tee();
};
