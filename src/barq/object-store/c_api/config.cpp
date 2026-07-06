#include <barq/object-store/c_api/types.hpp>
#include <barq/object-store/c_api/util.hpp>

using namespace barq;
using namespace barq::c_api;

BARQ_API barq_config_t* barq_config_new()
{
    return new barq_config_t{};
}

BARQ_API const char* barq_config_get_path(const barq_config_t* config)
{
    return config->path.c_str();
}

BARQ_API void barq_config_set_path(barq_config_t* config, const char* path)
{
    config->path = path;
}

BARQ_API size_t barq_config_get_encryption_key(const barq_config_t* config, uint8_t* out_key)
{
    if (out_key) {
        std::copy(config->encryption_key.begin(), config->encryption_key.end(), out_key);
    }
    return config->encryption_key.size();
}

BARQ_API bool barq_config_set_encryption_key(barq_config_t* config, const uint8_t* key, size_t key_size)
{
    return wrap_err([=]() {
        if (key_size != 0 && key_size != 64) {
            throw InvalidEncryptionKey();
        }

        config->encryption_key.clear();
        std::copy(key, key + key_size, std::back_inserter(config->encryption_key));
        return true;
    });
}

BARQ_API barq_schema_t* barq_config_get_schema(const barq_config_t* config)
{
    return wrap_err([=]() -> barq_schema_t* {
        if (config->schema) {
            return new barq_schema_t{std::make_unique<Schema>(*config->schema)};
        }
        else {
            return nullptr;
        }
    });
}

BARQ_API void barq_config_set_schema(barq_config_t* config, const barq_schema_t* schema)
{
    if (schema) {
        config->schema = *schema->ptr;
    }
    else {
        config->schema = util::none;
    }
}

BARQ_API uint64_t barq_config_get_schema_version(const barq_config_t* config)
{
    return config->schema_version;
}

BARQ_API void barq_config_set_schema_version(barq_config_t* config, uint64_t version)
{
    config->schema_version = version;
}

BARQ_API barq_schema_mode_e barq_config_get_schema_mode(const barq_config_t* config)
{
    return to_capi(config->schema_mode);
}

BARQ_API void barq_config_set_schema_mode(barq_config_t* config, barq_schema_mode_e mode)
{
    config->schema_mode = from_capi(mode);
}

BARQ_API barq_schema_subset_mode_e barq_config_get_schema_subset_mode(const barq_config_t* config)
{
    return to_capi(config->schema_subset_mode);
}

BARQ_API void barq_config_set_schema_subset_mode(barq_config_t* config, barq_schema_subset_mode_e subset_mode)
{
    config->schema_subset_mode = from_capi(subset_mode);
}

BARQ_API void barq_config_set_migration_function(barq_config_t* config, barq_migration_func_t func,
                                                 barq_userdata_t userdata, barq_free_userdata_func_t callback)
{
    if (func) {
        auto migration_func = [=](SharedBarq old_barq, SharedBarq new_barq, Schema& schema) {
            barq_t r1{old_barq};
            barq_t r2{new_barq};
            barq_schema_t sch{&schema};
            if (!(func)(userdata, &r1, &r2, &sch)) {
                throw CallbackFailed{ErrorStorage::get_thread_local()->get_and_clear_user_code_error()};
            }
        };
        config->migration_function = std::move(migration_func);
    }
    else {
        config->migration_function = nullptr;
    }
    if (callback) {
        config->free_functions.emplace(userdata, callback);
    }
}

BARQ_API void barq_config_set_data_initialization_function(barq_config_t* config,
                                                           barq_data_initialization_func_t func,
                                                           barq_userdata_t userdata,
                                                           barq_free_userdata_func_t callback)
{
    if (func) {
        auto init_func = [=](SharedBarq barq) {
            barq_t r{barq};
            if (!(func)(userdata, &r)) {
                throw CallbackFailed{ErrorStorage::get_thread_local()->get_and_clear_user_code_error()};
            }
        };
        config->initialization_function = std::move(init_func);
    }
    else {
        config->initialization_function = nullptr;
    }
    if (callback) {
        config->free_functions.emplace(userdata, callback);
    }
}

BARQ_API void barq_config_set_should_compact_on_launch_function(barq_config_t* config,
                                                                barq_should_compact_on_launch_func_t func,
                                                                barq_userdata_t userdata,
                                                                barq_free_userdata_func_t callback)
{
    if (func) {
        auto should_func = [=](uint64_t total_bytes, uint64_t used_bytes) -> bool {
            auto result = func(userdata, total_bytes, used_bytes);
            if (auto user_code_error = ErrorStorage::get_thread_local()->get_and_clear_user_code_error())
                throw CallbackFailed{user_code_error};
            return result;
        };
        config->should_compact_on_launch_function = std::move(should_func);
    }
    else {
        config->should_compact_on_launch_function = nullptr;
    }
    if (callback) {
        config->free_functions.emplace(userdata, callback);
    }
}

BARQ_API bool barq_config_get_disable_format_upgrade(const barq_config_t* config)
{
    return config->disable_format_upgrade;
}

BARQ_API bool barq_config_needs_file_format_upgrade(const barq_config_t* config)
{
    return config->needs_file_format_upgrade();
}

BARQ_API void barq_config_set_disable_format_upgrade(barq_config_t* config, bool b)
{
    config->disable_format_upgrade = b;
}

BARQ_API bool barq_config_get_force_sync_history(const barq_config_t* config)
{
    return config->force_sync_history;
}

BARQ_API void barq_config_set_force_sync_history(barq_config_t* config, bool b)
{
    config->force_sync_history = b;
}

BARQ_API bool barq_config_get_automatic_change_notifications(const barq_config_t* config)
{
    return config->automatic_change_notifications;
}

BARQ_API void barq_config_set_automatic_change_notifications(barq_config_t* config, bool b)
{
    config->automatic_change_notifications = b;
}

BARQ_API void barq_config_set_scheduler(barq_config_t* config, const barq_scheduler_t* scheduler)
{
    config->scheduler = *scheduler;
}

BARQ_API uint64_t barq_config_get_max_number_of_active_versions(const barq_config_t* config)
{
    return uint64_t(config->max_number_of_active_versions);
}

BARQ_API void barq_config_set_max_number_of_active_versions(barq_config_t* config, uint64_t n)
{
    config->max_number_of_active_versions = uint_fast64_t(n);
}

BARQ_API void barq_config_set_in_memory(barq_config_t* barq_config, bool value) noexcept
{
    barq_config->in_memory = value;
}

BARQ_API bool barq_config_get_in_memory(barq_config_t* barq_config) noexcept
{
    return barq_config->in_memory;
}

BARQ_API void barq_config_set_fifo_path(barq_config_t* barq_config, const char* fifo_path)
{
    barq_config->fifo_files_fallback_path = fifo_path;
}

BARQ_API const char* barq_config_get_fifo_path(barq_config_t* barq_config) noexcept
{
    return barq_config->fifo_files_fallback_path.c_str();
}

BARQ_API void barq_config_set_cached(barq_config_t* barq_config, bool cached) noexcept
{
    barq_config->cache = cached;
}

BARQ_API bool barq_config_get_cached(barq_config_t* barq_config) noexcept
{
    return barq_config->cache;
}

BARQ_API void barq_config_set_automatic_backlink_handling(barq_config_t* barq_config,
                                                          bool enable_automatic_handling) noexcept
{
    barq_config->automatically_handle_backlinks_in_migrations = enable_automatic_handling;
}
