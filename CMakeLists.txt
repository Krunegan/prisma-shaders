# SPDX-FileCopyrightText: 2026 Flay Krunegan
# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
if(WIN32)
	project(prisma LANGUAGES C CXX RC)
else()
	project(prisma LANGUAGES C CXX)
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
	message(FATAL_ERROR "Prisma must be built for x86_64 (Luanti for Windows is 64-bit)")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()

# Project headers are included by their path from src/ ("core/pack/pack.hpp")
include_directories(${CMAKE_SOURCE_DIR}/src)

# Prisma's own GLSL (glsl/internal/, MIT) becomes glsl_embedded.hpp, a header
# of string constants generated at build time (cmake/embed_files.cmake).
set(PRISMA_INTERNAL_GLSL api.glsl pass_header.glsl shadows.glsl pass.vert compute.glsl editor_stub.glsl
	lighting/caster.vert lighting/caster.frag lighting/gbuffer.vert lighting/gbuffer.frag)
set(PRISMA_GLSL_HEADER ${CMAKE_BINARY_DIR}/generated/glsl_embedded.hpp)
list(TRANSFORM PRISMA_INTERNAL_GLSL PREPEND ${CMAKE_SOURCE_DIR}/glsl/internal/ OUTPUT_VARIABLE PRISMA_INTERNAL_GLSL_PATHS)
string(REPLACE ";" "|" PRISMA_INTERNAL_GLSL_ARG "${PRISMA_INTERNAL_GLSL}")
add_custom_command(OUTPUT ${PRISMA_GLSL_HEADER}
	COMMAND ${CMAKE_COMMAND} -DROOT=${CMAKE_SOURCE_DIR}/glsl/internal -DOUT=${PRISMA_GLSL_HEADER}
		-DFILES=${PRISMA_INTERNAL_GLSL_ARG} -P ${CMAKE_SOURCE_DIR}/cmake/embed_files.cmake
	DEPENDS ${PRISMA_INTERNAL_GLSL_PATHS} ${CMAKE_SOURCE_DIR}/cmake/embed_files.cmake
	COMMENT "Embedding glsl/internal"
	VERBATIM)
# What Prisma relies on in the supported Luanti version: read by
# prisma_check --luanti and by the core's report in the game
set(PRISMA_LUANTI_VERSION 5.17.0)
set(PRISMA_BASELINE_HEADER ${CMAKE_BINARY_DIR}/generated/baseline_embedded.hpp)
add_compile_definitions(PRISMA_LUANTI_VERSION="${PRISMA_LUANTI_VERSION}")
add_custom_command(OUTPUT ${PRISMA_BASELINE_HEADER}
	COMMAND ${CMAKE_COMMAND} -DROOT=${CMAKE_SOURCE_DIR}/src/common/luanti/${PRISMA_LUANTI_VERSION}
		-DOUT=${PRISMA_BASELINE_HEADER} -DFILES=baseline.toml -P ${CMAKE_SOURCE_DIR}/cmake/embed_files.cmake
	DEPENDS ${CMAKE_SOURCE_DIR}/src/common/luanti/${PRISMA_LUANTI_VERSION}/baseline.toml
		${CMAKE_SOURCE_DIR}/cmake/embed_files.cmake
	COMMENT "Embedding the Luanti ${PRISMA_LUANTI_VERSION} baseline"
	VERBATIM)
include_directories(${CMAKE_BINARY_DIR}/generated)

# The launcher and the core library must sit side by side.
set(PRISMA_DIST_DIR ${CMAKE_BINARY_DIR}/dist)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PRISMA_DIST_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PRISMA_DIST_DIR})

if(WIN32)
	# Everything is linked statically: the core lives inside luanti.exe, which
	# ships its own libc++.dll, and we must not depend on (or clash with) it.
	add_link_options(-static)
else()
	# The core is preloaded into Luanti: nothing of it may be visible to the
	# game but the symbols it intercepts (see platform_linux.cpp).
	set(CMAKE_POSITION_INDEPENDENT_CODE ON)
	set(CMAKE_CXX_VISIBILITY_PRESET hidden)
	set(CMAKE_C_VISIBILITY_PRESET hidden)
	set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
endif()

# Dear ImGui (in-game menu) with its OpenGL 3 renderer
add_library(imgui STATIC
	third_party/imgui/imgui.cpp
	third_party/imgui/imgui_draw.cpp
	third_party/imgui/imgui_tables.cpp
	third_party/imgui/imgui_widgets.cpp
	third_party/imgui/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC third_party/imgui)
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS)

# miniaudio (the packs' sounds), with stb_vorbis for .ogg files. Packs are
# written by strangers, so only the decoders packs may use are built in: WAV
# and Vorbis. miniaudio picks the decoder by the
# file's contents, not its name, so the others must not exist at all.
add_library(miniaudio STATIC src/core/audio/miniaudio_impl.c)
target_include_directories(miniaudio PUBLIC third_party/miniaudio)
target_compile_definitions(miniaudio PUBLIC MA_NO_ENCODING MA_NO_MP3 MA_NO_FLAC)
target_compile_options(miniaudio PRIVATE -Wno-tautological-compare) # in stb_vorbis

set(PRISMA_CORE_SOURCES
	src/core/core.cpp
	${PRISMA_GLSL_HEADER}
	${PRISMA_BASELINE_HEADER}
	src/common/luanti_baseline.cpp
	src/common/ini.cpp
	src/common/log.cpp
	src/common/toml.cpp
	src/common/ui_theme.cpp
	src/common/util.cpp
	src/core/audio/audio.cpp
	src/core/dev/developer.cpp
	src/core/dev/hookcost.cpp
	src/core/dev/trace.cpp
	src/core/hooks/glapi.cpp
	src/core/hooks/hooks.cpp
	src/core/hooks/sdl.cpp
	src/core/pack/api.cpp
	src/core/pack/options.cpp
	src/core/pack/pack.cpp
	src/core/pack/passglsl.cpp
	src/core/render/frame.cpp
	src/core/render/gameshaders.cpp
	src/core/render/geometry.cpp
	src/core/render/lighting.cpp
	src/core/render/pipeline.cpp
	src/core/render/screen.cpp
	src/core/render/shader_compose.cpp
	src/core/tracking/compat.cpp
	src/core/tracking/shader_catalog.cpp
	src/core/tracking/tracker.cpp
	src/core/tracking/state.cpp
	src/core/ui/debugview.cpp
	src/core/ui/input.cpp
	src/core/ui/overlay.cpp
)

# The launcher's code common to both systems: configuration, packs and the
# configuration window (Dear ImGui)
set(PRISMA_LAUNCHER_SOURCES
	src/launcher/downloads.cpp
	src/launcher/image.cpp
	src/launcher/launcher.cpp
	src/launcher/luanticonf.cpp
	src/launcher/ui.cpp
	src/common/ini.cpp
	src/common/sha256.cpp
	src/common/toml.cpp
	src/common/ui_theme.cpp
	src/common/util.cpp
)

# What prisma_check needs of the core to build passes as the game does
set(PRISMA_CHECK_SOURCES
	src/check/check.cpp
	src/check/luanti_check.cpp
	${PRISMA_GLSL_HEADER}
	${PRISMA_BASELINE_HEADER}
	src/common/luanti_baseline.cpp
	src/common/sha256.cpp
	src/common/ini.cpp
	src/common/log.cpp
	src/common/toml.cpp
	src/common/util.cpp
	src/core/hooks/glapi.cpp
	src/core/pack/api.cpp
	src/core/pack/options.cpp
	src/core/pack/pack.cpp
	src/core/pack/passglsl.cpp
	src/core/ui/debugview.cpp
)

if(WIN32)
	# prisma_core.dll, loaded into luanti.exe by prisma.exe
	add_library(prisma_core SHARED ${PRISMA_CORE_SOURCES} src/core/platform/platform_win32.cpp)
	set_target_properties(prisma_core PROPERTIES PREFIX "")
	target_compile_definitions(prisma_core PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
	target_link_libraries(prisma_core PRIVATE imgui miniaudio version shell32)

	# The launcher: a window program (configuration window in Win32 + OpenGL)
	add_library(imgui_win32 STATIC third_party/imgui/backends/imgui_impl_win32.cpp)
	target_link_libraries(imgui_win32 PUBLIC imgui)
	add_executable(prisma WIN32
		${PRISMA_LAUNCHER_SOURCES}
		src/launcher/http_win32.cpp
		src/launcher/image_win32.cpp
		src/launcher/main_windows.cpp
		src/launcher/window_win32.cpp
		src/launcher/prisma.rc
	)
	target_compile_definitions(prisma PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00)
	target_link_options(prisma PRIVATE -municode)
	target_link_libraries(prisma PRIVATE imgui_win32 opengl32 gdi32 dwmapi imm32 comdlg32 ole32 uuid shell32 version winhttp windowscodecs)

	# Compiles a pack's passes outside the game (same headers, same driver).
	add_executable(prisma_check ${PRISMA_CHECK_SOURCES})
	target_compile_definitions(prisma_check PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
	target_link_options(prisma_check PRIVATE -municode)
	target_link_libraries(prisma_check PRIVATE opengl32 gdi32 version)
else()
	find_package(PkgConfig REQUIRED)
	pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)

	# libprisma_core.so, preloaded into Luanti by the launcher (LD_PRELOAD).
	# It links neither SDL nor OpenGL: it uses the game's, through dlsym.
	add_library(prisma_core SHARED ${PRISMA_CORE_SOURCES} src/core/platform/platform_linux.cpp)
	target_link_libraries(prisma_core PRIVATE imgui miniaudio dl pthread)
	# Nor the C++ library's templates it instantiates: only what it intercepts
	target_link_options(prisma_core PRIVATE -Wl,--version-script=${CMAKE_SOURCE_DIR}/src/core/platform/prisma_core.map
		-Wl,--no-undefined)
	set_property(TARGET prisma_core APPEND PROPERTY LINK_DEPENDS ${CMAKE_SOURCE_DIR}/src/core/platform/prisma_core.map)

	# The launcher, with its configuration window in SDL2 + ImGui
	add_library(imgui_sdl STATIC third_party/imgui/backends/imgui_impl_sdl2.cpp)
	target_link_libraries(imgui_sdl PUBLIC imgui PkgConfig::SDL2)
	add_executable(prisma
		${PRISMA_LAUNCHER_SOURCES}
		src/launcher/http_linux.cpp
		src/launcher/image_linux.cpp
		src/launcher/main_linux.cpp
		src/launcher/window_sdl.cpp
	)
	find_package(OpenGL REQUIRED)
	target_link_libraries(prisma PRIVATE imgui_sdl OpenGL::GL dl)

	add_executable(prisma_check ${PRISMA_CHECK_SOURCES})
	target_link_libraries(prisma_check PRIVATE PkgConfig::SDL2 dl)
endif()

# Ship the bundled packs, pack-author reference, standard GLSL library
# (#include <prisma/...>), and license texts next to the binaries: the folder
# is what players get. It runs on every build, also when only a pack changed
# (as a step of prisma_core it ran only when the core was linked again, and
# the packs in the folder fell behind), and each directory is rebuilt from
# source so removed files and packs do not linger.
add_custom_target(prisma_dist ALL
	COMMAND ${CMAKE_COMMAND} -E make_directory ${PRISMA_DIST_DIR}
	COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/LICENSE ${PRISMA_DIST_DIR}/LICENSE
	COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/packaging/THIRD_PARTY_NOTICES.txt ${PRISMA_DIST_DIR}/THIRD_PARTY_NOTICES.txt
	COMMAND ${CMAKE_COMMAND} -E rm -rf ${PRISMA_DIST_DIR}/LICENSES
	COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/LICENSES ${PRISMA_DIST_DIR}/LICENSES
	COMMAND ${CMAKE_COMMAND} -E make_directory ${PRISMA_DIST_DIR}/shaderpacks
	COMMAND ${CMAKE_COMMAND} -E rm -rf ${PRISMA_DIST_DIR}/docs
	COMMAND ${CMAKE_COMMAND} -E make_directory ${PRISMA_DIST_DIR}/docs
	COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/docs/usage.md ${CMAKE_SOURCE_DIR}/docs/how-it-works.md
		${CMAKE_SOURCE_DIR}/docs/shaderpacks.md ${PRISMA_DIST_DIR}/docs/
	COMMAND ${CMAKE_COMMAND} -E rm -rf ${PRISMA_DIST_DIR}/glsl
	COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/glsl/prisma ${PRISMA_DIST_DIR}/glsl/prisma
	COMMENT "Copying the GLSL library, docs and licenses to ${PRISMA_DIST_DIR}"
	VERBATIM)

# ---------------------------------------------------------------- tests
# ctest runs the unit tests (tests/unit/, no GPU needed) and prisma_check on
# Prisma's internal shaders and every pack in tests/packs/ (these need
# an OpenGL driver). Only in the development repository: the public one has
# no tests/, and builds without them.
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/unit)
	enable_testing()
	add_executable(prisma_tests
		tests/unit/main.cpp
		tests/unit/test_compose.cpp
		tests/unit/test_docs.cpp
		tests/unit/test_downloads.cpp
		tests/unit/test_image.cpp
		tests/unit/test_ini.cpp
		tests/unit/test_launcher.cpp
		tests/unit/test_luanti.cpp
		tests/unit/test_options.cpp
		tests/unit/test_pack.cpp
		tests/unit/test_toml.cpp
		${PRISMA_GLSL_HEADER}
		${PRISMA_BASELINE_HEADER}
		src/common/luanti_baseline.cpp
		src/check/luanti_check.cpp
		src/common/sha256.cpp
		src/common/ini.cpp
		src/common/log.cpp
		src/common/toml.cpp
		src/common/util.cpp
		src/core/hooks/glapi.cpp
		src/core/pack/api.cpp
		src/core/pack/options.cpp
		src/core/pack/pack.cpp
		src/core/render/shader_compose.cpp
		src/launcher/downloads.cpp
		src/launcher/image.cpp
		src/launcher/launcher.cpp
	)
	# The tests read tests/packs/ and tests/luanti/ from the source tree
	target_compile_definitions(prisma_tests PRIVATE PRISMA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
	if(WIN32)
		target_compile_definitions(prisma_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
		target_sources(prisma_tests PRIVATE src/launcher/image_win32.cpp)
		target_link_libraries(prisma_tests PRIVATE version shell32 ole32 windowscodecs)
	else()
		target_sources(prisma_tests PRIVATE src/launcher/image_linux.cpp)
		target_link_libraries(prisma_tests PRIVATE dl)
	endif()
	add_test(NAME unit COMMAND prisma_tests)

	add_test(NAME check_internal COMMAND prisma_check --internal)
	file(GLOB PRISMA_TEST_PACKS LIST_DIRECTORIES true CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/packs/*)
	foreach(pack IN LISTS PRISMA_TEST_PACKS)
		if(IS_DIRECTORY ${pack})
			get_filename_component(name ${pack} NAME)
			add_test(NAME check_pack_${name} COMMAND prisma_check ${pack})
		endif()
	endforeach()

	# A Luanti installation to compare with the baseline (prisma_check --luanti);
	# none by default, since the build does not need Luanti
	set(PRISMA_LUANTI_DIR "" CACHE PATH "Luanti ${PRISMA_LUANTI_VERSION} folder for the check_luanti test")
	if(PRISMA_LUANTI_DIR)
		add_test(NAME check_luanti COMMAND prisma_check --luanti ${PRISMA_LUANTI_DIR})
	endif()
endif()
