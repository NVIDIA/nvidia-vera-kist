#include <ist_app.hpp>
#include <sdbusplus/asio/connection.hpp>

#include <iostream>

static void start_ist_from_dbus(const std::shared_ptr<IstService>& service,
                                int32_t sw_timeout_sec, bool continue_on_fail,
                                bool auto_reboot_on_complete,
                                const std::string& test_list,
                                const std::string& socket_list,
                                const std::string& save_result_on_pass,
                                const std::string& save_result_on_fail)
{
    ParamMap params;
    if (sw_timeout_sec > 0)
    {
        params["istSwTimeoutSec"] = static_cast<int>(sw_timeout_sec);
    }
    params["istContinueOnFail"] = continue_on_fail;
    params["autoRebootOnComplete"] = auto_reboot_on_complete;
    if (!test_list.empty())
    {
        params["customTestList"] = test_list;
    }
    if (!socket_list.empty())
    {
        params["customSocketList"] = socket_list;
    }
    if (!save_result_on_pass.empty())
    {
        params["istSaveResOnPass"] = (save_result_on_pass == "Enable");
    }
    if (!save_result_on_fail.empty())
    {
        params["istSaveResOnFail"] = (save_result_on_fail == "Enable");
    }
    service->startIST(params);
}

int main(int, char**)
{
    boost::asio::io_context io;
    std::shared_ptr<sdbusplus::asio::connection> conn =
        std::make_shared<sdbusplus::asio::connection>(io);
    sdbusplus::asio::object_server server(conn);

    auto service =
        IstService::create(makeDbusStatePublisher(server), makeHookRunner(io),
                           makeHostPowerMonitor(io, conn), makeItmRunner(io));

    if (!service->initialize("/etc/ist/platform_cfg.json"))
    {
        std::cerr << "IST: failed to initialize, exiting\n";
        return 1;
    }

    // D-Bus control interface
    std::shared_ptr<sdbusplus::asio::dbus_interface> control_iface =
        server.add_interface("/com/nvidia/vera/ist",
                             "xyz.openbmc_project.ist.Control");
    control_iface->register_method(
        "StartIST",
        [service](int32_t sw_timeout_sec, bool continue_on_fail,
                  bool auto_reboot_on_complete, const std::string& test_list,
                  const std::string& socket_list,
                  const std::string& save_result_on_pass,
                  const std::string& save_result_on_fail) {
            start_ist_from_dbus(service, sw_timeout_sec, continue_on_fail,
                                auto_reboot_on_complete, test_list, socket_list,
                                save_result_on_pass, save_result_on_fail);
        });
    control_iface->initialize();

    conn->request_name("com.nvidia.vera.ist");
    io.run();
}
