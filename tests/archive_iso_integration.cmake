if(NOT DEFINED ARCHIVE_BUILD OR NOT EXISTS "${ARCHIVE_BUILD}")
    message(FATAL_ERROR "ARCHIVE_BUILD must point to mycom-archive-build")
endif()

if(NOT DEFINED ENV{MYCOM_ISO} OR "$ENV{MYCOM_ISO}" STREQUAL "")
    message(STATUS "SKIPPED: set MYCOM_ISO to run the full ISO archive integration test")
    return()
endif()

set(ISO_PATH "$ENV{MYCOM_ISO}")
if(NOT EXISTS "${ISO_PATH}")
    message(FATAL_ERROR "MYCOM_ISO does not exist: ${ISO_PATH}")
endif()

file(REMOVE_RECURSE "${ARCHIVE_WORK_DIRECTORY}")
execute_process(
    COMMAND "${ARCHIVE_BUILD}" "${ISO_PATH}" "${ARCHIVE_WORK_DIRECTORY}"
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_error
)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR "Full ISO archive build failed (${build_result})\n${build_output}\n${build_error}")
endif()

set(MANIFEST "${ARCHIVE_WORK_DIRECTORY}/manifest.json")
if(NOT EXISTS "${MANIFEST}")
    message(FATAL_ERROR "Archive build did not create manifest.json")
endif()
file(READ "${MANIFEST}" manifest_text)
foreach(expected
        "\"format\": \"mycom-archive/v1\""
        "\"schemaVersion\": 1"
        "\"mvbCount\": 17"
        "\"dbfRecordCount\": 2010"
        "\"conversionBookCount\": 17"
        "\"normalizedFileCount\": 7688")
    if(NOT manifest_text MATCHES "${expected}")
        message(FATAL_ERROR "Manifest verification failed; missing pattern: ${expected}")
    endif()
endforeach()

string(REGEX MATCH "\"isoSha256\": \"[0-9a-f]+\"" iso_hash_line "${manifest_text}")
string(REGEX REPLACE ".*\"isoSha256\": \"([0-9a-f]+)\"" "\\1" iso_hash "${iso_hash_line}")
string(LENGTH "${iso_hash}" iso_hash_length)
if(NOT iso_hash_length EQUAL 64)
    message(FATAL_ERROR "Manifest ISO SHA-256 is missing or has an invalid length")
endif()
string(REGEX MATCH "\"sha256\": \"[0-9a-f]+\"" file_hash_line "${manifest_text}")
string(REGEX REPLACE ".*\"sha256\": \"([0-9a-f]+)\"" "\\1" file_hash "${file_hash_line}")
string(LENGTH "${file_hash}" file_hash_length)
if(NOT file_hash_length EQUAL 64)
    message(FATAL_ERROR "Manifest normalized-file SHA-256 is missing or has an invalid length")
endif()

if(NOT EXISTS "${ARCHIVE_WORK_DIRECTORY}/content/HEADA.json")
    message(FATAL_ERROR "Archive build did not create HEADA content")
endif()
message(STATUS "Full ISO archive integration test passed")
