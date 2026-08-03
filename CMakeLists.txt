cmake_minimum_required(VERSION 3.22)

project(corral VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra -Wpedantic -Werror)

add_library(corralcore STATIC
  src/audit.cpp
  src/cgroup.cpp
  src/cli.cpp
  src/procscan.cpp
  src/runner.cpp
  src/spawn.cpp
  src/supervise.cpp
  src/terminate.cpp
)
target_include_directories(corralcore PUBLIC src)
target_compile_definitions(corralcore PUBLIC CORRAL_VERSION="${PROJECT_VERSION}")

add_executable(corral src/main.cpp)
target_link_libraries(corral PRIVATE corralcore)

add_executable(unit_tests tests/unit/unit_tests.cpp)
target_link_libraries(unit_tests PRIVATE corralcore)

# Misbehaving commands for the tests, built into build/faults/.
set(FAULT_PROGRAMS sleeper streamer stdin_waiter daemonizer daemonizer_stay term_ignorer
    forker pipe_holder mem_hog graceful)
foreach(name IN LISTS FAULT_PROGRAMS)
  add_executable(corral_${name} faults/corral_${name}.c)
  set_target_properties(corral_${name} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/faults)
endforeach()
file(COPY faults/corral_script_spawner.sh
  DESTINATION ${CMAKE_BINARY_DIR}/faults
  FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                   GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

enable_testing()

add_test(NAME unit_tests COMMAND unit_tests)

add_test(NAME integration
  COMMAND bash ${CMAKE_SOURCE_DIR}/scripts/run-integration.sh)
set_tests_properties(integration PROPERTIES
  ENVIRONMENT "CORRAL_BUILD_DIR=${CMAKE_BINARY_DIR};PYTHON=${Python3_EXECUTABLE}")

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

# Memory errors and leaks in corral itself. The child is not traced: it execs
# the command.
find_program(VALGRIND valgrind)
if(VALGRIND)
  add_test(NAME valgrind_true
    COMMAND ${VALGRIND} --quiet --leak-check=full --errors-for-leak-kinds=definite,indirect
            --error-exitcode=99 ${CORRAL_BIN} --quiet -- true)
endif()

# The audit record goes through /dev/fd/3 into a pipe.
add_test(NAME cli_json_parses
  COMMAND sh -c "\"$0\" --quiet --json /dev/fd/3 -- true 3>&1 >/dev/null | \"$1\" -m json.tool >/dev/null"
          ${CORRAL_BIN} ${Python3_EXECUTABLE})
add_test(NAME cli_summary_line
  COMMAND sh -c "\"$0\" -- true 2>&1 >/dev/null | grep -Eqx 'corral: exited code=0 in [0-9]+[.][0-9]{3}s mode=fallback leftovers_killed=0'"
          ${CORRAL_BIN})
