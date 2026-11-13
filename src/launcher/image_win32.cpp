// SPDX-FileCopyrightText: 2026 Flay Krunegan
// SPDX-License-Identifier: GPL-3.0-or-later
// decode_png() with the Windows Imaging Component (part of the system)
#include "launcher/image.hpp"

#include <windows.h>
#include <wincodec.h>

namespace prisma::launcher {

namespace {
template <typename T>
struct Com {
	T *p = nullptr;
	~Com()
	{
		if (p)
			p->Release();
	}
	T **operator&() { return &p; }
	T *operator->() { return p; }
};
} // namespace

bool decode_png(const std::string &bytes, Image &image, std::string &error)
{
	image = Image();
	static const char signature[8] = {'\x89', 'P', 'N', 'G', '\r', '\n', '\x1a', '\n'};
	if (bytes.size() < 8 || bytes.compare(0, 8, signature, 8) != 0) {
		error = "not a PNG image";
		return false;
	}
	// COM on this thread, if nobody did yet
	HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	bool ok = false;
	{
		Com<IWICImagingFactory> factory;
		Com<IWICStream> stream;
		Com<IWICBitmapDecoder> decoder;
		Com<IWICBitmapFrameDecode> frame;
		Com<IWICBitmapSource> rgba;
		UINT w = 0, h = 0;
		if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
			FAILED(factory->CreateStream(&stream)) ||
			FAILED(stream->InitializeFromMemory((BYTE *)bytes.data(), (DWORD)bytes.size())) ||
			FAILED(factory->CreateDecoderFromStream(stream.p, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
			FAILED(decoder->GetFrame(0, &frame)) ||
			FAILED(WICConvertBitmapSource(GUID_WICPixelFormat32bppRGBA, frame.p, &rgba)) ||
			FAILED(rgba->GetSize(&w, &h))) {
			error = "this PNG could not be read";
		} else if (w == 0 || h == 0 || w > (UINT)k_max_image_side || h > (UINT)k_max_image_side) {
			error = "the image is bigger than " + std::to_string(k_max_image_side) + " pixels on a side";
		} else {
			image.width = (int)w;
			image.height = (int)h;
			image.rgba.resize((size_t)w * h * 4);
			ok = SUCCEEDED(rgba->CopyPixels(nullptr, w * 4, (UINT)image.rgba.size(), image.rgba.data()));
			if (!ok) {
				error = "this PNG could not be read";
				image = Image();
			}
		}
	}
	if (SUCCEEDED(init))
		CoUninitialize();
	return ok;
}

} // namespace prisma::launcher
