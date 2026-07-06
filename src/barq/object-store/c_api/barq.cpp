#include <barq/object-store/c_api/types.hpp>
#include "barq.hpp"


barq_callback_token_barq::~barq_callback_token_barq()
{
    barq::c_api::CBindingContext::get(*m_barq).barq_changed_callbacks().remove(m_token);
}

barq_callback_token_schema::~barq_callback_token_schema()
{
    barq::c_api::CBindingContext::get(*m_barq).schema_changed_callbacks().remove(m_token);
}

barq_refresh_callback_token::~barq_refresh_callback_token()
{
    barq::c_api::CBindingContext::get(*m_barq).barq_pending_refresh_callbacks().remove(m_token);
}

namespace barq::c_api {


BARQ_API bool barq_get_version_id(const barq_t* barq, bool* out_found, barq_version_id_t* out_version)
{
    return wrap_err([&]() {
        util::Optional<VersionID> version = (*barq)->current_transaction_version();
        if (version) {
            if (out_version) {
                *out_version = to_capi(*version);
            }
            if (out_found) {
                *out_found = true;
            }
        }
        else {
            if (out_version) {
                *out_version = to_capi(VersionID(0, 0));
            }
            if (out_found) {
                *out_found = false;
            }
        }
        return true;
    });
}

BARQ_API bool barq_get_num_versions(const barq_t* barq, uint64_t* out_versions_count)
{
    return wrap_err([&]() {
        if (out_versions_count) {
            *out_versions_count = (*barq)->get_number_of_versions();
        }
        return true;
    });
}

BARQ_API const char* barq_get_library_version()
{
    return BARQ_VERSION_STRING;
}

BARQ_API void barq_get_library_version_numbers(int* out_major, int* out_minor, int* out_patch, const char** out_extra)
{
    *out_major = BARQ_VERSION_MAJOR;
    *out_minor = BARQ_VERSION_MINOR;
    *out_patch = BARQ_VERSION_PATCH;
    *out_extra = BARQ_VERSION_EXTRA;
}

BARQ_API barq_t* barq_open(const barq_config_t* config)
{
    return wrap_err([&]() {
        return new shared_barq{Barq::get_shared_barq(*config)};
    });
}

BARQ_API bool barq_convert_with_config(const barq_t* barq, const barq_config_t* config, bool merge_with_existing)
{
    return wrap_err([&]() {
        (*barq)->convert(*config, merge_with_existing);
        return true;
    });
}

BARQ_API bool barq_convert_with_path(const barq_t* barq, const char* path, barq_binary_t encryption_key,
                                     bool merge_with_existing)
{
    return wrap_err([&]() {
        Barq::Config config;
        config.path = path;
        if (encryption_key.data) {
            config.encryption_key.assign(encryption_key.data, encryption_key.data + encryption_key.size);
        }
        (*barq)->convert(config, merge_with_existing);
        return true;
    });
}

BARQ_API bool barq_delete_files(const char* barq_file_path, bool* did_delete_barq)
{
    return wrap_err([&]() {
        Barq::delete_files(barq_file_path, did_delete_barq);
        return true;
    });
}

BARQ_API barq_t* _barq_from_native_ptr(const void* pshared_ptr, size_t n)
{
    BARQ_ASSERT_RELEASE(n == sizeof(SharedBarq));
    auto ptr = static_cast<const SharedBarq*>(pshared_ptr);
    return new shared_barq{*ptr};
}

BARQ_API void _barq_get_native_ptr(const barq_t* barq, void* pshared_ptr, size_t n)
{
    BARQ_ASSERT_RELEASE(n == sizeof(SharedBarq));
    auto& shared_ptr = *static_cast<SharedBarq*>(pshared_ptr);
    shared_ptr = *barq;
}

BARQ_API bool barq_is_closed(barq_t* barq)
{
    return (*barq)->is_closed();
}

BARQ_API bool barq_is_writable(const barq_t* barq)
{
    return (*barq)->is_in_transaction() || (*barq)->is_in_async_transaction();
}

BARQ_API bool barq_close(barq_t* barq)
{
    return wrap_err([&]() {
        (*barq)->close();
        return true;
    });
}

BARQ_API bool barq_begin_read(barq_t* barq)
{
    return wrap_err([&]() {
        (*barq)->read_group();
        return true;
    });
}

BARQ_API bool barq_begin_write(barq_t* barq)
{
    return wrap_err([&]() {
        (*barq)->begin_transaction();
        return true;
    });
}

BARQ_API bool barq_commit(barq_t* barq)
{
    return wrap_err([&]() {
        (*barq)->commit_transaction();
        return true;
    });
}

BARQ_API bool barq_rollback(barq_t* barq)
{
    return wrap_err([&]() {
        (*barq)->cancel_transaction();
        return true;
    });
}

BARQ_API bool barq_async_begin_write(barq_t* barq, barq_async_begin_write_func_t callback,
                                     barq_userdata_t userdata, barq_free_userdata_func_t userdata_free,
                                     bool notify_only, unsigned int* transaction_id)
{
    auto cb = [callback, userdata = UserdataPtr{userdata, userdata_free}]() {
        callback(userdata.get());
    };
    return wrap_err([&]() {
        auto id = (*barq)->async_begin_transaction(std::move(cb), notify_only);
        if (transaction_id)
            *transaction_id = id;
        return true;
    });
}

BARQ_API bool barq_async_commit(barq_t* barq, barq_async_commit_func_t callback, barq_userdata_t userdata,
                                barq_free_userdata_func_t userdata_free, bool allow_grouping,
                                unsigned int* transaction_id)
{
    auto cb = [callback, userdata = UserdataPtr{userdata, userdata_free}](std::exception_ptr err) {
        if (err) {
            try {
                std::rethrow_exception(err);
            }
            catch (const std::exception& e) {
                callback(userdata.get(), true, e.what());
            }
        }
        else {
            callback(userdata.get(), false, nullptr);
        }
    };
    return wrap_err([&]() {
        auto id = (*barq)->async_commit_transaction(std::move(cb), allow_grouping);
        if (transaction_id)
            *transaction_id = id;
        return true;
    });
}

BARQ_API bool barq_async_cancel(barq_t* barq, unsigned int token, bool* cancelled)
{
    return wrap_err([&]() {
        auto res = (*barq)->async_cancel_transaction(token);
        if (cancelled)
            *cancelled = res;
        return true;
    });
}

BARQ_API barq_callback_token_t* barq_add_barq_changed_callback(barq_t* barq,
                                                                 barq_on_barq_change_func_t callback,
                                                                 barq_userdata_t userdata,
                                                                 barq_free_userdata_func_t free_userdata)
{
    util::UniqueFunction<void()> func = [callback, userdata = UserdataPtr{userdata, free_userdata}]() {
        callback(userdata.get());
    };
    return new barq_callback_token_barq(
        barq, CBindingContext::get(*barq).barq_changed_callbacks().add(std::move(func)));
}

BARQ_API barq_refresh_callback_token_t* barq_add_barq_refresh_callback(barq_t* barq,
                                                                         barq_on_barq_refresh_func_t callback,
                                                                         barq_userdata_t userdata,
                                                                         barq_free_userdata_func_t userdata_free)
{
    util::UniqueFunction<void()> func = [callback, userdata = UserdataPtr{userdata, userdata_free}]() {
        callback(userdata.get());
    };

    if ((*barq)->is_frozen())
        return nullptr;

    const util::Optional<DB::version_type>& latest_snapshot_version = (*barq)->latest_snapshot_version();

    if (!latest_snapshot_version)
        return nullptr;

    const auto current_version = (*barq)->current_transaction_version();
    if (!current_version || *latest_snapshot_version <= (*current_version).version)
        return nullptr;

    auto& refresh_callbacks = CBindingContext::get(*barq).barq_pending_refresh_callbacks();
    return new barq_refresh_callback_token(barq, refresh_callbacks.add(*latest_snapshot_version, std::move(func)));
}

BARQ_API bool barq_refresh(barq_t* barq, bool* did_refresh)
{
    return wrap_err([&]() {
        bool result = (*barq)->refresh();
        if (did_refresh) {
            *did_refresh = result;
        }

        // the call succeeded
        return true;
    });
}

BARQ_API barq_t* barq_freeze(const barq_t* live_barq)
{
    return wrap_err([&]() {
        auto& p = **live_barq;
        return new barq_t{p.freeze()};
    });
}

BARQ_API bool barq_compact(barq_t* barq, bool* did_compact)
{
    return wrap_err([&]() {
        auto& p = **barq;
        bool result = p.compact();
        if (did_compact) {
            *did_compact = result;
        }

        // the call succeeded
        return true;
    });
}

BARQ_API bool barq_remove_table(barq_t* barq, const char* table_name, bool* table_deleted)
{
    if (table_deleted)
        *table_deleted = false;

    return wrap_err([&] {
        auto table = ObjectStore::table_for_object_type((*barq)->read_group(), table_name);
        if (table) {
            const auto& schema = (*barq)->schema();
            const auto& object_schema = schema.find(table_name);
            if (object_schema != schema.end()) {
                throw LogicError(ErrorCodes::InvalidSchemaChange,
                                 "Attempt to remove a table that is currently part of the schema");
            }
            (*barq)->read_group().remove_table(table->get_key());
            *table_deleted = true;
        }
        return true;
    });
}

BARQ_API barq_t* barq_from_thread_safe_reference(barq_thread_safe_reference_t* tsr, barq_scheduler_t* scheduler)
{
    return wrap_err([&]() {
        auto rtsr = dynamic_cast<shared_barq::thread_safe_reference*>(tsr);
        if (!rtsr) {
            throw LogicError{ErrorCodes::IllegalOperation, "Thread safe reference type mismatch"};
        }

        std::shared_ptr<util::Scheduler> sch;
        if (scheduler) {
            sch = *scheduler;
        }
        auto barq = Barq::get_shared_barq(static_cast<ThreadSafeReference&&>(*rtsr), sch);
        return new shared_barq{std::move(barq)};
    });
}

CBindingContext& CBindingContext::get(SharedBarq barq)
{
    if (!barq->m_binding_context) {
        barq->m_binding_context.reset(new CBindingContext(barq));
    }

    CBindingContext* ctx = dynamic_cast<CBindingContext*>(barq->m_binding_context.get());
    BARQ_ASSERT(ctx != nullptr);
    return *ctx;
}

void CBindingContext::did_change(std::vector<ObserverState> const&, std::vector<void*> const&, bool)
{
    if (auto ptr = barq.lock()) {
        auto version_id = ptr->read_transaction_version();
        m_barq_pending_refresh_callbacks.invoke(version_id.version);
    }
    m_barq_changed_callbacks.invoke();
}

} // namespace barq::c_api
