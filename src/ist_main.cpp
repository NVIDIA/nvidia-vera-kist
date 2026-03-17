#include <unistd.h>

#include <ist_app.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/message/types.hpp>

#include <iostream>

static constexpr const char* config_path = "/etc/ist/platform_cfg.json";
static constexpr const char* sw_path_prefix =
    "/xyz/openbmc_project/inventory_software/";

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

    IstPlatformConfig platform_cfg;
    if (!parsePlatformConfig(platform_cfg, config_path))
    {
        std::cerr << "IST: failed to parse platform config, exiting\n";
        return 1;
    }

    std::string sw_path = sw_path_prefix + platform_cfg.softwareInventoryId;

    std::shared_ptr<IstService> service = IstService::create(
        io, makeDbusStatePublisher(server, sw_path), makeHookRunner(io),
        makeHostPowerMonitor(io, conn), makeItmRunner(io));

    if (!service->initialize(std::move(platform_cfg)))
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

    // Software update interface
    std::shared_ptr<sdbusplus::asio::dbus_interface> update_iface =
        server.add_interface(sw_path, "xyz.openbmc_project.Software.Update");
    update_iface->register_method("StartUpdate", [service, &io]() {
        sdbusplus::message::unix_fd fd = service->startUpdate();
        int raw_fd = static_cast<int>(fd);
        // The D-Bus layer dup()s this FD into the reply message, so we own
        // a now-redundant copy. Post the close so it runs after the reply
        // is fully built (i.e., after this handler returns).
        boost::asio::post(io, [raw_fd]() { ::close(raw_fd); });
        return fd;
    });
    update_iface->initialize();

    conn->request_name("com.nvidia.vera.ist");
    io.run();
}
