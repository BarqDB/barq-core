#ifndef BARQ_OBJECT_STORE_C_API_TYPES_HPP
#define BARQ_OBJECT_STORE_C_API_TYPES_HPP

#include <barq.h>

#include <barq/util/to_string.hpp>

#include <barq/object-store/c_api/conversion.hpp>
#include <barq/object-store/c_api/error.hpp>
#include <barq/object-store/object.hpp>
#include <barq/object-store/object_accessor.hpp>
#include <barq/object-store/object_schema.hpp>
#include <barq/object-store/shared_barq.hpp>
#include <barq/object-store/thread_safe_reference.hpp>
#include <barq/object-store/util/scheduler.hpp>

#if BARQ_ENABLE_SYNC

#include <barq/object-store/sync/generic_network_transport.hpp>
#include <barq/object-store/sync/impl/sync_client.hpp>
#include <barq/sync/binding_callback_thread_observer.hpp>
#include <barq/sync/socket_provider.hpp>
#include <barq/sync/subscriptions.hpp>
#endif // BARQ_ENABLE_SYNC

#include <memory>
#include <stdexcept>
#include <string>

namespace barq::c_api {
class NotClonable : public RuntimeError {
public:
    NotClonable()
        : RuntimeError(ErrorCodes::NotCloneable, "Not clonable")
    {
    }
};

class CallbackFailed : public RuntimeError {
public:
    // SDK-provided opaque error value when error == BARQ_ERR_CALLBACK with a callout to
    // barq_register_user_code_callback_error()
    void* user_code_error{nullptr};

    CallbackFailed()
        : RuntimeError(ErrorCodes::CallbackFailed, "User-provided callback failed")
    {
    }

    explicit CallbackFailed(void* error)
        : CallbackFailed()
    {
        user_code_error = error;
    }
};

struct WrapC {
    static constexpr uint64_t s_cookie_value = 0xdeadbeefdeadbeef;
    uint64_t cookie;
    WrapC()
        : cookie(s_cookie_value)
    {
    }
    virtual ~WrapC()
    {
        cookie = 0;
    }

    virtual WrapC* clone() const
    {
        throw NotClonable();
    }

    virtual bool is_frozen() const
    {
        return false;
    }

    virtual bool equals(const WrapC& other) const noexcept
    {
        return this == &other;
    }

    virtual barq_thread_safe_reference_t* get_thread_safe_reference() const
    {
        throw LogicError{ErrorCodes::IllegalOperation,
                         "Thread safe references cannot be created for this object type"};
    }
};

struct FreeUserdata {
    barq_free_userdata_func_t m_func;
    FreeUserdata(barq_free_userdata_func_t func = nullptr)
        : m_func(func)
    {
    }
    void operator()(void* ptr)
    {
        if (m_func) {
            (m_func)(ptr);
        }
    }
};

using UserdataPtr = std::unique_ptr<void, FreeUserdata>;
using SharedUserdata = std::shared_ptr<void>;

} // namespace barq::c_api

struct barq_async_error : barq::c_api::WrapC {
    barq::c_api::ErrorStorage error_storage;

    explicit barq_async_error(const barq::c_api::ErrorStorage& storage)
        : error_storage(storage)
    {
    }

    explicit barq_async_error(std::exception_ptr ep)
        : error_storage(std::move(ep))
    {
    }

    barq_async_error* clone() const override
    {
        return new barq_async_error(*this);
    }

    bool equals(const WrapC& other) const noexcept final
    {
        if (auto ptr = dynamic_cast<const barq_async_error_t*>(&other)) {
            return error_storage == ptr->error_storage;
        }
        return false;
    }
};

struct barq_thread_safe_reference : barq::c_api::WrapC {
    barq_thread_safe_reference(const barq_thread_safe_reference&) = delete;

protected:
    barq_thread_safe_reference() {}
};

struct barq_config : barq::c_api::WrapC, barq::BarqConfig {
    using BarqConfig::BarqConfig;
    std::map<void*, barq_free_userdata_func_t> free_functions;
    barq_config(const barq_config&) = delete;
    barq_config& operator=(const barq_config&) = delete;
    ~barq_config()
    {
        for (auto& f : free_functions) {
            f.second(f.first);
        }
    }
};

// LCOV_EXCL_START
struct barq_scheduler : barq::c_api::WrapC, std::shared_ptr<barq::util::Scheduler> {
    explicit barq_scheduler(std::shared_ptr<barq::util::Scheduler> ptr)
        : std::shared_ptr<barq::util::Scheduler>(std::move(ptr))
    {
    }

    barq_scheduler* clone() const
    {
        return new barq_scheduler{*this};
    }

    bool equals(const WrapC& other) const noexcept final
    {
        if (auto ptr = dynamic_cast<const barq_scheduler_t*>(&other)) {
            if (get() == ptr->get()) {
                return true;
            }
            if (get()->is_same_as(ptr->get())) {
                return true;
            }
        }
        return false;
    }
};
// LCOV_EXCL_STOP

struct barq_schema : barq::c_api::WrapC {
    std::unique_ptr<barq::Schema> owned;
    const barq::Schema* ptr = nullptr;

    barq_schema(std::unique_ptr<barq::Schema> o, const barq::Schema* ptr = nullptr)
        : owned(std::move(o))
        , ptr(ptr ? ptr : owned.get())
    {
    }

    explicit barq_schema(const barq::Schema* ptr)
        : ptr(ptr)
    {
    }

    barq_schema_t* clone() const override
    {
        auto o = std::make_unique<barq::Schema>(*ptr);
        return new barq_schema_t{std::move(o)};
    }

    bool equals(const WrapC& other) const noexcept final
    {
        if (auto other_ptr = dynamic_cast<const barq_schema_t*>(&other)) {
            return *ptr == *other_ptr->ptr;
        }
        return false;
    }
};

struct shared_barq : barq::c_api::WrapC, barq::SharedBarq {
    shared_barq(barq::SharedBarq barq)
        : barq::SharedBarq{std::move(barq)}
    {
    }

    shared_barq* clone() const override
    {
        return new shared_barq{*this};
    }

    bool is_frozen() const override
    {
        return get()->is_frozen();
    }

    bool equals(const WrapC& other) const noexcept final
    {
        if (auto ptr = dynamic_cast<const shared_barq*>(&other)) {
            return get() == ptr->get();
        }
        return false;
    }

    struct thread_safe_reference : barq_thread_safe_reference, barq::ThreadSafeReference {
        thread_safe_reference(const barq::SharedBarq& barq)
            : barq::ThreadSafeReference(barq)
        {
        }

        thread_safe_reference(barq::ThreadSafeReference&& other)
            : barq::ThreadSafeReference(std::move(other))
        {
            BARQ_ASSERT(this->is<barq::SharedBarq>());
        }
    };

    barq_thread_safe_reference_t* get_thread_safe_reference() const final
    {
        return new thread_safe_reference{*this};
    }
};

struct barq_object : barq::c_api::WrapC, barq::Object {
    explicit barq_object(barq::Object obj)
        : barq::Object(std::move(obj))
    {
    }

    barq_object* clone() const override
    {
        return new barq_object{*this};
    }

    bool is_frozen() const override
    {
        return barq::Object::is_frozen();
    }

    bool equals(const WrapC& other) const noexcept final
    {
        if (auto ptr = dynamic_cast<const barq_object_t*>(&other)) {
            auto a = get_obj();
            auto b = ptr->get_obj();
            return a.get_table() == b.get_table() && a.get_key() == b.get_key();
        }
        return false;
    }

    struct thread_safe_reference : barq_thread_safe_reference, barq::ThreadSafeReference {
        thread_safe_reference(const barq::Object& obj)
            : barq::ThreadSafeReference(obj)
        {
        }
    };

    barq_thread_safe_reference_t* get_thread_safe_reference() const final
    {
        return new thread_safe_reference{*this};
    }
};

struct barq_list : barq::c_api::WrapC, barq::List {
    explicit barq_list(List list)
        : List(std::move(list))
    {
    }

    barq_list* clone() const override
    {
        return new barq_list{*this};
    }

    bool is_frozen() const override
    {
        return List::is_frozen();
    }

    bool equals(const WrapC& other) const noexcept final
    {
        if (auto ptr = dynamic_cast<const barq_list_t*>(&other)) {
            return get_barq() == ptr->get_barq() && get_parent_table_key() == ptr->get_parent_table_key() &&
                   get_parent_column_key() == ptr->get_parent_column_key() &&
                   get_parent_object_key() == ptr->get_parent_object_key();
        }
        return false;
    }

    struct thread_safe_reference : barq_thread_safe_reference, barq::ThreadSafeReference {
        thread_safe_reference(const List& list)
            : barq::ThreadSafeReference(list)
        {
        }
    };

    barq_thread_safe_reference_t* get_thread_safe_reference() const final
    {
        return new thread_safe_reference{*this};
    }
};

struct barq_set : barq::c_api::WrapC, barq::object_store::Set {
    explicit barq_set(Set set)
        : Set(std::move(set))
    {
    }

    barq_set* clone() const override
    {
        return new barq_set{*this};
    }

    bool is_frozen() const override
    {
        return Set::is_frozen();
    }

    bool equals(const WrapC& other) const noexcept final
    {
        if (auto ptr = dynamic_cast<const barq_set_t*>(&other)) {
            return get_barq() == ptr->get_barq() && get_parent_table_key() == ptr->get_parent_table_key() &&
                   get_parent_column_key() == ptr->get_parent_column_key() &&
                   get_parent_object_key() == ptr->get_parent_object_key();
        }
        return false;
    }

    struct thread_safe_reference : barq_thread_safe_reference, barq::ThreadSafeReference {
        thread_safe_reference(const Set& set)
            : barq::ThreadSafeReference(set)
        {
        }
    };

    barq_thread_safe_reference_t* get_thread_safe_reference() const final
    {
        return new thread_safe_reference{*this};
    }
};

struct barq_dictionary : barq::c_api::WrapC, barq::object_store::Dictionary {
    explicit barq_dictionary(Dictionary set)
        : Dictionary(std::move(set))
    {
    }

    barq_dictionary* clone() const override
    {
        return new barq_dictionary{*this};
    }

    bool is_frozen() const override
    {
        return Dictionary::is_frozen();
    }

    bool equals(const WrapC& other) const noexcept final
    {
        if (auto ptr = dynamic_cast<const barq_dictionary_t*>(&other)) {
            return get_barq() == ptr->get_barq() && get_parent_table_key() == ptr->get_parent_table_key() &&
                   get_parent_column_key() == ptr->get_parent_column_key() &&
                   get_parent_object_key() == ptr->get_parent_object_key();
        }
        return false;
    }

    struct thread_safe_reference : barq_thread_safe_reference, barq::ThreadSafeReference {
        thread_safe_reference(const Dictionary& set)
            : barq::ThreadSafeReference(set)
        {
        }
    };

    barq_thread_safe_reference_t* get_thread_safe_reference() const final
    {
        return new thread_safe_reference{*this};
    }
};

struct barq_key_path_array : barq::c_api::WrapC, barq::KeyPathArray {
    explicit barq_key_path_array(barq::KeyPathArray kpa)
        : barq::KeyPathArray(std::move(kpa))
    {
    }
};

struct barq_object_changes : barq::c_api::WrapC, barq::CollectionChangeSet {
    explicit barq_object_changes(barq::CollectionChangeSet changes)
        : barq::CollectionChangeSet(std::move(changes))
    {
    }

    barq_object_changes* clone() const override
    {
        return new barq_object_changes{static_cast<const barq::CollectionChangeSet&>(*this)};
    }
};

struct barq_collection_changes : barq::c_api::WrapC, barq::CollectionChangeSet {
    explicit barq_collection_changes(barq::CollectionChangeSet changes)
        : barq::CollectionChangeSet(std::move(changes))
    {
    }

    barq_collection_changes* clone() const override
    {
        return new barq_collection_changes{static_cast<const barq::CollectionChangeSet&>(*this)};
    }
};

struct barq_dictionary_changes : barq::c_api::WrapC, barq::DictionaryChangeSet {
    explicit barq_dictionary_changes(barq::DictionaryChangeSet changes)
        : barq::DictionaryChangeSet(std::move(changes))
    {
    }

    barq_dictionary_changes* clone() const override
    {
        return new barq_dictionary_changes{static_cast<const barq::DictionaryChangeSet&>(*this)};
    }
};

struct barq_notification_token : barq::c_api::WrapC, barq::NotificationToken {
    explicit barq_notification_token(barq::NotificationToken token)
        : barq::NotificationToken(std::move(token))
    {
    }
};

struct barq_callback_token : barq::c_api::WrapC {
protected:
    barq_callback_token(barq_t* barq, uint64_t token)
        : m_barq(barq)
        , m_token(token)
    {
    }
    barq_t* m_barq;
    uint64_t m_token;
};

struct barq_callback_token_barq : barq_callback_token {
    barq_callback_token_barq(barq_t* barq, uint64_t token)
        : barq_callback_token(barq, token)
    {
    }
    ~barq_callback_token_barq() override;
};

struct barq_callback_token_schema : barq_callback_token {
    barq_callback_token_schema(barq_t* barq, uint64_t token)
        : barq_callback_token(barq, token)
    {
    }
    ~barq_callback_token_schema() override;
};

struct barq_refresh_callback_token : barq_callback_token {
    barq_refresh_callback_token(barq_t* barq, uint64_t token)
        : barq_callback_token(barq, token)
    {
    }
    ~barq_refresh_callback_token() override;
};

struct barq_query : barq::c_api::WrapC {
    barq::Query query;
    std::weak_ptr<barq::Barq> weak_barq;

    explicit barq_query(barq::Query query, barq::util::bind_ptr<barq::DescriptorOrdering> ordering,
                         std::weak_ptr<barq::Barq> barq)
        : query(std::move(query))
        , weak_barq(barq)
        , m_ordering(std::move(ordering))
    {
    }

    barq_query* clone() const override
    {
        return new barq_query{*this};
    }

    barq::Query& get_query()
    {
        return query;
    }

    const barq::DescriptorOrdering& get_ordering() const
    {
        static const barq::DescriptorOrdering null_ordering;
        return m_ordering ? *m_ordering : null_ordering;
    }

    const char* get_description()
    {
        m_description = query.get_description();
        if (m_ordering)
            m_description += " " + m_ordering->get_description(query.get_table());
        return m_description.c_str();
    }

private:
    barq::util::bind_ptr<barq::DescriptorOrdering> m_ordering;
    std::string m_description;

    barq_query(const barq_query&) = default;
};

struct barq_results : barq::c_api::WrapC, barq::Results {
    explicit barq_results(barq::Results results)
        : barq::Results(std::move(results))
    {
    }

    barq_results* clone() const override
    {
        return new barq_results{static_cast<const barq::Results&>(*this)};
    }

    bool is_frozen() const override
    {
        return barq::Results::is_frozen();
    }

    struct thread_safe_reference : barq_thread_safe_reference_t, barq::ThreadSafeReference {
        thread_safe_reference(const barq::Results& results)
            : barq::ThreadSafeReference(results)
        {
        }
    };

    barq_thread_safe_reference_t* get_thread_safe_reference() const final
    {
        return new thread_safe_reference{*this};
    }
};

#if BARQ_ENABLE_SYNC

struct barq_async_open_task_progress_notification_token : barq::c_api::WrapC {
    barq_async_open_task_progress_notification_token(std::shared_ptr<barq::AsyncOpenTask> task, uint64_t token)
        : task(task)
        , token(token)
    {
    }
    ~barq_async_open_task_progress_notification_token();
    std::shared_ptr<barq::AsyncOpenTask> task;
    uint64_t token;
};

struct barq_sync_session_connection_state_notification_token : barq::c_api::WrapC {
    barq_sync_session_connection_state_notification_token(std::shared_ptr<barq::SyncSession> session,
                                                           uint64_t token)
        : session(session)
        , token(token)
    {
    }
    ~barq_sync_session_connection_state_notification_token();
    std::shared_ptr<barq::SyncSession> session;
    uint64_t token;
};

struct barq_http_transport : barq::c_api::WrapC, std::shared_ptr<barq::networking::GenericNetworkTransport> {
    barq_http_transport(std::shared_ptr<barq::networking::GenericNetworkTransport> transport)
        : std::shared_ptr<barq::networking::GenericNetworkTransport>(std::move(transport))
    {
    }

    barq_http_transport* clone() const override
    {
        return new barq_http_transport{*this};
    }

    bool equals(const WrapC& other) const noexcept final
    {
        if (auto ptr = dynamic_cast<const barq_http_transport*>(&other)) {
            return get() == ptr->get();
        }
        return false;
    }
};

// This class must be freed using barq_release()
struct barq_sync_client_config : barq::c_api::WrapC, barq::SyncClientConfig {
    using SyncClientConfig::SyncClientConfig;
};

struct barq_sync_config : barq::c_api::WrapC, barq::SyncConfig {
    using SyncConfig::SyncConfig;
    barq_sync_config(const SyncConfig& c)
        : SyncConfig(c)
    {
    }
};

struct barq_user : barq::c_api::WrapC, std::shared_ptr<barq::SyncUser> {
    barq_user(std::shared_ptr<barq::SyncUser> user)
        : std::shared_ptr<barq::SyncUser>{std::move(user)}
    {
    }

    barq_user* clone() const override
    {
        return new barq_user{*this};
    }

    bool equals(const WrapC& other) const noexcept final
    {
        if (auto ptr = dynamic_cast<const barq_user*>(&other)) {
            return get() == ptr->get();
        }
        return false;
    }
};

struct barq_sync_session : barq::c_api::WrapC, std::shared_ptr<barq::SyncSession> {
    barq_sync_session(std::shared_ptr<barq::SyncSession> session)
        : std::shared_ptr<barq::SyncSession>{std::move(session)}
    {
    }

    barq_sync_session* clone() const override
    {
        return new barq_sync_session{*this};
    }

    bool equals(const WrapC& other) const noexcept final
    {
        if (auto ptr = dynamic_cast<const barq_sync_session*>(&other)) {
            return get() == ptr->get();
        }
        return false;
    }
};

struct barq_sync_manager : barq::c_api::WrapC, std::shared_ptr<barq::SyncManager> {
    barq_sync_manager(std::shared_ptr<barq::SyncManager> manager)
        : std::shared_ptr<barq::SyncManager>{std::move(manager)}
    {
    }

    barq_sync_manager* clone() const override
    {
        return new barq_sync_manager{*this};
    }

    bool equals(const WrapC& other) const noexcept final
    {
        if (auto ptr = dynamic_cast<const barq_sync_manager*>(&other)) {
            return get() == ptr->get();
        }
        return false;
    }
};

struct barq_flx_sync_subscription : barq::c_api::WrapC, barq::sync::Subscription {
    barq_flx_sync_subscription(barq::sync::Subscription&& subscription)
        : barq::sync::Subscription(std::move(subscription))
    {
    }

    barq_flx_sync_subscription(const barq::sync::Subscription& subscription)
        : barq::sync::Subscription(subscription)
    {
    }

    barq_flx_sync_subscription* clone() const override
    {
        return new barq_flx_sync_subscription{*this};
    }

    bool equals(const WrapC& other) const noexcept final
    {
        if (auto ptr = dynamic_cast<const barq_flx_sync_subscription*>(&other)) {
            return *ptr == *this;
        }
        return false;
    }
};

struct barq_flx_sync_subscription_set : barq::c_api::WrapC, barq::sync::SubscriptionSet {
    barq_flx_sync_subscription_set(barq::sync::SubscriptionSet&& subscription_set)
        : barq::sync::SubscriptionSet(std::move(subscription_set))
    {
    }
};

struct barq_flx_sync_mutable_subscription_set : barq::c_api::WrapC, barq::sync::MutableSubscriptionSet {
    barq_flx_sync_mutable_subscription_set(barq::sync::MutableSubscriptionSet&& subscription_set)
        : barq::sync::MutableSubscriptionSet(std::move(subscription_set))
    {
    }
};

struct barq_async_open_task : barq::c_api::WrapC, std::shared_ptr<barq::AsyncOpenTask> {
    barq_async_open_task(std::shared_ptr<barq::AsyncOpenTask> task)
        : std::shared_ptr<barq::AsyncOpenTask>{std::move(task)}
    {
    }

    barq_async_open_task* clone() const override
    {
        return new barq_async_open_task{*this};
    }

    bool equals(const WrapC& other) const noexcept final
    {
        if (auto ptr = dynamic_cast<const barq_async_open_task*>(&other)) {
            return get() == ptr->get();
        }
        return false;
    }
};

struct barq_sync_socket : barq::c_api::WrapC, std::shared_ptr<barq::sync::SyncSocketProvider> {
    explicit barq_sync_socket(std::shared_ptr<barq::sync::SyncSocketProvider> ptr)
        : std::shared_ptr<barq::sync::SyncSocketProvider>(std::move(ptr))
    {
    }

    barq_sync_socket* clone() const override
    {
        return new barq_sync_socket{*this};
    }

    bool equals(const WrapC& other) const noexcept final
    {
        if (auto ptr = dynamic_cast<const barq_sync_socket*>(&other)) {
            return get() == ptr->get();
        }
        return false;
    }
};

struct barq_websocket_observer : barq::c_api::WrapC, std::shared_ptr<barq::sync::WebSocketObserver> {
    explicit barq_websocket_observer(std::shared_ptr<barq::sync::WebSocketObserver> ptr)
        : std::shared_ptr<barq::sync::WebSocketObserver>(std::move(ptr))
    {
    }

    barq_websocket_observer* clone() const override
    {
        return new barq_websocket_observer{*this};
    }

    bool equals(const WrapC& other) const noexcept final
    {
        if (auto ptr = dynamic_cast<const barq_websocket_observer*>(&other)) {
            return get() == ptr->get();
        }
        return false;
    }
};

struct barq_sync_socket_callback : barq::c_api::WrapC,
                                    std::shared_ptr<barq::sync::SyncSocketProvider::FunctionHandler> {
    explicit barq_sync_socket_callback(std::shared_ptr<barq::sync::SyncSocketProvider::FunctionHandler> ptr)
        : std::shared_ptr<barq::sync::SyncSocketProvider::FunctionHandler>(std::move(ptr))
    {
    }

    bool equals(const WrapC& other) const noexcept final
    {
        if (auto ptr = dynamic_cast<const barq_sync_socket_callback*>(&other)) {
            return get() == ptr->get();
        }
        return false;
    }

    void operator()(barq_sync_socket_callback_result_e result, const char* reason)
    {
        if (!get()) {
            return;
        }

        auto complete_status = result == BARQ_ERR_SYNC_SOCKET_SUCCESS
                                   ? barq::Status::OK()
                                   : barq::Status{static_cast<barq::ErrorCodes::Error>(result), reason};
        (*get())(complete_status);
    }
};

struct CBindingThreadObserver final : public barq::BindingCallbackThreadObserver {
public:
    CBindingThreadObserver(barq_on_object_store_thread_callback_t on_thread_create,
                           barq_on_object_store_thread_callback_t on_thread_destroy,
                           barq_on_object_store_error_callback_t on_error, barq_userdata_t userdata,
                           barq_free_userdata_func_t free_userdata)
        : m_create_callback_func{on_thread_create}
        , m_destroy_callback_func{on_thread_destroy}
        , m_error_callback_func{on_error}
        , m_user_data{userdata, [&free_userdata] {
                          if (free_userdata)
                              return free_userdata;
                          return CBindingThreadObserver::m_default_free_userdata;
                      }()}
    {
    }

    void did_create_thread() override
    {
        if (m_create_callback_func)
            m_create_callback_func(m_user_data.get());
    }

    void will_destroy_thread() override
    {
        if (m_destroy_callback_func)
            m_destroy_callback_func(m_user_data.get());
    }

    bool handle_error(std::exception const& e) override
    {
        if (!m_error_callback_func)
            return false;

        return m_error_callback_func(m_user_data.get(), e.what());
    }

    bool has_handle_error() override
    {
        return bool(m_error_callback_func);
    }

    /// {@
    /// For testing: Return the values in this CBindingThreadObserver for comparing if two objects
    /// have the same callback functions and userdata ptr values.
    barq_on_object_store_thread_callback_t test_get_create_callback_func() const noexcept
    {
        return m_create_callback_func;
    }
    barq_on_object_store_thread_callback_t test_get_destroy_callback_func() const noexcept
    {
        return m_destroy_callback_func;
    }
    barq_on_object_store_error_callback_t test_get_error_callback_func() const noexcept
    {
        return m_error_callback_func;
    }
    barq_userdata_t test_get_userdata_ptr() const noexcept
    {
        return m_user_data.get();
    }
    /// @}

private:
    CBindingThreadObserver() = default;

    static constexpr barq_free_userdata_func_t m_default_free_userdata = [](barq_userdata_t) {};

    barq_on_object_store_thread_callback_t m_create_callback_func = nullptr;
    barq_on_object_store_thread_callback_t m_destroy_callback_func = nullptr;
    barq_on_object_store_error_callback_t m_error_callback_func = nullptr;
    barq::c_api::UserdataPtr m_user_data;
};

#endif // BARQ_ENABLE_SYNC

#endif // BARQ_OBJECT_STORE_C_API_TYPES_HPP
