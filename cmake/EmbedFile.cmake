if(NOT DEFINED INPUT_FILE OR NOT DEFINED OUTPUT_FILE)
    message(FATAL_ERROR "INPUT_FILE and OUTPUT_FILE are required")
endif()

# Numeric byte initializers preserve file encoding and avoid compiler string literal limits.
file(READ "${INPUT_FILE}" _bytes HEX)
string(REGEX REPLACE "(................................)" "\\1\n" _bytes "${_bytes}")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1, " _bytes "${_bytes}")

get_filename_component(_output_dir "${OUTPUT_FILE}" DIRECTORY)
file(MAKE_DIRECTORY "${_output_dir}")
file(WRITE "${OUTPUT_FILE}" "${_bytes}\n")
