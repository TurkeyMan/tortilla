# Linux packages: .deb and .rpm through CPack. One package with everything
# (below), or with TORTILLA_PACKAGE_SPLIT the split DESIGN.md's "Source
# layout and packaging" describes. packaging/arch/PKGBUILD builds the one
# package from the same install components.
#
#   cpack -G DEB    (on Debian/Ubuntu: needs dpkg-shlibdeps)
#   cpack -G RPM    (on Fedora/RHEL: needs rpmbuild)
#
# Configure with -DCMAKE_INSTALL_PREFIX=/usr: the core looks for backends in
# the folder it was configured with.
#
# rapport is Tortilla's own copy, in <libdir>/tortilla (CMakeLists.txt), and
# so are its packages: tortilla-rapport and tortilla-rapport-<toolkit>. An
# app on another rapport version ships its own the same way.

set(TORTILLA_PACKAGE_VERSION ${PROJECT_VERSION} CACHE STRING
    "Version the packages carry (CI sets one from git describe)")

# What the user installs: one per toolkit, pulling in the app, its backend
# and the toolkit; or plain "tortilla" (below), for whoever doesn't know
# which toolkit their desktop runs. Empty but for the licence, which also
# gives CPack something to package.
set(tortilla_toolkits)
foreach(tk gtk3 gtk4 qt6)
    if(TARGET rapport_${tk})
        list(APPEND tortilla_toolkits ${tk})
        string(REPLACE qt6 qt pkg tortilla-${tk})
        install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/${pkg}
                COMPONENT tortilla_${tk})
    endif()
endforeach()
# "tortilla": the app and every backend, but none of the toolkits. A desktop
# already has at least one of them, and rapport loads the backend that
# matches it at run time, falling back to whichever it can load.
# Its licence beside, not in, tortilla-core's doc/tortilla (which the app
# opens from its About links).
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/tortilla
        COMPONENT tortilla_all)

# The toolkit each tortilla-<toolkit> package depends on. The backend
# packages name none, so that "tortilla" can carry all three.
set(toolkit_deb_gtk3 "libgtk-3-0t64 | libgtk-3-0")
set(toolkit_deb_gtk4 libgtk-4-1)
set(toolkit_deb_qt6 "libqt6widgets6t64 | libqt6widgets6, libqt6printsupport6t64 | libqt6printsupport6, qt6-qpa-plugins")
set(toolkit_rpm_gtk3 gtk3)
set(toolkit_rpm_gtk4 gtk4)
set(toolkit_rpm_qt6 qt6-qtbase-gui)

# A file manager's context menu, one package each (tortilla-nautilus, ...):
# the extension rapport_add_shell_extension() installs, and the file
# manager's own package for each kind of package.
# PCManFM(-Qt) reads file manager actions rather than loading an extension,
# so its package is Tortilla's own (CMakeLists.txt), alongside rapport's.
set(tortilla_shells ${RAPPORT_SHELL_FILE_MANAGERS} pcmanfm)
set(fm_deb_nautilus nautilus)
set(fm_deb_nemo nemo)
set(fm_deb_caja caja)
set(fm_deb_thunar thunar)
set(fm_deb_dolphin dolphin)
set(fm_deb_pcmanfm "pcmanfm-qt | pcmanfm")
set(fm_rpm_nautilus nautilus)
set(fm_rpm_nemo nemo)
set(fm_rpm_caja caja)
set(fm_rpm_thunar Thunar)
set(fm_rpm_dolphin dolphin)
set(fm_rpm_pcmanfm "(pcmanfm-qt or pcmanfm)")

set(CPACK_PACKAGE_NAME tortilla)
set(CPACK_PACKAGE_VERSION ${TORTILLA_PACKAGE_VERSION})
set(CPACK_PACKAGE_VENDOR "Manu Evans")
set(CPACK_PACKAGE_CONTACT "Manu Evans <turkeyman@gmail.com>")
set(CPACK_PACKAGE_HOMEPAGE_URL https://github.com/TurkeyMan/tortilla)
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "A fast, native git client")
set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE)
set(CPACK_PACKAGE_DIRECTORY ${CMAKE_BINARY_DIR}/packages)

# Folders that are other packages' in an .rpm: the menu's, and the icon
# theme's, which are hicolor-icon-theme's (the app's icons, the menu's, and
# the badges' emblems, data/linux/icons).
list(APPEND CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
    ${CMAKE_INSTALL_FULL_DATADIR}/applications
    ${CMAKE_INSTALL_FULL_DATADIR}/icons
    ${CMAKE_INSTALL_FULL_DATADIR}/icons/hicolor)
# The icon theme's folders are hicolor-icon-theme's: the app's icons, the
# menu's, and the badges' emblems (data/linux/icons).
file(GLOB sizes RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/data/icons/hicolor
     ${CMAKE_CURRENT_SOURCE_DIR}/data/icons/hicolor/*)
file(GLOB linux_sizes RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/data/linux/icons/hicolor
     ${CMAKE_CURRENT_SOURCE_DIR}/data/linux/icons/hicolor/*)
list(APPEND sizes ${linux_sizes})
list(REMOVE_DUPLICATES sizes)
foreach(size IN LISTS sizes)
    list(APPEND CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
         ${CMAKE_INSTALL_FULL_DATADIR}/icons/hicolor/${size}
         ${CMAKE_INSTALL_FULL_DATADIR}/icons/hicolor/${size}/apps
         ${CMAKE_INSTALL_FULL_DATADIR}/icons/hicolor/${size}/emblems)
endforeach()

# -- One package --------------------------------------------------------------
# What a release offers: one "tortilla" with everything in it, the app, every
# backend and every file manager's extension, so a download is one file per
# distribution. Each backend and extension is loaded only where its toolkit
# or file manager is, so their libraries are no one's requirement: the
# package needs the app's own libraries and any one toolkit.
# TORTILLA_PACKAGE_SPLIT makes the split packages instead, for a package
# repository, where apt or dnf fetches what each one needs.
option(TORTILLA_PACKAGE_SPLIT "One package per install component (for a package repository)" OFF)
if(NOT TORTILLA_PACKAGE_SPLIT)
    set(CPACK_COMPONENTS_ALL core tortilla tortilla_all ${tortilla_toolkits})
    foreach(fm IN LISTS tortilla_shells)
        list(APPEND CPACK_COMPONENTS_ALL tortilla-shell-${fm})
    endforeach()
    set(CPACK_COMPONENTS_GROUPING ALL_COMPONENTS_IN_ONE)
    set(CPACK_PACKAGE_DESCRIPTION
        "A fast, native git client, with every user interface (it uses the one that\n"
        "matches your desktop) and the context menu of Nautilus, Nemo, Caja, Thunar,\n"
        "Dolphin, PCManFM-Qt and PCManFM.")
    list(JOIN CPACK_PACKAGE_DESCRIPTION "" CPACK_PACKAGE_DESCRIPTION)
    # The split packages of 0.1.0-alpha, and Tortilla's before its rapport
    # was its own: all of them are in this one now.
    set(split tortilla-rapport tortilla-core tortilla-gtk3 tortilla-gtk4 tortilla-qt
        librapport librapport-dev)
    foreach(tk gtk3 gtk4 qt6)
        list(APPEND split tortilla-rapport-${tk} librapport-${tk})
    endforeach()
    foreach(fm nautilus nemo caja thunar dolphin pcmanfm)
        list(APPEND split tortilla-${fm})
    endforeach()

    # .deb: the app's and the core's libraries, as dpkg-shlibdeps finds them
    # (.github/ci/package.sh passes TORTILLA_DEB_DEPENDS); a backend's or an
    # extension's would make every toolkit and file manager a requirement.
    set(TORTILLA_DEB_DEPENDS "libc6, libstdc++6, libgcc-s1" CACHE STRING
        "The app's and rapport core's shared library dependencies, for the .deb")
    set(CPACK_DEB_COMPONENT_INSTALL ON)
    set(CPACK_DEBIAN_PACKAGE_NAME tortilla)
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    set(CPACK_DEBIAN_PACKAGE_SECTION vcs)
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
    set(CPACK_DEBIAN_PACKAGE_DEPENDS
        "${TORTILLA_DEB_DEPENDS}, git, git-lfs, libgtk-4-1 | libgtk-3-0t64 | libgtk-3-0 | libqt6widgets6t64 | libqt6widgets6")
    set(CPACK_DEBIAN_PACKAGE_RECOMMENDS
        "libcurl4t64 | libcurl4 | libcurl3t64-gnutls, libgdk-pixbuf-2.0-0, ifritview")
    set(CPACK_DEBIAN_PACKAGE_ENHANCES "nautilus, nemo, caja, thunar, dolphin, pcmanfm-qt, pcmanfm")
    list(JOIN split ", " split_deb)
    set(CPACK_DEBIAN_PACKAGE_REPLACES "${split_deb}")
    set(CPACK_DEBIAN_PACKAGE_BREAKS "${split_deb}")

    # .rpm: rpmbuild finds the libraries, but not in the backends' and the
    # extensions' folders, whose libraries are only there to be loaded.
    set(CPACK_RPM_COMPONENT_INSTALL ON)
    set(CPACK_RPM_PACKAGE_NAME tortilla)
    set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
    set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-only AND MIT")
    set(CPACK_RPM_PACKAGE_GROUP "Development/Tools")
    set(CPACK_RPM_PACKAGE_REQUIRES "git, git-lfs, (gtk4 or gtk3 or qt6-qtbase-gui)")
    set(CPACK_RPM_PACKAGE_SUGGESTS "libcurl, gdk-pixbuf2, ifritview")
    list(JOIN split ", " split_rpm)
    set(CPACK_RPM_PACKAGE_OBSOLETES "${split_rpm}")
    cmake_path(ABSOLUTE_PATH RAPPORT_INSTALL_LIBDIR BASE_DIRECTORY ${CMAKE_INSTALL_PREFIX}
               OUTPUT_VARIABLE rapport_libdir)
    set(loaded_dirs ${rapport_libdir}/rapport)
    foreach(var IN ITEMS RAPPORT_SHELL_DIR_nautilus RAPPORT_SHELL_DIR_nemo RAPPORT_SHELL_DIR_caja
                         RAPPORT_SHELL_DIR_thunar RAPPORT_SHELL_DIR_dolphin
                         RAPPORT_SHELL_DIR_dolphin_badges)
        if(DEFINED ${var})
            cmake_path(ABSOLUTE_PATH ${var} BASE_DIRECTORY ${CMAKE_INSTALL_PREFIX} OUTPUT_VARIABLE dir)
            list(APPEND loaded_dirs ${dir})
        endif()
    endforeach()
    list(JOIN loaded_dirs "|" loaded_dirs)
    # Our libraries are no one else's: none offered to other packages.
    set(CPACK_RPM_SPEC_MORE_DEFINE
        "%global __provides_exclude_from ^(${rapport_libdir}|${loaded_dirs})/.*$\n%global __requires_exclude_from ^(${loaded_dirs})/.*$\n%global __requires_exclude ^librapport[.]so.*$")
    include(CPack)
    return()
endif()

# -- Split packages -------------------------------------------------------------
# The null backend is for tests, and isn't installed; rapport's headers
# (its "dev" component) aren't for others to build against Tortilla's copy.
set(CPACK_COMPONENTS_ALL core tortilla tortilla_all)
foreach(tk IN LISTS tortilla_toolkits)
    list(APPEND CPACK_COMPONENTS_ALL ${tk} tortilla_${tk})
endforeach()
foreach(fm IN LISTS tortilla_shells)
    list(APPEND CPACK_COMPONENTS_ALL tortilla-shell-${fm})
endforeach()

# Component -> package name, and what it needs of ours.
set(pkg_core tortilla-rapport)
set(pkg_tortilla tortilla-core)
set(pkg_tortilla_all tortilla)
set(CPACK_COMPONENT_TORTILLA_DEPENDS core)
set(CPACK_COMPONENT_TORTILLA_ALL_DEPENDS tortilla ${tortilla_toolkits})
foreach(tk IN LISTS tortilla_toolkits)
    string(TOUPPER ${tk} TK)
    string(REPLACE qt6 qt app tortilla-${tk})
    set(pkg_${tk} tortilla-rapport-${tk})
    set(pkg_tortilla_${tk} ${app})
    set(CPACK_COMPONENT_${TK}_DEPENDS core)
    set(CPACK_COMPONENT_TORTILLA_${TK}_DEPENDS tortilla ${tk})
endforeach()
foreach(fm IN LISTS tortilla_shells)
    string(TOUPPER ${fm} FM)
    set(pkg_tortilla-shell-${fm} tortilla-${fm})
    set(CPACK_COMPONENT_TORTILLA-SHELL-${FM}_DEPENDS tortilla)
endforeach()

set(CPACK_COMPONENT_CORE_DESCRIPTION
    "Tortilla's copy of rapport, the toolkit-neutral UI library it is built on:\n"
    "the core detects the desktop and loads the backend for its toolkit.")
set(CPACK_COMPONENT_TORTILLA_DESCRIPTION
    "The Tortilla application, linked to no toolkit. Install tortilla-gtk4,\n"
    "tortilla-gtk3 or tortilla-qt for the one that matches your desktop, or\n"
    "tortilla for all of them.")
set(CPACK_COMPONENT_TORTILLA_ALL_DESCRIPTION
    "A fast, native git client, with every user interface: it uses the one\n"
    "that matches your desktop. Installs no toolkit; your desktop has one.")
foreach(tk IN LISTS tortilla_toolkits)
    string(TOUPPER ${tk} TK)
    set(CPACK_COMPONENT_${TK}_DESCRIPTION "The ${tk} backend of Tortilla's copy of rapport.")
    set(CPACK_COMPONENT_TORTILLA_${TK}_DESCRIPTION
        "A fast, native git client, with the ${tk} user interface.")
endforeach()
foreach(fm IN LISTS tortilla_shells)
    string(TOUPPER ${fm} FM)
    set(CPACK_COMPONENT_TORTILLA-SHELL-${FM}_DESCRIPTION
        "Tortilla's entries in the ${fm} file manager's context menu.")
endforeach()
set(CPACK_COMPONENT_TORTILLA-SHELL-PCMANFM_DESCRIPTION
    "Tortilla's entries in the context menus of PCManFM-Qt and PCManFM, as\n"
    "file manager actions: a fixed menu inside and outside a working tree. Caja\n"
    "reads them too if caja-actions is installed, beside tortilla-caja's own menu.")

# -- .deb -------------------------------------------------------------------
set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SECTION vcs)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
# librapport.so.0 is in our own tortilla-rapport package, which shlibdeps can't
# know of; the component dependencies below name it instead.
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS_PRIVATE_DIRS ${CMAKE_BINARY_DIR}/bin)
set(CPACK_DEBIAN_ENABLE_COMPONENT_DEPENDS ON)
foreach(c IN LISTS CPACK_COMPONENTS_ALL)
    string(TOUPPER ${c} C)
    set(CPACK_DEBIAN_${C}_PACKAGE_NAME ${pkg_${c}})
endforeach()
set(CPACK_DEBIAN_CORE_PACKAGE_SECTION libs)
# The core loads these when first asked (HTTP, decoding images), not at start.
set(CPACK_DEBIAN_CORE_PACKAGE_RECOMMENDS
    "libcurl4t64 | libcurl4 | libcurl3t64-gnutls, libgdk-pixbuf-2.0-0")
# The app shells out to git for everything but the revision walk, and git
# needs git-lfs for a repository that uses Git LFS.
set(CPACK_DEBIAN_TORTILLA_PACKAGE_DEPENDS "git, git-lfs")
list(TRANSFORM tortilla_shells PREPEND tortilla- OUTPUT_VARIABLE shell_packages)
list(JOIN shell_packages ", " shell_packages)
set(CPACK_DEBIAN_TORTILLA_PACKAGE_SUGGESTS "${shell_packages}")
foreach(fm IN LISTS tortilla_shells)
    string(TOUPPER ${fm} FM)
    set(CPACK_DEBIAN_TORTILLA-SHELL-${FM}_PACKAGE_DEPENDS ${fm_deb_${fm}})
    set(CPACK_DEBIAN_TORTILLA-SHELL-${FM}_PACKAGE_ENHANCES ${fm_deb_${fm}})
endforeach()
set(CPACK_DEBIAN_TORTILLA-SHELL-PCMANFM_PACKAGE_ENHANCES "pcmanfm-qt, pcmanfm")
# Tortilla's packages before its rapport was its own (librapport,
# librapport-<toolkit>, librapport-dev) go on upgrade, so their old
# backends in <libdir>/rapport can't be found first.
set(CPACK_DEBIAN_CORE_PACKAGE_REPLACES "librapport, librapport-dev")
set(CPACK_DEBIAN_CORE_PACKAGE_BREAKS "librapport, librapport-dev")
set(CPACK_RPM_CORE_PACKAGE_OBSOLETES "librapport, librapport-dev")
foreach(tk IN LISTS tortilla_toolkits)
    string(TOUPPER ${tk} TK)
    set(CPACK_DEBIAN_${TK}_PACKAGE_REPLACES librapport-${tk})
    set(CPACK_DEBIAN_${TK}_PACKAGE_BREAKS librapport-${tk})
    set(CPACK_RPM_${TK}_PACKAGE_OBSOLETES librapport-${tk})
endforeach()
# The backends name no toolkit (shlibdeps would), only what every system
# has; each tortilla-<toolkit> names its own toolkit instead.
# Images in a comparison open in IfritView (Settings > Diff Viewer): each
# toolkit's package recommends it. One package for now; each toolkit's own
# (set here) once IfritView splits like Tortilla.
foreach(tk IN LISTS tortilla_toolkits)
    string(TOUPPER ${tk} TK)
    set(CPACK_DEBIAN_${TK}_PACKAGE_SHLIBDEPS OFF)
    set(CPACK_DEBIAN_${TK}_PACKAGE_DEPENDS "libc6, libstdc++6, libgcc-s1")
    set(CPACK_DEBIAN_TORTILLA_${TK}_PACKAGE_DEPENDS "${toolkit_deb_${tk}}")
    set(ifritview_for_${tk} ifritview)
    set(CPACK_DEBIAN_TORTILLA_${TK}_PACKAGE_RECOMMENDS ${ifritview_for_${tk}})
endforeach()
set(CPACK_DEBIAN_TORTILLA_ALL_PACKAGE_RECOMMENDS ifritview)
set(CPACK_DEBIAN_TORTILLA-SHELL-PCMANFM_PACKAGE_ARCHITECTURE all)

# -- .rpm -------------------------------------------------------------------
set(CPACK_RPM_COMPONENT_INSTALL ON)
set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-only AND MIT")
set(CPACK_RPM_PACKAGE_GROUP "Development/Tools")
# rpmbuild's own requires cover the shared libraries but librapport.so.0
# (below); these are the rest.
foreach(c IN LISTS CPACK_COMPONENTS_ALL)
    string(TOUPPER ${c} C)
    set(CPACK_RPM_${C}_PACKAGE_NAME ${pkg_${c}})
    set(requires)
    foreach(dep IN LISTS CPACK_COMPONENT_${C}_DEPENDS)
        list(APPEND requires "${pkg_${dep}} = ${TORTILLA_PACKAGE_VERSION}")
    endforeach()
    if(c STREQUAL tortilla)
        list(APPEND requires git git-lfs)
    elseif(c MATCHES "^tortilla-shell-(.*)")
        list(APPEND requires ${fm_rpm_${CMAKE_MATCH_1}})
    elseif(c IN_LIST tortilla_toolkits)
        # As on Debian: no toolkit, so rpmbuild's own requires are off.
        set(CPACK_RPM_${C}_PACKAGE_AUTOREQ OFF)
        list(APPEND requires glibc libstdc++ libgcc)
    elseif(c MATCHES "^tortilla_(.*)" AND CMAKE_MATCH_1 IN_LIST tortilla_toolkits)
        list(APPEND requires ${toolkit_rpm_${CMAKE_MATCH_1}})
    endif()
    list(JOIN requires ", " requires)
    set(CPACK_RPM_${C}_PACKAGE_REQUIRES "${requires}")
endforeach()
set(CPACK_RPM_CORE_PACKAGE_SUGGESTS "libcurl, gdk-pixbuf2")
# The private librapport.so.0 is no one else's: not offered to other
# packages, and not looked for (tortilla-core requires tortilla-rapport).
cmake_path(ABSOLUTE_PATH RAPPORT_INSTALL_LIBDIR BASE_DIRECTORY ${CMAKE_INSTALL_PREFIX}
           OUTPUT_VARIABLE rapport_libdir)
set(CPACK_RPM_SPEC_MORE_DEFINE
    "%global __provides_exclude_from ^${rapport_libdir}/.*$\n%global __requires_exclude ^librapport[.]so.*$")
set(CPACK_RPM_TORTILLA_PACKAGE_SUGGESTS "${shell_packages}")
foreach(tk IN LISTS tortilla_toolkits)
    string(TOUPPER ${tk} TK)
    set(CPACK_RPM_TORTILLA_${TK}_PACKAGE_SUGGESTS ${ifritview_for_${tk}})
endforeach()
set(CPACK_RPM_TORTILLA_ALL_PACKAGE_SUGGESTS ifritview)
include(CPack)
