# Disk-loaded QML tests use the same flat paths as the app's resource aliases.
set(PILON_TEST_SOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/test-fixtures/src")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/cmake/TestSources.json" test_sources)
string(JSON test_source_count LENGTH "${test_sources}")
math(EXPR test_source_last "${test_source_count} - 1")
foreach(index RANGE 0 ${test_source_last})
    string(JSON resource MEMBER "${test_sources}" ${index})
    string(JSON source GET "${test_sources}" "${resource}")
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/../${source}"
        "${PILON_TEST_SOURCE_DIR}/${resource}" COPYONLY)
endforeach()
foreach(example IN ITEMS counter checklist)
    if(example STREQUAL "counter")
        set(resource "addon")
    else()
        set(resource "${example}")
    endif()
    file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/../examples/${example}/"
        DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/test-fixtures/examples/${resource}")
endforeach()

file(GLOB_RECURSE fixture_files CONFIGURE_DEPENDS
    RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/*")
foreach(fixture IN LISTS fixture_files)
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/${fixture}"
        "${PILON_TEST_SOURCE_DIR}/tests/fixtures/${fixture}" COPYONLY)
endforeach()
