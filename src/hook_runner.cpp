#include <boost/process/v2/process.hpp>
#include <boost/process/v2/stdio.hpp>
#include <ist_app.hpp>

#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace bpv2 = boost::process::v2;

static void
    handle_hook_exit(const std::string& what,
                     const std::move_only_function<void(bool) const>& done,
                     const boost::system::error_code& ec, int exit_code)
{
    if (ec || exit_code != 0)
    {
        std::cerr << "Failed to execute " << what << ": ec=" << ec.message()
                  << " exit=" << exit_code << '\n';
        done(false);
        return;
    }
    done(true);
}

class HookRunnerImpl final : public HookRunner
{
  public:
    explicit HookRunnerImpl(boost::asio::io_context& io) : io_(io)
    {}
    void asyncRun(const std::string& cmd, std::string what,
                  std::move_only_function<void(bool) const> done) override;

  private:
    boost::asio::io_context& io_;
};

void HookRunnerImpl::asyncRun(const std::string& cmd, std::string what,
                              std::move_only_function<void(bool) const> done)
{
    std::shared_ptr<bpv2::process> proc;
    try
    {
        proc = std::make_shared<bpv2::process>(
            io_, cmd, std::vector<std::string>{},
            bpv2::process_stdio{.in = nullptr, .out = nullptr, .err = nullptr});
    }
    catch (const std::exception& e)
    {
        std::cerr << "Failed to execute " << what << ": " << e.what() << '\n';
        done(false);
        return;
    }

    proc->async_wait([proc, what = std::move(what), done = std::move(done)](
                         const boost::system::error_code& ec, int exit_code) {
        handle_hook_exit(what, done, ec, exit_code);
    });
}

std::unique_ptr<HookRunner> makeHookRunner(boost::asio::io_context& io)
{
    return std::make_unique<HookRunnerImpl>(io);
}
