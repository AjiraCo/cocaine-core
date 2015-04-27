#pragma once

#include <functional>
#include <string>
#include <system_error>

#include <boost/circular_buffer.hpp>
#include <boost/variant/variant.hpp>

#include <asio/io_service.hpp>
#include <asio/deadline_timer.hpp>
#include <asio/posix/stream_descriptor.hpp>

#include "cocaine/api/isolate.hpp"
#include "cocaine/logging.hpp"
#include "cocaine/idl/rpc.hpp"
#include "cocaine/idl/node.hpp"

#include "cocaine/detail/unique_id.hpp"
#include "cocaine/detail/service/node/manifest.hpp"
#include "cocaine/detail/service/node/profile.hpp"

#include "cocaine/detail/service/node.v2/splitter.hpp"

#include "slave/error.hpp"

namespace cocaine {

class state_t;
class terminating_t;
class broken_t;

class control_t;
class fetcher_t;

typedef std::shared_ptr<
    const dispatch<io::event_traits<io::worker::rpc::invoke>::dispatch_type>
> inject_dispatch_ptr_t;

typedef std::function<void()> close_callback;

struct slave_context {
    context_t&  context;
    manifest_t  manifest;
    profile_t   profile;
    std::string id;

    slave_context(context_t& context, manifest_t manifest, profile_t profile) :
        context(context),
        manifest(manifest),
        profile(profile),
        id(unique_id_t().string())
    {}
};

/// Actual slave implementation.
class state_machine_t:
    public std::enable_shared_from_this<state_machine_t>
{
    class spawning_t;
    class handshaking_t;
    class active_t;
    class sealing_t;

    friend class terminating_t;
    friend class broken_t;

    friend class control_t;
    friend class fetcher_t;

public:
    typedef std::function<void(const std::error_code&)> cleanup_handler;

private:
    const std::unique_ptr<logging::log_t> log;

    const slave_context context;
    asio::io_service& loop;

    /// The flag means that the overseer has been destroyed and we shouldn't call the callback.
    std::atomic<bool> closed;
    cleanup_handler cleanup;

    splitter_t splitter;
    std::shared_ptr<fetcher_t> fetcher;
    boost::circular_buffer<std::string> lines;

    synchronized<std::shared_ptr<state_t>> state;

public:
    state_machine_t(slave_context ctx, asio::io_service& loop, cleanup_handler cleanup);
    ~state_machine_t();

    void
    start();

    bool
    active() const noexcept;

    std::shared_ptr<control_t>
    activate(std::shared_ptr<session_t> session, upstream<io::worker::control_tag> stream);

    io::upstream_ptr_t
    inject(inject_dispatch_ptr_t dispatch);

    /// External termination.
    ///
    /// We shouldn't call the cleanup callback after this call.
    void
    terminate(std::error_code ec);

private:
    void
    output(const char* data, size_t size);

    void
    migrate(std::shared_ptr<state_t> desired);

    /// Internal termination.
    void
    close(std::error_code ec);
};

// TODO: Rename to `comrade`, because in Soviet Russia slave owns you!
class slave_t {
public:
    typedef state_machine_t::cleanup_handler cleanup_handler;

private:
    /// Termination reason.
    std::error_code ec;

    /// The slave state machine implementation.
    std::shared_ptr<state_machine_t> machine;

public:
    slave_t(slave_context context, asio::io_service& loop, cleanup_handler fn);
    slave_t(const slave_t& other) = delete;
    slave_t(slave_t&&) = default;

    ~slave_t();

    slave_t& operator=(const slave_t& other) = delete;
    slave_t& operator=(slave_t&&) = default;

    bool
    active() const noexcept;

    std::shared_ptr<control_t>
    activate(std::shared_ptr<session_t> session, upstream<io::worker::control_tag> stream);

    io::upstream_ptr_t
    inject(inject_dispatch_ptr_t dispatch);

    /// Marks the slave for termination using the given error code.
    ///
    /// It will be terminated later in destructor.
    void
    terminate(std::error_code ec);
};

}
