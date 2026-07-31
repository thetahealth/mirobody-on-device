find_package(Catch2 3 REQUIRED)
include(Catch)

add_executable(mirobody_tests
    cache/cache_test.cpp
    jwt/google_test.cpp
    jwt/jwt_test.cpp
    utils/fernet_test.cpp
    utils/config_test.cpp
    llm/sse_parser_test.cpp
    llm/think_splitter_test.cpp
    llm/mirothinker_client_test.cpp
    llm/openai_chat_client_test.cpp
    llm/openai_responses_client_test.cpp
    llm/gemini_client_test.cpp
    llm/gemini_live_client_test.cpp
    llm/openai_realtime_client_test.cpp
    mcp/tool_test.cpp
    mcp/resource_test.cpp
    memory/memory_test.cpp
    transcode/file_test.cpp
    oauth/pkce_test.cpp
    chat/agent_test.cpp
    chat/event_test.cpp
    chat/history_test.cpp
    chat/persist_test.cpp
    circle/access_test.cpp
    storage/sign_test.cpp
    storage/storage_test.cpp
    transcode/image_test.cpp
    transcode/document_test.cpp
    fhir/units_test.cpp
    fhir/resource_test.cpp
    indicator/normalize_test.cpp
    indicator/resolve_test.cpp
)

# Link the self-registering MCP tool and agent objects directly into the test
# binary, the same way the final app targets do (see the root CMakeLists.txt).
# This is what lets the registration tests observe echo/whoami and BaselineAgent:
# were they only in the mirobody_core archive, the linker would drop them here.
target_sources(mirobody_tests PRIVATE
    $<TARGET_OBJECTS:mcp_tools>
    $<TARGET_OBJECTS:mcp_resources>
    $<TARGET_OBJECTS:agent_impls>)

target_link_libraries(mirobody_tests PRIVATE
    mirobody_core
    Catch2::Catch2WithMain
)

catch_discover_tests(mirobody_tests)
