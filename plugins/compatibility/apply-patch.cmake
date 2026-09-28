# FetchContent can repeat its patch step when CMake reconfigures.
execute_process(COMMAND "${GIT_EXECUTABLE}" apply --reverse --check "${PATCH}"
  RESULT_VARIABLE applied OUTPUT_QUIET ERROR_QUIET)
if(NOT applied EQUAL 0)
  execute_process(COMMAND "${GIT_EXECUTABLE}" apply "${PATCH}"
    COMMAND_ERROR_IS_FATAL ANY)
endif()
