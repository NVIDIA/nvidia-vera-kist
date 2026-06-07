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

#include <ist_app.hpp>
#include <ist_errors.hpp>
#include <ist_results.hpp>
#include <nlohmann/json.hpp>
#include <sdbusplus/exception.hpp>
#include <signature_verify.hpp>
#include <vector_manager.hpp>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>

namespace fs = std::filesystem;
namespace ist_err = sdbusplus::error::com::nvidia::vera::ist;
using json = nlohmann::json;

static constexpr size_t max_param_string_length = 4096;

static constexpr int32_t min_sw_timeout_sec = 60;
static constexpr int32_t max_sw_timeout_sec = 7200;

// ---------------------
// Static helpers
// ---------------------

static fs::path json_path(const json& obj, const char* key)
{
    if (obj.contains(key) && obj[key].is_string())
    {
        return obj[key].get<std::string>();
    }
    return {};
}

// Returns true if child path is under parent after resolving symlinks.
// Guards against ".." traversal and symlink escapes.
// Both paths must exist on disk; returns false if either does not.
static bool is_path_within(const fs::path& child, const fs::path& parent)
{
    std::error_code ec;
    fs::path canonical_child = fs::canonical(child, ec);
    if (ec)
    {
        std::cerr << "is_path_within: cannot resolve child path '"
                  << child.string() << "': " << ec.message() << '\n';
        return false;
    }
    fs::path canonical_parent = fs::canonical(parent, ec);
    if (ec)
    {
        std::cerr << "is_path_within: cannot resolve parent path '"
                  << parent.string() << "': " << ec.message() << '\n';
        return false;
    }

    // Check that the resolved child starts with the resolved parent
    fs::path rel = canonical_child.lexically_relative(canonical_parent);
    return !rel.empty() && *rel.begin() != "..";
}

// ----------------
// IstService impl
// ----------------

std::shared_ptr<IstService>
    IstService::create(boost::asio::io_context& io,
                       std::unique_ptr<StatePublisher> publisher,
                       std::unique_ptr<HookRunner> hook_runner,
                       std::shared_ptr<HostPowerMonitor> power_monitor,
                       std::unique_ptr<ItmRunner> itm_runner)
{
    return std::shared_ptr<IstService>(
        new IstService(io, std::move(publisher), std::move(hook_runner),
                       std::move(power_monitor), std::move(itm_runner)));
}

IstService::IstService(boost::asio::io_context& io,
                       std::unique_ptr<StatePublisher> publisher,
                       std::unique_ptr<HookRunner> hook_runner,
                       std::shared_ptr<HostPowerMonitor> power_monitor,
                       std::unique_ptr<ItmRunner> itm_runner) :
    io_(io), publisher_(std::move(publisher)),
    hookRunner_(std::move(hook_runner)),
    powerMonitor_(std::move(power_monitor)), itmRunner_(std::move(itm_runner)),
    signatureVerifier_([&io](const IstPlatformConfig& cfg,
                             std::move_only_function<void(bool) const> cb) {
        verifyItmSignaturesAsync(io, cfg, std::move(cb));
    }),
    vectorManager_(VectorManager::create(io, *publisher_, platformCfg_)),
    reSignalTimer_(io)
{}

IstService::~IstService()
{
    removeServiceLogTee();
}

void IstService::setSignatureVerifier(SignatureVerifier fn)
{
    signatureVerifier_ = std::move(fn);
}

void IstService::setCpuDiscoverer(CpuDiscoverer fn)
{
    cpuDiscoverer_ = std::move(fn);
}

void IstService::setResultsFdCallback(ResultsFdCb cb)
{
    resultsFdCb_ = std::move(cb);
}

// ----------------
// D-Bus state helpers
// ----------------

void IstService::updateDbusState()
{
    publisher_->publish(state_);
}

void IstService::transitionTo(IstStage stage)
{
    reSignalTimer_.cancel();
    state_.stage = stage;
    updateDbusState();
}

void IstService::transitionTo(IstStage stage, IstStatus status,
                              std::string_view error_info)
{
    reSignalTimer_.cancel();
    state_.stage = stage;
    state_.status = status;
    if (stage == IstStage::idle)
    {
        if (status != IstStatus::completed && !error_info.empty())
        {
            std::cerr << "IST_ERROR: " << error_info << '\n';
        }
        removeServiceLogTee();
    }
    updateDbusState();
}

void IstService::installServiceLogTee()
{
    if (origCoutBuf_)
    {
        return;
    }

    fs::path log_path =
        platformCfg_.storage.resultStoragePath / "IST_service.log";
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    serviceLogFd_ = UniqueFd(::open(
        log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644));
    if (serviceLogFd_.get() < 0)
    {
        std::cerr << "Failed to open service log '" << log_path
                  << "': errno=" << errno << '\n';
        return;
    }

    coutTee_ =
        std::make_unique<TeeStreambuf>(std::cout.rdbuf(), serviceLogFd_.get());
    cerrTee_ =
        std::make_unique<TeeStreambuf>(std::cerr.rdbuf(), serviceLogFd_.get());
    origCoutBuf_ = std::cout.rdbuf(coutTee_.get());
    origCerrBuf_ = std::cerr.rdbuf(cerrTee_.get());
}

void IstService::removeServiceLogTee()
{
    if (origCoutBuf_)
    {
        std::cout.rdbuf(origCoutBuf_);
        origCoutBuf_ = nullptr;
    }
    if (origCerrBuf_)
    {
        std::cerr.rdbuf(origCerrBuf_);
        origCerrBuf_ = nullptr;
    }
    coutTee_.reset();
    cerrTee_.reset();
    serviceLogFd_ = UniqueFd();
}

// ----------------
// Platform config
// ----------------

void resolveItmPaths(IstPlatformConfig& cfg)
{
    if (cfg.storage.vectorMountPath.empty())
    {
        return;
    }

    if (!cfg.archSubDir.empty())
    {
        fs::path arch_dir =
            cfg.storage.vectorMountPath / "kist_itm" / cfg.archSubDir;
        std::error_code ec;
        if (fs::is_directory(arch_dir, ec) && !ec)
        {
            cfg.itmBinaryPath = arch_dir / "bin" / "kist_itm";
            cfg.itmLibDir = arch_dir / "lib";
            return;
        }
    }

    cfg.itmBinaryPath = cfg.storage.vectorMountPath / "kist_itm";
    cfg.itmLibDir.clear();
}

bool parsePlatformConfig(IstPlatformConfig& out, const std::string& path)
{
    std::ifstream f(path);
    if (!f)
    {
        std::cerr << "Failed to open " << path << '\n';
        return false;
    }

    json d;
    try
    {
        d = json::parse(f);
    }
    catch (const std::exception& e)
    {
        std::cerr << "Failed to parse JSON: " << e.what() << '\n';
        return false;
    }

    if (!d.contains("hookDirectory") || !d["hookDirectory"].is_string())
    {
        std::cerr << "hookDirectory missing or not a string\n";
        return false;
    }
    out.hookDir = d["hookDirectory"].get<std::string>();

    std::error_code fs_ec;
    if (!fs::is_directory(out.hookDir, fs_ec) || fs_ec)
    {
        std::cerr << "hookDirectory does not exist or is not a directory: "
                  << out.hookDir << '\n';
        return false;
    }

    if (!d.contains("softwareInventoryId") ||
        !d["softwareInventoryId"].is_string())
    {
        std::cerr << "softwareInventoryId is missing or not a string\n";
        return false;
    }
    std::string id = d["softwareInventoryId"].get<std::string>();
    if (id.empty() || std::any_of(id.begin(), id.end(), [](char c) {
            return std::isalnum(static_cast<unsigned char>(c)) == 0 && c != '_';
        }))
    {
        std::cerr << "softwareInventoryId contains invalid characters: " << id
                  << "\n";
        return false;
    }
    out.softwareInventoryId = std::move(id);

    json hp = d.value("hookPaths", json::object());
    out.hooks.istBootAssert = json_path(hp, "istBootAssert");
    out.hooks.istBootDeassert = json_path(hp, "istBootDeassert");
    out.hooks.resetSystem = json_path(hp, "resetSystem");
    out.hooks.errorCheck = json_path(hp, "errorCheck");

    json sc = d.value("storageConfig", json::object());
    out.storage.vectorMountPath = json_path(sc, "vectorMountPath");
    out.storage.vectorStoragePath = json_path(sc, "vectorStoragePath");
    out.storage.resultStoragePath = json_path(sc, "resultStoragePath");

    resolveItmPaths(out);

    // Validate hook paths: reject explicitly-set-but-empty values,
    // and ensure non-empty paths are within hookDirectory.
    for (const auto& [name, hookPath] :
         {std::pair<const char*, const fs::path&>{"istBootAssert",
                                                  out.hooks.istBootAssert},
          {"istBootDeassert", out.hooks.istBootDeassert},
          {"resetSystem", out.hooks.resetSystem},
          {"errorCheck", out.hooks.errorCheck}})
    {
        if (hookPath.empty())
        {
            if (hp.contains(name))
            {
                std::cerr << "Hook '" << name << "' has empty path\n";
                return false;
            }
            continue;
        }
        if (!is_path_within(hookPath, out.hookDir))
        {
            std::cerr << "Hook '" << name << "' path '" << hookPath
                      << "' is outside hookDirectory '" << out.hookDir << "'\n";
            return false;
        }
    }

    return true;
}

void IstService::printIstPlatformConfig() const
{
    std::cout << "istHookPath:    " << platformCfg_.hookDir << '\n';
    std::cout << "itmBinaryPath:  " << platformCfg_.itmBinaryPath << '\n';
    std::cout << "itmLibDir:      " << platformCfg_.itmLibDir << '\n';
    std::cout << "hooks:\n";
    std::cout << "  istBootAssert = " << platformCfg_.hooks.istBootAssert
              << '\n';
    std::cout << "  istBootDeassert = " << platformCfg_.hooks.istBootDeassert
              << '\n';
    std::cout << "  resetSystem = " << platformCfg_.hooks.resetSystem << '\n';
    std::cout << "  errorCheck = " << platformCfg_.hooks.errorCheck << '\n';
    std::cout << "storage:\n";
    std::cout << "  vectorMountPath = " << platformCfg_.storage.vectorMountPath
              << '\n';
    std::cout << "  vectorStoragePath = "
              << platformCfg_.storage.vectorStoragePath << '\n';
    std::cout << "  resultStoragePath = "
              << platformCfg_.storage.resultStoragePath << '\n';
}

bool IstService::initialize(IstPlatformConfig cfg)
{
    if (initialized_)
    {
        std::cerr << "Already initialized\n";
        return false;
    }

    platformCfg_ = std::move(cfg);
    printIstPlatformConfig();

    // Deassert IST boot on startup as a safety measure (e.g. after crash)
    const fs::path& hook_cmd = platformCfg_.hooks.istBootDeassert;
    if (!hook_cmd.empty())
    {
        hookRunner_->asyncRun(hook_cmd, "istBootDeassert on startup",
                              [](bool ok) {
                                  if (!ok)
                                  {
                                      std::cerr << "istBootDeassert on startup "
                                                   "failed (non-fatal)\n";
                                  }
                              });
    }

    initialized_ = true;

    vectorManager_->mountVectorsOnStartup();
    return true;
}

// ----------------
// Test params
// ----------------

bool IstService::getISTParams(const ParamMap& test_params)
{
    test_ = IstTestConfig{};

    for (const auto& [key, val] : test_params)
    {
        // Reject oversized string values
        if (const std::string* s = std::get_if<std::string>(&val);
            s && s->length() > max_param_string_length)
        {
            std::cerr << "Parameter '" << key << "' exceeds max length ("
                      << max_param_string_length << ")\n";
            return false;
        }

        if (key == "istSwTimeoutSec")
        {
            if (const int* i = std::get_if<int>(&val))
            {
                test_.swTimeoutSec =
                    std::clamp(static_cast<int32_t>(*i), min_sw_timeout_sec,
                               max_sw_timeout_sec);
            }
            else
            {
                std::cerr << "Parameter 'istSwTimeoutSec' has wrong type "
                             "(expected int)\n";
                return false;
            }
        }
        else if (key == "customTestList")
        {
            if (const std::string* s = std::get_if<std::string>(&val))
            {
                test_.customTestList = *s;
            }
            else
            {
                std::cerr << "Parameter 'customTestList' has wrong type "
                             "(expected string)\n";
                return false;
            }
        }
        else if (key == "customSocketList")
        {
            if (const std::string* s = std::get_if<std::string>(&val))
            {
                test_.customSocketList = *s;
            }
            else
            {
                std::cerr << "Parameter 'customSocketList' has wrong type "
                             "(expected string)\n";
                return false;
            }
        }
        else if (key == "istContinueOnFail")
        {
            if (const bool* b = std::get_if<bool>(&val))
            {
                test_.continueOnFail = *b;
            }
            else
            {
                std::cerr << "Parameter 'istContinueOnFail' has wrong type "
                             "(expected bool)\n";
                return false;
            }
        }
        else if (key == "istSaveResOnPass")
        {
            if (const bool* b = std::get_if<bool>(&val))
            {
                test_.saveResOnPass = *b;
            }
            else
            {
                std::cerr << "Parameter 'istSaveResOnPass' has wrong type "
                             "(expected bool)\n";
                return false;
            }
        }
        else if (key == "istSaveResOnFail")
        {
            if (const bool* b = std::get_if<bool>(&val))
            {
                test_.saveResOnFail = *b;
            }
            else
            {
                std::cerr << "Parameter 'istSaveResOnFail' has wrong type "
                             "(expected bool)\n";
                return false;
            }
        }
        else if (key == "autoRebootOnComplete")
        {
            if (const bool* b = std::get_if<bool>(&val))
            {
                test_.autoRebootOnComplete = *b;
            }
            else
            {
                std::cerr << "Parameter 'autoRebootOnComplete' has wrong type "
                             "(expected bool)\n";
                return false;
            }
        }
        else
        {
            std::cerr << "Unknown IST test param: " << key << '\n';
            return false;
        }
    }
    return true;
}

bool IstService::collateralVerification()
{
    if (platformCfg_.storage.vectorMountPath.empty())
    {
        std::cerr << "vectorMountPath not configured in platform config!\n";
        return false;
    }
    std::error_code fs_ec;
    if (!fs::exists(platformCfg_.storage.vectorMountPath, fs_ec) || fs_ec)
    {
        std::cerr << "Vector storage path '"
                  << platformCfg_.storage.vectorMountPath
                  << "' does not exist or is inaccessible";
        if (fs_ec)
        {
            std::cerr << ": " << fs_ec.message();
        }
        std::cerr << '\n';
        return false;
    }

    resolveItmPaths(platformCfg_);

    if (!fs::exists(platformCfg_.itmBinaryPath, fs_ec) || fs_ec)
    {
        std::cerr << "IST binary '" << platformCfg_.itmBinaryPath
                  << "' not found; test vectors may not be mounted\n";
        return false;
    }

    fs::path golden_res = platformCfg_.storage.vectorMountPath / "GOLDEN_RES";
    if (!fs::exists(golden_res, fs_ec) || fs_ec ||
        fs::is_empty(golden_res, fs_ec) || fs_ec)
    {
        std::cerr << "GOLDEN_RES not found or empty under '"
                  << platformCfg_.storage.vectorMountPath
                  << "'; test vectors may not be mounted\n";
        return false;
    }

    return true;
}

bool IstService::prepareResultStorage()
{
    if (platformCfg_.storage.resultStoragePath.empty())
    {
        std::cerr << "resultStoragePath not configured in platform config!\n";
        return false;
    }
    std::error_code fs_ec;
    fs::create_directories(platformCfg_.storage.resultStoragePath, fs_ec);
    if (fs_ec)
    {
        std::cerr << "Failed to create results directory: " << fs_ec.message()
                  << '\n';
        return false;
    }

    return true;
}

// ----------------
// Async step handlers
// ----------------

void IstService::onIstBootAssertDone(bool ok)
{
    if (!ok)
    {
        std::cerr << "Failed to assert IST boot\n";
        state_.progress = 0;
        transitionTo(IstStage::idle, IstStatus::failed,
                     "category=HOOK, reason=ist_boot_assert_failed");
        return;
    }

    std::error_code ec;
    if (fs::is_directory(err_marker_dir, ec))
    {
        for (const auto& entry : fs::directory_iterator(err_marker_dir, ec))
        {
            std::ifstream marker_file(entry.path());
            std::string content;
            if (marker_file)
            {
                std::getline(marker_file, content);
            }

            if (content == "deasserted")
            {
                fs::remove(entry.path(), ec);
                if (ec)
                {
                    std::cerr << "Failed to remove marker '"
                              << entry.path().string() << "': " << ec.message()
                              << "\n";
                }
            }
        }
    }

    transitionTo(IstStage::pendingPowerCycle);

    // Re-emit the Stage PropertiesChanged signal after a short delay.
    // The D-Bus client (bmcweb) registers its PropertiesChanged match only
    // after it receives the StartIST method reply, so there is a window
    // where the initial signal from transitionTo() is missed.
    // signal_property() forces a new emission of the current value.
    reSignalTimer_.expires_after(std::chrono::seconds(2));
    reSignalTimer_.async_wait(
        [self = shared_from_this()](const boost::system::error_code& ec) {
            if (ec)
            {
                return;
            }
            self->publisher_->reSignalStage();
        });

    powerMonitor_->asyncWaitForPowerCycle(
        [self = shared_from_this()](bool ok_power) {
            self->onPowerCycleDone(ok_power);
        });
}

void IstService::onPowerCycleDone(bool ok)
{
    if (!ok)
    {
        std::cerr << "Unable to detect power cycle, running cleanup\n";
        failureInfo_.emplace_back("category=POWER_CYCLE, reason=not_detected");
        runIstCleanup(-1);
        return;
    }

    transitionTo(IstStage::runningIst);

    std::filesystem::path cak_script =
        platformCfg_.hookDir / "ist_cak_bypass.sh";
    std::error_code ec;
    if (std::filesystem::exists(cak_script, ec) && !ec)
    {
        if (!is_path_within(cak_script, platformCfg_.hookDir))
        {
            std::cerr << "CAK bypass script '" << cak_script.string()
                      << "' resolves outside hookDirectory '"
                      << platformCfg_.hookDir.string() << "'\n";
            failureInfo_.emplace_back(
                "category=SECURITY, reason=cak_bypass_path_escape");
            runIstCleanup(-1);
            return;
        }
        hookRunner_->asyncRun(
            cak_script, "CAK bypass", [self = shared_from_this()](bool ok_cak) {
                if (!ok_cak)
                {
                    std::cerr << "CAK bypass hook failed, aborting IST\n";
                    self->failureInfo_.emplace_back(
                        "category=HOOK, reason=cak_bypass_failed");
                    self->runIstCleanup(-1);
                    return;
                }
                self->startItmRun();
            });
        return;
    }

    startItmRun();
}

void IstService::startItmRun()
{
    transitionTo(IstStage::runningIst);

    itmRunner_->asyncRun(
        test_, platformCfg_,
        [self = shared_from_this()](int itm_exit) {
            if (itm_exit > 0)
            {
                self->failureInfo_.emplace_back("category=ITM, exit_code=" +
                                                std::to_string(itm_exit));
            }
            else if (itm_exit == itm_exit_sw_timeout)
            {
                self->failureInfo_.emplace_back(
                    "category=ITM, reason=sw_timeout");
            }
            else if (itm_exit < 0)
            {
                self->failureInfo_.emplace_back(
                    "category=ITM, reason=process_error");
            }
            self->runIstCleanup(itm_exit);
        },
        [self = shared_from_this()](uint8_t progress) {
            self->state_.progress = progress;
            self->publisher_->publishProgress(progress);
        });
}

void IstService::runIstCleanup(int itm_exit)
{
    if (itm_exit >= 0)
    {
        std::cout << "kist_itm exited with code " << itm_exit << '\n';
    }
    else
    {
        std::cerr << "Failed to execute IST (code=" << itm_exit << ")\n";
    }
    emitIstEventLog(itm_exit);
    for (const auto& info : failureInfo_)
    {
        std::cerr << "IST_ERROR: " << info << '\n';
    }
    failureInfo_.clear();
    transitionTo(IstStage::cleanup);

    fs::path results_dir = platformCfg_.storage.resultStoragePath;
    archiveResultsAsync(io_, results_dir,
                        [weak = weak_from_this(), itm_exit](bool ok) {
                            if (!ok)
                            {
                                std::cerr << "IST: results archiving failed\n";
                            }
                            if (auto self = weak.lock())
                            {
                                self->onArchiveDone(itm_exit);
                            }
                        });
}

void IstService::onArchiveDone(int itm_exit)
{
    const fs::path& hook_cmd = platformCfg_.hooks.istBootDeassert;
    hookRunner_->asyncRun(
        hook_cmd, "istBootDeassert hook",
        [self = shared_from_this(), itm_exit](bool ok_deassert) {
            self->onDeassertDone(itm_exit, ok_deassert);
        });
}

void IstService::emitIstEventLog(int itm_exit)
{
    if (itm_exit != itm_exit_mismatch && itm_exit != itm_exit_platform_error)
    {
        return;
    }

    std::string result = (itm_exit == itm_exit_mismatch) ? "Failed" : "Error";
    std::string message = (itm_exit == itm_exit_mismatch)
                              ? "IST mismatch: one or more tests failed"
                              : "Failed due to error";

    if (itm_exit == itm_exit_platform_error)
    {
        std::error_code ec;
        std::string markers;
        for (const auto& entry : fs::directory_iterator(err_marker_dir, ec))
        {
            if (!markers.empty())
            {
                markers += " | ";
            }
            markers += entry.path().filename().string();
        }
        if (!markers.empty())
        {
            message += ": " + markers;
        }
    }

    std::map<std::string, std::string> additional_data = {
        {"REDFISH_MESSAGE_ID", "Platform.1.0.PlatformError"},
        {"IST_TYPE", "CPU"},
        {"IST_STAGE", istStageToString(state_.stage)},
        {"IST_PROGRESS", std::to_string(state_.progress)},
        {"IST_RESULT", result},
        {"IST_MESSAGE", message},
    };
    if (test_.continueOnFail.has_value())
    {
        additional_data["IST_CONTINUE_ON_FAIL"] =
            *test_.continueOnFail ? "true" : "false";
    }
    if (test_.customTestList.has_value())
    {
        additional_data["IST_TEST_LIST"] = *test_.customTestList;
    }
    if (test_.customSocketList.has_value())
    {
        additional_data["IST_PACKAGE_LIST"] = *test_.customSocketList;
    }

    publisher_->emitEventLog("IST failed",
                             "xyz.openbmc_project.Logging.Entry.Level.Warning",
                             additional_data);
}

void IstService::onDeassertDone(int itm_exit, bool ok_deassert)
{
    if (!ok_deassert)
    {
        std::cerr << "istBootDeassert hook failed during cleanup\n";
        transitionTo(IstStage::idle, IstStatus::failed,
                     "category=HOOK, reason=ist_boot_deassert_failed");
        return;
    }

    if (test_.autoRebootOnComplete)
    {
        const fs::path& reset_cmd = platformCfg_.hooks.resetSystem;
        if (reset_cmd.empty())
        {
            std::cerr << "resetSystem hook not found, failing cleanup\n";
            transitionTo(IstStage::idle, IstStatus::failed,
                         "category=HOOK, reason=reset_system_not_found");
            return;
        }
        hookRunner_->asyncRun(
            reset_cmd, "resetSystem hook",
            [self = shared_from_this(), itm_exit](bool ok_reset) {
                self->onResetDone(itm_exit, ok_reset);
            },
            {"--skip-cak"});
    }
    else
    {
        transitionTo(IstStage::idle,
                     itm_exit == 0 ? IstStatus::completed : IstStatus::failed);
    }
}

void IstService::onResetDone(int itm_exit, bool ok_reset)
{
    if (!ok_reset)
    {
        transitionTo(IstStage::idle, IstStatus::failed,
                     "category=HOOK, reason=reset_system_failed");
    }
    else
    {
        transitionTo(IstStage::idle,
                     itm_exit == 0 ? IstStatus::completed : IstStatus::failed);
    }
}

// ----------------
// StartIST entry point
// ----------------

std::string IstService::startIST(const ParamMap& test_params)
{
    if (state_.stage != IstStage::idle)
    {
        std::cerr << "Another IST run is in progress\n";
        throw sdbusplus::exception::SdBusError(EBUSY, "IST already running");
    }
    if (vectorManager_->inProgress())
    {
        std::cerr << "Cannot start IST while update is in progress\n";
        throw sdbusplus::exception::SdBusError(
            EBUSY, "Cannot start IST while update is in progress");
    }

    vectorManager_->ensureMounted();

    transitionTo(IstStage::collateralVerification, IstStatus::inProgress);

    if (!getISTParams(test_params))
    {
        transitionTo(IstStage::idle, IstStatus::aborted,
                     "category=PARAMS, reason=invalid_test_params");
        throw ist_err::InvalidParameter{};
    }

    if (!collateralVerification())
    {
        transitionTo(IstStage::idle, IstStatus::aborted,
                     "category=COLLATERAL, reason=verification_failed");
        throw ist_err::CollateralNotFound{};
    }

    if (!prepareResultStorage())
    {
        transitionTo(IstStage::idle, IstStatus::aborted,
                     "category=STORAGE, reason=result_storage_unavailable");
        throw ist_err::ResultStorageError{};
    }

    const fs::path& hook_cmd = platformCfg_.hooks.istBootAssert;
    if (hook_cmd.empty())
    {
        transitionTo(IstStage::idle, IstStatus::failed,
                     "category=HOOK, reason=ist_boot_assert_not_found");
        throw ist_err::HookNotFound{};
    }

    const fs::path& deassert_cmd = platformCfg_.hooks.istBootDeassert;
    if (deassert_cmd.empty())
    {
        transitionTo(IstStage::idle, IstStatus::failed,
                     "category=HOOK, reason=ist_boot_deassert_not_found");
        throw ist_err::HookNotFound{};
    }

    currentRunPath_ =
        "/com/nvidia/vera/ist/runs/" + std::to_string(runCounter_++);
    publisher_->createRunObject(currentRunPath_, resultsFdCb_);

    state_.progress = 0;

    std::error_code ec;
    for (const auto& entry :
         fs::directory_iterator(platformCfg_.storage.resultStoragePath, ec))
    {
        std::error_code rm_ec;
        fs::remove_all(entry.path(), rm_ec);
        if (rm_ec)
        {
            std::cerr << "Failed to remove " << entry.path() << ": "
                      << rm_ec.message() << '\n';
        }
    }

    installServiceLogTee();

    signatureVerifier_(platformCfg_, [self = shared_from_this()](bool ok) {
        self->onSignatureVerifyDone(ok);
    });

    return currentRunPath_;
}

void IstService::onSignatureVerifyDone(bool ok)
{
    if (!ok)
    {
        std::cerr << "IST binary/library signature verification failed\n";
        transitionTo(IstStage::idle, IstStatus::failed,
                     "category=SECURITY, reason=signature_verification_failed");
        return;
    }

    if (test_.customSocketList.has_value())
    {
        std::cout << "Using user-provided socket list: "
                  << *test_.customSocketList << '\n';
        launchBootAssert();
        return;
    }

    if (!cpuDiscoverer_)
    {
        std::cout << "No CPU discoverer configured, skipping discovery\n";
        launchBootAssert();
        return;
    }

    std::cout << "Starting CPU discovery via Entity Manager\n";
    cpuDiscoverer_(
        [self = shared_from_this()](const std::vector<std::string>& cpu_paths) {
            self->onCpuDiscoveryDone(cpu_paths);
        });
}

void IstService::onCpuDiscoveryDone(const std::vector<std::string>& cpu_paths)
{
    if (cpu_paths.empty())
    {
        std::cerr << "No CPUs discovered via Entity Manager\n";
        transitionTo(IstStage::idle, IstStatus::failed,
                     "category=DISCOVERY, reason=no_cpus_found");
        return;
    }

    std::vector<size_t> socket_ids;
    for (const auto& path : cpu_paths)
    {
        std::string cpu_name = fs::path(path).filename();
        auto pos = cpu_name.rfind('_');
        if (pos == std::string::npos || pos + 1 >= cpu_name.size())
        {
            std::cerr << "Cannot extract socket number from: " << path << '\n';
            transitionTo(IstStage::idle, IstStatus::failed,
                         "category=DISCOVERY, reason=package_parse_failed");
            return;
        }
        std::string_view digits(cpu_name.data() + pos + 1,
                                cpu_name.size() - pos - 1);
        size_t id = 0;
        auto [ptr, ec] =
            std::from_chars(digits.data(), digits.data() + digits.size(), id);
        if (ec != std::errc{} || ptr != digits.data() + digits.size())
        {
            std::cerr << "Cannot parse socket number from: " << path << '\n';
            transitionTo(IstStage::idle, IstStatus::failed,
                         "category=DISCOVERY, reason=package_parse_failed");
            return;
        }
        socket_ids.push_back(id);
    }

    std::sort(socket_ids.begin(), socket_ids.end());

    for (size_t i = 0; i < socket_ids.size(); ++i)
    {
        if (socket_ids[i] != i)
        {
            std::cerr << "Non-contiguous CPU sockets detected (expected socket "
                      << i << ", found " << socket_ids[i] << ")\n";
            transitionTo(IstStage::idle, IstStatus::failed,
                         "category=DISCOVERY, reason=non_contiguous_packages");
            return;
        }
    }

    std::string socket_list;
    for (size_t i = 0; i < socket_ids.size(); ++i)
    {
        if (i > 0)
        {
            socket_list += ',';
        }
        socket_list += std::to_string(socket_ids[i]);
    }
    test_.customSocketList = std::move(socket_list);
    std::cout << "Discovered " << cpu_paths.size()
              << " CPU(s), socket list: " << *test_.customSocketList << '\n';

    launchBootAssert();
}

void IstService::launchBootAssert()
{
    transitionTo(IstStage::pendingIstBoot);

    const fs::path& hook_cmd = platformCfg_.hooks.istBootAssert;
    hookRunner_->asyncRun(hook_cmd, "istBootAssert hook",
                          [self = shared_from_this()](bool hook_ok) {
                              self->onIstBootAssertDone(hook_ok);
                          });
}
