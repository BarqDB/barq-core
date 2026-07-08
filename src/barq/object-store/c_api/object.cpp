#include <barq/object-store/c_api/types.hpp>
#include <barq/object-store/c_api/util.hpp>

#include <barq/util/overload.hpp>

namespace barq::c_api {

BARQ_API bool barq_decimal128_from_string(const char* value, barq_decimal128_t* out_decimal)
{
    return wrap_err([&]() {
        if (!value || !out_decimal) {
            return false;
        }
        *out_decimal = to_capi(Decimal128(std::string(value)));
        return true;
    });
}

BARQ_API char* barq_decimal128_to_string(barq_decimal128_t value)
{
    return wrap_err([&]() {
        return duplicate_string(from_capi(value).to_string());
    });
}

BARQ_API bool barq_get_num_objects(const barq_t* barq, barq_class_key_t key, size_t* out_count)
{
    return wrap_err([&]() {
        auto& shared_barq = **barq;
        auto table = shared_barq.read_group().get_table(TableKey(key));
        if (out_count)
            *out_count = table->size();
        return true;
    });
}

BARQ_API barq_object_t* barq_get_object(const barq_t* barq, barq_class_key_t tbl_key, barq_object_key_t obj_key)
{
    return wrap_err([&]() {
        auto& shared_barq = *barq;
        auto table_key = TableKey(tbl_key);
        auto table = shared_barq->read_group().get_table(table_key);
        auto obj = table->get_object(ObjKey(obj_key));
        auto object = Object{shared_barq, std::move(obj)};
        return new barq_object_t{std::move(object)};
    });
}

BARQ_API bool barq_object_get_parent(const barq_object_t* object, barq_object_t** parent,
                                     barq_class_key_t* class_key)
{
    return wrap_err([&]() {
        const auto& obj = object->get_obj().get_parent_object();
        if (class_key)
            *class_key = obj.get_table()->get_key().value;

        if (parent)
            *parent = new barq_object_t{Object{object->barq(), std::move(obj)}};

        return true;
    });
}


BARQ_API barq_object_t* barq_object_find_with_primary_key(const barq_t* barq, barq_class_key_t class_key,
                                                           barq_value_t pk, bool* out_found)
{
    return wrap_err([&]() -> barq_object_t* {
        auto& shared_barq = *barq;
        auto table_key = TableKey(class_key);
        auto table = shared_barq->read_group().get_table(table_key);
        auto pk_val = from_capi(pk);

        auto pk_col = table->get_primary_key_column();
        if (pk_val.is_null() && !pk_col.is_nullable()) {
            if (out_found)
                *out_found = false;
            return nullptr;
        }
        if (!pk_val.is_null() && ColumnType(pk_val.get_type()) != pk_col.get_type() &&
            pk_col.get_type() != col_type_Mixed) {
            if (out_found)
                *out_found = false;
            return nullptr;
        }

        auto obj_key = table->find_primary_key(pk_val);
        if (obj_key) {
            if (out_found)
                *out_found = true;
            auto obj = table->get_object(obj_key);
            return new barq_object_t{Object{shared_barq, std::move(obj)}};
        }
        else {
            if (out_found)
                *out_found = false;
            return static_cast<barq_object_t*>(nullptr);
        }
    });
}

BARQ_API barq_results_t* barq_object_find_all(const barq_t* barq, barq_class_key_t key)
{
    return wrap_err([&]() {
        auto& shared_barq = *barq;
        auto table = shared_barq->read_group().get_table(TableKey(key));
        return new barq_results{Results{shared_barq, table}};
    });
}

BARQ_API barq_object_t* barq_object_create(barq_t* barq, barq_class_key_t table_key)
{
    return wrap_err([&]() {
        auto& shared_barq = *barq;
        auto tblkey = TableKey(table_key);
        auto table = shared_barq->read_group().get_table(tblkey);

        if (table->get_primary_key_column()) {
            auto& object_schema = schema_for_table(*barq, tblkey);
            throw MissingPrimaryKeyException{object_schema.name};
        }

        auto obj = table->create_object();
        auto object = Object{shared_barq, std::move(obj)};
        return new barq_object_t{std::move(object)};
    });
}

BARQ_API barq_object_t* barq_object_create_with_primary_key(barq_t* barq, barq_class_key_t table_key,
                                                             barq_value_t pk)
{
    bool did_create;
    barq_object_t* object = barq_object_get_or_create_with_primary_key(barq, table_key, pk, &did_create);
    if (object && !did_create) {
        delete object;
        object = wrap_err([&]() {
            auto& shared_barq = *barq;
            throw ObjectAlreadyExists(shared_barq->read_group().get_class_name(TableKey(table_key)), from_capi(pk));
            return nullptr;
        });
    }
    return object;
}

BARQ_API barq_object_t* barq_object_get_or_create_with_primary_key(barq_t* barq, barq_class_key_t table_key,
                                                                    barq_value_t pk, bool* did_create)
{
    return wrap_err([&]() {
        auto& shared_barq = *barq;
        auto tblkey = TableKey(table_key);
        auto table = shared_barq->read_group().get_table(tblkey);
        auto pkval = from_capi(pk);
        if (did_create)
            *did_create = false;

        auto obj = table->create_object_with_primary_key(pkval, did_create);
        auto object = Object{shared_barq, std::move(obj)};
        return new barq_object_t{std::move(object)};
    });
}

BARQ_API bool barq_object_delete(barq_object_t* obj)
{
    return wrap_err([&]() {
        obj->verify_attached();
        obj->get_obj().remove();
        return true;
    });
}

BARQ_API barq_object_t* _barq_object_from_native_copy(const void* pobj, size_t n)
{
    BARQ_ASSERT_RELEASE(n == sizeof(Object));

    return wrap_err([&]() {
        auto pobject = static_cast<const Object*>(pobj);
        return new barq_object_t{*pobject};
    });
}

BARQ_API barq_object_t* _barq_object_from_native_move(void* pobj, size_t n)
{
    BARQ_ASSERT_RELEASE(n == sizeof(Object));

    return wrap_err([&]() {
        auto pobject = static_cast<Object*>(pobj);
        return new barq_object_t{std::move(*pobject)};
    });
}

BARQ_API const void* _barq_object_get_native_ptr(barq_object_t* obj)
{
    return static_cast<const Object*>(obj);
}

BARQ_API bool barq_object_resolve_in(const barq_object_t* from_object, const barq_t* target_barq,
                                     barq_object_t** resolved)
{
    return wrap_err([&]() {
        try {
            const auto& barq = *target_barq;
            auto new_obj = from_object->freeze(barq);
            // clients of the C-API adhere to a different error handling strategy than Core.
            // Core represents lack of resolution as a new object which is invalid.
            // But clients of the C-API instead wants NO object to be produced.
            if (new_obj.is_valid()) {
                *resolved = new barq_object_t{std::move(new_obj)};
            }
            else {
                *resolved = nullptr;
            }
            return true;
        }
        catch (NoSuchTable&) {
            *resolved = nullptr;
            return true;
        }
        catch (KeyNotFound&) {
            *resolved = nullptr;
            return true;
        }
    });
}

BARQ_API bool barq_object_add_int(barq_object_t* object, barq_property_key_t property_key, int64_t value)
{
    BARQ_ASSERT(object);
    return wrap_err([&]() {
        object->verify_attached();
        object->get_obj().add_int(ColKey(property_key), value);
        return true;
    });
}


BARQ_API bool barq_object_is_valid(const barq_object_t* obj)
{
    return obj->is_valid();
}

BARQ_API barq_object_key_t barq_object_get_key(const barq_object_t* obj)
{
    return obj->get_obj().get_key().value;
}

BARQ_API barq_class_key_t barq_object_get_table(const barq_object_t* obj)
{
    return obj->get_obj().get_table()->get_key().value;
}

BARQ_API barq_link_t barq_object_as_link(const barq_object_t* object)
{
    const auto& obj = object->get_obj();
    auto table = obj.get_table();
    auto table_key = table->get_key();
    auto obj_key = obj.get_key();
    return barq_link_t{table_key.value, obj_key.value};
}

BARQ_API barq_object_t* barq_object_from_thread_safe_reference(const barq_t* barq,
                                                                barq_thread_safe_reference_t* tsr)
{
    return wrap_err([&]() {
        auto otsr = dynamic_cast<barq_object::thread_safe_reference*>(tsr);
        if (!otsr) {
            throw LogicError{ErrorCodes::IllegalOperation, "Thread safe reference type mismatch"};
        }

        auto obj = otsr->resolve<Object>(*barq);
        return new barq_object_t{std::move(obj)};
    });
}

BARQ_API bool barq_get_value(const barq_object_t* obj, barq_property_key_t col, barq_value_t* out_value)
{
    return barq_get_values(obj, 1, &col, out_value);
}

BARQ_API bool barq_get_values(const barq_object_t* obj, size_t num_values, const barq_property_key_t* properties,
                              barq_value_t* out_values)
{
    return wrap_err([&]() {
        obj->verify_attached();

        auto o = obj->get_obj();

        for (size_t i = 0; i < num_values; ++i) {
            auto col_key = ColKey(properties[i]);

            if (col_key.is_collection()) {
                auto table = o.get_table();
                auto& schema = schema_for_table(obj->get_barq(), table->get_key());
                throw PropertyTypeMismatch{schema.name, table->get_column_name(col_key)};
            }

            auto val = o.get_any(col_key);
            if (out_values) {
                auto converted = objkey_to_typed_link(val, col_key, *o.get_table());
                out_values[i] = to_capi(converted);
            }
        }

        return true;
    });
}

BARQ_API bool barq_set_value(barq_object_t* obj, barq_property_key_t col, barq_value_t new_value, bool is_default)
{
    return barq_set_values(obj, 1, &col, &new_value, is_default);
}

BARQ_API bool barq_set_values(barq_object_t* obj, size_t num_values, const barq_property_key_t* properties,
                              const barq_value_t* values, bool is_default)
{
    return wrap_err([&]() {
        obj->verify_attached();
        auto o = obj->get_obj();
        auto table = o.get_table();

        // Perform validation up front to avoid partial updates. This is
        // unlikely to incur performance overhead because the object itself is
        // not accessed here, just the bits of the column key and the input type.

        for (size_t i = 0; i < num_values; ++i) {
            auto col_key = ColKey(properties[i]);
            table->check_column(col_key);

            if (col_key.is_collection()) {
                auto& schema = schema_for_table(obj->get_barq(), table->get_key());
                throw PropertyTypeMismatch{schema.name, table->get_column_name(col_key)};
            }

            auto val = from_capi(values[i]);
            check_value_assignable(obj->get_barq(), *table, col_key, val);
        }

        // Actually write the properties.

        for (size_t i = 0; i < num_values; ++i) {
            auto col_key = ColKey(properties[i]);
            auto val = from_capi(values[i]);
            o.set_any(col_key, val, is_default);
        }

        return true;
    });
}

BARQ_API bool barq_set_json(barq_object_t* obj, barq_property_key_t col, const char* json_string)
{
    return wrap_err([&]() {
        obj->verify_attached();
        auto o = obj->get_obj();
        ColKey col_key(col);
        if (col_key.get_type() != col_type_Mixed) {
            auto table = o.get_table();
            auto& schema = schema_for_table(obj->get_barq(), table->get_key());
            throw PropertyTypeMismatch{schema.name, table->get_column_name(col_key)};
        }
        o.set_json(ColKey(col), json_string);
        return true;
    });
}


BARQ_API barq_object_t* barq_set_embedded(barq_object_t* obj, barq_property_key_t col)
{
    return wrap_err([&]() {
        obj->verify_attached();
        auto& o = obj->get_obj();
        return new barq_object_t({obj->get_barq(), o.create_and_set_linked_object(ColKey(col))});
    });
}

BARQ_API barq_list_t* barq_set_list(barq_object_t* object, barq_property_key_t col)
{
    return wrap_err([&]() {
        object->verify_attached();

        auto& obj = object->get_obj();
        auto col_key = ColKey(col);

        obj.set_collection(col_key, CollectionType::List);
        return new barq_list_t{List{object->get_barq(), std::move(obj), col_key}};
    });
}

BARQ_API barq_dictionary_t* barq_set_dictionary(barq_object_t* object, barq_property_key_t col)
{
    return wrap_err([&]() {
        object->verify_attached();

        auto& obj = object->get_obj();
        auto col_key = ColKey(col);

        obj.set_collection(col_key, CollectionType::Dictionary);
        return new barq_dictionary_t{object_store::Dictionary{object->get_barq(), obj, col_key}};
    });
}

BARQ_API barq_object_t* barq_get_linked_object(barq_object_t* obj, barq_property_key_t col)
{
    return wrap_err([&]() {
        obj->verify_attached();
        const auto& o = obj->get_obj().get_linked_object(ColKey(col));
        return o ? new barq_object_t({obj->get_barq(), o}) : nullptr;
    });
}

BARQ_API barq_list_t* barq_get_list(barq_object_t* object, barq_property_key_t key)
{
    return wrap_err([&]() {
        object->verify_attached();

        const auto& obj = object->get_obj();
        auto table = obj.get_table();

        auto col_key = ColKey(key);
        table->check_column(col_key);

        if (!(col_key.is_list() || col_key.get_type() == col_type_Mixed)) {
            report_type_mismatch(object->get_barq(), *table, col_key);
        }

        return new barq_list_t{List{object->get_barq(), std::move(obj), col_key}};
    });
}

BARQ_API barq_set_t* barq_get_set(barq_object_t* object, barq_property_key_t key)
{
    return wrap_err([&]() {
        object->verify_attached();

        const auto& obj = object->get_obj();
        auto table = obj.get_table();

        auto col_key = ColKey(key);
        table->check_column(col_key);

        if (!(col_key.is_set() || col_key.get_type() == col_type_Mixed)) {
            report_type_mismatch(object->get_barq(), *table, col_key);
        }

        return new barq_set_t{object_store::Set{object->get_barq(), std::move(obj), col_key}};
    });
}

BARQ_API barq_dictionary_t* barq_get_dictionary(barq_object_t* object, barq_property_key_t key)
{
    return wrap_err([&]() {
        object->verify_attached();

        const auto& obj = object->get_obj();
        auto table = obj.get_table();
        auto col_key = ColKey(key);
        table->check_column(col_key);

        if (!(col_key.is_dictionary() || col_key.get_type() == col_type_Mixed)) {
            report_type_mismatch(object->get_barq(), *table, col_key);
        }

        return new barq_dictionary_t{object_store::Dictionary{object->get_barq(), std::move(obj), col_key}};
    });
}

BARQ_API char* barq_object_to_string(barq_object_t* object)
{
    return wrap_err([&]() {
        object->verify_attached();

        const auto& obj = object->get_obj();
        return duplicate_string(obj.to_string());
    });
}

} // namespace barq::c_api
