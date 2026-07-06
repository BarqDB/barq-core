#if defined(NDEBUG)
#undef NDEBUG
#endif

#include <barq.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK_ERROR()                                                                                                \
    do {                                                                                                             \
        barq_error_t err;                                                                                           \
        if (barq_get_last_error(&err)) {                                                                            \
            fprintf(stderr, "ERROR: %s\n", err.message);                                                             \
            return 1;                                                                                                \
        }                                                                                                            \
    } while (0)

static void check_property_info_equal(const barq_property_info_t* lhs, const barq_property_info_t* rhs)
{
    assert(strcmp(lhs->name, rhs->name) == 0);
    assert(strcmp(lhs->public_name, rhs->public_name) == 0);
    assert(lhs->type == rhs->type);
    assert(lhs->collection_type == rhs->collection_type);
    assert(strcmp(lhs->link_target, rhs->link_target) == 0);
    assert(strcmp(lhs->link_origin_property_name, rhs->link_origin_property_name) == 0);
    assert(lhs->key == rhs->key);
    assert(lhs->flags == rhs->flags);
}

int barq_c_api_tests(const char* file);
int barq_c_api_tests(const char* file)
{
    const barq_class_info_t def_classes[] = {
        {
            .name = "Foo",
            .primary_key = "",
            .num_properties = 3,
            .num_computed_properties = 0,
            .key = BARQ_INVALID_CLASS_KEY,
            .flags = BARQ_CLASS_NORMAL,
        },
        {
            .name = "Bar",
            .primary_key = "int",
            .num_properties = 2,
            .num_computed_properties = 0,
            .key = BARQ_INVALID_CLASS_KEY,
            .flags = BARQ_CLASS_NORMAL,
        },
    };

    const barq_property_info_t def_foo_properties[] = {
        {
            .name = "int",
            .public_name = "",
            .type = BARQ_PROPERTY_TYPE_INT,
            .collection_type = BARQ_COLLECTION_TYPE_NONE,
            .link_target = "",
            .link_origin_property_name = "",
            .key = BARQ_INVALID_PROPERTY_KEY,
            .flags = BARQ_PROPERTY_NORMAL,
        },
        {
            .name = "str",
            .public_name = "",
            .type = BARQ_PROPERTY_TYPE_STRING,
            .collection_type = BARQ_COLLECTION_TYPE_NONE,
            .link_target = "",
            .link_origin_property_name = "",
            .key = BARQ_INVALID_PROPERTY_KEY,
            .flags = BARQ_PROPERTY_NORMAL,
        },
        {
            .name = "bars",
            .public_name = "",
            .type = BARQ_PROPERTY_TYPE_OBJECT,
            .collection_type = BARQ_COLLECTION_TYPE_LIST,
            .link_target = "Bar",
            .link_origin_property_name = "",
            .key = BARQ_INVALID_PROPERTY_KEY,
            .flags = BARQ_PROPERTY_NORMAL,
        },
    };

    const barq_property_info_t def_bar_properties[] = {
        {
            .name = "int",
            .public_name = "",
            .type = BARQ_PROPERTY_TYPE_INT,
            .collection_type = BARQ_COLLECTION_TYPE_NONE,
            .link_target = "",
            .link_origin_property_name = "",
            .key = BARQ_INVALID_PROPERTY_KEY,
            .flags = BARQ_PROPERTY_INDEXED | BARQ_PROPERTY_PRIMARY_KEY,
        },
        {
            .name = "strings",
            .public_name = "",
            .type = BARQ_PROPERTY_TYPE_STRING,
            .collection_type = BARQ_COLLECTION_TYPE_LIST,
            .link_target = "",
            .link_origin_property_name = "",
            .key = BARQ_INVALID_PROPERTY_KEY,
            .flags = BARQ_PROPERTY_NORMAL | BARQ_PROPERTY_NULLABLE,
        },
    };

    const barq_property_info_t* def_class_properties[] = {def_foo_properties, def_bar_properties};

    barq_schema_t* schema = barq_schema_new(def_classes, 2, def_class_properties);
    CHECK_ERROR();

    barq_config_t* config = barq_config_new();
    barq_config_set_schema(config, schema);
    barq_config_set_schema_mode(config, BARQ_SCHEMA_MODE_AUTOMATIC);
    barq_config_set_schema_version(config, 1);
    barq_config_set_path(config, file);

    barq_t* barq = barq_open(config);
    CHECK_ERROR();
    barq_release(config);
    barq_release(schema);

    assert(!barq_is_frozen(barq));
    assert(!barq_is_closed(barq));
    assert(!barq_is_writable(barq));

    {
        barq_begin_write(barq);
        CHECK_ERROR();
        assert(barq_is_writable(barq));
        barq_rollback(barq);
        CHECK_ERROR();
    }

    size_t num_classes = barq_get_num_classes(barq);
    assert(num_classes == 2);

    barq_class_key_t class_keys[2];
    size_t n;
    barq_get_class_keys(barq, class_keys, 2, &n);
    CHECK_ERROR();
    assert(n == 2);

    bool found = false;
    barq_class_info_t foo_info;
    barq_class_info_t bar_info;

    barq_find_class(barq, "Foo", &found, &foo_info);
    CHECK_ERROR();
    assert(found);
    assert(foo_info.num_properties == 3);
    assert(foo_info.key == class_keys[0] || foo_info.key == class_keys[1]);

    barq_find_class(barq, "Bar", &found, &bar_info);
    CHECK_ERROR();
    assert(found);
    assert(bar_info.num_properties == 2);
    assert(bar_info.key == class_keys[0] || bar_info.key == class_keys[1]);

    barq_class_info_t dummy_info;
    barq_find_class(barq, "DoesNotExist", &found, &dummy_info);
    CHECK_ERROR();
    assert(!found);

    barq_property_info_t* foo_properties = malloc(sizeof(barq_property_info_t) * foo_info.num_properties);
    barq_property_info_t* bar_properties = malloc(sizeof(barq_property_info_t) * bar_info.num_properties);

    barq_get_class_properties(barq, foo_info.key, foo_properties, foo_info.num_properties, NULL);
    CHECK_ERROR();
    barq_get_class_properties(barq, bar_info.key, bar_properties, bar_info.num_properties, NULL);
    CHECK_ERROR();

    // Find properties by name.
    barq_property_info_t foo_int, foo_str, foo_bars, bar_int, bar_strings;
    barq_find_property(barq, foo_info.key, "int", &found, &foo_int);
    CHECK_ERROR();
    assert(found);
    barq_find_property(barq, foo_info.key, "str", &found, &foo_str);
    CHECK_ERROR();
    assert(found);
    barq_find_property(barq, foo_info.key, "bars", &found, &foo_bars);
    CHECK_ERROR();
    assert(found);
    barq_find_property(barq, bar_info.key, "int", &found, &bar_int);
    CHECK_ERROR();
    assert(found);
    barq_find_property(barq, bar_info.key, "strings", &found, &bar_strings);
    CHECK_ERROR();
    assert(found);

    check_property_info_equal(&foo_int, &foo_properties[0]);
    check_property_info_equal(&foo_str, &foo_properties[1]);
    check_property_info_equal(&foo_bars, &foo_properties[2]);
    check_property_info_equal(&bar_int, &bar_properties[0]);
    check_property_info_equal(&bar_strings, &bar_properties[1]);


    // Find properties by key.
    {
        barq_property_info_t foo_int, foo_str, foo_bars, bar_int, bar_strings;

        barq_get_property(barq, foo_info.key, foo_properties[0].key, &foo_int);
        CHECK_ERROR();
        assert(found);
        barq_get_property(barq, foo_info.key, foo_properties[1].key, &foo_str);
        CHECK_ERROR();
        assert(found);
        barq_get_property(barq, foo_info.key, foo_properties[2].key, &foo_bars);
        CHECK_ERROR();
        assert(found);
        barq_get_property(barq, bar_info.key, bar_properties[0].key, &bar_int);
        CHECK_ERROR();
        assert(found);
        barq_get_property(barq, bar_info.key, bar_properties[1].key, &bar_strings);
        CHECK_ERROR();
        assert(found);

        check_property_info_equal(&foo_int, &foo_properties[0]);
        check_property_info_equal(&foo_str, &foo_properties[1]);
        check_property_info_equal(&foo_bars, &foo_properties[2]);
        check_property_info_equal(&bar_int, &bar_properties[0]);
        check_property_info_equal(&bar_strings, &bar_properties[1]);
    }

    size_t num_foos, num_bars;
    barq_get_num_objects(barq, foo_info.key, &num_foos);
    CHECK_ERROR();
    assert(num_foos == 0);
    barq_get_num_objects(barq, bar_info.key, &num_bars);
    CHECK_ERROR();
    assert(num_bars == 0);

    bool did_refresh = false;
    assert(barq_refresh(barq, &did_refresh));
    CHECK_ERROR();
    assert(!did_refresh);

    barq_object_create(barq, foo_info.key);
    barq_error_t err;
    assert(barq_get_last_error(&err));
    assert(err.error == BARQ_ERR_WRONG_TRANSACTION_STATE);
    barq_clear_last_error();

    barq_object_t* foo_1;
    {
        barq_begin_write(barq);
        CHECK_ERROR();

        foo_1 = barq_object_create(barq, foo_info.key);
        CHECK_ERROR();
        assert(barq_object_is_valid(foo_1));

        barq_object_key_t foo_1_key = barq_object_get_key(foo_1);

        barq_class_key_t foo_1_table = barq_object_get_table(foo_1);
        assert(foo_1_table == foo_info.key);

        barq_link_t foo_1_link = barq_object_as_link(foo_1);
        assert(foo_1_link.target == foo_1_key);
        assert(foo_1_link.target_table == foo_1_table);

        barq_commit(barq);
        CHECK_ERROR();
    }

    assert(barq_object_is_valid(foo_1));

    barq_release(foo_1);

    barq_close(barq);
    CHECK_ERROR();
    assert(barq_is_closed(barq));

    barq_release(barq);
    CHECK_ERROR();

    free(foo_properties);
    free(bar_properties);

    return 0;
}
