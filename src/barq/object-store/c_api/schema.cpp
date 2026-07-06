#include <barq/object-store/c_api/types.hpp>
#include <barq/object-store/c_api/util.hpp>
#include "barq.hpp"

namespace barq::c_api {

BARQ_API barq_schema_t* barq_schema_new(const barq_class_info_t* classes, size_t num_classes,
                                         const barq_property_info_t** class_properties)
{
    return wrap_err([&]() {
        std::vector<ObjectSchema> object_schemas;
        object_schemas.reserve(num_classes);

        for (size_t i = 0; i < num_classes; ++i) {
            const auto& class_info = classes[i];
            auto props_ptr = class_properties[i];
            auto computed_props_ptr = props_ptr + class_info.num_properties;

            ObjectSchema object_schema;
            object_schema.name = class_info.name;
            object_schema.primary_key = class_info.primary_key;
            object_schema.table_type = static_cast<ObjectSchema::ObjectType>(class_info.flags & BARQ_CLASS_MASK);
            object_schema.persisted_properties.reserve(class_info.num_properties);
            object_schema.computed_properties.reserve(class_info.num_computed_properties);

            for (size_t j = 0; j < class_info.num_properties; ++j) {
                Property prop = from_capi(props_ptr[j]);
                object_schema.persisted_properties.push_back(std::move(prop));
            }

            for (size_t j = 0; j < class_info.num_computed_properties; ++j) {
                Property prop = from_capi(computed_props_ptr[j]);
                object_schema.computed_properties.push_back(std::move(prop));
            }

            object_schemas.push_back(std::move(object_schema));
        }

        auto schema = new barq_schema(std::make_unique<Schema>(std::move(object_schemas)));
        return schema;
    });
}

BARQ_API barq_schema_t* barq_get_schema(const barq_t* barq)
{
    return wrap_err([&]() {
        auto& shared_barq = *barq;
        return new barq_schema_t{&shared_barq->schema()};
    });
}

BARQ_API uint64_t barq_get_schema_version(const barq_t* barq)
{
    auto& shared_barq = *barq;
    return shared_barq->schema_version();
}

BARQ_API uint64_t barq_get_persisted_schema_version(const barq_config_t* config)
{
    auto conf = BarqConfig();
    conf.schema_version = ObjectStore::NotVersioned;
    conf.path = config->path;
    conf.encryption_key = config->encryption_key;

    if (config->sync_config) {
        conf.sync_config = nullptr;
        conf.force_sync_history = true;
    }

    auto barq = Barq::get_shared_barq(conf);
    uint64_t version = ObjectStore::get_schema_version(barq->read_group());
    return version;
}

BARQ_API bool barq_schema_validate(const barq_schema_t* schema, uint64_t validation_mode)
{
    return wrap_err([&]() {
        schema->ptr->validate(static_cast<SchemaValidationMode>(validation_mode));
        return true;
    });
}

BARQ_API bool barq_update_schema(barq_t* barq, const barq_schema_t* schema)
{
    return wrap_err([&]() {
        barq->get()->update_schema(*schema->ptr);
        return true;
    });
}

BARQ_API bool barq_schema_rename_property(barq_t* barq, barq_schema_t* schema, const char* object_type,
                                          const char* old_name, const char* new_name)
{
    return wrap_err([&]() {
        barq->get()->rename_property(*schema->ptr, object_type, old_name, new_name);
        return true;
    });
}

BARQ_API size_t barq_get_num_classes(const barq_t* barq)
{
    size_t max = std::numeric_limits<size_t>::max();
    size_t n = 0;
    auto success = barq_get_class_keys(barq, nullptr, max, &n);
    BARQ_ASSERT(success);
    return n;
}

BARQ_API bool barq_get_class_keys(const barq_t* barq, barq_class_key_t* out_keys, size_t max, size_t* out_n)
{
    return wrap_err([&]() {
        const auto& shared_barq = **barq;
        const auto& schema = shared_barq.schema();
        set_out_param(out_n, schema.size());

        if (out_keys && max >= schema.size()) {
            size_t i = 0;
            for (auto& os : schema) {
                out_keys[i++] = os.table_key.value;
            }
        }
        return true;
    });
}

BARQ_API bool barq_find_class(const barq_t* barq, const char* name, bool* out_found,
                              barq_class_info_t* out_class_info)
{
    return wrap_err([&]() {
        const auto& schema = (*barq)->schema();
        auto it = schema.find(name);
        if (it != schema.end()) {
            if (out_found)
                *out_found = true;
            if (out_class_info)
                *out_class_info = to_capi(*it);
        }
        else {
            if (out_found)
                *out_found = false;
        }
        return true;
    });
}

BARQ_API bool barq_get_class(const barq_t* barq, barq_class_key_t key, barq_class_info_t* out_class_info)
{
    return wrap_err([&]() {
        auto& os = schema_for_table(*barq, TableKey(key));
        *out_class_info = to_capi(os);
        return true;
    });
}

BARQ_API bool barq_get_class_properties(const barq_t* barq, barq_class_key_t key,
                                        barq_property_info_t* out_properties, size_t max, size_t* out_n)
{
    return wrap_err([&]() {
        auto& os = schema_for_table(*barq, TableKey(key));
        const size_t prop_size = os.persisted_properties.size() + os.computed_properties.size();
        set_out_param(out_n, prop_size);

        if (out_properties && max >= prop_size) {
            size_t i = 0;
            for (auto& prop : os.persisted_properties) {
                out_properties[i++] = to_capi(prop);
            }
            for (auto& prop : os.computed_properties) {
                out_properties[i++] = to_capi(prop);
            }
        }
        return true;
    });
}

BARQ_API bool barq_get_property_keys(const barq_t* barq, barq_class_key_t key, barq_property_key_t* out_keys,
                                     size_t max, size_t* out_n)
{
    return wrap_err([&]() {
        auto& os = schema_for_table(*barq, TableKey(key));
        const size_t prop_size = os.persisted_properties.size() + os.computed_properties.size();
        set_out_param(out_n, prop_size);
        if (out_keys && max >= prop_size) {
            size_t i = 0;
            for (auto& prop : os.persisted_properties) {
                out_keys[i++] = prop.column_key.value;
            }
            for (auto& prop : os.computed_properties) {
                out_keys[i++] = prop.column_key.value;
            }
        }
        return true;
    });
}

BARQ_API bool barq_get_value_by_property_index(const barq_object_t* object, size_t prop_index,
                                               barq_value_t* out_value)
{
    return wrap_err([&] {
        object->verify_attached();
        auto& peristed_properties = object->get_object_schema().persisted_properties;
        BARQ_ASSERT(prop_index < peristed_properties.size());
        auto col_key = peristed_properties[prop_index].column_key;
        auto o = object->get_obj();
        auto val = o.get_any(col_key);
        auto converted = objkey_to_typed_link(val, col_key, *o.get_table());
        *out_value = to_capi(converted);
        return true;
    });
}

BARQ_API bool barq_get_property(const barq_t* barq, barq_class_key_t class_key, barq_property_key_t key,
                                barq_property_info_t* out_property_info)
{
    return wrap_err([&]() {
        auto& os = schema_for_table(*barq, TableKey(class_key));
        auto col_key = ColKey(key);

        for (auto& prop : os.persisted_properties) {
            if (prop.column_key == col_key) {
                *out_property_info = to_capi(prop);
                return true;
            }
        }

        for (auto& prop : os.computed_properties) {
            if (prop.column_key == col_key) {
                *out_property_info = to_capi(prop);
                return true;
            }
        }

        auto& shared_barq = *barq;
        throw InvalidColumnKey{shared_barq->read_group().get_class_name(TableKey(class_key))};
    });
}

BARQ_API bool barq_find_property(const barq_t* barq, barq_class_key_t class_key, const char* name, bool* out_found,
                                 barq_property_info_t* out_property_info)
{
    return wrap_err([&]() {
        auto& os = schema_for_table(*barq, TableKey(class_key));
        auto prop = os.property_for_name(name);

        if (prop) {
            if (out_found)
                *out_found = true;
            if (out_property_info)
                *out_property_info = to_capi(*prop);
        }
        else {
            if (out_found)
                *out_found = false;
        }

        return true;
    });
}

BARQ_API bool barq_find_property_by_public_name(const barq_t* barq, barq_class_key_t class_key,
                                                const char* public_name, bool* out_found,
                                                barq_property_info_t* out_property_info)
{
    return wrap_err([&]() {
        auto& os = schema_for_table(*barq, TableKey(class_key));
        auto prop = os.property_for_public_name(public_name);

        if (prop) {
            if (out_found)
                *out_found = true;
            if (out_property_info)
                *out_property_info = to_capi(*prop);
        }
        else {
            if (out_found)
                *out_found = false;
        }

        return true;
    });
}

BARQ_API barq_callback_token_t* barq_add_schema_changed_callback(barq_t* barq,
                                                                  barq_on_schema_change_func_t callback,
                                                                  barq_userdata_t userdata,
                                                                  barq_free_userdata_func_t free_userdata)
{
    util::UniqueFunction<void(const Schema&)> func =
        [callback, userdata = UserdataPtr{userdata, free_userdata}](const Schema& schema) {
            auto c_schema = new barq_schema_t(&schema);
            callback(userdata.get(), c_schema);
            barq_release(c_schema);
        };
    return new barq_callback_token_schema(
        barq, CBindingContext::get(*barq).schema_changed_callbacks().add(std::move(func)));
}

} // namespace barq::c_api
