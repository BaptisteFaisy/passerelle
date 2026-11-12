# SPDX-License-Identifier: Apache-2.0
#
# cmake/passerelle-install.cmake - install rules and the exported package.
#
# Gap 37 of docs/gap-analysis.md: the tree had no `cmake --install` target,
# no pkg-config file and no exported CMake package, so a downstream project
# could not consume Passerelle at all.
#
# What this file installs (only when PGW_INSTALL is ON, which is the default
# and never changes the build itself):
#
#   * the portable static libraries, under ${CMAKE_INSTALL_LIBDIR};
#   * the public headers, under ${CMAKE_INSTALL_INCLUDEDIR}, keeping their
#     `pgw/` prefix so that `#include "pgw/buf.h"` keeps working;
#   * `passerelle.pc`, the pkg-config description (version read from the
#     VERSION file, never written twice);
#   * `PasserelleConfig.cmake` + `PasserelleTargets.cmake`, so that a
#     downstream project can write `find_package(Passerelle REQUIRED)` and
#     link `Passerelle::pgw_common`.
#
# The exported targets keep their build-tree include directories through
# `$<BUILD_INTERFACE:...>` and advertise the installed ones through
# `$<INSTALL_INTERFACE:...>`: without that split CMake refuses to export a
# target whose include path points inside the source tree.

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(PGW_INSTALL "Generate the install rules (libraries, headers, pkg-config, CMake package)" ON)

set(PGW_EXPORT_NAME Passerelle)
set(PGW_PKG_NAME passerelle)
set(PGW_PKG_DESCRIPTION "Shared folder for QNX 8.0 guests under QEMU: 9P2000.L and virtiofs")

# The libraries of the installed package, in dependency order (a dependent
# before its dependencies: that is the order a static linker needs).
set(PGW_PUBLIC_LIBRARIES
    pgw_drivers_portable
    pgw_rmfs
    pgw_protofuse
    pgw_proto9p
    pgw_virtio
    pgw_common
)

# pgw_export_include_dirs(<target>) - make the include directories of a
# target exportable: the source-tree ones become BUILD_INTERFACE (they are
# only valid inside this build tree) and the installed ones become
# INSTALL_INTERFACE.
function(pgw_export_include_dirs target)
    get_target_property(pgw_incs ${target} INTERFACE_INCLUDE_DIRECTORIES)
    set(pgw_new "")
    if(pgw_incs)
        foreach(pgw_inc IN LISTS pgw_incs)
            if(IS_ABSOLUTE "${pgw_inc}")
                list(APPEND pgw_new "$<BUILD_INTERFACE:${pgw_inc}>")
            else()
                list(APPEND pgw_new "${pgw_inc}")
            endif()
        endforeach()
    endif()
    list(APPEND pgw_new "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
    set_property(TARGET ${target} PROPERTY
                 INTERFACE_INCLUDE_DIRECTORIES "${pgw_new}")
endfunction()

# pgw_header_dirs(<target> <out_var>) - collect the include directories of a
# target that really hold public headers.
function(pgw_header_dirs target out_var)
    get_target_property(pgw_incs ${target} INTERFACE_INCLUDE_DIRECTORIES)
    set(pgw_found "")
    if(pgw_incs)
        foreach(pgw_inc IN LISTS pgw_incs)
            if(IS_ABSOLUTE "${pgw_inc}" AND IS_DIRECTORY "${pgw_inc}")
                list(APPEND pgw_found "${pgw_inc}")
            endif()
        endforeach()
    endif()
    list(REMOVE_DUPLICATES pgw_found)
    set(${out_var} "${pgw_found}" PARENT_SCOPE)
endfunction()

# pgw_install_package() - the whole install/export story, called once from
# the top level CMakeLists.txt after the modules are added.
function(pgw_install_package)
    set(pgw_targets "")
    set(pgw_header_dirs "")
    set(pgw_libs "")
    foreach(pgw_tgt IN LISTS PGW_PUBLIC_LIBRARIES)
        if(NOT TARGET ${pgw_tgt})
            continue()
        endif()
        list(APPEND pgw_targets ${pgw_tgt})
        # The header directories must be collected before the include paths
        # are rewritten into BUILD_INTERFACE/INSTALL_INTERFACE generator
        # expressions, which IS_ABSOLUTE() cannot see through.
        pgw_header_dirs(${pgw_tgt} pgw_dirs)
        list(APPEND pgw_header_dirs ${pgw_dirs})
        pgw_export_include_dirs(${pgw_tgt})
    endforeach()
    if(NOT pgw_targets)
        message(WARNING "PGW_INSTALL is ON but no public library was built: "
                        "nothing to install")
        return()
    endif()
    list(REMOVE_DUPLICATES pgw_header_dirs)

    install(TARGETS ${pgw_targets}
            EXPORT ${PGW_EXPORT_NAME}Targets
            ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
            LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
            RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
            INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

    foreach(pgw_dir IN LISTS pgw_header_dirs)
        install(DIRECTORY "${pgw_dir}/" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
                FILES_MATCHING PATTERN "*.h")
    endforeach()

    install(EXPORT ${PGW_EXPORT_NAME}Targets
            FILE ${PGW_EXPORT_NAME}Targets.cmake
            NAMESPACE ${PGW_EXPORT_NAME}::
            DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${PGW_EXPORT_NAME}")

    configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/${PGW_EXPORT_NAME}Config.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/${PGW_EXPORT_NAME}Config.cmake"
        INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${PGW_EXPORT_NAME}")
    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/${PGW_EXPORT_NAME}ConfigVersion.cmake"
        VERSION "${PGW_VERSION}" COMPATIBILITY SameMajorVersion)
    install(FILES
            "${CMAKE_CURRENT_BINARY_DIR}/${PGW_EXPORT_NAME}Config.cmake"
            "${CMAKE_CURRENT_BINARY_DIR}/${PGW_EXPORT_NAME}ConfigVersion.cmake"
            DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${PGW_EXPORT_NAME}")

    pgw_write_pkgconfig("${pgw_targets}")
endfunction()

# pgw_write_pkgconfig(<targets>) - generate and install passerelle.pc.
# The prefix is computed from the location of the .pc file itself, so the
# installed tree can be moved anywhere without editing the file.
function(pgw_write_pkgconfig targets)
    set(pgw_libs "")
    foreach(pgw_tgt IN LISTS targets)
        list(APPEND pgw_libs "-l${pgw_tgt}")
    endforeach()
    string(REPLACE ";" " " PGW_PC_LIBS "${pgw_libs}")
    # Relative path from the library directory to the include directory, so
    # that the .pc file stays valid wherever the tree is installed.
    file(RELATIVE_PATH PGW_PC_INCLUDEDIR
         "${CMAKE_INSTALL_FULL_LIBDIR}" "${CMAKE_INSTALL_FULL_INCLUDEDIR}")
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/${PGW_PKG_NAME}.pc.in"
                   "${CMAKE_CURRENT_BINARY_DIR}/${PGW_PKG_NAME}.pc" @ONLY)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${PGW_PKG_NAME}.pc"
            DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")
endfunction()
