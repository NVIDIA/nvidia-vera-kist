#include <ist_app.hpp>
#include <nlohmann/json.hpp>
#include <sdbusplus/exception.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
using json = nlohmann::json;

static constexpr size_t max_param_string_length = 4096;

static constexpr int32_t min_sw_timeout_sec = 60;
static constexpr int32_t max_sw_timeout_sec = 7200;

// ---------------------
// Static helpers
// ---------------------

static void copy_json_strings(const json& obj,
                              std::unordered_map<std::string, fs::path>& dst)
{
    if (!obj.is_object())
    {
        return; // Non-object value (e.g. wrong type in JSON); skip silently
    }
    for (const auto& [key, val] : obj.items())
    {
        if (val.is_string())
        {
            dst[key] = val.get<std::string>();
        }
    }
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

IstService::IstService(std::unique_ptr<StatePublisher> publisher,
                       std::unique_ptr<HookRunner> hook_runner,
                       std::unique_ptr<HostPowerMonitor> power_monitor,
                       std::unique_ptr<ItmRunner> itm_runner) :
    publisher_(std::move(publisher)), hookRunner_(std::move(hook_runner)),
    powerMonitor_(std::move(power_monitor)), itmRunner_(std::move(itm_runner))
{}

// ----------------
// D-Bus state helpers
// ----------------

void IstService::updateDbusState()
{
    publisher_->publish(state_);
}

void IstService::transitionTo(IstStage stage)
{
    state_.stage = stage;
    updateDbusState();
}

void IstService::transitionTo(IstStage stage, IstStatus status)
{
    state_.stage = stage;
    state_.status = status;
    updateDbusState();
    if (status != IstStatus::inProgress)
    {
        publisher_->removeProgress();
    }
}

std::string IstService::lookupHook(const std::string& name) const
{
    std::unordered_map<std::string, fs::path>::const_iterator it =
        platformCfg_.hooks.find(name);
    if (it == platformCfg_.hooks.end())
    {
        std::cerr << "Hook '" << name << "' not found in platform config"
                  << '\n';
        return {};
    }
    return it->second;
}

// ----------------
// Platform config
// ----------------

bool IstService::parsePlatformConfig(IstPlatformConfig& out,
                                     const std::string& path)
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

    if (d.contains("itmBinaryPath") && d["itmBinaryPath"].is_string())
    {
        out.itmBinaryPath = d["itmBinaryPath"].get<std::string>();
    }

    copy_json_strings(d.value("hookPaths", json::object()), out.hooks);
    copy_json_strings(d.value("storageConfig", json::object()), out.storage);

    // Validate all hook paths are within hookDirectory
    for (const auto& [name, hookPath] : out.hooks)
    {
        if (hookPath.empty())
        {
            std::cerr << "Hook '" << name << "' has empty path\n";
            return false;
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
    std::cout << "hooks:\n";
    for (const auto& [key, val] : platformCfg_.hooks)
    {
        std::cout << "  " << key << " = " << val << '\n';
    }

    std::cout << "storage:\n";
    for (const auto& [key, val] : platformCfg_.storage)
    {
        std::cout << "  " << key << " = " << val << '\n';
    }
}

bool IstService::initialize(const std::string& path)
{
    if (initialized_)
    {
        std::cerr << "Already initialized\n";
        return false;
    }

    if (!parsePlatformConfig(platformCfg_, path))
    {
        return false; // parsePlatformConfig already logged the error
    }

    printIstPlatformConfig();

    // Deassert IST boot on startup as a safety measure (e.g. after crash)
    std::string hook_cmd = lookupHook("istBootDeassert");
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
            if (const std::string* s = std::get_if<std::string>(&val))
            {
                test_.continueOnFail = *s;
            }
            else
            {
                std::cerr << "Parameter 'istContinueOnFail' has wrong type "
                             "(expected string)\n";
                return false;
            }
        }
        else if (key == "istSaveResOnPass")
        {
            if (const std::string* s = std::get_if<std::string>(&val))
            {
                test_.saveResOnPass = *s;
            }
            else
            {
                std::cerr << "Parameter 'istSaveResOnPass' has wrong type "
                             "(expected string)\n";
                return false;
            }
        }
        else if (key == "istSaveResOnFail")
        {
            if (const std::string* s = std::get_if<std::string>(&val))
            {
                test_.saveResOnFail = *s;
            }
            else
            {
                std::cerr << "Parameter 'istSaveResOnFail' has wrong type "
                             "(expected string)\n";
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

bool IstService::collateralVerification(const ParamMap& test_params)
{
    if (!getISTParams(test_params))
    {
        std::cerr << "Failed to get IST test params\n";
        return false;
    }

    std::unordered_map<std::string, fs::path>::iterator it =
        platformCfg_.storage.find("vectorMountPath");
    if (it == platformCfg_.storage.end())
    {
        std::cerr << "Failed to find vectorMountPath in platform config!\n";
        return false;
    }
    std::error_code fs_ec;
    if (!fs::exists(it->second, fs_ec) || fs_ec)
    {
        std::cerr << "Vector storage path '" << it->second
                  << "' does not exist or is inaccessible";
        if (fs_ec)
        {
            std::cerr << ": " << fs_ec.message();
        }
        std::cerr << '\n';
        return false;
    }

    it = platformCfg_.storage.find("resultStoragePath");
    if (it == platformCfg_.storage.end())
    {
        std::cerr << "Failed to find resultStoragePath in platform config!\n";
        return false;
    }
    fs::create_directories(it->second, fs_ec);
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
        transitionTo(IstStage::idle, IstStatus::failed);
        return;
    }

    transitionTo(IstStage::pendingPowerCycle);

    powerMonitor_->asyncWaitForPowerCycle(
        [this](bool ok_power) { onPowerCycleDone(ok_power); });
}

void IstService::onPowerCycleDone(bool ok)
{
    if (!ok)
    {
        std::cerr << "Unable to detect power cycle, running cleanup\n";
        runIstCleanup(false);
        return;
    }

    transitionTo(IstStage::runningIst);

    itmRunner_->asyncRun(
        test_, platformCfg_, [this](bool itm_ok) { runIstCleanup(itm_ok); },
        [this](uint8_t progress) {
            state_.progress = progress;
            publisher_->publishProgress(progress);
        });
}

void IstService::runIstCleanup(bool itm_ok)
{
    transitionTo(IstStage::cleanup);

    std::string hook_cmd = lookupHook("istBootDeassert");
    if (hook_cmd.empty())
    {
        std::cerr << "istBootDeassert hook not found, failing cleanup\n";
        transitionTo(IstStage::idle, IstStatus::failed);
        return;
    }

    hookRunner_->asyncRun(hook_cmd, "istBootDeassert hook",
                          [this, itm_ok](bool ok_deassert) {
                              onDeassertDone(itm_ok, ok_deassert);
                          });
}

void IstService::onDeassertDone(bool itm_ok, bool ok_deassert)
{
    if (!ok_deassert)
    {
        std::cerr << "istBootDeassert hook failed during cleanup\n";
        transitionTo(IstStage::idle, IstStatus::failed);
        return;
    }

    if (test_.autoRebootOnComplete)
    {
        std::string reset_cmd = lookupHook("resetSystem");
        if (reset_cmd.empty())
        {
            std::cerr << "resetSystem hook not found, failing cleanup\n";
            transitionTo(IstStage::idle, IstStatus::failed);
            return;
        }
        hookRunner_->asyncRun(
            reset_cmd, "resetSystem hook",
            [this, itm_ok](bool ok_reset) { onResetDone(itm_ok, ok_reset); });
    }
    else
    {
        transitionTo(IstStage::idle,
                     itm_ok ? IstStatus::completed : IstStatus::failed);
    }
}

void IstService::onResetDone(bool itm_ok, bool ok_reset)
{
    if (!ok_reset)
    {
        transitionTo(IstStage::idle, IstStatus::failed);
    }
    else
    {
        transitionTo(IstStage::idle,
                     itm_ok ? IstStatus::completed : IstStatus::failed);
    }
}

// ----------------
// StartIST entry point
// ----------------

void IstService::startIST(const ParamMap& test_params)
{
    if (state_.stage != IstStage::idle)
    {
        std::cerr << "Another IST run is in progress\n";
        throw sdbusplus::exception::SdBusError(EBUSY, "IST already running");
    }

    state_.progress = 0;
    publisher_->createProgress();
    transitionTo(IstStage::collateralVerification, IstStatus::inProgress);

    if (!collateralVerification(test_params))
    {
        transitionTo(IstStage::idle, IstStatus::aborted);
        throw sdbusplus::exception::SdBusError(
            EINVAL, "Collateral verification failed");
    }

    std::string hook_cmd = lookupHook("istBootAssert");
    if (hook_cmd.empty())
    {
        transitionTo(IstStage::idle, IstStatus::failed);
        throw sdbusplus::exception::SdBusError(ENOENT,
                                               "istBootAssert hook not found");
    }

    transitionTo(IstStage::pendingIstBoot);

    hookRunner_->asyncRun(hook_cmd, "istBootAssert hook",
                          [this](bool ok) { onIstBootAssertDone(ok); });
}
