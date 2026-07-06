
add_executable(InspectorInspectClientBarq "inspect_client_barq.cpp")
set_target_properties(InspectorInspectClientBarq PROPERTIES
                      OUTPUT_NAME "barq-inspect-client-barq"
                      DEBUG_POSTFIX "${CMAKE_DEBUG_POSTFIX}")
target_link_libraries(InspectorInspectClientBarq Sync Storage)

add_executable(InspectorPrintChangeset "print_changeset.cpp")
set_target_properties(InspectorPrintChangeset PROPERTIES
                      OUTPUT_NAME "barq-print-changeset"
                      DEBUG_POSTFIX "${CMAKE_DEBUG_POSTFIX}")
target_link_libraries(InspectorPrintChangeset Sync Storage)

add_executable(ApplyToStateCommand apply_to_state_command.cpp)
set_target_properties(ApplyToStateCommand PROPERTIES
                      OUTPUT_NAME "barq-apply-to-state"
                      DEBUG_POSTFIX "${CMAKE_DEBUG_POSTFIX}")
target_link_libraries(ApplyToStateCommand Sync Storage)

add_executable(HistCommand hist_command.cpp)
set_target_properties(HistCommand PROPERTIES
                      OUTPUT_NAME "barq-hist"
                      DEBUG_POSTFIX "${CMAKE_DEBUG_POSTFIX}")
target_link_libraries(HistCommand Sync Storage)

add_executable(BarqSyncE2E barq_sync_e2e.cpp)
set_target_properties(BarqSyncE2E PROPERTIES
                      OUTPUT_NAME "barq-sync-e2e"
                      DEBUG_POSTFIX "${CMAKE_DEBUG_POSTFIX}")
target_link_libraries(BarqSyncE2E Sync Storage)

add_executable(BarqEncryptionTest barq_encryption_test.cpp)
set_target_properties(BarqEncryptionTest PROPERTIES
                      OUTPUT_NAME "barq-encryption-test"
                      DEBUG_POSTFIX "${CMAKE_DEBUG_POSTFIX}")
target_link_libraries(BarqEncryptionTest Storage)

install(TARGETS
    HistCommand
    ApplyToStateCommand
    InspectorInspectClientBarq
    InspectorPrintChangeset
    COMPONENT runtime
    DESTINATION bin
)
