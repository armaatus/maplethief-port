# CPack External package script for macOS (cmake/Packaging.cmake, ADR 0023).
#
# CPack has installed the kit into its staging directory; zip it with ditto, as Finder does,
# so that the .app keeps its symlinks and code signature. --keepParent makes the staging
# directory, named after CPACK_PACKAGE_FILE_NAME, the zip's top level. --norsrc leaves out
# extended attributes (e.g. com.apple.provenance), which ditto would otherwise store as an
# AppleDouble ._ file next to every entry; the bundle's signature doesn't depend on them
# (codesign --verify passes after ditto -x and after plain unzip; ADR 0024).

set(_stage "${CPACK_TEMPORARY_DIRECTORY}")
get_filename_component(_name "${_stage}" NAME)
if(NOT _name STREQUAL CPACK_PACKAGE_FILE_NAME)
    message(FATAL_ERROR "DittoZip: staging directory ${_stage} is not named "
                        "${CPACK_PACKAGE_FILE_NAME}")
endif()
set(_zip "${CPACK_TOPLEVEL_DIRECTORY}/${CPACK_PACKAGE_FILE_NAME}.zip")
file(REMOVE "${_zip}")
execute_process(
    COMMAND ditto -c -k --norsrc --keepParent "${_stage}" "${_zip}"
    RESULT_VARIABLE _result
    COMMAND_ECHO STDOUT)
if(NOT _result EQUAL 0)
    message(FATAL_ERROR "DittoZip: ditto failed (${_result})")
endif()
set(CPACK_EXTERNAL_BUILT_PACKAGES "${_zip}")
