/*************************************************************************
 *
 * Copyright 2021 Realm Inc.
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

#pragma once

#include <cstdint>
#include <type_traits>
#include <string>
#include <vector>
#include <barq/error_codes.h>

namespace barq {

// ErrorExtraInfo subclasses:

struct ErrorCategory {
    enum Type {
        logic_error = BARQ_ERR_CAT_LOGIC,
        runtime_error = BARQ_ERR_CAT_RUNTIME,
        invalid_argument = BARQ_ERR_CAT_INVALID_ARG,
        file_access = BARQ_ERR_CAT_FILE_ACCESS,
        system_error = BARQ_ERR_CAT_SYSTEM_ERROR,
        app_error = BARQ_ERR_CAT_APP_ERROR,
        client_error = BARQ_ERR_CAT_CLIENT_ERROR,
        json_error = BARQ_ERR_CAT_JSON_ERROR,
        service_error = BARQ_ERR_CAT_SERVICE_ERROR,
        http_error = BARQ_ERR_CAT_HTTP_ERROR,
        custom_error = BARQ_ERR_CAT_CUSTOM_ERROR,
        websocket_error = BARQ_ERR_CAT_WEBSOCKET_ERROR,
        sync_error = BARQ_ERR_CAT_SYNC_ERROR,
    };
    constexpr ErrorCategory() = default;
    constexpr bool test(Type cat)
    {
        return (m_value & cat) != 0;
    }
    constexpr ErrorCategory& set(Type cat)
    {
        m_value |= cat;
        return *this;
    }
    constexpr void reset(Type cat)
    {
        m_value &= ~cat;
    }
    constexpr bool operator==(const ErrorCategory& other) const
    {
        return m_value == other.m_value;
    }
    constexpr bool operator!=(const ErrorCategory& other) const
    {
        return m_value != other.m_value;
    }
    constexpr int value() const
    {
        return m_value;
    }

private:
    unsigned m_value = 0;
};

class ErrorCodes {
public:
    // Explicitly 32-bits wide so that non-symbolic values,
    // like uassert codes, are valid.
    enum Error : std::int32_t {
        OK = BARQ_ERR_NONE,
        RuntimeError = BARQ_ERR_RUNTIME,
        RangeError = BARQ_ERR_RANGE_ERROR,
        BrokenInvariant = BARQ_ERR_BROKEN_INVARIANT,
        OutOfMemory = BARQ_ERR_OUT_OF_MEMORY,
        OutOfDiskSpace = BARQ_ERR_OUT_OF_DISK_SPACE,
        AddressSpaceExhausted = BARQ_ERR_ADDRESS_SPACE_EXHAUSTED,
        MaximumFileSizeExceeded = BARQ_ERR_MAXIMUM_FILE_SIZE_EXCEEDED,
        IncompatibleSession = BARQ_ERR_INCOMPATIBLE_SESSION,
        IncompatibleLockFile = BARQ_ERR_INCOMPATIBLE_LOCK_FILE,
        UnsupportedFileFormatVersion = BARQ_ERR_UNSUPPORTED_FILE_FORMAT_VERSION,
        MultipleSyncAgents = BARQ_ERR_MULTIPLE_SYNC_AGENTS,
        ObjectAlreadyExists = BARQ_ERR_OBJECT_ALREADY_EXISTS,
        NotCloneable = BARQ_ERR_NOT_CLONABLE,
        BadChangeset = BARQ_ERR_BAD_CHANGESET,
        SubscriptionFailed = BARQ_ERR_SUBSCRIPTION_FAILED,
        FileOperationFailed = BARQ_ERR_FILE_OPERATION_FAILED,
        PermissionDenied = BARQ_ERR_FILE_PERMISSION_DENIED,
        FileNotFound = BARQ_ERR_FILE_NOT_FOUND,
        FileAlreadyExists = BARQ_ERR_FILE_ALREADY_EXISTS,
        InvalidDatabase = BARQ_ERR_INVALID_DATABASE,
        DecryptionFailed = BARQ_ERR_DECRYPTION_FAILED,
        IncompatibleHistories = BARQ_ERR_INCOMPATIBLE_HISTORIES,
        FileFormatUpgradeRequired = BARQ_ERR_FILE_FORMAT_UPGRADE_REQUIRED,
        SchemaVersionMismatch = BARQ_ERR_SCHEMA_VERSION_MISMATCH,
        NoSubscriptionForWrite = BARQ_ERR_NO_SUBSCRIPTION_FOR_WRITE,
        BadVersion = BARQ_ERR_BAD_VERSION,
        OperationAborted = BARQ_ERR_OPERATION_ABORTED,

        AutoClientResetFailed = BARQ_ERR_AUTO_CLIENT_RESET_FAILED,
        BadSyncPartitionValue = BARQ_ERR_BAD_SYNC_PARTITION_VALUE,
        ConnectionClosed = BARQ_ERR_CONNECTION_CLOSED,
        InvalidSubscriptionQuery = BARQ_ERR_INVALID_SUBSCRIPTION_QUERY,
        SyncClientResetRequired = BARQ_ERR_SYNC_CLIENT_RESET_REQUIRED,
        SyncCompensatingWrite = BARQ_ERR_SYNC_COMPENSATING_WRITE,
        SyncConnectFailed = BARQ_ERR_SYNC_CONNECT_FAILED,
        SyncConnectTimeout = BARQ_ERR_SYNC_CONNECT_TIMEOUT,
        SyncInvalidSchemaChange = BARQ_ERR_SYNC_INVALID_SCHEMA_CHANGE,
        SyncPermissionDenied = BARQ_ERR_SYNC_PERMISSION_DENIED,
        SyncProtocolInvariantFailed = BARQ_ERR_SYNC_PROTOCOL_INVARIANT_FAILED,
        SyncProtocolNegotiationFailed = BARQ_ERR_SYNC_PROTOCOL_NEGOTIATION_FAILED,
        SyncServerPermissionsChanged = BARQ_ERR_SYNC_SERVER_PERMISSIONS_CHANGED,
        SyncUserMismatch = BARQ_ERR_SYNC_USER_MISMATCH,
        TlsHandshakeFailed = BARQ_ERR_TLS_HANDSHAKE_FAILED,
        WrongSyncType = BARQ_ERR_WRONG_SYNC_TYPE,
        SyncWriteNotAllowed = BARQ_ERR_SYNC_WRITE_NOT_ALLOWED,
        SyncLocalClockBeforeEpoch = BARQ_ERR_SYNC_LOCAL_CLOCK_BEFORE_EPOCH,
        SyncSchemaMigrationError = BARQ_ERR_SYNC_SCHEMA_MIGRATION_ERROR,

        SystemError = BARQ_ERR_SYSTEM_ERROR,

        LogicError = BARQ_ERR_LOGIC,
        NotSupported = BARQ_ERR_NOT_SUPPORTED,
        BrokenPromise = BARQ_ERR_BROKEN_PROMISE,
        CrossTableLinkTarget = BARQ_ERR_CROSS_TABLE_LINK_TARGET,
        KeyAlreadyUsed = BARQ_ERR_KEY_ALREADY_USED,
        WrongTransactionState = BARQ_ERR_WRONG_TRANSACTION_STATE,
        WrongThread = BARQ_ERR_WRONG_THREAD,
        IllegalOperation = BARQ_ERR_ILLEGAL_OPERATION,
        SerializationError = BARQ_ERR_SERIALIZATION_ERROR,
        StaleAccessor = BARQ_ERR_STALE_ACCESSOR,
        InvalidatedObject = BARQ_ERR_INVALIDATED_OBJECT,
        ReadOnlyDB = BARQ_ERR_READ_ONLY_DB,
        DeleteOnOpenBarq = BARQ_ERR_DELETE_OPENED_BARQ,
        MismatchedConfig = BARQ_ERR_MISMATCHED_CONFIG,
        ClosedBarq = BARQ_ERR_CLOSED_BARQ,
        InvalidTableRef = BARQ_ERR_INVALID_TABLE_REF,
        SchemaValidationFailed = BARQ_ERR_SCHEMA_VALIDATION_FAILED,
        SchemaMismatch = BARQ_ERR_SCHEMA_MISMATCH,
        InvalidSchemaVersion = BARQ_ERR_INVALID_SCHEMA_VERSION,
        InvalidSchemaChange = BARQ_ERR_INVALID_SCHEMA_CHANGE,
        MigrationFailed = BARQ_ERR_MIGRATION_FAILED,
        InvalidQuery = BARQ_ERR_INVALID_QUERY,

        BadServerUrl = BARQ_ERR_BAD_SERVER_URL,
        InvalidArgument = BARQ_ERR_INVALID_ARGUMENT,
        TypeMismatch = BARQ_ERR_PROPERTY_TYPE_MISMATCH,
        PropertyNotNullable = BARQ_ERR_PROPERTY_NOT_NULLABLE,
        ReadOnlyProperty = BARQ_ERR_READ_ONLY_PROPERTY,
        MissingPropertyValue = BARQ_ERR_MISSING_PROPERTY_VALUE,
        MissingPrimaryKey = BARQ_ERR_MISSING_PRIMARY_KEY,
        UnexpectedPrimaryKey = BARQ_ERR_UNEXPECTED_PRIMARY_KEY,
        ModifyPrimaryKey = BARQ_ERR_MODIFY_PRIMARY_KEY,
        SyntaxError = BARQ_ERR_INVALID_QUERY_STRING,
        InvalidProperty = BARQ_ERR_INVALID_PROPERTY,
        InvalidName = BARQ_ERR_INVALID_NAME,
        InvalidDictionaryKey = BARQ_ERR_INVALID_DICTIONARY_KEY,
        InvalidDictionaryValue = BARQ_ERR_INVALID_DICTIONARY_VALUE,
        InvalidSortDescriptor = BARQ_ERR_INVALID_SORT_DESCRIPTOR,
        InvalidEncryptionKey = BARQ_ERR_INVALID_ENCRYPTION_KEY,
        InvalidQueryArg = BARQ_ERR_INVALID_QUERY_ARG,
        KeyNotFound = BARQ_ERR_NO_SUCH_OBJECT,
        OutOfBounds = BARQ_ERR_INDEX_OUT_OF_BOUNDS,
        LimitExceeded = BARQ_ERR_LIMIT_EXCEEDED,
        ObjectTypeMismatch = BARQ_ERR_OBJECT_TYPE_MISMATCH,
        NoSuchTable = BARQ_ERR_NO_SUCH_TABLE,
        TableNameInUse = BARQ_ERR_TABLE_NAME_IN_USE,
        IllegalCombination = BARQ_ERR_ILLEGAL_COMBINATION,
        TopLevelObject = BARQ_ERR_TOP_LEVEL_OBJECT,

        CustomError = BARQ_ERR_CUSTOM_ERROR,

        ClientUserNotFound = BARQ_ERR_CLIENT_USER_NOT_FOUND,
        ClientUserNotLoggedIn = BARQ_ERR_CLIENT_USER_NOT_LOGGED_IN,
        ClientUserAlreadyNamed = BARQ_ERR_CLIENT_USER_ALREADY_NAMED,
        ClientRedirectError = BARQ_ERR_CLIENT_REDIRECT_ERROR,
        ClientTooManyRedirects = BARQ_ERR_CLIENT_TOO_MANY_REDIRECTS,

        BadToken = BARQ_ERR_BAD_TOKEN,
        MalformedJson = BARQ_ERR_MALFORMED_JSON,
        MissingJsonKey = BARQ_ERR_MISSING_JSON_KEY,
        BadJsonParse = BARQ_ERR_BAD_JSON_PARSE,

        MissingAuthReq = BARQ_ERR_MISSING_AUTH_REQ,
        InvalidSession = BARQ_ERR_INVALID_SESSION,
        UserAppDomainMismatch = BARQ_ERR_USER_APP_DOMAIN_MISMATCH,
        DomainNotAllowed = BARQ_ERR_DOMAIN_NOT_ALLOWED,
        ReadSizeLimitExceeded = BARQ_ERR_READ_SIZE_LIMIT_EXCEEDED,
        InvalidParameter = BARQ_ERR_INVALID_PARAMETER,
        MissingParameter = BARQ_ERR_MISSING_PARAMETER,
        TwilioError = BARQ_ERR_TWILIO_ERROR,
        GCMError = BARQ_ERR_GCM_ERROR,
        HTTPError = BARQ_ERR_HTTP_ERROR,
        AWSError = BARQ_ERR_AWS_ERROR,
        ServiceError = BARQ_ERR_SERVICE_ERROR,
        ArgumentsNotAllowed = BARQ_ERR_ARGUMENTS_NOT_ALLOWED,
        FunctionExecutionError = BARQ_ERR_FUNCTION_EXECUTION_ERROR,
        NoMatchingRuleFound = BARQ_ERR_NO_MATCHING_RULE_FOUND,
        InternalServerError = BARQ_ERR_INTERNAL_SERVER_ERROR,
        AuthProviderNotFound = BARQ_ERR_AUTH_PROVIDER_NOT_FOUND,
        AuthProviderAlreadyExists = BARQ_ERR_AUTH_PROVIDER_ALREADY_EXISTS,
        ServiceNotFound = BARQ_ERR_SERVICE_NOT_FOUND,
        ServiceTypeNotFound = BARQ_ERR_SERVICE_TYPE_NOT_FOUND,
        ServiceAlreadyExists = BARQ_ERR_SERVICE_ALREADY_EXISTS,
        ServiceCommandNotFound = BARQ_ERR_SERVICE_COMMAND_NOT_FOUND,
        ValueNotFound = BARQ_ERR_VALUE_NOT_FOUND,
        ValueAlreadyExists = BARQ_ERR_VALUE_ALREADY_EXISTS,
        ValueDuplicateName = BARQ_ERR_VALUE_DUPLICATE_NAME,
        FunctionNotFound = BARQ_ERR_FUNCTION_NOT_FOUND,
        FunctionAlreadyExists = BARQ_ERR_FUNCTION_ALREADY_EXISTS,
        FunctionDuplicateName = BARQ_ERR_FUNCTION_DUPLICATE_NAME,
        FunctionSyntaxError = BARQ_ERR_FUNCTION_SYNTAX_ERROR,
        FunctionInvalid = BARQ_ERR_FUNCTION_INVALID,
        IncomingWebhookNotFound = BARQ_ERR_INCOMING_WEBHOOK_NOT_FOUND,
        IncomingWebhookAlreadyExists = BARQ_ERR_INCOMING_WEBHOOK_ALREADY_EXISTS,
        IncomingWebhookDuplicateName = BARQ_ERR_INCOMING_WEBHOOK_DUPLICATE_NAME,
        RuleNotFound = BARQ_ERR_RULE_NOT_FOUND,
        APIKeyNotFound = BARQ_ERR_API_KEY_NOT_FOUND,
        RuleAlreadyExists = BARQ_ERR_RULE_ALREADY_EXISTS,
        RuleDuplicateName = BARQ_ERR_RULE_DUPLICATE_NAME,
        AuthProviderDuplicateName = BARQ_ERR_AUTH_PROVIDER_DUPLICATE_NAME,
        RestrictedHost = BARQ_ERR_RESTRICTED_HOST,
        APIKeyAlreadyExists = BARQ_ERR_API_KEY_ALREADY_EXISTS,
        IncomingWebhookAuthFailed = BARQ_ERR_INCOMING_WEBHOOK_AUTH_FAILED,
        ExecutionTimeLimitExceeded = BARQ_ERR_EXECUTION_TIME_LIMIT_EXCEEDED,
        NotCallable = BARQ_ERR_NOT_CALLABLE,
        UserAlreadyConfirmed = BARQ_ERR_USER_ALREADY_CONFIRMED,
        UserNotFound = BARQ_ERR_USER_NOT_FOUND,
        UserDisabled = BARQ_ERR_USER_DISABLED,
        AuthError = BARQ_ERR_AUTH_ERROR,
        BadRequest = BARQ_ERR_BAD_REQUEST,
        AccountNameInUse = BARQ_ERR_ACCOUNT_NAME_IN_USE,
        InvalidPassword = BARQ_ERR_INVALID_PASSWORD,
        SchemaValidationFailedWrite = BARQ_ERR_SCHEMA_VALIDATION_FAILED_WRITE,
        AppUnknownError = BARQ_ERR_APP_UNKNOWN,
        MaintenanceInProgress = BARQ_ERR_MAINTENANCE_IN_PROGRESS,
        UserpassTokenInvalid = BARQ_ERR_USERPASS_TOKEN_INVALID,
        InvalidServerResponse = BARQ_ERR_INVALID_SERVER_RESPONSE,
        AppServerError = BARQ_ERR_APP_SERVER_ERROR,

        CallbackFailed = BARQ_ERR_CALLBACK,
        UnknownError = BARQ_ERR_UNKNOWN,
    };

    static ErrorCategory error_categories(Error code);
    static std::string_view error_string(Error code);
    static Error from_string(std::string_view str);
    static std::vector<Error> get_all_codes();
    static std::vector<std::string_view> get_all_names();
    static std::vector<std::pair<std::string_view, ErrorCodes::Error>> get_error_list();
};

std::ostream& operator<<(std::ostream& stream, ErrorCodes::Error code);

} // namespace barq
