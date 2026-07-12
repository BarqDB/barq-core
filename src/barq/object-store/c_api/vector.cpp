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

#include <cmath>
#include <stdexcept>
#include <vector>

namespace barq::c_api {

namespace {

void validate_config(const barq_vector_index_config_t& config)
{
    if (config.metric < BARQ_VECTOR_METRIC_INNER_PRODUCT || config.metric > BARQ_VECTOR_METRIC_COSINE)
        throw std::invalid_argument("Invalid vector metric");
    if (config.encoding < BARQ_VECTOR_ENCODING_FLOAT32 || config.encoding > BARQ_VECTOR_ENCODING_SQ8)
        throw std::invalid_argument("Invalid vector encoding");
    if (config.m == 0)
        throw std::invalid_argument("Vector index m must be greater than zero");
    if (config.ef_construction == 0)
        throw std::invalid_argument("Vector index ef_construction must be greater than zero");
}

} // namespace

BARQ_API bool barq_add_vector_index(barq_t* barq, barq_class_key_t class_key, barq_property_key_t col_key,
                                    const barq_vector_index_config_t* config)
{
    return wrap_err([&]() {
        auto& shared_barq = *barq;
        auto table = shared_barq->read_group().get_table(TableKey(class_key));
        if (config) {
            validate_config(*config);
            table->add_vector_index(ColKey(col_key), from_capi(*config));
        }
        else {
            table->add_vector_index(ColKey(col_key));
        }
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
        if (!query_data)
            throw std::invalid_argument("Query data must not be null");
        for (size_t i = 0; i < query_size; ++i) {
            if (!std::isfinite(query_data[i]))
                throw std::invalid_argument("Query vector contains a non-finite value");
        }
        auto table = results->get_table();
        auto* index = table->get_vector_index(ColKey(col_key));
        if (!index)
            throw IllegalOperation("The requested column has no vector index");
        if (auto dimensions = index->config().dimensions; dimensions != 0 && query_size != dimensions) {
            throw IllegalOperation("Query vector dimension does not match the vector index");
        }
        std::vector<float> query(query_data, query_data + query_size);
        return new barq_results{results->knn_search(ColKey(col_key), query, k, ef, exact)};
    });
}

} // namespace barq::c_api
