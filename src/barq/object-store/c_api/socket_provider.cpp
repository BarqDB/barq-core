#include <barq/error_codes.hpp>
#include <barq/status.hpp>
#include <barq/object-store/c_api/util.hpp>
#include <barq/sync/socket_provider.hpp>
#include <barq/sync/network/websocket.hpp>

namespace barq::c_api {
namespace {

// THis class represents the timer resource that is returned to the sync client from the
// CAPI implementation details for canceling and deleting the timer resources.
struct CAPITimer : sync::SyncSocketProvider::Timer {
public:
    CAPITimer(barq_userdata_t userdata, int64_t delay_ms, barq_sync_socket_timer_callback_t* handler,
              barq_sync_socket_create_timer_func_t create_timer_func,
              barq_sync_socket_timer_canceled_func_t cancel_timer_func,
              barq_sync_socket_timer_free_func_t free_timer_func)
        : m_userdata(userdata)
        , m_timer_create(create_timer_func)
        , m_timer_cancel(cancel_timer_func)
        , m_timer_free(free_timer_func)
    {
        m_timer = m_timer_create(userdata, delay_ms, handler);
    }

    /// Cancels the timer and destroys the timer instance.
    ~CAPITimer()
    {
        // Make sure the timer is stopped, if not already
        m_timer_cancel(m_userdata, m_timer);
        m_timer_free(m_userdata, m_timer);
    }

    // Cancel the timer immediately - the CAPI implementation will need to call the
    // barq_sync_socket_timer_canceled function to notify the sync client that the
    // timer has been canceled and must be called in the same execution thread as
    // the timer complete.
    void cancel() override
    {
        m_timer_cancel(m_userdata, m_timer);
    }

private:
    // A pointer to the CAPI implementation's timer instance. This is provided by the
    // CAPI implementation when the create_timer_func function is called.
    barq_sync_socket_timer_t m_timer = nullptr;

    // These values were originally provided to the socket_provider instance by the CAPI
    // implementation when it was created
    barq_userdata_t m_userdata = nullptr;
    barq_sync_socket_create_timer_func_t m_timer_create = nullptr;
    barq_sync_socket_timer_canceled_func_t m_timer_cancel = nullptr;
    barq_sync_socket_timer_free_func_t m_timer_free = nullptr;
};

static void barq_sync_socket_op_complete(barq_sync_socket_callback* barq_callback,
                                          barq_sync_socket_callback_result_e result, const char* reason)
{
    if (!barq_callback)
        return;

    (*barq_callback)(result, reason);
    barq_release(barq_callback);
}

BARQ_API void barq_sync_socket_timer_complete(barq_sync_socket_timer_callback_t* timer_handler,
                                              barq_sync_socket_callback_result_e result, const char* reason)
{
    barq_sync_socket_op_complete(timer_handler, result, reason);
}

BARQ_API void barq_sync_socket_timer_canceled(barq_sync_socket_timer_callback_t* timer_handler)
{
    barq_sync_socket_op_complete(timer_handler, BARQ_ERR_SYNC_SOCKET_OPERATION_ABORTED, "Timer canceled");
}

// This class represents a websocket instance provided by the CAPI implememtation for sending
// and receiving data and connection state from the websocket. This class is used directly by
// the sync client.
struct CAPIWebSocket : sync::WebSocketInterface {
public:
    CAPIWebSocket(barq_userdata_t userdata, barq_sync_socket_connect_func_t websocket_connect_func,
                  barq_sync_socket_websocket_async_write_func_t websocket_write_func,
                  barq_sync_socket_websocket_free_func_t websocket_free_func, barq_websocket_observer_t* observer,
                  sync::WebSocketEndpoint&& endpoint)
        : m_observer(observer)
        , m_userdata(userdata)
        , m_websocket_connect(websocket_connect_func)
        , m_websocket_async_write(websocket_write_func)
        , m_websocket_free(websocket_free_func)
    {
        barq_websocket_endpoint_t capi_endpoint;
        capi_endpoint.address = endpoint.address.c_str();
        capi_endpoint.port = endpoint.port;
        capi_endpoint.path = endpoint.path.c_str();

        std::vector<const char*> protocols;
        for (size_t i = 0; i < endpoint.protocols.size(); ++i) {
            auto& protocol = endpoint.protocols[i];
            protocols.push_back(protocol.c_str());
        }
        capi_endpoint.protocols = protocols.data();
        capi_endpoint.num_protocols = protocols.size();
        capi_endpoint.is_ssl = endpoint.is_ssl;

        m_socket = m_websocket_connect(m_userdata, capi_endpoint, observer);
    }

    /// Destroys the web socket instance.
    ~CAPIWebSocket()
    {
        m_websocket_free(m_userdata, m_socket);
        barq_release(m_observer);
    }

    void async_write_binary(util::Span<const char> data, sync::SyncSocketProvider::FunctionHandler&& handler) final
    {
        auto shared_handler = std::make_shared<sync::SyncSocketProvider::FunctionHandler>(std::move(handler));
        m_websocket_async_write(m_userdata, m_socket, data.data(), data.size(),
                                new barq_sync_socket_write_callback_t(std::move(shared_handler)));
    }

private:
    // A pointer to the CAPI implementation's websocket instance. This is provided by
    // the m_websocket_connect() function when this websocket instance is created.
    barq_sync_socket_websocket_t m_socket = nullptr;

    // A wrapped reference to the websocket observer in the sync client that receives the
    // websocket status callbacks. This is provided by the Sync Client.
    barq_websocket_observer_t* m_observer = nullptr;

    // These values were originally provided to the socket_provider instance by the CAPI
    // implementation when it was created.
    barq_userdata_t m_userdata = nullptr;
    barq_sync_socket_connect_func_t m_websocket_connect = nullptr;
    barq_sync_socket_websocket_async_write_func_t m_websocket_async_write = nullptr;
    barq_sync_socket_websocket_free_func_t m_websocket_free = nullptr;
};

// Represents the websocket observer in the sync client that receives websocket status
// callbacks and passes them along to the WebSocketObserver object.
struct CAPIWebSocketObserver : sync::WebSocketObserver {
public:
    CAPIWebSocketObserver(std::unique_ptr<sync::WebSocketObserver> observer)
        : m_observer(std::move(observer))
    {
        BARQ_ASSERT_EX(m_observer, "WebSocketObserver cannot be null");
    }

    ~CAPIWebSocketObserver() = default;

    void websocket_connected_handler(const std::string& protocol) final
    {
        m_observer->websocket_connected_handler(protocol);
    }

    void websocket_error_handler() final
    {
        m_observer->websocket_error_handler();
    }

    bool websocket_binary_message_received(util::Span<const char> data) final
    {
        return m_observer->websocket_binary_message_received(data);
    }

    bool websocket_closed_handler(bool was_clean, sync::websocket::WebSocketError code, std::string_view msg) final
    {
        return m_observer->websocket_closed_handler(was_clean, code, msg);
    }

private:
    std::unique_ptr<sync::WebSocketObserver> m_observer;
};

// This is the primary resource for providing event loop, timer and websocket
// resources and synchronization for the Sync Client. The CAPI implementation
// needs to implement the "funct_t" functions provided to this class for connecting
// the implementation to the operations called by the Sync Client.
struct CAPISyncSocketProvider : sync::SyncSocketProvider {
    barq_userdata_t m_userdata = nullptr;
    barq_free_userdata_func_t m_userdata_free = nullptr;
    barq_sync_socket_post_func_t m_post = nullptr;
    barq_sync_socket_create_timer_func_t m_timer_create = nullptr;
    barq_sync_socket_timer_canceled_func_t m_timer_cancel = nullptr;
    barq_sync_socket_timer_free_func_t m_timer_free = nullptr;
    barq_sync_socket_connect_func_t m_websocket_connect = nullptr;
    barq_sync_socket_websocket_async_write_func_t m_websocket_async_write = nullptr;
    barq_sync_socket_websocket_free_func_t m_websocket_free = nullptr;

    CAPISyncSocketProvider() = default;
    CAPISyncSocketProvider(CAPISyncSocketProvider&& other)
        : m_userdata(std::exchange(other.m_userdata, nullptr))
        , m_userdata_free(std::exchange(other.m_userdata_free, nullptr))
        , m_post(std::exchange(other.m_post, nullptr))
        , m_timer_create(std::exchange(other.m_timer_create, nullptr))
        , m_timer_cancel(std::exchange(other.m_timer_cancel, nullptr))
        , m_timer_free(std::exchange(other.m_timer_free, nullptr))
        , m_websocket_connect(std::exchange(other.m_websocket_connect, nullptr))
        , m_websocket_async_write(std::exchange(other.m_websocket_async_write, nullptr))
        , m_websocket_free(std::exchange(other.m_websocket_free, nullptr))
    {
        // userdata_free can be null if userdata is not used
        if (m_userdata != nullptr) {
            BARQ_ASSERT(m_userdata_free);
        }
        BARQ_ASSERT(m_post);
        BARQ_ASSERT(m_timer_create);
        BARQ_ASSERT(m_timer_cancel);
        BARQ_ASSERT(m_timer_free);
        BARQ_ASSERT(m_websocket_connect);
        BARQ_ASSERT(m_websocket_async_write);
        BARQ_ASSERT(m_websocket_free);
    }

    ~CAPISyncSocketProvider()
    {
        if (m_userdata_free) {
            m_userdata_free(m_userdata);
        }
    }

    // Create a websocket object that will be returned to the Sync Client, which is expected to
    // begin connecting to the endpoint as soon as the object is created. The state and any data
    // received is passed to the socket observer via the helper functions defined below this class.
    std::unique_ptr<sync::WebSocketInterface> connect(std::unique_ptr<sync::WebSocketObserver> observer,
                                                      sync::WebSocketEndpoint&& endpoint) final
    {
        auto capi_observer = std::make_shared<CAPIWebSocketObserver>(std::move(observer));
        return std::make_unique<CAPIWebSocket>(m_userdata, m_websocket_connect, m_websocket_async_write,
                                               m_websocket_free, new barq_websocket_observer_t(capi_observer),
                                               std::move(endpoint));
    }

    void post(FunctionHandler&& handler) final
    {
        auto shared_handler = std::make_shared<FunctionHandler>(std::move(handler));
        m_post(m_userdata, new barq_sync_socket_post_callback_t(std::move(shared_handler)));
    }

    SyncTimer create_timer(std::chrono::milliseconds delay, FunctionHandler&& handler) final
    {
        auto shared_handler = std::make_shared<FunctionHandler>(std::move(handler));
        return std::make_unique<CAPITimer>(m_userdata, delay.count(),
                                           new barq_sync_socket_timer_callback_t(std::move(shared_handler)),
                                           m_timer_create, m_timer_cancel, m_timer_free);
    }
};

} // namespace

BARQ_API barq_sync_socket_t* barq_sync_socket_new(
    barq_userdata_t userdata, barq_free_userdata_func_t userdata_free, barq_sync_socket_post_func_t post_func,
    barq_sync_socket_create_timer_func_t create_timer_func,
    barq_sync_socket_timer_canceled_func_t cancel_timer_func, barq_sync_socket_timer_free_func_t free_timer_func,
    barq_sync_socket_connect_func_t websocket_connect_func,
    barq_sync_socket_websocket_async_write_func_t websocket_write_func,
    barq_sync_socket_websocket_free_func_t websocket_free_func)
{
    return wrap_err([&]() {
        auto capi_socket_provider = std::make_shared<CAPISyncSocketProvider>();
        capi_socket_provider->m_userdata = userdata;
        capi_socket_provider->m_userdata_free = userdata_free;
        capi_socket_provider->m_post = post_func;
        capi_socket_provider->m_timer_create = create_timer_func;
        capi_socket_provider->m_timer_cancel = cancel_timer_func;
        capi_socket_provider->m_timer_free = free_timer_func;
        capi_socket_provider->m_websocket_connect = websocket_connect_func;
        capi_socket_provider->m_websocket_async_write = websocket_write_func;
        capi_socket_provider->m_websocket_free = websocket_free_func;
        return new barq_sync_socket_t(std::move(capi_socket_provider));
    });
}

BARQ_API void barq_sync_socket_post_complete(barq_sync_socket_post_callback_t* post_handler,
                                             barq_sync_socket_callback_result_e result, const char* reason)
{
    barq_sync_socket_op_complete(post_handler, result, reason);
}

BARQ_API void barq_sync_socket_write_complete(barq_sync_socket_write_callback_t* write_handler,
                                              barq_sync_socket_callback_result_e result, const char* reason)
{
    barq_sync_socket_op_complete(write_handler, result, reason);
}

BARQ_API void barq_sync_socket_websocket_connected(barq_websocket_observer_t* barq_websocket_observer,
                                                   const char* protocol)
{
    if (barq_websocket_observer)
        barq_websocket_observer->get()->websocket_connected_handler(protocol);
}

BARQ_API void barq_sync_socket_websocket_error(barq_websocket_observer_t* barq_websocket_observer)
{
    if (barq_websocket_observer)
        barq_websocket_observer->get()->websocket_error_handler();
}

BARQ_API bool barq_sync_socket_websocket_message(barq_websocket_observer_t* barq_websocket_observer,
                                                 const char* data, size_t data_size)
{
    if (!barq_websocket_observer)
        return false;

    return barq_websocket_observer->get()->websocket_binary_message_received(util::Span{data, data_size});
}

BARQ_API bool barq_sync_socket_websocket_closed(barq_websocket_observer_t* barq_websocket_observer, bool was_clean,
                                                barq_web_socket_errno_e code, const char* reason)
{
    if (!barq_websocket_observer)
        return false;

    return barq_websocket_observer->get()->websocket_closed_handler(
        was_clean, static_cast<sync::websocket::WebSocketError>(code), reason);
}

BARQ_API void barq_sync_client_config_set_sync_socket(barq_sync_client_config_t* config,
                                                      barq_sync_socket_t* sync_socket) BARQ_API_NOEXCEPT
{
    config->socket_provider = *sync_socket;
}

} // namespace barq::c_api
