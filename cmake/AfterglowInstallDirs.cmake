# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Where Afterglow installs and reads its files. Every Phosphor app keeps them
# under the suite folder, which phosphor-libs names once
# (PHOSPHOR_SUITE_DIRECTORY_NAME, cmake/PhosphorSuite.cmake there):
#
#   lib/<suite>/afterglow/            the private libraries and plugins
#   share/<suite>/afterglow/          themes, shell integration, the script API
#   share/<suite>/shaders/terminal/   the terminal shader packs
#
# The C++ code reads the same two names below through compile definitions, so
# that neither is written twice. cmake/AfterglowPhosphor.cmake includes this
# file once it has the phosphor-libs tree.

# Afterglow's own folder inside the suite folder, in each XDG root.
set(AFTERGLOW_APP_DIRECTORY_NAME "afterglow")
# The tree of the terminal shader packs, shared by any terminal that reads
# them, relative to the suite folder.
set(AFTERGLOW_SHADER_TREE "shaders/terminal")

set(AFTERGLOW_DATA_SUBDIR "${PHOSPHOR_SUITE_DIRECTORY_NAME}/${AFTERGLOW_APP_DIRECTORY_NAME}")
set(AFTERGLOW_SHADER_SUBDIR "${PHOSPHOR_SUITE_DIRECTORY_NAME}/${AFTERGLOW_SHADER_TREE}")

# Installed layout: the executables in bin/; afterglow's libraries, its own
# copies of the phosphor libraries and their layer-shell plugin in a private
# lib/<suite>/afterglow/, so a system install of phosphor is never touched.
# afterglow and aglow-mux find that directory and no other run path (aglow is
# static); the libraries in it and the plugin set their own run paths where
# they are installed (afterglow/CMakeLists.txt, and mux/CMakeLists.txt for
# aglow-mux alone).
set(AFTERGLOW_PRIVATE_LIBDIR "${CMAKE_INSTALL_LIBDIR}/${AFTERGLOW_DATA_SUBDIR}")
# The path from the executables to it, also for an absolute LIBDIR or a
# BINDIR deeper than bin/.
file(RELATIVE_PATH AFTERGLOW_PRIVATE_LIBDIR_FROM_BIN "${CMAKE_INSTALL_FULL_BINDIR}"
     "${CMAKE_INSTALL_FULL_LIBDIR}/${AFTERGLOW_DATA_SUBDIR}")
set(AFTERGLOW_INSTALL_RPATH "$ORIGIN/${AFTERGLOW_PRIVATE_LIBDIR_FROM_BIN}")
# qt_standard_project_setup() appends $ORIGIN and $ORIGIN/../lib to the run
# path: the executables would then load a library from bin/ or from the
# shared lib/ of the prefix (a Qt in ~/.local/lib, say) before the Qt of the
# build. They search only the private directory. Set here, after that call
# and before any target.
set(CMAKE_INSTALL_RPATH "${AFTERGLOW_INSTALL_RPATH}")
