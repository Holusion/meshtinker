# The wrapper finds subcommands in its own directory, then on PATH, and passes
# arguments and exit codes through.
# Variables: BUILD_DIR, WORK_DIR.

function(expect_run expected_code expected_stdout)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT code STREQUAL "${expected_code}")
        message(FATAL_ERROR "`${ARGN}` exited with ${code}, expected ${expected_code}\nstdout: ${out}\nstderr: ${err}")
    endif()
    if(NOT out STREQUAL "${expected_stdout}")
        message(FATAL_ERROR "`${ARGN}` printed '${out}', expected '${expected_stdout}'")
    endif()
endfunction()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}/alone" "${WORK_DIR}/path")

# A fake subcommand that echoes its arguments and exits with 5.
file(WRITE "${WORK_DIR}/path/meshtinker-fake" "#!/bin/sh\necho \"$(basename \"$0\") $*\"\nexit 5\n")
# A fake that would shadow the real inspect if PATH came first.
file(COPY_FILE "${WORK_DIR}/path/meshtinker-fake" "${WORK_DIR}/path/meshtinker-inspect")
file(CHMOD "${WORK_DIR}/path/meshtinker-fake" "${WORK_DIR}/path/meshtinker-inspect"
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
file(COPY_FILE "${BUILD_DIR}/meshtinker" "${WORK_DIR}/alone/meshtinker")
file(CHMOD "${WORK_DIR}/alone/meshtinker" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)

set(path_env "PATH=${WORK_DIR}/path:$ENV{PATH}")

# The wrapper's own directory comes before PATH: the real inspect, not the fake.
expect_run(2 "" "${CMAKE_COMMAND}" -E env "${path_env}" "${BUILD_DIR}/meshtinker" inspect)
# Found on PATH, with arguments and exit code passed through.
expect_run(5 "meshtinker-fake a b --c\n" "${CMAKE_COMMAND}" -E env "${path_env}" "${BUILD_DIR}/meshtinker" fake a b --c)
# A wrapper alone in its directory finds the real inspect on PATH.
expect_run(2 "" "${CMAKE_COMMAND}" -E env "PATH=${BUILD_DIR}" "${WORK_DIR}/alone/meshtinker" inspect)

# `help <name>` is `<name> --help`.
expect_run(5 "meshtinker-fake --help\n" "${CMAKE_COMMAND}" -E env "${path_env}" "${BUILD_DIR}/meshtinker" help fake)

# --help lists commands from both places, each with its --summary line.
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "${path_env}" "${BUILD_DIR}/meshtinker" --help
    RESULT_VARIABLE code OUTPUT_VARIABLE out)
if(NOT code EQUAL 0 OR NOT out MATCHES "\n  inspect +Report statistics" OR NOT out MATCHES "\n  fake +\\(no summary\\)")
    message(FATAL_ERROR "meshtinker --help exited with ${code} and printed:\n${out}")
endif()
# Alone, the same listing is a usage error, on stderr.
execute_process(COMMAND "${BUILD_DIR}/meshtinker" RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT code EQUAL 2 OR NOT out STREQUAL "" OR NOT err MATCHES "Commands:")
    message(FATAL_ERROR "bare meshtinker exited with ${code}, stdout '${out}'")
endif()
execute_process(COMMAND "${BUILD_DIR}/meshtinker" --version RESULT_VARIABLE code OUTPUT_VARIABLE out)
if(NOT code EQUAL 0 OR NOT out MATCHES "^meshtinker [0-9]+\\.[0-9]+")
    message(FATAL_ERROR "meshtinker --version printed '${out}'")
endif()

# Unknown and malformed names are command-line errors.
expect_run(2 "" "${BUILD_DIR}/meshtinker" no-such-command)
expect_run(2 "" "${BUILD_DIR}/meshtinker" ../meshtinker-inspect)
expect_run(2 "" "${BUILD_DIR}/meshtinker" Inspect)
