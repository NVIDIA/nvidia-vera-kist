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

#include <sdbusplus/asio/object_server.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>

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
    HookPaths hooks;
    StoragePaths storage;
    std::chrono::seconds transferInactivityTimeout{300};
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

class StatePublisher
{
  public:
    virtual ~StatePublisher() = default;
    virtual void createRunObject(const std::string& run_path) = 0;
    virtual void removeRunObject() = 0;
    virtual void publish(const IstState& state) = 0;
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

bool archiveResults(const std::filesystem::path& resultsDir);

struct PldmComponentInfo;
class TransferSession;
class PldmStripper;

struct PldmComponentInfo
{
    uint32_t offset;
    uint32_t size;
    uint32_t payloadCrc;
};

class IstService : public std::enable_shared_from_this<IstService>
{
  public:
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
    std::string startIST(const ParamMap& testParams);
    sdbusplus::message::unix_fd startUpdate();
    sdbusplus::message::unix_fd getResultsFd();

  private:
    IstService(boost::asio::io_context& io,
               std::unique_ptr<StatePublisher> publisher,
               std::unique_ptr<HookRunner> hookRunner,
               std::shared_ptr<HostPowerMonitor> powerMonitor,
               std::unique_ptr<ItmRunner> itmRunner);
    bool getISTParams(const ParamMap& testParams);
    bool collateralVerification(const ParamMap& testParams);

    void updateDbusState();
    void transitionTo(IstStage stage);
    void transitionTo(IstStage stage, IstStatus status);

    void onIstBootAssertDone(bool ok);
    void onPowerCycleDone(bool ok);
    void startItmRun();
    void runIstCleanup(int itmExit);
    void onArchiveDone(int itmExit);
    void emitIstEventLog(int itmExit);
    void onDeassertDone(int itmExit, bool okDeassert);
    void onResetDone(int itmExit, bool okReset);

    void onTransferComplete(bool ok, const std::filesystem::path& imagePath);
    void onPldmParseComplete(
        bool ok, const std::shared_ptr<std::optional<PldmComponentInfo>>& comp,
        const std::filesystem::path& imagePath);
    void onStripComplete(bool ok);
    void onTeardownComplete(bool ok, const std::shared_ptr<UniqueFd>& readFd,
                            const std::filesystem::path& imagePath);
    void onMountComplete(bool ok);
    bool mountImages();
    bool teardownMounts();
    void asyncMountImages(std::move_only_function<void(bool ok) const> done);
    void asyncTeardownMounts(std::move_only_function<void(bool ok) const> done);
    void ensureMounted();
    void readAndPublishVersion();
    void finishUpdate(bool ok);

    boost::asio::io_context& io_;
    std::unique_ptr<StatePublisher> publisher_;

    IstPlatformConfig platformCfg_;
    IstTestConfig test_;
    IstState state_;
    bool initialized_{false};
    bool updateInProgress_{false};
    uint64_t runCounter_{0};
    std::string currentRunPath_;

    std::unique_ptr<HookRunner> hookRunner_;
    std::shared_ptr<HostPowerMonitor> powerMonitor_;
    std::unique_ptr<ItmRunner> itmRunner_;
    std::shared_ptr<TransferSession> activeTransfer_;
    std::shared_ptr<PldmStripper> activeStripper_;
};
