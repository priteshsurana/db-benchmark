cmake_minimum_required(VERSION 3.26)
project(db_benchmark CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

set(CMAKE_CXX_FLAGS_RELEASE "-O2")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")

add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
)

find_package(PostgreSQL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PQXX REQUIRED libpqxx)

#MongoPackage
find_package(mongocxx REQUIRED)
find_package(bsoncxx REQUIRED)


#Cassandra
find_library(CASSANDRA_LIB
    NAMES cassandra
    PATHS /usr/local/lib /usr/lib
    REQUIRED)
find_path(CASSANDRA_INCLUDE_DIR
    NAMES cassandra.h
    PATHS /usr/local/include /usr/include
    REQUIRED)



find_package(yaml-cpp REQUIRED)
find_package(Threads REQUIRED)

set(SOURCES
    src/main.cpp
    src/config/ConfigLoader.cpp
    src/data/DataGenerator.cpp
    src/db/PostgresClient.cpp
    src/db/MongoClient.cpp
    src/db/CassandraClient.cpp

    src/experiment/ExperimentBase.cpp
    src/experiment/Experiment1.cpp
    src/metrics/LatencyRecorder.cpp
    src/metrics/MetricsCollector.cpp
    src/results/CSVWriter.cpp
    src/results/SummaryPrinter.cpp
)

add_executable(db_benchmark ${SOURCES})

target_include_directories(db_benchmark PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${PostgreSQL_INCLUDE_DIRS}
    ${PQXX_INCLUDE_DIRS}
    ${CASSANDRA_INCLUDE_DIR}

)

target_link_libraries(db_benchmark PRIVATE
    ${PostgreSQL_LIBRARIES}
    ${PQXX_LIBRARIES}
    mongo::mongocxx_shared
    mongo::bsoncxx_shared

    ${CASSANDRA_LIB}
    yaml-cpp
    Threads::Threads
)

set_target_properties(db_benchmark PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}
)

message(STATUS "PostgreSQL include: ${PostgreSQL_INCLUDE_DIRS}")
message(STATUS "PostgreSQL include: ${PostgreSQL_LIBRARIES}")
message(STATUS "libpqxx include: ${PQXX_INCLUDE_DIRS}")
message(STATUS "libpqxx libs: ${PQXX_LIBRARIES}")
message(STATUS "Cassandra include: ${CASSANDRA_INCLUDE_DIR}")
message(STATUS "Cassandra lib: ${CASSANDRA_LIB}")
message(STATUS "yaml-cpp found: YES")
message(STATUS "mongocxx found: YES")