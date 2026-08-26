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
// File-descriptor wrapper
// ----------------

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
    int fd_ = -1;
};

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

inline bool isAllowedApplyTime(std::string_view applyTime)
{
    return applyTime == k_apply_time_immediate ||
           applyTime == k_apply_time_on_reset;
}

// ----------------
// Configuration
// ----------------

struct HookPaths
{
    std::filesystem::path istBootAssert;
    std::filesystem::path istBootDeassert;
    std::filesystem::path resetSystem;
    std::filesystem::path errorCheck;
};

struct StoragePaths
{
    std::filesystem::path vectorMountPath;
    std::filesystem::path vectorStoragePath;
    std::filesystem::path resultStoragePath;
};

struct IstPlatformConfig
{
    std::string softwareInventoryId;
    std::filesystem::path hookDir;
    std::filesystem::path itmBinaryPath{"/bin/kist_itm"};
    std::filesystem::path itmLibDir;
    std::string archSubDir{KIST_ARCH_SUBDIR};
    std::filesystem::path signingKeyPath{"/etc/ist/kist_itm_verify_key.pem"};
    HookPaths hooks;
    StoragePaths storage;
    std::chrono::seconds transferInactivityTimeout{60};
    std::chrono::seconds transferProgressInterval{120};
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
    std::optional<std::string> customTestList;
    std::optional<std::string> customSocketList;
    std::optional<int32_t> swTimeoutSec;
    std::optional<bool> continueOnFail;
    std::optional<bool> saveResOnFail;
    std::optional<bool> saveResOnPass;
    bool autoRebootOnComplete{false};
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
    collateralVerification,
    pendingIstBoot,
    pendingPowerCycle,
    runningIst,
    cleanup,
};

inline std::string istStageToString(IstStage s)
{
    switch (s)
    {
        case IstStage::idle:
            return "Idle";
        case IstStage::collateralVerification:
            return "CollateralVerification";
        case IstStage::pendingIstBoot:
            return "PendingISTBoot";
        case IstStage::pendingPowerCycle:
            return "PendingPowerCycle";
        case IstStage::runningIst:
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
    inProgress,
    completed,
    failed,
    aborted,
};

inline std::string istStatusToString(IstStatus s)
{
    switch (s)
    {
        case IstStatus::inProgress:
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
    static constexpr std::chrono::seconds defaultTimeout{120};

    virtual ~HookRunner() = default;
    virtual void asyncRun(const std::string& cmd, std::string what,
                          std::move_only_function<void(bool ok) const> done,
                          std::vector<std::string> args = {},
                          std::chrono::seconds timeout = defaultTimeout) = 0;
};

class HostPowerMonitor
{
  public:
    virtual ~HostPowerMonitor() = default;
    virtual void asyncWaitForPowerCycle(
        std::move_only_function<void(bool ok) const> done) = 0;
};

class ItmRunner
{
  public:
    virtual ~ItmRunner() = default;
    virtual void
        asyncRun(const IstTestConfig& cfg, const IstPlatformConfig& platformCfg,
                 std::move_only_function<void(int exitCode) const> done,
                 std::move_only_function<void(uint8_t) const> onProgress) = 0;
};

using ResultsFdCb = std::function<sdbusplus::message::unix_fd()>;

class StatePublisher
{
  public:
    virtual ~StatePublisher() = default;
    virtual void createRunObject(const std::string& run_path,
                                 ResultsFdCb results_fd_cb) = 0;
    virtual void removeRunObject() = 0;
    virtual void publish(const IstState& state) = 0;
    virtual void reSignalStage() = 0;
    virtual void publishProgress(uint8_t progress) = 0;
    virtual void publishVersion(const std::string& version) = 0;
    virtual void publishActivation(std::string_view state) = 0;
    virtual void createActivationProgress() = 0;
    virtual void publishActivationProgress(uint8_t progress) = 0;
    virtual void removeActivationProgress() = 0;
    virtual void emitEventLog(
        const std::string& message, const std::string& severity,
        const std::map<std::string, std::string>& additionalData) = 0;
};

// ----------------
// Factory functions
// ----------------

std::unique_ptr<HookRunner> makeHookRunner(boost::asio::io_context& io);
std::shared_ptr<HostPowerMonitor>
    makeHostPowerMonitor(boost::asio::io_context& io,
                         std::shared_ptr<sdbusplus::asio::connection> conn);
std::unique_ptr<ItmRunner> makeItmRunner(boost::asio::io_context& io);
std::unique_ptr<StatePublisher>
    makeDbusStatePublisher(sdbusplus::asio::object_server& server,
                           std::shared_ptr<sdbusplus::asio::connection> conn,
                           const std::string& swPath,
                           const std::string& istPath);

// ----------------
// IstService
//
// Must be created via IstService::create() so that async callbacks can
// safely capture shared_from_this().
// ----------------

bool parsePlatformConfig(IstPlatformConfig& out, const std::string& path);
void resolveItmPaths(IstPlatformConfig& cfg);

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
    uint32_t payloadCrc;
    // Declared payload length, or 0 if unknown.  Progress reporting only.
    uint64_t payloadSize{0};
};

class IstService : public std::enable_shared_from_this<IstService>
{
  public:
    ~IstService();

    static std::shared_ptr<IstService>
        create(boost::asio::io_context& io,
               std::unique_ptr<StatePublisher> publisher,
               std::unique_ptr<HookRunner> hookRunner,
               std::shared_ptr<HostPowerMonitor> powerMonitor,
               std::unique_ptr<ItmRunner> itmRunner);

    bool initialize(IstPlatformConfig cfg);
    void printIstPlatformConfig() const;
    const IstState& state() const
    {
        return state_;
    }
    const std::string& softwareInventoryId() const
    {
        return platformCfg_.softwareInventoryId;
    }
    const std::string& currentRunPath() const
    {
        return currentRunPath_;
    }
    /**
     * Override the function used to verify kist_itm binary and library
     * signatures before execution.  It is invoked with the platform config and
     * a completion callback that must be called once with the result.  Defaults
     * to verifyItmSignaturesAsync.  Exposed so unit tests can bypass crypto
     * without real signing keys.
     */
    using SignatureVerifier = std::function<void(
        const IstPlatformConfig&, std::move_only_function<void(bool) const>)>;
    void setSignatureVerifier(SignatureVerifier fn);

    /**
     * Async callback that discovers the CPU sockets present on the system.
     * The result is a vector of D-Bus object paths (one per CPU).
     * Paths are expected to end in CPU_0 through CPU_N-1 with no gaps.
     * The default implementation queries Entity Manager via Object Mapper.
     * Exposed so unit tests can inject a canned response.
     */
    using CpuDiscoverer = std::function<void(
        std::move_only_function<void(const std::vector<std::string>&) const>)>;
    void setCpuDiscoverer(CpuDiscoverer fn);

    void setResultsFdCallback(ResultsFdCb cb);
    std::string startIST(const ParamMap& testParams);
    std::string startUpdate(int imageFd, std::string_view applyTime);
    sdbusplus::message::unix_fd getResultsFd();

  private:
    IstService(boost::asio::io_context& io,
               std::unique_ptr<StatePublisher> publisher,
               std::unique_ptr<HookRunner> hookRunner,
               std::shared_ptr<HostPowerMonitor> powerMonitor,
               std::unique_ptr<ItmRunner> itmRunner);
    bool getISTParams(const ParamMap& testParams);
    bool collateralVerification();
    bool prepareResultStorage();

    void updateDbusState();
    void transitionTo(IstStage stage);
    void transitionTo(IstStage stage, IstStatus status,
                      std::string_view error_info = {});

    void onSignatureVerifyDone(bool ok);
    void onCpuDiscoveryDone(const std::vector<std::string>& cpuPaths);
    void launchBootAssert();
    void onIstBootAssertDone(bool ok);
    void onPowerCycleDone(bool ok);
    void startItmRun();
    void runIstCleanup(int itmExit);
    void onArchiveDone(int itmExit);
    void emitIstEventLog(int itmExit);
    void onDeassertDone(int itmExit, bool okDeassert);
    void onResetDone(int itmExit, bool okReset);

    boost::asio::io_context& io_;
    std::unique_ptr<StatePublisher> publisher_;

    IstPlatformConfig platformCfg_;
    IstTestConfig test_;
    IstState state_;
    bool initialized_{false};
    uint64_t runCounter_{0};
    std::string currentRunPath_;
    ResultsFdCb resultsFdCb_;

    std::unique_ptr<HookRunner> hookRunner_;
    std::shared_ptr<HostPowerMonitor> powerMonitor_;
    std::unique_ptr<ItmRunner> itmRunner_;
    SignatureVerifier signatureVerifier_;
    CpuDiscoverer cpuDiscoverer_;
    std::shared_ptr<VectorManager> vectorManager_;
    boost::asio::steady_timer reSignalTimer_;

    std::vector<std::string> failureInfo_;

    UniqueFd serviceLogFd_;
    std::unique_ptr<TeeStreambuf> coutTee_;
    std::unique_ptr<TeeStreambuf> cerrTee_;
    std::streambuf* origCoutBuf_{nullptr};
    std::streambuf* origCerrBuf_{nullptr};

    void installServiceLogTee();
    void removeServiceLogTee();
};
