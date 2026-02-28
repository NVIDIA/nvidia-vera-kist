#include <boost/asio/steady_timer.hpp>
#include <ist_app.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/bus/match.hpp>

#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <variant>

class HostPowerMonitorImpl final :
    public HostPowerMonitor,
    public std::enable_shared_from_this<HostPowerMonitorImpl>
{
  public:
    HostPowerMonitorImpl(boost::asio::io_context& io,
                         std::shared_ptr<sdbusplus::asio::connection> conn) :
        conn_(std::move(conn)), timer_(io)
    {}
    void asyncWaitForPowerCycle(
        std::move_only_function<void(bool) const> done) override;

  private:
    void read_current_state();
    void on_properties_changed(sdbusplus::message_t& msg);
    void on_state_changed(const std::string& state);
    void finish(bool ok);

    std::shared_ptr<sdbusplus::asio::connection> conn_;
    boost::asio::steady_timer timer_;
    std::unique_ptr<sdbusplus::bus::match_t> match_;
    std::move_only_function<void(bool) const> done_;
    bool sawOff_{false};
};

void HostPowerMonitorImpl::asyncWaitForPowerCycle(
    std::move_only_function<void(bool) const> done)
{
    done_ = std::move(done);
    sawOff_ = false;

    match_ = std::make_unique<sdbusplus::bus::match_t>(
        static_cast<sdbusplus::bus_t&>(*conn_),
        sdbusplus::bus::match::rules::propertiesChanged(
            "/xyz/openbmc_project/state/host0",
            "xyz.openbmc_project.State.Host"),
        [weak = weak_from_this()](sdbusplus::message_t& msg) {
            auto self = weak.lock();
            if (!self)
            {
                return;
            }
            self->on_properties_changed(msg);
        });

    // Read the current host state in case it is already off
    read_current_state();

    timer_.expires_after(std::chrono::minutes(10));
    timer_.async_wait(
        [weak = weak_from_this()](const boost::system::error_code& ec) {
            if (ec)
            {
                return;
            }
            auto self = weak.lock();
            if (!self)
            {
                return;
            }
            std::cerr << "Failed to receive power cycle in 10 minutes\n";
            self->finish(false);
        });
}

void HostPowerMonitorImpl::read_current_state()
{
    conn_->async_method_call(
        [weak = weak_from_this()](const boost::system::error_code& ec,
                                  const std::variant<std::string>& value) {
            auto self = weak.lock();
            if (!self)
            {
                return;
            }
            if (ec)
            {
                std::cerr << "Failed to read CurrentHostState: " << ec.message()
                          << '\n';
                return;
            }
            const std::string* state = std::get_if<std::string>(&value);
            if (state)
            {
                self->on_state_changed(*state);
            }
        },
        "xyz.openbmc_project.State.Host", "/xyz/openbmc_project/state/host0",
        "org.freedesktop.DBus.Properties", "Get",
        "xyz.openbmc_project.State.Host", "CurrentHostState");
}

void HostPowerMonitorImpl::on_properties_changed(sdbusplus::message_t& msg)
{
    std::string iface;
    std::unordered_map<std::string, std::variant<std::string>> props;
    try
    {
        msg.read(iface, props);
    }
    catch (const std::exception& e)
    {
        std::cerr << "Failed to read PropertiesChanged signal: " << e.what()
                  << '\n';
        return;
    }
    auto it = props.find("CurrentHostState");
    if (it != props.end())
    {
        on_state_changed(std::get<std::string>(it->second));
    }
}

void HostPowerMonitorImpl::on_state_changed(const std::string& state)
{
    if (!sawOff_ && state == "xyz.openbmc_project.State.Host.HostState.Off")
    {
        sawOff_ = true;
    }
    else if (sawOff_ && state != "xyz.openbmc_project.State.Host.HostState.Off")
    {
        std::cerr << "Detected power cycle (host state: " << state << ")\n";
        finish(true);
    }
}

void HostPowerMonitorImpl::finish(bool ok)
{
    if (!done_)
    {
        return; // Already finished; guard against duplicate invocation
    }
    match_.reset();
    timer_.cancel();
    auto cb = std::move(done_);
    done_ = nullptr;
    cb(ok);
}

std::shared_ptr<HostPowerMonitor>
    makeHostPowerMonitor(boost::asio::io_context& io,
                         std::shared_ptr<sdbusplus::asio::connection> conn)
{
    return std::make_shared<HostPowerMonitorImpl>(io, std::move(conn));
}
