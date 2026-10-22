# Workspace: the rapport library and tortilla side by side.
#
# Packaging (see DESIGN.md): users install the package for their toolkit,
# which pulls in the rest:
#   tortilla-qt   -> tortilla-core + librapport-qt6  -> librapport
#   tortilla-gtk3 -> tortilla-core + librapport-gtk3 -> librapport
cmake_minimum_required(VERSION 3.25)
project(tortilla-workspace LANGUAGES CXX)

# Executables and backend plugins land side by side so the loader finds
# them next to the executable without installing.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_subdirectory(rapport)
add_subdirectory(tortilla)
