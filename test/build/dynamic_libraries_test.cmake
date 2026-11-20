# Only the C library and its companions are linked dynamically (AGENTS.md,
# "Architecture"). Variables: BUILD_DIR, COMMANDS.

find_program(LDD ldd REQUIRED)
# Before glibc 2.34, threads, dlopen and some realtime functions came in
# separate libraries of glibc: libpthread, libdl and librt.
set(allowed "^(linux-vdso\\.so|linux-gate\\.so|libc\\.so|libm\\.so|libpthread\\.so|libdl\\.so|librt\\.so|ld-linux[-a-z0-9_]*\\.so)")
foreach(program meshtinker ${COMMANDS})
    execute_process(COMMAND "${LDD}" "${BUILD_DIR}/${program}" OUTPUT_VARIABLE out RESULT_VARIABLE code)
    if(NOT code EQUAL 0)
        message(FATAL_ERROR "ldd failed on ${program}")
    endif()
    string(REPLACE "\n" ";" lines "${out}")
    foreach(line IN LISTS lines)
        string(STRIP "${line}" line)
        if(line STREQUAL "")
            continue()
        endif()
        # Lines start with the library's name ("libc.so.6 => /lib/...") or,
        # for the loader, its path ("/lib64/ld-linux-x86-64.so.2 (0x...)").
        string(REGEX REPLACE "[ \t].*" "" library "${line}")
        get_filename_component(library "${library}" NAME)
        if(NOT library MATCHES "${allowed}")
            message(FATAL_ERROR "${program} links ${library} dynamically:\n${out}")
        endif()
    endforeach()
endforeach()
