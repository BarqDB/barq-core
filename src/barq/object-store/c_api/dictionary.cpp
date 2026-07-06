#include "barq/object-store/c_api/types.hpp"
#include "barq/object-store/dictionary.hpp"
#include <barq/object-store/c_api/util.hpp>

namespace barq::c_api {

BARQ_API bool barq_dictionary_size(const barq_dictionary_t* dict, size_t* out_size)
{
    return wrap_err([&]() {
        size_t size = dict->size();
        if (out_size)
            *out_size = size;
        return true;
    });
}

BARQ_API bool barq_dictionary_get_property(const barq_dictionary_t* dict, barq_property_info_t* out_property_info)
{
    static_cast<void>(dict);
    static_cast<void>(out_property_info);
    BARQ_TERMINATE("Not implemented yet.");
}

BARQ_API bool barq_dictionary_find(const barq_dictionary_t* dict, barq_value_t key, barq_value_t* out_value,
                                   bool* out_found)
{
    if (key.type != BARQ_TYPE_STRING) {
        if (out_found)
            *out_found = false;
        return true;
    }

    return wrap_err([&]() {
        dict->verify_attached();
        StringData k{key.string.data, key.string.size};
        auto val = dict->try_get_any(k);
        if (!val) {
            if (out_found)
                *out_found = false;
        }
        else {
            if (out_value)
                *out_value = to_capi(*val);
            if (out_found)
                *out_found = true;
        }
        return true;
    });
}

BARQ_API bool barq_dictionary_get(const barq_dictionary_t* dict, size_t index, barq_value_t* out_key,
                                  barq_value_t* out_value)
{
    return wrap_err([&]() {
        dict->verify_attached();
        auto [key, value] = dict->get_pair(index);
        if (out_key) {
            out_key->type = BARQ_TYPE_STRING;
            out_key->string = to_capi(key);
        }
        if (out_value)
            *out_value = to_capi(value);
        return true;
    });
}

BARQ_API bool barq_dictionary_insert(barq_dictionary_t* dict, barq_value_t key, barq_value_t value,
                                     size_t* out_index, bool* out_inserted)
{
    return wrap_err([&]() {
        if (key.type != BARQ_TYPE_STRING) {
            throw InvalidArgument{"Only string keys are supported in dictionaries"};
        }

        StringData k{key.string.data, key.string.size};
        auto val = from_capi(value);
        check_value_assignable(*dict, val);
        auto [index, inserted] = dict->insert_any(k, val);

        if (out_index)
            *out_index = index;
        if (out_inserted)
            *out_inserted = inserted;

        return true;
    });
}

BARQ_API barq_object_t* barq_dictionary_insert_embedded(barq_dictionary_t* dict, barq_value_t key)
{
    return wrap_err([&]() {
        if (key.type != BARQ_TYPE_STRING) {
            throw InvalidArgument{"Only string keys are supported in dictionaries"};
        }

        StringData k{key.string.data, key.string.size};
        return new barq_object_t({dict->get_barq(), dict->insert_embedded(k)});
    });
}

BARQ_API barq_list_t* barq_dictionary_insert_list(barq_dictionary_t* dictionary, barq_value_t key)
{
    return wrap_err([&]() {
        if (key.type != BARQ_TYPE_STRING) {
            throw InvalidArgument{"Only string keys are supported in dictionaries"};
        }

        StringData k{key.string.data, key.string.size};
        dictionary->insert_collection(k, CollectionType::List);
        return new barq_list_t{dictionary->get_list(k)};
    });
}

BARQ_API barq_dictionary_t* barq_dictionary_insert_dictionary(barq_dictionary_t* dictionary, barq_value_t key)
{
    return wrap_err([&]() {
        if (key.type != BARQ_TYPE_STRING) {
            throw InvalidArgument{"Only string keys are supported in dictionaries"};
        }

        StringData k{key.string.data, key.string.size};
        dictionary->insert_collection(k, CollectionType::Dictionary);
        return new barq_dictionary_t{dictionary->get_dictionary(k)};
    });
}


BARQ_API barq_list_t* barq_dictionary_get_list(barq_dictionary_t* dictionary, barq_value_t key)
{
    return wrap_err([&]() {
        if (key.type != BARQ_TYPE_STRING) {
            throw InvalidArgument{"Only string keys are supported in dictionaries"};
        }

        StringData k{key.string.data, key.string.size};
        return new barq_list_t{dictionary->get_list(k)};
    });
}

BARQ_API barq_dictionary_t* barq_dictionary_get_dictionary(barq_dictionary_t* dictionary, barq_value_t key)
{
    return wrap_err([&]() {
        if (key.type != BARQ_TYPE_STRING) {
            throw InvalidArgument{"Only string keys are supported in dictionaries"};
        }

        StringData k{key.string.data, key.string.size};
        return new barq_dictionary_t{dictionary->get_dictionary(k)};
    });
}

BARQ_API barq_object_t* barq_dictionary_get_linked_object(barq_dictionary_t* dict, barq_value_t key)
{
    return wrap_err([&]() {
        if (key.type != BARQ_TYPE_STRING) {
            throw InvalidArgument{"Only string keys are supported in dictionaries"};
        }

        StringData k{key.string.data, key.string.size};
        auto o = dict->get_object(k);
        return o ? new barq_object_t({dict->get_barq(), o}) : nullptr;
    });
}

BARQ_API bool barq_dictionary_erase(barq_dictionary_t* dict, barq_value_t key, bool* out_erased)
{
    return wrap_err([&]() {
        bool erased = false;
        if (key.type == BARQ_TYPE_STRING) {
            StringData k{key.string.data, key.string.size};
            erased = dict->try_erase(k);
        }

        if (out_erased)
            *out_erased = erased;
        return true;
    });
}

BARQ_API bool barq_dictionary_get_keys(barq_dictionary_t* dict, size_t* out_size, barq_results_t** out_keys)
{
    return wrap_err([&]() {
        auto keys = dict->get_keys();
        *out_size = keys.size();
        *out_keys = new barq_results_t{keys};
        return true;
    });
}

BARQ_API bool barq_dictionary_contains_key(const barq_dictionary_t* dict, barq_value_t key, bool* found)
{
    return wrap_err([&]() {
        StringData k{key.string.data, key.string.size};
        *found = dict->contains(k);
        return true;
    });
}

BARQ_API bool barq_dictionary_contains_value(const barq_dictionary_t* dict, barq_value_t value, size_t* index)
{
    return wrap_err([&]() {
        auto val = from_capi(value);
        *index = dict->find_any(val);
        return true;
    });
}

BARQ_API bool barq_dictionary_clear(barq_dictionary_t* dict)
{
    return wrap_err([&]() {
        // Note: confusing naming.
        dict->remove_all();
        return true;
    });
}

BARQ_API barq_dictionary_t* barq_dictionary_from_thread_safe_reference(const barq_t* barq,
                                                                        barq_thread_safe_reference_t* tsr)
{
    return wrap_err([&]() {
        auto stsr = dynamic_cast<barq_dictionary::thread_safe_reference*>(tsr);
        if (!stsr) {
            throw LogicError{ErrorCodes::IllegalOperation, "Thread safe reference type mismatch"};
        }

        auto dict = stsr->resolve<object_store::Dictionary>(*barq);
        return new barq_dictionary_t{std::move(dict)};
    });
}

BARQ_API bool barq_dictionary_resolve_in(const barq_dictionary_t* from_dictionary, const barq_t* target_barq,
                                         barq_dictionary_t** resolved)
{
    return wrap_err([&]() {
        try {
            const auto& barq = *target_barq;
            auto frozen_dictionary = from_dictionary->freeze(barq);
            if (frozen_dictionary.is_valid()) {
                *resolved = new barq_dictionary_t{std::move(frozen_dictionary)};
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

BARQ_API bool barq_dictionary_is_valid(const barq_dictionary_t* dictionary)
{
    if (!dictionary)
        return false;
    return dictionary->is_valid();
}

} // namespace barq::c_api
