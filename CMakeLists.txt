cmake_minimum_required(VERSION 3.16)
project(BanksConnectApp)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SQLITE3 REQUIRED sqlite3)

add_executable(BanksConnectApp
        main.cpp
        src/acc.cc
        src/acc_invest.cc
        src/enablebanking_client.cc
        src/app_server.cc
        src/json_mapper.cc
        src/webhook_server.cc
        src/database.cc
)

find_package(OpenSSL REQUIRED)
target_include_directories(BanksConnectApp PRIVATE src ${SQLITE3_INCLUDE_DIRS} ${OPENSSL_INCLUDE_DIR})
target_link_libraries(BanksConnectApp PRIVATE Threads::Threads ${SQLITE3_LIBRARIES} OpenSSL::Crypto)
