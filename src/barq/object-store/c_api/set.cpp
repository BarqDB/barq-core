#include <barq/object-store/c_api/util.hpp>

namespace barq::c_api {

BARQ_API bool barq_set_size(const barq_set_t* set, size_t* out_size)
{
    return wrap_err([&]() {
        size_t size = set->size();
        if (out_size)
            *out_size = size;
        return true;
    });
}

BARQ_API bool barq_set_get_property(const barq_set_t* set, barq_property_info_t* out_property_info)
{
    static_cast<void>(set);
    static_cast<void>(out_property_info);
    BARQ_TERMINATE("Not implemented yet");
}

BARQ_API bool barq_set_get(const barq_set_t* set, size_t index, barq_value_t* out_value)
{
    return wrap_err([&]() {
        set->verify_attached();

        auto val = set->get_any(index);
        if (out_value) {
            *out_value = to_capi(val);
        }

        return true;
    });
}

BARQ_API bool barq_set_find(const barq_set_t* set, barq_value_t value, size_t* out_index, bool* out_found)
{
    return wrap_err([&]() {
        set->verify_attached();

        auto val = from_capi(value);

        // FIXME: Check this without try-catch.
        try {
            check_value_assignable(*set, val);
        }
        catch (const NotNullable&) {
            if (out_index)
                *out_index = barq::not_found;
            if (out_found)
                *out_found = false;
            return true;
        }
        catch (const PropertyTypeMismatch&) {
            if (out_index)
                *out_index = barq::not_found;
            if (out_found)
                *out_found = false;
            return true;
        }

        auto index = set->find_any(val);
        if (out_index)
            *out_index = index;
        if (out_found)
            *out_found = index < set->size();
        return true;
    });
}

BARQ_API bool barq_set_insert(barq_set_t* set, barq_value_t value, size_t* out_index, bool* out_inserted)
{
    return wrap_err([&]() {
        auto val = from_capi(value);
        check_value_assignable(*set, val);

        auto [index, inserted] = set->insert_any(val);
        if (out_index)
            *out_index = index;
        if (out_inserted)
            *out_inserted = inserted;
        return true;
    });
}

BARQ_API bool barq_set_erase(barq_set_t* set, barq_value_t value, bool* out_erased)
{
    return wrap_err([&]() {
        auto val = from_capi(value);

        // FIXME: Check this without try-catch.
        try {
            check_value_assignable(*set, val);
        }
        catch (const NotNullable&) {
            if (out_erased)
                *out_erased = false;
            return true;
        }
        catch (const PropertyTypeMismatch&) {
            if (out_erased)
                *out_erased = false;
            return true;
        }
        auto [index, erased] = set->remove_any(val);
        if (out_erased)
            *out_erased = erased;

        return true;
    });
}

BARQ_API bool barq_set_clear(barq_set_t* set)
{
    return wrap_err([&]() {
        // Note: Confusing naming.
        set->remove_all();
        return true;
    });
}

BARQ_API bool barq_set_remove_all(barq_set_t* set)
{
    return wrap_err([&]() {
        // Note: Confusing naming.
        set->delete_all();
        return true;
    });
}

BARQ_API barq_set_t* barq_set_from_thread_safe_reference(const barq_t* barq, barq_thread_safe_reference_t* tsr)
{
    return wrap_err([&]() {
        auto stsr = dynamic_cast<barq_set::thread_safe_reference*>(tsr);
        if (!stsr) {
            throw LogicError{ErrorCodes::IllegalOperation, "Thread safe reference type mismatch"};
        }

        auto set = stsr->resolve<object_store::Set>(*barq);
        return new barq_set_t{std::move(set)};
    });
}

BARQ_API bool barq_set_resolve_in(const barq_set_t* from_set, const barq_t* target_barq, barq_set_t** resolved)
{
    return wrap_err([&]() {
        try {
            const auto& barq = *target_barq;
            auto frozen_set = from_set->freeze(barq);
            if (frozen_set.is_valid()) {
                *resolved = new barq_set_t{std::move(frozen_set)};
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

BARQ_API bool barq_set_is_valid(const barq_set_t* set)
{
    if (!set)
        return false;
    return set->is_valid();
}


} // namespace barq::c_api
