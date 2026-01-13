#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <template_app.hpp>

int main(int, char**)
{
    boost::asio::io_context io;
    auto conn = std::make_shared<sdbusplus::asio::connection>(io);

    auto server = sdbusplus::asio::object_server(conn);

    std::shared_ptr<sdbusplus::asio::dbus_interface> iface =
        server.add_interface("/xyz/openbmc_project/template_app",
                             "xyz.openbmc_project.template");

    iface->register_property("IntegerValue", 23, changeIntegerValue);

    iface->register_method("UnlockDoor", unlockDoor);
    conn->request_name("xyz.openbmc_project.TemplateApp");

    io.run();
}
