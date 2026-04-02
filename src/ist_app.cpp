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

// ----------------
// Platform config
// ----------------

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

    if (d.contains("itmBinaryPath") && d["itmBinaryPath"].is_string())
    {
        out.itmBinaryPath = d["itmBinaryPath"].get<std::string>();
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

    // Best effort: Even if mount images fails here it may be due to the
    // test vectors already being mounted previously.
    mountImages();
    readAndPublishVersion();
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

bool IstService::collateralVerification(const ParamMap& test_params)
{
    if (!getISTParams(test_params))
    {
        std::cerr << "Failed to get IST test params\n";
        return false;
    }

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

    if (platformCfg_.storage.resultStoragePath.empty())
    {
        std::cerr << "resultStoragePath not configured in platform config!\n";
        return false;
    }
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
        transitionTo(IstStage::idle, IstStatus::failed);
        return;
    }

    transitionTo(IstStage::pendingPowerCycle);

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
        runIstCleanup(false);
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
            runIstCleanup(false);
            return;
        }
        hookRunner_->asyncRun(
            cak_script, "CAK bypass", [self = shared_from_this()](bool ok_cak) {
                if (!ok_cak)
                {
                    std::cerr << "CAK bypass hook failed, aborting IST\n";
                    self->runIstCleanup(false);
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
        [self = shared_from_this()](bool itm_ok) {
            self->runIstCleanup(itm_ok);
        },
        [self = shared_from_this()](uint8_t progress) {
            self->state_.progress = progress;
            self->publisher_->publishProgress(progress);
        });
}

void IstService::runIstCleanup(bool itm_ok)
{
    transitionTo(IstStage::cleanup);

    const fs::path& hook_cmd = platformCfg_.hooks.istBootDeassert;
    if (hook_cmd.empty())
    {
        std::cerr << "istBootDeassert hook not found, failing cleanup\n";
        transitionTo(IstStage::idle, IstStatus::failed);
        return;
    }

    hookRunner_->asyncRun(
        hook_cmd, "istBootDeassert hook",
        [self = shared_from_this(), itm_ok](bool ok_deassert) {
            self->onDeassertDone(itm_ok, ok_deassert);
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
        const fs::path& reset_cmd = platformCfg_.hooks.resetSystem;
        if (reset_cmd.empty())
        {
            std::cerr << "resetSystem hook not found, failing cleanup\n";
            transitionTo(IstStage::idle, IstStatus::failed);
            return;
        }
        hookRunner_->asyncRun(
            reset_cmd, "resetSystem hook",
            [self = shared_from_this(), itm_ok](bool ok_reset) {
                self->onResetDone(itm_ok, ok_reset);
            },
            {"--skip-cak"});
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

    const fs::path& hook_cmd = platformCfg_.hooks.istBootAssert;
    if (hook_cmd.empty())
    {
        transitionTo(IstStage::idle, IstStatus::failed);
        throw sdbusplus::exception::SdBusError(ENOENT,
                                               "istBootAssert hook not found");
    }

    transitionTo(IstStage::pendingIstBoot);

    hookRunner_->asyncRun(hook_cmd, "istBootAssert hook",
                          [self = shared_from_this()](bool ok) {
                              self->onIstBootAssertDone(ok);
                          });
}
