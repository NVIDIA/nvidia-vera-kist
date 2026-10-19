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
#include <shm_telemetry.hpp>

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
        server_(server), conn_(std::move(conn)), sw_path_(sw_path)
    {
        init_shm_telemetry();

        global_state_iface_ =
            server_.add_interface(ist_path, "com.nvidia.vera.ist.State");
        global_state_iface_->register_property("IstInProgress", false);
        global_state_iface_->initialize();

        sw_version_iface_ = server_.add_interface(
            sw_path, "xyz.openbmc_project.Software.Version");
        sw_version_iface_->register_property("Version", std::string("Unknown"));
        sw_version_iface_->initialize();

        activation_iface_ = server_.add_interface(
            sw_path, "xyz.openbmc_project.Software.Activation");
        activation_iface_->register_property("Activation",
                                             std::string(k_activation_active));
        activation_iface_->initialize();

        op_status_iface_ = server_.add_interface(sw_path, k_op_status_iface);
        op_status_iface_->register_property("Functional", true);
        op_status_iface_->initialize();
    }

    void createRunObject(const std::string& run_path,
                         ResultsFdCb results_fd_cb) override
    {
        removeRunObject();

        run_state_iface_ =
            server_.add_interface(run_path, "com.nvidia.vera.ist.run.State");
        run_state_iface_->register_property("Stage",
                                            istStageToString(IstStage::idle));
        run_state_iface_->initialize();

        run_prog_iface_ = server_.add_interface(
            run_path, "xyz.openbmc_project.Common.Progress");
        run_prog_iface_->register_property(
            "Status", status_to_dbus_string(IstStatus::inProgress));
        run_prog_iface_->register_property("Progress", uint8_t{0});
        run_prog_iface_->register_property("StartTime", epoch_now());
        run_prog_iface_->register_property("CompletedTime", uint64_t{0});
        run_prog_iface_->initialize();

        if (results_fd_cb)
        {
            run_results_iface_ = server_.add_interface(
                run_path, "com.nvidia.vera.ist.run.Results");
            run_results_iface_->register_method("GetResultsFd",
                                                std::move(results_fd_cb));
            run_results_iface_->initialize();
        }
    }

    void removeRunObject() override
    {
        if (run_results_iface_)
        {
            defer_remove_interface(std::move(run_results_iface_));
        }
        if (run_prog_iface_)
        {
            defer_remove_interface(std::move(run_prog_iface_));
        }
        if (run_state_iface_)
        {
            defer_remove_interface(std::move(run_state_iface_));
        }
    }

    void publish(const IstState& state) override
    {
        if (global_state_iface_)
        {
            global_state_iface_->set_property("IstInProgress",
                                              state.stage != IstStage::idle);
        }
        if (run_state_iface_)
        {
            run_state_iface_->set_property("Stage",
                                           istStageToString(state.stage));
        }
        if (run_prog_iface_)
        {
            run_prog_iface_->set_property("Status",
                                          status_to_dbus_string(state.status));
            run_prog_iface_->set_property("Progress", state.progress);
            if (state.status != IstStatus::inProgress)
            {
                run_prog_iface_->set_property("CompletedTime", epoch_now());
            }
        }
    }

    void reSignalStage() override
    {
        if (run_state_iface_)
        {
            run_state_iface_->signal_property("Stage");
        }
    }

    void publishProgress(uint8_t progress) override
    {
        if (run_prog_iface_)
        {
            run_prog_iface_->set_property("Progress", progress);
        }
    }

    void publishVersion(const std::string& version) override
    {
        if (sw_version_iface_)
        {
            sw_version_iface_->set_property("Version", version);
        }
        else
        {
            std::cerr << "publishVersion: sw_version_iface_ is null, version="
                      << version << '\n';
        }
    }

    void publishActivation(std::string_view state) override
    {
        const bool functional = (state == k_activation_active);
        if (activation_iface_)
        {
            activation_iface_->set_property("Activation", std::string(state));
        }
        if (op_status_iface_)
        {
            op_status_iface_->set_property("Functional", functional);
        }
        publish_functional_on_shm(sw_path_, functional);
    }

    void createActivationProgress() override
    {
        if (activation_progress_iface_)
        {
            server_.remove_interface(activation_progress_iface_);
        }
        activation_progress_iface_ = server_.add_interface(
            sw_path_, "xyz.openbmc_project.Software.ActivationProgress");
        activation_progress_iface_->register_property("Progress", uint8_t{0});
        activation_progress_iface_->initialize();
    }

    void publishActivationProgress(uint8_t progress) override
    {
        if (activation_progress_iface_)
        {
            activation_progress_iface_->set_property("Progress", progress);
        }
    }

    void removeActivationProgress() override
    {
        if (activation_progress_iface_)
        {
            defer_remove_interface(std::move(activation_progress_iface_));
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
    std::string sw_path_;
    std::shared_ptr<sdbusplus::asio::dbus_interface> global_state_iface_;
    std::shared_ptr<sdbusplus::asio::dbus_interface> run_state_iface_;
    std::shared_ptr<sdbusplus::asio::dbus_interface> run_prog_iface_;
    std::shared_ptr<sdbusplus::asio::dbus_interface> run_results_iface_;
    std::shared_ptr<sdbusplus::asio::dbus_interface> sw_version_iface_;
    std::shared_ptr<sdbusplus::asio::dbus_interface> activation_iface_;
    std::shared_ptr<sdbusplus::asio::dbus_interface> op_status_iface_;
    std::shared_ptr<sdbusplus::asio::dbus_interface> activation_progress_iface_;
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
