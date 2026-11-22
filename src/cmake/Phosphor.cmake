# https://registry.npmjs.org/@phosphor-icons/core/-/core-2.1.1.tgz
# The website uses Fuse 6.6.2. fuse.mjs is its dist/fuse.common.js with the final export adapted for Qt.
FetchContent_Declare(pilon_phosphor
    URL "${CMAKE_CURRENT_SOURCE_DIR}/assets/phosphor/phosphor-icons-core-2.1.1.tgz"
    URL_HASH SHA256=313332be6190b724da24107addd781799b48bf76b13963f24501112ffe1baadd
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(pilon_phosphor)

function(pilon_embed_phosphor target)
    # Qt builds the resource in a separate object linked into PIE executables and Android shared libraries.
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    file(GLOB_RECURSE phosphor_icons "${pilon_phosphor_SOURCE_DIR}/assets/*.svg")
    set_source_files_properties("${pilon_phosphor_SOURCE_DIR}/dist/index.mjs" PROPERTIES QT_RESOURCE_ALIAS "catalog.mjs")
    set_source_files_properties("${pilon_phosphor_SOURCE_DIR}/LICENSE" PROPERTIES QT_RESOURCE_ALIAS "LICENSE")
    foreach(file IN ITEMS search.mjs fuse.mjs FUSE_LICENSE.txt)
        set_source_files_properties("assets/phosphor/${file}" PROPERTIES QT_RESOURCE_ALIAS "${file}")
    endforeach()
    qt_add_resources(${target} phosphor_icons BIG_RESOURCES PREFIX "/phosphor" BASE "${pilon_phosphor_SOURCE_DIR}/assets"
        FILES ${phosphor_icons} "${pilon_phosphor_SOURCE_DIR}/dist/index.mjs" "${pilon_phosphor_SOURCE_DIR}/LICENSE"
        assets/phosphor/search.mjs assets/phosphor/fuse.mjs assets/phosphor/FUSE_LICENSE.txt)
endfunction()
pilon_embed_phosphor(pilon_files)
