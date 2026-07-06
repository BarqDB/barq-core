#ifndef BARQ_OBJECT_STORE_C_API_CONVERSION_HPP
#define BARQ_OBJECT_STORE_C_API_CONVERSION_HPP

#include <barq.h>

#include <barq/object-store/property.hpp>
#include <barq/object-store/schema.hpp>
#include <barq/object-store/object_schema.hpp>
#include <barq/object-store/shared_barq.hpp>

#include <barq/string_data.hpp>
#include <barq/binary_data.hpp>
#include <barq/timestamp.hpp>
#include <barq/decimal128.hpp>
#include <barq/object_id.hpp>
#include <barq/mixed.hpp>
#include <barq/uuid.hpp>

#include <string>

namespace barq::c_api {

static inline barq_string_t to_capi(StringData data)
{
    return barq_string_t{data.data(), data.size()};
}

// Because this is often used as `return to_capi(...);` it is dangerous to pass a temporary string here. If you really
// need to and know it is correct (eg passing to a C callback), you can explicitly create the StringData wrapper.
barq_string_t to_capi(const std::string&& str) = delete; // temporary std::string would dangle.

static inline barq_string_t to_capi(const std::string& str)
{
    return to_capi(StringData{str});
}

static inline barq_string_t to_capi(std::string_view str_view)
{
    return barq_string_t{str_view.data(), str_view.size()};
}

static inline StringData from_capi(barq_string_t str)
{
    return StringData{str.data, str.size};
}

static inline barq_binary_t to_capi(BinaryData bin)
{
    return barq_binary_t{reinterpret_cast<const unsigned char*>(bin.data()), bin.size()};
}

static inline BinaryData from_capi(barq_binary_t bin)
{
    return BinaryData{reinterpret_cast<const char*>(bin.data), bin.size};
}

static inline barq_timestamp_t to_capi(Timestamp ts)
{
    return barq_timestamp_t{ts.get_seconds(), ts.get_nanoseconds()};
}

static inline Timestamp from_capi(barq_timestamp_t ts)
{
    return Timestamp{ts.seconds, ts.nanoseconds};
}

static inline barq_decimal128_t to_capi(const Decimal128& dec)
{
    auto raw = dec.raw();
    return barq_decimal128_t{{raw->w[0], raw->w[1]}};
}

static inline Decimal128 from_capi(barq_decimal128_t dec)
{
    return Decimal128{Decimal128::Bid128{{dec.w[0], dec.w[1]}}};
}

static inline barq_object_id_t to_capi(ObjectId object_id)
{
    barq_object_id_t result;
    auto bytes = object_id.to_bytes();
    std::copy(bytes.begin(), bytes.end(), result.bytes);
    return result;
}

static inline ObjectId from_capi(barq_object_id_t object_id)
{
    static_assert(ObjectId::num_bytes == 12);
    ObjectId::ObjectIdBytes bytes;
    std::copy(object_id.bytes, object_id.bytes + 12, bytes.begin());
    return ObjectId(bytes);
}

static inline ObjLink from_capi(barq_link_t val)
{
    return ObjLink{TableKey(val.target_table), ObjKey(val.target)};
}

static inline barq_link_t to_capi(ObjLink link)
{
    return barq_link_t{link.get_table_key().value, link.get_obj_key().value};
}

static inline UUID from_capi(barq_uuid_t val)
{
    static_assert(sizeof(val.bytes) == UUID::num_bytes);
    UUID::UUIDBytes bytes;
    std::copy(val.bytes, val.bytes + UUID::num_bytes, bytes.data());
    return UUID{bytes};
}

static inline barq_uuid_t to_capi(UUID val)
{
    barq_uuid_t uuid;
    auto bytes = val.to_bytes();
    std::copy(bytes.data(), bytes.data() + UUID::num_bytes, uuid.bytes);
    return uuid;
}

static inline Mixed from_capi(barq_value_t val)
{
    switch (val.type) {
        case BARQ_TYPE_NULL:
            return Mixed{};
        case BARQ_TYPE_INT:
            return Mixed{val.integer};
        case BARQ_TYPE_BOOL:
            return Mixed{val.boolean};
        case BARQ_TYPE_STRING:
            return Mixed{from_capi(val.string)};
        case BARQ_TYPE_BINARY:
            return Mixed{from_capi(val.binary)};
        case BARQ_TYPE_TIMESTAMP:
            return Mixed{from_capi(val.timestamp)};
        case BARQ_TYPE_FLOAT:
            return Mixed{val.fnum};
        case BARQ_TYPE_DOUBLE:
            return Mixed{val.dnum};
        case BARQ_TYPE_DECIMAL128:
            return Mixed{from_capi(val.decimal128)};
        case BARQ_TYPE_OBJECT_ID:
            return Mixed{from_capi(val.object_id)};
        case BARQ_TYPE_LINK:
            return Mixed{ObjLink{TableKey(val.link.target_table), ObjKey(val.link.target)}};
        case BARQ_TYPE_UUID:
            return Mixed{UUID{from_capi(val.uuid)}};
        case BARQ_TYPE_LIST:
            return Mixed{0, CollectionType::List};
        case BARQ_TYPE_DICTIONARY:
            return Mixed{0, CollectionType::Dictionary};
    }
    BARQ_TERMINATE("Invalid barq_value_t"); // LCOV_EXCL_LINE
}

static inline barq_value_t to_capi(Mixed value)
{
    barq_value_t val;
    if (value.is_null()) {
        val.type = BARQ_TYPE_NULL;
    }
    else {
        auto type = value.get_type();
        switch (type) {
            case type_Int: {
                val.type = BARQ_TYPE_INT;
                val.integer = value.get<int64_t>();
                break;
            }
            case type_Bool: {
                val.type = BARQ_TYPE_BOOL;
                val.boolean = value.get<bool>();
                break;
            }
            case type_String: {
                val.type = BARQ_TYPE_STRING;
                val.string = to_capi(value.get<StringData>());
                break;
            }
            case type_Binary: {
                val.type = BARQ_TYPE_BINARY;
                val.binary = to_capi(value.get<BinaryData>());
                break;
            }
            case type_Timestamp: {
                val.type = BARQ_TYPE_TIMESTAMP;
                val.timestamp = to_capi(value.get<Timestamp>());
                break;
            }
            case type_Float: {
                val.type = BARQ_TYPE_FLOAT;
                val.fnum = value.get<float>();
                break;
            }
            case type_Double: {
                val.type = BARQ_TYPE_DOUBLE;
                val.dnum = value.get<double>();
                break;
            }
            case type_Decimal: {
                val.type = BARQ_TYPE_DECIMAL128;
                val.decimal128 = to_capi(value.get<Decimal128>());
                break;
            }
            case type_Link: {
                BARQ_TERMINATE("Not implemented yet"); // LCOV_EXCL_LINE
            }
            case type_ObjectId: {
                val.type = BARQ_TYPE_OBJECT_ID;
                val.object_id = to_capi(value.get<ObjectId>());
                break;
            }
            case type_TypedLink: {
                val.type = BARQ_TYPE_LINK;
                auto link = value.get<ObjLink>();
                val.link.target_table = link.get_table_key().value;
                val.link.target = link.get_obj_key().value;
                break;
            }
            case type_UUID: {
                val.type = BARQ_TYPE_UUID;
                auto uuid = value.get<UUID>();
                val.uuid = to_capi(uuid);
                break;
            }

            case type_Mixed:
                BARQ_TERMINATE("Invalid Mixed value type"); // LCOV_EXCL_LINE
            default:
                if (type == type_List) {
                    val.type = BARQ_TYPE_LIST;
                }
                else if (type == type_Dictionary) {
                    val.type = BARQ_TYPE_DICTIONARY;
                }
        }
    }

    return val;
}

static inline SchemaMode from_capi(barq_schema_mode_e mode)
{
    switch (mode) {
        case BARQ_SCHEMA_MODE_AUTOMATIC:
            return SchemaMode::Automatic;
        case BARQ_SCHEMA_MODE_IMMUTABLE:
            return SchemaMode::Immutable;
        case BARQ_SCHEMA_MODE_READ_ONLY:
            return SchemaMode::ReadOnly;
        case BARQ_SCHEMA_MODE_SOFT_RESET_FILE:
            return SchemaMode::SoftResetFile;
        case BARQ_SCHEMA_MODE_HARD_RESET_FILE:
            return SchemaMode::HardResetFile;
        case BARQ_SCHEMA_MODE_ADDITIVE_DISCOVERED:
            return SchemaMode::AdditiveDiscovered;
        case BARQ_SCHEMA_MODE_ADDITIVE_EXPLICIT:
            return SchemaMode::AdditiveExplicit;
        case BARQ_SCHEMA_MODE_MANUAL:
            return SchemaMode::Manual;
    }
    BARQ_TERMINATE("Invalid schema mode."); // LCOV_EXCL_LINE
}

static inline barq_schema_mode_e to_capi(SchemaMode mode)
{
    switch (mode) {
        case SchemaMode::Automatic:
            return BARQ_SCHEMA_MODE_AUTOMATIC;
        case SchemaMode::Immutable:
            return BARQ_SCHEMA_MODE_IMMUTABLE;
        case SchemaMode::ReadOnly:
            return BARQ_SCHEMA_MODE_READ_ONLY;
        case SchemaMode::SoftResetFile:
            return BARQ_SCHEMA_MODE_SOFT_RESET_FILE;
        case SchemaMode::HardResetFile:
            return BARQ_SCHEMA_MODE_HARD_RESET_FILE;
        case SchemaMode::AdditiveDiscovered:
            return BARQ_SCHEMA_MODE_ADDITIVE_DISCOVERED;
        case SchemaMode::AdditiveExplicit:
            return BARQ_SCHEMA_MODE_ADDITIVE_EXPLICIT;
        case SchemaMode::Manual:
            return BARQ_SCHEMA_MODE_MANUAL;
    }
    BARQ_TERMINATE("Invalid schema mode."); // LCOV_EXCL_LINE
}

static inline SchemaSubsetMode from_capi(barq_schema_subset_mode_e subset_mode)
{
    switch (subset_mode) {
        case BARQ_SCHEMA_SUBSET_MODE_ALL_CLASSES:
            return SchemaSubsetMode::AllClasses;
        case BARQ_SCHEMA_SUBSET_MODE_ALL_PROPERTIES:
            return SchemaSubsetMode::AllProperties;
        case BARQ_SCHEMA_SUBSET_MODE_COMPLETE:
            return SchemaSubsetMode::Complete;
        case BARQ_SCHEMA_SUBSET_MODE_STRICT:
            return SchemaSubsetMode::Strict;
    }
    BARQ_TERMINATE("Invalid subset schema mode."); // LCOV_EXCL_LINE
}

static inline barq_schema_subset_mode_e to_capi(const SchemaSubsetMode& subset_mode)
{
    if (subset_mode == SchemaSubsetMode::AllClasses)
        return BARQ_SCHEMA_SUBSET_MODE_ALL_CLASSES;
    else if (subset_mode == SchemaSubsetMode::AllProperties)
        return BARQ_SCHEMA_SUBSET_MODE_ALL_PROPERTIES;
    else if (subset_mode == SchemaSubsetMode::Complete)
        return BARQ_SCHEMA_SUBSET_MODE_COMPLETE;
    else if (subset_mode == SchemaSubsetMode::Strict)
        return BARQ_SCHEMA_SUBSET_MODE_STRICT;
    BARQ_TERMINATE("Invalid subset schema mode."); // LCOV_EXCL_LINE
}

static inline barq_property_type_e to_capi(PropertyType type) noexcept
{
    type &= ~PropertyType::Flags;

    switch (type) {
        case PropertyType::Int:
            return BARQ_PROPERTY_TYPE_INT;
        case PropertyType::Bool:
            return BARQ_PROPERTY_TYPE_BOOL;
        case PropertyType::String:
            return BARQ_PROPERTY_TYPE_STRING;
        case PropertyType::Data:
            return BARQ_PROPERTY_TYPE_BINARY;
        case PropertyType::Mixed:
            return BARQ_PROPERTY_TYPE_MIXED;
        case PropertyType::Date:
            return BARQ_PROPERTY_TYPE_TIMESTAMP;
        case PropertyType::Float:
            return BARQ_PROPERTY_TYPE_FLOAT;
        case PropertyType::Double:
            return BARQ_PROPERTY_TYPE_DOUBLE;
        case PropertyType::Decimal:
            return BARQ_PROPERTY_TYPE_DECIMAL128;
        case PropertyType::Object:
            return BARQ_PROPERTY_TYPE_OBJECT;
        case PropertyType::LinkingObjects:
            return BARQ_PROPERTY_TYPE_LINKING_OBJECTS;
        case PropertyType::ObjectId:
            return BARQ_PROPERTY_TYPE_OBJECT_ID;
        case PropertyType::UUID:
            return BARQ_PROPERTY_TYPE_UUID;
        // LCOV_EXCL_START
        case PropertyType::Nullable:
            [[fallthrough]];
        case PropertyType::Flags:
            [[fallthrough]];
        case PropertyType::Set:
            [[fallthrough]];
        case PropertyType::Dictionary:
            [[fallthrough]];
        case PropertyType::Collection:
            [[fallthrough]];
        case PropertyType::Array:
            BARQ_UNREACHABLE();
            // LCOV_EXCL_STOP
    }
    BARQ_TERMINATE("Unsupported property type"); // LCOV_EXCL_LINE
}

static inline PropertyType from_capi(barq_property_type_e type) noexcept
{
    switch (type) {
        case BARQ_PROPERTY_TYPE_INT:
            return PropertyType::Int;
        case BARQ_PROPERTY_TYPE_BOOL:
            return PropertyType::Bool;
        case BARQ_PROPERTY_TYPE_STRING:
            return PropertyType::String;
        case BARQ_PROPERTY_TYPE_BINARY:
            return PropertyType::Data;
        case BARQ_PROPERTY_TYPE_MIXED:
            return PropertyType::Mixed;
        case BARQ_PROPERTY_TYPE_TIMESTAMP:
            return PropertyType::Date;
        case BARQ_PROPERTY_TYPE_FLOAT:
            return PropertyType::Float;
        case BARQ_PROPERTY_TYPE_DOUBLE:
            return PropertyType::Double;
        case BARQ_PROPERTY_TYPE_DECIMAL128:
            return PropertyType::Decimal;
        case BARQ_PROPERTY_TYPE_OBJECT:
            return PropertyType::Object;
        case BARQ_PROPERTY_TYPE_LINKING_OBJECTS:
            return PropertyType::LinkingObjects;
        case BARQ_PROPERTY_TYPE_OBJECT_ID:
            return PropertyType::ObjectId;
        case BARQ_PROPERTY_TYPE_UUID:
            return PropertyType::UUID;
    }
    BARQ_TERMINATE("Unsupported property type"); // LCOV_EXCL_LINE
}


static inline Property from_capi(const barq_property_info_t& p) noexcept
{
    Property prop;
    prop.name = p.name;
    prop.public_name = p.public_name;
    prop.type = from_capi(p.type);
    prop.object_type = p.link_target;
    prop.link_origin_property_name = p.link_origin_property_name;
    prop.is_primary = Property::IsPrimary{bool(p.flags & BARQ_PROPERTY_PRIMARY_KEY)};
    prop.is_indexed = Property::IsIndexed{bool(p.flags & BARQ_PROPERTY_INDEXED)};
    prop.is_fulltext_indexed = Property::IsFulltextIndexed{bool(p.flags & BARQ_PROPERTY_FULLTEXT_INDEXED)};

    if (bool(p.flags & BARQ_PROPERTY_NULLABLE)) {
        prop.type |= PropertyType::Nullable;
    }
    switch (p.collection_type) {
        case BARQ_COLLECTION_TYPE_NONE:
            break;
        case BARQ_COLLECTION_TYPE_LIST: {
            prop.type |= PropertyType::Array;
            break;
        }
        case BARQ_COLLECTION_TYPE_SET: {
            prop.type |= PropertyType::Set;
            break;
        }
        case BARQ_COLLECTION_TYPE_DICTIONARY: {
            prop.type |= PropertyType::Dictionary;
            break;
        }
    }
    return prop;
}

static inline std::optional<CollectionType> from_capi(barq_collection_type_e type)
{
    switch (type) {
        case BARQ_COLLECTION_TYPE_NONE:
            break;
        case BARQ_COLLECTION_TYPE_LIST:
            return CollectionType::List;
        case BARQ_COLLECTION_TYPE_SET:
            return CollectionType::Set;
        case BARQ_COLLECTION_TYPE_DICTIONARY:
            return CollectionType::Dictionary;
    }
    return {};
}

static inline barq_property_info_t to_capi(const Property& prop) noexcept
{
    barq_property_info_t p;
    p.name = prop.name.c_str();
    p.public_name = prop.public_name.c_str();
    p.type = to_capi(prop.type & ~PropertyType::Flags);
    p.link_target = prop.object_type.c_str();
    p.link_origin_property_name = prop.link_origin_property_name.c_str();

    p.flags = BARQ_PROPERTY_NORMAL;
    if (prop.is_indexed)
        p.flags |= BARQ_PROPERTY_INDEXED;
    if (prop.is_fulltext_indexed)
        p.flags |= BARQ_PROPERTY_FULLTEXT_INDEXED;
    if (prop.is_primary)
        p.flags |= BARQ_PROPERTY_PRIMARY_KEY;
    if (bool(prop.type & PropertyType::Nullable))
        p.flags |= BARQ_PROPERTY_NULLABLE;

    p.collection_type = BARQ_COLLECTION_TYPE_NONE;
    if (bool(prop.type & PropertyType::Array))
        p.collection_type = BARQ_COLLECTION_TYPE_LIST;
    if (bool(prop.type & PropertyType::Set))
        p.collection_type = BARQ_COLLECTION_TYPE_SET;
    if (bool(prop.type & PropertyType::Dictionary))
        p.collection_type = BARQ_COLLECTION_TYPE_DICTIONARY;

    p.key = prop.column_key.value;

    return p;
}

static inline barq_class_info_t to_capi(const ObjectSchema& o)
{
    barq_class_info_t info;
    info.name = o.name.c_str();
    info.primary_key = o.primary_key.c_str();
    info.num_properties = o.persisted_properties.size();
    info.num_computed_properties = o.computed_properties.size();
    info.key = o.table_key.value;
    switch (o.table_type) {
        case ObjectSchema::ObjectType::Embedded: {
            info.flags = BARQ_CLASS_EMBEDDED;
            break;
        }
        case ObjectSchema::ObjectType::TopLevelAsymmetric: {
            info.flags = BARQ_CLASS_ASYMMETRIC;
            break;
        }
        case ObjectSchema::ObjectType::TopLevel: {
            info.flags = BARQ_CLASS_NORMAL;
            break;
        }
        default:
            BARQ_TERMINATE(util::format("Invalid table type: %1", uint8_t(o.table_type)).c_str());
    }
    return info;
}

static inline barq_version_id_t to_capi(const VersionID& v)
{
    barq_version_id_t version_id;
    version_id.version = v.version;
    version_id.index = v.index;
    return version_id;
}

static inline barq_error_t to_capi(const Status& s)
{
    barq_error_t err = {};
    err.error = static_cast<barq_errno_e>(s.code());
    err.categories = static_cast<barq_error_category_e>(ErrorCodes::error_categories(s.code()).value());
    err.message = s.reason().c_str();
    return err;
}

} // namespace barq::c_api


#endif // BARQ_OBJECT_STORE_C_API_CONVERSION_HPP
