cmake_minimum_required(VERSION 3.22)

project(corral VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Wpedantic -Werror)

add_library(corralcore STATIC
  src/cli.cpp
  src/runner.cpp
  src/spawn.cpp
  src/supervise.cpp
)
target_include_directories(corralcore PUBLIC src)
target_compile_definitions(corralcore PUBLIC CORRAL_VERSION="${PROJECT_VERSION}")

add_executable(corral src/main.cpp)
target_link_libraries(corral PRIVATE corralcore)

add_executable(unit_tests tests/unit/unit_tests.cpp)
target_link_libraries(unit_tests PRIVATE corralcore)

enable_testing()

add_test(NAME unit_tests COMMAND unit_tests)

# Command-line checks against the real binary.
set(CORRAL_BIN $<TARGET_FILE:corral>)
add_test(NAME cli_echo_hi
  COMMAND sh -c "out=$(\"$0\" -- echo hi) && [ \"$out\" = hi ]" ${CORRAL_BIN})
add_test(NAME cli_false_exits_1
  COMMAND sh -c "\"$0\" -- false; [ $? -eq 1 ]" ${CORRAL_BIN})
add_test(NAME cli_exit_7
  COMMAND sh -c "\"$0\" -- sh -c 'exit 7'; [ $? -eq 7 ]" ${CORRAL_BIN})
add_test(NAME cli_kill_9_exits_137
  COMMAND sh -c "\"$0\" -- sh -c 'kill -9 $$'; [ $? -eq 137 ]" ${CORRAL_BIN})
add_test(NAME cli_nonexistent_exits_127
  COMMAND sh -c "\"$0\" -- /nonexistent 2>/dev/null; [ $? -eq 127 ]" ${CORRAL_BIN})
add_test(NAME cli_usage_error_exits_125
  COMMAND sh -c "\"$0\" --wall 5x -- true 2>/dev/null; [ $? -eq 125 ]" ${CORRAL_BIN})
