/*************************************************************************
 *
 * Copyright 2024 Realm Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 **************************************************************************/

#ifndef BARQ_H
#define BARQ_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <barq/error_codes.h>

#if defined(_WIN32) || defined(__CYGWIN__)

#if defined(Barq_EXPORTS)
// Exporting Win32 symbols
#define BARQ_EXPORT __declspec(dllexport)
#else
// Importing Win32 symbols. Note: Clients linking statically should define
// BARQ_NO_DLLIMPORT.
#if !defined(BARQ_NO_DLLIMPORT)
#define BARQ_EXPORT __declspec(dllimport)
#else
#define BARQ_EXPORT
#endif // BARQ_NO_DLLIMPORT
#endif // Barq_EXPORTS

#else
// Not Win32
#define BARQ_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
#define BARQ_API extern "C" BARQ_EXPORT
#define BARQ_API_NOEXCEPT noexcept
#else
#define BARQ_API BARQ_EXPORT
#define BARQ_API_NOEXCEPT
#endif // __cplusplus

// Some platforms don't support anonymous unions in structs.
// BARQ_NO_ANON_UNIONS allows definining a member name for unions in structs where
// BARQ_ANON_UNION_MEMBER(name) is used.
#ifdef BARQ_NO_ANON_UNIONS
#define BARQ_ANON_UNION_MEMBER(name) name
#else
#define BARQ_ANON_UNION_MEMBER(name)
#endif

// Some platforms can benefit from redefining the userdata type to another type known to the tooling.
// For example, Dart with its ffigen utility can generate cleaner code if we define barq_userdata_t as Dart_Handle,
// which is a pointer to an opaque struct treated specially by the Dart code generator.
// WARNING: only define this to a pointer type, anything else breaks the ABI.
#ifndef barq_userdata_t
#define barq_userdata_t void*
#endif

typedef struct shared_barq barq_t;
typedef struct barq_schema barq_schema_t;
typedef struct barq_scheduler barq_scheduler_t;
typedef struct barq_work_queue barq_work_queue_t;
typedef struct barq_thread_safe_reference barq_thread_safe_reference_t;
typedef void (*barq_free_userdata_func_t)(barq_userdata_t userdata);
typedef barq_userdata_t (*barq_clone_userdata_func_t)(const barq_userdata_t userdata);
typedef void (*barq_on_object_store_thread_callback_t)(barq_userdata_t userdata);
typedef bool (*barq_on_object_store_error_callback_t)(barq_userdata_t userdata, const char*);
typedef struct barq_key_path_array barq_key_path_array_t;

/* Accessor types */
typedef struct barq_object barq_object_t;

typedef struct barq_list barq_list_t;
typedef struct barq_set barq_set_t;
typedef struct barq_dictionary barq_dictionary_t;

/* Query types */
typedef struct barq_query barq_query_t;
typedef struct barq_results barq_results_t;

/* Config types */
typedef struct barq_config barq_config_t;
typedef struct barq_sync_client_config barq_sync_client_config_t;
typedef struct barq_sync_config barq_sync_config_t;
typedef bool (*barq_migration_func_t)(barq_userdata_t userdata, barq_t* old_barq, barq_t* new_barq,
                                       const barq_schema_t* schema);
typedef bool (*barq_data_initialization_func_t)(barq_userdata_t userdata, barq_t* barq);
typedef bool (*barq_should_compact_on_launch_func_t)(barq_userdata_t userdata, uint64_t total_bytes,
                                                      uint64_t used_bytes);

typedef enum barq_schema_mode {
    BARQ_SCHEMA_MODE_AUTOMATIC,
    BARQ_SCHEMA_MODE_IMMUTABLE,
    BARQ_SCHEMA_MODE_READ_ONLY,
    BARQ_SCHEMA_MODE_SOFT_RESET_FILE,
    BARQ_SCHEMA_MODE_HARD_RESET_FILE,
    BARQ_SCHEMA_MODE_ADDITIVE_DISCOVERED,
    BARQ_SCHEMA_MODE_ADDITIVE_EXPLICIT,
    BARQ_SCHEMA_MODE_MANUAL,
} barq_schema_mode_e;

typedef enum barq_schema_subset_mode {
    BARQ_SCHEMA_SUBSET_MODE_STRICT,
    BARQ_SCHEMA_SUBSET_MODE_ALL_CLASSES,
    BARQ_SCHEMA_SUBSET_MODE_ALL_PROPERTIES,
    BARQ_SCHEMA_SUBSET_MODE_COMPLETE
} barq_schema_subset_mode_e;

/* Key types */
typedef uint32_t barq_class_key_t;
typedef int64_t barq_property_key_t;
typedef int64_t barq_object_key_t;
typedef uint64_t barq_version_t;

static const barq_class_key_t BARQ_INVALID_CLASS_KEY = ((uint32_t)-1) >> 1;
static const barq_property_key_t BARQ_INVALID_PROPERTY_KEY = -1;
static const barq_object_key_t BARQ_INVALID_OBJECT_KEY = -1;

/* Value types */

typedef enum barq_value_type {
    BARQ_TYPE_NULL,
    BARQ_TYPE_INT,
    BARQ_TYPE_BOOL,
    BARQ_TYPE_STRING,
    BARQ_TYPE_BINARY,
    BARQ_TYPE_TIMESTAMP,
    BARQ_TYPE_FLOAT,
    BARQ_TYPE_DOUBLE,
    BARQ_TYPE_DECIMAL128,
    BARQ_TYPE_OBJECT_ID,
    BARQ_TYPE_LINK,
    BARQ_TYPE_UUID,
    BARQ_TYPE_LIST,
    BARQ_TYPE_DICTIONARY,
} barq_value_type_e;

typedef enum barq_schema_validation_mode {
    BARQ_SCHEMA_VALIDATION_BASIC = 0,
    BARQ_SCHEMA_VALIDATION_SYNC_PBS = 1,
    BARQ_SCHEMA_VALIDATION_REJECT_EMBEDDED_ORPHANS = 2,
    BARQ_SCHEMA_VALIDATION_SYNC_FLX = 4
} barq_schema_validation_mode_e;

/**
 * Represents a view over a UTF-8 string buffer. The buffer is unowned by this struct.
 *
 * This string can have three states:
 * - null
 *   When the data member is NULL.
 * - empty
 *   When the data member is non-NULL, and the size member is 0. The actual contents of the data member in this case
 * don't matter.
 * - non-empty
 *   When the data member is non-NULL, and the size member is greater than 0.
 *
 */
typedef struct barq_string {
    const char* data;
    size_t size;
} barq_string_t;

typedef struct barq_binary {
    const uint8_t* data;
    size_t size;
} barq_binary_t;

typedef struct barq_timestamp {
    int64_t seconds;
    int32_t nanoseconds;
} barq_timestamp_t;

typedef struct barq_decimal128 {
    uint64_t w[2];
} barq_decimal128_t;

typedef struct barq_link {
    barq_class_key_t target_table;
    barq_object_key_t target;
} barq_link_t;

typedef struct barq_object_id {
    uint8_t bytes[12];
} barq_object_id_t;

typedef struct barq_uuid {
    uint8_t bytes[16];
} barq_uuid_t;

BARQ_API bool barq_decimal128_from_string(const char* value, barq_decimal128_t* out_decimal);
BARQ_API char* barq_decimal128_to_string(barq_decimal128_t value);

typedef struct barq_value {
    union {
        int64_t integer;
        bool boolean;
        barq_string_t string;
        barq_binary_t binary;
        barq_timestamp_t timestamp;
        float fnum;
        double dnum;
        barq_decimal128_t decimal128;
        barq_object_id_t object_id;
        barq_uuid_t uuid;
        barq_link_t link;

        char data[16];
    } BARQ_ANON_UNION_MEMBER(values);
    barq_value_type_e type;
} barq_value_t;
typedef struct barq_query_arg {
    size_t nb_args;
    bool is_list;
    barq_value_t* arg;
} barq_query_arg_t;

typedef struct barq_version_id {
    uint64_t version;
    uint64_t index;
} barq_version_id_t;


/* Error types */
typedef struct barq_async_error barq_async_error_t;
typedef unsigned barq_error_categories;

typedef struct barq_error {
    barq_errno_e error;
    barq_error_categories categories;
    const char* message;
    // When error is BARQ_ERR_CALLBACK this is an opaque pointer to an SDK-owned error object
    // thrown by user code inside a callback with barq_register_user_code_callback_error(), otherwise null.
    void* user_code_error;
    const char* path;
} barq_error_t;

/* Schema types */

typedef enum barq_column_attr {
    // Values matching `barq::ColumnAttr`.
    BARQ_COLUMN_ATTR_NONE = 0,
    BARQ_COLUMN_ATTR_INDEXED = 1,
    BARQ_COLUMN_ATTR_UNIQUE = 2,
    BARQ_COLUMN_ATTR_RESERVED = 4,
    BARQ_COLUMN_ATTR_STRONG_LINKS = 8,
    BARQ_COLUMN_ATTR_NULLABLE = 16,
    BARQ_COLUMN_ATTR_LIST = 32,
    BARQ_COLUMN_ATTR_DICTIONARY = 64,
    BARQ_COLUMN_ATTR_COLLECTION = 64 + 32,
} barq_column_attr_e;

typedef enum barq_property_type {
    // Values matching `barq::ColumnType`.
    BARQ_PROPERTY_TYPE_INT = 0,
    BARQ_PROPERTY_TYPE_BOOL = 1,
    BARQ_PROPERTY_TYPE_STRING = 2,
    BARQ_PROPERTY_TYPE_BINARY = 4,
    BARQ_PROPERTY_TYPE_MIXED = 6,
    BARQ_PROPERTY_TYPE_TIMESTAMP = 8,
    BARQ_PROPERTY_TYPE_FLOAT = 9,
    BARQ_PROPERTY_TYPE_DOUBLE = 10,
    BARQ_PROPERTY_TYPE_DECIMAL128 = 11,
    BARQ_PROPERTY_TYPE_OBJECT = 12,
    BARQ_PROPERTY_TYPE_LINKING_OBJECTS = 14,
    BARQ_PROPERTY_TYPE_OBJECT_ID = 15,
    BARQ_PROPERTY_TYPE_UUID = 17,
} barq_property_type_e;

typedef enum barq_collection_type {
    BARQ_COLLECTION_TYPE_NONE = 0,
    BARQ_COLLECTION_TYPE_LIST = 1,
    BARQ_COLLECTION_TYPE_SET = 2,
    BARQ_COLLECTION_TYPE_DICTIONARY = 4,
} barq_collection_type_e;

typedef struct barq_property_info {
    const char* name;
    const char* public_name;
    barq_property_type_e type;
    barq_collection_type_e collection_type;

    const char* link_target;
    const char* link_origin_property_name;
    barq_property_key_t key;
    int flags;
} barq_property_info_t;

typedef struct barq_class_info {
    const char* name;
    const char* primary_key;
    size_t num_properties;
    size_t num_computed_properties;
    barq_class_key_t key;
    int flags;
} barq_class_info_t;

typedef enum barq_class_flags {
    BARQ_CLASS_NORMAL = 0,
    BARQ_CLASS_EMBEDDED = 1,
    BARQ_CLASS_ASYMMETRIC = 2,
    BARQ_CLASS_MASK = 3,
} barq_class_flags_e;

typedef enum barq_property_flags {
    BARQ_PROPERTY_NORMAL = 0,
    BARQ_PROPERTY_NULLABLE = 1,
    BARQ_PROPERTY_PRIMARY_KEY = 2,
    BARQ_PROPERTY_INDEXED = 4,
    BARQ_PROPERTY_FULLTEXT_INDEXED = 8,
} barq_property_flags_e;

typedef enum barq_vector_metric {
    // Values matching `barq::VectorMetric`.
    BARQ_VECTOR_METRIC_INNER_PRODUCT = 0,
    BARQ_VECTOR_METRIC_L2 = 1,
    BARQ_VECTOR_METRIC_COSINE = 2,
} barq_vector_metric_e;

typedef enum barq_vector_encoding {
    // Values matching `barq::VectorEncoding`.
    BARQ_VECTOR_ENCODING_FLOAT32 = 0,
    BARQ_VECTOR_ENCODING_SQ8 = 1,
} barq_vector_encoding_e;

// Mirrors `barq::VectorIndexConfig`. `dimensions` of 0 means "infer from the
// first inserted vector"; `ef_search` of 0 means "use the index default".
typedef struct barq_vector_index_config {
    barq_vector_metric_e metric;
    barq_vector_encoding_e encoding;
    size_t dimensions;
    size_t m;
    size_t ef_construction;
    size_t ef_search;
    // Worker threads used for a full build/rebuild. 0 means one per core.
    // This is a build-time setting and is not persisted with the index.
    size_t build_threads;
} barq_vector_index_config_t;


/* Notification types */
typedef struct barq_notification_token barq_notification_token_t;
typedef struct barq_callback_token barq_callback_token_t;
typedef struct barq_refresh_callback_token barq_refresh_callback_token_t;
typedef struct barq_object_changes barq_object_changes_t;
typedef struct barq_collection_changes barq_collection_changes_t;
typedef struct barq_dictionary_changes barq_dictionary_changes_t;
typedef void (*barq_on_object_change_func_t)(barq_userdata_t userdata, const barq_object_changes_t*);
typedef void (*barq_on_collection_change_func_t)(barq_userdata_t userdata, const barq_collection_changes_t*);
typedef void (*barq_on_dictionary_change_func_t)(barq_userdata_t userdata, const barq_dictionary_changes_t*);
typedef void (*barq_on_barq_change_func_t)(barq_userdata_t userdata);
typedef void (*barq_on_barq_refresh_func_t)(barq_userdata_t userdata);
typedef void (*barq_async_begin_write_func_t)(barq_userdata_t userdata);
typedef void (*barq_async_commit_func_t)(barq_userdata_t userdata, bool error, const char* desc);

/**
 * Callback for barq schema changed notifications.
 *
 * @param new_schema The new schema. This object is released after the callback returns.
 *                   Preserve it with barq_clone() if you wish to keep it around for longer.
 */
typedef void (*barq_on_schema_change_func_t)(barq_userdata_t userdata, const barq_schema_t* new_schema);

/* Scheduler types */
typedef void (*barq_scheduler_notify_func_t)(barq_userdata_t userdata, barq_work_queue_t* work_queue);
typedef bool (*barq_scheduler_is_on_thread_func_t)(barq_userdata_t userdata);
typedef bool (*barq_scheduler_is_same_as_func_t)(const barq_userdata_t scheduler_userdata_1,
                                                  const barq_userdata_t scheduler_userdata_2);
typedef bool (*barq_scheduler_can_deliver_notifications_func_t)(barq_userdata_t userdata);
typedef barq_scheduler_t* (*barq_scheduler_default_factory_func_t)(barq_userdata_t userdata);

/* Sync Socket Provider types */
typedef struct barq_websocket_endpoint {
    const char* address;    // Host address
    uint16_t port;          // Host port number
    const char* path;       // Includes access token in query.
    const char** protocols; // Array of one or more websocket protocols
    size_t num_protocols;   // Number of protocols in array
    bool is_ssl;            // true if SSL should be used
} barq_websocket_endpoint_t;

// The following definitions are intended for internal state and structures
// used by the Sync Client. These values should be retained by the Platform
// Networking CAPI implementation so they can be provided back to the Platform
// Networking CAPI functions.
typedef struct barq_sync_socket barq_sync_socket_t;
typedef struct barq_sync_socket_callback barq_sync_socket_post_callback_t;
typedef struct barq_sync_socket_callback barq_sync_socket_timer_callback_t;
typedef struct barq_sync_socket_callback barq_sync_socket_write_callback_t;
typedef void* barq_sync_socket_timer_t;
typedef void* barq_sync_socket_websocket_t;
typedef struct barq_websocket_observer barq_websocket_observer_t;

// Called when the Sync Client posts a callback handler to be run within the context
// of the event loop.
// The post_callback pointer does not need to be released by the CAPI implementation.
typedef void (*barq_sync_socket_post_func_t)(barq_userdata_t userdata,
                                              barq_sync_socket_post_callback_t* post_callback);

// Called when a Sync Socket Timer is being created, which will start the timer countdown
// immediately. The Timer CAPI implementation will need to be stored locally so it can
// be used when calling barq_sync_socket_timer_complete() when the timer countdown
// reaches 0 (i.e. expired) or barq_sync_socket_timer_canceled() when the timer is canceled.
// The timer_callback pointer does not need to be released by the CAPI implementation.
typedef barq_sync_socket_timer_t (*barq_sync_socket_create_timer_func_t)(
    barq_userdata_t userdata, uint64_t delay_ms, barq_sync_socket_timer_callback_t* timer_callback);

// Called when a Sync Socket Timer has been explicitly canceled or the Timer is being
// destroyed. Use the barq_sync_socket_timer_canceled() function to notify the Sync Client
// that the timer cancel is complete. NOTE: This function will always be called before the
// timer is destroyed (even if the timer has completed), but the timer callback should only
// be executed one time.
typedef void (*barq_sync_socket_timer_canceled_func_t)(barq_userdata_t userdata,
                                                        barq_sync_socket_timer_t timer_userdata);

// Called when the timer object has been destroyed so the Sync Socket Timer CAPI
// implementation can clean up its timer resources.
typedef void (*barq_sync_socket_timer_free_func_t)(barq_userdata_t userdata,
                                                    barq_sync_socket_timer_t timer_userdata);

// Called when the Sync Client is initiating a connection to the server. The endpoint
// structure contains the server address/URL and the websocket_observer will need to
// be stored locally in the WebSocket CAPI implementation so it can be used with the
// barq_sync_socket_websocket_[connected|message|error|closed]() functions when
// providing WebSocket status or data to the Sync Client.
typedef barq_sync_socket_websocket_t (*barq_sync_socket_connect_func_t)(
    barq_userdata_t userdata, barq_websocket_endpoint_t endpoint, barq_websocket_observer_t* websocket_observer);

// Called by a connection in the Sync Client when it needs to send data to the server. The
// write_callback is used with barq_sync_socket_write_complete() to inform the connection
// that the data has been transferred successfully.
// If an error occurs during the async write operation, it needs to be provided to the
// write_callback handler, and the websocket is exepected to be closed by calling
// barq_sync_socket_websocket_error() followed by providing the error code and reason to
// barq_sync_socket_websocket_closed().
// The write_callback pointer does not need to be released by the CAPI implementation.
typedef void (*barq_sync_socket_websocket_async_write_func_t)(barq_userdata_t userdata,
                                                               barq_sync_socket_websocket_t websocket,
                                                               const char* data, size_t size,
                                                               barq_sync_socket_write_callback_t* write_callback);

// Called when the websocket has been destroyed in the Sync Client - no more write callbacks or observer
// functions should be called when this function is called.
typedef void (*barq_sync_socket_websocket_free_func_t)(barq_userdata_t userdata,
                                                        barq_sync_socket_websocket_t websocket);

/**
 * Get the VersionID of the current transaction.
 *
 * @param out_found True if version information is available. This requires an available Read or Write transaction.
 * @param out_version The version of the current transaction. If `out_found` returns False, this returns (0,0).
 * @return True if no exception occurred.
 */
BARQ_API bool barq_get_version_id(const barq_t*, bool* out_found, barq_version_id_t* out_version);

/**
 * Get a string representing the version number of the Barq library.
 *
 * @return A null-terminated string.
 */
BARQ_API const char* barq_get_library_version(void);

/**
 * Get individual components of the version number of the Barq library.
 *
 * @param out_major The major version number (X.0.0).
 * @param out_minor The minor version number (0.X.0).
 * @param out_patch The patch version number (0.0.X).
 * @param out_extra The extra version string (0.0.0-X).
 */
BARQ_API void barq_get_library_version_numbers(int* out_major, int* out_minor, int* out_patch,
                                               const char** out_extra);

/**
 * Get the last error that happened on this thread.
 *
 * Errors are thread-local. Getting the error must happen on the same thread as
 * the call that caused the error to occur. The error is specific to the current
 * thread, and not the Barq instance for which the error occurred.
 *
 * Note: The error message in @a err will only be safe to use until the next API
 *       call is made on the current thread.
 *
 * Note: The error is not cleared by subsequent successful calls to this
 *       function, but it will be overwritten by subsequent failing calls to
 *       other library functions.
 *
 * Note: Calling this function does not clear the current last error.
 *
 * This function does not allocate any memory.
 *
 * @param err A pointer to a `barq_error_t` struct that will be populated with
 *            information about the last error, if there is one. May be NULL.
 * @return True if an error occurred.
 */
BARQ_API bool barq_get_last_error(barq_error_t* err);

/**
 * Get information about an async error, potentially coming from another thread.
 *
 * This function does not allocate any memory.
 *
 * @param err A pointer to a `barq_error_t` struct that will be populated with
 *            information about the error. May not be NULL.
 * @return A bool indicating whether or not an error is available to be returned
 * @see barq_get_last_error()
 */
BARQ_API bool barq_get_async_error(const barq_async_error_t* err, barq_error_t* out_err);

/**
 * Convert the last error to `barq_async_error_t`, which can safely be passed
 * between threads.
 *
 * Note: This function does not clear the last error.
 *
 * @return A non-null pointer if there was an error on this thread.
 * @see barq_get_last_error()
 * @see barq_get_async_error()
 * @see barq_clear_last_error()
 */
BARQ_API barq_async_error_t* barq_get_last_error_as_async_error(void);

#if defined(__cplusplus)
/**
 * Invoke a function that may throw an exception, and report that exception as
 * part of the C API error handling mechanism.
 *
 * This is used to test translation of exceptions to error codes.
 *
 * @return True if no exception was thrown.
 */
BARQ_EXPORT bool barq_wrap_exceptions(void (*)()) noexcept;
#endif // __cplusplus

/**
 * Clear the last error on the calling thread.
 *
 * Use this if the system has recovered from an error, e.g. by closing the
 * offending Barq and reopening it, freeing up resources, or similar.
 *
 * @return True if an error was cleared.
 */
BARQ_API bool barq_clear_last_error(void);

/**
 * Free memory allocated by the module this library was linked into.
 *
 * This is needed for raw memory buffers such as string copies or arrays
 * returned from a library function. Barq C Wrapper objects on the other hand
 * should always be freed with barq_release() only.
 */
BARQ_API void barq_free(void* buffer);

/**
 * Free any Barq C Wrapper object.
 *
 * Note: Any pointer returned from a library function is owned by the caller.
 *       The caller is responsible for calling `barq_release()`. The only
 *       exception from this is C++ bridge functions that return `void*`, with
 *       the prefix `_barq`.
 *
 * Note: C++ destructors are typically `noexcept`, so it is likely that an
 *       exception will crash the process.
 *
 * @param ptr A pointer to a Barq C Wrapper object. May be NULL.
 */
BARQ_API void barq_release(void* ptr);

/**
 * Clone a Barq C Wrapper object.
 *
 * If the object is not clonable, this function fails with BARQ_ERR_NOT_CLONABLE.
 *
 * @return A pointer to an object of the same type as the input, or NULL if
 *         cloning failed.
 */
BARQ_API void* barq_clone(const void*);

/**
 * Return true if two API objects refer to the same underlying data. Objects
 * with different types are never equal.
 *
 * Note: This function cannot be used with types that have value semantics, only
 *       opaque types that have object semantics.
 *
 *    - `barq_t` objects are identical if they represent the same instance (not
 *      just if they represent the same file).
 *    - `barq_schema_t` objects are equal if the represented schemas are equal.
 *    - `barq_config_t` objects are equal if the configurations are equal.
 *    - `barq_object_t` objects are identical if they belong to the same barq
 *      and class, and have the same object key.
 *    - `barq_list_t` and other collection objects are identical if they come
 *      from the same object and property.
 *    - `barq_query_t` objects are never equal.
 *    - `barq_scheduler_t` objects are equal if they represent the same
 *      scheduler.
 *    - Query descriptor objects are equal if they represent equivalent
 *      descriptors.
 *    - `barq_async_error_t` objects are equal if they represent the same
 *      exception instance.
 *
 * This function cannot fail.
 */
BARQ_API bool barq_equals(const void*, const void*);

/**
 * True if a Barq C Wrapper object is "frozen" (immutable).
 *
 * Objects, collections, and results can be frozen. For all other types, this
 * function always returns false.
 */
BARQ_API bool barq_is_frozen(const void*);

/* Logging */
// equivalent to barq::util::Logger::Level in util/logger.hpp and must be kept in sync.
typedef enum barq_log_level {
    BARQ_LOG_LEVEL_ALL = 0,
    BARQ_LOG_LEVEL_TRACE = 1,
    BARQ_LOG_LEVEL_DEBUG = 2,
    BARQ_LOG_LEVEL_DETAIL = 3,
    BARQ_LOG_LEVEL_INFO = 4,
    BARQ_LOG_LEVEL_WARNING = 5,
    BARQ_LOG_LEVEL_ERROR = 6,
    BARQ_LOG_LEVEL_FATAL = 7,
    BARQ_LOG_LEVEL_OFF = 8,
} barq_log_level_e;

typedef void (*barq_log_func_t)(barq_userdata_t userdata, const char* category, barq_log_level_e level,
                                 const char* message);

/**
 * Install the default logger
 */
BARQ_API void barq_set_log_callback(barq_log_func_t, barq_userdata_t userdata,
                                    barq_free_userdata_func_t userdata_free) BARQ_API_NOEXCEPT;
BARQ_API void barq_set_log_level(barq_log_level_e) BARQ_API_NOEXCEPT;
/**
 * Set the logging level for given category. Return the previous level.
 */
BARQ_API barq_log_level_e barq_set_log_level_category(const char*, barq_log_level_e) BARQ_API_NOEXCEPT;
/**
 * Get the logging level for given category.
 */
BARQ_API barq_log_level_e barq_get_log_level_category(const char*) BARQ_API_NOEXCEPT;
/**
 * Get the actual log category names (currently 15)
  @param num_values number of values in the out_values array
  @param out_values pointer to an array of size num_values
  @return returns the number of categories returned. If num_values is zero, it will
          return the total number of categories.
 */
BARQ_API size_t barq_get_category_names(size_t num_values, const char** out_values);

/**
 * Get a thread-safe reference representing the same underlying object as some
 * API object.
 *
 * The thread safe reference can be passed to a different thread and resolved
 * against a different `barq_t` instance, which succeeds if the underlying
 * object still exists.
 *
 * The following types can produce thread safe references:
 *
 * - `barq_object_t`
 * - `barq_results_t`
 * - `barq_list_t`
 * - `barq_t`
 *
 * This does not assume ownership of the object, except for `barq_t`, where the
 * instance is transferred by value, and must be transferred back to the current
 * thread to be used. Note that the `barq_thread_safe_reference_t` object must
 * still be destroyed after having been converted into a `barq_t` object.
 *
 * @return A non-null pointer if no exception occurred.
 */
BARQ_API barq_thread_safe_reference_t* barq_create_thread_safe_reference(const void*);

/**
 * Allocate a new configuration with default options.
 */
BARQ_API barq_config_t* barq_config_new(void);

/**
 * Get the path of the barq being opened.
 *
 * This function cannot fail.
 */
BARQ_API const char* barq_config_get_path(const barq_config_t*);

/**
 * Set the path of the barq being opened.
 *
 * This function aborts when out of memory, but otherwise cannot fail.
 */
BARQ_API void barq_config_set_path(barq_config_t*, const char* path);

/**
 * Get the encryption key for the barq.
 *
 * The output buffer must be at least 64 bytes.
 *
 * @returns The length of the encryption key (0 or 64)
 */
BARQ_API size_t barq_config_get_encryption_key(const barq_config_t*, uint8_t* out_key);

/**
 * Set the encryption key for the barq.
 *
 * The key must be either 64 bytes long or have length zero (in which case
 * encryption is disabled).
 *
 * This function may fail if the encryption key has the wrong length.
 */
BARQ_API bool barq_config_set_encryption_key(barq_config_t*, const uint8_t* key, size_t key_size);

/**
 * Get the schema for this barq.
 *
 * Note: The caller obtains ownership of the returned value, and must manually
 *       free it by calling `barq_release()`.
 *
 * @return A schema object, or NULL if the schema is not set (empty).
 */
BARQ_API barq_schema_t* barq_config_get_schema(const barq_config_t*);

/**
 * Set the schema object for this barq.
 *
 * This does not take ownership of the schema object, and it should be released
 * afterwards.
 *
 * This function aborts when out of memory, but otherwise cannot fail.
 *
 * @param schema The schema object. May be NULL, which means an empty schema.
 */
BARQ_API void barq_config_set_schema(barq_config_t*, const barq_schema_t* schema);

/**
 * Get the schema version of the schema.
 *
 * This function cannot fail.
 */
BARQ_API uint64_t barq_config_get_schema_version(const barq_config_t*);

/**
 * Set the schema version of the schema.
 *
 * This function cannot fail.
 */
BARQ_API void barq_config_set_schema_version(barq_config_t*, uint64_t version);

/**
 * Get the schema mode.
 *
 * This function cannot fail.
 */
BARQ_API barq_schema_mode_e barq_config_get_schema_mode(const barq_config_t*);

/**
 * Set the schema mode.
 *
 * This function cannot fail.
 */
BARQ_API void barq_config_set_schema_mode(barq_config_t*, barq_schema_mode_e);

/**
 * Get the subset schema mode.
 *
 * This function cannot fail.
 */
BARQ_API barq_schema_subset_mode_e barq_config_get_schema_subset_mode(const barq_config_t*);

/**
 * Set schema subset mode
 *
 * This function cannot fail
 */
BARQ_API void barq_config_set_schema_subset_mode(barq_config_t*, barq_schema_subset_mode_e);

/**
 * Set the migration callback.
 *
 * The migration function is called during a migration for schema modes
 * `BARQ_SCHEMA_MODE_AUTOMATIC` and `BARQ_SCHEMA_MODE_MANUAL`. The callback is
 * invoked with a barq instance before the migration and the barq instance
 * that is currently performing the migration.
 *
 * This function cannot fail.
 */
BARQ_API void barq_config_set_migration_function(barq_config_t*, barq_migration_func_t, barq_userdata_t userdata,
                                                 barq_free_userdata_func_t userdata_free);

/**
 * Set the data initialization function.
 *
 * The callback is invoked the first time the schema is created, such that the
 * user can perform one-time initialization of the data in the barq.
 *
 * The barq instance passed to the callback is in a write transaction.
 *
 * This function cannot fail.
 */
BARQ_API void barq_config_set_data_initialization_function(barq_config_t*, barq_data_initialization_func_t,
                                                           barq_userdata_t userdata,
                                                           barq_free_userdata_func_t userdata_free);

/**
 * Set the should-compact-on-launch callback.
 *
 * The callback is invoked the first time a barq file is opened in this process
 * to decide whether the barq file should be compacted.
 *
 * Note: If another process has the barq file open, it will not be compacted.
 *
 * This function cannot fail.
 */
BARQ_API void barq_config_set_should_compact_on_launch_function(barq_config_t*,
                                                                barq_should_compact_on_launch_func_t,
                                                                barq_userdata_t userdata,
                                                                barq_free_userdata_func_t userdata_free);

/**
 * True if file format upgrades on open are disabled.
 *
 * This function cannot fail.
 */
BARQ_API bool barq_config_get_disable_format_upgrade(const barq_config_t*);

/**
 * True if you can open the file without a file_format_upgrade
 */
BARQ_API bool barq_config_needs_file_format_upgrade(const barq_config_t*);

/**
 * Disable file format upgrade on open (default: false).
 *
 * If a migration is needed to open the barq file with the provided schema, an
 * error is thrown rather than automatically performing the migration.
 *
 * This function cannot fail.
 */
BARQ_API void barq_config_set_disable_format_upgrade(barq_config_t*, bool);

/**
 * True if automatic change notifications should be generated.
 *
 * This function cannot fail.
 */
BARQ_API bool barq_config_get_automatic_change_notifications(const barq_config_t*);

/**
 * Automatically generated change notifications (default: true).
 *
 * This function cannot fail.
 */
BARQ_API void barq_config_set_automatic_change_notifications(barq_config_t*, bool);

/**
 * The scheduler which this barq should be bound to (default: NULL).
 *
 * If NULL, the barq will be bound to the default scheduler for the current thread.
 *
 * This function aborts when out of memory, but otherwise cannot fail.
 */
BARQ_API void barq_config_set_scheduler(barq_config_t*, const barq_scheduler_t*);

/**
 * Sync configuration for this barq (default: NULL).
 *
 * This function aborts when out of memory, but otherwise cannot fail.
 */
BARQ_API void barq_config_set_sync_config(barq_config_t*, barq_sync_config_t*);

/**
 * Get whether the barq file should be forcibly initialized as a synchronized.
 *
 * This function cannot fail.
 */
BARQ_API bool barq_config_get_force_sync_history(const barq_config_t*);

/**
 * Force the barq file to be initialized as a synchronized barq, even if no
 * sync config is provided (default: false).
 *
 * This function cannot fail.
 */
BARQ_API void barq_config_set_force_sync_history(barq_config_t*, bool);

/**
 * Set the audit interface for the barq (unimplemented).
 */
BARQ_API bool barq_config_set_audit_factory(barq_config_t*, void*);

/**
 * Get maximum number of active versions in the barq file allowed before an
 * exception is thrown.
 *
 * This function cannot fail.
 */
BARQ_API uint64_t barq_config_get_max_number_of_active_versions(const barq_config_t*);

/**
 * Set maximum number of active versions in the barq file allowed before an
 * exception is thrown (default: UINT64_MAX).
 *
 * This function cannot fail.
 */
BARQ_API void barq_config_set_max_number_of_active_versions(barq_config_t*, uint64_t);

/**
 * Configure barq to be in memory
 */
BARQ_API void barq_config_set_in_memory(barq_config_t*, bool) BARQ_API_NOEXCEPT;

/**
 * Check if barq is configured in memory
 */
BARQ_API bool barq_config_get_in_memory(barq_config_t*) BARQ_API_NOEXCEPT;

/**
 * Set FIFO path
 */
BARQ_API void barq_config_set_fifo_path(barq_config_t*, const char*);

/**
 Check barq FIFO path
 */
BARQ_API const char* barq_config_get_fifo_path(barq_config_t*) BARQ_API_NOEXCEPT;

/**
 * If 'cached' is false, always return a new Barq instance.
 */
BARQ_API void barq_config_set_cached(barq_config_t*, bool cached) BARQ_API_NOEXCEPT;

/**
 * Check if barqs are cached
 */
BARQ_API bool barq_config_get_cached(barq_config_t*) BARQ_API_NOEXCEPT;

/**
 * Allow barq to manage automatically embedded objects when a migration from TopLevel to Embedded takes place.
 */
BARQ_API void barq_config_set_automatic_backlink_handling(barq_config_t*, bool) BARQ_API_NOEXCEPT;

/**
 * Create a custom scheduler object from callback functions.
 *
 * @param notify Function which will be called whenever the scheduler has work
 *               to do. Each call to this should trigger a call to
 *               `barq_scheduler_perform_work()` from within the scheduler's
 *               event loop. This function must be thread-safe, or NULL, in
 *               which case the scheduler is considered unable to deliver
 *               notifications.
 * @param is_on_thread Function to return true if called from the same thread as
 *                     the scheduler. This function must be thread-safe.
 * @param can_deliver_notifications Function to return true if the scheduler can
 *                                  support `notify()`. This function does not
 *                                  need to be thread-safe.
 */
BARQ_API barq_scheduler_t*
barq_scheduler_new(barq_userdata_t userdata, barq_free_userdata_func_t userdata_free,
                    barq_scheduler_notify_func_t notify, barq_scheduler_is_on_thread_func_t is_on_thread,
                    barq_scheduler_is_same_as_func_t is_same_as,
                    barq_scheduler_can_deliver_notifications_func_t can_deliver_notifications);

/**
 * Performs all of the pending work for the given scheduler.
 *
 * This function must be called from within the scheduler's event loop. It must
 * be called each time the notify callback passed to the scheduler
 * is invoked.
 */
BARQ_API void barq_scheduler_perform_work(barq_work_queue_t*);
/**
 * Create an instance of the default scheduler for the current platform,
 * normally confined to the calling thread.
 */
BARQ_API barq_scheduler_t* barq_scheduler_make_default(void);

/**
 * Get the scheduler used by frozen barqs. This scheduler does not support
 * notifications, and does not perform any thread checking.
 *
 * This function is thread-safe, and cannot fail.
 */
BARQ_API const barq_scheduler_t* barq_scheduler_get_frozen(void);

/**
 * Open a Barq file.
 *
 * @param config Barq configuration. If the Barq is already opened on another
 *               thread, validate that the given configuration is compatible
 *               with the existing one.
 * @return If successful, the Barq object. Otherwise, NULL.
 */
BARQ_API barq_t* barq_open(const barq_config_t* config);

/**
 * The overloaded Barq::convert function offers a way to copy and/or convert a barq.
 *
 * The following options are supported:
 * - local -> local (config or path)
 * - local -> sync (config only)
 * - sync -> local (config only)
 * - sync -> sync  (config or path)
 * - sync -> bundlable sync (client file identifier removed)
 *
 * Note that for bundled barqs it is required that all local changes are synchronized with the
 * server before the copy can be written. This is to be sure that the file can be used as a
 * stating point for a newly installed application. The function will throw if there are
 * pending uploads.
 */
/**
 * Copy or convert a Barq using a config.
 *
 * If the file already exists and merge_with_existing is true, data will be copied over object per object.
 * When merging, all classes must have a pk called '_id" otherwise an exception is thrown.
 * If the file exists and merge_with_existing is false, an exception is thrown.
 * If the file does not exist, the barq file will be exported to the new location and if the
 * configuration object contains a sync part, a sync history will be synthesized.
 *
 * @param config The barq configuration that should be used to create a copy.
 *               This can be a local or a synced Barq, encrypted or not.
 * @param merge_with_existing If this is true and the destination file exists, data will be copied over object by
 * object. Otherwise, if this is false and the destination file exists, an exception is thrown.
 */
BARQ_API bool barq_convert_with_config(const barq_t* barq, const barq_config_t* config, bool merge_with_existing);
/**
 * Copy a Barq using a path.
 *
 * @param path The path the barq should be copied to. Local barqs will remain local, synced
 *             barqs will remain synced barqs.
 * @param encryption_key The optional encryption key for the new barq.
 * @param merge_with_existing If this is true and the destination file exists, data will be copied over object by
 object.
 *  Otherwise, if this is false and the destination file exists, an exception is thrown.

 */
BARQ_API bool barq_convert_with_path(const barq_t* barq, const char* path, barq_binary_t encryption_key,
                                     bool merge_with_existing);

/**
 * Deletes the following files for the given `barq_file_path` if they exist:
 * - the Barq file itself
 * - the .control folder
 * - the .note file
 * - the .log file
 *
 * The .lock file for this Barq cannot and will not be deleted as this is unsafe.
 * If a different process / thread is accessing the Barq at the same time a corrupt state
 * could be the result and checking for a single process state is not possible here.
 *
 * @param barq_file_path The path to the Barq file. All files will be derived from this.
 * @param[out] did_delete_barq If non-null, set to true if the primary Barq file was deleted.
 *                              Discard value if the function returns an error.
 *
 * @return true if no error occurred.
 *
 * @throws BARQ_ERR_FILE_PERMISSION_DENIED if the operation was not permitted.
 * @throws BARQ_ERR_FILE_ACCESS_ERROR for any other error while trying to delete the file or folder.
 * @throws BARQ_ERR_DELETE_OPENED_BARQ if the function was called on an open Barq.
 */
BARQ_API bool barq_delete_files(const char* barq_file_path, bool* did_delete_barq);

/**
 * Create a `barq_t` object from a thread-safe reference to the same barq.
 *
 * @param tsr Thread-safe reference object created by calling
 *            `barq_get_thread_safe_reference()` with a `barq_t` instance.
 * @param scheduler The scheduler to use for the new `barq_t` instance. May be
 *                  NULL, in which case the default scheduler for the current
 *                  thread is used.
 * @return A non-null pointer if no error occurred.
 */
BARQ_API barq_t* barq_from_thread_safe_reference(barq_thread_safe_reference_t* tsr, barq_scheduler_t* scheduler);

/**
 * Create a `barq_t*` from a `std::shared_ptr<Barq>*`.
 *
 * This is intended as a migration path for users of the C++ Object Store API.
 *
 * Call `barq_release()` on the returned `barq_t*` to decrement the refcount
 * on the inner `std::shared_ptr<Barq>`.
 *
 * @param pshared_ptr A pointer to an instance of `std::shared_ptr<Barq>`.
 * @param n Must be equal to `sizeof(std::shared_ptr<Barq>)`.
 * @return A `barq_t*` representing the same Barq object as the passed
 *         `std::shared_ptr<Barq>`.
 */
BARQ_API barq_t* _barq_from_native_ptr(const void* pshared_ptr, size_t n);

/**
 * Get a `std::shared_ptr<Barq>` from a `barq_t*`.
 *
 * This is intended as a migration path for users of the C++ Object Store API.
 *
 * @param pshared_ptr A pointer to an instance of `std::shared_ptr<Barq>`.
 * @param n Must be equal to `sizeof(std::shared_ptr<Barq>)`.
 */
BARQ_API void _barq_get_native_ptr(const barq_t*, void* pshared_ptr, size_t n);

/**
 * Forcibly close a Barq file.
 *
 * Note that this invalidates all Barq instances for the same path.
 *
 * The Barq will be automatically closed when the last reference is released,
 * including references to objects within the Barq.
 *
 * @return True if no exception occurred.
 */
BARQ_API bool barq_close(barq_t*);

/**
 * True if the Barq file is closed.
 *
 * This function cannot fail.
 */
BARQ_API bool barq_is_closed(barq_t*);

/**
 * Begin a read transaction for the Barq file.
 *
 * @return True if no exception occurred.
 */
BARQ_API bool barq_begin_read(barq_t*);

/**
 * Begin a write transaction for the Barq file.
 *
 * @return True if no exception occurred.
 */
BARQ_API bool barq_begin_write(barq_t*);

/**
 * Return true if the barq is in a write transaction.
 *
 * This function cannot fail.
 */
BARQ_API bool barq_is_writable(const barq_t*);

/**
 * Commit a write transaction.
 *
 * @return True if the commit succeeded and no exceptions were thrown.
 */
BARQ_API bool barq_commit(barq_t*);

/**
 * Roll back a write transaction.
 *
 * @return True if the rollback succeeded and no exceptions were thrown.
 */
BARQ_API bool barq_rollback(barq_t*);

/**
 * start a new write transaction asynchronously for the barq passed as argument.
 */
BARQ_API bool barq_async_begin_write(barq_t* barq, barq_async_begin_write_func_t, barq_userdata_t userdata,
                                     barq_free_userdata_func_t userdata_free, bool notify_only,
                                     unsigned int* transaction_id);

/**
 * commit a transaction asynchronously for the barq passed as argument.
 */
BARQ_API bool barq_async_commit(barq_t* barq, barq_async_commit_func_t, barq_userdata_t userdata,
                                barq_free_userdata_func_t userdata_free, bool allow_grouping,
                                unsigned int* transaction_id);

/**
 * Cancel the transaction referenced by the token passed as argument and set the optional boolean flag in order to
 * inform the caller if the transaction was cancelled.
 */
BARQ_API bool barq_async_cancel(barq_t* barq, unsigned int token, bool* cancelled);

/**
 * Add a callback that will be invoked every time the view of this file is updated.
 *
 * This callback is guaranteed to be invoked before any object or collection change
 * notifications for this barq are delivered.
 *
 * @return a registration token used to remove the callback.
 */
BARQ_API barq_callback_token_t* barq_add_barq_changed_callback(barq_t*, barq_on_barq_change_func_t,
                                                                 barq_userdata_t userdata,
                                                                 barq_free_userdata_func_t userdata_free);

/**
 * Add a callback that will be invoked the first time that the given barq is refreshed to the version which is the
 * latest version at the time when this is called.
 * @return a refresh token to remove the callback
 */
BARQ_API barq_refresh_callback_token_t* barq_add_barq_refresh_callback(barq_t*, barq_on_barq_refresh_func_t,
                                                                         barq_userdata_t userdata,
                                                                         barq_free_userdata_func_t userdata_free);

/**
 * Refresh the view of the barq file.
 *
 * If another process or thread has made changes to the barq file, this causes
 * those changes to become visible in this barq instance.
 *
 * This calls `advance_read()` at the Core layer.
 *
 * @return True if no exceptions are thrown, false otherwise.
 */
BARQ_API bool barq_refresh(barq_t*, bool* did_refresh);

/**
 * Produce a frozen view of this barq.
 *
 * @return A non-NULL barq instance representing the frozen state.
 */
BARQ_API barq_t* barq_freeze(const barq_t*);

/**
 * Vacuum the free space from the barq file, reducing its file size.
 *
 * @return True if no exceptions are thrown, false otherwise.
 */
BARQ_API bool barq_compact(barq_t*, bool* did_compact);

/**
 * Find and delete the table passed as parementer for the barq instance passed to this function.
 * @param table_name for the table the user wants to delete
 * @param table_deleted in order to indicate if the table was actually deleted from barq
 * @return true if no error has occurred, false otherwise
 */
BARQ_API bool barq_remove_table(barq_t*, const char* table_name, bool* table_deleted);

/**
 * Create a new schema from classes and their properties.
 *
 * Note: This function does not validate the schema.
 *
 * Note: `barq_class_key_t` and `barq_property_key_t` values inside
 *       `barq_class_info_t` and `barq_property_info_t` are unused when
 *       defining the schema. Call `barq_get_schema()` to obtain the values for
 *       these fields in an open barq.
 *
 * @return True if allocation of the schema structure succeeded.
 */
BARQ_API barq_schema_t* barq_schema_new(const barq_class_info_t* classes, size_t num_classes,
                                         const barq_property_info_t** class_properties);

/**
 * Get the schema for this barq.
 *
 * Note: The returned value is allocated by this function, so `barq_release()`
 *       must be called on it.
 */
BARQ_API barq_schema_t* barq_get_schema(const barq_t*);

/**
 * Get the schema version for this barq.
 *
 * This function cannot fail.
 */
BARQ_API uint64_t barq_get_schema_version(const barq_t* barq);

/**
 * Get the schema version for this barq at the path.
 */
BARQ_API uint64_t barq_get_persisted_schema_version(const barq_config_t* config);

/**
 * Update the schema of an open barq.
 *
 * This is equivalent to calling `barq_update_schema_advanced(barq, schema, 0,
 * NULL, NULL, NULL, NULL, false)`.
 */
BARQ_API bool barq_update_schema(barq_t* barq, const barq_schema_t* schema);

/**
 * Update the schema of an open barq, with options to customize certain steps
 * of the process.
 *
 * @param barq The barq for which the schema should be updated.
 * @param schema The new schema for the barq. If the schema is the same the
 *               existing schema, this function does nothing.
 * @param version The version of the new schema.
 * @param migration_func Callback to perform the migration. Has no effect if the
 *                       Barq is opened with `BARQ_SCHEMA_MODE_ADDITIVE`.
 * @param migration_func_userdata Userdata pointer to pass to `migration_func`.
 * @param data_init_func Callback to perform initialization of the data in the
 *                       Barq if it is opened for the first time (i.e., it has
 *                       no previous schema version).
 * @param data_init_func_userdata Userdata pointer to pass to `data_init_func`.
 * @param is_in_transaction Pass true if the barq is already in a write
 *                          transaction. Otherwise, if the migration requires a
 *                          write transaction, this function will perform the
 *                          migration in its own write transaction.
 */
BARQ_API bool barq_update_schema_advanced(barq_t* barq, const barq_schema_t* schema, uint64_t version,
                                          barq_migration_func_t migration_func,
                                          barq_userdata_t migration_func_userdata,
                                          barq_data_initialization_func_t data_init_func,
                                          barq_userdata_t data_init_func_userdata, bool is_in_transaction);

/**
 *  Rename a property for the schame  of the open barq.
 *  @param barq The barq for which the property schema has to be renamed
 *  @param schema The schema to modifies
 *  @param object_type type of the object to modify
 *  @param old_name old name of the property
 *  @param new_name new name of the property
 */
BARQ_API bool barq_schema_rename_property(barq_t* barq, barq_schema_t* schema, const char* object_type,
                                          const char* old_name, const char* new_name);

/**
 * Get the `barq::Schema*` pointer for this barq.
 *
 * This is intended as a migration path for users of the C++ Object Store API.
 *
 * The returned value is owned by the `barq_t` instance, and must not be freed.
 */
BARQ_API const void* _barq_get_schema_native(const barq_t*);

/**
 * Add a callback that will be invoked every time the schema of this barq is changed.
 *
 * @return a registration token used to remove the callback.
 */
BARQ_API barq_callback_token_t* barq_add_schema_changed_callback(barq_t*, barq_on_schema_change_func_t,
                                                                  barq_userdata_t userdata,
                                                                  barq_free_userdata_func_t userdata_free);


/**
 * Validate the schema.
 *
 *  @param validation_mode A bitwise combination of values from the
 *                         enum barq_schema_validation_mode.
 *
 * @return True if the schema passed validation. If validation failed,
 *         `barq_get_last_error()` will produce an error describing the
 *         validation failure.
 */
BARQ_API bool barq_schema_validate(const barq_schema_t*, uint64_t validation_mode);

/**
 * Return the number of classes in the Barq's schema.
 *
 * This cannot fail.
 */
BARQ_API size_t barq_get_num_classes(const barq_t*);

/**
 * Get the table keys for classes in the schema.
 * In case of errors this function will return false (errors to be fetched via `barq_get_last_error()`).
 * If data is not copied the function will return true and set  `out_n` with the capacity needed.
 * Data is only copied if the input array has enough capacity, otherwise the needed  array capacity will be set.
 *
 * @param out_keys An array that will contain the keys of each class in the
 *                 schema. Array may be NULL, in this case no data will be copied and `out_n` set if not NULL.
 * @param max The maximum number of keys to write to `out_keys`.
 * @param out_n The actual number of classes. May be NULL.
 * @return True if no exception occurred.
 */
BARQ_API bool barq_get_class_keys(const barq_t*, barq_class_key_t* out_keys, size_t max, size_t* out_n);

/**
 * Find a by the name of @a name.
 *
 * @param name The name of the class.
 * @param out_found Set to true if the class was found and no error occurred.
 *                  Otherwise, false. May not be NULL.
 * @param out_class_info A pointer to a `barq_class_info_t` that will be
 *                       populated with information about the class. May be
 *                       NULL.
 * @return True if no exception occurred.
 */
BARQ_API bool barq_find_class(const barq_t*, const char* name, bool* out_found, barq_class_info_t* out_class_info);

/**
 * Get the class with @a key from the schema.
 *
 * Passing an invalid @a key for this schema is considered an error.
 *
 * @param key The key of the class, as discovered by `barq_get_class_keys()`.
 * @param out_class_info A pointer to a `barq_class_info_t` that will be
 *                       populated with the information of the class. May be
 *                       NULL, though that's kind of pointless.
 * @return True if no exception occurred.
 */
BARQ_API bool barq_get_class(const barq_t*, barq_class_key_t key, barq_class_info_t* out_class_info);

/**
 * Get the list of properties for the class with this @a key.
 * In case of errors this function will return false (errors to be fetched via `barq_get_last_error()`).
 * If data is not copied the function will return true and set  `out_n` with the capacity needed.
 * Data is only copied if the input array has enough capacity, otherwise the needed  array capacity will be set.
 *
 * @param out_properties  A pointer to an array of `barq_property_info_t`, which
 *                       will be populated with the information about the
 *                       properties.  Array may be NULL, in this case no data will be copied and `out_n` set if not
 * NULL.
 * @param max The maximum number of entries to write to `out_properties`.
 * @param out_n The actual number of properties written to `out_properties`.
 * @return True if no exception occurred.
 */
BARQ_API bool barq_get_class_properties(const barq_t*, barq_class_key_t key, barq_property_info_t* out_properties,
                                        size_t max, size_t* out_n);

/**
 * Get the property keys for the class with this @a key.
 * In case of errors this function will return false (errors to be fetched via `barq_get_last_error()`).
 * If data is not copied the function will return true and set  `out_n` with the capacity needed.
 * Data is only copied if the input array has enough capacity, otherwise the needed  array capacity will be set.
 *
 * @param key The class key.
 * @param out_col_keys An array of property keys. Array may be NULL,
 *                     in this case no data will be copied and `out_n` set if not NULL.
 * @param max The maximum number of keys to write to `out_col_keys`. Ignored if
 *            `out_col_keys == NULL`.
 * @param out_n The actual number of properties written to `out_col_keys` (if
 *              non-NULL), or number of properties in the class.
 * @return True if no exception occurred.
 **/
BARQ_API bool barq_get_property_keys(const barq_t*, barq_class_key_t key, barq_property_key_t* out_col_keys,
                                     size_t max, size_t* out_n);

/**
 * Get the value for the property at the specified index in the object's schema.
 * @param prop_index The index of the property in the class properties array the barq was opened with.
 * @return True if no exception occurred.
 */
BARQ_API bool barq_get_value_by_property_index(const barq_object_t* object, size_t prop_index,
                                               barq_value_t* out_value);

/**
 * Find a property by its column key.
 *
 * It is an error to pass a property @a key that is not present in this class.
 *
 * @param class_key The key of the class.
 * @param key The column key for the property.
 * @param out_property_info A pointer to a `barq_property_info_t` that will be
 *                          populated with information about the property.
 * @return True if no exception occurred.
 */
BARQ_API bool barq_get_property(const barq_t*, barq_class_key_t class_key, barq_property_key_t key,
                                barq_property_info_t* out_property_info);

/**
 * Find a property by the internal (non-public) name of @a name.
 *
 * @param class_key The table key for the class.
 * @param name The name of the property.
 * @param out_found Will be set to true if the property was found. May not be
 *                  NULL.
 * @param out_property_info A pointer to a `barq_property_info_t` that will be
 *                          populated with information about the property. May
 *                          be NULL.
 * @return True if no exception occurred.
 */
BARQ_API bool barq_find_property(const barq_t*, barq_class_key_t class_key, const char* name, bool* out_found,
                                 barq_property_info_t* out_property_info);

/**
 * Find a property with the public name of @a name.
 *
 * @param class_key The table key for the class.
 * @param public_name The public name of the property.
 * @param out_found Will be set to true if the property was found. May not be
 *                  NULL.
 * @param out_property_info A pointer to a `barq_property_info_t` that will be
 *                          populated with information about the property. May
 *                          be NULL.
 * @return True if no exception occurred.
 */
BARQ_API bool barq_find_property_by_public_name(const barq_t*, barq_class_key_t class_key, const char* public_name,
                                                bool* out_found, barq_property_info_t* out_property_info);

/**
 * Find the primary key property for a class, if it has one.
 *
 * @param class_key The table key for this class.
 * @param out_found Will be set to true if the property was found. May not be
 *                  NULL.
 * @param out_property_info A property to a `barq_property_info_t` that will be
 *                          populated with information about the property, if it
 *                          was found. May be NULL.
 * @return True if no exception occurred.
 */
BARQ_API bool barq_find_primary_key_property(const barq_t*, barq_class_key_t class_key, bool* out_found,
                                             barq_property_info_t* out_property_info);

/**
 * Get the number of objects in a table (class).
 *
 * @param out_count A pointer to a `size_t` that will contain the number of
 *                  objects, if successful.
 * @return True if the table key was valid for this barq.
 */
BARQ_API bool barq_get_num_objects(const barq_t*, barq_class_key_t, size_t* out_count);

/**
 * Get the number of versions found in the Barq file.
 *
 * @param out_versions_count A pointer to a `size_t` that will contain the number of
 *                           versions, if successful.
 * @return True if no exception occurred.
 */
BARQ_API bool barq_get_num_versions(const barq_t*, uint64_t* out_versions_count);

/**
 * Get an object with a particular object key.
 *
 * @param class_key The class key.
 * @param obj_key The key to the object. Passing a non-existent key is
 *                considered an error.
 * @return A non-NULL pointer if no exception occurred.
 */
BARQ_API barq_object_t* barq_get_object(const barq_t*, barq_class_key_t class_key, barq_object_key_t obj_key);

/**
 * Get the parent object for the object passed as argument. Only works for embedded objects.
 * @return true, if no errors occurred.
 */
BARQ_API bool barq_object_get_parent(const barq_object_t* object, barq_object_t** parent,
                                     barq_class_key_t* class_key);

/**
 * Find an object with a particular primary key value.
 *
 * @param out_found A pointer to a boolean that will be set to true or false if
 *                  no error occurred.
 * @return A non-NULL pointer if the object was found and no exception occurred.
 */
BARQ_API barq_object_t* barq_object_find_with_primary_key(const barq_t*, barq_class_key_t, barq_value_t pk,
                                                           bool* out_found);

/**
 * Find all objects in class.
 *
 * Note: This is faster than running a query matching all objects (such as
 *       "TRUEPREDICATE").
 *
 * @return A non-NULL pointer if no exception was thrown.
 */
BARQ_API barq_results_t* barq_object_find_all(const barq_t*, barq_class_key_t);

/**
 * Add a vector (HNSW) index to a list-of-float column. Idempotent when an index
 * with an identical config already exists; throws on a conflicting config. Pass
 * a NULL @a config to use the engine defaults. Must run in a write transaction.
 *
 * @return True if no exception occurred.
 */
BARQ_API bool barq_add_vector_index(barq_t*, barq_class_key_t class_key, barq_property_key_t col_key,
                                    const barq_vector_index_config_t* config);

/**
 * Remove the vector index from a column, if any. Must run in a write transaction.
 *
 * @return True if no exception occurred.
 */
BARQ_API bool barq_remove_vector_index(barq_t*, barq_class_key_t class_key, barq_property_key_t col_key);

/**
 * Rebuild a column's vector index from the current table data. Must run in a
 * write transaction.
 *
 * @return True if no exception occurred.
 */
BARQ_API bool barq_rebuild_vector_index(barq_t*, barq_class_key_t class_key, barq_property_key_t col_key);

/**
 * Query whether a column has a vector index.
 *
 * @param out_has Set to true if the column has a vector index.
 * @return True if no exception occurred.
 */
BARQ_API bool barq_has_vector_index(const barq_t*, barq_class_key_t class_key, barq_property_key_t col_key,
                                    bool* out_has);

/**
 * Read the stored config of a column's vector index. Throws if the column has
 * no vector index.
 *
 * @param out_config Populated with the stored index config.
 * @return True if no exception occurred.
 */
BARQ_API bool barq_get_vector_index_config(const barq_t*, barq_class_key_t class_key, barq_property_key_t col_key,
                                           barq_vector_index_config_t* out_config);

/**
 * Run a k-nearest-neighbour search over a vector-indexed column and return the
 * results ordered closest-first.
 *
 * @param col_key A list-of-float column that has a vector index.
 * @param query_data The query vector.
 * @param query_size The number of floats in @a query_data.
 * @param k The number of neighbours to return.
 * @param ef Query-time beam width (0 = use the index config).
 * @param exact If true, run an exact flat scan for the true neighbours (overrides @a ef).
 * @return A non-NULL results pointer if no exception was thrown.
 */
BARQ_API barq_results_t* barq_results_knn_search(const barq_results_t*, barq_property_key_t col_key,
                                                 const float* query_data, size_t query_size, size_t k, size_t ef,
                                                 bool exact);

/**
 * Create an object in a class without a primary key.
 *
 * @return A non-NULL pointer if the object was created successfully.
 */
BARQ_API barq_object_t* barq_object_create(barq_t*, barq_class_key_t);

/**
 * Create an object in a class with a primary key. Will not succeed if an
 * object with the given primary key value already exists.
 *
 * @return A non-NULL pointer if the object was created successfully.
 */
BARQ_API barq_object_t* barq_object_create_with_primary_key(barq_t*, barq_class_key_t, barq_value_t pk);

/**
 * Create an object in a class with a primary key. If an object with the given
 * primary key value already exists, that object will be returned.
 *
 * @return A non-NULL pointer if the object was found/created successfully.
 */
BARQ_API barq_object_t* barq_object_get_or_create_with_primary_key(barq_t*, barq_class_key_t, barq_value_t pk,
                                                                    bool* did_create);

/**
 * Delete a barq object.
 *
 * Note: This does not call `barq_release()` on the `barq_object_t` instance.
 *
 * @return True if no exception occurred.
 */
BARQ_API bool barq_object_delete(barq_object_t*);

/**
 * Resolve the Barq object in the provided Barq.
 *
 * This is equivalent to producing a thread-safe reference and resolving it in the target barq.
 *
 * If the object can be resolved in the target barq, '*resolved' points to the new object
 * If the object cannot be resolved in the target barq, '*resolved' will be null.
 * @return True if no exception occurred (except exceptions that may normally occur if resolution fails)
 */
BARQ_API bool barq_object_resolve_in(const barq_object_t* live_object, const barq_t* target_barq,
                                     barq_object_t** resolved);

/**
 * Increment atomically property specified as parameter by value, for the object passed as argument.
 * @param object valid ptr to an object store in the database
 * @param property_key id of the property to change
 * @param value increment for the property passed as argument
 * @return True if not exception occurred.
 */
BARQ_API bool barq_object_add_int(barq_object_t* object, barq_property_key_t property_key, int64_t value);


BARQ_API barq_object_t* _barq_object_from_native_copy(const void* pobj, size_t n);
BARQ_API barq_object_t* _barq_object_from_native_move(void* pobj, size_t n);
BARQ_API const void* _barq_object_get_native_ptr(barq_object_t*);

/**
 * True if this object still exists in the barq.
 *
 * This function cannot fail.
 */
BARQ_API bool barq_object_is_valid(const barq_object_t*);

/**
 * Get the key for this object.
 *
 * This function cannot fail.
 */
BARQ_API barq_object_key_t barq_object_get_key(const barq_object_t* object);

/**
 * Get the table for this object.
 *
 * This function cannot fail.
 */
BARQ_API barq_class_key_t barq_object_get_table(const barq_object_t* object);

/**
 * Get a `barq_link_t` representing a link to @a object.
 *
 * This function cannot fail.
 */
BARQ_API barq_link_t barq_object_as_link(const barq_object_t* object);

/**
 * Helper method for making it easier to to convert SDK input to the underlying
 * `barq_key_path_array_t`.
 *
 * @return A pointer to the converted key path array. NULL in case of an error.
 */
BARQ_API barq_key_path_array_t* barq_create_key_path_array(const barq_t* barq,
                                                            const barq_class_key_t object_class_key,
                                                            size_t num_key_paths, const char** user_key_paths);

/**
 * Subscribe to notifications for this object.
 *
 * @return A non-null pointer if no exception occurred.
 */
BARQ_API barq_notification_token_t* barq_object_add_notification_callback(barq_object_t*, barq_userdata_t userdata,
                                                                           barq_free_userdata_func_t userdata_free,
                                                                           barq_key_path_array_t* key_path_array,
                                                                           barq_on_object_change_func_t on_change);

/**
 * Get an object from a thread-safe reference, potentially originating in a
 * different `barq_t` instance
 */
BARQ_API barq_object_t* barq_object_from_thread_safe_reference(const barq_t*, barq_thread_safe_reference_t*);

/**
 * Get the value for a property.
 *
 * @return True if no exception occurred.
 */
BARQ_API bool barq_get_value(const barq_object_t*, barq_property_key_t, barq_value_t* out_value);

/**
 * Get the values for several properties.
 *
 * This is provided as an alternative to calling `barq_get_value()` multiple
 * times in a row, which is particularly useful for language runtimes where
 * crossing the native bridge is comparatively expensive. In addition, it
 * eliminates some parameter validation that would otherwise be repeated for
 * each call.
 *
 * Example use cases:
 *
 *  - Extracting all properties of an object for serialization.
 *  - Converting an object to some in-memory representation.
 *
 * @param num_values The number of elements in @a properties and @a out_values.
 * @param properties The keys for the properties to fetch. May not be NULL.
 * @param out_values Where to write the property values. If an error occurs,
 *                   this array may only be partially initialized. May not be
 *                   NULL.
 * @return True if no exception occurs.
 */
BARQ_API bool barq_get_values(const barq_object_t*, size_t num_values, const barq_property_key_t* properties,
                              barq_value_t* out_values);

/**
 * Set the value for a property.
 *
 * @param new_value The new value for the property.
 * @param is_default True if this property is being set as part of setting the
 *                   default values for a new object. This has no effect in
 *                   non-sync'ed barqs.
 * @return True if no exception occurred.
 */
BARQ_API bool barq_set_value(barq_object_t*, barq_property_key_t, barq_value_t new_value, bool is_default);

/**
 * Assign a JSON formatted string to a Mixed property. Underlying structures will be created as needed
 *
 * @param json_string The new value for the property.
 * @return True if no exception occurred.
 */
BARQ_API bool barq_set_json(barq_object_t*, barq_property_key_t, const char* json_string);

/**
 * Create an embedded object in a given property.
 *
 * @return A non-NULL pointer if the object was created successfully.
 */
BARQ_API barq_object_t* barq_set_embedded(barq_object_t*, barq_property_key_t);

/**
 * Create a collection in a given Mixed property.
 *
 */
BARQ_API barq_list_t* barq_set_list(barq_object_t*, barq_property_key_t);
BARQ_API barq_dictionary_t* barq_set_dictionary(barq_object_t*, barq_property_key_t);

/** Return the object linked by the given property
 *
 * @return A non-NULL pointer if an object is found.
 */
BARQ_API barq_object_t* barq_get_linked_object(barq_object_t*, barq_property_key_t);

/**
 * Serializes an object to json and returns it as string. Serializes a single level of properties only.
 *
 * @return a json-serialized representation of the object.
 */
BARQ_API char* barq_object_to_string(barq_object_t*);

/**
 * Set the values for several properties.
 *
 * This is provided as an alternative to calling `barq_get_value()` multiple
 * times in a row, which is particularly useful for language runtimes where
 * crossing the native bridge is comparatively expensive. In addition, it
 * eliminates some parameter validation that would otherwise be repeated for
 * each call.
 *
 * Example use cases:
 *
 *  - Initializing a new object with default values.
 *  - Deserializing some in-memory structure into a barq object.
 *
 * This operation is "atomic"; if an exception occurs due to invalid input (such
 * as type mismatch, nullability mismatch, etc.), the object will remain
 * unmodified.
 *
 * @param num_values The number of elements in @a properties and @a values.
 * @param properties The keys of the properties to set. May not be NULL.
 * @param values The values to assign to the properties. May not be NULL.
 * @param is_default True if the properties are being set as part of setting
 *                   default values for a new object. This has no effect in
 *                   non-sync'ed barqs.
 * @return True if no exception occurred.
 */
BARQ_API bool barq_set_values(barq_object_t*, size_t num_values, const barq_property_key_t* properties,
                              const barq_value_t* values, bool is_default);

/**
 * Get a list instance for the property of an object.
 *
 * Note: It is up to the caller to call `barq_release()` on the returned list.
 *
 * @return A non-null pointer if no exception occurred.
 */
BARQ_API barq_list_t* barq_get_list(barq_object_t*, barq_property_key_t);

/**
 * Create a `barq_list_t` from a pointer to a `barq::List`, copy-constructing
 * the internal representation.
 *
 * @param plist A pointer to an instance of `barq::List`.
 * @param n Must be equal to `sizeof(barq::List)`.
 * @return A non-null pointer if no exception occurred.
 */
BARQ_API barq_list_t* _barq_list_from_native_copy(const void* plist, size_t n);

/**
 * Create a `barq_list_t` from a pointer to a `barq::List`, move-constructing
 * the internal representation.
 *
 * @param plist A pointer to an instance of `barq::List`.
 * @param n Must be equal to `sizeof(barq::List)`.
 * @return A non-null pointer if no exception occurred.
 */
BARQ_API barq_list_t* _barq_list_from_native_move(void* plist, size_t n);

/**
 * Resolve the list in the context of a given Barq instance.
 *
 * This is equivalent to producing a thread-safe reference and resolving it in the frozen barq.
 *
 * If resolution is possible, a valid resolved object is produced at '*resolved*'.
 * If resolution is not possible, but no error occurs, '*resolved' is set to NULL
 *
 * @return true if no error occurred.
 */
BARQ_API bool barq_list_resolve_in(const barq_list_t* list, const barq_t* target_barq, barq_list_t** resolved);

/**
 * Check if a list is valid.
 *
 * @return True if the list is valid.
 */
BARQ_API bool barq_list_is_valid(const barq_list_t*);

/**
 * Get the size of a list, in number of elements.
 *
 * This function may fail if the object owning the list has been deleted.
 *
 * @param out_size Where to put the list size. May be NULL.
 * @return True if no exception occurred.
 */
BARQ_API bool barq_list_size(const barq_list_t*, size_t* out_size);

/**
 * Get the property that this list came from.
 *
 * @return True if no exception occurred.
 */
BARQ_API bool barq_list_get_property(const barq_list_t*, barq_property_info_t* out_property_info);

/**
 * Get the value at @a index.
 *
 * @param out_value The resulting value, if no error occurred. May be NULL,
 *                  though nonsensical.
 * @return True if no exception occurred.
 */
BARQ_API bool barq_list_get(const barq_list_t*, size_t index, barq_value_t* out_value);

/**
 * Find the value in the list passed as parameter.
 * @param value to search in the list
 * @param out_index the index in the list where the value has been found or barq::not_found.
 * @param out_found boolean that indicates whether the value is found or not
 * @return true if no exception occurred.
 */
BARQ_API bool barq_list_find(const barq_list_t*, const barq_value_t* value, size_t* out_index, bool* out_found);

/**
 * Set the value at @a index.
 *
 * @param value The value to set.
 * @return True if no exception occurred.
 */
BARQ_API bool barq_list_set(barq_list_t*, size_t index, barq_value_t value);

/**
 * Insert @a value at @a index.
 *
 * @param value The value to insert.
 * @return True if no exception occurred.
 */
BARQ_API bool barq_list_insert(barq_list_t*, size_t index, barq_value_t value);

/**
 * Insert a collection inside a list (only available for mixed types)
 *
 * @param list valid ptr to a list of mixed
 * @param index position in the list where to add the collection
 * @return pointer to a valid collection that has been just inserted at the index passed as argument
 */
BARQ_API barq_list_t* barq_list_insert_list(barq_list_t* list, size_t index);
BARQ_API barq_dictionary_t* barq_list_insert_dictionary(barq_list_t* list, size_t index);

/**
 * Set a collection inside a list (only available for mixed types).
 * If the list already contains a collection of the requested type, the
 * operation is idempotent.
 *
 * @param list valid ptr to a list where a nested collection needs to be set
 * @param index position in the list where to set the collection
 * @return a valid ptr representing the collection just set
 */
BARQ_API barq_list_t* barq_list_set_list(barq_list_t* list, size_t index);
BARQ_API barq_dictionary_t* barq_list_set_dictionary(barq_list_t* list, size_t index);

/**
 * Returns a nested list if such collection exists, NULL otherwise.
 *
 * @param list pointer to the list that containes the nested list
 * @param index index of collection in the list
 * @return a pointer to the the nested list found at the index passed as argument
 */
BARQ_API barq_list_t* barq_list_get_list(barq_list_t* list, size_t index);

/**
 * Returns a nested dictionary if such collection exists, NULL otherwise.
 *
 * @param list pointer to the list that containes the nested collection into
 * @param index position of collection in the list
 * @return a pointer to the the nested dictionary found at index passed as argument
 */
BARQ_API barq_dictionary_t* barq_list_get_dictionary(barq_list_t* list, size_t index);

/**
 * Move the element at @a from_index to @a to_index.
 *
 * @param from_index The index of the element to move.
 * @param to_index The index to move the element to.
 * @return True if no exception occurred.
 */
BARQ_API bool barq_list_move(barq_list_t*, size_t from_index, size_t to_index);

/**
 * Insert an embedded object at a given position.
 *
 * @return A non-NULL pointer if the object was created successfully.
 */
BARQ_API barq_object_t* barq_list_insert_embedded(barq_list_t*, size_t index);

/**
 * Create an embedded object at a given position.
 *
 * @return A non-NULL pointer if the object was created successfully.
 */
BARQ_API barq_object_t* barq_list_set_embedded(barq_list_t*, size_t index);

/**
 * Get object identified at index
 *
 * @return A non-NULL pointer if value is an object.
 */
BARQ_API barq_object_t* barq_list_get_linked_object(barq_list_t*, size_t index);

/**
 * Erase the element at @a index.
 *
 * @return True if no exception occurred.
 */
BARQ_API bool barq_list_erase(barq_list_t*, size_t index);

/**
 * Clear a list, removing all elements in the list. In a list of links, this
 * does *NOT* delete the target objects.
 *
 * @return True if no exception occurred.
 */
BARQ_API bool barq_list_clear(barq_list_t*);

/**
 * In a list of objects, delete all objects in the list and clear the list. In a
 * list of values, clear the list.
 *
 * @return True if no exception occurred.
 */
BARQ_API bool barq_list_remove_all(barq_list_t*);

/**
 * Subscribe to notifications for this object.
 *
 * @return A non-null pointer if no exception occurred.
 */
BARQ_API barq_notification_token_t* barq_list_add_notification_callback(barq_list_t*, barq_userdata_t userdata,
                                                                         barq_free_userdata_func_t userdata_free,
                                                                         barq_key_path_array_t* key_path_array,
                                                                         barq_on_collection_change_func_t on_change);

/**
 * Get an list from a thread-safe reference, potentially originating in a
 * different `barq_t` instance
 */
BARQ_API barq_list_t* barq_list_from_thread_safe_reference(const barq_t*, barq_thread_safe_reference_t*);

/**
 * True if an object notification indicates that the object was deleted.
 *
 * This function cannot fail.
 */
BARQ_API bool barq_object_changes_is_deleted(const barq_object_changes_t*);

/**
 * Get the number of properties that were modified in an object notification.
 *
 * This function cannot fail.
 */
BARQ_API size_t barq_object_changes_get_num_modified_properties(const barq_object_changes_t*);

/**
 * Get the column keys for the properties that were modified in an object
 * notification.
 *
 * This function cannot fail.
 *
 * @param out_modified Where the column keys should be written. May be NULL.
 * @param max The maximum number of column keys to write.
 * @return The number of column keys written to @a out_modified, or the number
 *         of modified properties if @a out_modified is NULL.
 */
BARQ_API size_t barq_object_changes_get_modified_properties(const barq_object_changes_t*,
                                                            barq_property_key_t* out_modified, size_t max);

/**
 * Get the number of various types of changes in a collection notification.
 *
 * @param out_num_deletions The number of deletions. May be NULL.
 * @param out_num_insertions The number of insertions. May be NULL.
 * @param out_num_modifications The number of modifications. May be NULL.
 * @param out_num_moves The number of moved elements. May be NULL.
 * @param out_collection_was_cleared a flag to signal if the collection has been cleared. May be NULL
 * @param out_collection_was_deleted a flag to signal if the collection has been deleted. May be NULL
 */
BARQ_API void barq_collection_changes_get_num_changes(const barq_collection_changes_t*, size_t* out_num_deletions,
                                                      size_t* out_num_insertions, size_t* out_num_modifications,
                                                      size_t* out_num_moves, bool* out_collection_was_cleared,
                                                      bool* out_collection_was_deleted);

/**
 * Get the number of various types of changes in a collection notification,
 * suitable for acquiring the change indices as ranges, which is much more
 * compact in memory than getting the individual indices when multiple adjacent
 * elements have been modified.
 *
 * @param out_num_deletion_ranges The number of deleted ranges. May be NULL.
 * @param out_num_insertion_ranges The number of inserted ranges. May be NULL.
 * @param out_num_modification_ranges The number of modified ranges. May be
 *                                    NULL.
 * @param out_num_moves The number of moved elements. May be NULL.
 */
BARQ_API void barq_collection_changes_get_num_ranges(const barq_collection_changes_t*,
                                                     size_t* out_num_deletion_ranges,
                                                     size_t* out_num_insertion_ranges,
                                                     size_t* out_num_modification_ranges, size_t* out_num_moves);
typedef struct barq_collection_move {
    size_t from;
    size_t to;
} barq_collection_move_t;

typedef struct barq_index_range {
    size_t from;
    size_t to;
} barq_index_range_t;

/**
 * Get the indices of changes in a collection notification.
 *
 * Note: For moves, every `from` index will also be present among deletions, and
 *       every `to` index will also be present among insertions.
 *
 * This function cannot fail.
 *
 * @param out_deletion_indices Where to put the indices of deleted elements
 *                             (*before* the deletion happened). May be NULL.
 * @param max_deletion_indices The max number of indices to write to @a
 *                             out_deletion_indices.
 * @param out_insertion_indices Where the put the indices of inserted elements
 *                              (*after* the insertion happened). May be NULL.
 * @param max_insertion_indices The max number of indices to write to @a
 *                              out_insertion_indices.
 * @param out_modification_indices Where to put the indices of modified elements
 *                                 (*before* any insertions or deletions of
 *                                 other elements). May be NULL.
 * @param max_modification_indices The max number of indices to write to @a
 *                                 out_modification_indices.
 * @param out_modification_indices_after Where to put the indices of modified
 *                                       elements (*after* any insertions or
 *                                       deletions of other elements). May be
 *                                       NULL.
 * @param max_modification_indices_after The max number of indices to write to
 *                                       @a out_modification_indices_after.
 * @param out_moves Where to put the pairs of indices of moved elements. May be
 *                  NULL.
 * @param max_moves The max number of pairs to write to @a out_moves.
 */
BARQ_API void barq_collection_changes_get_changes(const barq_collection_changes_t*, size_t* out_deletion_indices,
                                                  size_t max_deletion_indices, size_t* out_insertion_indices,
                                                  size_t max_insertion_indices, size_t* out_modification_indices,
                                                  size_t max_modification_indices,
                                                  size_t* out_modification_indices_after,
                                                  size_t max_modification_indices_after,
                                                  barq_collection_move_t* out_moves, size_t max_moves);

BARQ_API void barq_collection_changes_get_ranges(
    const barq_collection_changes_t*, barq_index_range_t* out_deletion_ranges, size_t max_deletion_ranges,
    barq_index_range_t* out_insertion_ranges, size_t max_insertion_ranges,
    barq_index_range_t* out_modification_ranges, size_t max_modification_ranges,
    barq_index_range_t* out_modification_ranges_after, size_t max_modification_ranges_after,
    barq_collection_move_t* out_moves, size_t max_moves);

/**
 * Returns the number of changes occurred to the dictionary passed as argument
 *
 * @param changes valid ptr to the dictionary changes structure
 * @param out_deletions_size number of deletions
 * @param out_insertion_size number of insertions
 * @param out_modification_size number of modifications
 * @param out_was_deleted a flag to signal if the dictionary has been deleted.
 */
BARQ_API void barq_dictionary_get_changes(const barq_dictionary_changes_t* changes, size_t* out_deletions_size,
                                          size_t* out_insertion_size, size_t* out_modification_size,
                                          bool* out_was_deleted);

/**
 * Returns the list of keys changed for the dictionary passed as argument.
 * The user must assure that there is enough memory to accomodate all the keys
 * calling `barq_dictionary_get_changes` before.
 *
 * @param changes valid ptr to the dictionary changes structure
 * @param deletions list of deleted keys
 * @param deletions_size size of the list of deleted keys
 * @param insertions list of inserted keys
 * @param insertions_size size of the list of inserted keys
 * @param modifications list of modified keys
 * @param modification_size size of the list of modified keys
 * @param collection_was_cleared whether or not the collection was cleared
 */
BARQ_API void barq_dictionary_get_changed_keys(const barq_dictionary_changes_t* changes, barq_value_t* deletions,
                                               size_t* deletions_size, barq_value_t* insertions,
                                               size_t* insertions_size, barq_value_t* modifications,
                                               size_t* modification_size, bool* collection_was_cleared);

/**
 * Get a set instance for the property of an object.
 *
 * Note: It is up to the caller to call `barq_release()` on the returned set.
 *
 * @return A non-null pointer if no exception occurred.
 */
BARQ_API barq_set_t* barq_get_set(barq_object_t*, barq_property_key_t);

/**
 * Create a `barq_set_t` from a pointer to a `barq::object_store::Set`,
 * copy-constructing the internal representation.
 *
 * @param pset A pointer to an instance of `barq::object_store::Set`.
 * @param n Must be equal to `sizeof(barq::object_store::Set)`.
 * @return A non-null pointer if no exception occurred.
 */
BARQ_API barq_set_t* _barq_set_from_native_copy(const void* pset, size_t n);

/**
 * Create a `barq_set_t` from a pointer to a `barq::object_store::Set`,
 * move-constructing the internal representation.
 *
 * @param pset A pointer to an instance of `barq::object_store::Set`.
 * @param n Must be equal to `sizeof(barq::object_store::Set)`.
 * @return A non-null pointer if no exception occurred.
 */
BARQ_API barq_set_t* _barq_set_from_native_move(void* pset, size_t n);

/**
 * Resolve the set in the context of a given Barq instance.
 *
 * This is equivalent to producing a thread-safe reference and resolving it in the frozen barq.
 *
 * If resolution is possible, a valid resolved object is produced at '*resolved*'.
 * If resolution is not possible, but no error occurs, '*resolved' is set to NULL
 *
 * @return true if no error occurred.
 */
BARQ_API bool barq_set_resolve_in(const barq_set_t* list, const barq_t* target_barq, barq_set_t** resolved);

/**
 * Check if a set is valid.
 *
 * @return True if the set is valid.
 */
BARQ_API bool barq_set_is_valid(const barq_set_t*);

/**
 * Get the size of a set, in number of unique elements.
 *
 * This function may fail if the object owning the set has been deleted.
 *
 * @param out_size Where to put the set size. May be NULL.
 * @return True if no exception occurred.
 */
BARQ_API bool barq_set_size(const barq_set_t*, size_t* out_size);

/**
 * Get the property that this set came from.
 *
 * @return True if no exception occurred.
 */
BARQ_API bool barq_set_get_property(const barq_set_t*, barq_property_info_t* out_property_info);

/**
 * Get the value at @a index.
 *
 * Note that elements in a set move around arbitrarily when other elements are
 * inserted/removed.
 *
 * @param out_value The resulting value, if no error occurred. May be NULL,
 *                  though nonsensical.
 * @return True if no exception occurred.
 */
BARQ_API bool barq_set_get(const barq_set_t*, size_t index, barq_value_t* out_value);

/**
 * Find an element in a set.
 *
 * If @a value has a type that is incompatible with the set, it will be reported
 * as not existing in the set.
 *
 * @param value The value to look for in the set.
 * @param out_index If non-null, and the element is found, this will be
 *                  populated with the index of the found element in the set.
 * @param out_found If non-null, will be set to true if the element was found,
 *                  otherwise will be set to false.
 * @return True if no exception occurred.
 */
BARQ_API bool barq_set_find(const barq_set_t*, barq_value_t value, size_t* out_index, bool* out_found);

/**
 * Insert an element in a set.
 *
 * If the element is already in the set, this function does nothing (and does
 * not report an error).
 *
 * @param value The value to insert.
 * @param out_index If non-null, will be set to the index of the inserted
 *                  element, or the index of the existing element.
 * @param out_inserted If non-null, will be set to true if the element did not
 *                     already exist in the set. Otherwise set to false.
 * @return True if no exception occurred.
 */
BARQ_API bool barq_set_insert(barq_set_t*, barq_value_t value, size_t* out_index, bool* out_inserted);

/**
 * Erase an element from a set.
 *
 * If the element does not exist in the set, this function does nothing (and
 * does not report an error).
 *
 * @param value The value to erase.
 * @param out_erased If non-null, will be set to true if the element was found
 *                   and erased, and otherwise set to false.
 * @return True if no exception occurred.
 */
BARQ_API bool barq_set_erase(barq_set_t*, barq_value_t value, bool* out_erased);

/**
 * Clear a set of values.
 *
 * @return True if no exception occurred.
 */
BARQ_API bool barq_set_clear(barq_set_t*);

/**
 * In a set of objects, delete all objects in the set and clear the set. In a
 * set of values, clear the set.
 *
 * @return True if no exception occurred.
 */
BARQ_API bool barq_set_remove_all(barq_set_t*);

/**
 * Subscribe to notifications for this object.
 *
 * @return A non-null pointer if no exception occurred.
 */
BARQ_API barq_notification_token_t* barq_set_add_notification_callback(barq_set_t*, barq_userdata_t userdata,
                                                                        barq_free_userdata_func_t userdata_free,
                                                                        barq_key_path_array_t* key_path_array,
                                                                        barq_on_collection_change_func_t on_change);
/**
 * Get an set from a thread-safe reference, potentially originating in a
 * different `barq_t` instance
 */
BARQ_API barq_set_t* barq_set_from_thread_safe_reference(const barq_t*, barq_thread_safe_reference_t*);

/**
 * Get a dictionary instance for the property of an object.
 *
 * Note: It is up to the caller to call `barq_release()` on the returned dictionary.
 *
 * @return A non-null pointer if no exception occurred.
 */
BARQ_API barq_dictionary_t* barq_get_dictionary(barq_object_t*, barq_property_key_t);

/**
 * Create a `barq_dictionary_t` from a pointer to a `barq::object_store::Dictionary`,
 * copy-constructing the internal representation.
 *
 * @param pdict A pointer to an instance of `barq::object_store::Dictionary`.
 * @param n Must be equal to `sizeof(barq::object_store::Dictionary)`.
 * @return A non-null pointer if no exception occurred.
 */
BARQ_API barq_dictionary_t* _barq_dictionary_from_native_copy(const void* pdict, size_t n);

/**
 * Create a `barq_dictionary_t` from a pointer to a `barq::object_store::Dictionary`,
 * move-constructing the internal representation.
 *
 * @param pdict A pointer to an instance of `barq::object_store::Dictionary`.
 * @param n Must be equal to `sizeof(barq::object_store::Dictionary)`.
 * @return A non-null pointer if no exception occurred.
 */
BARQ_API barq_dictionary_t* _barq_dictionary_from_native_move(void* pdict, size_t n);

/**
 * Resolve the list in the context of a given Barq instance.
 *
 * This is equivalent to producing a thread-safe reference and resolving it in the frozen barq.
 *
 * If resolution is possible, a valid resolved object is produced at '*resolved*'.
 * If resolution is not possible, but no error occurs, '*resolved' is set to NULL
 *
 * @return true if no error occurred.
 */
BARQ_API bool barq_dictionary_resolve_in(const barq_dictionary_t* list, const barq_t* target_barq,
                                         barq_dictionary_t** resolved);

/**
 * Check if a list is valid.
 *
 * @return True if the list is valid.
 */
BARQ_API bool barq_dictionary_is_valid(const barq_dictionary_t*);

/**
 * Get the size of a dictionary (the number of unique keys).
 *
 * This function may fail if the object owning the dictionary has been deleted.
 *
 * @param out_size Where to put the dictionary size. May be NULL.
 * @return True if no exception occurred.
 */
BARQ_API bool barq_dictionary_size(const barq_dictionary_t*, size_t* out_size);


/**
 * Get the property that this dictionary came from.
 *
 * @return True if no exception occurred.
 */
BARQ_API bool barq_dictionary_get_property(const barq_dictionary_t*, barq_property_info_t* out_info);

/**
 * Find an element in a dictionary.
 *
 * @param key The key to look for.
 * @param out_value If non-null, the value for the corresponding key.
 * @param out_found If non-null, will be set to true if the dictionary contained the key.
 * @return True if no exception occurred.
 */
BARQ_API bool barq_dictionary_find(const barq_dictionary_t*, barq_value_t key, barq_value_t* out_value,
                                   bool* out_found);

/**
 * Get the key-value pair at @a index.
 *
 * Note that the indices of elements in the dictionary move around as other
 * elements are inserted/removed.
 *
 * @param index The index in the dictionary.
 * @param out_key If non-null, will be set to the key at the corresponding index.
 * @param out_value If non-null, will be set to the value at the corresponding index.
 * @return True if no exception occurred.
 */
BARQ_API bool barq_dictionary_get(const barq_dictionary_t*, size_t index, barq_value_t* out_key,
                                  barq_value_t* out_value);

/**
 * Insert or update an element in a dictionary.
 *
 * If the key already exists, the value will be overwritten.
 *
 * @param key The lookup key.
 * @param value The value to insert.
 * @param out_index If non-null, will be set to the index of the element after
 *                  insertion/update.
 * @param out_inserted If non-null, will be set to true if the key did not
 *                     already exist.
 * @return True if no exception occurred.
 */
BARQ_API bool barq_dictionary_insert(barq_dictionary_t*, barq_value_t key, barq_value_t value, size_t* out_index,
                                     bool* out_inserted);

/**
 * Insert an embedded object.
 *
 * @return A non-NULL pointer if the object was created successfully.
 */
BARQ_API barq_object_t* barq_dictionary_insert_embedded(barq_dictionary_t*, barq_value_t key);

/**
 * Insert a collection inside a dictionary (only available for mixed types)
 *
 * @param dictionary valid ptr to a dictionary of mixed
 * @param key the mixed representing a key for a dictionary (only string)
 * @return pointer to a valid collection that has been just inserted at the key passed as argument
 */
BARQ_API barq_list_t* barq_dictionary_insert_list(barq_dictionary_t* dictionary, barq_value_t key);
BARQ_API barq_dictionary_t* barq_dictionary_insert_dictionary(barq_dictionary_t*, barq_value_t);


/**
 * Fetch a list from a dictionary.
 * @return a valid list that needs to be deleted by the caller or nullptr in case of an error.
 */
BARQ_API barq_list_t* barq_dictionary_get_list(barq_dictionary_t* dictionary, barq_value_t key);

/**
 * Fetch a dictioanry from a dictionary.
 * @return a valid dictionary that needs to be deleted by the caller or nullptr in case of an error.
 */
BARQ_API barq_dictionary_t* barq_dictionary_get_dictionary(barq_dictionary_t* dictionary, barq_value_t key);

/**
 * Get object identified by key
 *
 * @return A non-NULL pointer if the value associated with key is an object.
 */
BARQ_API barq_object_t* barq_dictionary_get_linked_object(barq_dictionary_t*, barq_value_t key);

/**
 * Erase a dictionary element.
 *
 * @param key The key of the element to erase.
 * @param out_erased If non-null, will be set to true if the element was found
 *                   and erased.
 * @return True if no exception occurred.
 */
BARQ_API bool barq_dictionary_erase(barq_dictionary_t*, barq_value_t key, bool* out_erased);

/**
 * Return the list of keys stored in the dictionary
 *
 * @param out_size number of keys
 * @param out_keys the list of keys in the dictionary, the memory has to be released once it is no longer used.
 * @return True if no exception occurred.
 */
BARQ_API bool barq_dictionary_get_keys(barq_dictionary_t*, size_t* out_size, barq_results_t** out_keys);

/**
 * Check if the dictionary contains a certain key
 *
 * @param key to search in the dictionary
 * @param found True if the such key exists
 * @return True if no exception occurred
 */
BARQ_API bool barq_dictionary_contains_key(const barq_dictionary_t*, barq_value_t key, bool* found);

/**
 * Check if the dictionary contains a certain value
 *
 * @param value to search in the dictionary
 * @param index the index of the value in the dictionry if such value exists
 * @return True if no exception occurred
 */
BARQ_API bool barq_dictionary_contains_value(const barq_dictionary_t*, barq_value_t value, size_t* index);


/**
 * Clear a dictionary.
 *
 * @return True if no exception occurred.
 */
BARQ_API bool barq_dictionary_clear(barq_dictionary_t*);

/**
 * Subscribe to notifications for this object.
 *
 * @return A non-null pointer if no exception occurred.
 */
BARQ_API barq_notification_token_t* barq_dictionary_add_notification_callback(
    barq_dictionary_t*, barq_userdata_t userdata, barq_free_userdata_func_t userdata_free,
    barq_key_path_array_t* key_path_array, barq_on_dictionary_change_func_t on_change);

/**
 * Get an dictionary from a thread-safe reference, potentially originating in a
 * different `barq_t` instance
 */
BARQ_API barq_dictionary_t* barq_dictionary_from_thread_safe_reference(const barq_t*,
                                                                        barq_thread_safe_reference_t*);

/**
 * Parse a query string and bind it to a table.
 *
 * If the query failed to parse, the parser error is available from
 * `barq_get_last_error()`.
 *
 * @param target_table The table on which to run this query.
 * @param query_string A zero-terminated string in the Barq Query Language,
 *                     optionally containing argument placeholders (`$0`, `$1`,
 *                     etc.).
 * @param num_args The number of arguments for this query.
 * @param args A pointer to a list of argument values.
 * @return A non-null pointer if the query was successfully parsed and no
 *         exception occurred.
 */
BARQ_API barq_query_t* barq_query_parse(const barq_t*, barq_class_key_t target_table, const char* query_string,
                                         size_t num_args, const barq_query_arg_t* args);


/**
 * Get textual representation of query
 *
 * @return a string containing the description. The string memory is managed by the query object.
 */
BARQ_API const char* barq_query_get_description(barq_query_t*);


/**
 * Parse a query string and append it to an existing query via logical &&.
 * The query string applies to the same table and Barq as the existing query.
 *
 * If the query failed to parse, the parser error is available from
 * `barq_get_last_error()`.
 *
 * @param query_string A zero-terminated string in the Barq Query Language,
 *                     optionally containing argument placeholders (`$0`, `$1`,
 *                     etc.).
 * @param num_args The number of arguments for this query.
 * @param args A pointer to a list of argument values.
 * @return A non-null pointer if the query was successfully parsed and no
 *         exception occurred.
 */
BARQ_API barq_query_t* barq_query_append_query(const barq_query_t*, const char* query_string, size_t num_args,
                                                const barq_query_arg_t* args);

/**
 * Parse a query string and bind it to a list.
 *
 * If the query failed to parse, the parser error is available from
 * `barq_get_last_error()`.
 *
 * @param target_list The list on which to run this query.
 * @param query_string A string in the Barq Query Language, optionally
 *                     containing argument placeholders (`$0`, `$1`, etc.).
 * @param num_args The number of arguments for this query.
 * @param args A pointer to a list of argument values.
 * @return A non-null pointer if the query was successfully parsed and no
 *         exception occurred.
 */
BARQ_API barq_query_t* barq_query_parse_for_list(const barq_list_t* target_list, const char* query_string,
                                                  size_t num_args, const barq_query_arg_t* args);

/**
 * Parse a query string and bind it to a set.
 *
 * If the query failed to parse, the parser error is available from
 * `barq_get_last_error()`.
 *
 * @param target_set The set on which to run this query.
 * @param query_string A string in the Barq Query Language, optionally
 *                     containing argument placeholders (`$0`, `$1`, etc.).
 * @param num_args The number of arguments for this query.
 * @param args A pointer to a list of argument values.
 * @return A non-null pointer if the query was successfully parsed and no
 *         exception occurred.
 */
BARQ_API barq_query_t* barq_query_parse_for_set(const barq_set_t* target_set, const char* query_string,
                                                 size_t num_args, const barq_query_arg_t* args);
/**
 * Parse a query string and bind it to another query result.
 *
 * If the query failed to parse, the parser error is available from
 * `barq_get_last_error()`.
 *
 * @param target_results The results on which to run this query.
 * @param query_string A zero-terminated string in the Barq Query Language,
 *                     optionally containing argument placeholders (`$0`, `$1`,
 *                     etc.).
 * @param num_args The number of arguments for this query.
 * @param args A pointer to a list of argument values.
 * @return A non-null pointer if the query was successfully parsed and no
 *         exception occurred.
 */
BARQ_API barq_query_t* barq_query_parse_for_results(const barq_results_t* target_results, const char* query_string,
                                                     size_t num_args, const barq_query_arg_t* args);

/**
 * Count the number of objects found by this query.
 */
BARQ_API bool barq_query_count(const barq_query_t*, size_t* out_count);

/**
 * Return the first object matched by this query.
 *
 * Note: This function can only produce objects, not values. Use the
 *       `barq_results_t` returned by `barq_query_find_all()` to retrieve
 *       values from a list of primitive values.
 *
 * @param out_value Where to write the result, if any object matched the query.
 *                  May be NULL.
 * @param out_found Where to write whether the object was found. May be NULL.
 * @return True if no exception occurred.
 */
BARQ_API bool barq_query_find_first(barq_query_t*, barq_value_t* out_value, bool* out_found);

/**
 * Produce a results object for this query.
 *
 * Note: This does not actually run the query until the results are accessed in
 *       some way.
 *
 * @return A non-null pointer if no exception occurred.
 */
BARQ_API barq_results_t* barq_query_find_all(barq_query_t*);

/**
 * Convert a list to results.
 *
 * @return A non-null pointer if no exception occurred.
 */
BARQ_API barq_results_t* barq_list_to_results(barq_list_t*);

/**
 * Convert a set to results.
 *
 * @return A non-null pointer if no exception occurred.
 */
BARQ_API barq_results_t* barq_set_to_results(barq_set_t*);

/**
 * Convert a dictionary to results.
 *
 * @return A non-null pointer if no exception occurred.
 */
BARQ_API barq_results_t* barq_dictionary_to_results(barq_dictionary_t*);

/**
 * Fetch the backlinks for the object passed as argument.
 * @return a valid ptr to barq results that contains all the backlinks for the object, or null in case of errors.
 */
BARQ_API barq_results_t* barq_get_backlinks(barq_object_t* object, barq_class_key_t source_table_key,
                                             barq_property_key_t property_key);

/**
 * Delete all objects matched by a query.
 */
BARQ_API bool barq_query_delete_all(const barq_query_t*);

/**
 * Set the boolean passed as argument to true or false whether the barq_results passed is valid or not
 * @return true/false if no exception has occurred.
 */
BARQ_API bool barq_results_is_valid(const barq_results_t*, bool*);

/**
 * Count the number of results.
 *
 * If the result is "live" (not a snapshot), this may rerun the query if things
 * have changed.
 *
 * @return True if no exception occurred.
 */
BARQ_API bool barq_results_count(barq_results_t*, size_t* out_count);

/**
 * Create a new results object by further filtering existing result.
 *
 * @return A non-null pointer if no exception occurred.
 */
BARQ_API barq_results_t* barq_results_filter(barq_results_t*, barq_query_t*);

/**
 * Create a new results object by further sorting existing result.
 *
 * @param sort_string Specifies a sort condition. It has the format
          <param> ["," <param>]*
          <param> ::= <prop> ["." <prop>]* <direction>,
          <direction> ::= "ASCENDING" | "DESCENDING"
 * @return A non-null pointer if no exception occurred.
 */
BARQ_API barq_results_t* barq_results_sort(barq_results_t* results, const char* sort_string);

/**
 * Create a new results object by removing duplicates
 *
 * @param distinct_string Specifies a distinct condition. It has the format
          <param> ["," <param>]*
          <param> ::= <prop> ["." <prop>]*
 * @return A non-null pointer if no exception occurred.
 */
BARQ_API barq_results_t* barq_results_distinct(barq_results_t* results, const char* distinct_string);

/**
 * Create a new results object by limiting the number of items
 *
 * @param max_count Specifies the number of elements the new result can have at most
 * @return A non-null pointer if no exception occurred.
 */
BARQ_API barq_results_t* barq_results_limit(barq_results_t* results, size_t max_count);

/**
 * Get the matching element at @a index in the results.
 *
 * If the result is "live" (not a snapshot), this may rerun the query if things
 * have changed.
 *
 * Note: The bound returned by `barq_results_count()` for a non-snapshot result
 *       is not a reliable way to iterate over elements in the result, because
 *       the result will be live-updated if changes are made in each iteration
 *       that may change the number of query results or even change the
 *       ordering. In other words, this method should probably only be used with
 *       snapshot results.
 *
 * @return True if no exception occurred (including out-of-bounds).
 */
BARQ_API bool barq_results_get(barq_results_t*, size_t index, barq_value_t* out_value);

/**
 * Returns an instance of barq_list at the index passed as argument.
 * @return A valid ptr to a list instance or nullptr in case of errors
 */
BARQ_API barq_list_t* barq_results_get_list(barq_results_t*, size_t index);

/**
 * Returns an instance of barq_dictionary for the index passed as argument.
 * @return A valid ptr to a dictionary instance or nullptr in case of errors
 */
BARQ_API barq_dictionary_t* barq_results_get_dictionary(barq_results_t*, size_t index);

/**
 * Find the index for the value passed as parameter inside barq results pointer passed a input parameter.
 *  @param value the value to find inside the barq results
 *  @param out_index the index where the object has been found, or barq::not_found
 *  @param out_found boolean indicating if the value has been found or not
 *  @return true if no error occurred, false otherwise
 */
BARQ_API bool barq_results_find(barq_results_t*, barq_value_t* value, size_t* out_index, bool* out_found);

/**
 * Get the matching object at @a index in the results.
 *
 * If the result is "live" (not a snapshot), this may rerun the query if things
 * have changed.
 *
 * Note: The bound returned by `barq_results_count()` for a non-snapshot result
 *       is not a reliable way to iterate over elements in the result, because
 *       the result will be live-updated if changes are made in each iteration
 *       that may change the number of query results or even change the
 *       ordering. In other words, this method should probably only be used with
 *       snapshot results.
 *
 * @return An instance of `barq_object_t` if no exception occurred.
 */
BARQ_API barq_object_t* barq_results_get_object(barq_results_t*, size_t index);

/**
 * Return the query associated to the results passed as argument.
 *
 * @param results the ptr to a valid results object.
 * @return a valid ptr to barq_query_t if no error has occurred
 */
BARQ_API barq_query_t* barq_results_get_query(barq_results_t* results);

/**
 * Find the index for the barq object passed as parameter inside barq results pointer passed a input parameter.
 *  @param value the value to find inside the barq results
 *  @param out_index the index where the object has been found, or barq::not_found
 *  @param out_found boolean indicating if the value has been found or not
 *  @return true if no error occurred, false otherwise
 */
BARQ_API bool barq_results_find_object(barq_results_t*, barq_object_t* value, size_t* out_index, bool* out_found);

/**
 * Delete all objects in the result.
 *
 * If the result if "live" (not a snapshot), this may rerun the query if things
 * have changed.
 *
 * @return True if no exception occurred.
 */
BARQ_API bool barq_results_delete_all(barq_results_t*);

/**
 * Return a snapshot of the results that never automatically updates.
 *
 * The returned result is suitable for use with `barq_results_count()` +
 * `barq_results_get()`.
 */
BARQ_API barq_results_t* barq_results_snapshot(const barq_results_t*);

/**
 * Map the Results into a live Barq instance.
 *
 * This is equivalent to producing a thread-safe reference and resolving it in the live barq.
 *
 * @return A live copy of the Results.
 */
BARQ_API barq_results_t* barq_results_resolve_in(barq_results_t* from_results, const barq_t* target_barq);

/**
 * Compute the minimum value of a property in the results.
 *
 * @param out_min Where to write the result, if there were matching rows.
 * @param out_found Set to true if there are matching rows.
 * @return True if no exception occurred.
 */
BARQ_API bool barq_results_min(barq_results_t*, barq_property_key_t, barq_value_t* out_min, bool* out_found);

/**
 * Compute the maximum value of a property in the results.
 *
 * @param out_max Where to write the result, if there were matching rows.
 * @param out_found Set to true if there are matching rows.
 * @return True if no exception occurred.
 */
BARQ_API bool barq_results_max(barq_results_t*, barq_property_key_t, barq_value_t* out_max, bool* out_found);

/**
 * Compute the sum value of a property in the results.
 *
 * @param out_sum Where to write the result. Zero if no rows matched.
 * @param out_found Set to true if there are matching rows.
 * @return True if no exception occurred.
 */
BARQ_API bool barq_results_sum(barq_results_t*, barq_property_key_t, barq_value_t* out_sum, bool* out_found);

/**
 * Compute the average value of a property in the results.
 *
 * Note: For numeric columns, the average is always converted to double.
 *
 * @param out_average Where to write the result.
 * @param out_found Set to true if there are matching rows.
 * @return True if no exception occurred.
 */
BARQ_API bool barq_results_average(barq_results_t*, barq_property_key_t, barq_value_t* out_average,
                                   bool* out_found);

BARQ_API barq_notification_token_t* barq_results_add_notification_callback(barq_results_t*,
                                                                            barq_userdata_t userdata,
                                                                            barq_free_userdata_func_t userdata_free,
                                                                            barq_key_path_array_t* key_path_array,
                                                                            barq_on_collection_change_func_t);

/**
 * Get an results object from a thread-safe reference, potentially originating
 * in a different `barq_t` instance
 */
BARQ_API barq_results_t* barq_results_from_thread_safe_reference(const barq_t*, barq_thread_safe_reference_t*);

/* HTTP transport */
typedef enum barq_http_request_method {
    BARQ_HTTP_REQUEST_METHOD_GET,
    BARQ_HTTP_REQUEST_METHOD_POST,
    BARQ_HTTP_REQUEST_METHOD_PATCH,
    BARQ_HTTP_REQUEST_METHOD_PUT,
    BARQ_HTTP_REQUEST_METHOD_DELETE,
} barq_http_request_method_e;

typedef struct barq_http_header {
    const char* name;
    const char* value;
} barq_http_header_t;

typedef struct barq_http_request {
    barq_http_request_method_e method;
    const char* url;
    uint64_t timeout_ms;
    const barq_http_header_t* headers;
    size_t num_headers;
    const char* body;
    size_t body_size;
} barq_http_request_t;

typedef struct barq_http_response {
    int status_code;
    int custom_status_code;
    const barq_http_header_t* headers;
    size_t num_headers;
    const char* body;
    size_t body_size;
} barq_http_response_t;

/**
 * Callback function used by Core to make a HTTP request.
 *
 * Complete the request by calling barq_http_transport_complete_request(),
 * passing in the request_context pointer here and the received response.
 * Network request are expected to be asynchronous and can be completed on any thread.
 *
 * @param request The request to send.
 * @param request_context Internal state pointer of Core, needed by barq_http_transport_complete_request().
 */
typedef void (*barq_http_request_func_t)(barq_userdata_t userdata, const barq_http_request_t request,
                                          void* request_context);

typedef struct barq_http_transport barq_http_transport_t;

/**
 * Create a new HTTP transport with these callbacks implementing its functionality.
 */
BARQ_API barq_http_transport_t* barq_http_transport_new(barq_http_request_func_t, barq_userdata_t userdata,
                                                         barq_free_userdata_func_t userdata_free);

/**
 * Complete a HTTP request with the given response.
 *
 * @param request_context Internal state pointer passed by Core when invoking barq_http_request_func_t
 *                        to start the request.
 * @param response The server response to the HTTP request initiated by Core.
 */
BARQ_API void barq_http_transport_complete_request(void* request_context, const barq_http_response_t* response);

/* App */
typedef struct barq_user barq_user_t;
typedef enum barq_user_state {
    BARQ_USER_STATE_LOGGED_OUT,
    BARQ_USER_STATE_LOGGED_IN,
    BARQ_USER_STATE_REMOVED
} barq_user_state_e;

// This type should never be returned from a function.
// It's only meant as an asynchronous callback argument.
// Pointers to this struct and its pointer members are only valid inside the scope
// of the callback they were passed to.
typedef struct barq_app_error {
    barq_errno_e error;
    barq_error_categories categories;
    const char* message;

    /**
     * The underlying HTTP status code returned by the server,
     * otherwise zero.
     */
    int http_status_code;

    /**
     * A link to Barq server logs related to the error,
     * or NULL if error response didn't contain log information.
     */
    const char* link_to_server_logs;
} barq_app_error_t;

/* Sync */
typedef enum barq_sync_client_reconnect_mode {
    BARQ_SYNC_CLIENT_RECONNECT_MODE_NORMAL,
    BARQ_SYNC_CLIENT_RECONNECT_MODE_TESTING,
} barq_sync_client_reconnect_mode_e;

typedef enum barq_sync_session_resync_mode {
    BARQ_SYNC_SESSION_RESYNC_MODE_MANUAL,
    BARQ_SYNC_SESSION_RESYNC_MODE_DISCARD_LOCAL,
    BARQ_SYNC_SESSION_RESYNC_MODE_RECOVER,
    BARQ_SYNC_SESSION_RESYNC_MODE_RECOVER_OR_DISCARD,
} barq_sync_session_resync_mode_e;

typedef enum barq_sync_session_stop_policy {
    BARQ_SYNC_SESSION_STOP_POLICY_IMMEDIATELY,
    BARQ_SYNC_SESSION_STOP_POLICY_LIVE_INDEFINITELY,
    BARQ_SYNC_SESSION_STOP_POLICY_AFTER_CHANGES_UPLOADED,
} barq_sync_session_stop_policy_e;

typedef enum barq_sync_session_state {
    BARQ_SYNC_SESSION_STATE_ACTIVE,
    BARQ_SYNC_SESSION_STATE_DYING,
    BARQ_SYNC_SESSION_STATE_INACTIVE,
    BARQ_SYNC_SESSION_STATE_WAITING_FOR_ACCESS_TOKEN,
    BARQ_SYNC_SESSION_STATE_PAUSED,
} barq_sync_session_state_e;

typedef enum barq_sync_connection_state {
    BARQ_SYNC_CONNECTION_STATE_DISCONNECTED,
    BARQ_SYNC_CONNECTION_STATE_CONNECTING,
    BARQ_SYNC_CONNECTION_STATE_CONNECTED,
} barq_sync_connection_state_e;

typedef enum barq_sync_progress_direction {
    BARQ_SYNC_PROGRESS_DIRECTION_UPLOAD,
    BARQ_SYNC_PROGRESS_DIRECTION_DOWNLOAD,
} barq_sync_progress_direction_e;

typedef enum barq_sync_error_action {
    BARQ_SYNC_ERROR_ACTION_NO_ACTION,
    BARQ_SYNC_ERROR_ACTION_PROTOCOL_VIOLATION,
    BARQ_SYNC_ERROR_ACTION_APPLICATION_BUG,
    BARQ_SYNC_ERROR_ACTION_WARNING,
    BARQ_SYNC_ERROR_ACTION_TRANSIENT,
    BARQ_SYNC_ERROR_ACTION_DELETE_BARQ,
    BARQ_SYNC_ERROR_ACTION_CLIENT_RESET,
    BARQ_SYNC_ERROR_ACTION_CLIENT_RESET_NO_RECOVERY,
    BARQ_SYNC_ERROR_ACTION_MIGRATE_TO_FLX,
    BARQ_SYNC_ERROR_ACTION_REVERT_TO_PBS,
} barq_sync_error_action_e;

typedef enum barq_sync_file_action {
    BARQ_SYNC_FILE_ACTION_DELETE_BARQ,
    BARQ_SYNC_FILE_ACTION_BACK_UP_THEN_DELETE_BARQ,
} barq_sync_file_action_e;


typedef struct barq_sync_session barq_sync_session_t;
typedef struct barq_async_open_task barq_async_open_task_t;
typedef struct barq_sync_manager barq_sync_manager_t;

typedef struct barq_sync_error_user_info {
    const char* key;
    const char* value;
} barq_sync_error_user_info_t;

typedef struct barq_sync_error_compensating_write_info {
    const char* reason;
    const char* object_name;
    barq_value_t primary_key;
} barq_sync_error_compensating_write_info_t;

// The following interface allows C-API users to
// bring their own users. This API shouldn't be mixed
// with any SDK-provided User implementation.
/**
 * Generic completion callback for asynchronous Barq User operations.
 * @param userdata This must be the faithfully forwarded data parameter that was provided along with this callback.
 * @param error Pointer to an error object if the operation failed, otherwise null if it completed successfully.
 */
typedef void (*barq_user_void_completion_func_t)(barq_userdata_t userdata, const barq_app_error_t* error);


typedef const char* (*barq_user_get_access_token_cb_t)(barq_userdata_t userdata);
typedef const char* (*barq_user_get_refresh_token_cb_t)(barq_userdata_t userdata);
typedef barq_user_state_e (*barq_user_state_cb_t)(barq_userdata_t userdata);
typedef bool (*barq_user_access_token_refresh_required_cb_t)(barq_userdata_t userdata);
typedef barq_sync_manager_t* (*barq_user_get_sync_manager_cb_t)(barq_userdata_t userdata);
typedef void (*barq_user_request_log_out_cb_t)(barq_userdata_t userdata);
typedef void (*barq_user_request_refresh_location_cb_t)(barq_userdata_t userdata,
                                                         barq_user_void_completion_func_t cb,
                                                         barq_userdata_t cb_data);
typedef void (*barq_user_request_access_token_cb_t)(barq_userdata_t userdata, barq_user_void_completion_func_t cb,
                                                     barq_userdata_t cb_data);
typedef void (*barq_user_track_barq_cb_t)(barq_userdata_t userdata, const char* path);
typedef const char* (*barq_user_create_file_action_cb_t)(barq_userdata_t userdata, barq_sync_file_action_e action,
                                                          const char* original_path,
                                                          const char* requested_recovery_dir);
typedef struct barq_sync_user_create_config {
    barq_userdata_t userdata;
    barq_free_userdata_func_t free_func;
    const char* app_id;
    const char* user_id;
    barq_user_get_access_token_cb_t access_token_cb;
    barq_user_get_refresh_token_cb_t refresh_token_cb;
    barq_user_state_cb_t state_cb;
    barq_user_access_token_refresh_required_cb_t atrr_cb;
    barq_user_get_sync_manager_cb_t sync_manager_cb;
    barq_user_request_log_out_cb_t request_log_out_cb;
    barq_user_request_refresh_location_cb_t request_refresh_location_cb;
    barq_user_request_access_token_cb_t request_access_token_cb;
    barq_user_track_barq_cb_t track_barq_cb;
    barq_user_create_file_action_cb_t create_fa_cb;
} barq_sync_user_create_config_t;

/*
 * Construct a SyncUser instance that uses SDK provided
 * callbacks instead of a SDK-provided User implementation.
 */
BARQ_API barq_user_t* barq_user_new(barq_sync_user_create_config_t config) BARQ_API_NOEXCEPT;

/**
 * Create barq_sync_manager_t* instance given a valid barq sync client configuration.
 *
 * @return A non-null pointer if no error occurred.
 */
BARQ_API barq_sync_manager_t* barq_sync_manager_create(const barq_sync_client_config_t*);

/**
 * See SyncManager::set_sync_route()
 */
BARQ_API void barq_sync_manager_set_route(const barq_sync_manager_t* session, const char* route, bool is_verified);

// This type should never be returned from a function.
// It's only meant as an asynchronous callback argument.
// Pointers to this struct and its pointer members are only valid inside the scope
// of the callback they were passed to.
typedef struct barq_sync_error {
    barq_error_t status;
    const char* c_original_file_path_key;
    const char* c_recovery_file_path_key;
    bool is_fatal;
    bool is_unrecognized_by_client;
    bool is_client_reset_requested;
    barq_sync_error_action_e server_requests_action;

    barq_sync_error_user_info_t* user_info_map;
    size_t user_info_length;

    barq_sync_error_compensating_write_info_t* compensating_writes;
    size_t compensating_writes_length;
    void* user_code_error;
} barq_sync_error_t;

typedef struct barq_salted_file_ident {
    uint64_t ident;
    int64_t salt;
} barq_salted_file_ident_t;

/**
 * Callback function invoked by the sync session once it has uploaded or download
 * all available changesets. See @a barq_sync_session_wait_for_upload and
 * @a barq_sync_session_wait_for_download.
 *
 * This callback is invoked on the sync client's worker thread.
 *
 * @param error Null, if the operation completed successfully.
 */
typedef void (*barq_sync_wait_for_completion_func_t)(barq_userdata_t userdata, barq_error_t* error);
typedef void (*barq_sync_connection_state_changed_func_t)(barq_userdata_t userdata,
                                                           barq_sync_connection_state_e old_state,
                                                           barq_sync_connection_state_e new_state);
typedef void (*barq_sync_progress_func_t)(barq_userdata_t userdata, uint64_t transferred_bytes,
                                           uint64_t total_bytes, double progress_estimate);
typedef void (*barq_sync_error_handler_func_t)(barq_userdata_t userdata, barq_sync_session_t*,
                                                const barq_sync_error_t);
typedef bool (*barq_sync_ssl_verify_func_t)(barq_userdata_t userdata, const char* server_address, short server_port,
                                             const char* pem_data, size_t pem_size, int preverify_ok, int depth);
typedef bool (*barq_sync_before_client_reset_func_t)(barq_userdata_t userdata, barq_t* before_barq);
typedef bool (*barq_sync_after_client_reset_func_t)(barq_userdata_t userdata, barq_t* before_barq,
                                                     barq_thread_safe_reference_t* after_barq, bool did_recover);

typedef struct barq_flx_sync_subscription barq_flx_sync_subscription_t;
typedef struct barq_flx_sync_subscription_set barq_flx_sync_subscription_set_t;
typedef struct barq_flx_sync_mutable_subscription_set barq_flx_sync_mutable_subscription_set_t;
typedef struct barq_flx_sync_subscription_desc barq_flx_sync_subscription_desc_t;
typedef enum barq_flx_sync_subscription_set_state {
    BARQ_SYNC_SUBSCRIPTION_UNCOMMITTED = 0,
    BARQ_SYNC_SUBSCRIPTION_PENDING,
    BARQ_SYNC_SUBSCRIPTION_BOOTSTRAPPING,
    BARQ_SYNC_SUBSCRIPTION_COMPLETE,
    BARQ_SYNC_SUBSCRIPTION_ERROR,
    BARQ_SYNC_SUBSCRIPTION_SUPERSEDED,
    BARQ_SYNC_SUBSCRIPTION_AWAITING_MARK,
} barq_flx_sync_subscription_set_state_e;
typedef void (*barq_sync_on_subscription_state_changed_t)(barq_userdata_t userdata,
                                                           barq_flx_sync_subscription_set_state_e state);


typedef struct barq_async_open_task_progress_notification_token barq_async_open_task_progress_notification_token_t;
typedef struct barq_sync_session_connection_state_notification_token
    barq_sync_session_connection_state_notification_token_t;

/**
 * Callback function invoked by the async open task once the barq is open and fully synchronized.
 *
 * This callback is invoked on the sync client's worker thread.
 *
 * @param barq Downloaded barq instance, or null if an error occurred.
 *              Move to the thread you want to use it on and
 *              thaw with @a barq_from_thread_safe_reference().
 *              Be aware that once received through this call, you own
 *              the object and must release it when used.
 * @param error Null, if the operation complete successfully.
 */

// invoked when the synchronized barq file has been downloaded
typedef void (*barq_async_open_task_completion_func_t)(barq_userdata_t userdata,
                                                        barq_thread_safe_reference_t* barq,
                                                        const barq_async_error_t* error);

// invoked once the file has been downloaded. Allows the caller to run some initial subscription before the completion
// callback runs.
typedef void (*barq_async_open_task_init_subscription_func_t)(barq_thread_safe_reference_t* barq,
                                                               barq_userdata_t userdata);
// Creates a sync client config for barq_sync_manager_create().
// The returned config must be freed with barq_release().
BARQ_API barq_sync_client_config_t* barq_sync_client_config_new(void) BARQ_API_NOEXCEPT;

BARQ_API void barq_sync_client_config_set_reconnect_mode(barq_sync_client_config_t*,
                                                         barq_sync_client_reconnect_mode_e) BARQ_API_NOEXCEPT;
BARQ_API void barq_sync_client_config_set_multiplex_sessions(barq_sync_client_config_t*, bool) BARQ_API_NOEXCEPT;
BARQ_API void barq_sync_client_config_set_user_agent_binding_info(barq_sync_client_config_t*,
                                                                  const char*) BARQ_API_NOEXCEPT;
BARQ_API void barq_sync_client_config_set_user_agent_application_info(barq_sync_client_config_t*,
                                                                      const char*) BARQ_API_NOEXCEPT;
BARQ_API void barq_sync_client_config_set_connect_timeout(barq_sync_client_config_t*, uint64_t) BARQ_API_NOEXCEPT;
BARQ_API void barq_sync_client_config_set_connection_linger_time(barq_sync_client_config_t*,
                                                                 uint64_t) BARQ_API_NOEXCEPT;
BARQ_API void barq_sync_client_config_set_ping_keepalive_period(barq_sync_client_config_t*,
                                                                uint64_t) BARQ_API_NOEXCEPT;
BARQ_API void barq_sync_client_config_set_pong_keepalive_timeout(barq_sync_client_config_t*,
                                                                 uint64_t) BARQ_API_NOEXCEPT;
BARQ_API void barq_sync_client_config_set_fast_reconnect_limit(barq_sync_client_config_t*,
                                                               uint64_t) BARQ_API_NOEXCEPT;
BARQ_API void barq_sync_client_config_set_resumption_delay_interval(barq_sync_client_config_t*,
                                                                    uint64_t) BARQ_API_NOEXCEPT;
BARQ_API void barq_sync_client_config_set_max_resumption_delay_interval(barq_sync_client_config_t*,
                                                                        uint64_t) BARQ_API_NOEXCEPT;
BARQ_API void barq_sync_client_config_set_resumption_delay_backoff_multiplier(barq_sync_client_config_t*,
                                                                              int) BARQ_API_NOEXCEPT;
BARQ_API void barq_sync_client_config_set_sync_socket(barq_sync_client_config_t*,
                                                      barq_sync_socket_t*) BARQ_API_NOEXCEPT;
BARQ_API void barq_sync_client_config_set_default_binding_thread_observer(
    barq_sync_client_config_t* config, barq_on_object_store_thread_callback_t on_thread_create,
    barq_on_object_store_thread_callback_t on_thread_destroy, barq_on_object_store_error_callback_t on_error,
    barq_userdata_t user_data, barq_free_userdata_func_t free_userdata);

/**
 * Create a sync user identified by a tenant and a pre-supplied access token (a
 * signed JWT), for use with self-hosted Barq sync.
 * All three arguments must be non-empty. Set a route with
 * barq_sync_user_set_route() before creating a sync config from this user.
 *
 * This is the shared Barq implementation of the SyncUser interface; every client
 * SDK uses it rather than re-implementing the interface. Returns a new user, or
 * null on error (e.g. an empty argument), with the error set on the thread.
 */
BARQ_API barq_user_t* barq_sync_user_new_from_token(const char* tenant_id, const char* user_id,
                                                    const char* access_token) BARQ_API_NOEXCEPT;
/** Set the websocket route to the sync server. Only valid for a token user. */
BARQ_API void barq_sync_user_set_route(barq_user_t*, const char* route, bool verified) BARQ_API_NOEXCEPT;
/** Replace the user's access token (e.g. after refreshing it). Token user only. */
BARQ_API void barq_sync_user_set_access_token(barq_user_t*, const char* access_token) BARQ_API_NOEXCEPT;
/** Flag the access token as needing a refresh. Token user only. */
BARQ_API void barq_sync_user_mark_access_token_refresh_required(barq_user_t*) BARQ_API_NOEXCEPT;

/**
 * Validated sync-config builders shared with the C++ SDK. Both require a route to
 * have been set; the partition-based one requires a non-empty partition that does
 * not start with '/'. Return null on error (with the error set on the thread).
 * Prefer these over barq_sync_config_new()/barq_flx_sync_config_new(), which skip
 * the checks. Token user only.
 */
BARQ_API barq_sync_config_t* barq_sync_user_make_sync_config(barq_user_t*,
                                                             const char* partition) BARQ_API_NOEXCEPT;
BARQ_API barq_sync_config_t* barq_sync_user_make_flexible_sync_config(barq_user_t*) BARQ_API_NOEXCEPT;

BARQ_API barq_sync_config_t* barq_sync_config_new(const barq_user_t*, const char* partition_value) BARQ_API_NOEXCEPT;
BARQ_API barq_sync_config_t* barq_flx_sync_config_new(const barq_user_t*) BARQ_API_NOEXCEPT;
BARQ_API void barq_sync_config_set_session_stop_policy(barq_sync_config_t*,
                                                       barq_sync_session_stop_policy_e) BARQ_API_NOEXCEPT;
BARQ_API void barq_sync_config_set_error_handler(barq_sync_config_t*, barq_sync_error_handler_func_t,
                                                 barq_userdata_t userdata,
                                                 barq_free_userdata_func_t userdata_free) BARQ_API_NOEXCEPT;
/// DEPRECATED - Will be removed in a future release
BARQ_API void barq_sync_config_set_client_validate_ssl(barq_sync_config_t*, bool) BARQ_API_NOEXCEPT;
/// DEPRECATED - Will be removed in a future release
BARQ_API void barq_sync_config_set_ssl_trust_certificate_path(barq_sync_config_t*, const char*) BARQ_API_NOEXCEPT;
/// DEPRECATED - Will be removed in a future release
BARQ_API void barq_sync_config_set_ssl_verify_callback(barq_sync_config_t*, barq_sync_ssl_verify_func_t,
                                                       barq_userdata_t userdata,
                                                       barq_free_userdata_func_t userdata_free) BARQ_API_NOEXCEPT;
BARQ_API void barq_sync_config_set_cancel_waits_on_nonfatal_error(barq_sync_config_t*, bool) BARQ_API_NOEXCEPT;
/// DEPRECATED - Will be removed in a future release
BARQ_API void barq_sync_config_set_authorization_header_name(barq_sync_config_t*, const char*) BARQ_API_NOEXCEPT;
/// DEPRECATED - Will be removed in a future release
BARQ_API void barq_sync_config_set_custom_http_header(barq_sync_config_t*, const char* name,
                                                      const char* value) BARQ_API_NOEXCEPT;
BARQ_API void barq_sync_config_set_recovery_directory_path(barq_sync_config_t*, const char*) BARQ_API_NOEXCEPT;
BARQ_API void barq_sync_config_set_resync_mode(barq_sync_config_t*,
                                               barq_sync_session_resync_mode_e) BARQ_API_NOEXCEPT;
BARQ_API void
barq_sync_config_set_before_client_reset_handler(barq_sync_config_t*, barq_sync_before_client_reset_func_t,
                                                  barq_userdata_t userdata,
                                                  barq_free_userdata_func_t userdata_free) BARQ_API_NOEXCEPT;
BARQ_API void
barq_sync_config_set_after_client_reset_handler(barq_sync_config_t*, barq_sync_after_client_reset_func_t,
                                                 barq_userdata_t userdata,
                                                 barq_free_userdata_func_t userdata_free) BARQ_API_NOEXCEPT;
BARQ_API void barq_sync_config_set_initial_subscription_handler(barq_sync_config_t*,
                                                                barq_async_open_task_init_subscription_func_t,
                                                                bool rerun_on_open, barq_userdata_t userdata,
                                                                barq_free_userdata_func_t userdata_free);
/**
 * Fetch subscription id for the subscription passed as argument.
 * @return barq_object_id_t for the subscription passed as argument
 */
BARQ_API barq_object_id_t barq_sync_subscription_id(const barq_flx_sync_subscription_t* subscription)
    BARQ_API_NOEXCEPT;

/**
 * Fetch subscription name for the subscription passed as argument.
 * @return barq_string_t which contains the name of the subscription.
 */
BARQ_API barq_string_t barq_sync_subscription_name(const barq_flx_sync_subscription_t* subscription)
    BARQ_API_NOEXCEPT;

/**
 * Fetch object class name for the subscription passed as argument.
 * @return a barq_string_t which contains the class name of the subscription.
 */
BARQ_API barq_string_t barq_sync_subscription_object_class_name(const barq_flx_sync_subscription_t* subscription)
    BARQ_API_NOEXCEPT;

/**
 * Fetch the query string associated with the subscription passed as argument.
 * @return barq_string_t which contains the query associated with the subscription.
 */
BARQ_API barq_string_t barq_sync_subscription_query_string(const barq_flx_sync_subscription_t* subscription)
    BARQ_API_NOEXCEPT;

/**
 * Fetch the timestamp in which the subscription was created for the subscription passed as argument.
 * @return barq_timestamp_t representing the timestamp in which the subscription for created.
 */
BARQ_API barq_timestamp_t barq_sync_subscription_created_at(const barq_flx_sync_subscription_t* subscription)
    BARQ_API_NOEXCEPT;

/**
 * Fetch the timestamp in which the subscription was updated for the subscription passed as argument.
 * @return barq_timestamp_t representing the timestamp in which the subscription was updated.
 */
BARQ_API barq_timestamp_t barq_sync_subscription_updated_at(const barq_flx_sync_subscription_t* subscription)
    BARQ_API_NOEXCEPT;

/**
 * Get latest subscription set
 * @return a non null subscription set pointer if such it exists.
 */
BARQ_API barq_flx_sync_subscription_set_t* barq_sync_get_latest_subscription_set(const barq_t*);

/**
 * Get active subscription set
 * @return a non null subscription set pointer if such it exists.
 */
BARQ_API barq_flx_sync_subscription_set_t* barq_sync_get_active_subscription_set(const barq_t*);

/**
 * Wait until subscripton set state is equal to the state passed as parameter.
 * This is a blocking operation.
 * @return the current subscription state
 */
BARQ_API barq_flx_sync_subscription_set_state_e barq_sync_on_subscription_set_state_change_wait(
    const barq_flx_sync_subscription_set_t*, barq_flx_sync_subscription_set_state_e) BARQ_API_NOEXCEPT;

/**
 * Register a handler in order to be notified when subscription set is equal to the one passed as parameter
 * This is an asynchronous operation.
 * @return true/false if the handler was registered correctly
 */
BARQ_API bool barq_sync_on_subscription_set_state_change_async(
    const barq_flx_sync_subscription_set_t* subscription_set, barq_flx_sync_subscription_set_state_e notify_when,
    barq_sync_on_subscription_state_changed_t, barq_userdata_t userdata, barq_free_userdata_func_t userdata_free);

/**
 *  Retrieve version for the subscription set passed as parameter
 *  @return subscription set version if the poiter to the subscription is valid
 */
BARQ_API int64_t barq_sync_subscription_set_version(const barq_flx_sync_subscription_set_t*) BARQ_API_NOEXCEPT;

/**
 * Fetch current state for the subscription set passed as parameter
 *  @return the current state of the subscription_set
 */
BARQ_API barq_flx_sync_subscription_set_state_e
barq_sync_subscription_set_state(const barq_flx_sync_subscription_set_t*) BARQ_API_NOEXCEPT;

/**
 *  Query subscription set error string
 *  @return error string for the subscription passed as parameter
 */
BARQ_API const char* barq_sync_subscription_set_error_str(const barq_flx_sync_subscription_set_t*) BARQ_API_NOEXCEPT;

/**
 *  Retrieve the number of subscriptions for the subscription set passed as parameter
 *  @return the number of subscriptions
 */
BARQ_API size_t barq_sync_subscription_set_size(const barq_flx_sync_subscription_set_t*) BARQ_API_NOEXCEPT;

/**
 *  Access the subscription at index.
 *  @return the subscription or nullptr if the index is not valid
 */
BARQ_API barq_flx_sync_subscription_t* barq_sync_subscription_at(const barq_flx_sync_subscription_set_t*,
                                                                  size_t index);
/**
 *  Find subscription associated to the query passed as parameter
 *  @return a pointer to the subscription or nullptr if not found
 */
BARQ_API barq_flx_sync_subscription_t* barq_sync_find_subscription_by_query(const barq_flx_sync_subscription_set_t*,
                                                                             barq_query_t*) BARQ_API_NOEXCEPT;

/**
 *  Find subscription associated to the results set  passed as parameter
 *  @return a pointer to the subscription or nullptr if not found
 */
BARQ_API barq_flx_sync_subscription_t*
barq_sync_find_subscription_by_results(const barq_flx_sync_subscription_set_t*, barq_results_t*) BARQ_API_NOEXCEPT;


/**
 *  Find subscription by name passed as parameter
 *  @return a pointer to the subscription or nullptr if not found
 */
BARQ_API barq_flx_sync_subscription_t* barq_sync_find_subscription_by_name(const barq_flx_sync_subscription_set_t*,
                                                                            const char* name) BARQ_API_NOEXCEPT;

/**
 *  Refresh subscription
 *  @return true/false if the operation was successful or not
 */
BARQ_API bool barq_sync_subscription_set_refresh(barq_flx_sync_subscription_set_t*);

/**
 *  Convert a subscription into a mutable one in order to alter the subscription itself
 *  @return a pointer to a mutable subscription
 */
BARQ_API barq_flx_sync_mutable_subscription_set_t*
barq_sync_make_subscription_set_mutable(barq_flx_sync_subscription_set_t*);

/**
 *  Clear the subscription set passed as parameter
 *  @return true/false if operation was successful
 */
BARQ_API bool barq_sync_subscription_set_clear(barq_flx_sync_mutable_subscription_set_t*);

/**
 * Insert ot update the query contained inside a result object for the subscription set passed as parameter, if
 * successful the index where the query was inserted or updated is returned along with the info whether a new query
 * was inserted or not. It is possible to specify a name for the query inserted (optional).
 *  @return true/false if operation was successful
 */
BARQ_API bool barq_sync_subscription_set_insert_or_assign_results(barq_flx_sync_mutable_subscription_set_t*,
                                                                  barq_results_t*, const char* name,
                                                                  size_t* out_index, bool* out_inserted);
/**
 * Insert ot update a query for the subscription set passed as parameter, if successful the index where the query
 * was inserted or updated is returned along with the info whether a new query was inserted or not. It is possible to
 * specify a name for the query inserted (optional).
 *  @return true/false if operation was successful
 */
BARQ_API bool barq_sync_subscription_set_insert_or_assign_query(barq_flx_sync_mutable_subscription_set_t*,
                                                                barq_query_t*, const char* name, size_t* out_index,
                                                                bool* out_inserted);
/**
 *  Erase from subscription set by id. If operation completes successfully set the bool out param.
 *  @return true if no error occurred, false otherwise (use barq_get_last_error for fetching the error).
 */
BARQ_API bool barq_sync_subscription_set_erase_by_id(barq_flx_sync_mutable_subscription_set_t*,
                                                     const barq_object_id_t*, bool* erased);
/**
 *  Erase from subscription set by name. If operation completes successfully set the bool out param.
 *  @return true if no error occurred, false otherwise (use barq_get_last_error for fetching the error)
 */
BARQ_API bool barq_sync_subscription_set_erase_by_name(barq_flx_sync_mutable_subscription_set_t*, const char*,
                                                       bool* erased);
/**
 *  Erase from subscription set by query. If operation completes successfully set the bool out param.
 *  @return true if no error occurred, false otherwise (use barq_get_last_error for fetching the error)
 */
BARQ_API bool barq_sync_subscription_set_erase_by_query(barq_flx_sync_mutable_subscription_set_t*, barq_query_t*,
                                                        bool* erased);
/**
 *  Erase from subscription set by results. If operation completes successfully set the bool out param.
 *  @return true if no error occurred, false otherwise (use barq_get_last_error for fetching the error)
 */
BARQ_API bool barq_sync_subscription_set_erase_by_results(barq_flx_sync_mutable_subscription_set_t*,
                                                          barq_results_t*, bool* erased);
/**
 *  Remove all subscriptions for a given class type. If operation completes successfully set the bool out param.
 *  @return true if no error occurred, false otherwise (use barq_get_last_error for fetching the error).
 */
BARQ_API bool barq_sync_subscription_set_erase_by_class_name(barq_flx_sync_mutable_subscription_set_t*, const char*,
                                                             bool* erased);
/**
 *  Commit the subscription_set passed as parameter (in order that all the changes made will take effect)
 *  @return pointer to a valid immutable subscription if commit was successful
 */
BARQ_API barq_flx_sync_subscription_set_t*
barq_sync_subscription_set_commit(barq_flx_sync_mutable_subscription_set_t*);

/**
 * Create a task that will open a barq with the specific configuration
 * and also download all changes from the sync server.
 *
 * Use @a barq_async_open_task_start() to start the download process.
 */
BARQ_API barq_async_open_task_t* barq_open_synchronized(barq_config_t*) BARQ_API_NOEXCEPT;
BARQ_API void barq_async_open_task_start(barq_async_open_task_t*, barq_async_open_task_completion_func_t,
                                         barq_userdata_t userdata,
                                         barq_free_userdata_func_t userdata_free) BARQ_API_NOEXCEPT;
BARQ_API void barq_async_open_task_cancel(barq_async_open_task_t*) BARQ_API_NOEXCEPT;
BARQ_API barq_async_open_task_progress_notification_token_t*
barq_async_open_task_register_download_progress_notifier(barq_async_open_task_t*, barq_sync_progress_func_t,
                                                          barq_userdata_t userdata,
                                                          barq_free_userdata_func_t userdata_free) BARQ_API_NOEXCEPT;
/**
 * Get the sync session for a specific barq.
 *
 * This function will not fail if the barq wasn't open with a sync configuration in place,
 * but just return NULL;
 *
 * @return A non-null pointer if a session exists.
 */
BARQ_API barq_sync_session_t* barq_sync_session_get(const barq_t*) BARQ_API_NOEXCEPT;

/**
 * Fetch state for the session passed as parameter
 * @param session ptr to the sync session to retrieve the state for
 * @return barq_sync_session_state_e value
 */
BARQ_API barq_sync_session_state_e barq_sync_session_get_state(const barq_sync_session_t* session) BARQ_API_NOEXCEPT;

/**
 * Fetch connection state for the session passed as parameter
 * @param session ptr to the sync session to retrieve the state for
 * @return barq_sync_connection_state_e value
 */
BARQ_API barq_sync_connection_state_e barq_sync_session_get_connection_state(const barq_sync_session_t* session)
    BARQ_API_NOEXCEPT;

/**
 * Fetch user for the session passed as parameter
 * @param session ptr to the sync session to retrieve the user for
 * @return ptr to barq_user_t
 */
BARQ_API barq_user_t* barq_sync_session_get_user(const barq_sync_session_t* session) BARQ_API_NOEXCEPT;

/**
 * Fetch partition value for the session passed as parameter
 * @param session ptr to the sync session to retrieve the partition value for
 * @return a string containing the partition value
 */
BARQ_API const char* barq_sync_session_get_partition_value(const barq_sync_session_t* session) BARQ_API_NOEXCEPT;

/**
 * Get the filesystem path of the barq file backing this session.
 */
BARQ_API const char* barq_sync_session_get_file_path(const barq_sync_session_t*) BARQ_API_NOEXCEPT;

/**
 * Ask the session to pause synchronization.
 *
 * No-op if the session is already inactive.
 */
BARQ_API void barq_sync_session_pause(barq_sync_session_t*) BARQ_API_NOEXCEPT;

/**
 * Ask the session to resume synchronization.
 *
 * No-op if the session is already active.
 */
BARQ_API void barq_sync_session_resume(barq_sync_session_t*) BARQ_API_NOEXCEPT;

/**
 * Gets the file ident/salt currently assigned to the barq by sync. Callers should supply a pointer token
 * a barq_salted_file_ident_t for this function to fill out.
 */
BARQ_API void barq_sync_session_get_file_ident(barq_sync_session_t*,
                                               barq_salted_file_ident_t* out) BARQ_API_NOEXCEPT;


/**
 * Register a callback that will be invoked every time the session's connection state changes.
 *
 * @return a notification token object. Dispose it to stop receiving notifications.
 */
BARQ_API barq_sync_session_connection_state_notification_token_t*
barq_sync_session_register_connection_state_change_callback(
    barq_sync_session_t*, barq_sync_connection_state_changed_func_t, barq_userdata_t userdata,
    barq_free_userdata_func_t userdata_free) BARQ_API_NOEXCEPT;

/**
 * Register a callback that will be invoked every time the session reports progress.
 *
 * @param is_streaming If true, then the notifier will be called forever, and will
 *                     always contain the most up-to-date number of downloadable or uploadable bytes.
 *                     Otherwise, the number of downloaded or uploaded bytes will always be reported
 *                     relative to the number of downloadable or uploadable bytes at the point in time
 *                     when the notifier was registered.
 * @return a notification token object. Dispose it to stop receiving notifications.
 */
BARQ_API barq_sync_session_connection_state_notification_token_t* barq_sync_session_register_progress_notifier(
    barq_sync_session_t*, barq_sync_progress_func_t, barq_sync_progress_direction_e, bool is_streaming,
    barq_userdata_t userdata, barq_free_userdata_func_t userdata_free) BARQ_API_NOEXCEPT;


/**
 * Register a callback that will be invoked when all pending downloads have completed.
 */
BARQ_API void
barq_sync_session_wait_for_download_completion(barq_sync_session_t*, barq_sync_wait_for_completion_func_t,
                                                barq_userdata_t userdata,
                                                barq_free_userdata_func_t userdata_free) BARQ_API_NOEXCEPT;

/**
 * Register a callback that will be invoked when all pending uploads have completed.
 */
BARQ_API void barq_sync_session_wait_for_upload_completion(barq_sync_session_t*,
                                                           barq_sync_wait_for_completion_func_t,
                                                           barq_userdata_t userdata,
                                                           barq_free_userdata_func_t userdata_free) BARQ_API_NOEXCEPT;

/**
 * Wrapper for SyncSession::OnlyForTesting::handle_error. This routine should be used only for testing.
 * @param session ptr to a valid sync session
 * @param error_code barq_errno_e representing the error to simulate
 * @param error_str error message to be included with Status
 * @param is_fatal boolean to signal if the error is fatal or not
 */
BARQ_API void barq_sync_session_handle_error_for_testing(const barq_sync_session_t* session,
                                                         barq_errno_e error_code, const char* error_str,
                                                         bool is_fatal);

/**
 * In case of exception thrown in user code callbacks, this api will allow the sdk to store the user code exception
 * and retrieve a it later via barq_get_last_error.
 * Most importantly the SDK is responsible to handle the memory pointed by user_code_error.
 * @param usercode_error pointer representing whatever object the SDK treats as exception/error.
 */
BARQ_API void barq_register_user_code_callback_error(barq_userdata_t usercode_error) BARQ_API_NOEXCEPT;


/**
 * Creates a new sync socket instance for the Sync Client that handles the operations for a custom
 * websocket and event loop implementation.
 * @param userdata CAPI implementation specific pointer containing custom context data that is provided to
 *                 each of the provided functions.
 * @param userdata_free function that will be called when the sync socket is destroyed to delete userdata. This
 *                      is required if userdata is not null.
 * @param post_func function that will be called to post a callback handler onto the event loop - use the
 *                  barq_sync_socket_post_complete() function when the callback handler is scheduled to run.
 * @param create_timer_func function that will be called to create a new timer resource with the callback
 *                          handler that will be run when the timer expires or an erorr occurs - use the
 *                          barq_sync_socket_timer_canceled() function if the timer is canceled or the
 *                          barq_sync_socket_timer_complete() function if the timer expires or an error occurs.
 * @param cancel_timer_func function that will be called when the timer has been canceled by the sync client.
 * @param free_timer_func function that will be called when the timer resource has been destroyed by the sync client.
 * @param websocket_connect_func function that will be called when the sync client creates a websocket.
 * @param websocket_write_func function that will be called when the sync client sends data over the websocket.
 * @param websocket_free_func function that will be called when the sync client closes the websocket conneciton.
 * @return a barq_sync_socket_t pointer suitable for passing to barq_sync_client_config_set_sync_socket()
 */
BARQ_API barq_sync_socket_t* barq_sync_socket_new(
    barq_userdata_t userdata, barq_free_userdata_func_t userdata_free, barq_sync_socket_post_func_t post_func,
    barq_sync_socket_create_timer_func_t create_timer_func,
    barq_sync_socket_timer_canceled_func_t cancel_timer_func, barq_sync_socket_timer_free_func_t free_timer_func,
    barq_sync_socket_connect_func_t websocket_connect_func,
    barq_sync_socket_websocket_async_write_func_t websocket_write_func,
    barq_sync_socket_websocket_free_func_t websocket_free_func);

/**
 * To be called to execute the callback handler provided to the create_timer_func when the timer is
 * complete or an error occurs while processing the timer.
 * @param timer_handler the timer callback handler that was provided when the timer was created.
 * @param result the error code for the error that occurred or BARQ_ERR_SYNC_SOCKET_SUCCESS if the timer
 *               expired normally.
 * @param reason a string describing details about the error that occurred or empty string if no error.
 * NOTE: This function must be called by the event loop execution thread.
 */
BARQ_API void barq_sync_socket_timer_complete(barq_sync_socket_timer_callback_t* timer_handler,
                                              barq_sync_socket_callback_result_e result, const char* reason);

/**
 * To be called to execute the callback handler provided to the create_timer_func when the timer has been
 * canceled.
 * @param timer_handler the timer callback handler that was provided when the timer was created.
 * NOTE: This function must be called by the event loop execution thread.
 */
BARQ_API void barq_sync_socket_timer_canceled(barq_sync_socket_timer_callback_t* timer_handler);

/**
 * To be called to execute the callback function provided to the post_func when the event loop executes
 * that post'ed operation. The post_handler resource will automatically be destroyed during this
 * operation.
 * @param post_handler the post callback handler that was originally provided to the post_func
 * @param result the error code for the error that occurred or BARQ_ERR_SYNC_SOCKET_SUCCESS if the
 *               callback handler should be executed normally.
 * @param reason a string describing details about the error that occurred or empty string if no error.
 * NOTE: This function must be called by the event loop execution thread.
 */
BARQ_API void barq_sync_socket_post_complete(barq_sync_socket_post_callback_t* post_handler,
                                             barq_sync_socket_callback_result_e result, const char* reason);

/**
 * To be called to execute the callback function provided to the websocket_write_func when the write
 * operation is complete. The write_handler resource will automatically be destroyed during this
 * operation.
 * @param write_handler the write callback handler that was originally provided to the websocket_write_func
 * @param result the error code for the error that occurred or BARQ_ERR_SYNC_SOCKET_SUCCESS if write completed
 *               successfully
 * @param reason a string describing details about the error that occurred or empty string if no error.
 * NOTE: This function must be called by the event loop execution thread.
 */
BARQ_API void barq_sync_socket_write_complete(barq_sync_socket_write_callback_t* write_handler,
                                              barq_sync_socket_callback_result_e result, const char* reason);

/**
 * To be called when the websocket successfully connects to the server.
 * @param barq_websocket_observer the websocket observer object that was provided to the websocket_connect_func
 * @param protocol the value of the Sec-WebSocket-Protocol header in the connect response from the server.
 * NOTE: This function must be called by the event loop execution thread and should not be called
 *       after the websocket_free_func has been called to release the websocket resources.
 */
BARQ_API void barq_sync_socket_websocket_connected(barq_websocket_observer_t* barq_websocket_observer,
                                                   const char* protocol);

/**
 * To be called when an error occurs - the actual error value will be provided when the websocket_closed
 * function is called. This function informs that the socket object is in an error state and no further
 * TX operations should be performed.
 * @param barq_websocket_observer the websocket observer object that was provided to the websocket_connect_func
 * NOTE: This function must be called by the event loop execution thread and should not be called
 *       after the websocket_free_func has been called to release the websocket resources.
 */
BARQ_API void barq_sync_socket_websocket_error(barq_websocket_observer_t* barq_websocket_observer);

/**
 * To be called to provide the received data to the Sync Client when a write operation has completed.
 * The data buffer can be safely discarded after this function has completed.
 * @param barq_websocket_observer the websocket observer object that was provided to the websocket_connect_func
 * @param data a pointer to the buffer that contains the data received over the websocket
 * @param data_size the number of bytes in the data buffer
 * @return bool designates whether the WebSocket object should continue processing messages. The normal return
 *         value is true. False must be returned if the websocket object has been destroyed during execution of
 *         the function.
 * NOTE: This function must be called by the event loop execution thread and should not be called
 *       after the websocket_free_func has been called to release the websocket resources.
 */
BARQ_API bool barq_sync_socket_websocket_message(barq_websocket_observer_t* barq_websocket_observer,
                                                 const char* data, size_t data_size);

/**
 * To be called when the websocket has been closed, either due to an error or a normal close operation.
 * @param barq_websocket_observer the websocket observer object that was provided to the websocket_connect_func
 * @param was_clean boolean value that indicates whether this is a normal close situation (true), the
 *                  close code was provided by the server via a close message (true), or if the close code was
 *                  generated by the local websocket as a result of some other error (false) (e.g. host
 *                  unreachable, etc.)
 * @param code the websocket close code (per the WebSocket spec) that describes why the websocket was closed.
 * @param reason a string describing details about the error that occurred or empty string if no error.
 * @return bool designates whether the WebSocket object has been destroyed during the execution of this
 *         function. The normal return value is True to indicate the WebSocket object is no longer valid. If
 *         False is returned, the WebSocket object will be destroyed at some point in the future.
 * NOTE: This function must be called by the event loop execution thread and should not be called
 *       after the websocket_free_func has been called to release the websocket resources.
 */
BARQ_API bool barq_sync_socket_websocket_closed(barq_websocket_observer_t* barq_websocket_observer, bool was_clean,
                                                barq_web_socket_errno_e code, const char* reason);

#endif // BARQ_H
