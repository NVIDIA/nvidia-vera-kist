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
#include <unistd.h>

#include <ist_app.hpp>
#include <ist_errors.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/message/types.hpp>

#include <cerrno>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace ist_err = sdbusplus::error::com::nvidia::vera::ist;

static constexpr const char* config_path = "/etc/ist/platform_cfg.json";
static constexpr const char* sw_path_prefix =
    "/xyz/openbmc_project/inventory_software/";

// The D-Bus layer dup()s the FD into the reply message, so we own a
// now-redundant copy.  Post the close so it runs after the reply is
// fully built (i.e., after the handler returns).
static sdbusplus::message::unix_fd
    return_and_post_close(sdbusplus::message::unix_fd fd,
                          boost::asio::io_context& io)
{
    int raw_fd = static_cast<int>(fd);
    boost::asio::post(io, [raw_fd]() {
        if (::close(raw_fd) < 0)
        {
            std::cerr << "IST: close fd " << raw_fd << " failed: " << errno
                      << '\n';
        }
    });
    return fd;
}

static sdbusplus::object_path start_ist_from_dbus(
    const std::shared_ptr<IstService>& service, int32_t sw_timeout_sec,
    const std::string& continue_on_fail, bool auto_reboot_on_complete,
    const std::string& test_list, const std::string& socket_list,
    const std::string& save_result_on_pass,
    const std::string& save_result_on_fail)
{
    ParamMap params;
    if (sw_timeout_sec > 0)
    {
        params["istSwTimeoutSec"] = static_cast<int>(sw_timeout_sec);
    }
    if (continue_on_fail != "Default" && !continue_on_fail.empty())
    {
        params["istContinueOnFail"] = (continue_on_fail == "Enable");
    }
    params["autoRebootOnComplete"] = auto_reboot_on_complete;
    if (!test_list.empty())
    {
        params["customTestList"] = test_list;
    }
    if (!socket_list.empty())
    {
        params["customSocketList"] = socket_list;
    }
    if (save_result_on_pass != "Default" && !save_result_on_pass.empty())
    {
        params["istSaveResOnPass"] = (save_result_on_pass == "Enable");
    }
    if (save_result_on_fail != "Default" && !save_result_on_fail.empty())
    {
        params["istSaveResOnFail"] = (save_result_on_fail == "Enable");
    }
    return {service->startIST(params)};
}

int main(int, char**)
{
    boost::asio::io_context io;
    std::shared_ptr<sdbusplus::asio::connection> conn =
        std::make_shared<sdbusplus::asio::connection>(io);
    sdbusplus::asio::object_server server(conn);

    IstPlatformConfig platform_cfg;
    if (!parsePlatformConfig(platform_cfg, config_path))
    {
        std::cerr << "IST: failed to parse platform config, exiting\n";
        return 1;
    }

    std::string sw_path = sw_path_prefix + platform_cfg.softwareInventoryId;

    static constexpr const char* ist_path = "/com/nvidia/vera/ist";

    std::shared_ptr<IstService> service = IstService::create(
        io, makeDbusStatePublisher(server, conn, sw_path, ist_path),
        makeHookRunner(io), makeHostPowerMonitor(io, conn), makeItmRunner(io));

    if (!service->initialize(std::move(platform_cfg)))
    {
        std::cerr << "IST: failed to initialize, exiting\n";
        return 1;
    }

    service->setResultsFdCallback(
        [weak = std::weak_ptr<IstService>(service), &io]() {
            auto svc = weak.lock();
            if (!svc)
            {
                throw ist_err::InternalFailure{};
            }
            return return_and_post_close(svc->getResultsFd(), io);
        });

    service->setCpuDiscoverer(
        [conn](
            std::move_only_function<void(const std::vector<std::string>&) const>
                done) {
            conn->async_method_call(
                [done =
                     std::move(done)](const boost::system::error_code& ec,
                                      const std::vector<std::string>& paths) {
                    if (ec)
                    {
                        std::cerr
                            << "ObjectMapper CPU query failed: " << ec.message()
                            << '\n';
                        done({});
                        return;
                    }
                    done(paths);
                },
                "xyz.openbmc_project.ObjectMapper",
                "/xyz/openbmc_project/object_mapper",
                "xyz.openbmc_project.ObjectMapper", "GetSubTreePaths",
                "/xyz/openbmc_project/inventory", 0,
                std::array<const char*, 1>{
                    "xyz.openbmc_project.Inventory.Item.Cpu"});
        });

    // D-Bus control interface
    std::shared_ptr<sdbusplus::asio::dbus_interface> control_iface =
        server.add_interface(ist_path, "xyz.openbmc_project.ist.Control");
    control_iface->register_method(
        "StartIST",
        [service](int32_t sw_timeout_sec, const std::string& continue_on_fail,
                  bool auto_reboot_on_complete, const std::string& test_list,
                  const std::string& socket_list,
                  const std::string& save_result_on_pass,
                  const std::string& save_result_on_fail) {
            return start_ist_from_dbus(
                service, sw_timeout_sec, continue_on_fail,
                auto_reboot_on_complete, test_list, socket_list,
                save_result_on_pass, save_result_on_fail);
        });
    control_iface->initialize();

    // Software update interface
    std::shared_ptr<sdbusplus::asio::dbus_interface> update_iface =
        server.add_interface(sw_path, "xyz.openbmc_project.Software.Update");
    update_iface->register_property(
        "AllowedApplyTimes",
        std::set<std::string>{std::string(k_apply_time_immediate),
                              std::string(k_apply_time_on_reset)});
    update_iface->register_method(
        "StartUpdate", [service](sdbusplus::message::unix_fd image,
                                 const std::string& apply_time) {
            // The received fd is owned by the inbound D-Bus message and will be
            // closed once this handler returns.  The async upload outlives the
            // handler, so take our own close-on-exec copy to keep the read end
            // of the pipe alive independently of the message lifetime.
            UniqueFd owned(
                ::fcntl(static_cast<int>(image), F_DUPFD_CLOEXEC, 0));
            if (owned.get() < 0)
            {
                throw ist_err::InternalFailure{};
            }
            return sdbusplus::object_path(
                service->startUpdate(owned.release(), apply_time));
        });
    update_iface->initialize();

    conn->request_name("com.nvidia.vera.ist");
    io.run();
}
