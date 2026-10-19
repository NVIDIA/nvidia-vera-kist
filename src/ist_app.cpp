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

static void emit_ist_error(std::string_view info)
{
    std::cerr << "IST_ERROR: " << info << '\n';
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
    remove_service_log_tee();
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
        remove_service_log_tee();
    }
    update_dbus_state();
}

void IstService::install_service_log_tee()
{
    if (orig_cout_buf_)
    {
        return;
    }

    fs::path log_path =
        platform_cfg_.storage.result_storage_path / "IST_service.log";
    if (!run_logs_.service.open(log_path))
    {
        return;
    }

    cout_tee_ = std::make_unique<TeeStreambuf>(std::cout.rdbuf(),
                                               run_logs_.service.fd());
    cerr_tee_ = std::make_unique<TeeStreambuf>(std::cerr.rdbuf(),
                                               run_logs_.service.fd());
    orig_cout_buf_ = std::cout.rdbuf(cout_tee_.get());
    orig_cerr_buf_ = std::cerr.rdbuf(cerr_tee_.get());
}

void IstService::remove_service_log_tee()
{
    if (orig_cout_buf_)
    {
        std::cout.rdbuf(orig_cout_buf_);
        orig_cout_buf_ = nullptr;
    }
    if (orig_cerr_buf_)
    {
        std::cerr.rdbuf(orig_cerr_buf_);
        orig_cerr_buf_ = nullptr;
    }
    cout_tee_.reset();
    cerr_tee_.reset();
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
    out.hook_dir = d["hookDirectory"].get<std::string>();

    std::error_code fs_ec;
    if (!fs::is_directory(out.hook_dir, fs_ec) || fs_ec)
    {
        std::cerr << "hookDirectory does not exist or is not a directory: "
                  << out.hook_dir << '\n';
        return false;
    }

    if (!is_path_within(out.hook_dir, fs::path(hook_root)))
    {
        std::cerr << "hookDirectory '" << out.hook_dir
                  << "' resolves outside the trusted hook root '" << hook_root
                  << "'\n";
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
                std::cerr << "Hook '" << name << "' has empty path\n";
                return false;
            }
            continue;
        }
        if (!is_path_within(hookPath, out.hook_dir))
        {
            std::cerr << "Hook '" << name << "' path '" << hookPath
                      << "' is outside hookDirectory '" << out.hook_dir
                      << "'\n";
            return false;
        }
    }

    return true;
}

void IstService::print_ist_platform_config() const
{
    std::cout << "istHookPath:    " << platform_cfg_.hook_dir << '\n';
    std::cout << "itmBinaryPath:  " << platform_cfg_.itm_binary_path << '\n';
    std::cout << "itmLibDir:      " << platform_cfg_.itm_lib_dir << '\n';
    std::cout << "hooks:\n";
    std::cout << "  istBootAssert = " << platform_cfg_.hooks.ist_boot_assert
              << '\n';
    std::cout << "  istBootDeassert = " << platform_cfg_.hooks.ist_boot_deassert
              << '\n';
    std::cout << "  resetSystem = " << platform_cfg_.hooks.reset_system << '\n';
    std::cout << "  errorCheck = " << platform_cfg_.hooks.error_check << '\n';
    std::cout << "storage:\n";
    std::cout << "  vectorMountPath = "
              << platform_cfg_.storage.vector_mount_path << '\n';
    std::cout << "  vectorStoragePath = "
              << platform_cfg_.storage.vector_storage_path << '\n';
    std::cout << "  resultStoragePath = "
              << platform_cfg_.storage.result_storage_path << '\n';
}

bool IstService::initialize(IstPlatformConfig cfg)
{
    if (initialized_)
    {
        std::cerr << "Already initialized\n";
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
                    std::cerr << "istBootDeassert on startup "
                                 "failed (non-fatal)\n";
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
            std::cerr << "Parameter '" << key << "' exceeds max length ("
                      << max_param_string_length << ")\n";
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
                std::cerr << "Parameter 'istSwTimeoutSec' has wrong type "
                             "(expected int)\n";
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
                std::cerr << "Parameter 'customTestList' has wrong type "
                             "(expected string)\n";
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
                std::cerr << "Parameter 'customSocketList' has wrong type "
                             "(expected string)\n";
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
                std::cerr << "Parameter 'istContinueOnFail' has wrong type "
                             "(expected bool)\n";
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
                std::cerr << "Parameter 'istSaveResOnPass' has wrong type "
                             "(expected bool)\n";
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
                std::cerr << "Parameter 'istSaveResOnFail' has wrong type "
                             "(expected bool)\n";
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

bool IstService::collateral_verification()
{
    if (platform_cfg_.storage.vector_mount_path.empty())
    {
        std::cerr << "vectorMountPath not configured in platform config!\n";
        return false;
    }
    std::error_code fs_ec;
    if (!fs::exists(platform_cfg_.storage.vector_mount_path, fs_ec) || fs_ec)
    {
        std::cerr << "Vector storage path '"
                  << platform_cfg_.storage.vector_mount_path
                  << "' does not exist or is inaccessible";
        if (fs_ec)
        {
            std::cerr << ": " << fs_ec.message();
        }
        std::cerr << '\n';
        return false;
    }

    resolve_itm_paths(platform_cfg_);

    if (!fs::exists(platform_cfg_.itm_binary_path, fs_ec) || fs_ec)
    {
        std::cerr << "IST binary '" << platform_cfg_.itm_binary_path
                  << "' not found; test vectors may not be mounted\n";
        return false;
    }

    fs::path golden_res =
        platform_cfg_.storage.vector_mount_path / "GOLDEN_RES";
    if (!fs::exists(golden_res, fs_ec) || fs_ec ||
        fs::is_empty(golden_res, fs_ec) || fs_ec)
    {
        std::cerr << "GOLDEN_RES not found or empty under '"
                  << platform_cfg_.storage.vector_mount_path
                  << "'; test vectors may not be mounted\n";
        return false;
    }

    return true;
}

bool IstService::prepare_result_storage()
{
    if (platform_cfg_.storage.result_storage_path.empty())
    {
        std::cerr << "resultStoragePath not configured in platform config!\n";
        return false;
    }
    std::error_code fs_ec;
    fs::create_directories(platform_cfg_.storage.result_storage_path, fs_ec);
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

void IstService::on_ist_boot_assert_done(bool ok)
{
    if (!ok)
    {
        std::cerr << "Failed to assert IST boot\n";
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
                    std::cerr << "Failed to remove marker '"
                              << entry.path().string() << "': " << ec.message()
                              << "\n";
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
        std::cerr << "Unable to detect power cycle, running cleanup\n";
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
            std::cerr << "CAK bypass script '" << cak_script.string()
                      << "' resolves outside hookDirectory '"
                      << platform_cfg_.hook_dir.string() << "'\n";
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
                    std::cerr << "CAK bypass hook failed, aborting IST\n";
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
        std::cout << "kist_itm exited with code " << itm_exit << '\n';
    }
    else
    {
        std::cerr << "Failed to execute IST (code=" << itm_exit << ")\n";
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
            std::cerr << "resetSystem hook not found, failing cleanup\n";
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
        std::cerr << "istBootDeassert hook failed during cleanup\n";
        emit_ist_error("category=HOOK, reason=ist_boot_deassert_failed");
    }

    fs::path results_dir = platform_cfg_.storage.result_storage_path;
    archive_results_async(
        io_, results_dir,
        [weak = weak_from_this(), itm_exit, ok_deassert](bool ok) {
            if (!ok)
            {
                std::cerr << "IST: results archiving failed\n";
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
            std::cerr << "IST: results archiving failed\n";
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
        std::cerr << "Another IST run is in progress\n";
        throw sdbusplus::exception::SdBusError(EBUSY, "IST already running");
    }
    if (vector_manager_->in_progress())
    {
        std::cerr << "Cannot start IST while update is in progress\n";
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
            std::cerr << "Failed to remove " << entry.path() << ": "
                      << rm_ec.message() << '\n';
        }
    }

    install_service_log_tee();

    signature_verifier_(platform_cfg_, [self = shared_from_this()](bool ok) {
        self->on_signature_verify_done(ok);
    });

    return current_run_path_;
}

void IstService::on_signature_verify_done(bool ok)
{
    if (!ok)
    {
        std::cerr << "IST binary/library signature verification failed\n";
        fail_run_with_archive(
            "category=SECURITY, reason=signature_verification_failed");
        return;
    }

    if (test_.custom_socket_list.has_value())
    {
        std::cout << "Using user-provided socket list: "
                  << *test_.custom_socket_list << '\n';
        launch_boot_assert();
        return;
    }

    if (!cpu_discoverer_)
    {
        std::cout << "No CPU discoverer configured, skipping discovery\n";
        launch_boot_assert();
        return;
    }

    std::cout << "Starting CPU discovery via Entity Manager\n";
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
        std::cerr << "No CPUs discovered via Entity Manager\n";
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
            std::cerr << "Cannot extract socket number from: " << path << '\n';
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
            std::cerr << "Cannot parse socket number from: " << path << '\n';
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
            std::cerr << "Non-contiguous CPU sockets detected (expected socket "
                      << i << ", found " << socket_ids[i] << ")\n";
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
    std::cout << "Discovered " << socket_ids.size() << " CPU socket(s) from "
              << cpu_paths.size() << " inventory path(s), socket list: "
              << *test_.custom_socket_list << '\n';

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
