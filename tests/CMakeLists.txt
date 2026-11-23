include(FetchContent)

set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

# Every test binary gets a hard timeout so a deadlock fails the run instead of
# hanging it.
function(add_search_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE search_core GTest::gtest_main)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  # Discovery runs at test time: a ThreadSanitizer binary cannot be executed
  # during the build unless ASLR is disabled, and `setarch -R` wraps ctest.
  gtest_discover_tests(${name} PROPERTIES TIMEOUT 120 DISCOVERY_MODE PRE_TEST)
endfunction()

add_search_test(api_test)
add_search_test(health_test)
add_search_test(index_service_test)
add_search_test(inverted_index_test)
add_search_test(ollama_client_test)
add_search_test(search_engine_test)
add_search_test(search_pipeline_test)
add_search_test(test_support_test)
add_search_test(thread_pool_test)
add_search_test(tokenizer_test)
add_search_test(web_ui_test)
add_search_test(ws_route_test)
add_search_test(ws_search_test)

# Behavioural JavaScript tests for the UI state machine. They use only Node's
# built-in test runner, so no npm install is needed.
find_program(NODE_EXECUTABLE NAMES node nodejs)
if(NODE_EXECUTABLE)
  add_test(
    NAME ui_search_client
    COMMAND ${NODE_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/ui/search_client.test.js)
  set_tests_properties(ui_search_client PROPERTIES TIMEOUT 120)
else()
  message(STATUS "Node.js not found: skipping JavaScript UI tests")
endif()

# The dataset generator uses only the Python standard library.
find_program(PYTHON3_EXECUTABLE NAMES python3 python)
if(PYTHON3_EXECUTABLE)
  add_test(
    NAME dataset_generator
    COMMAND ${PYTHON3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/test_generate_dataset.py)
  set_tests_properties(dataset_generator PROPERTIES TIMEOUT 300)
  add_test(NAME cli_client COMMAND ${PYTHON3_EXECUTABLE}
           ${CMAKE_CURRENT_SOURCE_DIR}/test_cli.py --binary $<TARGET_FILE:search>)
  set_tests_properties(cli_client PROPERTIES TIMEOUT 120)
  add_test(NAME model_preparation COMMAND ${PYTHON3_EXECUTABLE}
           ${CMAKE_CURRENT_SOURCE_DIR}/test_prepare_model.py)
  set_tests_properties(model_preparation PROPERTIES TIMEOUT 120)
else()
  message(STATUS "Python 3 not found: skipping dataset generator tests")
endif()
