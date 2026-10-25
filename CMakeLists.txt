# Tortilla, and the rapport library it's built on (the rapport/ submodule).
#
# Packaging (see DESIGN.md): users install the package for their toolkit,
# which pulls in the rest:
#   tortilla-qt   -> tortilla-core + librapport-qt6  -> librapport
#   tortilla-gtk3 -> tortilla-core + librapport-gtk3 -> librapport
cmake_minimum_required(VERSION 3.25)
project(tortilla VERSION 0.1 LANGUAGES CXX)

# Executables and backend plugins land side by side so the loader finds
# them next to the executable without installing.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Windows builds are self-contained: rapport and its Win32 backend linked
# in, and with MSVC the static C runtime, so tortilla.exe needs no DLLs of
# ours (tortilla-shell.dll is separate only because Explorer loads it).
if(WIN32 AND NOT DEFINED RAPPORT_STATIC)
    set(RAPPORT_STATIC ON CACHE BOOL "Link rapport and its backends into tortilla")
endif()
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_subdirectory(rapport)

# Package: tortilla-core (the app; links no toolkit).
# The user-facing packages tortilla-gtk3 / tortilla-qt / ... depend on this and
# on the matching librapport backend.
# libgit2: pkg-config where there is one (distro packages; Debian's CMake
# package drags in its private dependencies, e.g. Kerberos, as link
# requirements), else libgit2's own CMake package (the Windows cross-build).
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND AND NOT WIN32)
    pkg_check_modules(LIBGIT2 QUIET IMPORTED_TARGET libgit2)
endif()
if(NOT TARGET PkgConfig::LIBGIT2)
    find_package(libgit2 CONFIG QUIET)
endif()
if(TARGET PkgConfig::LIBGIT2)
    set(TORTILLA_LIBGIT2 PkgConfig::LIBGIT2)
elseif(TARGET libgit2::libgit2package)
    set(TORTILLA_LIBGIT2 libgit2::libgit2package)
else()
    message(FATAL_ERROR "libgit2 not found (pkg-config libgit2, or libgit2_DIR)")
endif()

add_library(tortilla_git STATIC
    src/git/repo.cpp
    src/graph/lanes.cpp
    src/log/log_model.cpp
    src/log/ref_labels.cpp
    src/settings.cpp
    src/rebase/rebase_model.cpp
    src/rebase/rebase_engine.cpp
    src/git/command.cpp
    src/git/status.cpp
    src/git/repo_worker.cpp
    src/git/diff_tool.cpp
    src/git/ssh_client.cpp
    src/git/askpass_env.cpp
    src/git/remotes.cpp
    src/git/merge_tool.cpp
    src/util/programs.cpp
    src/util/sha256.cpp
    src/git/ignore_patterns.cpp
    src/git/ref_list.cpp
    src/changes/working_tree.cpp)
target_include_directories(tortilla_git PUBLIC src)
target_link_libraries(tortilla_git PUBLIC ${TORTILLA_LIBGIT2} rapport::rapport)
target_compile_features(tortilla_git PUBLIC cxx_std_23)

add_executable(tortilla
    src/main.cpp
    src/log/log_window.cpp
    src/log/log_actions.cpp
    src/changes/changes_view.cpp
    src/changes/changes_window.cpp
    src/changes/commit_window.cpp
    src/changes/file_action_window.cpp
    src/changes/conflict_editor.cpp
    src/ui/stash_dialog.cpp
    src/rebase/rebase_window.cpp
    src/ui/compare.cpp
    src/ui/progress_dialog.cpp
    src/ui/diff_window.cpp
    src/ui/settings_dialog.cpp
    src/shell/menu_icons.cpp
    src/util/gravatar.cpp
    src/ui/welcome_window.cpp
    src/changes/remove.cpp
    src/changes/ignore_dialog.cpp
    src/i18n.cpp
    src/ui/askpass.cpp
    src/ui/windows.cpp
    src/remote/pull_fetch_dialog.cpp
    src/remote/push_dialog.cpp
    src/remote/clone_dialog.cpp
    src/branch/ref_chooser.cpp
    src/branch/create_ref_dialog.cpp
    src/branch/switch_dialog.cpp
    src/branch/merge_dialog.cpp
    src/branch/browse_refs.cpp
    src/ui/create_repo_dialog.cpp)
target_link_libraries(tortilla PRIVATE rapport::rapport tortilla_git)
if(WIN32)
    enable_language(RC)
    target_sources(tortilla PRIVATE data/windows/tortilla.rc)
    set_target_properties(tortilla PROPERTIES WIN32_EXECUTABLE ON)  # no console window
    if(MSVC)
        # main(), not WinMain; and tortilla.rc carries the manifest.
        target_link_options(tortilla PRIVATE /ENTRY:mainCRTStartup /MANIFEST:NO)
    endif()
endif()

# A static rapport registers the backends linked in, from a generated source
# (a registration inside a static library would be dropped by the linker).
if(RAPPORT_STATIC)
    set(builtins "#include <rapport/rapport.h>\n")
    foreach(backend win32 qt6 gtk4 gtk3)
        if(TARGET rapport::backend_${backend})
            string(APPEND builtins "RAPPORT_BUILTIN_BACKEND(${backend});\n")
            target_link_libraries(tortilla PRIVATE rapport::backend_${backend})
        endif()
    endforeach()
    file(GENERATE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/builtin_backends.cpp CONTENT "${builtins}")
    target_sources(tortilla PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/builtin_backends.cpp)
endif()

# The file manager's context menu: TortoiseGit's Explorer menu, launching
# tortilla [spec 4]. On Windows built beside tortilla, which registers it
# (tortilla --register-shell); on Linux one extension per file manager, each
# its own install component (tortilla-shell-nautilus, ...), which packages
# put in the file manager's extension directory.
rapport_add_shell_extension(tortilla-shell
    NAME "Tortilla" ID org.tortilla.Tortilla
    WINDOWS_CLSID {D73E7C0C-F88B-4DD5-AC56-9AABAB52E4EB}
    SOURCES src/shell/shell_provider.cpp)
if(UNIX AND NOT APPLE)
    include(GNUInstallDirs)
    target_compile_definitions(tortilla-shell_provider PRIVATE
        TORTILLA_INSTALL_BINDIR="${CMAKE_INSTALL_FULL_BINDIR}")
endif()
# Its icons, beside it as installed (tools/make_menu_icons.cpp draws them).
add_custom_command(TARGET tortilla-shell-probe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/data/windows/tortilla.ico
            $<TARGET_FILE_DIR:tortilla-shell-probe>/tortilla.ico
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/data/windows/menu
            $<TARGET_FILE_DIR:tortilla-shell-probe>/menu)
target_link_libraries(tortilla PRIVATE rapport::shell)

# The welcome window's mascot, built in: nothing to install or lose.
set(TORTILLA_MASCOT_PNG ${CMAKE_CURRENT_SOURCE_DIR}/data/icons/hicolor/256x256/apps/tortilla.png)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${TORTILLA_MASCOT_PNG})
file(READ ${TORTILLA_MASCOT_PNG} TORTILLA_MASCOT HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," TORTILLA_MASCOT "${TORTILLA_MASCOT}")
file(GENERATE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/mascot.cpp CONTENT
     "#include <cstddef>\n#include <cstdint>\nnamespace tortilla\n{\nextern const uint8_t k_mascot_png[] = {${TORTILLA_MASCOT}};\nextern const size_t k_mascot_png_size = sizeof k_mascot_png;\n}\n")
target_sources(tortilla PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/mascot.cpp)
target_compile_definitions(tortilla PRIVATE TORTILLA_VERSION="${PROJECT_VERSION}")

# Translations: Tortilla's and rapport's .po files, read as they are
# (data/translations; rapport/i18n). Copied beside the build's programs on
# every build (a changed translation relinks nothing), so a built tortilla
# finds them as an installed one does.
file(GLOB TORTILLA_TRANSLATIONS CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/data/translations/*.po ${CMAKE_CURRENT_SOURCE_DIR}/rapport/i18n/*.po)
if(TORTILLA_TRANSLATIONS)
    add_custom_target(tortilla-translations ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:tortilla>/translations
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${TORTILLA_TRANSLATIONS}
                $<TARGET_FILE_DIR:tortilla>/translations
        COMMAND_EXPAND_LISTS)
    add_dependencies(tortilla-translations tortilla)
endif()

if(UNIX AND NOT APPLE)
    include(GNUInstallDirs)
    install(TARGETS tortilla RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT tortilla)
    install(DIRECTORY data/icons/hicolor DESTINATION ${CMAKE_INSTALL_DATADIR}/icons
            COMPONENT tortilla)
    # The file managers' menu icons, from the icon theme.
    install(DIRECTORY data/linux/icons/hicolor DESTINATION ${CMAKE_INSTALL_DATADIR}/icons
            COMPONENT tortilla)
    install(FILES ${TORTILLA_TRANSLATIONS} DESTINATION ${CMAKE_INSTALL_DATADIR}/tortilla/translations
            COMPONENT tortilla)
    # The welcome window's Licence and Third-party notices links open these.
    install(FILES LICENSE NOTICES.md DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/tortilla
            COMPONENT tortilla)
    install(FILES data/linux/tortilla.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
            COMPONENT tortilla)
    include(cmake/packaging.cmake)
endif()

add_executable(tortilla-graphdump tools/graphdump.cpp)
target_link_libraries(tortilla-graphdump PRIVATE tortilla_git)
if(NOT MSVC)
    target_compile_options(tortilla_git PUBLIC -Wno-missing-field-initializers)
endif()

add_executable(tortilla-tests tests/log_model_test.cpp)
target_link_libraries(tortilla-tests PRIVATE tortilla_git rapport::rapport)

add_executable(tortilla-icon-preview tools/icon_preview.cpp)
target_link_libraries(tortilla-icon-preview PRIVATE tortilla_git rapport::rapport)

add_executable(tortilla-rebase-tests tests/rebase_test.cpp)
target_link_libraries(tortilla-rebase-tests PRIVATE tortilla_git rapport::rapport)

# The tests run on the headless null backend: linked in, in a static build.
if(RAPPORT_STATIC AND TARGET rapport::backend_null)
    file(GENERATE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/null_backend.cpp
         CONTENT "#include <rapport/rapport.h>\nRAPPORT_BUILTIN_BACKEND(null);\n")
    foreach(test tortilla-tests tortilla-rebase-tests)
        target_sources(${test} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/null_backend.cpp)
        target_link_libraries(${test} PRIVATE rapport::backend_null)
    endforeach()
endif()
