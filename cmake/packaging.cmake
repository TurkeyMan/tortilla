# Linux packages: .deb and .rpm through CPack, split the way DESIGN.md's
# "Source layout and packaging" describes. The Arch package builds the same
# install components from packaging/arch/PKGBUILD.
#
#   cpack -G DEB    (on Debian/Ubuntu: needs dpkg-shlibdeps)
#   cpack -G RPM    (on Fedora/RHEL: needs rpmbuild)
#
# Configure with -DCMAKE_INSTALL_PREFIX=/usr: the core looks for backends in
# the libdir it was configured with.

set(TORTILLA_PACKAGE_VERSION ${PROJECT_VERSION} CACHE STRING
    "Version the packages carry (CI sets one from git describe)")

# What the user installs: one per toolkit, pulling in the app and its backend.
# Empty but for the licence, which also gives CPack something to package.
set(tortilla_toolkits)
foreach(tk gtk3 gtk4 qt6)
    if(TARGET rapport_${tk})
        list(APPEND tortilla_toolkits ${tk})
        string(REPLACE qt6 qt pkg tortilla-${tk})
        install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/${pkg}
                COMPONENT tortilla_${tk})
    endif()
endforeach()

set(CPACK_PACKAGE_NAME tortilla)
set(CPACK_PACKAGE_VERSION ${TORTILLA_PACKAGE_VERSION})
set(CPACK_PACKAGE_VENDOR "Manu Evans")
set(CPACK_PACKAGE_CONTACT "Manu Evans <turkeyman@gmail.com>")
set(CPACK_PACKAGE_HOMEPAGE_URL https://github.com/TurkeyMan/tortilla)
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "A fast, native git client")
set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE)
set(CPACK_PACKAGE_DIRECTORY ${CMAKE_BINARY_DIR}/packages)
# The null backend is for tests, and isn't installed.
set(CPACK_COMPONENTS_ALL core dev tortilla)
foreach(tk IN LISTS tortilla_toolkits)
    list(APPEND CPACK_COMPONENTS_ALL ${tk} tortilla_${tk})
endforeach()

# Component -> package name, and what it needs of ours.
set(pkg_core librapport)
set(pkg_dev librapport-dev)
set(pkg_tortilla tortilla-core)
set(CPACK_COMPONENT_DEV_DEPENDS core)
set(CPACK_COMPONENT_TORTILLA_DEPENDS core)
foreach(tk IN LISTS tortilla_toolkits)
    string(TOUPPER ${tk} TK)
    string(REPLACE qt6 qt app tortilla-${tk})
    set(pkg_${tk} librapport-${tk})
    set(pkg_tortilla_${tk} ${app})
    set(CPACK_COMPONENT_${TK}_DEPENDS core)
    set(CPACK_COMPONENT_TORTILLA_${TK}_DEPENDS tortilla ${tk})
endforeach()

set(CPACK_COMPONENT_CORE_DESCRIPTION
    "rapport, the toolkit-neutral UI library Tortilla is built on: the core\n"
    "detects the desktop and loads the backend for its toolkit.")
set(CPACK_COMPONENT_DEV_DESCRIPTION "rapport's headers and CMake package, for building against it.")
set(CPACK_COMPONENT_TORTILLA_DESCRIPTION
    "The Tortilla application, linked to no toolkit. Install tortilla-gtk4,\n"
    "tortilla-gtk3 or tortilla-qt for the one that matches your desktop.")
foreach(tk IN LISTS tortilla_toolkits)
    string(TOUPPER ${tk} TK)
    set(CPACK_COMPONENT_${TK}_DESCRIPTION "rapport's ${tk} backend plugin.")
    set(CPACK_COMPONENT_TORTILLA_${TK}_DESCRIPTION
        "A fast, native git client, with the ${tk} user interface.")
endforeach()

# -- .deb -------------------------------------------------------------------
set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SECTION vcs)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
# librapport.so.0 is in our own librapport package, which shlibdeps can't
# know of; the component dependencies below name it instead.
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS_PRIVATE_DIRS ${CMAKE_BINARY_DIR}/bin)
set(CPACK_DEBIAN_ENABLE_COMPONENT_DEPENDS ON)
foreach(c IN LISTS CPACK_COMPONENTS_ALL)
    string(TOUPPER ${c} C)
    set(CPACK_DEBIAN_${C}_PACKAGE_NAME ${pkg_${c}})
endforeach()
set(CPACK_DEBIAN_DEV_PACKAGE_SECTION libdevel)
set(CPACK_DEBIAN_CORE_PACKAGE_SECTION libs)
# The core loads these when first asked (HTTP, decoding images), not at start.
set(CPACK_DEBIAN_CORE_PACKAGE_RECOMMENDS
    "libcurl4t64 | libcurl4 | libcurl3t64-gnutls, libgdk-pixbuf-2.0-0")
# The app shells out to git for everything but the revision walk.
set(CPACK_DEBIAN_TORTILLA_PACKAGE_DEPENDS git)
set(CPACK_DEBIAN_TORTILLA_PACKAGE_RECOMMENDS "tortilla-gtk4 | tortilla-qt | tortilla-gtk3")

# -- .rpm -------------------------------------------------------------------
set(CPACK_RPM_COMPONENT_INSTALL ON)
set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-only AND MIT")
set(CPACK_RPM_PACKAGE_GROUP "Development/Tools")
# rpmbuild's own requires cover the shared libraries (librapport.so.0
# included, which librapport provides); these are the rest.
foreach(c IN LISTS CPACK_COMPONENTS_ALL)
    string(TOUPPER ${c} C)
    set(CPACK_RPM_${C}_PACKAGE_NAME ${pkg_${c}})
    set(requires)
    foreach(dep IN LISTS CPACK_COMPONENT_${C}_DEPENDS)
        list(APPEND requires "${pkg_${dep}} = ${TORTILLA_PACKAGE_VERSION}")
    endforeach()
    if(c STREQUAL tortilla)
        list(APPEND requires git)
    endif()
    list(JOIN requires ", " requires)
    set(CPACK_RPM_${C}_PACKAGE_REQUIRES "${requires}")
endforeach()
set(CPACK_RPM_CORE_PACKAGE_SUGGESTS "libcurl, gdk-pixbuf2")
set(CPACK_RPM_TORTILLA_PACKAGE_SUGGESTS "tortilla-gtk4")
list(APPEND CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
    ${CMAKE_INSTALL_FULL_DATADIR}/applications
    ${CMAKE_INSTALL_FULL_DATADIR}/icons
    ${CMAKE_INSTALL_FULL_DATADIR}/icons/hicolor)
file(GLOB sizes RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/data/icons/hicolor
     ${CMAKE_CURRENT_SOURCE_DIR}/data/icons/hicolor/*)
foreach(size IN LISTS sizes)
    list(APPEND CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
         ${CMAKE_INSTALL_FULL_DATADIR}/icons/hicolor/${size}
         ${CMAKE_INSTALL_FULL_DATADIR}/icons/hicolor/${size}/apps)
endforeach()

include(CPack)
