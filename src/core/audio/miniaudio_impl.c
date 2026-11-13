// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
/*
	The implementation of miniaudio (third_party/miniaudio), with stb_vorbis
	so it reads .ogg files too, as its documentation says to include them.
	Used by audio.cpp. MA_NO_ENCODING, MA_NO_MP3 and MA_NO_FLAC come from
	CMakeLists.txt, so the header sees the same configuration everywhere:
	packs may only use WAV and Ogg Vorbis.
*/
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#undef STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"
