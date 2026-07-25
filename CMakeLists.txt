cmake_minimum_required(VERSION 3.16)
project(BanksConnectApp)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SQLITE3 REQUIRED sqlite3)
find_package(OpenSSL REQUIRED)

# Everything except main() lives in a library so the test binary can link against
# the same objects the server runs.
add_library(banksconnect_core STATIC
        src/acc.cc
        src/acc_invest.cc
        src/enablebanking_client.cc
        src/app_server.cc
        src/json_mapper.cc
        src/money.cc
        src/database.cc
)
target_include_directories(banksconnect_core PUBLIC src ${SQLITE3_INCLUDE_DIRS} ${OPENSSL_INCLUDE_DIR})
target_link_libraries(banksconnect_core PUBLIC Threads::Threads ${SQLITE3_LIBRARIES} OpenSSL::Crypto)

add_executable(BanksConnectApp main.cpp)
target_link_libraries(BanksConnectApp PRIVATE banksconnect_core)

option(BANKSCONNECT_BUILD_TESTS "Build the unit test binary" ON)
if (BANKSCONNECT_BUILD_TESTS)
    enable_testing()
    add_executable(banksconnect_tests
            tests/test_main.cc
            tests/test_money.cc
            tests/test_json_mapper.cc
            tests/test_database.cc
    )
    target_link_libraries(banksconnect_tests PRIVATE banksconnect_core)
    target_include_directories(banksconnect_tests PRIVATE tests)
    add_test(NAME banksconnect_tests COMMAND banksconnect_tests)
endif ()
