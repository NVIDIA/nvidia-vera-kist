#include <ist_app.hpp>
#include <sdbusplus/exception.hpp>

#include <filesystem>
#include <fstream>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace fs = std::filesystem;
using ::testing::NiceMock;
using ::testing::StrEq;

using DoneCb = std::move_only_function<void(bool) const>;
using ProgressCb = std::move_only_function<void(uint8_t) const>;

// ----------------
// Mock implementations for dependency injection
// ----------------

class MockHookRunner : public HookRunner
{
  public:
    MOCK_METHOD(void, asyncRun,
                (const std::string& cmd, std::string what,
                 std::move_only_function<void(bool ok) const> done),
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
                 std::move_only_function<void(bool ok) const> done,
                 std::move_only_function<void(uint8_t) const> on_progress),
                (override));
};

class MockStatePublisher : public StatePublisher
{
  public:
    MOCK_METHOD(void, publish, (const IstState& state), (override));
    MOCK_METHOD(void, publishProgress, (uint8_t progress), (override));
    MOCK_METHOD(void, createProgress, (), (override));
    MOCK_METHOD(void, removeProgress, (), (override));
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
    EXPECT_TRUE(cfg.hookDir.empty());
    EXPECT_TRUE(cfg.hooks.empty());
    EXPECT_TRUE(cfg.storage.empty());
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
        fs::create_directories(tmpDir_ / "vectors");
        fs::create_directories(tmpDir_ / "results");
        fs::create_directories(tmpDir_ / "hooks");

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
                "resultStoragePath": ")" +
                     (tmpDir_ / "results").string() + R"("
            }
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

        service_ =
            IstService::create(std::move(publisher), std::move(hook_runner),
                               std::move(power_monitor), std::move(itm_runner));
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

    std::shared_ptr<IstService> service_;

    MockStatePublisher* publisher_ = nullptr;
    MockHookRunner* hookRunner_ = nullptr;
    MockHostPowerMonitor* powerMonitor_ = nullptr;
    MockItmRunner* itmRunner_ = nullptr;

    fs::path tmpDir_;
    std::string configPath_;

    static inline int counter = 0;
};

// ----------------
// Initialize tests
// ----------------

TEST_F(IstServiceTest, InitializeValidConfig)
{
    EXPECT_TRUE(service_->initialize(configPath_));
}

TEST_F(IstServiceTest, InitializeInvalidPath)
{
    EXPECT_FALSE(service_->initialize("/nonexistent/path.json"));
}

TEST_F(IstServiceTest, InitializeMalformedJson)
{
    write_config("not valid json {{{");
    EXPECT_FALSE(service_->initialize(configPath_));
}

TEST_F(IstServiceTest, InitializeMissingHookDirectory)
{
    write_config(R"({ "storageConfig": {} })");
    EXPECT_FALSE(service_->initialize(configPath_));
}

TEST_F(IstServiceTest, InitializeRejectsDoubleInit)
{
    EXPECT_TRUE(service_->initialize(configPath_));
    EXPECT_FALSE(service_->initialize(configPath_));
}

TEST_F(IstServiceTest, InitializeRejectsHookOutsideHookDir)
{
    write_config(R"({
        "hookDirectory": ")" +
                 (tmpDir_ / "hooks").string() + R"(",
        "hookPaths": {
            "istBootAssert": "/etc/shadow"
        },
        "storageConfig": {}
    })");
    EXPECT_FALSE(service_->initialize(configPath_));
}

// ----------------
// StartIST tests
// ----------------

TEST_F(IstServiceTest, StartIstRejectsWhenInProgress)
{
    service_->initialize(configPath_);

    // First call should succeed
    ParamMap params;
    EXPECT_NO_THROW(service_->startIST(params));
    EXPECT_EQ(service_->state().status, IstStatus::inProgress);

    // Second call while in progress should throw EBUSY
    ParamMap params2;
    EXPECT_THROW(service_->startIST(params2), sdbusplus::exception::SdBusError);
}

TEST_F(IstServiceTest, StartIstAbortsOnMissingVectorStorage)
{
    // Remove the vectors directory so collateral verification fails
    fs::remove_all(tmpDir_ / "vectors");

    service_->initialize(configPath_);

    ParamMap params;
    EXPECT_THROW(service_->startIST(params), sdbusplus::exception::SdBusError);
    EXPECT_EQ(service_->state().status, IstStatus::aborted);
    EXPECT_EQ(service_->state().stage, IstStage::idle);
}

TEST_F(IstServiceTest, StartIstAbortsOnUnknownParam)
{
    service_->initialize(configPath_);

    ParamMap params;
    params["unknownParam"] = std::string("value");
    EXPECT_THROW(service_->startIST(params), sdbusplus::exception::SdBusError);
    EXPECT_EQ(service_->state().status, IstStatus::aborted);
}

TEST_F(IstServiceTest, StartIstAbortsOnOversizedParam)
{
    service_->initialize(configPath_);

    ParamMap params;
    params["customTestList"] = std::string(5000, 'A'); // exceeds 4096 limit
    EXPECT_THROW(service_->startIST(params), sdbusplus::exception::SdBusError);
    EXPECT_EQ(service_->state().status, IstStatus::aborted);
}

TEST_F(IstServiceTest, StartIstCallsAssertHook)
{
    service_->initialize(configPath_);

    // Expect the istBootAssert hook to be called
    std::move_only_function<void(bool) const> assert_done;
    EXPECT_CALL(
        *hookRunner_,
        asyncRun(::testing::_, StrEq("istBootAssert hook"), ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done) {
            assert_done = std::move(done);
        });

    ParamMap params;
    service_->startIST(params);

    EXPECT_EQ(service_->state().status, IstStatus::inProgress);
    EXPECT_EQ(service_->state().stage, IstStage::pendingIstBoot);

    // Verify callback was captured
    ASSERT_TRUE(assert_done);
}

TEST_F(IstServiceTest, AssertHookFailureTransitionsToFailed)
{
    service_->initialize(configPath_);

    std::move_only_function<void(bool) const> assert_done;
    EXPECT_CALL(
        *hookRunner_,
        asyncRun(::testing::_, StrEq("istBootAssert hook"), ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done) {
            assert_done = std::move(done);
        });

    ParamMap params;
    service_->startIST(params);

    // Simulate hook failure
    assert_done(false);

    EXPECT_EQ(service_->state().status, IstStatus::failed);
    EXPECT_EQ(service_->state().stage, IstStage::idle);
}

TEST_F(IstServiceTest, AssertHookSuccessWaitsForPowerCycle)
{
    service_->initialize(configPath_);

    std::move_only_function<void(bool) const> assert_done;
    EXPECT_CALL(
        *hookRunner_,
        asyncRun(::testing::_, StrEq("istBootAssert hook"), ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done) {
            assert_done = std::move(done);
        });

    std::move_only_function<void(bool) const> power_done;
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([&](DoneCb done) { power_done = std::move(done); });

    ParamMap params;
    service_->startIST(params);

    // Simulate hook success → should start power cycle wait
    assert_done(true);

    EXPECT_EQ(service_->state().stage, IstStage::pendingPowerCycle);
    ASSERT_TRUE(power_done);
}

TEST_F(IstServiceTest, PowerCycleFailureRunsDeassertThenFails)
{
    service_->initialize(configPath_);

    std::move_only_function<void(bool) const> assert_done;
    EXPECT_CALL(
        *hookRunner_,
        asyncRun(::testing::_, StrEq("istBootAssert hook"), ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done) {
            assert_done = std::move(done);
        });

    std::move_only_function<void(bool) const> power_done;
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([&](DoneCb done) { power_done = std::move(done); });

    std::move_only_function<void(bool) const> deassert_done;
    EXPECT_CALL(
        *hookRunner_,
        asyncRun(::testing::_, StrEq("istBootDeassert hook"), ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done) {
            deassert_done = std::move(done);
        });

    ParamMap params;
    service_->startIST(params);
    assert_done(true);

    // Simulate power cycle failure → should trigger cleanup/deassert
    power_done(false);

    EXPECT_EQ(service_->state().stage, IstStage::cleanup);
    ASSERT_TRUE(deassert_done);

    deassert_done(true);

    EXPECT_EQ(service_->state().status, IstStatus::failed);
    EXPECT_EQ(service_->state().stage, IstStage::idle);
}

TEST_F(IstServiceTest, PowerCycleSuccessStartsItm)
{
    service_->initialize(configPath_);

    std::move_only_function<void(bool) const> assert_done;
    EXPECT_CALL(
        *hookRunner_,
        asyncRun(::testing::_, StrEq("istBootAssert hook"), ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done) {
            assert_done = std::move(done);
        });

    std::move_only_function<void(bool) const> power_done;
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([&](DoneCb done) { power_done = std::move(done); });

    std::move_only_function<void(bool) const> itm_done;
    EXPECT_CALL(*itmRunner_, asyncRun(::testing::_, ::testing::_, ::testing::_,
                                      ::testing::_))
        .WillOnce([&](const IstTestConfig&, const IstPlatformConfig&,
                      DoneCb done, ProgressCb) { itm_done = std::move(done); });

    ParamMap params;
    service_->startIST(params);
    assert_done(true);
    power_done(true);

    EXPECT_EQ(service_->state().stage, IstStage::runningIst);
    ASSERT_TRUE(itm_done);
}

TEST_F(IstServiceTest, ItmSuccessCompletesWithCleanup)
{
    service_->initialize(configPath_);

    // Chain: assert hook → power cycle → ITM → deassert hook
    std::move_only_function<void(bool) const> assert_done;
    EXPECT_CALL(
        *hookRunner_,
        asyncRun(::testing::_, StrEq("istBootAssert hook"), ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done) {
            assert_done = std::move(done);
        });

    std::move_only_function<void(bool) const> power_done;
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([&](DoneCb done) { power_done = std::move(done); });

    std::move_only_function<void(bool) const> itm_done;
    EXPECT_CALL(*itmRunner_, asyncRun(::testing::_, ::testing::_, ::testing::_,
                                      ::testing::_))
        .WillOnce([&](const IstTestConfig&, const IstPlatformConfig&,
                      DoneCb done, ProgressCb) { itm_done = std::move(done); });

    std::move_only_function<void(bool) const> deassert_done;
    EXPECT_CALL(
        *hookRunner_,
        asyncRun(::testing::_, StrEq("istBootDeassert hook"), ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done) {
            deassert_done = std::move(done);
        });

    ParamMap params;
    service_->startIST(params);
    assert_done(true);
    power_done(true);
    itm_done(true); // ITM succeeded

    EXPECT_EQ(service_->state().stage, IstStage::cleanup);

    deassert_done(true); // Deassert succeeded

    EXPECT_EQ(service_->state().status, IstStatus::completed);
    EXPECT_EQ(service_->state().stage, IstStage::idle);
}

TEST_F(IstServiceTest, ItmFailureResultsInFailedStatus)
{
    service_->initialize(configPath_);

    std::move_only_function<void(bool) const> assert_done;
    EXPECT_CALL(
        *hookRunner_,
        asyncRun(::testing::_, StrEq("istBootAssert hook"), ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done) {
            assert_done = std::move(done);
        });

    std::move_only_function<void(bool) const> power_done;
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([&](DoneCb done) { power_done = std::move(done); });

    std::move_only_function<void(bool) const> itm_done;
    EXPECT_CALL(*itmRunner_, asyncRun(::testing::_, ::testing::_, ::testing::_,
                                      ::testing::_))
        .WillOnce([&](const IstTestConfig&, const IstPlatformConfig&,
                      DoneCb done, ProgressCb) { itm_done = std::move(done); });

    std::move_only_function<void(bool) const> deassert_done;
    EXPECT_CALL(
        *hookRunner_,
        asyncRun(::testing::_, StrEq("istBootDeassert hook"), ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done) {
            deassert_done = std::move(done);
        });

    ParamMap params;
    service_->startIST(params);
    assert_done(true);
    power_done(true);
    itm_done(false); // ITM failed

    EXPECT_EQ(service_->state().stage, IstStage::cleanup);

    deassert_done(true);

    EXPECT_EQ(service_->state().status, IstStatus::failed);
    EXPECT_EQ(service_->state().stage, IstStage::idle);
}

TEST_F(IstServiceTest, CleanupDeassertFailureStaysInFailed)
{
    service_->initialize(configPath_);

    std::move_only_function<void(bool) const> assert_done;
    EXPECT_CALL(
        *hookRunner_,
        asyncRun(::testing::_, StrEq("istBootAssert hook"), ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done) {
            assert_done = std::move(done);
        });

    std::move_only_function<void(bool) const> power_done;
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([&](DoneCb done) { power_done = std::move(done); });

    std::move_only_function<void(bool) const> itm_done;
    EXPECT_CALL(*itmRunner_, asyncRun(::testing::_, ::testing::_, ::testing::_,
                                      ::testing::_))
        .WillOnce([&](const IstTestConfig&, const IstPlatformConfig&,
                      DoneCb done, ProgressCb) { itm_done = std::move(done); });

    std::move_only_function<void(bool) const> deassert_done;
    EXPECT_CALL(
        *hookRunner_,
        asyncRun(::testing::_, StrEq("istBootDeassert hook"), ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done) {
            deassert_done = std::move(done);
        });

    ParamMap params;
    service_->startIST(params);
    assert_done(true);
    power_done(true);
    itm_done(true);

    // Deassert hook fails
    deassert_done(false);

    EXPECT_EQ(service_->state().status, IstStatus::failed);
    EXPECT_EQ(service_->state().stage, IstStage::idle);
}

TEST_F(IstServiceTest, AutoRebootCallsResetHook)
{
    service_->initialize(configPath_);

    std::move_only_function<void(bool) const> assert_done;
    EXPECT_CALL(
        *hookRunner_,
        asyncRun(::testing::_, StrEq("istBootAssert hook"), ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done) {
            assert_done = std::move(done);
        });

    std::move_only_function<void(bool) const> power_done;
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([&](DoneCb done) { power_done = std::move(done); });

    std::move_only_function<void(bool) const> itm_done;
    EXPECT_CALL(*itmRunner_, asyncRun(::testing::_, ::testing::_, ::testing::_,
                                      ::testing::_))
        .WillOnce([&](const IstTestConfig&, const IstPlatformConfig&,
                      DoneCb done, ProgressCb) { itm_done = std::move(done); });

    std::move_only_function<void(bool) const> deassert_done;
    EXPECT_CALL(
        *hookRunner_,
        asyncRun(::testing::_, StrEq("istBootDeassert hook"), ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done) {
            deassert_done = std::move(done);
        });

    std::move_only_function<void(bool) const> reset_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("resetSystem hook"), ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done) {
            reset_done = std::move(done);
        });

    // Enable auto-reboot
    ParamMap params;
    params["autoRebootOnComplete"] = true;
    service_->startIST(params);
    assert_done(true);
    power_done(true);
    itm_done(true);
    deassert_done(true);

    // Reset hook should have been called
    ASSERT_TRUE(reset_done);

    reset_done(true);

    EXPECT_EQ(service_->state().status, IstStatus::completed);
    EXPECT_EQ(service_->state().stage, IstStage::idle);
}

TEST_F(IstServiceTest, StartIstPassesTestParams)
{
    service_->initialize(configPath_);

    IstTestConfig captured_cfg;
    EXPECT_CALL(
        *hookRunner_,
        asyncRun(::testing::_, StrEq("istBootAssert hook"), ::testing::_))
        .WillOnce([](const std::string&, const std::string&, DoneCb done) {
            done(true);
        });

    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([](DoneCb done) { done(true); });

    EXPECT_CALL(*itmRunner_, asyncRun(::testing::_, ::testing::_, ::testing::_,
                                      ::testing::_))
        .WillOnce([&](const IstTestConfig& cfg, const IstPlatformConfig&,
                      DoneCb done, ProgressCb) {
            captured_cfg = cfg;
            done(true);
        });

    // Deassert hook in cleanup
    EXPECT_CALL(
        *hookRunner_,
        asyncRun(::testing::_, StrEq("istBootDeassert hook"), ::testing::_))
        .WillOnce([](const std::string&, const std::string&, DoneCb done) {
            done(true);
        });

    ParamMap params;
    params["customTestList"] = std::string("test1,test2");
    params["customSocketList"] = std::string("0,1");
    params["istContinueOnFail"] = std::string("yes");
    params["istSaveResOnPass"] = std::string("enable");
    params["istSaveResOnFail"] = std::string("disable");
    params["istSwTimeoutSec"] = 600;
    params["autoRebootOnComplete"] = false;
    service_->startIST(params);

    ASSERT_TRUE(captured_cfg.customTestList.has_value());
    EXPECT_EQ(*captured_cfg.customTestList, "test1,test2");
    ASSERT_TRUE(captured_cfg.customSocketList.has_value());
    EXPECT_EQ(*captured_cfg.customSocketList, "0,1");
    ASSERT_TRUE(captured_cfg.continueOnFail.has_value());
    EXPECT_EQ(*captured_cfg.continueOnFail, "yes");
    ASSERT_TRUE(captured_cfg.saveResOnPass.has_value());
    EXPECT_EQ(*captured_cfg.saveResOnPass, "enable");
    ASSERT_TRUE(captured_cfg.saveResOnFail.has_value());
    EXPECT_EQ(*captured_cfg.saveResOnFail, "disable");
    ASSERT_TRUE(captured_cfg.swTimeoutSec.has_value());
    EXPECT_EQ(*captured_cfg.swTimeoutSec, 600);
    EXPECT_FALSE(captured_cfg.autoRebootOnComplete);
}

TEST_F(IstServiceTest, SwTimeoutClampedToRange)
{
    service_->initialize(configPath_);

    IstTestConfig captured_cfg;
    EXPECT_CALL(
        *hookRunner_,
        asyncRun(::testing::_, StrEq("istBootAssert hook"), ::testing::_))
        .WillOnce([](const std::string&, const std::string&, DoneCb done) {
            done(true);
        });
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([](DoneCb done) { done(true); });
    EXPECT_CALL(*itmRunner_, asyncRun(::testing::_, ::testing::_, ::testing::_,
                                      ::testing::_))
        .WillOnce([&](const IstTestConfig& cfg, const IstPlatformConfig&,
                      DoneCb done, ProgressCb) {
            captured_cfg = cfg;
            done(true);
        });
    EXPECT_CALL(
        *hookRunner_,
        asyncRun(::testing::_, StrEq("istBootDeassert hook"), ::testing::_))
        .WillOnce([](const std::string&, const std::string&, DoneCb done) {
            done(true);
        });

    // Pass a timeout below minimum (60s)
    ParamMap params;
    params["istSwTimeoutSec"] = 5;
    service_->startIST(params);

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
    EXPECT_FALSE(service_->initialize(configPath_));
}

TEST_F(IstServiceTest, InitializeRejectsEmptyHookPath)
{
    write_config(R"({
        "hookDirectory": ")" +
                 (tmpDir_ / "hooks").string() + R"(",
        "hookPaths": {
            "istBootAssert": ""
        },
        "storageConfig": {}
    })");
    EXPECT_FALSE(service_->initialize(configPath_));
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
            "resultStoragePath": ")" +
                 (tmpDir_ / "results").string() + R"("
        }
    })");
    service_->initialize(configPath_);

    ParamMap params;
    EXPECT_THROW(service_->startIST(params), sdbusplus::exception::SdBusError);
    EXPECT_EQ(service_->state().status, IstStatus::failed);
    EXPECT_EQ(service_->state().stage, IstStage::idle);
}

TEST_F(IstServiceTest, SwTimeoutClampedToMax)
{
    service_->initialize(configPath_);

    IstTestConfig captured_cfg;
    EXPECT_CALL(
        *hookRunner_,
        asyncRun(::testing::_, StrEq("istBootAssert hook"), ::testing::_))
        .WillOnce([](const std::string&, const std::string&, DoneCb done) {
            done(true);
        });
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([](DoneCb done) { done(true); });
    EXPECT_CALL(*itmRunner_, asyncRun(::testing::_, ::testing::_, ::testing::_,
                                      ::testing::_))
        .WillOnce([&](const IstTestConfig& cfg, const IstPlatformConfig&,
                      DoneCb done, ProgressCb) {
            captured_cfg = cfg;
            done(true);
        });
    EXPECT_CALL(
        *hookRunner_,
        asyncRun(::testing::_, StrEq("istBootDeassert hook"), ::testing::_))
        .WillOnce([](const std::string&, const std::string&, DoneCb done) {
            done(true);
        });

    ParamMap params;
    params["istSwTimeoutSec"] = 99999;
    service_->startIST(params);

    ASSERT_TRUE(captured_cfg.swTimeoutSec.has_value());
    EXPECT_EQ(*captured_cfg.swTimeoutSec, 7200); // clamped to max
}

TEST_F(IstServiceTest, CleanupFailsWhenDeassertHookMissing)
{
    // Config without istBootDeassert hook
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
            "resultStoragePath": ")" +
                 (tmpDir_ / "results").string() + R"("
        }
    })");
    service_->initialize(configPath_);

    std::move_only_function<void(bool) const> assert_done;
    EXPECT_CALL(
        *hookRunner_,
        asyncRun(::testing::_, StrEq("istBootAssert hook"), ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done) {
            assert_done = std::move(done);
        });

    std::move_only_function<void(bool) const> power_done;
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([&](DoneCb done) { power_done = std::move(done); });

    std::move_only_function<void(bool) const> itm_done;
    EXPECT_CALL(*itmRunner_, asyncRun(::testing::_, ::testing::_, ::testing::_,
                                      ::testing::_))
        .WillOnce([&](const IstTestConfig&, const IstPlatformConfig&,
                      DoneCb done, ProgressCb) { itm_done = std::move(done); });

    ParamMap params;
    service_->startIST(params);
    assert_done(true);
    power_done(true);
    itm_done(true);

    // Cleanup should fail because istBootDeassert is missing
    EXPECT_EQ(service_->state().status, IstStatus::failed);
    EXPECT_EQ(service_->state().stage, IstStage::idle);
}

TEST_F(IstServiceTest, AutoRebootResetFailureTransitionsToFailed)
{
    service_->initialize(configPath_);

    std::move_only_function<void(bool) const> assert_done;
    EXPECT_CALL(
        *hookRunner_,
        asyncRun(::testing::_, StrEq("istBootAssert hook"), ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done) {
            assert_done = std::move(done);
        });

    std::move_only_function<void(bool) const> power_done;
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([&](DoneCb done) { power_done = std::move(done); });

    std::move_only_function<void(bool) const> itm_done;
    EXPECT_CALL(*itmRunner_, asyncRun(::testing::_, ::testing::_, ::testing::_,
                                      ::testing::_))
        .WillOnce([&](const IstTestConfig&, const IstPlatformConfig&,
                      DoneCb done, ProgressCb) { itm_done = std::move(done); });

    std::move_only_function<void(bool) const> deassert_done;
    EXPECT_CALL(
        *hookRunner_,
        asyncRun(::testing::_, StrEq("istBootDeassert hook"), ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done) {
            deassert_done = std::move(done);
        });

    std::move_only_function<void(bool) const> reset_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("resetSystem hook"), ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done) {
            reset_done = std::move(done);
        });

    ParamMap params;
    params["autoRebootOnComplete"] = true;
    service_->startIST(params);
    assert_done(true);
    power_done(true);
    itm_done(true);
    deassert_done(true);

    // Reset hook fails
    reset_done(false);

    EXPECT_EQ(service_->state().status, IstStatus::failed);
    EXPECT_EQ(service_->state().stage, IstStage::idle);
}

TEST_F(IstServiceTest, AutoRebootWithItmFailureStillFailed)
{
    service_->initialize(configPath_);

    std::move_only_function<void(bool) const> assert_done;
    EXPECT_CALL(
        *hookRunner_,
        asyncRun(::testing::_, StrEq("istBootAssert hook"), ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done) {
            assert_done = std::move(done);
        });

    std::move_only_function<void(bool) const> power_done;
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([&](DoneCb done) { power_done = std::move(done); });

    std::move_only_function<void(bool) const> itm_done;
    EXPECT_CALL(*itmRunner_, asyncRun(::testing::_, ::testing::_, ::testing::_,
                                      ::testing::_))
        .WillOnce([&](const IstTestConfig&, const IstPlatformConfig&,
                      DoneCb done, ProgressCb) { itm_done = std::move(done); });

    std::move_only_function<void(bool) const> deassert_done;
    EXPECT_CALL(
        *hookRunner_,
        asyncRun(::testing::_, StrEq("istBootDeassert hook"), ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done) {
            deassert_done = std::move(done);
        });

    std::move_only_function<void(bool) const> reset_done;
    EXPECT_CALL(*hookRunner_,
                asyncRun(::testing::_, StrEq("resetSystem hook"), ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done) {
            reset_done = std::move(done);
        });

    ParamMap params;
    params["autoRebootOnComplete"] = true;
    service_->startIST(params);
    assert_done(true);
    power_done(true);
    itm_done(false); // ITM failed

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
            "resultStoragePath": ")" +
                 (tmpDir_ / "results").string() + R"("
        }
    })");
    service_->initialize(configPath_);

    std::move_only_function<void(bool) const> assert_done;
    EXPECT_CALL(
        *hookRunner_,
        asyncRun(::testing::_, StrEq("istBootAssert hook"), ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done) {
            assert_done = std::move(done);
        });

    std::move_only_function<void(bool) const> power_done;
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([&](DoneCb done) { power_done = std::move(done); });

    std::move_only_function<void(bool) const> itm_done;
    EXPECT_CALL(*itmRunner_, asyncRun(::testing::_, ::testing::_, ::testing::_,
                                      ::testing::_))
        .WillOnce([&](const IstTestConfig&, const IstPlatformConfig&,
                      DoneCb done, ProgressCb) { itm_done = std::move(done); });

    std::move_only_function<void(bool) const> deassert_done;
    EXPECT_CALL(
        *hookRunner_,
        asyncRun(::testing::_, StrEq("istBootDeassert hook"), ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done) {
            deassert_done = std::move(done);
        });

    ParamMap params;
    params["autoRebootOnComplete"] = true;
    service_->startIST(params);
    assert_done(true);
    power_done(true);
    itm_done(true);
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
        }
    })");
    service_->initialize(configPath_);

    ParamMap params;
    EXPECT_THROW(service_->startIST(params), sdbusplus::exception::SdBusError);
    EXPECT_EQ(service_->state().status, IstStatus::aborted);
}

TEST_F(IstServiceTest, SecondRunAfterCompletionWorks)
{
    service_->initialize(configPath_);

    // --- First run: full success ---
    std::move_only_function<void(bool) const> assert_done1;
    EXPECT_CALL(
        *hookRunner_,
        asyncRun(::testing::_, StrEq("istBootAssert hook"), ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done) {
            assert_done1 = std::move(done);
        });

    std::move_only_function<void(bool) const> power_done1;
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([&](DoneCb done) { power_done1 = std::move(done); });

    std::move_only_function<void(bool) const> itm_done1;
    EXPECT_CALL(*itmRunner_, asyncRun(::testing::_, ::testing::_, ::testing::_,
                                      ::testing::_))
        .WillOnce([&](const IstTestConfig&, const IstPlatformConfig&,
                      DoneCb done,
                      ProgressCb) { itm_done1 = std::move(done); });

    std::move_only_function<void(bool) const> deassert_done1;
    EXPECT_CALL(
        *hookRunner_,
        asyncRun(::testing::_, StrEq("istBootDeassert hook"), ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done) {
            deassert_done1 = std::move(done);
        });

    ParamMap params1;
    params1["customTestList"] = std::string("testA");
    service_->startIST(params1);
    assert_done1(true);
    power_done1(true);
    itm_done1(true);
    deassert_done1(true);

    EXPECT_EQ(service_->state().status, IstStatus::completed);
    EXPECT_EQ(service_->state().stage, IstStage::idle);

    // --- Second run: should start cleanly with fresh state ---
    std::move_only_function<void(bool) const> assert_done2;
    EXPECT_CALL(
        *hookRunner_,
        asyncRun(::testing::_, StrEq("istBootAssert hook"), ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done) {
            assert_done2 = std::move(done);
        });

    std::move_only_function<void(bool) const> power_done2;
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([&](DoneCb done) { power_done2 = std::move(done); });

    std::move_only_function<void(bool) const> itm_done2;
    EXPECT_CALL(*itmRunner_, asyncRun(::testing::_, ::testing::_, ::testing::_,
                                      ::testing::_))
        .WillOnce([&](const IstTestConfig& cfg, const IstPlatformConfig&,
                      DoneCb done, ProgressCb) {
            // Verify old params are NOT carried over
            EXPECT_FALSE(cfg.customTestList.has_value() &&
                         *cfg.customTestList == "testA");
            itm_done2 = std::move(done);
        });

    std::move_only_function<void(bool) const> deassert_done2;
    EXPECT_CALL(
        *hookRunner_,
        asyncRun(::testing::_, StrEq("istBootDeassert hook"), ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done) {
            deassert_done2 = std::move(done);
        });

    ParamMap params2; // no customTestList this time
    EXPECT_NO_THROW(service_->startIST(params2));
    EXPECT_EQ(service_->state().progress, 0);
    EXPECT_EQ(service_->state().status, IstStatus::inProgress);

    assert_done2(true);
    power_done2(true);
    itm_done2(true);
    deassert_done2(true);

    EXPECT_EQ(service_->state().status, IstStatus::completed);
    EXPECT_EQ(service_->state().stage, IstStage::idle);
}

TEST_F(IstServiceTest, ProgressInterfaceCreatedOnStartAndRemovedOnComplete)
{
    service_->initialize(configPath_);

    ::testing::InSequence seq;

    EXPECT_CALL(*publisher_, createProgress()).Times(1);

    EXPECT_CALL(
        *hookRunner_,
        asyncRun(::testing::_, StrEq("istBootAssert hook"), ::testing::_))
        .WillOnce([](const std::string&, const std::string&, DoneCb done) {
            done(true);
        });
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([](DoneCb done) { done(true); });
    EXPECT_CALL(*itmRunner_, asyncRun(::testing::_, ::testing::_, ::testing::_,
                                      ::testing::_))
        .WillOnce([](const IstTestConfig&, const IstPlatformConfig&,
                     DoneCb done, ProgressCb) { done(true); });
    EXPECT_CALL(
        *hookRunner_,
        asyncRun(::testing::_, StrEq("istBootDeassert hook"), ::testing::_))
        .WillOnce([](const std::string&, const std::string&, DoneCb done) {
            done(true);
        });

    EXPECT_CALL(*publisher_, removeProgress()).Times(1);

    ParamMap params;
    service_->startIST(params);

    EXPECT_EQ(service_->state().status, IstStatus::completed);
}

TEST_F(IstServiceTest, ProgressInterfaceRemovedOnAbort)
{
    fs::remove_all(tmpDir_ / "vectors");
    service_->initialize(configPath_);

    EXPECT_CALL(*publisher_, createProgress()).Times(1);
    EXPECT_CALL(*publisher_, removeProgress()).Times(1);

    ParamMap params;
    EXPECT_THROW(service_->startIST(params), sdbusplus::exception::SdBusError);
    EXPECT_EQ(service_->state().status, IstStatus::aborted);
}

TEST_F(IstServiceTest, ProgressInterfaceRemovedOnFailure)
{
    service_->initialize(configPath_);

    EXPECT_CALL(*publisher_, createProgress()).Times(1);

    DoneCb assert_done;
    EXPECT_CALL(
        *hookRunner_,
        asyncRun(::testing::_, StrEq("istBootAssert hook"), ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done) {
            assert_done = std::move(done);
        });

    EXPECT_CALL(*publisher_, removeProgress()).Times(1);

    ParamMap params;
    service_->startIST(params);
    assert_done(false);

    EXPECT_EQ(service_->state().status, IstStatus::failed);
}

TEST_F(IstServiceTest, ProgressCallbackUpdatesStateAndPublishes)
{
    service_->initialize(configPath_);

    DoneCb assert_done;
    EXPECT_CALL(
        *hookRunner_,
        asyncRun(::testing::_, StrEq("istBootAssert hook"), ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done) {
            assert_done = std::move(done);
        });

    DoneCb power_done;
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([&](DoneCb done) { power_done = std::move(done); });

    DoneCb itm_done;
    ProgressCb progress_cb;
    EXPECT_CALL(*itmRunner_, asyncRun(::testing::_, ::testing::_, ::testing::_,
                                      ::testing::_))
        .WillOnce([&](const IstTestConfig&, const IstPlatformConfig&,
                      DoneCb done, ProgressCb on_progress) {
            itm_done = std::move(done);
            progress_cb = std::move(on_progress);
        });

    ParamMap params;
    service_->startIST(params);
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
    service_->initialize(configPath_);

    ParamMap params;
    params["istSwTimeoutSec"] = std::string("not_a_number");
    EXPECT_THROW(service_->startIST(params), sdbusplus::exception::SdBusError);
    EXPECT_EQ(service_->state().status, IstStatus::aborted);
}

TEST_F(IstServiceTest, SecondRunAfterFailureWorks)
{
    service_->initialize(configPath_);

    // --- First run: fails at assert hook ---
    DoneCb assert_done1;
    EXPECT_CALL(
        *hookRunner_,
        asyncRun(::testing::_, StrEq("istBootAssert hook"), ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done) {
            assert_done1 = std::move(done);
        });

    ParamMap params1;
    service_->startIST(params1);
    assert_done1(false);

    EXPECT_EQ(service_->state().status, IstStatus::failed);
    EXPECT_EQ(service_->state().stage, IstStage::idle);

    // --- Second run: should be accepted and start cleanly ---
    DoneCb assert_done2;
    EXPECT_CALL(
        *hookRunner_,
        asyncRun(::testing::_, StrEq("istBootAssert hook"), ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done) {
            assert_done2 = std::move(done);
        });

    DoneCb power_done2;
    EXPECT_CALL(*powerMonitor_, asyncWaitForPowerCycle(::testing::_))
        .WillOnce([&](DoneCb done) { power_done2 = std::move(done); });

    DoneCb itm_done2;
    EXPECT_CALL(*itmRunner_, asyncRun(::testing::_, ::testing::_, ::testing::_,
                                      ::testing::_))
        .WillOnce([&](const IstTestConfig&, const IstPlatformConfig&,
                      DoneCb done,
                      ProgressCb) { itm_done2 = std::move(done); });

    DoneCb deassert_done2;
    EXPECT_CALL(
        *hookRunner_,
        asyncRun(::testing::_, StrEq("istBootDeassert hook"), ::testing::_))
        .WillOnce([&](const std::string&, const std::string&, DoneCb done) {
            deassert_done2 = std::move(done);
        });

    ParamMap params2;
    EXPECT_NO_THROW(service_->startIST(params2));
    EXPECT_EQ(service_->state().status, IstStatus::inProgress);
    EXPECT_EQ(service_->state().progress, 0);

    assert_done2(true);
    power_done2(true);
    itm_done2(true);
    deassert_done2(true);

    EXPECT_EQ(service_->state().status, IstStatus::completed);
    EXPECT_EQ(service_->state().stage, IstStage::idle);
}

TEST_F(IstServiceTest, StartIstBeforeInitializeThrows)
{
    ParamMap params;
    EXPECT_THROW(service_->startIST(params), sdbusplus::exception::SdBusError);
    EXPECT_EQ(service_->state().status, IstStatus::aborted);
    EXPECT_EQ(service_->state().stage, IstStage::idle);
}
