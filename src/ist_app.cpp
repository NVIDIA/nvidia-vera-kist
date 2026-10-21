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
#include <phosphor-logging/lg2.hpp>
#include <sdbusplus/exception.hpp>
#include <signature_verify.hpp>
#include <vector_manager.hpp>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <format>
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

// An external tool parses this line out of IST_service.log in the results
// tarball. It matches on a line containing "IST_ERROR: category=<>, <key>=<>",
// so text may be added ahead of the prefix but nothing from "IST_ERROR:"
// onward may change.
static void emit_ist_error(std::string_view info)
{
    log_to_results("ERR", std::format("IST_ERROR: {}", info));
    lg2::error("IST_ERROR: {FAILURE}", "FAILURE", info);
}

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
        lg2::error("is_path_within: cannot resolve child path '{PATH}': "
                   "{ERROR}",
                   "PATH", child.string(), "ERROR", ec.message());
        return false;
    }
    fs::path canonical_parent = fs::canonical(parent, ec);
    if (ec)
    {
        lg2::error("is_path_within: cannot resolve parent path '{PATH}': "
                   "{ERROR}",
                   "PATH", parent.string(), "ERROR", ec.message());
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
    hook_runner_(std::move(hook_runner)),
    power_monitor_(std::move(power_monitor)),
    itm_runner_(std::move(itm_runner)),
    signature_verifier_([&io](const IstPlatformConfig& cfg,
                              std::move_only_function<void(bool) const> cb) {
        verify_itm_signatures_async(io, cfg, std::move(cb));
    }),
    vector_manager_(VectorManager::create(io, *publisher_, platform_cfg_)),
    re_signal_timer_(io)
{
    hook_runner_->set_log_file(run_logs_.service);
}

IstService::~IstService()
{
    close_service_log();
}

void IstService::set_signature_verifier(SignatureVerifier fn)
{
    signature_verifier_ = std::move(fn);
}

void IstService::set_cpu_discoverer(CpuDiscoverer fn)
{
    cpu_discoverer_ = std::move(fn);
}

void IstService::set_results_fd_callback(ResultsFdCb cb)
{
    results_fd_cb_ = std::move(cb);
}

// ----------------
// D-Bus state helpers
// ----------------

void IstService::update_dbus_state()
{
    publisher_->publish(state_);
}

void IstService::transition_to(IstStage stage)
{
    re_signal_timer_.cancel();
    state_.stage = stage;
    update_dbus_state();
}

void IstService::transition_to(IstStage stage, IstStatus status,
                               std::string_view error_info)
{
    re_signal_timer_.cancel();
    state_.stage = stage;
    state_.status = status;
    if (stage == IstStage::idle)
    {
        if (status != IstStatus::completed && !error_info.empty())
        {
            emit_ist_error(error_info);
        }
        close_service_log();
    }
    update_dbus_state();
}

void IstService::open_service_log()
{
    if (run_logs_.service.is_open())
    {
        return;
    }

    fs::path log_path =
        platform_cfg_.storage.result_storage_path / "IST_service.log";
    if (run_logs_.service.open(log_path))
    {
        set_service_log(&run_logs_.service);
    }
}

void IstService::close_service_log()
{
    set_service_log(nullptr);
    run_logs_.service.close();
}

// ----------------
// Platform config
// ----------------

void resolve_itm_paths(IstPlatformConfig& cfg)
{
    if (cfg.storage.vector_mount_path.empty())
    {
        return;
    }

    if (!cfg.arch_sub_dir.empty())
    {
        fs::path arch_dir =
            cfg.storage.vector_mount_path / "kist_itm" / cfg.arch_sub_dir;
        std::error_code ec;
        if (fs::is_directory(arch_dir, ec) && !ec)
        {
            cfg.itm_binary_path = arch_dir / "bin" / "kist_itm";
            cfg.itm_lib_dir = arch_dir / "lib";
            return;
        }
    }

    cfg.itm_binary_path = cfg.storage.vector_mount_path / "kist_itm";
    cfg.itm_lib_dir.clear();
}

bool parse_platform_config(IstPlatformConfig& out, const std::string& path,
                           std::string_view hook_root)
{
    std::ifstream f(path);
    if (!f)
    {
        lg2::error("Failed to open {PATH}", "PATH", path);
        return false;
    }

    json d;
    try
    {
        d = json::parse(f);
    }
    catch (const std::exception& e)
    {
        lg2::error("Failed to parse JSON: {ERROR}", "ERROR", e.what());
        return false;
    }

    if (!d.contains("hookDirectory") || !d["hookDirectory"].is_string())
    {
        lg2::error("hookDirectory missing or not a string");
        return false;
    }
    out.hook_dir = d["hookDirectory"].get<std::string>();

    std::error_code fs_ec;
    if (!fs::is_directory(out.hook_dir, fs_ec) || fs_ec)
    {
        lg2::error("hookDirectory does not exist or is not a directory: {PATH}",
                   "PATH", out.hook_dir);
        return false;
    }

    if (!is_path_within(out.hook_dir, fs::path(hook_root)))
    {
        lg2::error("hookDirectory '{PATH}' resolves outside the trusted hook "
                   "root '{ROOT}'",
                   "PATH", out.hook_dir, "ROOT", hook_root);
        return false;
    }

    if (!d.contains("softwareInventoryId") ||
        !d["softwareInventoryId"].is_string())
    {
        lg2::error("softwareInventoryId is missing or not a string");
        return false;
    }
    std::string id = d["softwareInventoryId"].get<std::string>();
    if (id.empty() || std::any_of(id.begin(), id.end(), [](char c) {
            return std::isalnum(static_cast<unsigned char>(c)) == 0 && c != '_';
        }))
    {
        lg2::error("softwareInventoryId contains invalid characters: {ID}",
                   "ID", id);
        return false;
    }
    out.software_inventory_id = std::move(id);

    json hp = d.value("hookPaths", json::object());
    out.hooks.ist_boot_assert = json_path(hp, "istBootAssert");
    out.hooks.ist_boot_deassert = json_path(hp, "istBootDeassert");
    out.hooks.reset_system = json_path(hp, "resetSystem");
    out.hooks.error_check = json_path(hp, "errorCheck");

    json sc = d.value("storageConfig", json::object());
    out.storage.vector_mount_path = json_path(sc, "vectorMountPath");
    out.storage.vector_storage_path = json_path(sc, "vectorStoragePath");
    out.storage.result_storage_path = json_path(sc, "resultStoragePath");

    resolve_itm_paths(out);

    // Validate hook paths: reject explicitly-set-but-empty values,
    // and ensure non-empty paths are within hookDirectory.
    for (const auto& [name, hookPath] :
         {std::pair<const char*, const fs::path&>{"istBootAssert",
                                                  out.hooks.ist_boot_assert},
          {"istBootDeassert", out.hooks.ist_boot_deassert},
          {"resetSystem", out.hooks.reset_system},
          {"errorCheck", out.hooks.error_check}})
    {
        if (hookPath.empty())
        {
            if (hp.contains(name))
            {
                lg2::error("Hook '{NAME}' has empty path", "NAME", name);
                return false;
            }
            continue;
        }
        if (!is_path_within(hookPath, out.hook_dir))
        {
            lg2::error("Hook '{NAME}' path '{PATH}' is outside hookDirectory "
                       "'{DIR}'",
                       "NAME", name, "PATH", hookPath, "DIR", out.hook_dir);
            return false;
        }
    }

    return true;
}

void IstService::print_ist_platform_config() const
{
    lg2::info("Platform config: hookPath={HOOK_DIR}, itmBinary={ITM_BINARY}, "
              "itmLibDir={ITM_LIB_DIR}, bootAssert={BOOT_ASSERT}, "
              "bootDeassert={BOOT_DEASSERT}, resetSystem={RESET_SYSTEM}, "
              "errorCheck={ERROR_CHECK}, vectorMountPath={VECTOR_MOUNT}, "
              "vectorStoragePath={VECTOR_STORAGE}, "
              "resultStoragePath={RESULT_STORAGE}",
              "HOOK_DIR", platform_cfg_.hook_dir, "ITM_BINARY",
              platform_cfg_.itm_binary_path, "ITM_LIB_DIR",
              platform_cfg_.itm_lib_dir, "BOOT_ASSERT",
              platform_cfg_.hooks.ist_boot_assert, "BOOT_DEASSERT",
              platform_cfg_.hooks.ist_boot_deassert, "RESET_SYSTEM",
              platform_cfg_.hooks.reset_system, "ERROR_CHECK",
              platform_cfg_.hooks.error_check, "VECTOR_MOUNT",
              platform_cfg_.storage.vector_mount_path, "VECTOR_STORAGE",
              platform_cfg_.storage.vector_storage_path, "RESULT_STORAGE",
              platform_cfg_.storage.result_storage_path);
}

bool IstService::initialize(IstPlatformConfig cfg)
{
    if (initialized_)
    {
        lg2::error("Already initialized");
        return false;
    }

    platform_cfg_ = std::move(cfg);
    print_ist_platform_config();

    // Deassert IST boot on startup as a safety measure (e.g. after crash)
    const fs::path& hook_cmd = platform_cfg_.hooks.ist_boot_deassert;
    if (!hook_cmd.empty())
    {
        hook_runner_->async_run(
            hook_cmd, "istBootDeassert on startup", [](bool ok) {
                if (!ok)
                {
                    lg2::warning("istBootDeassert on startup failed "
                                 "(non-fatal)");
                }
            });
    }

    initialized_ = true;

    vector_manager_->mount_vectors_on_startup();
    return true;
}

// ----------------
// Test params
// ----------------

bool IstService::get_ist_params(const ParamMap& test_params)
{
    test_ = IstTestConfig{};

    for (const auto& [key, val] : test_params)
    {
        // Reject oversized string values
        if (const std::string* s = std::get_if<std::string>(&val);
            s && s->length() > max_param_string_length)
        {
            lg2::error("Parameter '{KEY}' exceeds max length ({LIMIT})", "KEY",
                       key, "LIMIT", max_param_string_length);
            return false;
        }

        if (key == "istSwTimeoutSec")
        {
            if (const int* i = std::get_if<int>(&val))
            {
                test_.sw_timeout_sec =
                    std::clamp(static_cast<int32_t>(*i), min_sw_timeout_sec,
                               max_sw_timeout_sec);
            }
            else
            {
                lg2::error("Parameter 'istSwTimeoutSec' has wrong type "
                           "(expected int)");
                return false;
            }
        }
        else if (key == "customTestList")
        {
            if (const std::string* s = std::get_if<std::string>(&val))
            {
                test_.custom_test_list = *s;
            }
            else
            {
                lg2::error("Parameter 'customTestList' has wrong type "
                           "(expected string)");
                return false;
            }
        }
        else if (key == "customSocketList")
        {
            if (const std::string* s = std::get_if<std::string>(&val))
            {
                test_.custom_socket_list = *s;
            }
            else
            {
                lg2::error("Parameter 'customSocketList' has wrong type "
                           "(expected string)");
                return false;
            }
        }
        else if (key == "istContinueOnFail")
        {
            if (const bool* b = std::get_if<bool>(&val))
            {
                test_.continue_on_fail = *b;
            }
            else
            {
                lg2::error("Parameter 'istContinueOnFail' has wrong type "
                           "(expected bool)");
                return false;
            }
        }
        else if (key == "istSaveResOnPass")
        {
            if (const bool* b = std::get_if<bool>(&val))
            {
                test_.save_res_on_pass = *b;
            }
            else
            {
                lg2::error("Parameter 'istSaveResOnPass' has wrong type "
                           "(expected bool)");
                return false;
            }
        }
        else if (key == "istSaveResOnFail")
        {
            if (const bool* b = std::get_if<bool>(&val))
            {
                test_.save_res_on_fail = *b;
            }
            else
            {
                lg2::error("Parameter 'istSaveResOnFail' has wrong type "
                           "(expected bool)");
                return false;
            }
        }
        else if (key == "autoRebootOnComplete")
        {
            if (const bool* b = std::get_if<bool>(&val))
            {
                test_.auto_reboot_on_complete = *b;
            }
            else
            {
                lg2::error("Parameter 'autoRebootOnComplete' has wrong type "
                           "(expected bool)");
                return false;
            }
        }
        else
        {
            lg2::error("Unknown IST test param: {KEY}", "KEY", key);
            return false;
        }
    }
    return true;
}

bool IstService::collateral_verification()
{
    if (platform_cfg_.storage.vector_mount_path.empty())
    {
        lg2::error("vectorMountPath not configured in platform config!");
        return false;
    }
    std::error_code fs_ec;
    if (!fs::exists(platform_cfg_.storage.vector_mount_path, fs_ec) || fs_ec)
    {
        lg2::error("Vector storage path '{PATH}' does not exist or is "
                   "inaccessible: {ERROR}",
                   "PATH", platform_cfg_.storage.vector_mount_path, "ERROR",
                   fs_ec ? fs_ec.message() : std::string("no error reported"));
        return false;
    }

    resolve_itm_paths(platform_cfg_);

    if (!fs::exists(platform_cfg_.itm_binary_path, fs_ec) || fs_ec)
    {
        lg2::error("IST binary '{PATH}' not found; test vectors may not be "
                   "mounted",
                   "PATH", platform_cfg_.itm_binary_path);
        return false;
    }

    fs::path golden_res =
        platform_cfg_.storage.vector_mount_path / "GOLDEN_RES";
    if (!fs::exists(golden_res, fs_ec) || fs_ec ||
        fs::is_empty(golden_res, fs_ec) || fs_ec)
    {
        lg2::error("GOLDEN_RES not found or empty under '{PATH}'; test vectors "
                   "may not be mounted",
                   "PATH", platform_cfg_.storage.vector_mount_path);
        return false;
    }

    return true;
}

bool IstService::prepare_result_storage()
{
    if (platform_cfg_.storage.result_storage_path.empty())
    {
        lg2::error("resultStoragePath not configured in platform config!");
        return false;
    }
    std::error_code fs_ec;
    fs::create_directories(platform_cfg_.storage.result_storage_path, fs_ec);
    if (fs_ec)
    {
        lg2::error("Failed to create results directory: {ERROR}", "ERROR",
                   fs_ec.message());
        return false;
    }

    return true;
}

// ----------------
// Async step handlers
// ----------------

void IstService::on_ist_boot_assert_done(bool ok)
{
    if (!ok)
    {
        lg2::error("Failed to assert IST boot");
        state_.progress = 0;
        fail_run_with_archive("category=HOOK, reason=ist_boot_assert_failed");
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
                    lg2::error("Failed to remove marker '{PATH}': {ERROR}",
                               "PATH", entry.path().string(), "ERROR",
                               ec.message());
                }
            }
        }
    }

    transition_to(IstStage::pending_power_cycle);

    // Re-emit the Stage PropertiesChanged signal after a short delay.
    // The D-Bus client (bmcweb) registers its PropertiesChanged match only
    // after it receives the StartIST method reply, so there is a window
    // where the initial signal from transition_to() is missed.
    // signal_property() forces a new emission of the current value.
    re_signal_timer_.expires_after(std::chrono::seconds(2));
    re_signal_timer_.async_wait(
        [self = shared_from_this()](const boost::system::error_code& ec) {
            if (ec)
            {
                return;
            }
            self->publisher_->re_signal_stage();
        });

    power_monitor_->async_wait_for_power_cycle(
        [self = shared_from_this()](bool ok_power) {
            self->on_power_cycle_done(ok_power);
        });
}

void IstService::on_power_cycle_done(bool ok)
{
    if (!ok)
    {
        lg2::error("Unable to detect power cycle, running cleanup");
        failure_info_.emplace_back("category=POWER_CYCLE, reason=not_detected");
        run_ist_cleanup(-1);
        return;
    }

    transition_to(IstStage::running_ist);

    std::filesystem::path cak_script =
        platform_cfg_.hook_dir / "ist_cak_bypass.sh";
    std::error_code ec;
    if (std::filesystem::exists(cak_script, ec) && !ec)
    {
        if (!is_path_within(cak_script, platform_cfg_.hook_dir))
        {
            log_to_results("ERR", std::format("CAK bypass script '{}' resolves "
                                              "outside hookDirectory '{}'",
                                              cak_script.string(),
                                              platform_cfg_.hook_dir.string()));
            lg2::error("CAK bypass script '{PATH}' resolves outside "
                       "hookDirectory '{DIR}'",
                       "PATH", cak_script.string(), "DIR",
                       platform_cfg_.hook_dir.string());
            failure_info_.emplace_back(
                "category=SECURITY, reason=cak_bypass_path_escape");
            run_ist_cleanup(-1);
            return;
        }
        hook_runner_->async_run(
            cak_script, "CAK bypass",
            [self = shared_from_this()](bool ok_cak) {
                if (!ok_cak)
                {
                    lg2::error("CAK bypass hook failed, aborting IST");
                    self->failure_info_.emplace_back(
                        "category=HOOK, reason=cak_bypass_failed");
                    self->run_ist_cleanup(-1);
                    return;
                }
                self->start_itm_run();
            },
            {}, HookRunner::k_cak_bypass_timeout);
        return;
    }

    start_itm_run();
}

void IstService::start_itm_run()
{
    transition_to(IstStage::running_ist);

    itm_runner_->async_run(
        test_, platform_cfg_,
        [self = shared_from_this()](int itm_exit) {
            if (itm_exit > 0)
            {
                self->failure_info_.emplace_back("category=ITM, exit_code=" +
                                                 std::to_string(itm_exit));
            }
            else if (itm_exit == itm_exit_sw_timeout)
            {
                self->failure_info_.emplace_back(
                    "category=ITM, reason=sw_timeout");
            }
            else if (itm_exit < 0)
            {
                self->failure_info_.emplace_back(
                    "category=ITM, reason=process_error");
            }
            self->run_ist_cleanup(itm_exit);
        },
        [self = shared_from_this()](uint8_t progress) {
            self->state_.progress = progress;
            self->publisher_->publish_progress(progress);
        });
}

void IstService::run_ist_cleanup(int itm_exit)
{
    if (itm_exit >= 0)
    {
        log_to_results("INFO",
                       std::format("kist_itm exited with code {}", itm_exit));
        lg2::info("kist_itm exited with code {EXIT_CODE}", "EXIT_CODE",
                  itm_exit);
    }
    else
    {
        lg2::error("Failed to execute IST (code={EXIT_CODE})", "EXIT_CODE",
                   itm_exit);
    }
    emit_ist_event_log(itm_exit);
    for (const auto& info : failure_info_)
    {
        emit_ist_error(info);
    }
    failure_info_.clear();
    transition_to(IstStage::cleanup);

    const fs::path& hook_cmd = platform_cfg_.hooks.ist_boot_deassert;
    hook_runner_->async_run(
        hook_cmd, "istBootDeassert hook",
        [self = shared_from_this(), itm_exit](bool ok_deassert) {
            self->on_deassert_done(itm_exit, ok_deassert);
        });
}

void IstService::on_archive_done(int itm_exit, bool ok_deassert)
{
    if (!ok_deassert)
    {
        transition_to(IstStage::idle, IstStatus::failed);
        return;
    }

    if (test_.auto_reboot_on_complete)
    {
        const fs::path& reset_cmd = platform_cfg_.hooks.reset_system;
        if (reset_cmd.empty())
        {
            lg2::error("resetSystem hook not found, failing cleanup");
            transition_to(IstStage::idle, IstStatus::failed,
                          "category=HOOK, reason=reset_system_not_found");
            return;
        }
        hook_runner_->async_run(
            reset_cmd, "resetSystem hook",
            [self = shared_from_this(), itm_exit](bool ok_reset) {
                self->on_reset_done(itm_exit, ok_reset);
            },
            {"--skip-cak"}, HookRunner::k_reset_system_timeout);
    }
    else
    {
        transition_to(IstStage::idle,
                      itm_exit == 0 ? IstStatus::completed : IstStatus::failed);
    }
}

void IstService::emit_ist_event_log(int itm_exit)
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
        {"IST_STAGE", ist_stage_to_string(state_.stage)},
        {"IST_PROGRESS", std::to_string(state_.progress)},
        {"IST_RESULT", result},
        {"IST_MESSAGE", message},
    };
    if (test_.continue_on_fail.has_value())
    {
        additional_data["IST_CONTINUE_ON_FAIL"] =
            *test_.continue_on_fail ? "true" : "false";
    }
    if (test_.custom_test_list.has_value())
    {
        additional_data["IST_TEST_LIST"] = *test_.custom_test_list;
    }
    if (test_.custom_socket_list.has_value())
    {
        additional_data["IST_PACKAGE_LIST"] = *test_.custom_socket_list;
    }

    publisher_->emit_event_log(
        "IST failed", "xyz.openbmc_project.Logging.Entry.Level.Warning",
        additional_data);
}

void IstService::on_deassert_done(int itm_exit, bool ok_deassert)
{
    if (!ok_deassert)
    {
        lg2::error("istBootDeassert hook failed during cleanup");
        emit_ist_error("category=HOOK, reason=ist_boot_deassert_failed");
    }

    fs::path results_dir = platform_cfg_.storage.result_storage_path;
    archive_results_async(
        io_, results_dir,
        [weak = weak_from_this(), itm_exit, ok_deassert](bool ok) {
            if (!ok)
            {
                lg2::error("IST: results archiving failed");
            }
            if (auto self = weak.lock())
            {
                self->on_archive_done(itm_exit, ok_deassert);
            }
        });
}

void IstService::fail_run_with_archive(std::string_view error_info)
{
    emit_ist_error(error_info);
    transition_to(IstStage::cleanup);

    fs::path results_dir = platform_cfg_.storage.result_storage_path;
    archive_results_async(io_, results_dir, [weak = weak_from_this()](bool ok) {
        if (!ok)
        {
            lg2::error("IST: results archiving failed");
        }
        if (auto self = weak.lock())
        {
            self->transition_to(IstStage::idle, IstStatus::failed);
        }
    });
}

void IstService::on_reset_done(int itm_exit, bool ok_reset)
{
    if (!ok_reset)
    {
        transition_to(IstStage::idle, IstStatus::failed,
                      "category=HOOK, reason=reset_system_failed");
    }
    else
    {
        transition_to(IstStage::idle,
                      itm_exit == 0 ? IstStatus::completed : IstStatus::failed);
    }
}

// ----------------
// StartIST entry point
// ----------------

std::string IstService::start_ist(const ParamMap& test_params)
{
    if (state_.stage != IstStage::idle)
    {
        lg2::error("Another IST run is in progress");
        throw sdbusplus::exception::SdBusError(EBUSY, "IST already running");
    }
    if (vector_manager_->in_progress())
    {
        lg2::error("Cannot start IST while update is in progress");
        throw sdbusplus::exception::SdBusError(
            EBUSY, "Cannot start IST while update is in progress");
    }

    vector_manager_->ensure_mounted();

    transition_to(IstStage::collateral_verification, IstStatus::in_progress);

    if (!get_ist_params(test_params))
    {
        transition_to(IstStage::idle, IstStatus::aborted,
                      "category=PARAMS, reason=invalid_test_params");
        throw ist_err::InvalidArgument{};
    }

    if (!collateral_verification())
    {
        transition_to(IstStage::idle, IstStatus::aborted,
                      "category=COLLATERAL, reason=verification_failed");
        throw ist_err::ResourceNotFound{};
    }

    if (!prepare_result_storage())
    {
        transition_to(IstStage::idle, IstStatus::aborted,
                      "category=STORAGE, reason=result_storage_unavailable");
        throw ist_err::InternalFailure{};
    }

    const fs::path& hook_cmd = platform_cfg_.hooks.ist_boot_assert;
    if (hook_cmd.empty())
    {
        transition_to(IstStage::idle, IstStatus::failed,
                      "category=HOOK, reason=ist_boot_assert_not_found");
        throw ist_err::InternalFailure{};
    }

    const fs::path& deassert_cmd = platform_cfg_.hooks.ist_boot_deassert;
    if (deassert_cmd.empty())
    {
        transition_to(IstStage::idle, IstStatus::failed,
                      "category=HOOK, reason=ist_boot_deassert_not_found");
        throw ist_err::InternalFailure{};
    }

    current_run_path_ =
        "/com/nvidia/vera/ist/runs/" + std::to_string(run_counter_++);
    publisher_->create_run_object(current_run_path_, results_fd_cb_);

    state_.progress = 0;

    std::error_code ec;
    for (const auto& entry :
         fs::directory_iterator(platform_cfg_.storage.result_storage_path, ec))
    {
        std::error_code rm_ec;
        fs::remove_all(entry.path(), rm_ec);
        if (rm_ec)
        {
            lg2::error("Failed to remove {PATH}: {ERROR}", "PATH", entry.path(),
                       "ERROR", rm_ec.message());
        }
    }

    open_service_log();

    signature_verifier_(platform_cfg_, [self = shared_from_this()](bool ok) {
        self->on_signature_verify_done(ok);
    });

    return current_run_path_;
}

void IstService::on_signature_verify_done(bool ok)
{
    if (!ok)
    {
        lg2::error("IST binary/library signature verification failed");
        fail_run_with_archive(
            "category=SECURITY, reason=signature_verification_failed");
        return;
    }

    if (test_.custom_socket_list.has_value())
    {
        log_to_results("INFO",
                       std::format("Using user-provided socket list: {}",
                                   *test_.custom_socket_list));
        lg2::info("Using user-provided socket list: {SOCKETS}", "SOCKETS",
                  *test_.custom_socket_list);
        launch_boot_assert();
        return;
    }

    if (!cpu_discoverer_)
    {
        log_to_results("INFO", "No CPU discoverer configured, skipping "
                               "discovery");
        lg2::info("No CPU discoverer configured, skipping discovery");
        launch_boot_assert();
        return;
    }

    log_to_results("INFO", "Starting CPU discovery via Entity Manager");
    lg2::info("Starting CPU discovery via Entity Manager");
    cpu_discoverer_(
        [self = shared_from_this()](const std::vector<std::string>& cpu_paths) {
            self->on_cpu_discovery_done(cpu_paths);
        });
}

void IstService::on_cpu_discovery_done(
    const std::vector<std::string>& cpu_paths)
{
    if (cpu_paths.empty())
    {
        lg2::error("No CPUs discovered via Entity Manager");
        fail_run_with_archive("category=DISCOVERY, reason=no_cpus_found");
        return;
    }

    std::vector<size_t> socket_ids;
    for (const auto& path : cpu_paths)
    {
        std::string cpu_name = fs::path(path).filename();
        auto pos = cpu_name.rfind('_');
        if (pos == std::string::npos || pos + 1 >= cpu_name.size())
        {
            log_to_results(
                "ERR",
                std::format("Cannot extract socket number from: {}", path));
            lg2::error("Cannot extract socket number from: {PATH}", "PATH",
                       path);
            fail_run_with_archive(
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
            log_to_results(
                "ERR",
                std::format("Cannot parse socket number from: {}", path));
            lg2::error("Cannot parse socket number from: {PATH}", "PATH", path);
            fail_run_with_archive(
                "category=DISCOVERY, reason=package_parse_failed");
            return;
        }
        socket_ids.push_back(id);
    }

    std::sort(socket_ids.begin(), socket_ids.end());

    // A single physical socket may be exposed under more than one inventory
    // path (e.g. .../system/component/CPU_0 and .../system/cpu/CPU_0). Collapse
    // duplicate socket numbers so the count and contiguity check reflect
    // physical sockets, not the number of inventory representations.
    socket_ids.erase(std::unique(socket_ids.begin(), socket_ids.end()),
                     socket_ids.end());

    for (size_t i = 0; i < socket_ids.size(); ++i)
    {
        if (socket_ids[i] != i)
        {
            log_to_results("ERR",
                           std::format("Non-contiguous CPU sockets detected "
                                       "(expected socket {}, found {})",
                                       i, socket_ids[i]));
            lg2::error("Non-contiguous CPU sockets detected (expected socket "
                       "{EXPECTED}, found {FOUND})",
                       "EXPECTED", i, "FOUND", socket_ids[i]);
            fail_run_with_archive(
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
    test_.custom_socket_list = std::move(socket_list);
    log_to_results("INFO",
                   std::format("Discovered {} CPU socket(s) from {} inventory "
                               "path(s), socket list: {}",
                               socket_ids.size(), cpu_paths.size(),
                               *test_.custom_socket_list));
    lg2::info("Discovered {COUNT} CPU socket(s) from {PATHS} inventory "
              "path(s), socket list: {SOCKETS}",
              "COUNT", socket_ids.size(), "PATHS", cpu_paths.size(), "SOCKETS",
              *test_.custom_socket_list);

    launch_boot_assert();
}

void IstService::launch_boot_assert()
{
    transition_to(IstStage::pending_ist_boot);

    const fs::path& hook_cmd = platform_cfg_.hooks.ist_boot_assert;
    hook_runner_->async_run(hook_cmd, "istBootAssert hook",
                            [self = shared_from_this()](bool hook_ok) {
                                self->on_ist_boot_assert_done(hook_ok);
                            });
}
