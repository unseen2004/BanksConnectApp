cmake_minimum_required(VERSION 3.16)
project(BanksConnectApp)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Threads REQUIRED)

add_executable(BanksConnectApp
        main.cpp
        src/acc.cc
        src/acc_invest.cc
        src/enablebanking_client.cc
        src/app_server.cc
        src/json_mapper.cc
        src/webhook_server.cc
)

target_include_directories(BanksConnectApp PRIVATE src)
target_link_libraries(BanksConnectApp PRIVATE Threads::Threads)
