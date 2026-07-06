////////////////////////////////////////////////////////////////////////////
//
// Copyright 2022 Realm Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
////////////////////////////////////////////////////////////////////////////

#include <barq/object-store/c_api/types.hpp>
#include <barq/object-store/c_api/util.hpp>

namespace barq::c_api {

BARQ_API bool barq_list_size(const barq_list_t* list, size_t* out_size)
{
    return wrap_err([&]() {
        size_t size = list->size();
        if (out_size)
            *out_size = size;
        return true;
    });
}

BARQ_API bool barq_list_get_property(const barq_list_t* list, barq_property_info_t* out_property_info)
{
    static_cast<void>(list);
    static_cast<void>(out_property_info);
    BARQ_TERMINATE("Not implemented yet.");
}

BARQ_API bool barq_list_get(const barq_list_t* list, size_t index, barq_value_t* out_value)
{
    return wrap_err([&]() {
        list->verify_attached();
        auto mixed = list->get_any(index);

        if (out_value) {
            *out_value = to_capi(mixed);
        }
        return true;
    });
}

BARQ_API bool barq_list_find(const barq_list_t* list, const barq_value_t* value, size_t* out_index, bool* out_found)
{
    if (out_index)
        *out_index = barq::not_found;
    if (out_found)
        *out_found = false;

    return wrap_err([&] {
        list->verify_attached();
        auto val = from_capi(*value);
        check_value_assignable(*list, val);
        auto index = list->find_any(val);
        if (out_index)
            *out_index = index;
        if (out_found)
            *out_found = index < list->size();
        return true;
    });
}


BARQ_API bool barq_list_insert(barq_list_t* list, size_t index, barq_value_t value)
{
    return wrap_err([&]() {
        auto val = from_capi(value);
        check_value_assignable(*list, val);

        list->insert_any(index, val);
        return true;
    });
}

BARQ_API barq_list_t* barq_list_insert_list(barq_list_t* list, size_t index)
{
    return wrap_err([&]() {
        list->insert_collection(index, CollectionType::List);
        return new barq_list_t{list->get_list(index)};
    });
}

BARQ_API barq_dictionary_t* barq_list_insert_dictionary(barq_list_t* list, size_t index)
{
    return wrap_err([&]() {
        list->insert_collection(index, CollectionType::Dictionary);
        return new barq_dictionary_t{list->get_dictionary(index)};
    });
}

BARQ_API barq_list_t* barq_list_set_list(barq_list_t* list, size_t index)
{
    return wrap_err([&]() {
        list->set_collection(index, CollectionType::List);
        return new barq_list_t{list->get_list(index)};
    });
}

BARQ_API barq_dictionary_t* barq_list_set_dictionary(barq_list_t* list, size_t index)
{
    return wrap_err([&]() {
        list->set_collection(index, CollectionType::Dictionary);
        return new barq_dictionary_t{list->get_dictionary(index)};
    });
}


BARQ_API barq_list_t* barq_list_get_list(barq_list_t* list, size_t index)
{
    return wrap_err([&]() {
        return new barq_list_t{list->get_list(index)};
    });
}

BARQ_API barq_dictionary_t* barq_list_get_dictionary(barq_list_t* list, size_t index)
{
    return wrap_err([&]() {
        return new barq_dictionary_t{list->get_dictionary(index)};
    });
}

BARQ_API bool barq_list_move(barq_list_t* list, size_t from_index, size_t to_index)
{
    return wrap_err([&]() {
        list->move(from_index, to_index);
        return true;
    });
}


BARQ_API bool barq_list_set(barq_list_t* list, size_t index, barq_value_t value)
{
    return wrap_err([&]() {
        auto val = from_capi(value);
        check_value_assignable(*list, val);

        list->set_any(index, val);
        return true;
    });
}

BARQ_API barq_object_t* barq_list_insert_embedded(barq_list_t* list, size_t index)
{
    return wrap_err([&]() {
        return new barq_object_t({list->get_barq(), list->insert_embedded(index)});
    });
}

BARQ_API barq_object_t* barq_list_set_embedded(barq_list_t* list, size_t index)
{
    return wrap_err([&]() {
        list->verify_attached();
        return new barq_object_t({list->get_barq(), list->set_embedded(index)});
    });
}

BARQ_API barq_object_t* barq_list_get_linked_object(barq_list_t* list, size_t index)
{
    return wrap_err([&]() {
        list->verify_attached();
        auto o = list->get_object(index);
        return o ? new barq_object_t({list->get_barq(), o}) : nullptr;
    });
}

BARQ_API bool barq_list_erase(barq_list_t* list, size_t index)
{
    return wrap_err([&]() {
        list->remove(index);
        return true;
    });
}

BARQ_API bool barq_list_clear(barq_list_t* list)
{
    return wrap_err([&]() {
        // Note: Confusing naming.
        list->remove_all();
        return true;
    });
}

BARQ_API bool barq_list_remove_all(barq_list_t* list)
{
    return wrap_err([&]() {
        // Note: Confusing naming.
        list->delete_all();
        return true;
    });
}

BARQ_API barq_list_t* barq_list_from_thread_safe_reference(const barq_t* barq, barq_thread_safe_reference_t* tsr)
{
    return wrap_err([&]() {
        auto ltsr = dynamic_cast<barq_list::thread_safe_reference*>(tsr);
        if (!ltsr) {
            throw LogicError{ErrorCodes::IllegalOperation, "Thread safe reference type mismatch"};
        }

        auto list = ltsr->resolve<List>(*barq);
        return new barq_list_t{std::move(list)};
    });
}

BARQ_API bool barq_list_resolve_in(const barq_list_t* from_list, const barq_t* target_barq,
                                   barq_list_t** resolved)
{
    return wrap_err([&]() {
        try {
            const auto& barq = *target_barq;
            auto frozen_list = from_list->freeze(barq);
            if (frozen_list.is_valid()) {
                *resolved = new barq_list_t{std::move(frozen_list)};
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

BARQ_API bool barq_list_is_valid(const barq_list_t* list)
{
    if (!list)
        return false;
    return list->is_valid();
}

} // namespace barq::c_api
