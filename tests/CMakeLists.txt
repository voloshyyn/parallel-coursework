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
