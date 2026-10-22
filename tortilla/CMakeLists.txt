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
    src/git/diff_tool.cpp)
target_include_directories(tortilla_git PUBLIC src)
target_link_libraries(tortilla_git PUBLIC ${TORTILLA_LIBGIT2} rapport::rapport)
target_compile_features(tortilla_git PUBLIC cxx_std_23)

add_executable(tortilla
    src/main.cpp
    src/log/log_window.cpp
    src/log/log_actions.cpp
    src/log/log_file_actions.cpp
    src/rebase/rebase_window.cpp
    src/ui/progress_dialog.cpp
    src/ui/diff_window.cpp
    src/ui/settings_dialog.cpp)
target_link_libraries(tortilla PRIVATE rapport::rapport tortilla_git)
if(WIN32)
    enable_language(RC)
    target_sources(tortilla PRIVATE data/windows/tortilla.rc)
    set_target_properties(tortilla PROPERTIES WIN32_EXECUTABLE ON)  # no console window
endif()

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
