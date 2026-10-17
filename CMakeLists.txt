cmake_minimum_required(VERSION 3.18)
project(adaptive_hash_join LANGUAGES CXX)

option(HJ_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

find_package(OpenMP REQUIRED)

add_library(hash_join
    src/data.cpp
    src/partition.cpp
    src/scheduler.cpp
    src/join.cpp
)
target_include_directories(hash_join PUBLIC include)
target_compile_features(hash_join PUBLIC cxx_std_17)
target_link_libraries(hash_join PUBLIC OpenMP::OpenMP_CXX)

if(MSVC)
    target_compile_options(hash_join PRIVATE /W4 /permissive-)
else()
    target_compile_options(hash_join PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

if(HJ_ENABLE_SANITIZERS AND NOT MSVC)
    target_compile_options(hash_join PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(hash_join PUBLIC -fsanitize=address,undefined)
endif()

add_executable(hashjoin src/main.cpp)
target_link_libraries(hashjoin PRIVATE hash_join)

add_executable(hashjoin_tests tests/test_hash_join.cpp)
target_link_libraries(hashjoin_tests PRIVATE hash_join)

enable_testing()
add_test(NAME hashjoin_correctness COMMAND hashjoin_tests)
