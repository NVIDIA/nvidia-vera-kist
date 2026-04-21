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
#include <boost/asio/post.hpp>
#include <ist_app.hpp>
#include <sdbusplus/asio/connection.hpp>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class DbusStatePublisher final : public StatePublisher
{
  public:
    DbusStatePublisher(sdbusplus::asio::object_server& server,
                       std::shared_ptr<sdbusplus::asio::connection> conn,
                       const std::string& sw_path,
                       const std::string& ist_path) :
        server_(server), conn_(std::move(conn)), swPath_(sw_path)
    {
        globalStateIface_ =
            server_.add_interface(ist_path, "com.nvidia.vera.ist.State");
        globalStateIface_->register_property("IstInProgress", false);
        globalStateIface_->initialize();

        swVersionIface_ = server_.add_interface(
            sw_path, "xyz.openbmc_project.Software.Version");
        swVersionIface_->register_property("Version", std::string("Unknown"));
        swVersionIface_->initialize();

        activationIface_ = server_.add_interface(
            sw_path, "xyz.openbmc_project.Software.Activation");
        activationIface_->register_property("Activation",
                                            std::string(k_activation_active));
        activationIface_->register_property("Functional", true);
        activationIface_->initialize();
    }

    void createRunObject(const std::string& run_path,
                         ResultsFdCb results_fd_cb) override
    {
        removeRunObject();

        runStateIface_ =
            server_.add_interface(run_path, "com.nvidia.vera.ist.run.State");
        runStateIface_->register_property("Stage",
                                          istStageToString(IstStage::idle));
        runStateIface_->initialize();

        runProgIface_ = server_.add_interface(
            run_path, "xyz.openbmc_project.Common.Progress");
        runProgIface_->register_property(
            "Status", status_to_dbus_string(IstStatus::inProgress));
        runProgIface_->register_property("Progress", uint8_t{0});
        runProgIface_->register_property("StartTime", epoch_now());
        runProgIface_->register_property("CompletedTime", uint64_t{0});
        runProgIface_->initialize();

        if (results_fd_cb)
        {
            runResultsIface_ = server_.add_interface(
                run_path, "com.nvidia.vera.ist.run.Results");
            runResultsIface_->register_method("GetResultsFd",
                                              std::move(results_fd_cb));
            runResultsIface_->initialize();
        }
    }

    void removeRunObject() override
    {
        if (runResultsIface_)
        {
            defer_remove_interface(std::move(runResultsIface_));
        }
        if (runProgIface_)
        {
            defer_remove_interface(std::move(runProgIface_));
        }
        if (runStateIface_)
        {
            defer_remove_interface(std::move(runStateIface_));
        }
    }

    void publish(const IstState& state) override
    {
        if (globalStateIface_)
        {
            globalStateIface_->set_property("IstInProgress",
                                            state.stage != IstStage::idle);
        }
        if (runStateIface_)
        {
            runStateIface_->set_property("Stage",
                                         istStageToString(state.stage));
        }
        if (runProgIface_)
        {
            runProgIface_->set_property("Status",
                                        status_to_dbus_string(state.status));
            runProgIface_->set_property("Progress", state.progress);
            if (state.status != IstStatus::inProgress)
            {
                runProgIface_->set_property("CompletedTime", epoch_now());
            }
        }
    }

    void reSignalStage() override
    {
        if (runStateIface_)
        {
            runStateIface_->signal_property("Stage");
        }
    }

    void publishProgress(uint8_t progress) override
    {
        if (runProgIface_)
        {
            runProgIface_->set_property("Progress", progress);
        }
    }

    void publishVersion(const std::string& version) override
    {
        if (swVersionIface_)
        {
            swVersionIface_->set_property("Version", version);
        }
        else
        {
            std::cerr << "publishVersion: swVersionIface_ is null, version="
                      << version << '\n';
        }
    }

    void publishActivation(std::string_view state) override
    {
        if (activationIface_)
        {
            activationIface_->set_property("Activation", std::string(state));
            activationIface_->set_property("Functional",
                                           state == k_activation_active);
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
            defer_remove_interface(std::move(activationProgressIface_));
        }
    }

    void emitEventLog(
        const std::string& message, const std::string& severity,
        const std::map<std::string, std::string>& additional_data) override
    {
        conn_->async_method_call(
            [](const boost::system::error_code& ec) {
                if (ec)
                {
                    std::cerr << "Failed to create event log: " << ec.message()
                              << '\n';
                }
            },
            "xyz.openbmc_project.Logging", "/xyz/openbmc_project/logging",
            "xyz.openbmc_project.Logging.Create", "Create", message, severity,
            additional_data);
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

    void defer_remove_interface(
        std::shared_ptr<sdbusplus::asio::dbus_interface> iface)
    {
        server_.remove_interface(iface);
        boost::asio::post(conn_->get_io_context(),
                          [iface = std::move(iface)]() {});
    }

    sdbusplus::asio::object_server& server_;
    std::shared_ptr<sdbusplus::asio::connection> conn_;
    std::string swPath_;
    std::shared_ptr<sdbusplus::asio::dbus_interface> globalStateIface_;
    std::shared_ptr<sdbusplus::asio::dbus_interface> runStateIface_;
    std::shared_ptr<sdbusplus::asio::dbus_interface> runProgIface_;
    std::shared_ptr<sdbusplus::asio::dbus_interface> runResultsIface_;
    std::shared_ptr<sdbusplus::asio::dbus_interface> swVersionIface_;
    std::shared_ptr<sdbusplus::asio::dbus_interface> activationIface_;
    std::shared_ptr<sdbusplus::asio::dbus_interface> activationProgressIface_;
};

std::unique_ptr<StatePublisher>
    makeDbusStatePublisher(sdbusplus::asio::object_server& server,
                           std::shared_ptr<sdbusplus::asio::connection> conn,
                           const std::string& sw_path,
                           const std::string& ist_path)
{
    return std::make_unique<DbusStatePublisher>(server, std::move(conn),
                                                sw_path, ist_path);
}
