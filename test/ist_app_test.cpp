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
#include <sys/socket.h>
#include <unistd.h>

#include <ist_app.hpp>
#include <ist_errors.hpp>
#include <ist_results.hpp>
#include <sdbusplus/exception.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace fs = std::filesystem;
using ::testing::NiceMock;
using ::testing::StrEq;

namespace ist_err = sdbusplus::error::com::nvidia::vera::ist;

using DoneCb = std::move_only_function<void(bool) const>;
using ItmDoneCb = std::move_only_function<void(int) const>;
using ProgressCb = std::move_only_function<void(uint8_t) const>;

static std::pair<int, int> make_upload_socket_pair()
{
    int fds[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) != 0)
    {
        throw std::system_error(errno, std::generic_category(), "socketpair");
    }
    return {fds[0], fds[1]};
}

// ----------------
// Mock implementations for dependency injection
// ----------------

class MockHookRunner : public HookRunner
{
  public:
    MOCK_METHOD(void, asyncRun,
                (const std::string& cmd, std::string what,
                 std::move_only_function<void(bool ok) const> done,
                 std::vector<std::string> args, std::chrono::seconds timeout),
                (override));
};

class MockHostPowerMonitor : public HostPowerMonitor
{
  public:
    MOCK_METHOD(void, asyncWaitForPowerCycle,
                (std::move_only_function<void(bool ok) const> done),
                (override));
};

class MockItmRunner : public ItmRunner
{
  public:
    MOCK_METHOD(void, asyncRun,
                (const IstTestConfig& cfg,
                 const IstPlatformConfig& platform_cfg,
                 std::move_only_function<void(int exit_code) const> done,
                 std::move_only_function<void(uint8_t) const> on_progress),
                (override));
};

class MockStatePublisher : public StatePublisher
{
  public:
    MOCK_METHOD(void, createRunObject,
                (const std::string& run_path, ResultsFdCb results_fd_cb),
                (override));
    MOCK_METHOD(void, removeRunObject, (), (override));
    MOCK_METHOD(void, publish, (const IstState& state), (override));
    MOCK_METHOD(void, reSignalStage, (), (override));
    MOCK_METHOD(void, publishProgress, (uint8_t progress), (override));
    MOCK_METHOD(void, publishVersion, (const std::string& version), (override));
    MOCK_METHOD(void, publishActivation, (std::string_view state), (override));
    MOCK_METHOD(void, createActivationProgress, (), (override));
    MOCK_METHOD(void, publishActivationProgress, (uint8_t progress),
                (override));
    MOCK_METHOD(void, removeActivationProgress, (), (override));
    MOCK_METHOD(void, emitEventLog,
                (const std::string& message, const std::string& severity,
                 (const std::map<std::string, std::string>& additional_data)),
                (override));
};

// ----------------
// Pure unit tests (no D-Bus needed)
// ----------------

TEST(IstStageTest, AllStagesToString)
{
    EXPECT_EQ(istStageToString(IstStage::idle), "Idle");
    EXPECT_EQ(istStageToString(IstStage::collateralVerification),
              "CollateralVerification");
    EXPECT_EQ(istStageToString(IstStage::pendingIstBoot), "PendingISTBoot");
    EXPECT_EQ(istStageToString(IstStage::pendingPowerCycle),
              "PendingPowerCycle");
    EXPECT_EQ(istStageToString(IstStage::runningIst), "RunningIST");
    EXPECT_EQ(istStageToString(IstStage::cleanup), "Cleanup");
}

TEST(IstStatusTest, AllStatusesToString)
{
    EXPECT_EQ(istStatusToString(IstStatus::inProgress), "InProgress");
    EXPECT_EQ(istStatusToString(IstStatus::completed), "Completed");
    EXPECT_EQ(istStatusToString(IstStatus::failed), "Failed");
    EXPECT_EQ(istStatusToString(IstStatus::aborted), "Aborted");
}

TEST(IstStateTest, DefaultValues)
{
    IstState state;
    EXPECT_EQ(state.progress, 0);
    EXPECT_EQ(state.status, IstStatus::completed);
    EXPECT_EQ(state.stage, IstStage::idle);
}

TEST(IstTestConfigTest, DefaultValues)
{
    IstTestConfig cfg;
    EXPECT_FALSE(cfg.customTestList.has_value());
    EXPECT_FALSE(cfg.customSocketList.has_value());
    EXPECT_FALSE(cfg.swTimeoutSec.has_value());
    EXPECT_FALSE(cfg.continueOnFail.has_value());
    EXPECT_FALSE(cfg.saveResOnFail.has_value());
    EXPECT_FALSE(cfg.saveResOnPass.has_value());
    EXPECT_FALSE(cfg.autoRebootOnComplete);
}

TEST(IstPlatformConfigTest, DefaultValues)
{
    IstPlatformConfig cfg;
    EXPECT_TRUE(cfg.softwareInventoryId.empty());
    EXPECT_TRUE(cfg.hookDir.empty());
    EXPECT_TRUE(cfg.hooks.istBootAssert.empty());
    EXPECT_TRUE(cfg.hooks.istBootDeassert.empty());
    EXPECT_TRUE(cfg.hooks.resetSystem.empty());
    EXPECT_TRUE(cfg.hooks.errorCheck.empty());
    EXPECT_TRUE(cfg.storage.vectorMountPath.empty());
    EXPECT_TRUE(cfg.storage.vectorStoragePath.empty());
    EXPECT_TRUE(cfg.storage.resultStoragePath.empty());
    EXPECT_TRUE(cfg.itmLibDir.empty());
    EXPECT_EQ(cfg.transferInactivityTimeout, std::chrono::seconds(60));
}

// ----------------
// resolveItmPaths tests
// ----------------

TEST(ResolveItmPathsTest, EmptyVectorMountPathIsNoop)
{
    IstPlatformConfig cfg;
    cfg.archSubDir = "aarch64";
    fs::path original = cfg.itmBinaryPath;
    resolveItmPaths(cfg);
    EXPECT_EQ(cfg.itmBinaryPath, original);
    EXPECT_TRUE(cfg.itmLibDir.empty());
}

TEST(ResolveItmPathsTest, FallsBackToFlatLayoutWhenNoArchSubDir)
{
    auto tmp_dir = fs::temp_directory_path() /
                   ("resolve_test_flat_" + std::to_string(getpid()));
    fs::create_directories(tmp_dir / "vectors");

    IstPlatformConfig cfg;
    cfg.storage.vectorMountPath = tmp_dir / "vectors";
    resolveItmPaths(cfg);

    EXPECT_EQ(cfg.itmBinaryPath, tmp_dir / "vectors" / "kist_itm");
    EXPECT_TRUE(cfg.itmLibDir.empty());

    fs::remove_all(tmp_dir);
}

TEST(ResolveItmPathsTest, FallsBackToFlatLayoutWhenArchDirMissing)
{
    auto tmp_dir = fs::temp_directory_path() /
                   ("resolve_test_missing_" + std::to_string(getpid()));
    fs::create_directories(tmp_dir / "vectors");

    IstPlatformConfig cfg;
    cfg.storage.vectorMountPath = tmp_dir / "vectors";
    cfg.archSubDir = "aarch64";
    resolveItmPaths(cfg);

    EXPECT_EQ(cfg.itmBinaryPath, tmp_dir / "vectors" / "kist_itm");
    EXPECT_TRUE(cfg.itmLibDir.empty());

    fs::remove_all(tmp_dir);
}

TEST(ResolveItmPathsTest, UsesArchSpecificPathsWhenArchDirExists)
{
    auto tmp_dir = fs::temp_directory_path() /
                   ("resolve_test_arch_" + std::to_string(getpid()));
    fs::create_directories(tmp_dir / "vectors" / "kist_itm" / "aarch64" /
                           "bin");
    fs::create_directories(tmp_dir / "vectors" / "kist_itm" / "aarch64" /
                           "lib");

    IstPlatformConfig cfg;
    cfg.storage.vectorMountPath = tmp_dir / "vectors";
    cfg.archSubDir = "aarch64";
    resolveItmPaths(cfg);

    EXPECT_EQ(cfg.itmBinaryPath, tmp_dir / "vectors" / "kist_itm" / "aarch64" /
                                     "bin" / "kist_itm");
    EXPECT_EQ(cfg.itmLibDir,
              tmp_dir / "vectors" / "kist_itm" / "aarch64" / "lib");

    fs::remove_all(tmp_dir);
}

TEST(ResolveItmPathsTest, ClearsLibDirOnFallback)
{
    auto tmp_dir = fs::temp_directory_path() /
                   ("resolve_test_clear_" + std::to_string(getpid()));
    fs::create_directories(tmp_dir / "vectors" / "kist_itm" / "aarch64" /
                           "bin");
    fs::create_directories(tmp_dir / "vectors" / "kist_itm" / "aarch64" /
                           "lib");

    IstPlatformConfig cfg;
    cfg.storage.vectorMountPath = tmp_dir / "vectors";
    cfg.archSubDir = "aarch64";
    resolveItmPaths(cfg);
    EXPECT_FALSE(cfg.itmLibDir.empty());

    // Simulate fallback by removing the arch dir and re-resolving
    fs::remove_all(tmp_dir / "vectors" / "kist_itm" / "aarch64");
    resolveItmPaths(cfg);

    EXPECT_EQ(cfg.itmBinaryPath, tmp_dir / "vectors" / "kist_itm");
    EXPECT_TRUE(cfg.itmLibDir.empty());

    fs::remove_all(tmp_dir);
}

// ----------------
// IstService test fixture
// ----------------

class IstServiceTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        // Create unique temp directory
        tmpDir_ = fs::temp_directory_path() /
                  ("ist_test_" + std::to_string(getpid()) + "_" +
                   std::to_string(counter++));
        fs::create_directories(tmpDir_ / "vectors" / "GOLDEN_RES");
        fs::create_directories(tmpDir_ / "storage");
        fs::create_directories(tmpDir_ / "results");
        fs::create_directories(tmpDir_ / "hooks");

        // Create expected collateral so startIST() passes verification
        std::ofstream itm_stub(tmpDir_ / "vectors" / "kist_itm");
        std::ofstream golden_stub(tmpDir_ / "vectors" / "GOLDEN_RES" /
                                  "placeholder");

        // Create hook files so canonical() can resolve them
        for (const auto& name : {"assert.sh", "deassert.sh", "reset.sh"})
        {
            std::ofstream(tmpDir_ / "hooks" / name);
        }

        // Write a valid platform config
        configPath_ = (tmpDir_ / "platform_cfg.json").string();
        write_config(R"({
            "hookDirectory": ")" +
                     (tmpDir_ / "hooks").string() + R"(",
            "hookPaths": {
                "istBootAssert": ")" +
                     (tmpDir_ / "hooks/assert.sh").string() + R"(",
                "istBootDeassert": ")" +
                     (tmpDir_ / "hooks/deassert.sh").string() + R"(",
                "resetSystem": ")" +
                     (tmpDir_ / "hooks/reset.sh").string() + R"("
            },
            "storageConfig": {
                "vectorMountPath": ")" +
                     (tmpDir_ / "vectors").string() + R"(",
                "vectorStoragePath": ")" +
                     (tmpDir_ / "storage").string() + R"(",
                "resultStoragePath": ")" +
                     (tmpDir_ / "results").string() + R"("
            },
            "softwareInventoryId": "IST_Vectors"
        })");

        create_service();
    }

    void create_service()
    {
        auto publisher = std::make_unique<NiceMock<MockStatePublisher>>();
        auto hook_runner = std::make_unique<NiceMock<MockHookRunner>>();
        auto power_monitor = std::make_shared<NiceMock<MockHostPowerMonitor>>();
        auto itm_runner = std::make_unique<NiceMock<MockItmRunner>>();

        publisher_ = publisher.get();
        hookRunner_ = hook_runner.get();
        powerMonitor_ = power_monitor.get();
        itmRunner_ = itm_runner.get();

        service_ = IstService::create(
            io_, std::move(publisher), std::move(hook_runner),
            std::move(power_monitor), std::move(itm_runner));
    }

    bool init_from_file(const std::string& path)
    {
        IstPlatformConfig cfg;
        if (!parsePlatformConfig(cfg, path))
        {
            return false;
        }
        service_->setSignatureVerifier(
            [](const IstPlatformConfig&, DoneCb cb) { cb(true); });
        return service_->initialize(std::move(cfg));
    }

    void write_config(const std::string& content)
    {
        std::ofstream f(configPath_);
        f << content;
    }

    void TearDown() override
    {
        service_.reset();
        fs::remove_all(tmpDir_);
    }

    boost::asio::io_context io_;
    std::shared_ptr<IstService> service_;

    MockStatePublisher* publisher_ = nullptr;
    MockHookRunner* hookRunner_ = nullptr;
    MockHostPowerMonitor* powerMonitor_ = nullptr;
    MockItmRunner* itmRunner_ = nullptr;

    fs::path tmpDir_;
    std::string configPath_;

    std::string start_ist(const ParamMap& params = {})
    {
        auto path = service_->startIST(params);
        io_.run();
        io_.restart();
        return path;
    }

    int begin_start_update(std::string* object_path = nullptr)
    {
        auto [readFd, writeFd] = make_upload_socket_pair();
        std::string path =
            service_->startUpdate(readFd, k_apply_time_immediate);
        if (object_path != nullptr)
        {
            *object_path = std::move(path);
        }
        return writeFd;
    }

    static inline int counter = 0;
};

// ----------------
// Initialize tests
// ----------------

TEST_F(IstServiceTest, InitializeValidConfig)
{
    EXPECT_TRUE(init_from_file(configPath_));
}

TEST_F(IstServiceTest, InitializeInvalidPath)
{
    EXPECT_FALSE(init_from_file("/nonexistent/path.json"));
}

TEST_F(IstServiceTest, InitializeMalformedJson)
{
    write_config("not valid json {{{");
    EXPECT_FALSE(init_from_file(configPath_));
}

TEST_F(IstServiceTest, InitializeMissingHookDirectory)
{
    write_config(R"({ "storageConfig": {} })");
    EXPECT_FALSE(init_from_file(configPath_));
}

TEST_F(IstServiceTest, InitializeRejectsDoubleInit)
{
    EXPECT_TRUE(init_from_file(configPath_));
    EXPECT_FALSE(init_from_file(configPath_));
}

TEST_F(IstServiceTest, SoftwareInventoryIdMissingFails)
{
    write_config(R"({
        "hookDirectory": ")" +
                 (tmpDir_ / "hooks").string() + R"(",
        "hookPaths": {
            "istBootAssert": ")" +
                 (tmpDir_ / "hooks/assert.sh").string() + R"(",
            "istBootDeassert": ")" +
                 (tmpDir_ / "hooks/deassert.sh").string() + R"(",
            "resetSystem": ")" +
                 (tmpDir_ / "hooks/reset.sh").string() + R"("
        },
        "storageConfig": {
            "vectorMountPath": ")" +
                 (tmpDir_ / "vectors").string() + R"(",
            "vectorStoragePath": ")" +
                 (tmpDir_ / "storage").string() + R"(",
            "resultStoragePath": ")" +
                 (tmpDir_ / "results").string() + R"("
        }
    })");
    EXPECT_FALSE(init_from_file(configPath_));
}

TEST_F(IstServiceTest, SoftwareInventoryIdCustomValue)
{
    write_config(R"({
        "hookDirectory": ")" +
                 (tmpDir_ / "hooks").string() + R"(",
        "hookPaths": {
            "istBootAssert": ")" +
                 (tmpDir_ / "hooks/assert.sh").string() + R"(",
            "istBootDeassert": ")" +
                 (tmpDir_ / "hooks/deassert.sh").string() + R"(",
            "resetSystem": ")" +
                 (tmpDir_ / "hooks/reset.sh").string() + R"("
        },
        "storageConfig": {
            "vectorMountPath": ")" +
                 (tmpDir_ / "vectors").string() + R"(",
            "vectorStoragePath": ")" +
                 (tmpDir_ / "storage").string() + R"(",
            "resultStoragePath": ")" +
                 (tmpDir_ / "results").string() + R"("
        },
        "softwareInventoryId": "Custom_Vectors"
    })");
    EXPECT_TRUE(init_from_file(configPath_));
    EXPECT_EQ(service_->softwareInventoryId(), "Custom_Vectors");
}

TEST_F(IstServiceTest, SoftwareInventoryIdRejectsInvalidChars)
{
    write_config(R"({
        "hookDirectory": ")" +
                 (tmpDir_ / "hooks").string() + R"(",
        "hookPaths": {
            "istBootAssert": ")" +
                 (tmpDir_ / "hooks/assert.sh").string() + R"(",
            "istBootDeassert": ")" +
                 (tmpDir_ / "hooks/deassert.sh").string() + R"(",
            "resetSystem": ")" +
                 (tmpDir_ / "hooks/reset.sh").string() + R"("
        },
        "storageConfig": {
            "vectorMountPath": ")" +
                 (tmpDir_ / "vectors").string() + R"(",
            "vectorStoragePath": ")" +
                 (tmpDir_ / "storage").string() + R"(",
            "resultStoragePath": ")" +
                 (tmpDir_ / "results").string() + R"("
        },
        "softwareInventoryId": "bad/path"
    })");
    EXPECT_FALSE(init_from_file(configPath_));
}

TEST_F(IstServiceTest, SoftwareInventoryIdRejectsDash)
{
    write_config(R"({
        "hookDirectory": ")" +
                 (tmpDir_ / "hooks").string() + R"(",
        "hookPaths": {
            "istBootAssert": ")" +
                 (tmpDir_ / "hooks/assert.sh").string() + R"(",
            "istBootDeassert": ")" +
                 (tmpDir_ / "hooks/deassert.sh").string() + R"(",
            "resetSystem": ")" +
                 (tmpDir_ / "hooks/reset.sh").string() + R"("
        },
        "storageConfig": {
            "vectorMountPath": ")" +
                 (tmpDir_ / "vectors").string() + R"(",
            "vectorStoragePath": ")" +
                 (tmpDir_ / "storage").string() + R"(",
            "resultStoragePath": ")" +
                 (tmpDir_ / "results").string() + R"("
        },
        "softwareInventoryId": "bad-id"
    })");
    EXPECT_FALSE(init_from_file(configPath_));
}

TEST_F(IstServiceTest, SoftwareInventoryIdRejectsSpace)
{
    write_config(R"({
        "hookDirectory": ")" +
                 (tmpDir_ / "hooks").string() + R"(",
        "hookPaths": {
            "istBootAssert": ")" +
                 (tmpDir_ / "hooks/assert.sh").string() + R"(",
            "istBootDeassert": ")" +
                 (tmpDir_ / "hooks/deassert.sh").string() + R"(",
            "resetSystem": ")" +
                 (tmpDir_ / "hooks/reset.sh").string() + R"("
        },
        "storageConfig": {
            "vectorMountPath": ")" +
                 (tmpDir_ / "vectors").string() + R"(",
            "vectorStoragePath": ")" +
                 (tmpDir_ / "storage").string() + R"(",
            "resultStoragePath": ")" +
                 (tmpDir_ / "results").string() + R"("
        },
        "softwareInventoryId": "bad id"
    })");
    EXPECT_FALSE(init_from_file(configPath_));
}

TEST_F(IstServiceTest, SoftwareInventoryIdRejectsDot)
{
    write_config(R"({
        "hookDirectory": ")" +
                 (tmpDir_ / "hooks").string() + R"(",
        "hookPaths": {
            "istBootAssert": ")" +
                 (tmpDir_ / "hooks/assert.sh").string() + R"(",
            "istBootDeassert": ")" +
                 (tmpDir_ / "hooks/deassert.sh").string() + R"(",
            "resetSystem": ")" +
                 (tmpDir_ / "hooks/reset.sh").string() + R"("
        },
        "storageConfig": {
            "vectorMountPath": ")" +
                 (tmpDir_ / "vectors").string() + R"(",
            "vectorStoragePath": ")" +
                 (tmpDir_ / "storage").string() + R"(",
            "resultStoragePath": ")" +
                 (tmpDir_ / "results").string() + R"("
        },
        "softwareInventoryId": "bad.id"
    })");
    EXPECT_FALSE(init_from_file(configPath_));
}

TEST_F(IstServiceTest, SoftwareInventoryIdRejectsEmpty)
{
    write_config(R"({
        "hookDirectory": ")" +
                 (tmpDir_ / "hooks").string() + R"(",
        "hookPaths": {
            "istBootAssert": ")" +
                 (tmpDir_ / "hooks/assert.sh").string() + R"(",
            "istBootDeassert": ")" +
                 (tmpDir_ / "hooks/deassert.sh").string() + R"(",
            "resetSystem": ")" +
                 (tmpDir_ / "hooks/reset.sh").string() + R"("
        },
        "storageConfig": {
            "vectorMountPath": ")" +
                 (tmpDir_ / "vectors").string() + R"(",
            "vectorStoragePath": ")" +
                 (tmpDir_ / "storage").string() + R"(",
            "resultStoragePath": ")" +
                 (tmpDir_ / "results").string() + R"("
        },
        "softwareInventoryId": ""
    })");
    EXPECT_FALSE(init_from_file(configPath_));
}

TEST_F(IstServiceTest, SoftwareInventoryIdAcceptsUnderscoresAndDigits)
{
    write_config(R"({
        "hookDirectory": ")" +
                 (tmpDir_ / "hooks").string() + R"(",
        "hookPaths": {
            "istBootAssert": ")" +
                 (tmpDir_ / "hooks/assert.sh").string() + R"(",
            "istBootDeassert": ")" +
                 (tmpDir_ / "hooks/deassert.sh").string() + R"(",
            "resetSystem": ")" +
                 (tmpDir_ / "hooks/reset.sh").string() + R"("
        },
        "storageConfig": {
            "vectorMountPath": ")" +
                 (tmpDir_ / "vectors").string() + R"(",
            "vectorStoragePath": ")" +
                 (tmpDir_ / "storage").string() + R"(",
            "resultStoragePath": ")" +
                 (tmpDir_ / "results").string() + R"("
        },
        "softwareInventoryId": "___123___"
    })");
    EXPECT_TRUE(init_from_file(configPath_));
    EXPECT_EQ(service_->softwareInventoryId(), "___123___");
}

TEST_F(IstServiceTest, StartUpdateBeforeInitializeThrows)
{
    EXPECT_THROW(service_->startUpdate(-1, k_apply_time_immediate),
                 ist_err::Unavailable);
}

TEST_F(IstServiceTest, InitializeRejectsHookOutsideHookDir)
{
    write_config(R"({
        "hookDirectory": ")" +
                 (tmpDir_ / "hooks").string() + R"(",
        "hookPaths": {
            "istBootAssert": "/etc/shadow"
        },
        "storageConfig": {},
        "softwareInventoryId": "IST_Vectors"
    })");
    EXPECT_FALSE(init_from_file(configPath_));
}

// ----------------
// StartIST tests
// ----------------

TEST_F(IstServiceTest, StartIstRejectsWhenInProgress)
{
    init_from_file(configPath_);

    ParamMap params;
    std::string run_path = start_ist(params);
    EXPECT_FALSE(run_path.empty());
    EXPECT_EQ(service_->state().status, IstStatus::inProgress);

    ParamMap params2;
    EXPECT_THROW(service_->startIST(params2), sdbusplus::exception::SdBusError);
}

TEST_F(IstServiceTest, StartIstRejectsWhileUpdateInProgress)
{
    init_from_file(configPath_);
    int fd = begin_start_update();

    ParamMap params;
    EXPECT_THROW(service_->startIST(params), sdbusplus::exception::SdBusError);
    EXPECT_EQ(service_->state().stage, IstStage::idle);

    ::close(fd);
    io_.run();
}

TEST_F(IstServiceTest, StartIstAbortsOnMissingVectorStorage)
{
    // Remove the vectors directory so collateral verification fails
    fs::remove_all(tmpDir_ / "vectors");

    init_from_file(configPath_);

    ParamMap params;
    EXPECT_THROW(service_->startIST(params), ist_err::CollateralNotFound);
    EXPECT_EQ(service_->state().status, IstStatus::aborted);
    EXPECT_EQ(service_->state().stage, IstStage::idle);
}

TEST_F(IstServiceTest, StartIstAbortsOnMissingItmBinary)
{
    fs::remove(tmpDir_ / "vectors" / "kist_itm");

    init_from_file(configPath_);

    ParamMap params;
    EXPECT_THROW(service_->startIST(params), ist_err::CollateralNotFound);
    EXPECT_EQ(service_->state().status, IstStatus::aborted);
    EXPECT_EQ(service_->state().stage, IstStage::idle);
}

TEST_F(IstServiceTest, StartIstAbortsOnMissingGoldenRes)
{
    fs::remove_all(tmpDir_ / "vectors" / "GOLDEN_RES");

    init_from_file(configPath_);

    ParamMap params;
    EXPECT_THROW(service_->startIST(params), ist_err::CollateralNotFound);
    EXPECT_EQ(service_->state().status, IstStatus::aborted);
    EXPECT_EQ(service_->state().stage, IstStage::idle);
}

TEST_F(IstServiceTest, StartIstAbortsOnEmptyGoldenRes)
{
    fs::remove_all(tmpDir_ / "vectors" / "GOLDEN_RES");
    fs::create_directories(tmpDir_ / "vectors" / "GOLDEN_RES");

    init_from_file(configPath_);

    ParamMap params;
    EXPECT_THROW(service_->startIST(params), ist_err::CollateralNotFound);
    EXPECT_EQ(service_->state().status, IstStatus::aborted);
    EXPECT_EQ(service_->state().stage, IstStage::idle);
}

TEST_F(IstServiceTest, StartIstAbortsOnUnknownParam)
{
    init_from_file(configPath_);

    ParamMap params;
    params["unknownParam"] = std::string("value");
    EXPECT_THROW(service_->startIST(params), ist_err::InvalidParameter);
    EXPECT_EQ(service_->state().status, IstStatus::aborted);
}

TEST_F(IstServiceTest, StartIstAbortsOnOversizedParam)
{
    init_from_file(configPath_);

    ParamMap params;
    params["customTestList"] = std::string(5000, 'A'); // exceeds 4096 limit
    EXPECT_THROW(service_->startIST(params), ist_err::InvalidParameter);
    EXPECT_EQ(service_->state().status, IstStatus::aborted);
}

TEST_F(IstServiceTest, StartIstCallsAssertHook)
{
    init_from_file(configPath_);

    // Expect the istBootAssert hook to be called
    std::move_only_function<void(bool) const> assert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&,
                      std::chrono::seconds) { assert_done = std::move(done); });

    ParamMap params;
    start_ist(params);

    EXPECT_EQ(service_->state().status, IstStatus::inProgress);
    EXPECT_EQ(service_->state().stage, IstStage::pendingIstBoot);

    // Verify callback was captured
    ASSERT_TRUE(assert_done);
}

TEST_F(IstServiceTest, AssertHookFailureTransitionsToFailed)
{
    init_from_file(configPath_);

    std::move_only_function<void(bool) const> assert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&,
                      std::chrono::seconds) { assert_done = std::move(done); });

    ParamMap params;
    start_ist(params);

    // Simulate hook failure
    assert_done(false);

    EXPECT_EQ(service_->state().status, IstStatus::failed);
    EXPECT_EQ(service_->state().stage, IstStage::idle);
}

TEST_F(IstServiceTest, AssertHookSuccessWaitsForPowerCycle)
{
    init_from_file(configPath_);

    std::move_only_function<void(bool) const> assert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&,
                      std::chrono::seconds) { assert_done = std::move(done); });

    std::move_only_function<void(bool) const> power_done;
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([&](DoneCb done) { power_done = std::move(done); });

    ParamMap params;
    start_ist(params);

    // Simulate hook success → should start power cycle wait
    assert_done(true);

    EXPECT_EQ(service_->state().stage, IstStage::pendingPowerCycle);
    ASSERT_TRUE(power_done);
}

TEST_F(IstServiceTest, PendingPowerCycleReSignalsStageAfterDelay)
{
    init_from_file(configPath_);

    std::move_only_function<void(bool) const> assert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&,
                      std::chrono::seconds) { assert_done = std::move(done); });

    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([](DoneCb) {});

    ParamMap params;
    start_ist(params);

    EXPECT_CALL(*publisher_, reSignalStage()).Times(1);

    assert_done(true);
    EXPECT_EQ(service_->state().stage, IstStage::pendingPowerCycle);

    // Advance past the 2-second re-signal timer
    io_.run_for(std::chrono::seconds(3));
}

TEST_F(IstServiceTest, ReSignalTimerCancelledOnAssertFailure)
{
    init_from_file(configPath_);

    std::move_only_function<void(bool) const> assert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&,
                      std::chrono::seconds) { assert_done = std::move(done); });

    ParamMap params;
    start_ist(params);

    EXPECT_CALL(*publisher_, reSignalStage()).Times(0);

    assert_done(false);
    EXPECT_EQ(service_->state().stage, IstStage::idle);

    io_.run_for(std::chrono::seconds(3));
}

TEST_F(IstServiceTest, PowerCycleFailureRunsDeassertThenFails)
{
    init_from_file(configPath_);

    std::move_only_function<void(bool) const> assert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&,
                      std::chrono::seconds) { assert_done = std::move(done); });

    std::move_only_function<void(bool) const> power_done;
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([&](DoneCb done) { power_done = std::move(done); });

    std::move_only_function<void(bool) const> deassert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootDeassert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&, std::chrono::seconds) {
            deassert_done = std::move(done);
        });

    ParamMap params;
    start_ist(params);
    assert_done(true);

    // Simulate power cycle failure → should trigger cleanup/deassert
    power_done(false);
    io_.run();
    io_.restart();

    EXPECT_EQ(service_->state().stage, IstStage::cleanup);
    ASSERT_TRUE(deassert_done);

    deassert_done(true);

    EXPECT_EQ(service_->state().status, IstStatus::failed);
    EXPECT_EQ(service_->state().stage, IstStage::idle);
}

TEST_F(IstServiceTest, PowerCycleSuccessStartsItm)
{
    init_from_file(configPath_);

    std::move_only_function<void(bool) const> assert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&,
                      std::chrono::seconds) { assert_done = std::move(done); });

    std::move_only_function<void(bool) const> power_done;
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([&](DoneCb done) { power_done = std::move(done); });

    ItmDoneCb itm_done;
    EXPECT_CALL(*itmRunner_, asyncRun(::testing::_, ::testing::_, ::testing::_,
                                      ::testing::_))
        .WillOnce([&](const IstTestConfig&, const IstPlatformConfig&,
                      ItmDoneCb done,
                      ProgressCb) { itm_done = std::move(done); });

    ParamMap params;
    start_ist(params);
    assert_done(true);
    power_done(true);

    EXPECT_EQ(service_->state().stage, IstStage::runningIst);
    ASSERT_TRUE(itm_done);
}

TEST_F(IstServiceTest, CakBypassScriptRunsWhenPresent)
{
    std::ofstream give_me_a_name(tmpDir_ / "hooks" / "ist_cak_bypass.sh");
    init_from_file(configPath_);

    std::move_only_function<void(bool) const> assert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&,
                      std::chrono::seconds) { assert_done = std::move(done); });

    std::move_only_function<void(bool) const> power_done;
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([&](DoneCb done) { power_done = std::move(done); });

    std::move_only_function<void(bool) const> cak_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("CAK bypass"), ::testing::_,
                         ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&,
                      std::chrono::seconds) { cak_done = std::move(done); });

    ItmDoneCb itm_done;
    EXPECT_CALL(*itmRunner_, asyncRun(::testing::_, ::testing::_, ::testing::_,
                                      ::testing::_))
        .WillOnce([&](const IstTestConfig&, const IstPlatformConfig&,
                      ItmDoneCb done,
                      ProgressCb) { itm_done = std::move(done); });

    ParamMap params;
    start_ist(params);
    assert_done(true);
    power_done(true);

    ASSERT_TRUE(cak_done);
    EXPECT_FALSE(itm_done);

    cak_done(true);

    EXPECT_EQ(service_->state().stage, IstStage::runningIst);
    ASSERT_TRUE(itm_done);
}

TEST_F(IstServiceTest, CakBypassFailureAbortsIst)
{
    std::ofstream give_me_a_name(tmpDir_ / "hooks" / "ist_cak_bypass.sh");
    init_from_file(configPath_);

    std::move_only_function<void(bool) const> assert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&,
                      std::chrono::seconds) { assert_done = std::move(done); });

    std::move_only_function<void(bool) const> power_done;
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([&](DoneCb done) { power_done = std::move(done); });

    std::move_only_function<void(bool) const> cak_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("CAK bypass"), ::testing::_,
                         ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&,
                      std::chrono::seconds) { cak_done = std::move(done); });

    EXPECT_CALL(*itmRunner_, asyncRun(::testing::_, ::testing::_, ::testing::_,
                                      ::testing::_))
        .Times(0);

    std::move_only_function<void(bool) const> deassert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootDeassert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&, std::chrono::seconds) {
            deassert_done = std::move(done);
        });

    ParamMap params;
    start_ist(params);
    assert_done(true);
    power_done(true);
    ASSERT_TRUE(cak_done);

    cak_done(false);
    io_.run();
    io_.restart();

    EXPECT_EQ(service_->state().stage, IstStage::cleanup);
    ASSERT_TRUE(deassert_done);

    deassert_done(true);

    EXPECT_EQ(service_->state().status, IstStatus::failed);
    EXPECT_EQ(service_->state().stage, IstStage::idle);
}

TEST_F(IstServiceTest, CakBypassSkippedWhenScriptMissing)
{
    init_from_file(configPath_);

    std::move_only_function<void(bool) const> assert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&,
                      std::chrono::seconds) { assert_done = std::move(done); });

    std::move_only_function<void(bool) const> power_done;
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([&](DoneCb done) { power_done = std::move(done); });

    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("CAK bypass"), ::testing::_,
                         ::testing::_, ::testing::_))
        .Times(0);

    ItmDoneCb itm_done;
    EXPECT_CALL(*itmRunner_, asyncRun(::testing::_, ::testing::_, ::testing::_,
                                      ::testing::_))
        .WillOnce([&](const IstTestConfig&, const IstPlatformConfig&,
                      ItmDoneCb done,
                      ProgressCb) { itm_done = std::move(done); });

    ParamMap params;
    start_ist(params);
    assert_done(true);
    power_done(true);

    EXPECT_EQ(service_->state().stage, IstStage::runningIst);
    ASSERT_TRUE(itm_done);
}

TEST_F(IstServiceTest, AutoRebootPassesSkipCakToResetHook)
{
    init_from_file(configPath_);

    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([](const std::string&, const std::string&, DoneCb done,
                     const std::vector<std::string>&,
                     std::chrono::seconds) { done(true); });
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([](DoneCb done) { done(true); });
    EXPECT_CALL(*itmRunner_, asyncRun(::testing::_, ::testing::_, ::testing::_,
                                      ::testing::_))
        .WillOnce([](const IstTestConfig&, const IstPlatformConfig&,
                     ItmDoneCb done, ProgressCb) { done(0); });
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootDeassert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([](const std::string&, const std::string&, DoneCb done,
                     const std::vector<std::string>&,
                     std::chrono::seconds) { done(true); });

    std::vector<std::string> captured_args;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("resetSystem hook"), ::testing::_,
                         ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      std::vector<std::string> args, std::chrono::seconds) {
            captured_args = std::move(args);
            done(true);
        });

    ParamMap params;
    params["autoRebootOnComplete"] = true;
    start_ist(params);
    io_.run();
    io_.restart();

    ASSERT_THAT(captured_args,
                ::testing::ElementsAre(std::string("--skip-cak")));
    EXPECT_EQ(service_->state().status, IstStatus::completed);
}

TEST_F(IstServiceTest, ItmSuccessCompletesWithCleanup)
{
    init_from_file(configPath_);

    // Chain: assert hook → power cycle → ITM → deassert hook
    std::move_only_function<void(bool) const> assert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&,
                      std::chrono::seconds) { assert_done = std::move(done); });

    std::move_only_function<void(bool) const> power_done;
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([&](DoneCb done) { power_done = std::move(done); });

    ItmDoneCb itm_done;
    EXPECT_CALL(*itmRunner_, asyncRun(::testing::_, ::testing::_, ::testing::_,
                                      ::testing::_))
        .WillOnce([&](const IstTestConfig&, const IstPlatformConfig&,
                      ItmDoneCb done,
                      ProgressCb) { itm_done = std::move(done); });

    std::move_only_function<void(bool) const> deassert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootDeassert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&, std::chrono::seconds) {
            deassert_done = std::move(done);
        });

    ParamMap params;
    start_ist(params);
    assert_done(true);
    power_done(true);
    itm_done(0); // ITM succeeded
    io_.run();
    io_.restart();

    EXPECT_EQ(service_->state().stage, IstStage::cleanup);

    deassert_done(true); // Deassert succeeded

    EXPECT_EQ(service_->state().status, IstStatus::completed);
    EXPECT_EQ(service_->state().stage, IstStage::idle);
}

TEST_F(IstServiceTest, ItmFailureResultsInFailedStatus)
{
    init_from_file(configPath_);

    std::move_only_function<void(bool) const> assert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&,
                      std::chrono::seconds) { assert_done = std::move(done); });

    std::move_only_function<void(bool) const> power_done;
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([&](DoneCb done) { power_done = std::move(done); });

    ItmDoneCb itm_done;
    EXPECT_CALL(*itmRunner_, asyncRun(::testing::_, ::testing::_, ::testing::_,
                                      ::testing::_))
        .WillOnce([&](const IstTestConfig&, const IstPlatformConfig&,
                      ItmDoneCb done,
                      ProgressCb) { itm_done = std::move(done); });

    std::move_only_function<void(bool) const> deassert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootDeassert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&, std::chrono::seconds) {
            deassert_done = std::move(done);
        });

    ParamMap params;
    start_ist(params);
    assert_done(true);
    power_done(true);
    itm_done(1); // ITM failed (infra error)
    io_.run();
    io_.restart();

    EXPECT_EQ(service_->state().stage, IstStage::cleanup);

    deassert_done(true);

    EXPECT_EQ(service_->state().status, IstStatus::failed);
    EXPECT_EQ(service_->state().stage, IstStage::idle);
}

TEST_F(IstServiceTest, CleanupDeassertFailureStaysInFailed)
{
    init_from_file(configPath_);

    std::move_only_function<void(bool) const> assert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&,
                      std::chrono::seconds) { assert_done = std::move(done); });

    std::move_only_function<void(bool) const> power_done;
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([&](DoneCb done) { power_done = std::move(done); });

    ItmDoneCb itm_done;
    EXPECT_CALL(*itmRunner_, asyncRun(::testing::_, ::testing::_, ::testing::_,
                                      ::testing::_))
        .WillOnce([&](const IstTestConfig&, const IstPlatformConfig&,
                      ItmDoneCb done,
                      ProgressCb) { itm_done = std::move(done); });

    std::move_only_function<void(bool) const> deassert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootDeassert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&, std::chrono::seconds) {
            deassert_done = std::move(done);
        });

    ParamMap params;
    start_ist(params);
    assert_done(true);
    power_done(true);
    itm_done(0);
    io_.run();
    io_.restart();

    // Deassert hook fails
    deassert_done(false);

    EXPECT_EQ(service_->state().status, IstStatus::failed);
    EXPECT_EQ(service_->state().stage, IstStage::idle);
}

TEST_F(IstServiceTest, AutoRebootCallsResetHook)
{
    init_from_file(configPath_);

    std::move_only_function<void(bool) const> assert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&,
                      std::chrono::seconds) { assert_done = std::move(done); });

    std::move_only_function<void(bool) const> power_done;
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([&](DoneCb done) { power_done = std::move(done); });

    ItmDoneCb itm_done;
    EXPECT_CALL(*itmRunner_, asyncRun(::testing::_, ::testing::_, ::testing::_,
                                      ::testing::_))
        .WillOnce([&](const IstTestConfig&, const IstPlatformConfig&,
                      ItmDoneCb done,
                      ProgressCb) { itm_done = std::move(done); });

    std::move_only_function<void(bool) const> deassert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootDeassert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&, std::chrono::seconds) {
            deassert_done = std::move(done);
        });

    std::move_only_function<void(bool) const> reset_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("resetSystem hook"), ::testing::_,
                         ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&,
                      std::chrono::seconds) { reset_done = std::move(done); });

    // Enable auto-reboot
    ParamMap params;
    params["autoRebootOnComplete"] = true;
    start_ist(params);
    assert_done(true);
    power_done(true);
    itm_done(0);
    io_.run();
    io_.restart();
    deassert_done(true);

    // Reset hook should have been called
    ASSERT_TRUE(reset_done);

    reset_done(true);

    EXPECT_EQ(service_->state().status, IstStatus::completed);
    EXPECT_EQ(service_->state().stage, IstStage::idle);
}

TEST_F(IstServiceTest, StartIstPassesTestParams)
{
    init_from_file(configPath_);

    IstTestConfig captured_cfg;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([](const std::string&, const std::string&, DoneCb done,
                     const std::vector<std::string>&,
                     std::chrono::seconds) { done(true); });

    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([](DoneCb done) { done(true); });

    EXPECT_CALL(*itmRunner_, asyncRun(::testing::_, ::testing::_, ::testing::_,
                                      ::testing::_))
        .WillOnce([&](const IstTestConfig& cfg, const IstPlatformConfig&,
                      ItmDoneCb done, ProgressCb) {
            captured_cfg = cfg;
            done(0);
        });

    // Deassert hook in cleanup
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootDeassert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([](const std::string&, const std::string&, DoneCb done,
                     const std::vector<std::string>&,
                     std::chrono::seconds) { done(true); });

    ParamMap params;
    params["customTestList"] = std::string("test1,test2");
    params["customSocketList"] = std::string("0,1");
    params["istContinueOnFail"] = true;
    params["istSaveResOnPass"] = true;
    params["istSaveResOnFail"] = false;
    params["istSwTimeoutSec"] = 600;
    params["autoRebootOnComplete"] = false;
    start_ist(params);
    io_.run();
    io_.restart();

    ASSERT_TRUE(captured_cfg.customTestList.has_value());
    EXPECT_EQ(*captured_cfg.customTestList, "test1,test2");
    ASSERT_TRUE(captured_cfg.customSocketList.has_value());
    EXPECT_EQ(*captured_cfg.customSocketList, "0,1");
    ASSERT_TRUE(captured_cfg.continueOnFail.has_value());
    EXPECT_TRUE(*captured_cfg.continueOnFail);
    ASSERT_TRUE(captured_cfg.saveResOnPass.has_value());
    EXPECT_TRUE(*captured_cfg.saveResOnPass);
    ASSERT_TRUE(captured_cfg.saveResOnFail.has_value());
    EXPECT_FALSE(*captured_cfg.saveResOnFail);
    ASSERT_TRUE(captured_cfg.swTimeoutSec.has_value());
    EXPECT_EQ(*captured_cfg.swTimeoutSec, 600);
    EXPECT_FALSE(captured_cfg.autoRebootOnComplete);
}

TEST_F(IstServiceTest, SwTimeoutClampedToRange)
{
    init_from_file(configPath_);

    IstTestConfig captured_cfg;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([](const std::string&, const std::string&, DoneCb done,
                     const std::vector<std::string>&,
                     std::chrono::seconds) { done(true); });
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([](DoneCb done) { done(true); });
    EXPECT_CALL(*itmRunner_, asyncRun(::testing::_, ::testing::_, ::testing::_,
                                      ::testing::_))
        .WillOnce([&](const IstTestConfig& cfg, const IstPlatformConfig&,
                      ItmDoneCb done, ProgressCb) {
            captured_cfg = cfg;
            done(0);
        });
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootDeassert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([](const std::string&, const std::string&, DoneCb done,
                     const std::vector<std::string>&,
                     std::chrono::seconds) { done(true); });

    // Pass a timeout below minimum (60s)
    ParamMap params;
    params["istSwTimeoutSec"] = 5;
    start_ist(params);
    io_.run();
    io_.restart();

    ASSERT_TRUE(captured_cfg.swTimeoutSec.has_value());
    EXPECT_EQ(*captured_cfg.swTimeoutSec, 60); // clamped to min
}

TEST_F(IstServiceTest, InitializeRejectsNonexistentHookDir)
{
    write_config(R"({
        "hookDirectory": "/nonexistent/path/hooks",
        "hookPaths": {},
        "storageConfig": {}
    })");
    EXPECT_FALSE(init_from_file(configPath_));
}

TEST_F(IstServiceTest, InitializeRejectsEmptyHookPath)
{
    write_config(R"({
        "hookDirectory": ")" +
                 (tmpDir_ / "hooks").string() + R"(",
        "hookPaths": {
            "istBootAssert": ""
        },
        "storageConfig": {},
        "softwareInventoryId": "IST_Vectors"
    })");
    EXPECT_FALSE(init_from_file(configPath_));
}

TEST_F(IstServiceTest, StartIstThrowsWhenAssertHookMissing)
{
    // Config without istBootAssert hook
    write_config(R"({
        "hookDirectory": ")" +
                 (tmpDir_ / "hooks").string() + R"(",
        "hookPaths": {
            "istBootDeassert": ")" +
                 (tmpDir_ / "hooks/deassert.sh").string() + R"("
        },
        "storageConfig": {
            "vectorMountPath": ")" +
                 (tmpDir_ / "vectors").string() + R"(",
            "vectorStoragePath": ")" +
                 (tmpDir_ / "storage").string() + R"(",
            "resultStoragePath": ")" +
                 (tmpDir_ / "results").string() + R"("
        },
        "softwareInventoryId": "IST_Vectors"
    })");
    init_from_file(configPath_);

    ParamMap params;
    EXPECT_THROW(service_->startIST(params), ist_err::HookNotFound);
    EXPECT_EQ(service_->state().status, IstStatus::failed);
    EXPECT_EQ(service_->state().stage, IstStage::idle);
}

TEST_F(IstServiceTest, SwTimeoutClampedToMax)
{
    init_from_file(configPath_);

    IstTestConfig captured_cfg;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([](const std::string&, const std::string&, DoneCb done,
                     const std::vector<std::string>&,
                     std::chrono::seconds) { done(true); });
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([](DoneCb done) { done(true); });
    EXPECT_CALL(*itmRunner_, asyncRun(::testing::_, ::testing::_, ::testing::_,
                                      ::testing::_))
        .WillOnce([&](const IstTestConfig& cfg, const IstPlatformConfig&,
                      ItmDoneCb done, ProgressCb) {
            captured_cfg = cfg;
            done(0);
        });
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootDeassert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([](const std::string&, const std::string&, DoneCb done,
                     const std::vector<std::string>&,
                     std::chrono::seconds) { done(true); });

    ParamMap params;
    params["istSwTimeoutSec"] = 99999;
    start_ist(params);
    io_.run();
    io_.restart();

    ASSERT_TRUE(captured_cfg.swTimeoutSec.has_value());
    EXPECT_EQ(*captured_cfg.swTimeoutSec, 7200); // clamped to max
}

TEST_F(IstServiceTest, AutoRebootResetFailureTransitionsToFailed)
{
    init_from_file(configPath_);

    std::move_only_function<void(bool) const> assert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&,
                      std::chrono::seconds) { assert_done = std::move(done); });

    std::move_only_function<void(bool) const> power_done;
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([&](DoneCb done) { power_done = std::move(done); });

    ItmDoneCb itm_done;
    EXPECT_CALL(*itmRunner_, asyncRun(::testing::_, ::testing::_, ::testing::_,
                                      ::testing::_))
        .WillOnce([&](const IstTestConfig&, const IstPlatformConfig&,
                      ItmDoneCb done,
                      ProgressCb) { itm_done = std::move(done); });

    std::move_only_function<void(bool) const> deassert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootDeassert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&, std::chrono::seconds) {
            deassert_done = std::move(done);
        });

    std::move_only_function<void(bool) const> reset_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("resetSystem hook"), ::testing::_,
                         ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&,
                      std::chrono::seconds) { reset_done = std::move(done); });

    ParamMap params;
    params["autoRebootOnComplete"] = true;
    start_ist(params);
    assert_done(true);
    power_done(true);
    itm_done(0);
    io_.run();
    io_.restart();
    deassert_done(true);

    // Reset hook fails
    reset_done(false);

    EXPECT_EQ(service_->state().status, IstStatus::failed);
    EXPECT_EQ(service_->state().stage, IstStage::idle);
}

TEST_F(IstServiceTest, AutoRebootWithItmFailureStillFailed)
{
    init_from_file(configPath_);

    std::move_only_function<void(bool) const> assert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&,
                      std::chrono::seconds) { assert_done = std::move(done); });

    std::move_only_function<void(bool) const> power_done;
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([&](DoneCb done) { power_done = std::move(done); });

    ItmDoneCb itm_done;
    EXPECT_CALL(*itmRunner_, asyncRun(::testing::_, ::testing::_, ::testing::_,
                                      ::testing::_))
        .WillOnce([&](const IstTestConfig&, const IstPlatformConfig&,
                      ItmDoneCb done,
                      ProgressCb) { itm_done = std::move(done); });

    std::move_only_function<void(bool) const> deassert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootDeassert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&, std::chrono::seconds) {
            deassert_done = std::move(done);
        });

    std::move_only_function<void(bool) const> reset_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("resetSystem hook"), ::testing::_,
                         ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&,
                      std::chrono::seconds) { reset_done = std::move(done); });

    ParamMap params;
    params["autoRebootOnComplete"] = true;
    start_ist(params);
    assert_done(true);
    power_done(true);
    itm_done(1); // ITM failed (infra error)
    io_.run();
    io_.restart();

    deassert_done(true);

    // Reset succeeds but ITM had failed
    reset_done(true);

    EXPECT_EQ(service_->state().status, IstStatus::failed);
    EXPECT_EQ(service_->state().stage, IstStage::idle);
}

TEST_F(IstServiceTest, AutoRebootFailsWhenResetHookMissing)
{
    // Config without resetSystem hook
    write_config(R"({
        "hookDirectory": ")" +
                 (tmpDir_ / "hooks").string() + R"(",
        "hookPaths": {
            "istBootAssert": ")" +
                 (tmpDir_ / "hooks/assert.sh").string() + R"(",
            "istBootDeassert": ")" +
                 (tmpDir_ / "hooks/deassert.sh").string() + R"("
        },
        "storageConfig": {
            "vectorMountPath": ")" +
                 (tmpDir_ / "vectors").string() + R"(",
            "vectorStoragePath": ")" +
                 (tmpDir_ / "storage").string() + R"(",
            "resultStoragePath": ")" +
                 (tmpDir_ / "results").string() + R"("
        },
        "softwareInventoryId": "IST_Vectors"
    })");
    init_from_file(configPath_);

    std::move_only_function<void(bool) const> assert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&,
                      std::chrono::seconds) { assert_done = std::move(done); });

    std::move_only_function<void(bool) const> power_done;
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([&](DoneCb done) { power_done = std::move(done); });

    ItmDoneCb itm_done;
    EXPECT_CALL(*itmRunner_, asyncRun(::testing::_, ::testing::_, ::testing::_,
                                      ::testing::_))
        .WillOnce([&](const IstTestConfig&, const IstPlatformConfig&,
                      ItmDoneCb done,
                      ProgressCb) { itm_done = std::move(done); });

    std::move_only_function<void(bool) const> deassert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootDeassert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&, std::chrono::seconds) {
            deassert_done = std::move(done);
        });

    ParamMap params;
    params["autoRebootOnComplete"] = true;
    start_ist(params);
    assert_done(true);
    power_done(true);
    itm_done(0);
    io_.run();
    io_.restart();
    deassert_done(true);

    // resetSystem hook is missing -> should fail in cleanup
    EXPECT_EQ(service_->state().status, IstStatus::failed);
    EXPECT_EQ(service_->state().stage, IstStage::idle);
}

TEST_F(IstServiceTest, StartIstAbortsOnMissingResultStorageConfig)
{
    // Config without resultStoragePath
    write_config(R"({
        "hookDirectory": ")" +
                 (tmpDir_ / "hooks").string() + R"(",
        "hookPaths": {
            "istBootAssert": ")" +
                 (tmpDir_ / "hooks/assert.sh").string() + R"(",
            "istBootDeassert": ")" +
                 (tmpDir_ / "hooks/deassert.sh").string() + R"("
        },
        "storageConfig": {
            "vectorMountPath": ")" +
                 (tmpDir_ / "vectors").string() + R"("
        },
        "softwareInventoryId": "IST_Vectors"
    })");
    init_from_file(configPath_);

    ParamMap params;
    EXPECT_THROW(service_->startIST(params), ist_err::ResultStorageError);
    EXPECT_EQ(service_->state().status, IstStatus::aborted);
}

TEST_F(IstServiceTest, EachRunGetsUniqueObjectPath)
{
    init_from_file(configPath_);

    // --- First run ---
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([](const std::string&, const std::string&, DoneCb done,
                     const std::vector<std::string>&,
                     std::chrono::seconds) { done(true); });
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([](DoneCb done) { done(true); });
    EXPECT_CALL(*itmRunner_, asyncRun(::testing::_, ::testing::_, ::testing::_,
                                      ::testing::_))
        .WillOnce([](const IstTestConfig&, const IstPlatformConfig&,
                     ItmDoneCb done, ProgressCb) { done(0); });
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootDeassert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([](const std::string&, const std::string&, DoneCb done,
                     const std::vector<std::string>&,
                     std::chrono::seconds) { done(true); });

    ParamMap params1;
    std::string path1 = start_ist(params1);
    io_.run();
    io_.restart();

    // --- Second run ---
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([](const std::string&, const std::string&, DoneCb done,
                     const std::vector<std::string>&,
                     std::chrono::seconds) { done(true); });
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([](DoneCb done) { done(true); });
    EXPECT_CALL(*itmRunner_, asyncRun(::testing::_, ::testing::_, ::testing::_,
                                      ::testing::_))
        .WillOnce([](const IstTestConfig&, const IstPlatformConfig&,
                     ItmDoneCb done, ProgressCb) { done(0); });
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootDeassert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([](const std::string&, const std::string&, DoneCb done,
                     const std::vector<std::string>&,
                     std::chrono::seconds) { done(true); });

    ParamMap params2;
    std::string path2 = start_ist(params2);
    io_.run();
    io_.restart();

    EXPECT_NE(path1, path2);
    EXPECT_EQ(path1, "/com/nvidia/vera/ist/runs/0");
    EXPECT_EQ(path2, "/com/nvidia/vera/ist/runs/1");
}

TEST_F(IstServiceTest, SecondRunAfterCompletionWorks)
{
    init_from_file(configPath_);

    // --- First run: full success ---
    std::move_only_function<void(bool) const> assert_done1;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&, std::chrono::seconds) {
            assert_done1 = std::move(done);
        });

    std::move_only_function<void(bool) const> power_done1;
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([&](DoneCb done) { power_done1 = std::move(done); });

    ItmDoneCb itm_done1;
    EXPECT_CALL(*itmRunner_, asyncRun(::testing::_, ::testing::_, ::testing::_,
                                      ::testing::_))
        .WillOnce([&](const IstTestConfig&, const IstPlatformConfig&,
                      ItmDoneCb done,
                      ProgressCb) { itm_done1 = std::move(done); });

    std::move_only_function<void(bool) const> deassert_done1;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootDeassert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&, std::chrono::seconds) {
            deassert_done1 = std::move(done);
        });

    ParamMap params1;
    params1["customTestList"] = std::string("testA");
    start_ist(params1);
    assert_done1(true);
    power_done1(true);
    itm_done1(0);
    io_.run();
    io_.restart();
    deassert_done1(true);

    EXPECT_EQ(service_->state().status, IstStatus::completed);
    EXPECT_EQ(service_->state().stage, IstStage::idle);

    // --- Second run: should start cleanly with fresh state ---
    std::move_only_function<void(bool) const> assert_done2;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&, std::chrono::seconds) {
            assert_done2 = std::move(done);
        });

    std::move_only_function<void(bool) const> power_done2;
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([&](DoneCb done) { power_done2 = std::move(done); });

    ItmDoneCb itm_done2;
    EXPECT_CALL(*itmRunner_, asyncRun(::testing::_, ::testing::_, ::testing::_,
                                      ::testing::_))
        .WillOnce([&](const IstTestConfig& cfg, const IstPlatformConfig&,
                      ItmDoneCb done, ProgressCb) {
            // Verify old params are NOT carried over
            EXPECT_FALSE(cfg.customTestList.has_value() &&
                         *cfg.customTestList == "testA");
            itm_done2 = std::move(done);
        });

    std::move_only_function<void(bool) const> deassert_done2;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootDeassert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&, std::chrono::seconds) {
            deassert_done2 = std::move(done);
        });

    ParamMap params2; // no customTestList this time
    EXPECT_NO_THROW(start_ist(params2));
    EXPECT_EQ(service_->state().progress, 0);
    EXPECT_EQ(service_->state().status, IstStatus::inProgress);

    assert_done2(true);
    power_done2(true);
    itm_done2(0);
    io_.run();
    io_.restart();
    deassert_done2(true);

    EXPECT_EQ(service_->state().status, IstStatus::completed);
    EXPECT_EQ(service_->state().stage, IstStage::idle);
}

TEST_F(IstServiceTest, RunObjectCreatedWithCorrectPath)
{
    init_from_file(configPath_);

    EXPECT_CALL(
        *publisher_,
        createRunObject(StrEq("/com/nvidia/vera/ist/runs/0"), ::testing::_))
        .Times(1);

    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([](const std::string&, const std::string&, DoneCb done,
                     const std::vector<std::string>&,
                     std::chrono::seconds) { done(true); });
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([](DoneCb done) { done(true); });
    EXPECT_CALL(*itmRunner_, asyncRun(::testing::_, ::testing::_, ::testing::_,
                                      ::testing::_))
        .WillOnce([](const IstTestConfig&, const IstPlatformConfig&,
                     ItmDoneCb done, ProgressCb) { done(0); });
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootDeassert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([](const std::string&, const std::string&, DoneCb done,
                     const std::vector<std::string>&,
                     std::chrono::seconds) { done(true); });

    ParamMap params;
    std::string path = start_ist(params);
    io_.run();
    io_.restart();

    EXPECT_EQ(path, "/com/nvidia/vera/ist/runs/0");
    EXPECT_EQ(service_->currentRunPath(), path);
    EXPECT_EQ(service_->state().status, IstStatus::completed);
}

TEST_F(IstServiceTest, RunObjectNotCreatedOnCollateralAbort)
{
    fs::remove_all(tmpDir_ / "vectors");
    init_from_file(configPath_);

    EXPECT_CALL(*publisher_, createRunObject(::testing::_, ::testing::_))
        .Times(0);

    ParamMap params;
    EXPECT_THROW(service_->startIST(params), ist_err::CollateralNotFound);
    EXPECT_EQ(service_->state().status, IstStatus::aborted);
}

TEST_F(IstServiceTest, RunObjectCreatedOnFailure)
{
    init_from_file(configPath_);

    EXPECT_CALL(
        *publisher_,
        createRunObject(StrEq("/com/nvidia/vera/ist/runs/0"), ::testing::_))
        .Times(1);

    DoneCb assert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&,
                      std::chrono::seconds) { assert_done = std::move(done); });

    ParamMap params;
    start_ist(params);
    assert_done(false);

    EXPECT_EQ(service_->state().status, IstStatus::failed);
}

TEST_F(IstServiceTest, RunCounterIncrementsAfterFailure)
{
    init_from_file(configPath_);

    // First run: fails at assert hook
    DoneCb assert_done1;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&, std::chrono::seconds) {
            assert_done1 = std::move(done);
        });

    ParamMap params1;
    std::string path1 = start_ist(params1);
    assert_done1(false);
    EXPECT_EQ(path1, "/com/nvidia/vera/ist/runs/0");

    // Second run: counter should still be 1, not reset
    DoneCb assert_done2;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&, std::chrono::seconds) {
            assert_done2 = std::move(done);
        });

    ParamMap params2;
    std::string path2 = start_ist(params2);
    EXPECT_EQ(path2, "/com/nvidia/vera/ist/runs/1");
}

TEST_F(IstServiceTest, ProgressCallbackUpdatesStateAndPublishes)
{
    init_from_file(configPath_);

    DoneCb assert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&,
                      std::chrono::seconds) { assert_done = std::move(done); });

    DoneCb power_done;
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([&](DoneCb done) { power_done = std::move(done); });

    ItmDoneCb itm_done;
    ProgressCb progress_cb;
    EXPECT_CALL(*itmRunner_, asyncRun(::testing::_, ::testing::_, ::testing::_,
                                      ::testing::_))
        .WillOnce([&](const IstTestConfig&, const IstPlatformConfig&,
                      ItmDoneCb done, ProgressCb on_progress) {
            itm_done = std::move(done);
            progress_cb = std::move(on_progress);
        });

    ParamMap params;
    start_ist(params);
    assert_done(true);
    power_done(true);

    ASSERT_TRUE(progress_cb);

    EXPECT_CALL(*publisher_, publishProgress(42)).Times(1);
    progress_cb(42);
    EXPECT_EQ(service_->state().progress, 42);

    EXPECT_CALL(*publisher_, publishProgress(99)).Times(1);
    progress_cb(99);
    EXPECT_EQ(service_->state().progress, 99);
}

TEST_F(IstServiceTest, ParamTypeMismatchRejected)
{
    init_from_file(configPath_);

    ParamMap params;
    params["istSwTimeoutSec"] = std::string("not_a_number");
    EXPECT_THROW(service_->startIST(params), ist_err::InvalidParameter);
    EXPECT_EQ(service_->state().status, IstStatus::aborted);
}

TEST_F(IstServiceTest, SecondRunAfterFailureWorks)
{
    init_from_file(configPath_);

    // --- First run: fails at assert hook ---
    DoneCb assert_done1;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&, std::chrono::seconds) {
            assert_done1 = std::move(done);
        });

    ParamMap params1;
    start_ist(params1);
    assert_done1(false);

    EXPECT_EQ(service_->state().status, IstStatus::failed);
    EXPECT_EQ(service_->state().stage, IstStage::idle);

    // --- Second run: should be accepted and start cleanly ---
    DoneCb assert_done2;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&, std::chrono::seconds) {
            assert_done2 = std::move(done);
        });

    DoneCb power_done2;
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([&](DoneCb done) { power_done2 = std::move(done); });

    ItmDoneCb itm_done2;
    EXPECT_CALL(*itmRunner_, asyncRun(::testing::_, ::testing::_, ::testing::_,
                                      ::testing::_))
        .WillOnce([&](const IstTestConfig&, const IstPlatformConfig&,
                      ItmDoneCb done,
                      ProgressCb) { itm_done2 = std::move(done); });

    DoneCb deassert_done2;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootDeassert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&, std::chrono::seconds) {
            deassert_done2 = std::move(done);
        });

    ParamMap params2;
    EXPECT_NO_THROW(start_ist(params2));
    EXPECT_EQ(service_->state().status, IstStatus::inProgress);
    EXPECT_EQ(service_->state().progress, 0);

    assert_done2(true);
    power_done2(true);
    itm_done2(0);
    io_.run();
    io_.restart();
    deassert_done2(true);

    EXPECT_EQ(service_->state().status, IstStatus::completed);
    EXPECT_EQ(service_->state().stage, IstStage::idle);
}

TEST_F(IstServiceTest, StartIstBeforeInitializeThrows)
{
    ParamMap params;
    EXPECT_THROW(service_->startIST(params), ist_err::CollateralNotFound);
    EXPECT_EQ(service_->state().status, IstStatus::aborted);
    EXPECT_EQ(service_->state().stage, IstStage::idle);
}

// ----------------
// PLDM test helpers
// ----------------

namespace
{

uint32_t test_crc32(const uint8_t* data, size_t len)
{
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
        {
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
        }
    }
    return ~crc;
}

void write_u16_le(uint8_t* p, uint16_t v)
{
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

void write_u32_le(uint8_t* p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

static constexpr std::array<uint8_t, 16> k_ist_vectors_component_uuid = {
    0x8f, 0x1b, 0x9b, 0x0e, 0x3c, 0x6d, 0x4a, 0x8c,
    0x9f, 0x32, 0x12, 0xe7, 0xb4, 0xd9, 0xc5, 0xa1,
};

static constexpr std::array<uint8_t, 16> k_test_pldm_package_id = {
    0x7b, 0x29, 0x1c, 0x99, 0x6d, 0xb6, 0x42, 0x08,
    0x80, 0x1b, 0x02, 0x02, 0x6e, 0x46, 0x3c, 0x78,
};

void write_ist_vectors_component_descriptor(std::vector<uint8_t>& hdr,
                                            size_t offset)
{
    hdr[offset + 0] = 0x02;
    hdr[offset + 1] = 0x00;
    hdr[offset + 2] = 0x10;
    hdr[offset + 3] = 0x00;
    std::memcpy(hdr.data() + offset + 4, k_ist_vectors_component_uuid.data(),
                k_ist_vectors_component_uuid.size());
}

// Build a minimal PLDM firmware package (revision 4) wrapping `payload`.
// PackageHeaderIdentifier at offset 0, component UUID descriptor embedded
// before the header CRC trailer.
std::vector<uint8_t> build_pldm_package(const std::vector<uint8_t>& payload)
{
    constexpr uint16_t header_size = 64;
    constexpr uint8_t revision = 4;

    std::vector<uint8_t> hdr(header_size, 0);
    std::memcpy(hdr.data(), k_test_pldm_package_id.data(),
                k_test_pldm_package_id.size());
    hdr[16] = revision;
    write_u16_le(&hdr[17], header_size);
    write_ist_vectors_component_descriptor(hdr, 36);

    uint32_t payload_crc = test_crc32(payload.data(), payload.size());
    write_u32_le(&hdr[header_size - 4], payload_crc);

    uint32_t header_crc = test_crc32(hdr.data(), header_size - 8);
    write_u32_le(&hdr[header_size - 8], header_crc);

    std::vector<uint8_t> package;
    package.reserve(hdr.size() + payload.size());
    package.insert(package.end(), hdr.begin(), hdr.end());
    package.insert(package.end(), payload.begin(), payload.end());
    return package;
}

} // namespace

// ----------------
// StartUpdate / image transfer tests
// ----------------

TEST_F(IstServiceTest, StartUpdateReturnsSoftwareObjectPath)
{
    init_from_file(configPath_);
    std::string object_path;
    int fd = begin_start_update(&object_path);
    EXPECT_EQ(object_path,
              "/xyz/openbmc_project/inventory_software/IST_Vectors");
    ::close(fd);
    io_.run();
}

TEST_F(IstServiceTest, StartUpdateTransfersData)
{
    init_from_file(configPath_);
    int fd = begin_start_update();

    const std::string raw_payload = "hello image data";
    std::vector<uint8_t> payload(raw_payload.begin(), raw_payload.end());
    auto pldm_pkg = build_pldm_package(payload);
    ASSERT_EQ(::write(fd, pldm_pkg.data(), pldm_pkg.size()),
              static_cast<ssize_t>(pldm_pkg.size()));
    ::close(fd);

    io_.run();

    fs::path image_path = tmpDir_ / "storage" / "CPU-IST.img";
    ASSERT_TRUE(fs::exists(image_path));

    std::ifstream img(image_path, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(img)),
                        std::istreambuf_iterator<char>());
    EXPECT_EQ(content, raw_payload);
}

TEST_F(IstServiceTest, StartUpdateRejectsConcurrentTransfer)
{
    init_from_file(configPath_);
    int fd = begin_start_update();

    auto [readFd2, writeFd2] = make_upload_socket_pair();
    EXPECT_THROW(service_->startUpdate(readFd2, k_apply_time_immediate),
                 ist_err::Unavailable);
    ::close(writeFd2);

    ::close(fd);
    io_.run();
}

TEST_F(IstServiceTest, StartUpdateRejectsWhileIstRunning)
{
    init_from_file(configPath_);

    DoneCb assert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&,
                      std::chrono::seconds) { assert_done = std::move(done); });

    ParamMap params;
    start_ist(params);

    auto [readFd, writeFd] = make_upload_socket_pair();
    EXPECT_THROW(service_->startUpdate(readFd, k_apply_time_immediate),
                 ist_err::Unavailable);
    ::close(writeFd);
}

TEST_F(IstServiceTest, StartUpdateEmptyTransferCleansUp)
{
    init_from_file(configPath_);
    int fd = begin_start_update();
    ::close(fd);

    io_.run();

    fs::path image_path = tmpDir_ / "storage" / "CPU-IST.img";
    EXPECT_FALSE(fs::exists(image_path));
}

TEST_F(IstServiceTest, StartUpdateTimesOut)
{
    IstPlatformConfig cfg;
    ASSERT_TRUE(parsePlatformConfig(cfg, configPath_));
    cfg.transferInactivityTimeout = std::chrono::seconds(1);
    ASSERT_TRUE(service_->initialize(std::move(cfg)));

    int fd = begin_start_update();

    io_.run();

    fs::path image_path = tmpDir_ / "storage" / "CPU-IST.img";
    EXPECT_FALSE(fs::exists(image_path));

    ::close(fd);
}

TEST_F(IstServiceTest, StartUpdateAllowsNewTransferAfterCompletion)
{
    init_from_file(configPath_);

    const std::string raw1 = "first transfer";
    std::vector<uint8_t> payload1(raw1.begin(), raw1.end());
    auto pkg1 = build_pldm_package(payload1);

    int fd1 = begin_start_update();
    ASSERT_EQ(::write(fd1, pkg1.data(), pkg1.size()),
              static_cast<ssize_t>(pkg1.size()));
    ::close(fd1);
    io_.run();

    fs::path image_path = tmpDir_ / "storage" / "CPU-IST.img";
    ASSERT_TRUE(fs::exists(image_path));

    io_.restart();

    const std::string raw2 = "second transfer";
    std::vector<uint8_t> payload2(raw2.begin(), raw2.end());
    auto pkg2 = build_pldm_package(payload2);

    int fd2 = begin_start_update();
    ASSERT_EQ(::write(fd2, pkg2.data(), pkg2.size()),
              static_cast<ssize_t>(pkg2.size()));
    ::close(fd2);
    io_.run();

    std::ifstream img(image_path, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(img)),
                        std::istreambuf_iterator<char>());
    EXPECT_EQ(content, raw2);
}

TEST_F(IstServiceTest, StartUpdateTruncatedTransferRemovesImageAndReleasesLock)
{
    init_from_file(configPath_);

    fs::path image_path = tmpDir_ / "storage" / "CPU-IST.img";

    const std::string raw1 = "payload that gets cut off partway through";
    std::vector<uint8_t> payload1(raw1.begin(), raw1.end());
    auto pkg1 = build_pldm_package(payload1);

    // Send the complete header but only a few payload bytes, then drop the
    // connection mid-transfer.
    size_t partial = 64 + 5;
    ASSERT_LT(partial, pkg1.size());

    int fd1 = begin_start_update();
    ASSERT_EQ(::write(fd1, pkg1.data(), partial),
              static_cast<ssize_t>(partial));
    ::close(fd1);
    io_.run();

    // The payload CRC cannot match a truncated stream, so nothing is left
    // behind on disk.
    EXPECT_FALSE(fs::exists(image_path));

    io_.restart();

    // The update lock must have been released: a second, complete upload
    // succeeds (begin_start_update would throw if the service were still busy).
    const std::string raw2 = "second complete payload";
    std::vector<uint8_t> payload2(raw2.begin(), raw2.end());
    auto pkg2 = build_pldm_package(payload2);

    int fd2 = begin_start_update();
    ASSERT_EQ(::write(fd2, pkg2.data(), pkg2.size()),
              static_cast<ssize_t>(pkg2.size()));
    ::close(fd2);
    io_.run();

    ASSERT_TRUE(fs::exists(image_path));
    std::ifstream img2(image_path, std::ios::binary);
    std::string content2((std::istreambuf_iterator<char>(img2)),
                         std::istreambuf_iterator<char>());
    EXPECT_EQ(content2, raw2);
}

TEST_F(IstServiceTest, StartUpdateAcceptsOnResetApplyTime)
{
    init_from_file(configPath_);

    auto [readFd, writeFd] = make_upload_socket_pair();
    std::string object_path =
        service_->startUpdate(readFd, k_apply_time_on_reset);
    EXPECT_FALSE(object_path.empty());
    ::close(writeFd);
}

TEST_F(IstServiceTest, StartUpdateRejectsUnsupportedApplyTime)
{
    init_from_file(configPath_);

    auto [readFd, writeFd] = make_upload_socket_pair();
    EXPECT_THROW(service_->startUpdate(readFd, "not-a-valid-apply-time"),
                 ist_err::InvalidArgument);
    ::close(writeFd);
}

TEST_F(IstServiceTest, StartUpdateRejectsEmptyStoragePath)
{
    write_config(R"({
        "hookDirectory": ")" +
                 (tmpDir_ / "hooks").string() + R"(",
        "hookPaths": {
            "istBootAssert": ")" +
                 (tmpDir_ / "hooks/assert.sh").string() + R"(",
            "istBootDeassert": ")" +
                 (tmpDir_ / "hooks/deassert.sh").string() + R"(",
            "resetSystem": ")" +
                 (tmpDir_ / "hooks/reset.sh").string() + R"("
        },
        "storageConfig": {
            "vectorMountPath": ")" +
                 (tmpDir_ / "vectors").string() + R"(",
            "resultStoragePath": ")" +
                 (tmpDir_ / "results").string() + R"("
        },
        "softwareInventoryId": "IST_Vectors"
    })");
    init_from_file(configPath_);

    auto [readFd, writeFd] = make_upload_socket_pair();
    EXPECT_THROW(service_->startUpdate(readFd, k_apply_time_immediate),
                 ist_err::InternalFailure);
    ::close(writeFd);
}

// ----------------
// PLDM header validation / strip tests
// ----------------

TEST_F(IstServiceTest, StartUpdatePldmStripSucceeds)
{
    init_from_file(configPath_);

    const std::string raw_payload = "This is the ext4 image payload content!";
    std::vector<uint8_t> payload(raw_payload.begin(), raw_payload.end());
    auto pldm_pkg = build_pldm_package(payload);

    int fd = begin_start_update();
    ASSERT_EQ(::write(fd, pldm_pkg.data(), pldm_pkg.size()),
              static_cast<ssize_t>(pldm_pkg.size()));
    ::close(fd);
    io_.run();

    fs::path image_path = tmpDir_ / "storage" / "CPU-IST.img";
    ASSERT_TRUE(fs::exists(image_path));
    EXPECT_EQ(fs::file_size(image_path), payload.size());

    std::ifstream img(image_path, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(img)),
                        std::istreambuf_iterator<char>());
    EXPECT_EQ(content, raw_payload);
}

TEST_F(IstServiceTest, StartUpdatePldmBadHeaderCrc)
{
    init_from_file(configPath_);

    const std::string raw_payload = "payload data";
    std::vector<uint8_t> payload(raw_payload.begin(), raw_payload.end());
    auto pldm_pkg = build_pldm_package(payload);

    // Corrupt a byte in the header (not the CRC field itself, but the data).
    pldm_pkg[10] ^= 0xFF;

    int fd = begin_start_update();
    ASSERT_EQ(::write(fd, pldm_pkg.data(), pldm_pkg.size()),
              static_cast<ssize_t>(pldm_pkg.size()));
    ::close(fd);
    io_.run();

    fs::path image_path = tmpDir_ / "storage" / "CPU-IST.img";
    EXPECT_FALSE(fs::exists(image_path));
}

TEST_F(IstServiceTest, StartUpdatePldmBadPayloadCrc)
{
    init_from_file(configPath_);

    const std::string raw_payload = "payload data for crc test";
    std::vector<uint8_t> payload(raw_payload.begin(), raw_payload.end());
    auto pldm_pkg = build_pldm_package(payload);

    // Corrupt a payload byte (after the header) to invalidate payload CRC.
    pldm_pkg[pldm_pkg.size() - 1] ^= 0xFF;

    int fd = begin_start_update();
    ASSERT_EQ(::write(fd, pldm_pkg.data(), pldm_pkg.size()),
              static_cast<ssize_t>(pldm_pkg.size()));
    ::close(fd);
    io_.run();

    fs::path image_path = tmpDir_ / "storage" / "CPU-IST.img";
    EXPECT_FALSE(fs::exists(image_path));
}

TEST_F(IstServiceTest, StartUpdatePldmFileTooSmall)
{
    init_from_file(configPath_);

    // Send fewer than 36 bytes — too small for a PLDM header.
    const std::string tiny = "too small";
    int fd = begin_start_update();
    ASSERT_EQ(::write(fd, tiny.data(), tiny.size()),
              static_cast<ssize_t>(tiny.size()));
    ::close(fd);
    io_.run();

    fs::path image_path = tmpDir_ / "storage" / "CPU-IST.img";
    EXPECT_FALSE(fs::exists(image_path));
}

TEST_F(IstServiceTest, StartUpdatePldmBadUuidLogsError)
{
    init_from_file(configPath_);

    const std::string raw_payload = "payload data";
    std::vector<uint8_t> payload(raw_payload.begin(), raw_payload.end());
    auto pldm_pkg = build_pldm_package(payload);
    pldm_pkg[40] ^= 0xFF;

    testing::internal::CaptureStderr();
    int fd = begin_start_update();
    ASSERT_EQ(::write(fd, pldm_pkg.data(), pldm_pkg.size()),
              static_cast<ssize_t>(pldm_pkg.size()));
    ::close(fd);
    io_.run();
    std::string err = testing::internal::GetCapturedStderr();

    EXPECT_THAT(err, ::testing::HasSubstr("PLDM component UUID mismatch"));
}

TEST_F(IstServiceTest, StartUpdateBadUuidDoesNotTruncateExistingImage)
{
    init_from_file(configPath_);

    fs::path image_path = tmpDir_ / "storage" / "CPU-IST.img";
    fs::create_directories(image_path.parent_path());
    static constexpr std::string_view k_existing = "existing-vector-image";
    {
        std::ofstream existing(image_path, std::ios::binary);
        existing << k_existing;
    }

    const std::string raw_payload = "payload data";
    std::vector<uint8_t> payload(raw_payload.begin(), raw_payload.end());
    auto pldm_pkg = build_pldm_package(payload);
    pldm_pkg[40] ^= 0xFF;

    int fd = begin_start_update();
    ASSERT_EQ(::write(fd, pldm_pkg.data(), pldm_pkg.size()),
              static_cast<ssize_t>(pldm_pkg.size()));
    ::close(fd);
    io_.run();

    ASSERT_TRUE(fs::exists(image_path));
    std::ifstream img(image_path, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(img)),
                        std::istreambuf_iterator<char>());
    EXPECT_EQ(content, k_existing);
}

TEST_F(IstServiceTest, StartUpdatePldmBadHeaderCrcLogsError)
{
    init_from_file(configPath_);

    const std::string raw_payload = "payload data";
    std::vector<uint8_t> payload(raw_payload.begin(), raw_payload.end());
    auto pldm_pkg = build_pldm_package(payload);
    pldm_pkg[20] ^= 0xFF;

    testing::internal::CaptureStderr();
    int fd = begin_start_update();
    ASSERT_EQ(::write(fd, pldm_pkg.data(), pldm_pkg.size()),
              static_cast<ssize_t>(pldm_pkg.size()));
    ::close(fd);
    io_.run();
    std::string err = testing::internal::GetCapturedStderr();

    EXPECT_THAT(err, ::testing::HasSubstr("PLDM header CRC mismatch"));
}

TEST_F(IstServiceTest, StartUpdatePldmBadPayloadCrcLogsError)
{
    init_from_file(configPath_);

    const std::string raw_payload = "payload data for crc test";
    std::vector<uint8_t> payload(raw_payload.begin(), raw_payload.end());
    auto pldm_pkg = build_pldm_package(payload);
    pldm_pkg[pldm_pkg.size() - 1] ^= 0xFF;

    testing::internal::CaptureStderr();
    int fd = begin_start_update();
    ASSERT_EQ(::write(fd, pldm_pkg.data(), pldm_pkg.size()),
              static_cast<ssize_t>(pldm_pkg.size()));
    ::close(fd);
    io_.run();
    std::string err = testing::internal::GetCapturedStderr();

    EXPECT_THAT(err, ::testing::HasSubstr("PLDM payload CRC mismatch"));
}

TEST_F(IstServiceTest, StartUpdatePldmRejectsOldRevision)
{
    init_from_file(configPath_);

    // Build a header with revision 3 (no payload checksum).
    constexpr uint16_t header_size = 64;
    std::vector<uint8_t> hdr(header_size, 0);
    std::memcpy(hdr.data(), k_test_pldm_package_id.data(),
                k_test_pldm_package_id.size());
    hdr[16] = 3;
    write_u16_le(&hdr[17], header_size);
    write_ist_vectors_component_descriptor(hdr, 36);
    uint32_t header_crc = test_crc32(hdr.data(), header_size - 4);
    write_u32_le(&hdr[header_size - 4], header_crc);

    const std::string raw = "payload";
    hdr.insert(hdr.end(), raw.begin(), raw.end());

    int fd = begin_start_update();
    ASSERT_EQ(::write(fd, hdr.data(), hdr.size()),
              static_cast<ssize_t>(hdr.size()));
    ::close(fd);
    io_.run();

    fs::path image_path = tmpDir_ / "storage" / "CPU-IST.img";
    EXPECT_FALSE(fs::exists(image_path));
}

TEST_F(IstServiceTest, StartUpdatePldmZeroLengthPayload)
{
    init_from_file(configPath_);

    std::vector<uint8_t> payload; // empty
    auto pldm_pkg = build_pldm_package(payload);

    int fd = begin_start_update();
    ASSERT_EQ(::write(fd, pldm_pkg.data(), pldm_pkg.size()),
              static_cast<ssize_t>(pldm_pkg.size()));
    ::close(fd);
    io_.run();

    fs::path image_path = tmpDir_ / "storage" / "CPU-IST.img";
    ASSERT_TRUE(fs::exists(image_path));
    EXPECT_EQ(fs::file_size(image_path), 0u);
}

TEST_F(IstServiceTest, StartUpdatePldmStripMultiChunk)
{
    init_from_file(configPath_);

    // Payload larger than one 64 KiB strip chunk to exercise the async loop.
    std::vector<uint8_t> payload(200000, 0xAB);
    auto pldm_pkg = build_pldm_package(payload);

    int fd = begin_start_update();
    ASSERT_EQ(::write(fd, pldm_pkg.data(), pldm_pkg.size()),
              static_cast<ssize_t>(pldm_pkg.size()));
    ::close(fd);
    io_.run();

    fs::path image_path = tmpDir_ / "storage" / "CPU-IST.img";
    ASSERT_TRUE(fs::exists(image_path));
    EXPECT_EQ(fs::file_size(image_path), payload.size());

    std::ifstream img(image_path, std::ios::binary);
    std::vector<uint8_t> content((std::istreambuf_iterator<char>(img)),
                                 std::istreambuf_iterator<char>());
    EXPECT_EQ(content, payload);
}

// ----------------
// Version publishing tests
// ----------------

TEST_F(IstServiceTest, ReadAndPublishVersionSucceeds)
{
    std::ofstream(tmpDir_ / "vectors" / "version.txt") << "1.2.3-abc";

    EXPECT_CALL(*publisher_, publishVersion("1.2.3-abc")).Times(1);
    ASSERT_TRUE(init_from_file(configPath_));
}

TEST_F(IstServiceTest, ReadAndPublishVersionMissingFile)
{
    EXPECT_CALL(*publisher_, publishVersion(testing::_)).Times(0);
    ASSERT_TRUE(init_from_file(configPath_));
}

TEST_F(IstServiceTest, ReadAndPublishVersionEmptyFile)
{
    std::ofstream(tmpDir_ / "vectors" / "version.txt") << "";

    EXPECT_CALL(*publisher_, publishVersion(testing::_)).Times(0);
    ASSERT_TRUE(init_from_file(configPath_));
}

TEST_F(IstServiceTest, ReadAndPublishVersionWhitespaceOnly)
{
    std::ofstream(tmpDir_ / "vectors" / "version.txt") << "   \t\n";

    EXPECT_CALL(*publisher_, publishVersion(testing::_)).Times(0);
    ASSERT_TRUE(init_from_file(configPath_));
}

TEST_F(IstServiceTest, ReadAndPublishVersionTrimWhitespace)
{
    std::ofstream(tmpDir_ / "vectors" / "version.txt") << "  1.2.3 \t";

    EXPECT_CALL(*publisher_, publishVersion("1.2.3")).Times(1);
    ASSERT_TRUE(init_from_file(configPath_));
}

TEST_F(IstServiceTest, ReadAndPublishVersionMultipleLines)
{
    std::ofstream(tmpDir_ / "vectors" / "version.txt") << "1.2.3\nextra\n";

    EXPECT_CALL(*publisher_, publishVersion("1.2.3")).Times(1);
    ASSERT_TRUE(init_from_file(configPath_));
}

TEST_F(IstServiceTest, ReadAndPublishVersionVeryLongString)
{
    std::string long_version(4096, 'x');
    std::ofstream(tmpDir_ / "vectors" / "version.txt") << long_version;

    std::string truncated(256, 'x');
    EXPECT_CALL(*publisher_, publishVersion(truncated)).Times(1);
    ASSERT_TRUE(init_from_file(configPath_));
}

// ----------------
// Activation state on initialization tests
// ----------------

TEST_F(IstServiceTest, InitNoImagePublishesActivationFailed)
{
    EXPECT_CALL(*publisher_, publishActivation(StrEq(k_activation_failed)))
        .Times(1);
    ASSERT_TRUE(init_from_file(configPath_));
}

TEST_F(IstServiceTest, InitNoStoragePathPublishesActivationFailed)
{
    write_config(R"({
        "hookDirectory": ")" +
                 (tmpDir_ / "hooks").string() + R"(",
        "hookPaths": {
            "istBootAssert": ")" +
                 (tmpDir_ / "hooks/assert.sh").string() + R"(",
            "istBootDeassert": ")" +
                 (tmpDir_ / "hooks/deassert.sh").string() + R"(",
            "resetSystem": ")" +
                 (tmpDir_ / "hooks/reset.sh").string() + R"("
        },
        "storageConfig": {
            "vectorMountPath": "",
            "vectorStoragePath": "",
            "resultStoragePath": ")" +
                 (tmpDir_ / "results").string() + R"("
        },
        "softwareInventoryId": "IST_Vectors"
    })");

    EXPECT_CALL(*publisher_, publishActivation(StrEq(k_activation_failed)))
        .Times(1);
    ASSERT_TRUE(init_from_file(configPath_));
}

TEST_F(IstServiceTest, InitNoImageDoesNotPublishActivationActive)
{
    EXPECT_CALL(*publisher_, publishActivation(StrEq(k_activation_failed)));
    EXPECT_CALL(*publisher_, publishActivation(StrEq(k_activation_active)))
        .Times(0);
    ASSERT_TRUE(init_from_file(configPath_));
}

// ----------------
// Event log emission tests
// ----------------

TEST_F(IstServiceTest, ItmMismatchEmitsEventLog)
{
    init_from_file(configPath_);

    DoneCb assert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&,
                      std::chrono::seconds) { assert_done = std::move(done); });

    DoneCb power_done;
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([&](DoneCb done) { power_done = std::move(done); });

    ItmDoneCb itm_done;
    EXPECT_CALL(*itmRunner_, asyncRun(::testing::_, ::testing::_, ::testing::_,
                                      ::testing::_))
        .WillOnce([&](const IstTestConfig&, const IstPlatformConfig&,
                      ItmDoneCb done,
                      ProgressCb) { itm_done = std::move(done); });

    DoneCb deassert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootDeassert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&, std::chrono::seconds) {
            deassert_done = std::move(done);
        });

    EXPECT_CALL(*publisher_,
                emitEventLog(::testing::_, ::testing::_, ::testing::_))
        .WillOnce([](const std::string& message, const std::string& severity,
                     const std::map<std::string, std::string>& data) {
            EXPECT_EQ(message, "IST failed");
            EXPECT_EQ(severity,
                      "xyz.openbmc_project.Logging.Entry.Level.Warning");
            EXPECT_EQ(data.at("REDFISH_MESSAGE_ID"),
                      "Platform.1.0.PlatformError");
            EXPECT_EQ(data.at("IST_TYPE"), "CPU");
            EXPECT_EQ(data.at("IST_RESULT"), "Failed");
        });

    ParamMap params;
    start_ist(params);
    assert_done(true);
    power_done(true);
    itm_done(8); // IST_MISMATCH
    io_.run();
    io_.restart();

    deassert_done(true);

    EXPECT_EQ(service_->state().status, IstStatus::failed);
    EXPECT_EQ(service_->state().stage, IstStage::idle);
}

TEST_F(IstServiceTest, ItmPlatformErrorEmitsEventLog)
{
    init_from_file(configPath_);

    DoneCb assert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&,
                      std::chrono::seconds) { assert_done = std::move(done); });

    DoneCb power_done;
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([&](DoneCb done) { power_done = std::move(done); });

    ItmDoneCb itm_done;
    EXPECT_CALL(*itmRunner_, asyncRun(::testing::_, ::testing::_, ::testing::_,
                                      ::testing::_))
        .WillOnce([&](const IstTestConfig&, const IstPlatformConfig&,
                      ItmDoneCb done,
                      ProgressCb) { itm_done = std::move(done); });

    DoneCb deassert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootDeassert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&, std::chrono::seconds) {
            deassert_done = std::move(done);
        });

    EXPECT_CALL(*publisher_,
                emitEventLog(::testing::_, ::testing::_, ::testing::_))
        .WillOnce([](const std::string& message, const std::string& severity,
                     const std::map<std::string, std::string>& data) {
            EXPECT_EQ(message, "IST failed");
            EXPECT_EQ(severity,
                      "xyz.openbmc_project.Logging.Entry.Level.Warning");
            EXPECT_EQ(data.at("REDFISH_MESSAGE_ID"),
                      "Platform.1.0.PlatformError");
            EXPECT_EQ(data.at("IST_TYPE"), "CPU");
            EXPECT_EQ(data.at("IST_RESULT"), "Error");
        });

    ParamMap params;
    start_ist(params);
    assert_done(true);
    power_done(true);
    itm_done(14); // IST_PLATFORM_ERROR
    io_.run();
    io_.restart();

    deassert_done(true);

    EXPECT_EQ(service_->state().status, IstStatus::failed);
    EXPECT_EQ(service_->state().stage, IstStage::idle);
}

TEST_F(IstServiceTest, PlatformErrorIncludesMarkerFiles)
{
    init_from_file(configPath_);

    DoneCb assert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&,
                      std::chrono::seconds) { assert_done = std::move(done); });

    DoneCb power_done;
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([&](DoneCb done) { power_done = std::move(done); });

    ItmDoneCb itm_done;
    EXPECT_CALL(*itmRunner_, asyncRun(::testing::_, ::testing::_, ::testing::_,
                                      ::testing::_))
        .WillOnce([&](const IstTestConfig&, const IstPlatformConfig&,
                      ItmDoneCb done,
                      ProgressCb) { itm_done = std::move(done); });

    DoneCb deassert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootDeassert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&, std::chrono::seconds) {
            deassert_done = std::move(done);
        });

    EXPECT_CALL(*publisher_,
                emitEventLog(::testing::_, ::testing::_, ::testing::_))
        .WillOnce([](const std::string&, const std::string&,
                     const std::map<std::string, std::string>& data) {
            const auto& msg = data.at("IST_MESSAGE");
            EXPECT_THAT(msg, ::testing::HasSubstr("PWR_BRAKE"));
            EXPECT_THAT(msg, ::testing::HasSubstr("THERMAL_FAULT"));
        });

    ParamMap params;
    start_ist(params);
    assert_done(true);
    power_done(true);

    fs::create_directories("/tmp/ist/err_marker");
    std::ofstream("/tmp/ist/err_marker/PWR_BRAKE") << "asserted";
    std::ofstream("/tmp/ist/err_marker/THERMAL_FAULT") << "asserted";

    itm_done(14);
    io_.run();
    io_.restart();
    deassert_done(true);

    fs::remove_all("/tmp/ist/err_marker");
}

TEST_F(IstServiceTest, PreExistingActiveMarkersContinueToIst)
{
    init_from_file(configPath_);

    DoneCb assert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&,
                      std::chrono::seconds) { assert_done = std::move(done); });

    DoneCb power_done;
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([&](DoneCb done) { power_done = std::move(done); });

    fs::create_directories("/tmp/ist/err_marker");
    std::ofstream("/tmp/ist/err_marker/PWR_BRAKE") << "asserted";

    ParamMap params;
    start_ist(params);
    assert_done(true);

    EXPECT_EQ(service_->state().stage, IstStage::pendingPowerCycle);
    EXPECT_TRUE(fs::exists("/tmp/ist/err_marker/PWR_BRAKE"));

    fs::remove_all("/tmp/ist/err_marker");
}

TEST_F(IstServiceTest, DeassertedMarkersCleanedUpAtStart)
{
    init_from_file(configPath_);

    DoneCb assert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&,
                      std::chrono::seconds) { assert_done = std::move(done); });

    DoneCb power_done;
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([&](DoneCb done) { power_done = std::move(done); });

    fs::create_directories("/tmp/ist/err_marker");
    std::ofstream("/tmp/ist/err_marker/PWR_BRAKE") << "deasserted";

    ParamMap params;
    start_ist(params);
    assert_done(true);

    EXPECT_EQ(service_->state().stage, IstStage::pendingPowerCycle);
    EXPECT_FALSE(fs::exists("/tmp/ist/err_marker/PWR_BRAKE"));

    fs::remove_all("/tmp/ist/err_marker");
}

TEST_F(IstServiceTest, ResultsPreservedWhenCollateralVerificationFails)
{
    init_from_file(configPath_);

    // Create a fake result from a previous run
    std::ofstream(tmpDir_ / "results" / "prev_result.txt") << "data";
    fs::create_directories("/tmp/ist/err_marker");
    std::ofstream("/tmp/ist/err_marker/PWR_BRAKE") << "asserted";

    // Remove vectors so collateral verification fails
    fs::remove_all(tmpDir_ / "vectors");

    ParamMap params;
    EXPECT_THROW(service_->startIST(params), ist_err::CollateralNotFound);
    EXPECT_EQ(service_->state().status, IstStatus::aborted);

    // Previous results and markers should still be intact
    EXPECT_TRUE(fs::exists(tmpDir_ / "results" / "prev_result.txt"));
    EXPECT_TRUE(fs::exists("/tmp/ist/err_marker/PWR_BRAKE"));

    fs::remove_all("/tmp/ist/err_marker");
}

TEST_F(IstServiceTest, ItmSuccessDoesNotEmitEventLog)
{
    init_from_file(configPath_);

    DoneCb assert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&,
                      std::chrono::seconds) { assert_done = std::move(done); });

    DoneCb power_done;
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([&](DoneCb done) { power_done = std::move(done); });

    ItmDoneCb itm_done;
    EXPECT_CALL(*itmRunner_, asyncRun(::testing::_, ::testing::_, ::testing::_,
                                      ::testing::_))
        .WillOnce([&](const IstTestConfig&, const IstPlatformConfig&,
                      ItmDoneCb done,
                      ProgressCb) { itm_done = std::move(done); });

    DoneCb deassert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootDeassert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&, std::chrono::seconds) {
            deassert_done = std::move(done);
        });

    EXPECT_CALL(*publisher_,
                emitEventLog(::testing::_, ::testing::_, ::testing::_))
        .Times(0);

    ParamMap params;
    start_ist(params);
    assert_done(true);
    power_done(true);
    itm_done(0); // Success
    io_.run();
    io_.restart();

    deassert_done(true);

    EXPECT_EQ(service_->state().status, IstStatus::completed);
    EXPECT_EQ(service_->state().stage, IstStage::idle);
}

TEST_F(IstServiceTest, ItmInfraFailureDoesNotEmitEventLog)
{
    init_from_file(configPath_);

    DoneCb assert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&,
                      std::chrono::seconds) { assert_done = std::move(done); });

    DoneCb power_done;
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([&](DoneCb done) { power_done = std::move(done); });

    ItmDoneCb itm_done;
    EXPECT_CALL(*itmRunner_, asyncRun(::testing::_, ::testing::_, ::testing::_,
                                      ::testing::_))
        .WillOnce([&](const IstTestConfig&, const IstPlatformConfig&,
                      ItmDoneCb done,
                      ProgressCb) { itm_done = std::move(done); });

    DoneCb deassert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootDeassert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&, std::chrono::seconds) {
            deassert_done = std::move(done);
        });

    EXPECT_CALL(*publisher_,
                emitEventLog(::testing::_, ::testing::_, ::testing::_))
        .Times(0);

    ParamMap params;
    start_ist(params);
    assert_done(true);
    power_done(true);
    itm_done(-1); // Infra failure (e.g. timeout, spawn error)
    io_.run();
    io_.restart();

    deassert_done(true);

    EXPECT_EQ(service_->state().status, IstStatus::failed);
    EXPECT_EQ(service_->state().stage, IstStage::idle);
}

// ----------------
// archiveResults tests
// ----------------

TEST(ArchiveResultsTest, ArchivesFilesAndDeletesOriginals)
{
    fs::path tmp_dir = fs::temp_directory_path() /
                       ("archive_test_" + std::to_string(getpid()));
    fs::create_directories(tmp_dir);

    std::ofstream(tmp_dir / "file1.txt") << "hello";
    std::ofstream(tmp_dir / "file2.gz") << std::string(1024, 'X');

    EXPECT_TRUE(archiveResults(tmp_dir));

    EXPECT_TRUE(fs::exists(tmp_dir / "ist_results.tar.gz"));
    EXPECT_FALSE(fs::exists(tmp_dir / "file1.txt"));
    EXPECT_FALSE(fs::exists(tmp_dir / "file2.gz"));
    EXPECT_GT(fs::file_size(tmp_dir / "ist_results.tar.gz"), 0u);

    fs::remove_all(tmp_dir);
}

TEST(ArchiveResultsTest, ReturnsFalseOnEmptyDirectory)
{
    fs::path tmp_dir = fs::temp_directory_path() /
                       ("archive_empty_" + std::to_string(getpid()));
    fs::create_directories(tmp_dir);

    EXPECT_FALSE(archiveResults(tmp_dir));
    EXPECT_FALSE(fs::exists(tmp_dir / "ist_results.tar.gz"));

    fs::remove_all(tmp_dir);
}

TEST(ArchiveResultsTest, SkipsExistingArchiveFile)
{
    fs::path tmp_dir = fs::temp_directory_path() /
                       ("archive_skip_" + std::to_string(getpid()));
    fs::create_directories(tmp_dir);

    std::ofstream(tmp_dir / "ist_results.tar.gz") << "old archive";
    std::ofstream(tmp_dir / "data.txt") << "new data";

    EXPECT_TRUE(archiveResults(tmp_dir));

    EXPECT_TRUE(fs::exists(tmp_dir / "ist_results.tar.gz"));
    EXPECT_FALSE(fs::exists(tmp_dir / "data.txt"));

    fs::remove_all(tmp_dir);
}

// ----------------
// getResultsFd tests
// ----------------

TEST_F(IstServiceTest, GetResultsFdReturnsValidFdWhenArchiveExists)
{
    init_from_file(configPath_);

    fs::path results_dir = tmpDir_ / "results";
    std::ofstream(results_dir / "ist_results.tar.gz") << "archive data";

    int fd = static_cast<int>(service_->getResultsFd());
    EXPECT_GE(fd, 0);

    char buf[64] = {};
    ssize_t n = ::read(fd, buf, sizeof(buf));
    EXPECT_EQ(std::string(buf, static_cast<size_t>(n)), "archive data");
    ::close(fd);
}

TEST_F(IstServiceTest, GetResultsFdThrowsWhenNoArchive)
{
    init_from_file(configPath_);

    EXPECT_THROW(service_->getResultsFd(), ist_err::ResourceNotFound);
}

TEST_F(IstServiceTest, GetResultsFdThrowsWhenArchiveTooLarge)
{
    init_from_file(configPath_);

    fs::path results_dir = tmpDir_ / "results";
    fs::path archive = results_dir / "ist_results.tar.gz";

    // Create a file just over the 50MB limit via truncate
    std::ofstream ofs(archive, std::ios::binary);
    ofs.seekp(50LL * 1024 * 1024);
    ofs.put('\0');
    ofs.close();

    EXPECT_THROW(service_->getResultsFd(), ist_err::InternalFailure);
}

TEST_F(IstServiceTest, StartIstCleansUpOldResults)
{
    init_from_file(configPath_);

    fs::path results_dir = tmpDir_ / "results";
    std::ofstream(results_dir / "ist_results.tar.gz") << "stale archive";
    std::ofstream(results_dir / "leftover.gz") << "stale data";
    EXPECT_TRUE(fs::exists(results_dir / "ist_results.tar.gz"));
    EXPECT_TRUE(fs::exists(results_dir / "leftover.gz"));

    ParamMap params;
    start_ist(params);

    EXPECT_FALSE(fs::exists(results_dir / "ist_results.tar.gz"));
    EXPECT_FALSE(fs::exists(results_dir / "leftover.gz"));
    EXPECT_TRUE(fs::exists(results_dir));
}

// ----------------
// CPU discovery tests
// ----------------

using CpuDoneCb =
    std::move_only_function<void(const std::vector<std::string>&) const>;

TEST_F(IstServiceTest, CpuDiscoverySetsTwoCpuSocketList)
{
    init_from_file(configPath_);

    service_->setCpuDiscoverer([](CpuDoneCb done) {
        done({"/xyz/openbmc_project/inventory/system/chassis/CPU_0",
              "/xyz/openbmc_project/inventory/system/chassis/CPU_1"});
    });

    IstTestConfig captured_cfg;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([](const std::string&, const std::string&, DoneCb done,
                     const std::vector<std::string>&,
                     std::chrono::seconds) { done(true); });
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([](DoneCb done) { done(true); });
    EXPECT_CALL(*itmRunner_, asyncRun(::testing::_, ::testing::_, ::testing::_,
                                      ::testing::_))
        .WillOnce([&](const IstTestConfig& cfg, const IstPlatformConfig&,
                      ItmDoneCb done, ProgressCb) {
            captured_cfg = cfg;
            done(0);
        });
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootDeassert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([](const std::string&, const std::string&, DoneCb done,
                     const std::vector<std::string>&,
                     std::chrono::seconds) { done(true); });

    start_ist();
    io_.run();
    io_.restart();

    ASSERT_TRUE(captured_cfg.customSocketList.has_value());
    EXPECT_EQ(*captured_cfg.customSocketList, "0,1");
}

TEST_F(IstServiceTest, CpuDiscoverySetsOneCpuSocketList)
{
    init_from_file(configPath_);

    service_->setCpuDiscoverer([](CpuDoneCb done) {
        done({"/xyz/openbmc_project/inventory/system/chassis/CPU_0"});
    });

    IstTestConfig captured_cfg;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([](const std::string&, const std::string&, DoneCb done,
                     const std::vector<std::string>&,
                     std::chrono::seconds) { done(true); });
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([](DoneCb done) { done(true); });
    EXPECT_CALL(*itmRunner_, asyncRun(::testing::_, ::testing::_, ::testing::_,
                                      ::testing::_))
        .WillOnce([&](const IstTestConfig& cfg, const IstPlatformConfig&,
                      ItmDoneCb done, ProgressCb) {
            captured_cfg = cfg;
            done(0);
        });
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootDeassert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([](const std::string&, const std::string&, DoneCb done,
                     const std::vector<std::string>&,
                     std::chrono::seconds) { done(true); });

    start_ist();
    io_.run();
    io_.restart();

    ASSERT_TRUE(captured_cfg.customSocketList.has_value());
    EXPECT_EQ(*captured_cfg.customSocketList, "0");
}

TEST_F(IstServiceTest, CpuDiscoveryEmptyFailsIst)
{
    init_from_file(configPath_);

    service_->setCpuDiscoverer([](CpuDoneCb done) { done({}); });

    start_ist();

    EXPECT_EQ(service_->state().stage, IstStage::idle);
    EXPECT_EQ(service_->state().status, IstStatus::failed);
}

TEST_F(IstServiceTest, UserSocketListSkipsCpuDiscovery)
{
    init_from_file(configPath_);

    bool discoverer_called = false;
    service_->setCpuDiscoverer([&](CpuDoneCb done) {
        discoverer_called = true;
        done({"/xyz/openbmc_project/inventory/system/chassis/CPU_0"});
    });

    IstTestConfig captured_cfg;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([](const std::string&, const std::string&, DoneCb done,
                     const std::vector<std::string>&,
                     std::chrono::seconds) { done(true); });
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([](DoneCb done) { done(true); });
    EXPECT_CALL(*itmRunner_, asyncRun(::testing::_, ::testing::_, ::testing::_,
                                      ::testing::_))
        .WillOnce([&](const IstTestConfig& cfg, const IstPlatformConfig&,
                      ItmDoneCb done, ProgressCb) {
            captured_cfg = cfg;
            done(0);
        });
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootDeassert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([](const std::string&, const std::string&, DoneCb done,
                     const std::vector<std::string>&,
                     std::chrono::seconds) { done(true); });

    ParamMap params;
    params["customSocketList"] = std::string("3");
    start_ist(params);
    io_.run();
    io_.restart();

    EXPECT_FALSE(discoverer_called);
    ASSERT_TRUE(captured_cfg.customSocketList.has_value());
    EXPECT_EQ(*captured_cfg.customSocketList, "3");
}

TEST_F(IstServiceTest, CpuDiscoveryOutOfOrderPathsSorted)
{
    init_from_file(configPath_);

    service_->setCpuDiscoverer([](CpuDoneCb done) {
        done({"/xyz/openbmc_project/inventory/system/chassis/CPU_1",
              "/xyz/openbmc_project/inventory/system/chassis/CPU_0"});
    });

    IstTestConfig captured_cfg;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([](const std::string&, const std::string&, DoneCb done,
                     const std::vector<std::string>&,
                     std::chrono::seconds) { done(true); });
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([](DoneCb done) { done(true); });
    EXPECT_CALL(*itmRunner_, asyncRun(::testing::_, ::testing::_, ::testing::_,
                                      ::testing::_))
        .WillOnce([&](const IstTestConfig& cfg, const IstPlatformConfig&,
                      ItmDoneCb done, ProgressCb) {
            captured_cfg = cfg;
            done(0);
        });
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootDeassert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([](const std::string&, const std::string&, DoneCb done,
                     const std::vector<std::string>&,
                     std::chrono::seconds) { done(true); });

    start_ist();
    io_.run();
    io_.restart();

    ASSERT_TRUE(captured_cfg.customSocketList.has_value());
    EXPECT_EQ(*captured_cfg.customSocketList, "0,1");
}

TEST_F(IstServiceTest, CpuDiscoveryNonContiguousSocketsFails)
{
    init_from_file(configPath_);

    service_->setCpuDiscoverer([](CpuDoneCb done) {
        done({"/xyz/openbmc_project/inventory/system/chassis/CPU_1"});
    });

    start_ist();

    EXPECT_EQ(service_->state().stage, IstStage::idle);
    EXPECT_EQ(service_->state().status, IstStatus::failed);
}

TEST_F(IstServiceTest, CpuDiscoveryBadPathFails)
{
    init_from_file(configPath_);

    service_->setCpuDiscoverer([](CpuDoneCb done) {
        done({"/xyz/openbmc_project/inventory/system/chassis/CPU0"});
    });

    start_ist();

    EXPECT_EQ(service_->state().stage, IstStage::idle);
    EXPECT_EQ(service_->state().status, IstStatus::failed);
}

TEST_F(IstServiceTest, NoCpuDiscovererSkipsDiscovery)
{
    init_from_file(configPath_);

    std::move_only_function<void(bool) const> assert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&,
                      std::chrono::seconds) { assert_done = std::move(done); });

    start_ist();

    EXPECT_EQ(service_->state().stage, IstStage::pendingIstBoot);
    ASSERT_TRUE(assert_done);
}

// ----------------
// Service log tee tests
// ----------------

TEST_F(IstServiceTest, ServiceLogCreatedOnStartIst)
{
    init_from_file(configPath_);

    fs::path log_path = tmpDir_ / "results" / "IST_service.log";
    EXPECT_FALSE(fs::exists(log_path));

    start_ist();

    EXPECT_TRUE(fs::exists(log_path));
}

TEST_F(IstServiceTest, ServiceLogCapturesCerrOutput)
{
    init_from_file(configPath_);

    DoneCb assert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&,
                      std::chrono::seconds) { assert_done = std::move(done); });

    start_ist();

    // Force a cerr write while tee is active
    std::cerr << "test_service_log_message\n";

    // Simulate failure to drive to idle (removes tee)
    assert_done(false);

    fs::path log_path = tmpDir_ / "results" / "IST_service.log";
    ASSERT_TRUE(fs::exists(log_path));

    std::ifstream f(log_path);
    std::string content((std::istreambuf_iterator<char>(f)), {});
    EXPECT_NE(content.find("test_service_log_message"), std::string::npos);
}

TEST_F(IstServiceTest, ServiceLogTeeRemovedOnIdle)
{
    init_from_file(configPath_);

    DoneCb assert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&,
                      std::chrono::seconds) { assert_done = std::move(done); });

    start_ist();

    // Drive to idle
    assert_done(false);
    EXPECT_EQ(service_->state().stage, IstStage::idle);

    // Write after tee removed — should NOT appear in log
    std::cerr << "after_tee_removed\n";

    fs::path log_path = tmpDir_ / "results" / "IST_service.log";
    std::ifstream f(log_path);
    std::string content((std::istreambuf_iterator<char>(f)), {});
    EXPECT_EQ(content.find("after_tee_removed"), std::string::npos);
}

// ----------------
// IST_ERROR structured error message tests
// ----------------

TEST_F(IstServiceTest, ErrorCollateralVerificationFailed)
{
    fs::remove_all(tmpDir_ / "vectors");
    init_from_file(configPath_);

    testing::internal::CaptureStderr();
    ParamMap params;
    EXPECT_THROW(service_->startIST(params), ist_err::CollateralNotFound);
    std::string err = testing::internal::GetCapturedStderr();

    EXPECT_THAT(
        err, ::testing::HasSubstr(
                 "IST_ERROR: category=COLLATERAL, reason=verification_failed"));
}

TEST_F(IstServiceTest, ErrorHookAssertNotFound)
{
    write_config(R"({
        "hookDirectory": ")" +
                 (tmpDir_ / "hooks").string() + R"(",
        "hookPaths": {
            "istBootDeassert": ")" +
                 (tmpDir_ / "hooks/deassert.sh").string() + R"("
        },
        "storageConfig": {
            "vectorMountPath": ")" +
                 (tmpDir_ / "vectors").string() + R"(",
            "vectorStoragePath": ")" +
                 (tmpDir_ / "storage").string() + R"(",
            "resultStoragePath": ")" +
                 (tmpDir_ / "results").string() + R"("
        },
        "softwareInventoryId": "IST_Vectors"
    })");
    init_from_file(configPath_);

    testing::internal::CaptureStderr();
    ParamMap params;
    EXPECT_THROW(service_->startIST(params), ist_err::HookNotFound);
    std::string err = testing::internal::GetCapturedStderr();

    EXPECT_THAT(
        err, ::testing::HasSubstr(
                 "IST_ERROR: category=HOOK, reason=ist_boot_assert_not_found"));
}

TEST_F(IstServiceTest, ErrorHookAssertFailed)
{
    init_from_file(configPath_);

    DoneCb assert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&,
                      std::chrono::seconds) { assert_done = std::move(done); });

    testing::internal::CaptureStderr();
    start_ist();
    assert_done(false);
    std::string err = testing::internal::GetCapturedStderr();

    EXPECT_THAT(err,
                ::testing::HasSubstr(
                    "IST_ERROR: category=HOOK, reason=ist_boot_assert_failed"));
}

TEST_F(IstServiceTest, ErrorHookDeassertNotFound)
{
    write_config(R"({
        "hookDirectory": ")" +
                 (tmpDir_ / "hooks").string() + R"(",
        "hookPaths": {
            "istBootAssert": ")" +
                 (tmpDir_ / "hooks/assert.sh").string() + R"("
        },
        "storageConfig": {
            "vectorMountPath": ")" +
                 (tmpDir_ / "vectors").string() + R"(",
            "vectorStoragePath": ")" +
                 (tmpDir_ / "storage").string() + R"(",
            "resultStoragePath": ")" +
                 (tmpDir_ / "results").string() + R"("
        },
        "softwareInventoryId": "IST_Vectors"
    })");
    init_from_file(configPath_);

    testing::internal::CaptureStderr();
    ParamMap params;
    EXPECT_THROW(service_->startIST(params), ist_err::HookNotFound);
    std::string err = testing::internal::GetCapturedStderr();

    EXPECT_THAT(
        err,
        ::testing::HasSubstr(
            "IST_ERROR: category=HOOK, reason=ist_boot_deassert_not_found"));
    EXPECT_EQ(service_->state().status, IstStatus::failed);
    EXPECT_EQ(service_->state().stage, IstStage::idle);
}

TEST_F(IstServiceTest, ErrorHookDeassertFailed)
{
    init_from_file(configPath_);

    DoneCb assert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&,
                      std::chrono::seconds) { assert_done = std::move(done); });

    DoneCb power_done;
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([&](DoneCb done) { power_done = std::move(done); });

    ItmDoneCb itm_done;
    EXPECT_CALL(*itmRunner_, asyncRun(::testing::_, ::testing::_, ::testing::_,
                                      ::testing::_))
        .WillOnce([&](const IstTestConfig&, const IstPlatformConfig&,
                      ItmDoneCb done,
                      ProgressCb) { itm_done = std::move(done); });

    DoneCb deassert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootDeassert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&, std::chrono::seconds) {
            deassert_done = std::move(done);
        });

    testing::internal::CaptureStderr();
    start_ist();
    assert_done(true);
    power_done(true);
    itm_done(0);
    io_.run();
    io_.restart();
    deassert_done(false);
    std::string err = testing::internal::GetCapturedStderr();

    EXPECT_THAT(
        err, ::testing::HasSubstr(
                 "IST_ERROR: category=HOOK, reason=ist_boot_deassert_failed"));
}

TEST_F(IstServiceTest, ErrorHookResetFailed)
{
    init_from_file(configPath_);

    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([](const std::string&, const std::string&, DoneCb done,
                     const std::vector<std::string>&,
                     std::chrono::seconds) { done(true); });
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([](DoneCb done) { done(true); });
    EXPECT_CALL(*itmRunner_, asyncRun(::testing::_, ::testing::_, ::testing::_,
                                      ::testing::_))
        .WillOnce([](const IstTestConfig&, const IstPlatformConfig&,
                     ItmDoneCb done, ProgressCb) { done(0); });
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootDeassert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([](const std::string&, const std::string&, DoneCb done,
                     const std::vector<std::string>&,
                     std::chrono::seconds) { done(true); });

    DoneCb reset_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("resetSystem hook"), ::testing::_,
                         ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&,
                      std::chrono::seconds) { reset_done = std::move(done); });

    testing::internal::CaptureStderr();
    ParamMap params;
    params["autoRebootOnComplete"] = true;
    start_ist(params);
    io_.run();
    io_.restart();
    reset_done(false);
    std::string err = testing::internal::GetCapturedStderr();

    EXPECT_THAT(err,
                ::testing::HasSubstr(
                    "IST_ERROR: category=HOOK, reason=reset_system_failed"));
}

TEST_F(IstServiceTest, ErrorHookResetNotFound)
{
    write_config(R"({
        "hookDirectory": ")" +
                 (tmpDir_ / "hooks").string() + R"(",
        "hookPaths": {
            "istBootAssert": ")" +
                 (tmpDir_ / "hooks/assert.sh").string() + R"(",
            "istBootDeassert": ")" +
                 (tmpDir_ / "hooks/deassert.sh").string() + R"("
        },
        "storageConfig": {
            "vectorMountPath": ")" +
                 (tmpDir_ / "vectors").string() + R"(",
            "vectorStoragePath": ")" +
                 (tmpDir_ / "storage").string() + R"(",
            "resultStoragePath": ")" +
                 (tmpDir_ / "results").string() + R"("
        },
        "softwareInventoryId": "IST_Vectors"
    })");
    init_from_file(configPath_);

    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([](const std::string&, const std::string&, DoneCb done,
                     const std::vector<std::string>&,
                     std::chrono::seconds) { done(true); });
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([](DoneCb done) { done(true); });
    EXPECT_CALL(*itmRunner_, asyncRun(::testing::_, ::testing::_, ::testing::_,
                                      ::testing::_))
        .WillOnce([](const IstTestConfig&, const IstPlatformConfig&,
                     ItmDoneCb done, ProgressCb) { done(0); });
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootDeassert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([](const std::string&, const std::string&, DoneCb done,
                     const std::vector<std::string>&,
                     std::chrono::seconds) { done(true); });

    testing::internal::CaptureStderr();
    ParamMap params;
    params["autoRebootOnComplete"] = true;
    start_ist(params);
    io_.run();
    io_.restart();
    std::string err = testing::internal::GetCapturedStderr();

    EXPECT_THAT(err,
                ::testing::HasSubstr(
                    "IST_ERROR: category=HOOK, reason=reset_system_not_found"));
}

TEST_F(IstServiceTest, ErrorHookCakBypassFailed)
{
    std::ofstream give_me_a_name(tmpDir_ / "hooks" / "ist_cak_bypass.sh");
    init_from_file(configPath_);

    DoneCb assert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&,
                      std::chrono::seconds) { assert_done = std::move(done); });

    DoneCb power_done;
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([&](DoneCb done) { power_done = std::move(done); });

    DoneCb cak_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("CAK bypass"), ::testing::_,
                         ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&,
                      std::chrono::seconds) { cak_done = std::move(done); });

    DoneCb deassert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootDeassert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&, std::chrono::seconds) {
            deassert_done = std::move(done);
        });

    testing::internal::CaptureStderr();
    start_ist();
    assert_done(true);
    power_done(true);
    ASSERT_TRUE(cak_done);
    cak_done(false);
    io_.run();
    io_.restart();
    deassert_done(true);
    std::string err = testing::internal::GetCapturedStderr();

    EXPECT_THAT(err, ::testing::HasSubstr(
                         "IST_ERROR: category=HOOK, reason=cak_bypass_failed"));
}

TEST_F(IstServiceTest, ErrorPowerCycleNotDetected)
{
    init_from_file(configPath_);

    DoneCb assert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&,
                      std::chrono::seconds) { assert_done = std::move(done); });

    DoneCb power_done;
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([&](DoneCb done) { power_done = std::move(done); });

    DoneCb deassert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootDeassert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&, std::chrono::seconds) {
            deassert_done = std::move(done);
        });

    testing::internal::CaptureStderr();
    start_ist();
    assert_done(true);
    power_done(false);
    io_.run();
    io_.restart();
    deassert_done(true);
    std::string err = testing::internal::GetCapturedStderr();

    EXPECT_THAT(err,
                ::testing::HasSubstr(
                    "IST_ERROR: category=POWER_CYCLE, reason=not_detected"));
}

TEST_F(IstServiceTest, ErrorItmExitCode)
{
    init_from_file(configPath_);

    DoneCb assert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&,
                      std::chrono::seconds) { assert_done = std::move(done); });

    DoneCb power_done;
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([&](DoneCb done) { power_done = std::move(done); });

    ItmDoneCb itm_done;
    EXPECT_CALL(*itmRunner_, asyncRun(::testing::_, ::testing::_, ::testing::_,
                                      ::testing::_))
        .WillOnce([&](const IstTestConfig&, const IstPlatformConfig&,
                      ItmDoneCb done,
                      ProgressCb) { itm_done = std::move(done); });

    DoneCb deassert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootDeassert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&, std::chrono::seconds) {
            deassert_done = std::move(done);
        });

    testing::internal::CaptureStderr();
    start_ist();
    assert_done(true);
    power_done(true);
    itm_done(8);
    io_.run();
    io_.restart();
    deassert_done(true);
    std::string err = testing::internal::GetCapturedStderr();

    EXPECT_THAT(err,
                ::testing::HasSubstr("IST_ERROR: category=ITM, exit_code=8"));
}

TEST_F(IstServiceTest, ErrorItmSwTimeout)
{
    init_from_file(configPath_);

    DoneCb assert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&,
                      std::chrono::seconds) { assert_done = std::move(done); });

    DoneCb power_done;
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([&](DoneCb done) { power_done = std::move(done); });

    ItmDoneCb itm_done;
    EXPECT_CALL(*itmRunner_, asyncRun(::testing::_, ::testing::_, ::testing::_,
                                      ::testing::_))
        .WillOnce([&](const IstTestConfig&, const IstPlatformConfig&,
                      ItmDoneCb done,
                      ProgressCb) { itm_done = std::move(done); });

    DoneCb deassert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootDeassert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&, std::chrono::seconds) {
            deassert_done = std::move(done);
        });

    testing::internal::CaptureStderr();
    start_ist();
    assert_done(true);
    power_done(true);
    itm_done(itm_exit_sw_timeout);
    io_.run();
    io_.restart();
    deassert_done(true);
    std::string err = testing::internal::GetCapturedStderr();

    EXPECT_THAT(err, ::testing::HasSubstr(
                         "IST_ERROR: category=ITM, reason=sw_timeout"));
}

TEST_F(IstServiceTest, ErrorSecuritySignatureFailed)
{
    init_from_file(configPath_);
    service_->setSignatureVerifier(
        [](const IstPlatformConfig&, DoneCb cb) { cb(false); });

    testing::internal::CaptureStderr();
    start_ist();
    std::string err = testing::internal::GetCapturedStderr();

    EXPECT_THAT(err,
                ::testing::HasSubstr("IST_ERROR: category=SECURITY, "
                                     "reason=signature_verification_failed"));
    EXPECT_EQ(service_->state().status, IstStatus::failed);
}

TEST_F(IstServiceTest, ErrorDoubleFailureItmAndDeassert)
{
    init_from_file(configPath_);

    DoneCb assert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&,
                      std::chrono::seconds) { assert_done = std::move(done); });

    DoneCb power_done;
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([&](DoneCb done) { power_done = std::move(done); });

    ItmDoneCb itm_done;
    EXPECT_CALL(*itmRunner_, asyncRun(::testing::_, ::testing::_, ::testing::_,
                                      ::testing::_))
        .WillOnce([&](const IstTestConfig&, const IstPlatformConfig&,
                      ItmDoneCb done,
                      ProgressCb) { itm_done = std::move(done); });

    DoneCb deassert_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootDeassert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done,
                      const std::vector<std::string>&, std::chrono::seconds) {
            deassert_done = std::move(done);
        });

    testing::internal::CaptureStderr();
    start_ist();
    assert_done(true);
    power_done(true);
    itm_done(8);
    io_.run();
    io_.restart();
    deassert_done(false);
    std::string err = testing::internal::GetCapturedStderr();

    EXPECT_THAT(err,
                ::testing::HasSubstr("IST_ERROR: category=ITM, exit_code=8"));
    EXPECT_THAT(
        err, ::testing::HasSubstr(
                 "IST_ERROR: category=HOOK, reason=ist_boot_deassert_failed"));
}

TEST_F(IstServiceTest, NoErrorOnSuccessfulRun)
{
    init_from_file(configPath_);

    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootAssert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([](const std::string&, const std::string&, DoneCb done,
                     const std::vector<std::string>&,
                     std::chrono::seconds) { done(true); });
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([](DoneCb done) { done(true); });
    EXPECT_CALL(*itmRunner_, asyncRun(::testing::_, ::testing::_, ::testing::_,
                                      ::testing::_))
        .WillOnce([](const IstTestConfig&, const IstPlatformConfig&,
                     ItmDoneCb done, ProgressCb) { done(0); });
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("istBootDeassert hook"),
                         ::testing::_, ::testing::_, ::testing::_))
        .WillOnce([](const std::string&, const std::string&, DoneCb done,
                     const std::vector<std::string>&,
                     std::chrono::seconds) { done(true); });

    testing::internal::CaptureStderr();
    start_ist();
    io_.run();
    io_.restart();
    std::string err = testing::internal::GetCapturedStderr();

    EXPECT_THAT(err, ::testing::Not(::testing::HasSubstr("IST_ERROR:")));
    EXPECT_EQ(service_->state().status, IstStatus::completed);
}
