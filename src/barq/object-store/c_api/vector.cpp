////////////////////////////////////////////////////////////////////////////
//
// Vector (HNSW) index management and kNN search for the C API. Vector indexes
// are local: they are built imperatively (not from the shared schema) and never
// travel over the sync wire.
//
////////////////////////////////////////////////////////////////////////////

#include <barq/object-store/c_api/types.hpp>
#include <barq/object-store/c_api/util.hpp>
#include <barq/object-store/c_api/conversion.hpp>

#include <barq/index_vector.hpp>
#include <barq/table.hpp>

#include <stdexcept>
#include <vector>

namespace barq::c_api {

BARQ_API bool barq_add_vector_index(barq_t* barq, barq_class_key_t class_key, barq_property_key_t col_key,
                                    const barq_vector_index_config_t* config)
{
    return wrap_err([&]() {
        auto& shared_barq = *barq;
        auto table = shared_barq->read_group().get_table(TableKey(class_key));
        if (config)
            table->add_vector_index(ColKey(col_key), from_capi(*config));
        else
            table->add_vector_index(ColKey(col_key));
        return true;
    });
}

BARQ_API bool barq_remove_vector_index(barq_t* barq, barq_class_key_t class_key, barq_property_key_t col_key)
{
    return wrap_err([&]() {
        auto& shared_barq = *barq;
        auto table = shared_barq->read_group().get_table(TableKey(class_key));
        table->remove_vector_index(ColKey(col_key));
        return true;
    });
}

BARQ_API bool barq_rebuild_vector_index(barq_t* barq, barq_class_key_t class_key, barq_property_key_t col_key)
{
    return wrap_err([&]() {
        auto& shared_barq = *barq;
        auto table = shared_barq->read_group().get_table(TableKey(class_key));
        table->rebuild_vector_index(ColKey(col_key));
        return true;
    });
}

BARQ_API bool barq_has_vector_index(const barq_t* barq, barq_class_key_t class_key, barq_property_key_t col_key,
                                    bool* out_has)
{
    return wrap_err([&]() {
        auto& shared_barq = **barq;
        auto table = shared_barq.read_group().get_table(TableKey(class_key));
        if (out_has)
            *out_has = table->has_vector_index(ColKey(col_key));
        return true;
    });
}

BARQ_API bool barq_get_vector_index_config(const barq_t* barq, barq_class_key_t class_key,
                                           barq_property_key_t col_key, barq_vector_index_config_t* out_config)
{
    return wrap_err([&]() {
        auto& shared_barq = **barq;
        auto table = shared_barq.read_group().get_table(TableKey(class_key));
        auto* index = table->get_vector_index(ColKey(col_key));
        if (!index)
            throw std::out_of_range("No vector index on the requested column");
        if (out_config)
            *out_config = to_capi(index->config());
        return true;
    });
}

BARQ_API barq_results_t* barq_results_knn_search(const barq_results_t* results, barq_property_key_t col_key,
                                                 const float* query_data, size_t query_size, size_t k, size_t ef,
                                                 bool exact)
{
    return wrap_err([&]() {
        std::vector<float> query(query_data, query_data + query_size);
        return new barq_results{results->knn_search(ColKey(col_key), query, k, ef, exact)};
    });
}

} // namespace barq::c_api
