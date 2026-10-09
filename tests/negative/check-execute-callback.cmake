# A compile failure is expected, but only the execute callback diagnostic proves it.
foreach(required IN ITEMS QBM_NEGATIVE_BUILD_DIR QBM_NEGATIVE_TARGET QBM_NEGATIVE_CONFIG)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${QBM_NEGATIVE_BUILD_DIR}" --config "${QBM_NEGATIVE_CONFIG}"
            --target "${QBM_NEGATIVE_TARGET}"
    RESULT_VARIABLE compile_result
    OUTPUT_VARIABLE compile_stdout
    ERROR_VARIABLE compile_stderr)

if("${compile_result}" STREQUAL "0")
    message(FATAL_ERROR "${QBM_NEGATIVE_TARGET} accepted an invalid execute callback")
endif()
string(FIND "${compile_stdout}\n${compile_stderr}" "execute callback must accept" diagnostic_at)
if(diagnostic_at EQUAL -1)
    message(FATAL_ERROR "${QBM_NEGATIVE_TARGET} failed for an unrelated reason:\n${compile_stdout}\n${compile_stderr}")
endif()
message(STATUS "${QBM_NEGATIVE_TARGET} rejected its invalid execute callback")
