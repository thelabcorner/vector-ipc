cmake_minimum_required(VERSION 3.20)

project(VectorIPCPackageConsumer LANGUAGES C CXX)

find_package(VectorIPC 0.1 CONFIG REQUIRED)

add_executable(vectoripc_package_consumer_c main.c)
target_compile_features(vectoripc_package_consumer_c PRIVATE c_std_11)
target_link_libraries(vectoripc_package_consumer_c PRIVATE VectorIPC::vectoripc)

add_executable(vectoripc_package_consumer main.cpp)
target_compile_features(vectoripc_package_consumer PRIVATE cxx_std_17)
target_link_libraries(vectoripc_package_consumer PRIVATE VectorIPC::vectoripc)
