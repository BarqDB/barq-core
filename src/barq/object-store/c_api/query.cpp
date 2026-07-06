#include "barq/sort_descriptor.hpp"
#include "barq/util/scope_exit.hpp"
#include <barq/object-store/c_api/types.hpp>
#include <barq/object-store/c_api/util.hpp>

#include <barq/object-store/keypath_helpers.hpp>
#include <barq/parser/query_parser.hpp>
#include <barq/parser/keypath_mapping.hpp>

namespace barq::c_api {

namespace {
struct QueryArgumentsAdapter : query_parser::Arguments {
    const barq_query_arg_t* m_args = nullptr;

    QueryArgumentsAdapter(size_t num_args, const barq_query_arg_t* args) noexcept
        : Arguments(num_args)
        , m_args(args)
    {
    }

    bool bool_for_argument(size_t i) final
    {
        verify_ndx(i);
        BARQ_ASSERT(m_args[i].arg[0].type == BARQ_TYPE_BOOL);
        return m_args[i].arg[0].boolean;
    }

    long long long_for_argument(size_t i) final
    {
        verify_ndx(i);
        BARQ_ASSERT(m_args[i].arg[0].type == BARQ_TYPE_INT);
        return m_args[i].arg[0].integer;
    }

    float float_for_argument(size_t i) final
    {
        verify_ndx(i);
        BARQ_ASSERT(m_args[i].arg[0].type == BARQ_TYPE_FLOAT);
        return m_args[i].arg[0].fnum;
    }

    double double_for_argument(size_t i) final
    {
        verify_ndx(i);
        BARQ_ASSERT(m_args[i].arg[0].type == BARQ_TYPE_DOUBLE);
        return m_args[i].arg[0].dnum;
    }

    StringData string_for_argument(size_t i) final
    {
        verify_ndx(i);
        BARQ_ASSERT(m_args[i].arg[0].type == BARQ_TYPE_STRING);
        return from_capi(m_args[i].arg[0].string);
    }

    BinaryData binary_for_argument(size_t i) final
    {
        verify_ndx(i);
        BARQ_ASSERT(m_args[i].arg[0].type == BARQ_TYPE_BINARY);
        return from_capi(m_args[i].arg[0].binary);
    }

    Timestamp timestamp_for_argument(size_t i) final
    {
        verify_ndx(i);
        BARQ_ASSERT(m_args[i].arg[0].type == BARQ_TYPE_TIMESTAMP);
        return from_capi(m_args[i].arg[0].timestamp);
    }

    ObjKey object_index_for_argument(size_t i) final
    {
        verify_ndx(i);
        BARQ_ASSERT(m_args[i].arg[0].type == BARQ_TYPE_LINK);
        // FIXME: Somehow check the target table type?
        return from_capi(m_args[i].arg[0].link).get_obj_key();
    }

    ObjectId objectid_for_argument(size_t i) final
    {
        verify_ndx(i);
        BARQ_ASSERT(m_args[i].arg[0].type == BARQ_TYPE_OBJECT_ID);
        return from_capi(m_args[i].arg[0].object_id);
    }

    Decimal128 decimal128_for_argument(size_t i) final
    {
        verify_ndx(i);
        BARQ_ASSERT(m_args[i].arg[0].type == BARQ_TYPE_DECIMAL128);
        return from_capi(m_args[i].arg[0].decimal128);
    }

    UUID uuid_for_argument(size_t i) final
    {
        verify_ndx(i);
        BARQ_ASSERT(m_args[i].arg[0].type == BARQ_TYPE_UUID);
        return from_capi(m_args[i].arg[0].uuid);
    }

    ObjLink objlink_for_argument(size_t i) final
    {
        verify_ndx(i);
        BARQ_ASSERT(m_args[i].arg[0].type == BARQ_TYPE_LINK);
        return from_capi(m_args[i].arg[0].link);
    }

#if BARQ_ENABLE_GEOSPATIAL
    Geospatial geospatial_for_argument(size_t i) final
    {
        verify_ndx(i);
        // FIXME: implement this
        throw LogicError{
            ErrorCodes::RuntimeError,
            util::format("geospatial in the C-API is not yet implemented (for argument %1)", i)}; // LCOV_EXCL_LINE
    }
#endif

    bool is_argument_null(size_t i) final
    {
        verify_ndx(i);
        return !m_args[i].is_list && m_args[i].arg[0].type == BARQ_TYPE_NULL;
    }

    bool is_argument_list(size_t i) final
    {
        verify_ndx(i);
        return m_args[i].is_list;
    }

    std::vector<Mixed> list_for_argument(size_t ndx) final
    {
        verify_ndx(ndx);
        BARQ_ASSERT(m_args[ndx].is_list);
        std::vector<Mixed> list;
        list.reserve(m_args[ndx].nb_args);
        for (size_t i = 0; i < m_args[ndx].nb_args; ++i) {
            list.push_back(from_capi(m_args[ndx].arg[i]));
        }
        return list;
    }
    DataType type_for_argument(size_t i) override
    {
        verify_ndx(i);
        switch (m_args[i].arg[0].type) {
            case BARQ_TYPE_NULL:                                                  // LCOV_EXCL_LINE
                BARQ_TERMINATE("Query parser did not call is_argument_null()"); // LCOV_EXCL_LINE
            case BARQ_TYPE_INT:
                return type_Int;
            case BARQ_TYPE_STRING:
                return type_String;
            case BARQ_TYPE_BOOL:
                return type_Bool;
            case BARQ_TYPE_FLOAT:
                return type_Float;
            case BARQ_TYPE_DOUBLE:
                return type_Double;
            case BARQ_TYPE_BINARY:
                return type_Binary;
            case BARQ_TYPE_TIMESTAMP:
                return type_Timestamp;
            case BARQ_TYPE_LINK:
                return type_Link;
            case BARQ_TYPE_OBJECT_ID:
                return type_ObjectId;
            case BARQ_TYPE_DECIMAL128:
                return type_Decimal;
            case BARQ_TYPE_UUID:
                return type_UUID;
            case BARQ_TYPE_LIST:
                return type_List;
            case BARQ_TYPE_DICTIONARY:
                return type_Dictionary;
        }
        throw LogicError{ErrorCodes::TypeMismatch, "Unsupported type"}; // LCOV_EXCL_LINE
        return type_Int;
    }
};
} // namespace

static Query parse_and_apply_query(const std::shared_ptr<Barq>& barq, ConstTableRef table, const char* query_string,
                                   size_t num_args, const barq_query_arg_t* args)
{
    query_parser::KeyPathMapping mapping;
    barq::populate_keypath_mapping(mapping, *barq);
    QueryArgumentsAdapter arguments{num_args, args};
    Query query = table->query(query_string, arguments, mapping);
    return query;
}

BARQ_API barq_query_t* barq_query_parse(const barq_t* barq, barq_class_key_t target_table_key,
                                         const char* query_string, size_t num_args, const barq_query_arg_t* args)
{
    return wrap_err([&]() {
        auto table = (*barq)->read_group().get_table(TableKey(target_table_key));
        Query query = parse_and_apply_query(*barq, table, query_string, num_args, args);
        auto ordering = query.get_ordering();
        return new barq_query_t{std::move(query), std::move(ordering), *barq};
    });
}

BARQ_API const char* barq_query_get_description(barq_query_t* query)
{
    return wrap_err([&]() {
        return query->get_description();
    });
}

BARQ_API barq_query_t* barq_query_append_query(const barq_query_t* existing_query, const char* query_string,
                                                size_t num_args, const barq_query_arg_t* args)
{
    return wrap_err([&]() {
        auto barq = existing_query->weak_barq.lock();
        auto table = existing_query->query.get_table();
        auto query = parse_and_apply_query(barq, table, query_string, num_args, args);
        auto combined = Query(existing_query->query).and_query(query);
        auto ordering_copy = util::make_bind<DescriptorOrdering>();
        *ordering_copy = existing_query->get_ordering();
        if (auto ordering = query.get_ordering())
            ordering_copy->append(*ordering);
        return new barq_query_t{std::move(combined), std::move(ordering_copy), barq};
    });
}

BARQ_API barq_query_t* barq_query_parse_for_list(const barq_list_t* list, const char* query_string, size_t num_args,
                                                  const barq_query_arg_t* args)
{
    return wrap_err([&]() {
        auto existing_query = list->get_query();
        auto barq = list->get_barq();
        auto table = list->get_table();
        auto query = parse_and_apply_query(barq, table, query_string, num_args, args);
        auto combined = existing_query.and_query(query);
        auto ordering_copy = util::make_bind<DescriptorOrdering>();
        if (auto ordering = query.get_ordering())
            ordering_copy->append(*ordering);
        return new barq_query_t{std::move(combined), std::move(ordering_copy), barq};
    });
}

BARQ_API barq_query_t* barq_query_parse_for_set(const barq_set_t* set, const char* query_string, size_t num_args,
                                                 const barq_query_arg_t* args)
{
    return wrap_err([&]() {
        auto existing_query = set->get_query();
        auto barq = set->get_barq();
        auto table = set->get_table();
        auto query = parse_and_apply_query(barq, table, query_string, num_args, args);
        auto combined = existing_query.and_query(query);
        auto ordering_copy = util::make_bind<DescriptorOrdering>();
        if (auto ordering = query.get_ordering())
            ordering_copy->append(*ordering);
        return new barq_query_t{std::move(combined), std::move(ordering_copy), barq};
    });
}

BARQ_API barq_query_t* barq_query_parse_for_results(const barq_results_t* results, const char* query_string,
                                                     size_t num_args, const barq_query_arg_t* args)
{
    return wrap_err([&]() {
        auto existing_query = results->get_query();
        auto barq = results->get_barq();
        auto table = results->get_table();
        auto query = parse_and_apply_query(barq, table, query_string, num_args, args);
        auto combined = existing_query.and_query(query);
        auto ordering_copy = util::make_bind<DescriptorOrdering>();
        if (auto ordering = query.get_ordering())
            ordering_copy->append(*ordering);
        return new barq_query_t{std::move(combined), std::move(ordering_copy), barq};
    });
}

BARQ_API bool barq_query_count(const barq_query_t* query, size_t* out_count)
{
    return wrap_err([&]() {
        *out_count = Query(query->query).count(query->get_ordering());
        return true;
    });
}

BARQ_API bool barq_query_find_first(barq_query_t* query, barq_value_t* out_value, bool* out_found)
{
    return wrap_err([&]() {
        const auto& barq_query_ordering = query->get_ordering();
        if (barq_query_ordering.size() > 0) {
            auto orderding = util::make_bind<DescriptorOrdering>();
            orderding->append(barq_query_ordering);
            query->query.set_ordering(orderding);
        }
        auto key = query->query.find();
        if (out_found)
            *out_found = bool(key);
        if (key && out_value) {
            ObjLink link{query->query.get_table()->get_key(), key};
            out_value->type = BARQ_TYPE_LINK;
            out_value->link = to_capi(link);
        }
        return true;
    });
}

BARQ_API barq_results_t* barq_query_find_all(barq_query_t* query)
{
    return wrap_err([&]() {
        auto shared_barq = query->weak_barq.lock();
        BARQ_ASSERT_RELEASE(shared_barq);
        return new barq_results{Results{shared_barq, query->query, query->get_ordering()}};
    });
}

BARQ_API barq_results_t* barq_list_to_results(barq_list_t* list)
{
    return wrap_err([&]() {
        return new barq_results_t{list->as_results()};
    });
}

BARQ_API barq_results_t* barq_set_to_results(barq_set_t* set)
{
    return wrap_err([&]() {
        return new barq_results_t{set->as_results()};
    });
}

BARQ_API barq_results_t* barq_dictionary_to_results(barq_dictionary_t* dictionary)
{
    return wrap_err([&]() {
        return new barq_results_t{dictionary->as_results()};
    });
}

BARQ_API barq_results_t* barq_get_backlinks(barq_object_t* object, barq_class_key_t source_table_key,
                                             barq_property_key_t property_key)
{
    return wrap_err([&]() {
        object->verify_attached();
        auto barq = object->barq();
        return new barq_results_t{
            Results{barq, object->get_obj(), TableKey{source_table_key}, ColKey{property_key}}};
    });
}

BARQ_API bool barq_results_is_valid(const barq_results_t* results, bool* is_valid)
{
    return wrap_err([&]() {
        if (is_valid)
            *is_valid = results->is_valid();
        return true;
    });
}

BARQ_API bool barq_results_count(barq_results_t* results, size_t* out_count)
{
    return wrap_err([&]() {
        auto count = results->size();
        if (out_count) {
            *out_count = count;
        }
        return true;
    });
}

BARQ_API barq_results_t* barq_results_filter(barq_results_t* results, barq_query_t* query)
{
    return wrap_err([&]() {
        return new barq_results{results->filter(std::move(query->query))};
    });
}

namespace {
barq_results_t* barq_results_ordering(barq_results_t* results, const char* op, const char* ordering)
{
    return wrap_err([&]() -> barq_results_t* {
        std::string str = "TRUEPREDICATE " + std::string(op) + "(" + std::string(ordering) + ")";
        auto q = results->get_table()->query(str);
        auto ordering{q.get_ordering()};
        return new barq_results{results->apply_ordering(std::move(*ordering))};
        return nullptr;
    });
}
} // namespace

BARQ_API barq_results_t* barq_results_sort(barq_results_t* results, const char* sort_string)
{
    return barq_results_ordering(results, "SORT", sort_string);
}

BARQ_API barq_results_t* barq_results_distinct(barq_results_t* results, const char* distinct_string)
{
    return barq_results_ordering(results, "DISTINCT", distinct_string);
}

BARQ_API barq_results_t* barq_results_limit(barq_results_t* results, size_t max_count)
{
    return wrap_err([&]() {
        return new barq_results{results->limit(max_count)};
    });
}


BARQ_API bool barq_results_get(barq_results_t* results, size_t index, barq_value_t* out_value)
{
    return wrap_err([&]() {
        auto mixed = results->get_any(index);
        if (out_value) {
            *out_value = to_capi(mixed);
        }
        return true;
    });
}

BARQ_API barq_list_t* barq_results_get_list(barq_results_t* results, size_t index)
{
    return wrap_err([&]() {
        barq_list_t* out = nullptr;
        auto result_list = results->get_list(index);
        if (result_list.is_valid())
            out = new barq_list_t{result_list};
        return out;
    });
}

BARQ_API barq_dictionary_t* barq_results_get_dictionary(barq_results_t* results, size_t index)
{
    return wrap_err([&]() {
        barq_dictionary_t* out = nullptr;
        auto result_dictionary = results->get_dictionary(index);
        if (result_dictionary.is_valid())
            out = new barq_dictionary_t{result_dictionary};
        return out;
    });
}

BARQ_API bool barq_results_find(barq_results_t* results, barq_value_t* value, size_t* out_index, bool* out_found)
{
    if (out_index)
        *out_index = barq::not_found;
    if (out_found)
        *out_found = false;

    return wrap_err([&]() {
        auto val = from_capi(*value);
        if (out_index) {
            *out_index = results->index_of(val);
            if (out_found && *out_index != barq::not_found) {
                *out_found = true;
            }
        }
        return true;
    });
}

BARQ_API barq_object_t* barq_results_get_object(barq_results_t* results, size_t index)
{
    return wrap_err([&]() {
        auto shared_barq = results->get_barq();
        auto obj = results->get<Obj>(index);
        return new barq_object_t{Object{shared_barq, std::move(obj)}};
    });
}

BARQ_API barq_query_t* barq_results_get_query(barq_results_t* results)
{
    return wrap_err([&]() {
        auto query = results->get_query();
        auto shared_barq = results->get_barq();
        auto ordering = query.get_ordering();
        return new barq_query_t{std::move(query), std::move(ordering), shared_barq};
    });
}

BARQ_API bool barq_results_find_object(barq_results_t* results, barq_object_t* value, size_t* out_index,
                                       bool* out_found)
{
    if (out_index)
        *out_index = barq::not_found;
    if (out_found)
        *out_found = false;

    return wrap_err([&]() {
        if (out_index) {
            *out_index = results->index_of(value->get_obj());
            if (out_found && *out_index != barq::not_found)
                *out_found = true;
        }
        return true;
    });
}

BARQ_API bool barq_results_delete_all(barq_results_t* results)
{
    return wrap_err([&]() {
        // Note: This method is very confusingly named. It actually does erase
        // all the objects.
        results->clear();
        return true;
    });
}

BARQ_API barq_results_t* barq_results_snapshot(const barq_results_t* results)
{
    return wrap_err([&]() {
        return new barq_results{results->snapshot()};
    });
}

BARQ_API bool barq_results_min(barq_results_t* results, barq_property_key_t col, barq_value_t* out_value,
                               bool* out_found)
{
    return wrap_err([&]() {
        if (auto x = results->min(ColKey(col))) {
            if (out_found) {
                *out_found = true;
            }
            if (out_value) {
                *out_value = to_capi(*x);
            }
        }
        else {
            if (out_found) {
                *out_found = false;
            }
            if (out_value) {
                out_value->type = BARQ_TYPE_NULL;
            }
        }
        return true;
    });
}

BARQ_API bool barq_results_max(barq_results_t* results, barq_property_key_t col, barq_value_t* out_value,
                               bool* out_found)
{
    return wrap_err([&]() {
        if (auto x = results->max(ColKey(col))) {
            if (out_found) {
                *out_found = true;
            }
            if (out_value) {
                *out_value = to_capi(*x);
            }
        }
        else {
            if (out_found) {
                *out_found = false;
            }
            if (out_value) {
                out_value->type = BARQ_TYPE_NULL;
            }
        }
        return true;
    });
}

BARQ_API bool barq_results_sum(barq_results_t* results, barq_property_key_t col, barq_value_t* out_value,
                               bool* out_found)
{
    return wrap_err([&]() {
        if (out_found) {
            *out_found = results->size() != 0;
        }

        if (auto x = results->sum(ColKey(col))) {
            if (out_value)
                *out_value = to_capi(*x);
        }
        else {
            // Note: This can only be hit when the `m_table` and `m_collection`
            // pointers in `Results` are NULL.
            //
            // FIXME: It is unclear when that happens.

            // LCOV_EXCL_START
            if (out_value) {
                out_value->type = BARQ_TYPE_NULL;
            }
            // LCOV_EXCL_STOP
        }
        return true;
    });
}

BARQ_API bool barq_results_average(barq_results_t* results, barq_property_key_t col, barq_value_t* out_value,
                                   bool* out_found)
{
    return wrap_err([&]() {
        if (auto x = results->average(ColKey(col))) {
            if (out_found) {
                *out_found = true;
            }
            if (out_value) {
                *out_value = to_capi(*x);
            }
        }
        else {
            if (out_found) {
                *out_found = false;
            }
            if (out_value) {
                out_value->type = BARQ_TYPE_NULL;
            }
        }
        return true;
    });
}

BARQ_API barq_results_t* barq_results_from_thread_safe_reference(const barq_t* barq,
                                                                  barq_thread_safe_reference_t* tsr)
{
    return wrap_err([&]() {
        auto rtsr = dynamic_cast<barq_results::thread_safe_reference*>(tsr);
        if (!rtsr) {
            throw LogicError{ErrorCodes::IllegalOperation, "Thread safe reference type mismatch"};
        }

        auto results = rtsr->resolve<Results>(*barq);
        return new barq_results_t{std::move(results)};
    });
}

BARQ_API barq_results_t* barq_results_resolve_in(barq_results_t* from_results, const barq_t* target_barq)
{
    return wrap_err([&]() {
        const auto& barq = *target_barq;
        auto resolved_results = from_results->freeze(barq);
        return new barq_results_t{std::move(resolved_results)};
    });
}

} // namespace barq::c_api
