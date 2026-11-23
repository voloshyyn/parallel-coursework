cmake_minimum_required(VERSION 3.22)
project(parallel_search LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ENABLE_TSAN "Build with ThreadSanitizer" OFF)
if(ENABLE_TSAN)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR "ENABLE_TSAN is supported only by GCC and Clang")
  endif()
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=thread)
endif()

find_package(Threads REQUIRED)

include(FetchContent)
set(FETCHCONTENT_QUIET ON)

FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(nlohmann_json)

set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZSTD_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG v0.65.0
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(httplib)

set(CROW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CROW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CROW_INSTALL OFF CACHE BOOL "" FORCE)
set(CROW_ENABLE_TSAN OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  Crow
  GIT_REPOSITORY https://github.com/CrowCpp/Crow.git
  GIT_TAG v1.3.4
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(Crow)

# The Web UI is embedded into the binary at configure time, so the server needs
# no static directory next to it. CMAKE_CONFIGURE_DEPENDS reruns the configure
# step when an asset changes.
set(WEB_ASSET_DIR ${CMAKE_CURRENT_SOURCE_DIR}/web)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  ${WEB_ASSET_DIR}/index.html
  ${WEB_ASSET_DIR}/app.js
  ${WEB_ASSET_DIR}/search_client.js
  ${WEB_ASSET_DIR}/style.css
  ${WEB_ASSET_DIR}/vue.global.prod.js)
file(READ ${WEB_ASSET_DIR}/index.html INDEX_HTML)
file(READ ${WEB_ASSET_DIR}/app.js APP_JS)
file(READ ${WEB_ASSET_DIR}/search_client.js SEARCH_CLIENT_JS)
file(READ ${WEB_ASSET_DIR}/style.css STYLE_CSS)
file(READ ${WEB_ASSET_DIR}/vue.global.prod.js VUE_JS)
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/src/web/assets.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/web_assets.h
  @ONLY)

add_library(search_core STATIC
  src/api.cpp
  src/config.cpp
  src/core/inverted_index.cpp
  src/core/search_engine.cpp
  src/core/thread_pool.cpp
  src/core/tokenizer.cpp
  src/integrations/ollama_client.cpp
  src/integrations/ollama_monitor.cpp
  src/services/index_service.cpp
  src/services/search_pipeline.cpp
  src/services/search_socket.cpp
  src/web_ui.cpp)
target_include_directories(search_core PUBLIC src)
target_include_directories(search_core PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(search_core
  PUBLIC
    Crow::Crow
    nlohmann_json::nlohmann_json
    httplib::httplib
    Threads::Threads)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(search_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(search_server src/main.cpp)
add_executable(search src/cli.cpp)
target_link_libraries(search PRIVATE httplib::httplib nlohmann_json::nlohmann_json)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(search PRIVATE -Wall -Wextra -Wpedantic)
endif()
target_link_libraries(search_server PRIVATE search_core)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(search_server PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
