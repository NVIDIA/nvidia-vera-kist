#include <ist_app.hpp>
#include <sdbusplus/asio/connection.hpp>

#include <iostream>

int main(int, char**)
{
    boost::asio::io_context io;
    std::shared_ptr<sdbusplus::asio::connection> conn =
        std::make_shared<sdbusplus::asio::connection>(io);
    sdbusplus::asio::object_server server(conn);

    IstService service(makeDbusStatePublisher(server), makeHookRunner(io),
                       makeHostPowerMonitor(io, conn), makeItmRunner(io));

    if (!service.initialize("/etc/ist/platform_cfg.json"))
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
        [&service](const ParamMap& params) { service.startIST(params); });
    control_iface->initialize();

    // Software version interface
    std::shared_ptr<sdbusplus::asio::dbus_interface> sw_iface =
        server.add_interface("/xyz/openbmc_project/software/ist",
                             "xyz.openbmc_project.Software.Version");
    sw_iface->initialize();

    conn->request_name("com.nvidia.vera.ist");
    io.run();
}
