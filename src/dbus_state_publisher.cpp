#include <ist_app.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

class DbusStatePublisher final : public StatePublisher
{
  public:
    DbusStatePublisher(sdbusplus::asio::object_server& server,
                       const std::string& sw_path) :
        server_(server), swPath_(sw_path)
    {
        stateIface_ = server_.add_interface("/com/nvidia/vera/ist",
                                            "com.nvidia.vera.ist.State");
        stateIface_->register_property("Stage",
                                       istStageToString(IstStage::idle));
        stateIface_->initialize();

        swVersionIface_ = server_.add_interface(
            sw_path, "xyz.openbmc_project.Software.Version");
        swVersionIface_->register_property("Version", std::string("Unknown"));
        swVersionIface_->initialize();

        activationIface_ = server_.add_interface(
            sw_path, "xyz.openbmc_project.Software.Activation");
        activationIface_->register_property("Activation",
                                            std::string(k_activation_active));
        activationIface_->initialize();
    }

    void publish(const IstState& state) override
    {
        stateIface_->set_property("Stage", istStageToString(state.stage));
        if (progIface_)
        {
            progIface_->set_property("Status",
                                     status_to_dbus_string(state.status));
            progIface_->set_property("Progress", state.progress);
            if (state.status != IstStatus::inProgress)
            {
                progIface_->set_property("CompletedTime", epoch_now());
            }
        }
    }

    void publishProgress(uint8_t progress) override
    {
        if (progIface_)
        {
            progIface_->set_property("Progress", progress);
        }
    }

    void createProgress() override
    {
        if (progIface_)
        {
            server_.remove_interface(progIface_);
        }
        progIface_ = server_.add_interface(
            "/com/nvidia/vera/ist", "xyz.openbmc_project.Common.Progress");
        progIface_->register_property(
            "Status", status_to_dbus_string(IstStatus::inProgress));
        progIface_->register_property("Progress", uint8_t{0});
        progIface_->register_property("StartTime", epoch_now());
        progIface_->register_property("CompletedTime", uint64_t{0});
        progIface_->initialize();
    }

    void removeProgress() override
    {
        if (progIface_)
        {
            server_.remove_interface(progIface_);
            progIface_.reset();
        }
    }

    void publishActivation(std::string_view state) override
    {
        if (activationIface_)
        {
            activationIface_->set_property("Activation", std::string(state));
        }
    }

    void createActivationProgress() override
    {
        if (activationProgressIface_)
        {
            server_.remove_interface(activationProgressIface_);
        }
        activationProgressIface_ = server_.add_interface(
            swPath_, "xyz.openbmc_project.Software.ActivationProgress");
        activationProgressIface_->register_property("Progress", uint8_t{0});
        activationProgressIface_->initialize();
    }

    void publishActivationProgress(uint8_t progress) override
    {
        if (activationProgressIface_)
        {
            activationProgressIface_->set_property("Progress", progress);
        }
    }

    void removeActivationProgress() override
    {
        if (activationProgressIface_)
        {
            server_.remove_interface(activationProgressIface_);
            activationProgressIface_.reset();
        }
    }

  private:
    static uint64_t epoch_now()
    {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
    }

    static std::string status_to_dbus_string(IstStatus s)
    {
        static constexpr std::string_view prefix =
            "xyz.openbmc_project.Common.Progress.OperationStatus.";
        switch (s)
        {
            case IstStatus::inProgress:
                return std::string(prefix) + "InProgress";
            case IstStatus::completed:
                return std::string(prefix) + "Completed";
            case IstStatus::failed:
                return std::string(prefix) + "Failed";
            case IstStatus::aborted:
                return std::string(prefix) + "Aborted";
        }
        return std::string(prefix) + "InProgress";
    }

    static constexpr std::string_view k_activation_active =
        "xyz.openbmc_project.Software.Activation.Activations.Active";

    sdbusplus::asio::object_server& server_;
    std::string swPath_;
    std::shared_ptr<sdbusplus::asio::dbus_interface> progIface_;
    std::shared_ptr<sdbusplus::asio::dbus_interface> stateIface_;
    std::shared_ptr<sdbusplus::asio::dbus_interface> swVersionIface_;
    std::shared_ptr<sdbusplus::asio::dbus_interface> activationIface_;
    std::shared_ptr<sdbusplus::asio::dbus_interface> activationProgressIface_;
};

std::unique_ptr<StatePublisher>
    makeDbusStatePublisher(sdbusplus::asio::object_server& server,
                           const std::string& sw_path)
{
    return std::make_unique<DbusStatePublisher>(server, sw_path);
}
