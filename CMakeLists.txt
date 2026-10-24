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
    src/ui/stash_dialog.cpp
    src/rebase/rebase_window.cpp
    src/ui/compare.cpp
    src/ui/progress_dialog.cpp
    src/ui/diff_window.cpp
    src/ui/settings_dialog.cpp)
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
# tortilla [spec 4]. Built beside tortilla, which registers it
# (tortilla --register-shell).
rapport_add_shell_extension(tortilla-shell
    NAME "Tortilla" ID org.tortilla.Tortilla
    WINDOWS_CLSID {D73E7C0C-F88B-4DD5-AC56-9AABAB52E4EB}
    SOURCES src/shell/shell_provider.cpp)
add_custom_command(TARGET tortilla-shell-probe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/data/windows/tortilla.ico
            $<TARGET_FILE_DIR:tortilla-shell-probe>/tortilla.ico)
target_link_libraries(tortilla PRIVATE rapport::shell)

if(UNIX AND NOT APPLE)
    include(GNUInstallDirs)
    install(TARGETS tortilla RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(DIRECTORY data/icons/hicolor DESTINATION ${CMAKE_INSTALL_DATADIR}/icons)
    install(FILES data/linux/tortilla.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
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
