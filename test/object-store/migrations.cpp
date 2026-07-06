////////////////////////////////////////////////////////////////////////////
//
// Copyright 2016 Realm Inc.
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

#include <util/test_file.hpp>
#include <util/test_utils.hpp>

#include <barq/group.hpp>
#include <barq/table.hpp>

#include <barq/object-store/object_schema.hpp>
#include <barq/object-store/object_store.hpp>
#include <barq/object-store/property.hpp>
#include <barq/object-store/schema.hpp>
#include <barq/object-store/impl/object_accessor_impl.hpp>

#include <barq/util/scope_exit.hpp>

#include <catch2/catch_all.hpp>

#ifdef _WIN32
#include <Windows.h>
#endif

using namespace barq;
using ObjectType = ObjectSchema::ObjectType;
using util::any_cast;

#define VERIFY_SCHEMA(r, m) verify_schema((r), __LINE__, m)

#define REQUIRE_UPDATE_FAILS(r, schema, msg)                                                                         \
    REQUIRE_EXCEPTION((r).update_schema(schema), SchemaMismatch, Catch::Matchers::ContainsSubstring(msg));

#define INVALID_SCHEMA_CHANGE(r, schema, msg)                                                                        \
    REQUIRE_EXCEPTION((r).update_schema(schema), InvalidSchemaChange, Catch::Matchers::ContainsSubstring(msg));

#define REQUIRE_UPDATE_SUCCEEDS(r, s, version)                                                                       \
    do {                                                                                                             \
        REQUIRE_NOTHROW((r).update_schema(s, version));                                                              \
        VERIFY_SCHEMA(r, false);                                                                                     \
        REQUIRE((r).schema() == s);                                                                                  \
        REQUIRE((r).schema_version() == version);                                                                    \
    } while (0)

#define REQUIRE_MIGRATION_SUCCEEDS(r, s, version, fn)                                                                \
    do {                                                                                                             \
        REQUIRE_NOTHROW((r).update_schema(s, version, fn));                                                          \
        VERIFY_SCHEMA(r, false);                                                                                     \
        REQUIRE((r).schema() == s);                                                                                  \
        REQUIRE((r).schema_version() == version);                                                                    \
    } while (0)

#define REQUIRE_NO_MIGRATION_NEEDED(r, schema1, schema2)                                                             \
    do {                                                                                                             \
        REQUIRE_UPDATE_SUCCEEDS(r, schema1, 0);                                                                      \
        REQUIRE_UPDATE_SUCCEEDS(r, schema2, 0);                                                                      \
    } while (0)

#define REQUIRE_MIGRATION_NEEDED(r, schema1, schema2, msg)                                                           \
    do {                                                                                                             \
        REQUIRE_UPDATE_SUCCEEDS(r, schema1, 0);                                                                      \
        REQUIRE_UPDATE_FAILS(r, schema2, msg);                                                                       \
        REQUIRE((r).schema() == schema1);                                                                            \
        REQUIRE_UPDATE_SUCCEEDS(r, schema2, 1);                                                                      \
    } while (0)

namespace {
void verify_schema(Barq& r, int line, bool in_migration)
{
    CAPTURE(line);
    for (auto&& object_schema : r.schema()) {
        auto table = r.read_group().get_table(object_schema.table_key);
        REQUIRE(table);
        REQUIRE(std::string(table->get_name()) == ObjectStore::table_name_for_object_type(object_schema.name));
        CAPTURE(object_schema.name);
        std::string primary_key;
        if (!in_migration) {
            auto col = table->get_primary_key_column();
            primary_key = col ? table->get_column_name(col) : "";
            REQUIRE(primary_key == object_schema.primary_key);
            REQUIRE(table->get_table_type() == Table::Type(object_schema.table_type));
        }
        else {
            primary_key = object_schema.primary_key;
            // Tables are not changed to embedded until after the migration block completes
            if (object_schema.table_type != ObjectType::Embedded) {
                REQUIRE(table->get_table_type() == Table::Type(object_schema.table_type));
            }
        }
        for (auto&& prop : object_schema.persisted_properties) {
            auto col = table->get_column_key(prop.name);
            CAPTURE(prop.name);
            REQUIRE(col);
            REQUIRE(col == prop.column_key);
            REQUIRE(to_underlying(ObjectSchema::from_core_type(col)) == to_underlying(prop.type));
            REQUIRE(table->has_search_index(col) == prop.requires_index());
            REQUIRE(bool(prop.is_primary) == (prop.name == primary_key));
        }
    }
}

TableRef get_table(std::shared_ptr<Barq> const& barq, StringData object_type)
{
    return ObjectStore::table_for_object_type(barq->read_group(), object_type);
}

// Helper functions for modifying Schema objects, mostly for the sake of making
// it clear what exactly is different about the 2+ schema objects used in
// various tests
Schema add_table(Schema const& schema, ObjectSchema object_schema)
{
    std::vector<ObjectSchema> new_schema(schema.begin(), schema.end());
    new_schema.push_back(std::move(object_schema));
    return new_schema;
}

Schema remove_table(Schema const& schema, StringData object_name)
{
    std::vector<ObjectSchema> new_schema;
    std::remove_copy_if(schema.begin(), schema.end(), std::back_inserter(new_schema), [&](auto&& object_schema) {
        return object_schema.name == object_name;
    });
    return new_schema;
}

Schema add_property(Schema schema, StringData object_name, Property property)
{
    schema.find(object_name)->persisted_properties.push_back(std::move(property));
    return schema;
}

Schema remove_property(Schema schema, StringData object_name, StringData property_name)
{
    auto& properties = schema.find(object_name)->persisted_properties;
    properties.erase(std::find_if(begin(properties), end(properties), [&](auto&& prop) {
        return prop.name == property_name;
    }));
    return schema;
}

Schema set_indexed(Schema schema, StringData object_name, StringData property_name, bool value)
{
    schema.find(object_name)->property_for_name(property_name)->is_indexed = value;
    return schema;
}

Schema set_fulltext_indexed(Schema schema, StringData object_name, StringData property_name, bool value)
{
    schema.find(object_name)->property_for_name(property_name)->is_fulltext_indexed = value;
    return schema;
}

Schema set_optional(Schema schema, StringData object_name, StringData property_name, bool value)
{
    auto& prop = *schema.find(object_name)->property_for_name(property_name);
    if (value)
        prop.type |= PropertyType::Nullable;
    else
        prop.type &= ~PropertyType::Nullable;
    return schema;
}

Schema set_type(Schema schema, StringData object_name, StringData property_name, PropertyType value)
{
    schema.find(object_name)->property_for_name(property_name)->type = value;
    return schema;
}

Schema set_target(Schema schema, StringData object_name, StringData property_name, StringData new_target)
{
    schema.find(object_name)->property_for_name(property_name)->object_type = new_target;
    return schema;
}

Schema set_primary_key(Schema schema, StringData object_name, StringData new_primary_property)
{
    auto& object_schema = *schema.find(object_name);
    if (auto old_primary = object_schema.primary_key_property()) {
        old_primary->is_primary = false;
    }
    if (new_primary_property.size()) {
        object_schema.property_for_name(new_primary_property)->is_primary = true;
    }
    object_schema.primary_key = new_primary_property;
    return schema;
}

Schema set_table_type(Schema schema, StringData object_name, ObjectType table_type)
{
    schema.find(object_name)->table_type = table_type;
    return schema;
}

auto create_objects(Table& table, size_t count)
{
    std::vector<ObjKey> keys;
    table.create_objects(count, keys);
    return keys;
}
} // anonymous namespace

TEST_CASE("migration: Automatic", "[migration]") {
    InMemoryTestFile config;
    config.automatic_change_notifications = false;

    SECTION("no migration required") {
        SECTION("add object schema") {
            auto barq = Barq::get_shared_barq(config);

            Schema schema1 = {};
            Schema schema2 = add_table(schema1, {"object", {{"value", PropertyType::Int}}});
            Schema schema3 = add_table(schema2, {"object2", {{"value", PropertyType::Int}}});
            REQUIRE_UPDATE_SUCCEEDS(*barq, schema1, 0);
            REQUIRE_UPDATE_SUCCEEDS(*barq, schema2, 0);
            REQUIRE_UPDATE_SUCCEEDS(*barq, schema3, 0);
        }

        SECTION("add embedded object schema") {
            auto barq = Barq::get_shared_barq(config);

            Schema schema1 = {};
            Schema schema2 = add_table(
                schema1, {"object1", {{"link", PropertyType::Object | PropertyType::Nullable, "embedded1"}}});
            schema2 = add_table(schema2, {"embedded1", ObjectType::Embedded, {{"value", PropertyType::Int}}});
            Schema schema3 =
                add_table(schema2, {"object2", {{"link", PropertyType::Object | PropertyType::Array, "embedded2"}}});
            schema3 = add_table(schema3, {"embedded2", ObjectType::Embedded, {{"value", PropertyType::Int}}});
            REQUIRE_UPDATE_SUCCEEDS(*barq, schema1, 0);
            REQUIRE_UPDATE_SUCCEEDS(*barq, schema2, 0);
            REQUIRE_UPDATE_SUCCEEDS(*barq, schema3, 0);
        }

        SECTION("remove object schema") {
            auto barq = Barq::get_shared_barq(config);

            Schema schema1 = {
                {"object", {{"value", PropertyType::Int}}},
                {"object2", {{"value", PropertyType::Int}}},
            };
            Schema schema2 = remove_table(schema1, "object2");
            Schema schema3 = remove_table(schema2, "object");
            REQUIRE_UPDATE_SUCCEEDS(*barq, schema3, 0);
            REQUIRE_UPDATE_SUCCEEDS(*barq, schema2, 0);
            REQUIRE_UPDATE_SUCCEEDS(*barq, schema1, 0);
        }

        SECTION("add index") {
            auto barq = Barq::get_shared_barq(config);
            Schema schema = {
                {"object", {{"value", PropertyType::Int}}},
            };
            REQUIRE_NO_MIGRATION_NEEDED(*barq, schema, set_indexed(schema, "object", "value", true));
        }

        SECTION("remove index") {
            auto barq = Barq::get_shared_barq(config);
            Schema schema = {
                {"object", {{"value", PropertyType::Int, Property::IsPrimary{false}, Property::IsIndexed{true}}}},
            };
            REQUIRE_NO_MIGRATION_NEEDED(*barq, schema, set_indexed(schema, "object", "value", false));
        }

        SECTION("reordering properties") {
            auto barq = Barq::get_shared_barq(config);

            Schema schema1 = {
                {"object",
                 {
                     {"col1", PropertyType::Int},
                     {"col2", PropertyType::Int},
                 }},
            };
            Schema schema2 = {
                {"object",
                 {
                     {"col2", PropertyType::Int},
                     {"col1", PropertyType::Int},
                 }},
            };
            REQUIRE_NO_MIGRATION_NEEDED(*barq, schema1, schema2);
        }
    }

    SECTION("migration required") {
        SECTION("add property to existing object schema") {
            auto barq = Barq::get_shared_barq(config);

            Schema schema1 = {
                {"object", {{"col1", PropertyType::Int}}},
            };
            auto schema2 = add_property(schema1, "object", {"col2", PropertyType::Int});
            REQUIRE_MIGRATION_NEEDED(*barq, schema1, schema2, "Property 'object.col2' has been added.");
        }

        SECTION("remove property from existing object schema") {
            auto barq = Barq::get_shared_barq(config);
            Schema schema = {
                {"object",
                 {
                     {"col1", PropertyType::Int},
                     {"col2", PropertyType::Int},
                 }},
            };
            REQUIRE_MIGRATION_NEEDED(*barq, schema, remove_property(schema, "object", "col2"),
                                     "Property 'object.col2' has been removed.");
        }

        SECTION("migratation which replaces a persisted property with a computed one") {
            auto barq = Barq::get_shared_barq(config);
            Schema schema1 = {
                {"object",
                 {
                     {"value", PropertyType::Int},
                     {"link", PropertyType::Object | PropertyType::Nullable, "object2"},
                 }},
                {"object2",
                 {
                     {"value", PropertyType::Int},
                     {"inverse", PropertyType::Object | PropertyType::Nullable, "object"},
                 }},
            };
            Schema schema2 = remove_property(schema1, "object", "link");
            Property new_property{"link", PropertyType::LinkingObjects | PropertyType::Array, "object2", "inverse"};
            schema2.find("object")->computed_properties.emplace_back(new_property);

            REQUIRE_UPDATE_SUCCEEDS(*barq, schema1, 0);
            REQUIRE_UPDATE_FAILS(*barq, schema2, "Property 'object.link' has been removed.");
            REQUIRE((*barq).schema() == schema1);
            REQUIRE_MIGRATION_SUCCEEDS(*barq, schema2, 1, [](auto, auto, auto&) {});
        }

        SECTION("change property type") {
            auto barq = Barq::get_shared_barq(config);
            Schema schema = {
                {"object", {{"value", PropertyType::Int}}},
            };
            REQUIRE_MIGRATION_NEEDED(*barq, schema, set_type(schema, "object", "value", PropertyType::Float),
                                     "Property 'object.value' has been changed from 'int' to 'float'.");
        }

        SECTION("make property nullable") {
            auto barq = Barq::get_shared_barq(config);

            Schema schema = {
                {"object", {{"value", PropertyType::Int}}},
            };
            REQUIRE_MIGRATION_NEEDED(*barq, schema, set_optional(schema, "object", "value", true),
                                     "Property 'object.value' has been made optional.");
        }

        SECTION("make property required") {
            auto barq = Barq::get_shared_barq(config);

            Schema schema = {
                {"object", {{"value", PropertyType::Int | PropertyType::Nullable}}},
            };
            REQUIRE_MIGRATION_NEEDED(*barq, schema, set_optional(schema, "object", "value", false),
                                     "Property 'object.value' has been made required.");
        }

        SECTION("change link target") {
            auto barq = Barq::get_shared_barq(config);

            Schema schema = {
                {"target 1", {{"value", PropertyType::Int}}},
                {"target 2", {{"value", PropertyType::Int}}},
                {"origin",
                 {
                     {"value", PropertyType::Object | PropertyType::Nullable, "target 1"},
                 }},
            };
            REQUIRE_MIGRATION_NEEDED(*barq, schema, set_target(schema, "origin", "value", "target 2"),
                                     "Property 'origin.value' has been changed from '<target 1>' to '<target 2>'");
        }

        SECTION("add pk") {
            auto barq = Barq::get_shared_barq(config);

            Schema schema = {
                {"object", {{"value", PropertyType::Int}}},
            };
            REQUIRE_MIGRATION_NEEDED(*barq, schema, set_primary_key(schema, "object", "value"),
                                     "Primary Key for class 'object' has been added.");
        }

        SECTION("remove pk") {
            auto barq = Barq::get_shared_barq(config);

            Schema schema = {
                {"object", {{"value", PropertyType::Int, Property::IsPrimary{true}}}},
            };
            REQUIRE_MIGRATION_NEEDED(*barq, schema, set_primary_key(schema, "object", ""),
                                     "Primary Key for class 'object' has been removed.");
        }

        SECTION("adding column and table in same migration doesn't add duplicate columns") {
            auto barq = Barq::get_shared_barq(config);

            Schema schema1 = {
                {"object",
                 {
                     {"col1", PropertyType::Int},
                 }},
            };
            auto schema2 = add_table(add_property(schema1, "object", {"col2", PropertyType::Int}),
                                     {"object2", {{"value", PropertyType::Int}}});
            REQUIRE_UPDATE_SUCCEEDS(*barq, schema1, 0);
            REQUIRE_UPDATE_SUCCEEDS(*barq, schema2, 1);

            auto& table = *get_table(barq, "object2");
            REQUIRE(table.get_column_count() == 1);
        }

        SECTION("adding column and embedded table in same migration") {
            auto barq = Barq::get_shared_barq(config);

            Schema schema1 = {
                {"object", {{"col1", PropertyType::Int}}},
            };
            auto schema2 = add_table(
                add_property(schema1, "object", {"link", PropertyType::Object | PropertyType::Nullable, "object2"}),
                {"object2", ObjectType::Embedded, {{"value", PropertyType::Int}}});
            REQUIRE_UPDATE_SUCCEEDS(*barq, schema1, 0);
            REQUIRE_UPDATE_SUCCEEDS(*barq, schema2, 1);
        }

        SECTION("change table from embedded to top-level without version bump") {
            auto barq = Barq::get_shared_barq(config);

            Schema schema = {
                {"top", {{"link", PropertyType::Object | PropertyType::Nullable, "object"}}},
                {"object", ObjectType::Embedded, {{"value", PropertyType::Int}}},
            };
            REQUIRE_MIGRATION_NEEDED(*barq, schema, set_table_type(schema, "object", ObjectType::TopLevel),
                                     "Class 'object' has been changed from Embedded to TopLevel.");
        }

        SECTION("change table from top-level to embedded without version bump") {
            auto barq = Barq::get_shared_barq(config);

            Schema schema = {
                {"top", {{"link", PropertyType::Object | PropertyType::Nullable, "object"}}},
                {"object", {{"value", PropertyType::Int}}},
            };
            REQUIRE_MIGRATION_NEEDED(*barq, schema, set_table_type(schema, "object", ObjectType::Embedded),
                                     "Class 'object' has been changed from TopLevel to Embedded.");
        }
    }

    SECTION("migration block invocations") {
        SECTION("not called for initial creation of schema") {
            Schema schema = {
                {"object", {{"value", PropertyType::Int}}},
            };
            auto barq = Barq::get_shared_barq(config);
            barq->update_schema(schema, 5, [](SharedBarq, SharedBarq, Schema&) {
                REQUIRE(false);
            });
        }

        SECTION("not called when schema version is unchanged even if there are schema changes") {
            Schema schema1 = {
                {"object", {{"value", PropertyType::Int}}},
            };
            Schema schema2 = add_table(schema1, {"second object", {{"value", PropertyType::Int}}});
            auto barq = Barq::get_shared_barq(config);
            barq->update_schema(schema1, 1);
            barq->update_schema(schema2, 1, [](SharedBarq, SharedBarq, Schema&) {
                REQUIRE(false);
            });
        }

        SECTION("called when schema version is bumped even if there are no schema changes") {
            Schema schema = {
                {"object", {{"value", PropertyType::Int}}},
            };
            auto barq = Barq::get_shared_barq(config);
            REQUIRE_UPDATE_SUCCEEDS(*barq, schema, 0);
            bool called = false;
            barq->update_schema(schema, 5, [&](SharedBarq, SharedBarq, Schema&) {
                called = true;
            });
            REQUIRE(called);
        }
    }

    SECTION("migration errors") {
        SECTION("schema version cannot go down") {
            auto barq = Barq::get_shared_barq(config);
            barq->update_schema({}, 1);
            barq->update_schema({}, 2);
            REQUIRE_EXCEPTION(barq->update_schema({}, 0), InvalidSchemaVersion,
                              "Provided schema version 0 is less than last set version 2.");
        }

        SECTION("insert duplicate keys for existing PK during migration") {
            Schema schema = {
                {"object", {{"value", PropertyType::Int, Property::IsPrimary{true}}}},
            };
            auto barq = Barq::get_shared_barq(config);
            REQUIRE_UPDATE_SUCCEEDS(*barq, schema, 1);
            REQUIRE_THROWS_CONTAINING(
                barq->update_schema(schema, 2,
                                     [](SharedBarq, SharedBarq barq, Schema&) {
                                         auto table =
                                             ObjectStore::table_for_object_type(barq->read_group(), "object");
                                         table->create_object_with_primary_key(1);
                                         table->create_object_with_primary_key(2).set("value", 1);
                                     }),
                "Primary key property 'object.value' has duplicate values after migration.");
        }

        SECTION("add pk to existing table with duplicate keys") {
            Schema schema = {
                {"object", {{"value", PropertyType::Int}}},
            };
            auto barq = Barq::get_shared_barq(config);
            REQUIRE_UPDATE_SUCCEEDS(*barq, schema, 1);

            barq->begin_transaction();
            auto table = ObjectStore::table_for_object_type(barq->read_group(), "object");
            create_objects(*table, 2);
            barq->commit_transaction();

            schema = set_primary_key(schema, "object", "value");
            REQUIRE_EXCEPTION(barq->update_schema(schema, 2, nullptr), MigrationFailed,
                              "Primary key property 'object.value' has duplicate values after migration.");
        }

        SECTION("throwing an exception from migration function rolls back all changes") {
            Schema schema1 = {
                {"object", {{"value", PropertyType::Int}}},
            };
            Schema schema2 = add_property(schema1, "object", {"value2", PropertyType::Int});
            auto barq = Barq::get_shared_barq(config);
            barq->update_schema(schema1, 1);

            REQUIRE_THROWS_AS(barq->update_schema(schema2, 2,
                                                   [](SharedBarq, SharedBarq barq, Schema&) {
                                                       auto table = ObjectStore::table_for_object_type(
                                                           barq->read_group(), "object");
                                                       table->create_object();
                                                       throw 5;
                                                   }),
                              int);

            auto table = ObjectStore::table_for_object_type(barq->read_group(), "object");
            REQUIRE(table->size() == 0);
            REQUIRE(barq->schema_version() == 1);
            REQUIRE(barq->schema() == schema1);
        }

        SECTION("changing a table to embedded fails if there are any objects in the table and there are no incoming "
                "links to the object type") {
            Schema schema = {{"object", {{"value", PropertyType::Int}}}};
            auto barq = Barq::get_shared_barq(config);
            REQUIRE_UPDATE_SUCCEEDS(*barq, schema, 1);
            auto table = ObjectStore::table_for_object_type(barq->read_group(), "object");
            barq->begin_transaction();
            table->create_object();
            barq->commit_transaction();

            auto new_schema = set_table_type(schema, "object", ObjectType::Embedded);
            REQUIRE_EXCEPTION(barq->update_schema(new_schema, 2, nullptr), IllegalOperation,
                              "Cannot convert 'object' to embedded: at least one object has no incoming links and "
                              "would be deleted.");
            REQUIRE_MIGRATION_SUCCEEDS(*barq, new_schema, 2, [](auto, auto barq, auto&) {
                ObjectStore::table_for_object_type(barq->read_group(), "object")->clear();
            });
        }

        SECTION("changing table to embedded with multiple incoming links fails") {
            Schema schema = {
                {"child", {{"value", PropertyType::Int}}},
                {"parent", {{"link", PropertyType::Object | PropertyType::Nullable, "child"}}},
            };
            auto barq = Barq::get_shared_barq(config);
            REQUIRE_UPDATE_SUCCEEDS(*barq, schema, 1);

            barq->begin_transaction();
            auto child = ObjectStore::table_for_object_type(barq->read_group(), "child");
            auto parent = ObjectStore::table_for_object_type(barq->read_group(), "parent");
            auto child_obj = child->create_object().get_key();
            parent->create_object().set_all(child_obj);
            parent->create_object().set_all(child_obj);
            barq->commit_transaction();

            REQUIRE_EXCEPTION(
                barq->update_schema(set_table_type(schema, "child", ObjectType::Embedded), 2), IllegalOperation,
                "Cannot convert 'child' to embedded: at least one object has more than one incoming link.");
        }

        SECTION("changing table to embedded fails if more links are added inside the migratioon block") {
            Schema schema = {
                {"child", {{"value", PropertyType::Int}}},
                {"parent", {{"link", PropertyType::Object | PropertyType::Nullable, "child"}}},
            };
            auto barq = Barq::get_shared_barq(config);
            REQUIRE_UPDATE_SUCCEEDS(*barq, schema, 1);

            barq->begin_transaction();
            ObjectStore::table_for_object_type(barq->read_group(), "child")->create_object();
            barq->commit_transaction();

            REQUIRE_EXCEPTION(
                barq->update_schema(set_table_type(schema, "child", ObjectType::Embedded), 2,
                                     [](auto, auto new_barq, auto) {
                                         auto child =
                                             ObjectStore::table_for_object_type(new_barq->read_group(), "child");
                                         auto parent =
                                             ObjectStore::table_for_object_type(new_barq->read_group(), "parent");
                                         parent->create_object().set_all(child->get_object(0).get_key());
                                         parent->create_object().set_all(child->get_object(0).get_key());
                                     }),
                IllegalOperation,
                "Cannot convert 'child' to embedded: at least one object has more than one incoming link.");
        }

        SECTION("changing table to embedded fails if there are incoming Mixed linkes") {
            auto type = GENERATE(PropertyType::Array, PropertyType::Set, PropertyType::Dictionary, PropertyType::Int);
            type |= PropertyType::Mixed | PropertyType::Nullable;

            InMemoryTestFile config;
            config.automatically_handle_backlinks_in_migrations = true;
            Schema schema = {
                {"child", {{"value", PropertyType::Int}}},
                {"parent", {{"link", type}}},
            };
            auto barq = Barq::get_shared_barq(config);
            REQUIRE_UPDATE_SUCCEEDS(*barq, schema, 1);

            barq->begin_transaction();
            auto child_table = ObjectStore::table_for_object_type(barq->read_group(), "child");
            auto child = child_table->create_object().set_all(42).get_key();
            auto parent_table = ObjectStore::table_for_object_type(barq->read_group(), "parent");
            auto parent_object = parent_table->create_object();
            ColKey link_col = parent_table->get_column_key("link");

            BARQ_ASSERT(link_col.get_type() == col_type_Mixed);
            Mixed child_link = ObjLink{child_table->get_key(), child};
            if (link_col.is_set()) {
                parent_object.get_set<Mixed>(link_col).insert(child_link);
            }
            else if (link_col.is_list()) {
                parent_object.get_list<Mixed>(link_col).add(child_link);
            }
            else if (link_col.is_dictionary()) {
                parent_object.get_dictionary(link_col).insert("foo", child_link);
            }
            else {
                BARQ_ASSERT(!link_col.is_collection());
                parent_object.set_any(link_col, child_link);
            }
            barq->commit_transaction();

            REQUIRE_EXCEPTION(barq->update_schema(set_table_type(barq->schema(), "child", ObjectType::Embedded), 2),
                              IllegalOperation,
                              "Cannot convert 'child' to embedded: there is an incoming link from the Mixed property "
                              "'parent.link', which does not support linking to embedded objects.");
        }
    }

    SECTION("valid migrations") {
        Schema schema = {{"object", {{"value", PropertyType::Int}}}};

        SECTION("changing all columns does not lose row count") {
            auto barq = Barq::get_shared_barq(config);
            REQUIRE_UPDATE_SUCCEEDS(*barq, schema, 1);

            barq->begin_transaction();
            auto table = ObjectStore::table_for_object_type(barq->read_group(), "object");
            create_objects(*table, 10);
            barq->commit_transaction();

            REQUIRE_UPDATE_SUCCEEDS(*barq, set_type(schema, "object", "value", PropertyType::Float), 2);
            REQUIRE(table->size() == 10);
        }

        SECTION("values for required properties are copied when converitng to nullable") {
            auto barq = Barq::get_shared_barq(config);
            REQUIRE_UPDATE_SUCCEEDS(*barq, schema, 1);

            barq->begin_transaction();
            auto table = ObjectStore::table_for_object_type(barq->read_group(), "object");
            auto key = table->get_column_key("value");
            create_objects(*table, 10);
            for (int i = 0; i < 10; ++i)
                table->get_object(i).set(key, i);
            barq->commit_transaction();

            REQUIRE_UPDATE_SUCCEEDS(*barq, set_optional(schema, "object", "value", true), 2);
            key = table->get_column_key("value");
            for (int i = 0; i < 10; ++i)
                REQUIRE(table->get_object(i).get<util::Optional<int64_t>>(key) == i);
        }

        SECTION("values for nullable properties are discarded when converting to required") {
            auto barq = Barq::get_shared_barq(config);
            REQUIRE_UPDATE_SUCCEEDS(*barq, set_optional(schema, "object", "value", true), 1);

            barq->begin_transaction();
            auto table = ObjectStore::table_for_object_type(barq->read_group(), "object");
            auto key = table->get_column_key("value");
            create_objects(*table, 10);
            for (int i = 0; i < 10; ++i)
                table->get_object(i).set(key, i);
            barq->commit_transaction();

            REQUIRE_UPDATE_SUCCEEDS(*barq, schema, 2);
            key = table->get_column_key("value");
            for (size_t i = 0; i < 10; ++i)
                REQUIRE(table->get_object(i).get<int64_t>(key) == 0);
        }

        SECTION("deleting table removed from the schema deletes it") {
            auto barq = Barq::get_shared_barq(config);
            REQUIRE_UPDATE_SUCCEEDS(*barq, schema, 1);

            REQUIRE_MIGRATION_SUCCEEDS(*barq, Schema{}, 2, [](SharedBarq, SharedBarq barq, Schema&) {
                ObjectStore::delete_data_for_object(barq->read_group(), "object");
            });
            REQUIRE_FALSE(ObjectStore::table_for_object_type(barq->read_group(), "object"));
        }

        SECTION("deleting table still in the schema recreates it with no rows") {
            auto barq = Barq::get_shared_barq(config);
            REQUIRE_UPDATE_SUCCEEDS(*barq, schema, 1);

            barq->begin_transaction();
            ObjectStore::table_for_object_type(barq->read_group(), "object")->create_object();
            barq->commit_transaction();

            REQUIRE_MIGRATION_SUCCEEDS(*barq, schema, 2, [](SharedBarq, SharedBarq barq, Schema&) {
                ObjectStore::delete_data_for_object(barq->read_group(), "object");
            });
            auto table = ObjectStore::table_for_object_type(barq->read_group(), "object");
            REQUIRE(table);
            REQUIRE(table->size() == 0);
        }

        SECTION("deleting table which doesn't exist does nothing") {
            auto barq = Barq::get_shared_barq(config);
            REQUIRE_UPDATE_SUCCEEDS(*barq, schema, 1);

            REQUIRE_MIGRATION_SUCCEEDS(*barq, Schema{}, 2, [](SharedBarq, SharedBarq barq, Schema&) {
                ObjectStore::delete_data_for_object(barq->read_group(), "foo");
            });
        }

        const Schema basic_link_schema = {
            {"child",
             {
                 {"value", PropertyType::Int},
             }},
            {"parent",
             {
                 {"link", PropertyType::Object | PropertyType::Nullable, "child"},
             }},
        };
        const auto basic_embedded_schema = set_table_type(basic_link_schema, "child", ObjectType::Embedded);

        SECTION("changing empty table from top-level to embedded requires a migration") {
            auto barq = Barq::get_shared_barq(config);
            REQUIRE_MIGRATION_NEEDED(*barq, basic_link_schema, basic_embedded_schema,
                                     "Class 'child' has been changed from TopLevel to Embedded.");
        }

        SECTION("changing empty table from embedded to top-level requires a migration") {
            auto barq = Barq::get_shared_barq(config);
            REQUIRE_MIGRATION_NEEDED(*barq, basic_embedded_schema, basic_link_schema,
                                     "Class 'child' has been changed from Embedded to TopLevel.");
        }

        SECTION("changing table to embedded with exactly one incoming link per object works") {
            auto barq = Barq::get_shared_barq(config);
            REQUIRE_UPDATE_SUCCEEDS(*barq, basic_link_schema, 1);

            barq->begin_transaction();
            auto child_table = ObjectStore::table_for_object_type(barq->read_group(), "child");
            ObjKey child1 = child_table->create_object().set_all(42).get_key();
            ObjKey child2 = child_table->create_object().set_all(43).get_key();
            auto parent_table = ObjectStore::table_for_object_type(barq->read_group(), "parent");
            parent_table->create_object().set_all(child1);
            parent_table->create_object().set_all(child2);
            barq->commit_transaction();

            REQUIRE_UPDATE_SUCCEEDS(*barq, basic_embedded_schema, 2);

            REQUIRE(parent_table->size() == 2);
            REQUIRE(child_table->size() == 2);
            int64_t expected = 42;
            for (auto parent : *parent_table) {
                auto child = child_table->get_object(parent.get<ObjKey>("link"));
                REQUIRE(child.get<int64_t>("value") == expected++);
            }
        }

        SECTION("changing table to embedded works if duplicate links were from a removed column") {
            auto barq = Barq::get_shared_barq(config);
            REQUIRE_UPDATE_SUCCEEDS(
                *barq,
                add_property(basic_link_schema, "parent",
                             Property{"link 2", PropertyType::Object | PropertyType::Nullable, "child"}),
                1);

            barq->begin_transaction();
            auto child_table = ObjectStore::table_for_object_type(barq->read_group(), "child");
            ObjKey child_object1 = child_table->create_object().set_all(42).get_key();
            ObjKey child_object2 = child_table->create_object().set_all(43).get_key();
            auto parent_table = ObjectStore::table_for_object_type(barq->read_group(), "parent");
            parent_table->create_object().set_all(child_object1, child_object2);
            parent_table->create_object().set_all(child_object2, child_object1);
            barq->commit_transaction();

            REQUIRE_UPDATE_SUCCEEDS(*barq, basic_embedded_schema, 2);

            REQUIRE(parent_table->size() == 2);
            REQUIRE(child_table->size() == 2);
            int64_t expected = 42;
            for (auto parent : *parent_table) {
                auto child = child_table->get_object(parent.get<ObjKey>("link"));
                REQUIRE(child.get<int64_t>("value") == expected++);
            }
        }

        SECTION("changing table to embedded works if duplicate links are resolved in migration block") {
            auto barq = Barq::get_shared_barq(config);
            REQUIRE_UPDATE_SUCCEEDS(*barq, basic_link_schema, 1);

            barq->begin_transaction();
            auto child_table = ObjectStore::table_for_object_type(barq->read_group(), "child");
            ObjKey child_object = child_table->create_object().set_all(42).get_key();
            auto parent_table = ObjectStore::table_for_object_type(barq->read_group(), "parent");
            parent_table->create_object().set_all(child_object);
            parent_table->create_object().set_all(child_object);
            barq->commit_transaction();

            REQUIRE_THROWS_CONTAINING(
                barq->update_schema(basic_embedded_schema, 2),
                "Cannot convert 'child' to embedded: at least one object has more than one incoming link.");
            REQUIRE_MIGRATION_SUCCEEDS(*barq, basic_embedded_schema, 2, [](auto, auto new_barq, auto&) {
                auto child = ObjectStore::table_for_object_type(new_barq->read_group(), "child");
                auto parent = ObjectStore::table_for_object_type(new_barq->read_group(), "parent");
                parent->get_object(1).set("link", child->create_object().set_all(42).get_key());
            });

            REQUIRE(parent_table->size() == 2);
            REQUIRE(child_table->size() == 2);
            for (auto parent : *parent_table) {
                auto child = child_table->get_object(parent.get<ObjKey>("link"));
                REQUIRE(child.get<int64_t>("value") == 42);
            }
        }

        SECTION("changing table to embedded works if there are backlink columns from a Mixed property but currently "
                "no incoming links") {
            const Schema schema = {
                {"child",
                 {
                     {"value", PropertyType::Int},
                 }},
                {"parent",
                 {
                     {"link", PropertyType::Object | PropertyType::Nullable, "child"},
                     {"mixed", PropertyType::Mixed | PropertyType::Nullable},
                     {"list", PropertyType::Mixed | PropertyType::Nullable | PropertyType::Array},
                     {"set", PropertyType::Mixed | PropertyType::Nullable | PropertyType::Set},
                     {"dictionary", PropertyType::Mixed | PropertyType::Nullable | PropertyType::Dictionary},
                 }},
            };

            auto barq = Barq::get_shared_barq(config);
            REQUIRE_UPDATE_SUCCEEDS(*barq, schema, 1);

            barq->begin_transaction();
            auto child = ObjectStore::table_for_object_type(barq->read_group(), "child");
            auto parent = ObjectStore::table_for_object_type(barq->read_group(), "parent");
            ObjLink child_obj{child->get_key(), child->create_object().get_key()};
            auto parent_obj = parent->create_object().set_all(child_obj.get_obj_key(), Mixed(child_obj));
            parent_obj.get_list<Mixed>("list").add(child_obj);
            parent_obj.get_set<Mixed>("set").insert(child_obj);
            parent_obj.get_dictionary("dictionary").insert("foo", child_obj);
            barq->commit_transaction();

            auto embedded_schema = set_table_type(schema, "child", ObjectType::Embedded);
            REQUIRE_EXCEPTION(barq->update_schema(embedded_schema, 2), IllegalOperation,
                              "Cannot convert 'child' to embedded: there is an incoming link from the Mixed "
                              "property 'parent.mixed', which does not support linking to embedded objects.");

            barq->begin_transaction();
            parent_obj.set_any("mixed", Mixed());
            parent_obj.get_list<Mixed>("list").clear();
            parent_obj.get_set<Mixed>("set").clear();
            parent_obj.get_dictionary("dictionary").clear();
            barq->commit_transaction();

            REQUIRE_UPDATE_SUCCEEDS(*barq, embedded_schema, 2);
        }

        SECTION("automatic migration to embedded deletes objects with no incoming links") {
            config.automatically_handle_backlinks_in_migrations = true;
            config.schema = basic_link_schema;
            auto barq = Barq::get_shared_barq(config);

            auto child_table = ObjectStore::table_for_object_type(barq->read_group(), "child");
            barq->begin_transaction();
            child_table->create_object();
            barq->commit_transaction();

            REQUIRE_UPDATE_SUCCEEDS(*barq, basic_embedded_schema, 1);
            REQUIRE(child_table->size() == 0);
        }

        SECTION("automatic migration to embedded does not modify valid objects") {
            config.automatically_handle_backlinks_in_migrations = true;
            config.schema = basic_link_schema;
            auto barq = Barq::get_shared_barq(config);

            auto child_table = ObjectStore::table_for_object_type(barq->read_group(), "child");
            barq->begin_transaction();
            Obj child_object = child_table->create_object().set_all(42);
            auto parent_table = ObjectStore::table_for_object_type(barq->read_group(), "parent");
            parent_table->create_object().set_all(child_object.get_key());
            barq->commit_transaction();

            REQUIRE_UPDATE_SUCCEEDS(*barq, basic_embedded_schema, 1);
            REQUIRE(parent_table->size() == 1);
            REQUIRE(child_table->size() == 1);
            // Verifies that the existing accessor is still valid
            REQUIRE(child_object.get<int64_t>("value") == 42);
        }

        SECTION("automatic migration to embedded duplicates objects with multiple incoming links") {
            config.automatically_handle_backlinks_in_migrations = true;
            config.schema = basic_link_schema;
            auto barq = Barq::get_shared_barq(config);

            auto child_table = ObjectStore::table_for_object_type(barq->read_group(), "child");
            barq->begin_transaction();
            Obj child_object = child_table->create_object().set_all(42);
            auto parent_table = ObjectStore::table_for_object_type(barq->read_group(), "parent");
            parent_table->create_object().set_all(child_object.get_key());
            parent_table->create_object().set_all(child_object.get_key());
            parent_table->create_object().set_all(child_object.get_key());
            barq->commit_transaction();

            REQUIRE_UPDATE_SUCCEEDS(*barq, basic_embedded_schema, 1);
            REQUIRE(parent_table->size() == 3);
            REQUIRE(child_table->size() == 3);

            // The existing accessor is no longer valid because we delete the original object
            REQUIRE_FALSE(child_object.is_valid());
            for (auto obj : *parent_table) {
                REQUIRE(child_table->get_object(obj.get<ObjKey>("link")).get<int64_t>("value") == 42);
            }
        }
    }

    SECTION("schema correctness during migration") {
        InMemoryTestFile config;
        config.schema_mode = SchemaMode::Automatic;
        auto barq = Barq::get_shared_barq(config);

        Schema schema = {
            {"object",
             {
                 {"pk", PropertyType::Int, Property::IsPrimary{true}},
                 {"value", PropertyType::Int, Property::IsPrimary{false}, Property::IsIndexed{true}},
                 {"optional", PropertyType::Int | PropertyType::Nullable},
                 {"text", PropertyType::String},
             }},
            {"link origin",
             {
                 {"not a pk", PropertyType::Int},
                 {"object", PropertyType::Object | PropertyType::Nullable, "object"},
                 {"array", PropertyType::Array | PropertyType::Object, "object"},
             }},
            {"no pk object",
             {
                 {"value", PropertyType::Int, Property::IsPrimary{false}, Property::IsIndexed{true}},
                 {"optional", PropertyType::Int | PropertyType::Nullable},
             }},
        };
        REQUIRE_UPDATE_SUCCEEDS(*barq, schema, 0);

#define VERIFY_SCHEMA_IN_MIGRATION(target_schema)                                                                    \
    do {                                                                                                             \
        Schema new_schema = (target_schema);                                                                         \
        barq->update_schema(new_schema, 1, [&](SharedBarq old_barq, SharedBarq new_barq, Schema&) {             \
            REQUIRE(old_barq->schema_version() == 0);                                                               \
            REQUIRE(old_barq->schema() == schema);                                                                  \
            REQUIRE(old_barq->schema() != new_schema);                                                              \
            REQUIRE(new_barq->schema_version() == 1);                                                               \
            REQUIRE(new_barq->schema() != schema);                                                                  \
            REQUIRE(new_barq->schema() == new_schema);                                                              \
            VERIFY_SCHEMA(*old_barq, true);                                                                         \
            VERIFY_SCHEMA(*new_barq, true);                                                                         \
        });                                                                                                          \
        REQUIRE(barq->schema() == new_schema);                                                                      \
        VERIFY_SCHEMA(*barq, false);                                                                                \
    } while (false)

        SECTION("add new table") {
            VERIFY_SCHEMA_IN_MIGRATION(add_table(schema, {"new table",
                                                          {
                                                              {"value", PropertyType::Int},
                                                          }}));
        }
        SECTION("add embedded table") {
            VERIFY_SCHEMA_IN_MIGRATION(add_table(
                add_property(schema, "object", {"link", PropertyType::Object | PropertyType::Nullable, "new table"}),
                {"new table",
                 ObjectType::Embedded,
                 {
                     {"value", PropertyType::Int},
                 }}));
        }
        SECTION("change table type") {
            VERIFY_SCHEMA_IN_MIGRATION(
                set_table_type(add_property(schema, "object",
                                            {"link", PropertyType::Object | PropertyType::Nullable, "no pk object"}),
                               "no pk object", ObjectType::Embedded));
        }
        SECTION("add property to table") {
            VERIFY_SCHEMA_IN_MIGRATION(add_property(schema, "object", {"new", PropertyType::Int}));
        }
        SECTION("remove property from table") {
            VERIFY_SCHEMA_IN_MIGRATION(remove_property(schema, "object", "value"));
        }
        SECTION("remove multiple properties from table") {
            VERIFY_SCHEMA_IN_MIGRATION(
                remove_property(remove_property(schema, "object", "value"), "object", "optional"));
        }
        SECTION("add primary key to table") {
            VERIFY_SCHEMA_IN_MIGRATION(set_primary_key(schema, "link origin", "not a pk"));
        }
        SECTION("remove primary key from table") {
            VERIFY_SCHEMA_IN_MIGRATION(set_primary_key(schema, "object", ""));
        }
        SECTION("change primary key") {
            VERIFY_SCHEMA_IN_MIGRATION(set_primary_key(schema, "object", "value"));
        }
        SECTION("change property type") {
            VERIFY_SCHEMA_IN_MIGRATION(set_type(schema, "object", "value", PropertyType::Date));
        }
        SECTION("change link target") {
            VERIFY_SCHEMA_IN_MIGRATION(set_target(schema, "link origin", "object", "link origin"));
        }
        SECTION("change linklist target") {
            VERIFY_SCHEMA_IN_MIGRATION(set_target(schema, "link origin", "array", "link origin"));
        }
        SECTION("make property optional") {
            VERIFY_SCHEMA_IN_MIGRATION(set_optional(schema, "object", "value", true));
        }
        SECTION("make property required") {
            VERIFY_SCHEMA_IN_MIGRATION(set_optional(schema, "object", "optional", false));
        }
        SECTION("add index") {
            VERIFY_SCHEMA_IN_MIGRATION(set_indexed(schema, "object", "optional", true));
        }
        SECTION("add fulltext index") {
            VERIFY_SCHEMA_IN_MIGRATION(set_fulltext_indexed(schema, "object", "text", true));
        }
        SECTION("remove index") {
            VERIFY_SCHEMA_IN_MIGRATION(set_indexed(schema, "object", "value", false));
        }
        SECTION("reorder properties") {
            auto schema2 = schema;
            auto& properties = schema2.find("object")->persisted_properties;
            std::swap(properties[0], properties[1]);
            VERIFY_SCHEMA_IN_MIGRATION(schema2);
        }
        SECTION("change linklist to set") {
            auto schema2 = schema;
            auto prop = schema2.find("link origin")->property_for_name("array");
            prop->type = PropertyType::Set | PropertyType::Object;
            VERIFY_SCHEMA_IN_MIGRATION(schema2);
        }
    }

    SECTION("change nullability and primary key") {
        using namespace std::string_literals;
        Schema schema{{"EmpDetails",
                       {
                           {"UId", PropertyType::String, Property::IsPrimary{true}},
                           {"EmployeeId", PropertyType::String | PropertyType::Nullable},
                           {"Name", PropertyType::String},
                       }}};
        Schema schema2{{"EmpDetails",
                        {
                            {"UId", PropertyType::String},
                            {"EmployeeId", PropertyType::String, Property::IsPrimary{true}},
                            {"Name", PropertyType::String},
                        }}};
        InMemoryTestFile config;
        config.schema_mode = SchemaMode::Automatic;
        config.schema = schema;
        auto barq = Barq::get_shared_barq(config);

        CppContext ctx(barq);
        std::any values = AnyDict{
            {"UId", "ID_001"s},
            {"EmployeeId", "XHGR"s},
            {"Name", "John Doe"s},
        };
        barq->begin_transaction();
        Object::create(ctx, barq, *barq->schema().find("EmpDetails"), values);
        barq->commit_transaction();

        barq->update_schema(schema2, 2, [](auto old_barq, auto new_barq, auto&) {
            // ObjectStore::delete_data_for_object(barq->read_group(), "DetailStudentStatus");
            Object old_obj(old_barq, "EmpDetails", 0);
            Object new_obj(new_barq, "EmpDetails", 0);

            CppContext ctx1(old_barq);
            CppContext ctx2(new_barq);
            auto val = old_obj.get_property_value<std::any>(ctx1, "EmployeeId");
            new_obj.set_property_value(ctx2, "EmployeeId", val);
        });
    }

    SECTION("change primary key from int to string") {
        using namespace std::string_literals;
        Schema schema{{"Foo",
                       {
                           {"_id", PropertyType::Int, Property::IsPrimary{true}},
                           {"value", PropertyType::String},
                       }}};
        Schema schema2{{"Foo",
                        {
                            {"_id", PropertyType::String, Property::IsPrimary{true}},
                            {"value", PropertyType::String},
                        }}};
        TestFile config;
        config.schema_mode = SchemaMode::Automatic;
        config.schema = schema;
        config.schema_version = 1;
        {
            auto barq = Barq::get_shared_barq(config);

            CppContext ctx(barq);
            std::any value1 = AnyDict{
                {"_id", INT64_C(1)},
                {"value", "foo"s},
            };
            std::any value2 = AnyDict{
                {"_id", INT64_C(2)},
                {"value", "bar"s},
            };
            barq->begin_transaction();
            auto s = barq->schema().find("Foo");
            Object::create(ctx, barq, *s, value1);
            Object::create(ctx, barq, *s, value2);
            barq->commit_transaction();
        }
        config.schema = schema2;
        config.schema_version = 2;
        config.migration_function = [](SharedBarq old_barq, SharedBarq barq, Schema&) {
            Results r{Class(old_barq, &*old_barq->schema().find("Foo"))};
            auto sz = r.size();
            auto t = ObjectStore::table_for_object_type(barq->read_group(), "Foo");
            for (size_t i = 0; i < sz; i++) {
                Obj o = r.get(i);
                auto new_obj = t->get_object(o.get_key());
                new_obj.set("_id", util::to_string(o.get<Int>("_id")));
            }
        };
        auto barq = Barq::get_shared_barq(config);
    }

    SECTION("change primary key from string to UUID without migration function") {
        using namespace std::string_literals;
        Schema schema{{"Foo",
                       {
                           {"_id", PropertyType::String, Property::IsPrimary{true}},
                       }}};
        Schema schema2{{"Foo",
                        {
                            {"_id", PropertyType::UUID, Property::IsPrimary{true}},
                        }}};
        InMemoryTestFile config;
        config.schema_mode = SchemaMode::Automatic;
        config.schema = schema;
        auto barq = Barq::get_shared_barq(config);
        barq->update_schema(schema2, 2);

        // Make sure you can actually create an object with UUID as primary key
        CppContext ctx(barq);
        std::any values = AnyDict{
            {"_id", UUID("3b241101-0000-0000-0000-4136c566a964"s)},
        };
        barq->begin_transaction();
        Object::create(ctx, barq, *barq->schema().find("Foo"), values);
        barq->commit_transaction();
    }

    SECTION("object accessors inside migrations") {
        using namespace std::string_literals;

        Schema schema{
            {"all types",
             {
                 {"pk", PropertyType::Int, Property::IsPrimary{true}},
                 {"bool", PropertyType::Bool},
                 {"int", PropertyType::Int},
                 {"float", PropertyType::Float},
                 {"double", PropertyType::Double},
                 {"string", PropertyType::String},
                 {"data", PropertyType::Data},
                 {"date", PropertyType::Date},
                 {"object id", PropertyType::ObjectId},
                 {"decimal", PropertyType::Decimal},
                 {"object", PropertyType::Object | PropertyType::Nullable, "link target"},
                 {"array", PropertyType::Object | PropertyType::Array, "array target"},
             }},
            {"link target",
             {
                 {"value", PropertyType::Int},
             },
             {
                 {"origin", PropertyType::LinkingObjects | PropertyType::Array, "all types", "object"},
             }},
            {"array target",
             {
                 {"value", PropertyType::Int},
             }},
            {"int pk",
             {
                 {"pk", PropertyType::Int, Property::IsPrimary{true}},
                 {"value", PropertyType::Int},
             }},
            {"string pk",
             {
                 {"pk", PropertyType::String, Property::IsPrimary{true}},
                 {"value", PropertyType::Int},
             }},
        };

        InMemoryTestFile config;
        config.schema_mode = SchemaMode::Automatic;
        config.schema = schema;
        auto barq = Barq::get_shared_barq(config);

        CppContext ctx(barq);
        std::any values = AnyDict{
            {"pk", INT64_C(1)},
            {"bool", true},
            {"int", INT64_C(5)},
            {"float", 2.2f},
            {"double", 3.3},
            {"string", "hello"s},
            {"data", "olleh"s},
            {"date", Timestamp(10, 20)},
            {"object id", ObjectId("000000000000000000000001")},
            {"decimal", Decimal128("123.45e6")},
            {"object", AnyDict{{"value", INT64_C(10)}}},
            {"array", AnyVector{AnyDict{{"value", INT64_C(20)}}}},
        };
        barq->begin_transaction();
        Object::create(ctx, barq, *barq->schema().find("all types"), values);
        barq->commit_transaction();

        SECTION("read values from old barq") {
            Schema schema{
                {"all types",
                 {
                     {"pk", PropertyType::Int, Property::IsPrimary{true}},
                 }},
            };
            barq->update_schema(schema, 2, [](auto old_barq, auto new_barq, Schema&) {
                CppContext ctx(old_barq);
                Object obj = Object::get_for_primary_key(ctx, old_barq, "all types", std::any(INT64_C(1)));
                REQUIRE(obj.is_valid());

                REQUIRE(util::any_cast<bool>(obj.get_property_value<std::any>(ctx, "bool")) == true);
                REQUIRE(util::any_cast<int64_t>(obj.get_property_value<std::any>(ctx, "int")) == 5);
                REQUIRE(util::any_cast<float>(obj.get_property_value<std::any>(ctx, "float")) == 2.2f);
                REQUIRE(util::any_cast<double>(obj.get_property_value<std::any>(ctx, "double")) == 3.3);
                REQUIRE(util::any_cast<std::string>(obj.get_property_value<std::any>(ctx, "string")) == "hello");
                REQUIRE(util::any_cast<std::string>(obj.get_property_value<std::any>(ctx, "data")) == "olleh");
                REQUIRE(util::any_cast<Timestamp>(obj.get_property_value<std::any>(ctx, "date")) ==
                        Timestamp(10, 20));
                REQUIRE(util::any_cast<ObjectId>(obj.get_property_value<std::any>(ctx, "object id")) ==
                        ObjectId("000000000000000000000001"));
                REQUIRE(util::any_cast<Decimal128>(obj.get_property_value<std::any>(ctx, "decimal")) ==
                        Decimal128("123.45e6"));

                auto link = util::any_cast<Object>(obj.get_property_value<std::any>(ctx, "object"));
                REQUIRE(link.is_valid());
                REQUIRE(util::any_cast<int64_t>(link.get_property_value<std::any>(ctx, "value")) == 10);

                auto list = util::any_cast<List>(obj.get_property_value<std::any>(ctx, "array"));
                REQUIRE(list.size() == 1);

                CppContext list_ctx(ctx, obj.get_obj(), *obj.get_object_schema().property_for_name("array"));
                link = util::any_cast<Object>(list.get(list_ctx, 0));
                REQUIRE(link.is_valid());
                REQUIRE(util::any_cast<int64_t>(link.get_property_value<std::any>(list_ctx, "value")) == 20);

                CppContext ctx2(new_barq);
                obj = Object::get_for_primary_key(ctx, new_barq, "all types", std::any(INT64_C(1)));
                REQUIRE(obj.is_valid());
                REQUIRE_EXCEPTION(obj.get_property_value<std::any>(ctx, "bool"), InvalidProperty,
                                  "Property 'all types.bool' does not exist");
            });
        }

        SECTION("cannot mutate old barq") {
            barq->update_schema(schema, 2, [](auto old_barq, auto, Schema&) {
                CppContext ctx(old_barq);
                Object obj = Object::get_for_primary_key(ctx, old_barq, "all types", std::any(INT64_C(1)));
                REQUIRE(obj.is_valid());
                REQUIRE_EXCEPTION(obj.set_property_value(ctx, "bool", std::any(false)), WrongTransactionState,
                                  "Cannot modify managed objects outside of a write transaction.");
                REQUIRE_EXCEPTION(old_barq->begin_transaction(), WrongTransactionState,
                                  "Can't perform transactions on read-only Barqs.");
            });
        }

        SECTION("cannot read values for removed properties from new barq") {
            Schema schema{
                {"all types", {{"pk", PropertyType::Int, Property::IsPrimary{true}}}},
            };
            barq->update_schema(schema, 2, [](auto, auto new_barq, Schema&) {
                CppContext ctx(new_barq);
                Object obj = Object::get_for_primary_key(ctx, new_barq, "all types", std::any(INT64_C(1)));
                REQUIRE(obj.is_valid());
                REQUIRE_EXCEPTION(obj.get_property_value<std::any>(ctx, "bool"), InvalidProperty,
                                  "Property 'all types.bool' does not exist");
                REQUIRE_EXCEPTION(obj.get_property_value<std::any>(ctx, "object"), InvalidProperty,
                                  "Property 'all types.object' does not exist");
                REQUIRE_EXCEPTION(obj.get_property_value<std::any>(ctx, "array"), InvalidProperty,
                                  "Property 'all types.array' does not exist");
            });
        }

        SECTION("read values from new object") {
            barq->update_schema(schema, 2, [](auto, auto new_barq, Schema&) {
                CppContext ctx(new_barq);
                Object obj = Object::get_for_primary_key(ctx, new_barq, "all types", std::any(INT64_C(1)));
                REQUIRE(obj.is_valid());

                auto link = util::any_cast<Object>(obj.get_property_value<std::any>(ctx, "object"));
                REQUIRE(link.is_valid());
                REQUIRE(util::any_cast<int64_t>(link.get_property_value<std::any>(ctx, "value")) == 10);

                auto list = util::any_cast<List>(obj.get_property_value<std::any>(ctx, "array"));
                REQUIRE(list.size() == 1);

                CppContext list_ctx(ctx, obj.get_obj(), *obj.get_object_schema().property_for_name("array"));
                link = util::any_cast<Object>(list.get(list_ctx, 0));
                REQUIRE(link.is_valid());
                REQUIRE(util::any_cast<int64_t>(link.get_property_value<std::any>(list_ctx, "value")) == 20);
            });
        }

        SECTION("read and write values in new object") {
            barq->update_schema(schema, 2, [](auto, auto new_barq, Schema&) {
                CppContext ctx(new_barq);
                Object obj = Object::get_for_primary_key(ctx, new_barq, "all types", std::any(INT64_C(1)));
                REQUIRE(obj.is_valid());

                REQUIRE(util::any_cast<bool>(obj.get_property_value<std::any>(ctx, "bool")) == true);
                obj.set_property_value(ctx, "bool", std::any(false));
                REQUIRE(util::any_cast<bool>(obj.get_property_value<std::any>(ctx, "bool")) == false);

                REQUIRE(util::any_cast<int64_t>(obj.get_property_value<std::any>(ctx, "int")) == 5);
                obj.set_property_value(ctx, "int", std::any(INT64_C(6)));
                REQUIRE(util::any_cast<int64_t>(obj.get_property_value<std::any>(ctx, "int")) == 6);

                REQUIRE(util::any_cast<float>(obj.get_property_value<std::any>(ctx, "float")) == 2.2f);
                obj.set_property_value(ctx, "float", std::any(1.23f));
                REQUIRE(util::any_cast<float>(obj.get_property_value<std::any>(ctx, "float")) == 1.23f);

                REQUIRE(util::any_cast<double>(obj.get_property_value<std::any>(ctx, "double")) == 3.3);
                obj.set_property_value(ctx, "double", std::any(1.23));
                REQUIRE(util::any_cast<double>(obj.get_property_value<std::any>(ctx, "double")) == 1.23);

                REQUIRE(util::any_cast<std::string>(obj.get_property_value<std::any>(ctx, "string")) == "hello");
                obj.set_property_value(ctx, "string", std::any("abc"s));
                REQUIRE(util::any_cast<std::string>(obj.get_property_value<std::any>(ctx, "string")) == "abc");

                REQUIRE(util::any_cast<std::string>(obj.get_property_value<std::any>(ctx, "data")) == "olleh");
                obj.set_property_value(ctx, "data", std::any("abc"s));
                REQUIRE(util::any_cast<std::string>(obj.get_property_value<std::any>(ctx, "data")) == "abc");

                REQUIRE(util::any_cast<Timestamp>(obj.get_property_value<std::any>(ctx, "date")) ==
                        Timestamp(10, 20));
                obj.set_property_value(ctx, "date", std::any(Timestamp(1, 2)));
                REQUIRE(util::any_cast<Timestamp>(obj.get_property_value<std::any>(ctx, "date")) == Timestamp(1, 2));

                REQUIRE(util::any_cast<ObjectId>(obj.get_property_value<std::any>(ctx, "object id")) ==
                        ObjectId("000000000000000000000001"));
                ObjectId generated = ObjectId::gen();
                obj.set_property_value(ctx, "object id", std::any(generated));
                REQUIRE(util::any_cast<ObjectId>(obj.get_property_value<std::any>(ctx, "object id")) == generated);

                REQUIRE(util::any_cast<Decimal128>(obj.get_property_value<std::any>(ctx, "decimal")) ==
                        Decimal128("123.45e6"));
                obj.set_property_value(ctx, "decimal", std::any(Decimal128("77.88E-99")));
                REQUIRE(util::any_cast<Decimal128>(obj.get_property_value<std::any>(ctx, "decimal")) ==
                        Decimal128("77.88E-99"));

                Object linked_obj(new_barq, "link target", 0);
                Object new_obj(new_barq, get_table(new_barq, "link target")->create_object());

                auto linking = util::any_cast<Results>(linked_obj.get_property_value<std::any>(ctx, "origin"));
                REQUIRE(linking.size() == 1);

                REQUIRE(util::any_cast<Object>(obj.get_property_value<std::any>(ctx, "object")).get_obj().get_key() ==
                        linked_obj.get_obj().get_key());
                obj.set_property_value(ctx, "object", std::any(new_obj));
                REQUIRE(util::any_cast<Object>(obj.get_property_value<std::any>(ctx, "object")).get_obj().get_key() ==
                        new_obj.get_obj().get_key());

                REQUIRE(linking.size() == 0);
            });
        }

        SECTION("create object in new barq") {
            barq->update_schema(schema, 2, [&values](auto, auto new_barq, Schema&) {
                REQUIRE(new_barq->is_in_transaction());

                CppContext ctx(new_barq);
                util::any_cast<AnyDict&>(values)["pk"] = INT64_C(2);
                Object obj = Object::create(ctx, new_barq, "all types", values);

                REQUIRE(get_table(new_barq, "all types")->size() == 2);
                REQUIRE(get_table(new_barq, "link target")->size() == 2);
                REQUIRE(get_table(new_barq, "array target")->size() == 2);
                REQUIRE(util::any_cast<int64_t>(obj.get_property_value<std::any>(ctx, "pk")) == 2);
            });
        }

        SECTION("upsert in new barq") {
            barq->update_schema(schema, 2, [&values](auto, auto new_barq, Schema&) {
                REQUIRE(new_barq->is_in_transaction());
                CppContext ctx(new_barq);
                util::any_cast<AnyDict&>(values)["bool"] = false;
                Object obj = Object::create(ctx, new_barq, "all types", values, CreatePolicy::UpdateAll);
                REQUIRE(get_table(new_barq, "all types")->size() == 1);
                REQUIRE(get_table(new_barq, "link target")->size() == 2);
                REQUIRE(get_table(new_barq, "array target")->size() == 2);
                REQUIRE(util::any_cast<bool>(obj.get_property_value<std::any>(ctx, "bool")) == false);
            });
        }

        SECTION("upsert in new barq after modifying primary key") {
            barq->update_schema(schema, 2, [&values](auto, auto new_barq, Schema&) {
                get_table(new_barq, "all types")->set_primary_key_column(ColKey());
                REQUIRE(new_barq->is_in_transaction());
                CppContext ctx(new_barq);
                util::any_cast<AnyDict&>(values)["bool"] = false;
                Object obj = Object::create(ctx, new_barq, "all types", values, CreatePolicy::UpdateAll);
                REQUIRE(get_table(new_barq, "all types")->size() == 1);
                REQUIRE(get_table(new_barq, "link target")->size() == 2);
                REQUIRE(get_table(new_barq, "array target")->size() == 2);
                REQUIRE(util::any_cast<bool>(obj.get_property_value<std::any>(ctx, "bool")) == false);
            });
        }

        SECTION("change primary key property type") {
            schema = set_type(schema, "all types", "pk", PropertyType::String);
            barq->update_schema(schema, 2, [](auto, auto new_barq, auto&) {
                Object obj(new_barq, "all types", 0);

                CppContext ctx(new_barq);
                obj.set_property_value(ctx, "pk", std::any("1"s));
            });
        }

        SECTION("set primary key to duplicate values in migration") {
            auto bad_migration = [&](auto, auto new_barq, Schema&) {
                // shoud not be able to create a new object with the same PK
                Object::create(ctx, new_barq, "all types", values);
            };
            REQUIRE_THROWS_AS(barq->update_schema(schema, 2, bad_migration), MigrationFailed);
            REQUIRE(get_table(barq, "all types")->size() == 1);

            auto good_migration = [&](auto, auto new_barq, Schema&) {
                // Change the old object's PK to elminate the duplication
                Object old_obj(new_barq, "all types", 0);
                CppContext ctx(new_barq);
                old_obj.set_property_value(ctx, "pk", std::any(INT64_C(5)));

                REQUIRE_NOTHROW(Object::create(ctx, new_barq, "all types", values));
            };
            REQUIRE_NOTHROW(barq->update_schema(schema, 2, good_migration));
            REQUIRE(get_table(barq, "all types")->size() == 2);
        }

        SECTION("modify existing int primary key values in migration") {
            // Create several more objects to increase the chance of things
            // actually breaking if we're doing invalid things
            CppContext ctx(barq);
            auto object_schema = barq->schema().find("all types");
            barq->begin_transaction();
            for (int i = 1; i < 10; ++i) {
                util::any_cast<AnyDict&>(values)["pk"] = INT64_C(1) + i;
                util::any_cast<AnyDict&>(values)["int"] = INT64_C(5) + i;
                Object::create(ctx, barq, *object_schema, values);
            }
            barq->commit_transaction();

            // Increase the PK of each object by one in a migration
            barq->update_schema(schema, 2, [](auto, auto new_barq, Schema&) {
                CppContext ctx(new_barq);
                Results results(new_barq, get_table(new_barq, "all types"));
                for (size_t i = 0, count = results.size(); i < count; ++i) {
                    Object obj(new_barq, results.get<Obj>(i));
                    std::any v = 1 + util::any_cast<int64_t>(obj.get_property_value<std::any>(ctx, "pk"));
                    obj.set_property_value(ctx, "pk", v);
                }
            });

            // Create a new object with the no-longer-used pk of 1
            barq->begin_transaction();
            util::any_cast<AnyDict&>(values)["pk"] = INT64_C(1);
            util::any_cast<AnyDict&>(values)["int"] = INT64_C(4);
            object_schema = barq->schema().find("all types");
            Object::create(ctx, barq, *object_schema, values);
            barq->commit_transaction();

            // Verify results
            auto table = get_table(barq, "all types");
            REQUIRE(table->size() == 11);
            REQUIRE(table->get_primary_key_column() == table->get_column_key("pk"));
            for (int i = 0; i < 11; ++i) {
                auto obj = table->get_object_with_primary_key(i + 1);
                REQUIRE(obj.get<int64_t>("pk") + 3 == obj.get<int64_t>("int"));
            }
        }

        SECTION("modify existing string primary key values in migration") {
            // Create several objects to increase the chance of things
            // actually breaking if we're doing invalid things
            CppContext ctx(barq);
            auto object_schema = barq->schema().find("string pk");
            barq->begin_transaction();
            for (int64_t i = 0; i < 10; ++i) {
                std::any values = AnyDict{
                    {"pk", util::to_string(i)},
                    {"value", i + 1},
                };
                Object::create(ctx, barq, *object_schema, values);
            }
            barq->commit_transaction();

            // Increase the PK of each object by one in a migration
            barq->update_schema(schema, 2, [](auto, auto new_barq, Schema&) {
                CppContext ctx(new_barq);
                Results results(new_barq, get_table(new_barq, "string pk"));
                for (size_t i = 0, count = results.size(); i < count; ++i) {
                    Object obj(new_barq, results.get<Obj>(i));
                    std::any v =
                        util::to_string(util::any_cast<int64_t>(obj.get_property_value<std::any>(ctx, "value")));
                    obj.set_property_value(ctx, "pk", v);
                }
            });

            // Create a new object with the no-longer-used pk of 0
            barq->begin_transaction();
            std::any values = AnyDict{
                {"pk", "0"s},
                {"value", INT64_C(0)},
            };
            object_schema = barq->schema().find("string pk");
            Object::create(ctx, barq, *object_schema, values);
            barq->commit_transaction();

            // Verify results
            auto table = get_table(barq, "string pk");
            REQUIRE(table->size() == 11);
            REQUIRE(table->get_primary_key_column() == table->get_column_key("pk"));
            for (auto& obj : *table) {
                REQUIRE(util::to_string(obj.get<int64_t>("value")).c_str() == obj.get<StringData>("pk"));
            }
        }

        SECTION("create and modify int primary key inside migration") {
            SECTION("with index") {
                barq->begin_transaction();
                auto table = get_table(barq, "int pk");
                table->add_search_index(table->get_column_key("pk"));
                barq->commit_transaction();
            }
            SECTION("no index") {
            }

            barq->update_schema(schema, 2, [](auto, auto new_barq, Schema&) {
                CppContext ctx(new_barq);
                for (int64_t i = 0; i < 10; ++i) {
                    auto obj = Object::create(ctx, new_barq, *new_barq->schema().find("int pk"),
                                              std::any(AnyDict{{"pk", INT64_C(0)}, {"value", i}}));
                    obj.set_property_value(ctx, "pk", std::any(i));
                }
            });

            auto table = get_table(barq, "int pk");
            REQUIRE(table->size() == 10);
            REQUIRE(table->get_primary_key_column() == table->get_column_key("pk"));
            for (int i = 0; i < 10; ++i) {
                auto obj = table->get_object(i);
                REQUIRE(obj.get<int64_t>("pk") == i);
                REQUIRE(obj.get<int64_t>("value") == i);
            }
        }

        SECTION("create and modify string primary key inside migration") {
            SECTION("with index") {
                barq->begin_transaction();
                auto table = get_table(barq, "string pk");
                table->add_search_index(table->get_column_key("pk"));
                barq->commit_transaction();
            }
            SECTION("no index") {
            }

            barq->update_schema(schema, 2, [](auto, auto new_barq, Schema&) {
                CppContext ctx(new_barq);
                for (int64_t i = 0; i < 10; ++i) {
                    auto obj = Object::create(ctx, new_barq, *new_barq->schema().find("string pk"),
                                              std::any(AnyDict{{"pk", ""s}, {"value", i}}));
                    obj.set_property_value(ctx, "pk", std::any(util::to_string(i)));
                }
            });

            auto table = get_table(barq, "string pk");
            REQUIRE(table->size() == 10);
            REQUIRE(table->get_primary_key_column() == table->get_column_key("pk"));
            for (auto& obj : *table)
                REQUIRE(obj.get<StringData>("pk") == util::to_string(obj.get<int64_t>("value")).c_str());
        }

        SECTION("create object after adding primary key") {
            schema = set_primary_key(schema, "all types", "");
            barq->update_schema(schema, 2);
            schema = set_primary_key(schema, "all types", "pk");
            REQUIRE_NOTHROW(barq->update_schema(schema, 3, [&](auto, auto new_barq, Schema&) {
                CppContext ctx(new_barq);
                util::any_cast<AnyDict&>(values)["pk"] = INT64_C(2);
                Object::create(ctx, barq, "all types", values);
            }));
        }
    }

    SECTION("property renaming") {
        InMemoryTestFile config;
        config.schema_mode = SchemaMode::Automatic;
        auto barq = Barq::get_shared_barq(config);

        struct Rename {
            StringData object_type;
            StringData old_name;
            StringData new_name;
        };

        auto apply_renames = [&](std::initializer_list<Rename> renames) -> MigrationFunction {
            return [=](SharedBarq, SharedBarq barq, Schema& schema) {
                for (auto rename : renames) {
                    ObjectStore::rename_property(barq->read_group(), schema, rename.object_type, rename.old_name,
                                                 rename.new_name);
                }
            };
        };

#define FAILED_RENAME(old_schema, new_schema, error, ...)                                                            \
    do {                                                                                                             \
        barq->update_schema(old_schema, 1);                                                                         \
        REQUIRE_THROWS_WITH(barq->update_schema(new_schema, 2, apply_renames({__VA_ARGS__})), error);               \
    } while (false)

        Schema schema = {
            {"object", {{"value", PropertyType::Int}}},
        };

        SECTION("table does not exist in old schema") {
            auto schema2 = add_table(schema, {"object 2",
                                              {
                                                  {"value 2", PropertyType::Int},
                                              }});
            FAILED_RENAME(schema, schema2, "Cannot rename property 'object 2.value' because it does not exist.",
                          {"object 2", "value", "value 2"});
        }

        SECTION("table does not exist in new schema") {
            FAILED_RENAME(schema, {},
                          "Cannot rename properties for type 'object' because it has been removed from the Barq.",
                          {"object", "value", "value 2"});
        }

        SECTION("property does not exist in old schema") {
            auto schema2 = add_property(schema, "object", {"new", PropertyType::Int});
            FAILED_RENAME(schema, schema2, "Cannot rename property 'object.nonexistent' because it does not exist.",
                          {"object", "nonexistent", "new"});
        }

        auto rename_value = [](Schema schema) {
            schema.find("object")->property_for_name("value")->name = "new";
            return schema;
        };

        SECTION("property does not exist in new schema") {
            FAILED_RENAME(schema, rename_value(schema), "Renamed property 'object.nonexistent' does not exist.",
                          {"object", "value", "nonexistent"});
        }

        SECTION("source propety still exists in the new schema") {
            auto schema2 = add_property(schema, "object", {"new", PropertyType::Int});
            FAILED_RENAME(schema, schema2,
                          "Cannot rename property 'object.value' to 'new' because the source property still exists.",
                          {"object", "value", "new"});
        }

        SECTION("different type") {
            auto schema2 = rename_value(set_type(schema, "object", "value", PropertyType::Date));
            FAILED_RENAME(
                schema, schema2,
                "Cannot rename property 'object.value' to 'new' because it would change from type 'int' to 'date'.",
                {"object", "value", "new"});
        }

        SECTION("different link targets") {
            Schema schema = {
                {"target", {{"value", PropertyType::Int}}},
                {"origin", {{"link", PropertyType::Object | PropertyType::Nullable, "target"}}},
            };
            auto schema2 = set_target(schema, "origin", "link", "origin");
            schema2.find("origin")->property_for_name("link")->name = "new";
            FAILED_RENAME(schema, schema2,
                          "Cannot rename property 'origin.link' to 'new' because it would change from type "
                          "'<target>' to '<origin>'.",
                          {"origin", "link", "new"});
        }

        SECTION("different linklist targets") {
            Schema schema = {
                {"target", {{"value", PropertyType::Int}}},
                {"origin", {{"link", PropertyType::Array | PropertyType::Object, "target"}}},
            };
            auto schema2 = set_target(schema, "origin", "link", "origin");
            schema2.find("origin")->property_for_name("link")->name = "new";
            FAILED_RENAME(schema, schema2,
                          "Cannot rename property 'origin.link' to 'new' because it would change from type "
                          "'array<target>' to 'array<origin>'.",
                          {"origin", "link", "new"});
        }

        SECTION("different object set targets") {
            Schema schema = {
                {"target", {{"value", PropertyType::Int}}},
                {"origin", {{"link", PropertyType::Set | PropertyType::Object, "target"}}},
            };
            auto schema2 = set_target(schema, "origin", "link", "origin");
            schema2.find("origin")->property_for_name("link")->name = "new";
            FAILED_RENAME(schema, schema2,
                          "Cannot rename property 'origin.link' to 'new' because it would change from type "
                          "'set<target>' to 'set<origin>'.",
                          {"origin", "link", "new"});
        }

        SECTION("make required") {
            schema = set_optional(schema, "object", "value", true);
            auto schema2 = rename_value(set_optional(schema, "object", "value", false));
            FAILED_RENAME(
                schema, schema2,
                "Cannot rename property 'object.value' to 'new' because it would change from optional to required.",
                {"object", "value", "new"});
        }

        auto init = [&](Schema const& old_schema) {
            barq->update_schema(old_schema, 1);
            barq->begin_transaction();
            auto table = ObjectStore::table_for_object_type(barq->read_group(), "object");
            auto col = table->get_primary_key_column();
            if (col)
                table->create_object_with_primary_key(10);
            else
                table->create_object().set_all(10);

            barq->commit_transaction();
        };

#define SUCCESSFUL_RENAME(old_schema, new_schema, ...)                                                               \
    do {                                                                                                             \
        init(old_schema);                                                                                            \
        REQUIRE_NOTHROW(barq->update_schema(new_schema, 2, apply_renames({__VA_ARGS__})));                          \
        REQUIRE(barq->schema() == new_schema);                                                                      \
        VERIFY_SCHEMA(*barq, false);                                                                                \
        auto table = ObjectStore::table_for_object_type(barq->read_group(), "object");                              \
        auto key = table->get_column_keys()[0];                                                                      \
        if (table->get_column_attr(key).test(col_attr_Nullable))                                                     \
            REQUIRE(table->begin()->get<util::Optional<int64_t>>(key) == 10);                                        \
        else                                                                                                         \
            REQUIRE(table->begin()->get<int64_t>(key) == 10);                                                        \
    } while (false)

        SECTION("basic valid rename") {
            auto schema2 = rename_value(schema);
            SUCCESSFUL_RENAME(schema, schema2, {"object", "value", "new"});
        }

        SECTION("chained rename") {
            auto schema2 = rename_value(schema);
            SUCCESSFUL_RENAME(schema, schema2, {"object", "value", "a"}, {"object", "a", "b"},
                              {"object", "b", "new"});
        }

        SECTION("old is pk, new is not") {
            auto schema2 = rename_value(schema);
            schema = set_primary_key(schema, "object", "value");
            SUCCESSFUL_RENAME(schema, schema2, {"object", "value", "new"});
        }

        SECTION("new is pk, old is not") {
            auto schema2 = set_primary_key(rename_value(schema), "object", "new");
            SUCCESSFUL_RENAME(schema, schema2, {"object", "value", "new"});
        }

        SECTION("both are pk") {
            schema = set_primary_key(schema, "object", "value");
            auto schema2 = set_primary_key(rename_value(schema), "object", "new");
            SUCCESSFUL_RENAME(schema, schema2, {"object", "value", "new"});
        }

        SECTION("make optional") {
            auto schema2 = rename_value(set_optional(schema, "object", "value", true));
            SUCCESSFUL_RENAME(schema, schema2, {"object", "value", "new"});
        }

        SECTION("add index") {
            auto schema2 = rename_value(set_indexed(schema, "object", "value", true));
            SUCCESSFUL_RENAME(schema, schema2, {"object", "value", "new"});
        }

        SECTION("remove index") {
            auto schema2 = rename_value(schema);
            schema = set_indexed(schema, "object", "value", true);
            SUCCESSFUL_RENAME(schema, schema2, {"object", "value", "new"});
        }

        SECTION("create object inside migration after renaming pk") {
            schema = set_primary_key(schema, "object", "value");
            auto new_schema = set_primary_key(rename_value(schema), "object", "new");
            init(schema);
            REQUIRE_NOTHROW(barq->update_schema(new_schema, 2, [](auto, auto barq, Schema& schema) {
                ObjectStore::rename_property(barq->read_group(), schema, "object", "value", "new");

                CppContext ctx(barq);
                std::any values = AnyDict{{"new", INT64_C(11)}};
                Object::create(ctx, barq, "object", values);
            }));
            REQUIRE(barq->schema() == new_schema);
            VERIFY_SCHEMA(*barq, false);
            auto table = ObjectStore::table_for_object_type(barq->read_group(), "object");
            auto key = table->get_column_keys()[0];
            auto it = table->begin();
            REQUIRE(it->get<int64_t>(key) == 10);
            REQUIRE((++it)->get<int64_t>(key) == 11);
        }
    }
}

TEST_CASE("migration: Immutable", "[migration]") {
    TestFile config;

    auto barq_with_schema = [&](Schema schema) {
        {
            auto barq = Barq::get_shared_barq(config);
            barq->update_schema(std::move(schema));
        }
        config.schema_mode = SchemaMode::Immutable;
        return Barq::get_shared_barq(config);
    };

    SECTION("allowed schema mismatches") {
        SECTION("index") {
            auto barq = barq_with_schema({
                {"object",
                 {
                     {"indexed", PropertyType::Int, Property::IsPrimary{false}, Property::IsIndexed{true}},
                     {"unindexed", PropertyType::Int},
                 }},
            });
            Schema schema = {
                {"object",
                 {
                     {"indexed", PropertyType::Int},
                     {"unindexed", PropertyType::Int, Property::IsPrimary{false}, Property::IsIndexed{true}},
                 }},
            };
            REQUIRE_NOTHROW(barq->update_schema(schema));
            REQUIRE(barq->schema() == schema);
        }

        SECTION("extra tables") {
            auto barq = barq_with_schema({
                {"object", {{"value", PropertyType::Int}}},
                {"object 2", {{"value", PropertyType::Int}}},
            });
            Schema schema = {
                {"object", {{"value", PropertyType::Int}}},
            };
            REQUIRE_NOTHROW(barq->update_schema(schema));
            REQUIRE(barq->schema() == schema);
        }

        SECTION("missing tables") {
            auto barq = barq_with_schema({
                {"object", {{"value", PropertyType::Int}}},
            });
            Schema schema = {
                {"object", {{"value", PropertyType::Int}}},
                {"second object", {{"value", PropertyType::Int}}},
            };
            REQUIRE_NOTHROW(barq->update_schema(schema));
            REQUIRE(barq->schema() == schema);

            auto object_schema = barq->schema().find("object");
            REQUIRE(object_schema->persisted_properties.size() == 1);
            REQUIRE(object_schema->persisted_properties[0].column_key);

            object_schema = barq->schema().find("second object");
            REQUIRE(object_schema->persisted_properties.size() == 1);
            REQUIRE(!object_schema->persisted_properties[0].column_key);
        }

        SECTION("extra columns in table") {
            auto barq = barq_with_schema({
                {"object",
                 {
                     {"value", PropertyType::Int},
                     {"value 2", PropertyType::Int},
                 }},
            });
            Schema schema = {
                {"object", {{"value", PropertyType::Int}}},
            };
            REQUIRE_NOTHROW(barq->update_schema(schema));
            REQUIRE(barq->schema() == schema);
        }

        SECTION("differing embeddedness") {
            auto barq = barq_with_schema({
                {"top", {{"link", PropertyType::Object | PropertyType::Nullable, "object"}}},
                {"object", {{"value", PropertyType::Int}}},
            });
            Schema schema = set_table_type(barq->schema(), "object", ObjectType::Embedded);
            REQUIRE_NOTHROW(barq->update_schema(schema));
            REQUIRE(barq->schema() == schema);
        }
    }

    SECTION("disallowed mismatches") {
        SECTION("missing columns in table") {
            auto barq = barq_with_schema({
                {"object", {{"value", PropertyType::Int}}},
            });
            Schema schema = {
                {"object",
                 {
                     {"value", PropertyType::Int},
                     {"value 2", PropertyType::Int},
                 }},
            };
            INVALID_SCHEMA_CHANGE(*barq, schema, "Property 'object.value 2' has been added.");
        }

        SECTION("bump schema version") {
            Schema schema = {
                {"object", {{"value", PropertyType::Int}}},
            };
            auto barq = barq_with_schema(schema);
            REQUIRE_EXCEPTION(barq->update_schema(schema, 1), InvalidSchemaVersion,
                              "Provided schema version 1 does not equal last set version 0.");
        }
    }
}

TEST_CASE("migration: ReadOnly", "[migration]") {
    TestFile config;

    auto barq_with_schema = [&](Schema schema) {
        {
            auto barq = Barq::get_shared_barq(config);
            REQUIRE_UPDATE_SUCCEEDS(*barq, schema, 0);
        }
        config.schema_mode = SchemaMode::ReadOnly;
        return Barq::get_shared_barq(config);
    };

    SECTION("allowed schema mismatches") {
        SECTION("index") {
            auto barq = barq_with_schema({
                {"object",
                 {
                     {"indexed", PropertyType::Int, Property::IsPrimary{false}, Property::IsIndexed{true}},
                     {"unindexed", PropertyType::Int},
                 }},
            });
            Schema schema = {
                {"object",
                 {
                     {"indexed", PropertyType::Int},
                     {"unindexed", PropertyType::Int, Property::IsPrimary{false}, Property::IsIndexed{true}},
                 }},
            };
            REQUIRE_NOTHROW(barq->update_schema(schema));
        }

        SECTION("extra tables") {
            auto barq = barq_with_schema({
                {"object", {{"value", PropertyType::Int}}},
                {"object 2", {{"value", PropertyType::Int}}},
            });
            Schema schema = {
                {"object", {{"value", PropertyType::Int}}},
            };
            REQUIRE_NOTHROW(barq->update_schema(schema));
        }

        SECTION("extra columns in table") {
            auto barq = barq_with_schema({
                {"object",
                 {
                     {"value", PropertyType::Int},
                     {"value 2", PropertyType::Int},
                 }},
            });
            Schema schema = {
                {"object", {{"value", PropertyType::Int}}},
            };
            REQUIRE_NOTHROW(barq->update_schema(schema));
        }

        SECTION("missing tables") {
            auto barq = barq_with_schema({
                {"object", {{"value", PropertyType::Int}}},
            });
            Schema schema = {
                {"object", {{"value", PropertyType::Int}}},
                {"second object", {{"value", PropertyType::Int}}},
            };
            REQUIRE_NOTHROW(barq->update_schema(schema));
        }

        SECTION("bump schema version") {
            Schema schema = {
                {"object", {{"value", PropertyType::Int}}},
            };
            auto barq = barq_with_schema(schema);
            REQUIRE_NOTHROW(barq->update_schema(schema, 1));
        }

        SECTION("differing embeddedness") {
            Schema schema = {
                {"top", {{"link", PropertyType::Object | PropertyType::Nullable, "object"}}},
                {"object", {{"value", PropertyType::Int}}},
            };
            auto barq = barq_with_schema(schema);
            REQUIRE_NOTHROW(barq->update_schema(set_table_type(barq->schema(), "object", ObjectType::Embedded)));
        }
    }

    SECTION("disallowed mismatches") {
        SECTION("missing columns in table") {
            auto barq = barq_with_schema({
                {"object", {{"value", PropertyType::Int}}},
            });
            Schema schema = {
                {"object",
                 {
                     {"value", PropertyType::Int},
                     {"value 2", PropertyType::Int},
                 }},
            };
            INVALID_SCHEMA_CHANGE(*barq, schema, "Property 'object.value 2' has been added.");
        }
    }
}

TEST_CASE("migration: SoftResetFile", "[migration]") {
    TestFile config;
    config.schema_mode = SchemaMode::SoftResetFile;

    Schema schema = {
        {"object", {{"value", PropertyType::Int}}},
        {"object 2", {{"value", PropertyType::Int}}},
    };

    auto get_fileid = [&] {
        auto id = util::File::get_unique_id(config.path);
        REQUIRE(id);
        return *id;
    };
// To verify that the file has actually be deleted and recreated, on
// non-Windows we need to hold an open file handle to the old file to force
// using a new inode, but on Windows we *can't*
#ifndef _WIN32
    util::File holder(config.path, util::File::mode_Write);
#endif

    {
        auto barq = Barq::get_shared_barq(config);
        auto ino = get_fileid();
        REQUIRE_UPDATE_SUCCEEDS(*barq, schema, 0);
        REQUIRE(ino == get_fileid());
        barq->begin_transaction();
        ObjectStore::table_for_object_type(barq->read_group(), "object")->create_object();
        barq->commit_transaction();
    }
    SharedBarq barq;
    if (!config.needs_file_format_upgrade()) {
        barq = Barq::get_shared_barq(config);
    }
    REQUIRE(barq);
    auto ino = get_fileid();

    SECTION("file is reset when schema version increases") {
        REQUIRE_UPDATE_SUCCEEDS(*barq, schema, 1);
        REQUIRE(ObjectStore::table_for_object_type(barq->read_group(), "object")->size() == 0);
        REQUIRE(ino != get_fileid());
    }

    SECTION("file is reset when an existing table is modified") {
        REQUIRE_UPDATE_SUCCEEDS(*barq, add_property(schema, "object", {"value 2", PropertyType::Int}), 0);
        REQUIRE(ObjectStore::table_for_object_type(barq->read_group(), "object")->size() == 0);
        REQUIRE(ino != get_fileid());
    }

    SECTION("file is not reset when adding a new table") {
        REQUIRE_UPDATE_SUCCEEDS(*barq, add_table(schema, {"object 3", {{"value", PropertyType::Int}}}), 0);
        REQUIRE(ObjectStore::table_for_object_type(barq->read_group(), "object")->size() == 1);
        REQUIRE(barq->schema().size() == 3);
        REQUIRE(ino == get_fileid());
    }

    SECTION("file is not reset when removing a table") {
        REQUIRE_UPDATE_SUCCEEDS(*barq, remove_table(schema, "object 2"), 0);
        REQUIRE(ObjectStore::table_for_object_type(barq->read_group(), "object")->size() == 1);
        REQUIRE(ObjectStore::table_for_object_type(barq->read_group(), "object 2"));
        REQUIRE(barq->schema().size() == 1);
        REQUIRE(ino == get_fileid());
    }

    SECTION("file is not reset when adding an index") {
        REQUIRE_UPDATE_SUCCEEDS(*barq, set_indexed(schema, "object", "value", true), 0);
        REQUIRE(ObjectStore::table_for_object_type(barq->read_group(), "object")->size() == 1);
        REQUIRE(ino == get_fileid());
    }

    SECTION("file is not reset when removing an index") {
        REQUIRE_UPDATE_SUCCEEDS(*barq, set_indexed(schema, "object", "value", true), 0);
        REQUIRE_UPDATE_SUCCEEDS(*barq, schema, 0);
        REQUIRE(ObjectStore::table_for_object_type(barq->read_group(), "object")->size() == 1);
        REQUIRE(ino == get_fileid());
    }
}

TEST_CASE("migration: HardResetFile", "[migration]") {
    TestFile config;

    Schema schema = {
        {"object", {{"value", PropertyType::Int}}},
        {"object 2", {{"value", PropertyType::Int}}},
    };

    auto get_fileid = [&] {
        auto id = util::File::get_unique_id(config.path);
        REQUIRE(id);
        return *id;
    };
#ifndef _WIN32
    util::File holder(config.path, util::File::mode_Write);
#endif

    {
        auto barq = Barq::get_shared_barq(config);
        auto ino = get_fileid();
        REQUIRE_UPDATE_SUCCEEDS(*barq, schema, 0);
        REQUIRE(ino == get_fileid());
        barq->begin_transaction();
        ObjectStore::table_for_object_type(barq->read_group(), "object")->create_object();
        barq->commit_transaction();
    }
    config.schema_mode = SchemaMode::HardResetFile;
    auto barq = Barq::get_shared_barq(config);
    auto ino = get_fileid();

    SECTION("file is reset when schema version increases") {
        REQUIRE_UPDATE_SUCCEEDS(*barq, schema, 1);
        REQUIRE(ObjectStore::table_for_object_type(barq->read_group(), "object")->size() == 0);
        REQUIRE(ino != get_fileid());
    }

    SECTION("file is reset when an existing table is modified") {
        barq->update_schema(add_property(schema, "object", {"value 2", PropertyType::Int}));
        REQUIRE(ObjectStore::table_for_object_type(barq->read_group(), "object")->size() == 0);
        REQUIRE(ino != get_fileid());
    }

    SECTION("file is reset when adding a new table") {
        barq->update_schema(add_table(schema, {"object 3", {{"value", PropertyType::Int}}}));
        REQUIRE(ObjectStore::table_for_object_type(barq->read_group(), "object")->size() == 0);
        REQUIRE(ino != get_fileid());
    }
}

TEST_CASE("migration: Additive", "[migration]") {
    Schema schema = {
        {"object",
         {
             {"value", PropertyType::Int, Property::IsPrimary{false}, Property::IsIndexed{true}},
             {"value 2", PropertyType::Int | PropertyType::Nullable},
         }},
    };

    TestFile config;
    config.schema = schema;
    config.schema_mode = GENERATE(SchemaMode::AdditiveDiscovered, SchemaMode::AdditiveExplicit);
    auto barq = Barq::get_shared_barq(config);
    REQUIRE_UPDATE_SUCCEEDS(*barq, schema, 0);

    INFO((config.schema_mode == SchemaMode::AdditiveDiscovered ? "AdditiveDiscovered" : "AdditiveExplicit"));

    SECTION("can add new properties to existing tables") {
        REQUIRE_NOTHROW(barq->update_schema(add_property(schema, "object", {"value 3", PropertyType::Int})));
        REQUIRE(ObjectStore::table_for_object_type(barq->read_group(), "object")->get_column_count() == 3);
    }

    SECTION("can add new tables") {
        REQUIRE_NOTHROW(barq->update_schema(add_table(schema, {"object 2",
                                                                {
                                                                    {"value", PropertyType::Int},
                                                                }})));
        REQUIRE(ObjectStore::table_for_object_type(barq->read_group(), "object"));
        REQUIRE(ObjectStore::table_for_object_type(barq->read_group(), "object 2"));
    }

    SECTION("embedded orphan types") {
        if (config.schema_mode == SchemaMode::AdditiveDiscovered) {
            // in discovered mode, adding embedded orphan types is allowed but ignored
            REQUIRE_NOTHROW(barq->update_schema(
                add_table(schema, {"origin",
                                   ObjectType::Embedded,
                                   {{"link", PropertyType::Object | PropertyType::Nullable, "object"}}})));
            REQUIRE(ObjectStore::table_for_object_type(barq->read_group(), "object"));
            REQUIRE(!ObjectStore::table_for_object_type(barq->read_group(), "origin"));
        }
    }

    SECTION("cannot change existing table type") {
        Schema schema = {
            {"child", {{"value", PropertyType::Int}}},
            {"parent", {{"link", PropertyType::Object | PropertyType::Nullable, "child"}}},
        };
        REQUIRE_UPDATE_SUCCEEDS(*barq, schema, 0);
        REQUIRE_THROWS_CONTAINING(barq->update_schema(set_table_type(schema, "child", ObjectType::Embedded)),
                                  "Class 'child' has been changed from TopLevel to Embedded.");
    }

    SECTION("indexes are updated when schema version is bumped") {
        auto table = ObjectStore::table_for_object_type(barq->read_group(), "object");
        auto col_keys = table->get_column_keys();
        REQUIRE(table->has_search_index(col_keys[0]));
        REQUIRE(!table->has_search_index(col_keys[1]));

        REQUIRE_NOTHROW(barq->update_schema(set_indexed(schema, "object", "value", false), 1));
        REQUIRE(!table->has_search_index(col_keys[0]));

        REQUIRE_NOTHROW(barq->update_schema(set_indexed(schema, "object", "value 2", true), 2));
        REQUIRE(table->has_search_index(col_keys[1]));
    }

    SECTION("indexes are not updated when schema version is not bumped") {
        auto table = ObjectStore::table_for_object_type(barq->read_group(), "object");
        auto col_keys = table->get_column_keys();
        REQUIRE(table->has_search_index(col_keys[0]));
        REQUIRE(!table->has_search_index(col_keys[1]));

        REQUIRE_NOTHROW(barq->update_schema(set_indexed(schema, "object", "value", false)));
        REQUIRE(table->has_search_index(col_keys[0]));

        REQUIRE_NOTHROW(barq->update_schema(set_indexed(schema, "object", "value 2", true)));
        REQUIRE(!table->has_search_index(col_keys[1]));
    }

    SECTION("can remove properties from existing tables, but column is not removed") {
        auto table = ObjectStore::table_for_object_type(barq->read_group(), "object");
        REQUIRE_NOTHROW(barq->update_schema(remove_property(schema, "object", "value")));
        REQUIRE(ObjectStore::table_for_object_type(barq->read_group(), "object")->get_column_count() == 2);
        auto const& properties = barq->schema().find("object")->persisted_properties;
        REQUIRE(properties.size() == 1);
        auto col_keys = table->get_column_keys();
        REQUIRE(col_keys.size() == 2);
        REQUIRE(properties[0].column_key == col_keys[1]);
    }

    SECTION("cannot change existing property types") {
        REQUIRE_THROWS_CONTAINING(barq->update_schema(set_type(schema, "object", "value", PropertyType::String)),
                                  "Property 'object.value' has been changed from 'int' to 'string'.");
    }

    SECTION("cannot change existing property nullability") {
        REQUIRE_THROWS_CONTAINING(barq->update_schema(set_optional(schema, "object", "value", true)),
                                  "Property 'object.value' has been made optional.");
        REQUIRE_THROWS_CONTAINING(barq->update_schema(set_optional(schema, "object", "value 2", false)),
                                  "Property 'object.value 2' has been made required.");
    }

    SECTION("cannot change existing link targets") {
        REQUIRE_NOTHROW(barq->update_schema(
            add_table(schema, {"object 2",
                               {
                                   {"link", PropertyType::Object | PropertyType::Nullable, "object"},
                               }})));
        REQUIRE_THROWS_CONTAINING(barq->update_schema(set_target(barq->schema(), "object 2", "link", "object 2")),
                                  "Property 'object 2.link' has been changed from '<object>' to '<object 2>'.");
    }

    SECTION("cannot change primary keys") {
        REQUIRE_THROWS_CONTAINING(barq->update_schema(set_primary_key(schema, "object", "value")),
                                  "Primary Key for class 'object' has been added.");

        REQUIRE_NOTHROW(
            barq->update_schema(add_table(schema, {"object 2",
                                                    {
                                                        {"pk", PropertyType::Int, Property::IsPrimary{true}},
                                                    }})));

        REQUIRE_THROWS_CONTAINING(barq->update_schema(set_primary_key(barq->schema(), "object 2", "")),
                                  "Primary Key for class 'object 2' has been removed.");
    }

    SECTION("schema version is allowed to go down") {
        REQUIRE_NOTHROW(barq->update_schema(schema, 1));
        REQUIRE(barq->schema_version() == 1);
        REQUIRE_NOTHROW(barq->update_schema(schema, 0));
        REQUIRE(barq->schema_version() == 0);
    }

    SECTION("migration function is not used") {
        REQUIRE_NOTHROW(barq->update_schema(schema, 1, [&](SharedBarq, SharedBarq, Schema&) {
            REQUIRE(false);
        }));
    }

    SECTION("new columns added externally are ignored") {
        auto barq2 = Barq::get_shared_barq(config);
        auto& group = barq2->read_group();
        barq2->begin_transaction();
        auto table = ObjectStore::table_for_object_type(group, "object");
        auto col_keys = table->get_column_keys();
        table->add_column(type_Int, "new column");
        barq2->commit_transaction();

        REQUIRE_NOTHROW(barq->refresh());
        REQUIRE(barq->schema() == schema);
        REQUIRE(barq->schema().find("object")->persisted_properties[0].column_key == col_keys[0]);
        REQUIRE(barq->schema().find("object")->persisted_properties[1].column_key == col_keys[1]);

        auto frozen = barq->freeze();
        REQUIRE(frozen->schema() == schema);
        REQUIRE(frozen->schema().find("object")->persisted_properties[0].column_key == col_keys[0]);
        REQUIRE(frozen->schema().find("object")->persisted_properties[1].column_key == col_keys[1]);
    }

    SECTION("opening new Barqs uses the correct schema after an external change") {
        auto barq2 = Barq::get_shared_barq(config);
        auto& group = barq2->read_group();
        barq2->begin_transaction();
        auto table = ObjectStore::table_for_object_type(group, "object");
        auto col_keys = table->get_column_keys();
        table->add_column(type_Double, "newcol");
        barq2->commit_transaction();

        REQUIRE_NOTHROW(barq->refresh());
        REQUIRE(barq->schema() == schema);
        REQUIRE(barq->schema().find("object")->persisted_properties[0].column_key == col_keys[0]);
        REQUIRE(barq->schema().find("object")->persisted_properties[1].column_key == col_keys[1]);

        // Gets the schema from the BarqCoordinator
        auto barq3 = Barq::get_shared_barq(config);
        REQUIRE(barq->schema().find("object")->persisted_properties[0].column_key == col_keys[0]);
        REQUIRE(barq->schema().find("object")->persisted_properties[1].column_key == col_keys[1]);

        ColKey k0 = col_keys[0], k1 = col_keys[1];

        // Close and re-open the file entirely so that the coordinator is recreated
        barq.reset();
        barq2.reset();
        barq3.reset();

        barq = Barq::get_shared_barq(config);
        REQUIRE(barq->schema() == schema);
        REQUIRE(barq->schema().find("object")->persisted_properties[0].column_key == k0);
        REQUIRE(barq->schema().find("object")->persisted_properties[1].column_key == k1);
    }

    SECTION("obtaining a frozen Barq from before an external schema change") {
        auto barq2 = Barq::get_shared_barq(config);
        barq->read_group();
        barq2->read_group();
        auto table = ObjectStore::table_for_object_type(barq->read_group(), "object");
        auto col_keys = table->get_column_keys();

        {
            auto write_barq = Barq::get_shared_barq(config);
            write_barq->begin_transaction();
            auto table = ObjectStore::table_for_object_type(write_barq->read_group(), "object");
            table->add_column(type_Double, "newcol");
            write_barq->commit_transaction();
        }

        // Before refreshing when we haven't seen the new version at all
        auto frozen = barq->freeze();
        REQUIRE(frozen->schema() == schema);
        REQUIRE(frozen->schema().find("object")->persisted_properties[0].column_key == col_keys[0]);
        REQUIRE(frozen->schema().find("object")->persisted_properties[1].column_key == col_keys[1]);
        frozen = Barq::get_frozen_barq(config, barq->read_transaction_version());
        REQUIRE(frozen->schema() == schema);
        REQUIRE(frozen->schema().find("object")->persisted_properties[0].column_key == col_keys[0]);
        REQUIRE(frozen->schema().find("object")->persisted_properties[1].column_key == col_keys[1]);

        // Refresh the other instance so that the schema cache is updated, and
        // then repeat
        barq2->refresh();

        frozen = barq->freeze();
        REQUIRE(frozen->schema() == schema);
        REQUIRE(frozen->schema().find("object")->persisted_properties[0].column_key == col_keys[0]);
        REQUIRE(frozen->schema().find("object")->persisted_properties[1].column_key == col_keys[1]);
        frozen = Barq::get_frozen_barq(config, barq->read_transaction_version());
        REQUIRE(frozen->schema() == schema);
        REQUIRE(frozen->schema().find("object")->persisted_properties[0].column_key == col_keys[0]);
        REQUIRE(frozen->schema().find("object")->persisted_properties[1].column_key == col_keys[1]);
    }

    SECTION("can have different subsets of columns in different Barq instances") {
        Barq::Config config2 = config;
        config2.schema = add_property(schema, "object", {"value 3", PropertyType::Int});
        Barq::Config config3 = config;
        config3.schema = remove_property(schema, "object", "value 2");

        Barq::Config config4 = config;
        config4.schema = util::none;

        auto barq2 = Barq::get_shared_barq(config2);
        auto barq3 = Barq::get_shared_barq(config3);
        REQUIRE(barq->schema().find("object")->persisted_properties.size() == 2);
        REQUIRE(barq2->schema().find("object")->persisted_properties.size() == 3);
        REQUIRE(barq3->schema().find("object")->persisted_properties.size() == 1);

        barq->refresh();
        barq2->refresh();
        REQUIRE(barq->schema().find("object")->persisted_properties.size() == 2);
        REQUIRE(barq2->schema().find("object")->persisted_properties.size() == 3);

        // No schema specified; should see all of them
        auto barq4 = Barq::get_shared_barq(config4);
        REQUIRE(barq4->schema().find("object")->persisted_properties.size() == 3);
    }

    SECTION("updating a schema to include already-present column") {
        Barq::Config config2 = config;
        config2.schema = add_property(schema, "object", {"value 3", PropertyType::Int});
        auto barq2 = Barq::get_shared_barq(config2);
        auto& properties2 = barq2->schema().find("object")->persisted_properties;

        REQUIRE_NOTHROW(barq->update_schema(*config2.schema));
        REQUIRE(barq->schema().find("object")->persisted_properties.size() == 3);
        auto& properties = barq->schema().find("object")->persisted_properties;
        REQUIRE(properties[0].column_key == properties2[0].column_key);
        REQUIRE(properties[1].column_key == properties2[1].column_key);
        REQUIRE(properties[2].column_key == properties2[2].column_key);
    }

    SECTION("increasing schema version without modifying schema properly leaves the schema untouched") {
        TestFile config1;
        config1.schema = schema;
        config1.schema_mode = SchemaMode::AdditiveDiscovered;
        config1.schema_version = 0;

        auto barq1 = Barq::get_shared_barq(config1);
        REQUIRE(barq1->schema().size() == 1);
        Schema schema1 = barq1->schema();
        barq1->close();

        Barq::Config config2 = config1;
        config2.schema_version = 1;
        auto barq2 = Barq::get_shared_barq(config2);
        REQUIRE(barq2->schema() == schema1);
    }

    SECTION("invalid schema update leaves the schema untouched") {
        Barq::Config config2 = config;
        config2.schema = add_property(schema, "object", {"value 3", PropertyType::Int});
        auto barq2 = Barq::get_shared_barq(config2);

        REQUIRE_THROWS_CONTAINING(
            barq->update_schema(add_property(schema, "object", {"value 3", PropertyType::Float})),
            "Property 'object.value 3' has been changed from 'int' to 'float'.");
        REQUIRE(barq->schema().find("object")->persisted_properties.size() == 2);
    }

    SECTION("update_schema() does not begin a write transaction when extra columns are present") {
        barq->begin_transaction();

        auto barq2 = Barq::get_shared_barq(config);
        // will deadlock if it tries to start a write transaction
        barq2->update_schema(remove_property(schema, "object", "value"));
    }

    SECTION("update_schema() does not begin a write transaction when indexes are changed without bumping schema "
            "version") {
        barq->begin_transaction();

        auto barq2 = Barq::get_shared_barq(config);
        // will deadlock if it tries to start a write transaction
        barq->update_schema(set_indexed(schema, "object", "value 2", true));
    }

    SECTION("update_schema() does not begin a write transaction for invalid schema changes") {
        barq->begin_transaction();

        auto barq2 = Barq::get_shared_barq(config);
        auto new_schema =
            add_property(remove_property(schema, "object", "value"), "object", {"value", PropertyType::Float});
        // will deadlock if it tries to start a write transaction
        REQUIRE_THROWS_CONTAINING(barq2->update_schema(new_schema),
                                  "Property 'object.value' has been changed from 'int' to 'float'.");
    }
}

TEST_CASE("migration: Manual", "[migration]") {
    TestFile config;
    config.schema_mode = SchemaMode::Manual;
    auto barq = Barq::get_shared_barq(config);

    Schema schema = {{"object",
                      {
                          {"pk", PropertyType::Int, Property::IsPrimary{true}},
                          {"value", PropertyType::Int, Property::IsPrimary{false}, Property::IsIndexed{true}},
                          {"optional", PropertyType::Int | PropertyType::Nullable},
                      }},
                     {"link origin",
                      {
                          {"not a pk", PropertyType::Int},
                          {"object", PropertyType::Object | PropertyType::Nullable, "object"},
                          {"array", PropertyType::Array | PropertyType::Object, "object"},
                      }}};
    REQUIRE_UPDATE_SUCCEEDS(*barq, schema, 0);
    auto col_keys = barq->read_group().get_table("class_object")->get_column_keys();

#define REQUIRE_MIGRATION(schema, migration, msg)                                                                    \
    do {                                                                                                             \
        Schema new_schema = (schema);                                                                                \
        REQUIRE_EXCEPTION(barq->update_schema(new_schema), SchemaMismatch,                                          \
                          Catch::Matchers::ContainsSubstring(msg));                                                  \
        REQUIRE(barq->schema_version() == 0);                                                                       \
        REQUIRE_EXCEPTION(barq->update_schema(new_schema, 1, [](SharedBarq, SharedBarq, Schema&) {}),             \
                          SchemaMismatch, Catch::Matchers::ContainsSubstring(msg));                                  \
        REQUIRE(barq->schema_version() == 0);                                                                       \
        REQUIRE_NOTHROW(barq->update_schema(new_schema, 1, migration));                                             \
        REQUIRE(barq->schema_version() == 1);                                                                       \
    } while (false)

    SECTION("add new table") {
        REQUIRE_MIGRATION(
            add_table(schema, {"new table", {{"value", PropertyType::Int}}}),
            [](SharedBarq, SharedBarq barq, Schema&) {
                barq->read_group().add_table("class_new table")->add_column(type_Int, "value");
            },
            "Class 'new table' has been added.");
    }
    SECTION("add property to table") {
        REQUIRE_MIGRATION(
            add_property(schema, "object", {"new", PropertyType::Int}),
            [&](SharedBarq, SharedBarq barq, Schema&) {
                get_table(barq, "object")->add_column(type_Int, "new");
            },
            "Property 'object.new' has been added.");
    }
    SECTION("remove property from table") {
        REQUIRE_MIGRATION(
            remove_property(schema, "object", "value"),
            [&](SharedBarq, SharedBarq barq, Schema&) {
                get_table(barq, "object")->remove_column(col_keys[1]);
            },
            "Property 'object.value' has been removed.");
    }
    SECTION("add primary key to table") {
        REQUIRE_MIGRATION(
            set_primary_key(schema, "link origin", "not a pk"),
            [&](SharedBarq, SharedBarq barq, Schema&) {
                auto table = get_table(barq, "link origin");
                table->set_primary_key_column(table->get_column_key("not a pk"));
            },
            "Primary Key for class 'link origin' has been added.");
    }
    SECTION("remove primary key from table") {
        REQUIRE_MIGRATION(
            set_primary_key(schema, "object", ""),
            [&](SharedBarq, SharedBarq barq, Schema&) {
                get_table(barq, "object")->set_primary_key_column({});
            },
            "Primary Key for class 'object' has been removed.");
    }
    SECTION("change primary key") {
        REQUIRE_MIGRATION(
            set_primary_key(schema, "object", "value"),
            [&](SharedBarq, SharedBarq barq, Schema&) {
                get_table(barq, "object")->set_primary_key_column(col_keys[1]);
            },
            "Primary Key for class 'object' has changed from 'pk' to 'value'.");
    }
    SECTION("change property type") {
        REQUIRE_MIGRATION(
            set_type(schema, "object", "value", PropertyType::Date),
            [&](SharedBarq, SharedBarq barq, Schema&) {
                auto table = get_table(barq, "object");
                table->remove_column(col_keys[1]);
                auto col = table->add_column(type_Timestamp, "value");
                table->add_search_index(col);
            },
            "Property 'object.value' has been changed from 'int' to 'date'.");
    }
    SECTION("change link target") {
        REQUIRE_MIGRATION(
            set_target(schema, "link origin", "object", "link origin"),
            [&](SharedBarq, SharedBarq barq, Schema&) {
                auto table = get_table(barq, "link origin");
                table->remove_column(table->get_column_keys()[1]);
                table->add_column(*table, "object");
            },
            "Property 'link origin.object' has been changed from '<object>' to '<link origin>'.");
    }
    SECTION("change linklist target") {
        REQUIRE_MIGRATION(
            set_target(schema, "link origin", "array", "link origin"),
            [&](SharedBarq, SharedBarq barq, Schema&) {
                auto table = get_table(barq, "link origin");
                table->remove_column(table->get_column_keys()[2]);
                table->add_column_list(*table, "array");
            },
            "Property 'link origin.array' has been changed from 'array<object>' to 'array<link origin>'.");
    }
    SECTION("make property optional") {
        REQUIRE_MIGRATION(
            set_optional(schema, "object", "value", true),
            [&](SharedBarq, SharedBarq barq, Schema&) {
                auto table = get_table(barq, "object");
                table->remove_column(col_keys[1]);
                auto col = table->add_column(type_Int, "value", true);
                table->add_search_index(col);
            },
            "Property 'object.value' has been made optional.");
    }
    SECTION("make property required") {
        REQUIRE_MIGRATION(
            set_optional(schema, "object", "optional", false),
            [&](SharedBarq, SharedBarq barq, Schema&) {
                auto table = get_table(barq, "object");
                table->remove_column(col_keys[2]);
                table->add_column(type_Int, "optional", false);
            },
            "Property 'object.optional' has been made required.");
    }
    SECTION("add index") {
        REQUIRE_MIGRATION(
            set_indexed(schema, "object", "optional", true),
            [&](SharedBarq, SharedBarq barq, Schema&) {
                get_table(barq, "object")->add_search_index(col_keys[2]);
            },
            "Property 'object.optional' has been made indexed.");
    }
    SECTION("remove index") {
        REQUIRE_MIGRATION(
            set_indexed(schema, "object", "value", false),
            [&](SharedBarq, SharedBarq barq, Schema&) {
                get_table(barq, "object")->remove_search_index(col_keys[1]);
            },
            "Property 'object.value' has been made unindexed.");
    }
    SECTION("reorder properties") {
        auto schema2 = schema;
        auto& properties = schema2.find("object")->persisted_properties;
        std::swap(properties[0], properties[1]);
        REQUIRE_NOTHROW(barq->update_schema(schema2));
    }

    SECTION("cannot lower schema version") {
        REQUIRE_NOTHROW(barq->update_schema(schema, 1, [](SharedBarq, SharedBarq, Schema&) {}));
        REQUIRE(barq->schema_version() == 1);
        REQUIRE_EXCEPTION(barq->update_schema(schema, 0, [](SharedBarq, SharedBarq, Schema&) {}),
                          InvalidSchemaVersion, "Provided schema version 0 is less than last set version 1.");
        REQUIRE(barq->schema_version() == 1);
    }

    SECTION("update_schema() does not begin a write transaction when schema version is unchanged") {
        barq->begin_transaction();

        auto barq2 = Barq::get_shared_barq(config);
        // will deadlock if it tries to start a write transaction
        REQUIRE_NOTHROW(barq2->update_schema(schema));
        REQUIRE_UPDATE_FAILS(*barq2, remove_property(schema, "object", "value"),
                             "Property 'object.value' has been removed.");
    }

    SECTION("null migration callback should throw SchemaMismatchException") {
        Schema new_schema = remove_property(schema, "object", "value");
        REQUIRE_EXCEPTION(barq->update_schema(new_schema, 1, nullptr), SchemaMismatch,
                          Catch::Matchers::ContainsSubstring("Property 'object.value' has been removed."));
    }
}
