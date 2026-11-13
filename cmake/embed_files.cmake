# SPDX-FileCopyrightText: 2026 Flay Krunegan
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Turns files into one C++ header of string constants, so the binaries carry
# them: glsl/internal/ (all GLSL Prisma puts into pack shaders lives under
# glsl/) and the Luanti baseline (src/common/luanti/). The
# "// SPDX-" lines are left out: a shader must start with #version. The header
# is only rewritten when it changes.
#
#   cmake -DROOT=<folder> -DOUT=<header> -DFILES=<a|b|...> -P embed_files.cmake
#
# Each file becomes prisma::embedded::<path as a C identifier>, e.g.
# lighting/caster.vert -> lighting_caster_vert.

string(REPLACE "|" ";" FILES "${FILES}") # the list arrives with | between the names
set(text "// Generated from ${ROOT} by cmake/embed_files.cmake. Do not edit.\n#pragma once\n\nnamespace prisma::embedded {\n\n")
foreach(name IN LISTS FILES)
	file(READ "${ROOT}/${name}" content)
	string(REPLACE "\r" "" content "${content}")
	string(REGEX REPLACE "//[ ]*SPDX-[A-Za-z-]+:[^\n]*\n" "" content "${content}")
	string(FIND "${content}" ")prisma_glsl\"" clash)
	if(NOT clash EQUAL -1)
		message(FATAL_ERROR "${name} contains the raw string delimiter )prisma_glsl\"")
	endif()
	string(MAKE_C_IDENTIFIER "${name}" id)
	string(APPEND text "inline constexpr const char ${id}[] = R\"prisma_glsl(${content})prisma_glsl\";\n\n")
endforeach()
string(APPEND text "} // namespace prisma::embedded\n")

file(WRITE "${OUT}.tmp" "${text}")
execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different "${OUT}.tmp" "${OUT}")
file(REMOVE "${OUT}.tmp")
