#include "Hooks_Screenshot.h"

#include "Hooks_Input.h"
#include "NiRenderer.h"
#include "Utilities.h"

#include <utility>
#include <wincodec.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace
{
	enum ScreenshotFormat
	{
		kScreenshotFormat_Engine = 0,
		kScreenshotFormat_Bmp,
		kScreenshotFormat_Png,
		kScreenshotFormat_Jpg,
		kScreenshotFormat_Gif,
		kScreenshotFormat_Tiff,
	};

	struct CapturedFrame
	{
		UInt32 width = 0;
		UInt32 height = 0;
		std::vector<UInt8> pixels;
	};

	struct GifProfile
	{
		UInt32 maxWidth = 720;
		UInt32 maxHeight = 450;
		UInt32 seconds = 5;
		UInt32 fps = 12;
		UInt32 colors = 256;
		ULONGLONG targetBytes = 3ULL * 1024ULL * 1024ULL;
		ULONGLONG maxBytes = 5ULL * 1024ULL * 1024ULL;
		bool loop = true;
	};

	struct GifRecorder
	{
		bool active = false;
		UInt32 nextCaptureTick = 0;
		UInt32 frameIntervalMs = 83;
		UInt32 targetFrames = 60;
		UInt32 attemptedFrames = 0;
		std::string path;
		std::vector<CapturedFrame> frames;
	};

	struct GifExportAttempt
	{
		UInt32 maxWidth;
		UInt32 maxHeight;
		UInt32 fps;
		UInt32 colors;
	};

	static constexpr UInt16 kTESReloadedDefaultScreenshotKey = 87;
	static constexpr UInt16 kOblivionScreenshotKey = DIK_SYSRQ;

	static UInt16 s_screenshotKey = kTESReloadedDefaultScreenshotKey;
	static ScreenshotFormat s_format = kScreenshotFormat_Gif;
	static GifProfile s_gifProfile;
	static GifRecorder s_recorder;
	static bool s_pendingStillCapture = false;
	static ScreenshotFormat s_pendingStillFormat = kScreenshotFormat_Png;
	static void (__cdecl* const s_vanillaTakeScreenshot)(UInt32) = reinterpret_cast<void (__cdecl*)(UInt32)>(0x00411750);

	template <class T>
	void SafeRelease(T*& ptr)
	{
		if (ptr)
		{
			ptr->Release();
			ptr = nullptr;
		}
	}

	static bool KeyIsDown(OSInputGlobalsEx* input, UInt16 keycode, bool previous)
	{
		if (!input)
			return false;

		if (keycode < 256)
			return previous ? input->PreviousKeyState[keycode] == 0x80 : input->CurrentKeyState[keycode] == 0x80;

		if (keycode >= 256 && keycode < kMaxButtons)
		{
			UInt16 mouseButton = keycode - 256;
			if (input->oldMouseButtonSwap && mouseButton == 0)
				mouseButton = 1;
			else if (input->oldMouseButtonSwap && mouseButton == 1)
				mouseButton = 0;

			return previous ? input->PreviousMouseState.rgbButtons[mouseButton] == 0x80 : input->CurrentMouseState.rgbButtons[mouseButton] == 0x80;
		}

		if (keycode == 264)
			return previous ? input->PreviousMouseState.lZ > 0 : input->CurrentMouseState.lZ > 0;

		if (keycode == 265)
			return previous ? input->PreviousMouseState.lZ < 0 : input->CurrentMouseState.lZ < 0;

		return false;
	}

	static void ApplyEnginePrintScreenMapping(OSInputGlobalsEx* input, bool current, bool previous)
	{
		input->CurrentKeyState[kOblivionScreenshotKey] = current ? 0x80 : 0;
		input->PreviousKeyState[kOblivionScreenshotKey] = previous ? 0x80 : 0;
	}

	static bool IsFormatName(const char* lhs, const char* rhs)
	{
		if (!lhs || !rhs)
			return false;

		while (*lhs == '.')
			++lhs;
		while (*rhs == '.')
			++rhs;

		return _stricmp(lhs, rhs) == 0;
	}

	static const char* FormatToString(ScreenshotFormat format)
	{
		switch (format)
		{
			case kScreenshotFormat_Engine:	return "engine";
			case kScreenshotFormat_Bmp:		return "bmp";
			case kScreenshotFormat_Png:		return "png";
			case kScreenshotFormat_Jpg:		return "jpg";
			case kScreenshotFormat_Gif:		return "gif";
			case kScreenshotFormat_Tiff:	return "tiff";
			default:						return "gif";
		}
	}

	static const GUID& ContainerForFormat(ScreenshotFormat format)
	{
		switch (format)
		{
			case kScreenshotFormat_Bmp:		return GUID_ContainerFormatBmp;
			case kScreenshotFormat_Png:		return GUID_ContainerFormatPng;
			case kScreenshotFormat_Jpg:		return GUID_ContainerFormatJpeg;
			case kScreenshotFormat_Tiff:	return GUID_ContainerFormatTiff;
			default:						return GUID_ContainerFormatPng;
		}
	}

	static std::string MakeScreenshotPath(const char* extension)
	{
		std::string root = GetOblivionDirectory();
		if (root.empty())
			root = ".\\";

		for (UInt32 idx = 0; idx < 10000; ++idx)
		{
			char filename[MAX_PATH] = { 0 };
			sprintf_s(filename, sizeof(filename), "%sScreenShot%u.%s", root.c_str(), idx, extension);
			if (GetFileAttributes(filename) == INVALID_FILE_ATTRIBUTES)
				return filename;
		}

		char fallback[MAX_PATH] = { 0 };
		sprintf_s(fallback, sizeof(fallback), "%sScreenShot.%s", root.c_str(), extension);
		return fallback;
	}

	static bool PathToWide(const std::string& path, std::wstring& out)
	{
		int chars = MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, nullptr, 0);
		if (chars <= 0)
			return false;

		out.resize(chars);
		if (MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, &out[0], chars) <= 0)
			return false;

		if (!out.empty() && out.back() == L'\0')
			out.pop_back();

		return true;
	}

	static bool GetFileSizeBytes(const std::string& path, ULONGLONG& outSize)
	{
		WIN32_FILE_ATTRIBUTE_DATA data = {};
		if (!GetFileAttributesEx(path.c_str(), GetFileExInfoStandard, &data))
			return false;

		outSize = (static_cast<ULONGLONG>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
		return true;
	}

	static bool CreateWICFactory(IWICImagingFactory** outFactory)
	{
		*outFactory = nullptr;

		static bool s_comInitialized = false;
		static bool s_comUsable = true;

		if (!s_comInitialized)
		{
			HRESULT init = CoInitialize(nullptr);
			s_comUsable = SUCCEEDED(init) || init == RPC_E_CHANGED_MODE;
			s_comInitialized = true;
		}

		if (!s_comUsable)
			return false;

		return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(outFactory)));
	}

	static void ComputeTargetSize(UInt32 srcWidth, UInt32 srcHeight, UInt32 maxWidth, UInt32 maxHeight, UInt32& outWidth, UInt32& outHeight)
	{
		if (!srcWidth || !srcHeight)
		{
			outWidth = 0;
			outHeight = 0;
			return;
		}

		double scale = 1.0;
		if (maxWidth && srcWidth > maxWidth)
			scale = std::min(scale, static_cast<double>(maxWidth) / static_cast<double>(srcWidth));
		if (maxHeight && srcHeight > maxHeight)
			scale = std::min(scale, static_cast<double>(maxHeight) / static_cast<double>(srcHeight));

		outWidth = std::max<UInt32>(1, static_cast<UInt32>(srcWidth * scale));
		outHeight = std::max<UInt32>(1, static_cast<UInt32>(srcHeight * scale));
	}

	static UInt8 SampleChannel(const UInt8* src, UInt32 pitch, UInt32 width, UInt32 height, UInt32 x, UInt32 y, UInt32 channel)
	{
		x = std::min(x, width - 1);
		y = std::min(y, height - 1);
		return *(src + y * pitch + x * 4 + channel);
	}

	static bool ResizeBgra(const UInt8* src, UInt32 pitch, UInt32 srcWidth, UInt32 srcHeight, CapturedFrame& out)
	{
		if (!src || !srcWidth || !srcHeight || !out.width || !out.height)
			return false;

		out.pixels.resize(out.width * out.height * 4);

		for (UInt32 y = 0; y < out.height; ++y)
		{
			double srcY = ((static_cast<double>(y) + 0.5) * srcHeight / out.height) - 0.5;
			UInt32 y0 = static_cast<UInt32>(std::max(0.0, floor(srcY)));
			UInt32 y1 = std::min(y0 + 1, srcHeight - 1);
			double fy = srcY - y0;
			if (fy < 0)
				fy = 0;

			for (UInt32 x = 0; x < out.width; ++x)
			{
				double srcX = ((static_cast<double>(x) + 0.5) * srcWidth / out.width) - 0.5;
				UInt32 x0 = static_cast<UInt32>(std::max(0.0, floor(srcX)));
				UInt32 x1 = std::min(x0 + 1, srcWidth - 1);
				double fx = srcX - x0;
				if (fx < 0)
					fx = 0;

				UInt8* dst = &out.pixels[(y * out.width + x) * 4];
				for (UInt32 c = 0; c < 3; ++c)
				{
					double c00 = SampleChannel(src, pitch, srcWidth, srcHeight, x0, y0, c);
					double c10 = SampleChannel(src, pitch, srcWidth, srcHeight, x1, y0, c);
					double c01 = SampleChannel(src, pitch, srcWidth, srcHeight, x0, y1, c);
					double c11 = SampleChannel(src, pitch, srcWidth, srcHeight, x1, y1, c);
					double top = c00 + (c10 - c00) * fx;
					double bottom = c01 + (c11 - c01) * fx;
					dst[c] = static_cast<UInt8>(top + (bottom - top) * fy);
				}
				dst[3] = 255;
			}
		}

		return true;
	}

	static bool ResizeCapturedFrame(const CapturedFrame& src, UInt32 maxWidth, UInt32 maxHeight, CapturedFrame& out)
	{
		if (src.pixels.empty() || !src.width || !src.height)
			return false;

		ComputeTargetSize(src.width, src.height, maxWidth, maxHeight, out.width, out.height);
		if (out.width == src.width && out.height == src.height)
		{
			out = src;
			return true;
		}

		return ResizeBgra(src.pixels.data(), src.width * 4, src.width, src.height, out);
	}

	static bool CaptureBackBuffer(CapturedFrame& out, UInt32 maxWidth, UInt32 maxHeight)
	{
		if (!g_renderer || !*g_renderer)
			return false;

		NiDX9Renderer* renderer = static_cast<NiDX9Renderer*>(*g_renderer);
		if (!renderer || !renderer->device)
			return false;

		IDirect3DDevice9* device = renderer->device;
		IDirect3DSurface9* backBuffer = nullptr;
		IDirect3DSurface9* resolvedSurface = nullptr;
		IDirect3DSurface9* systemSurface = nullptr;
		D3DSURFACE_DESC desc = {};
		D3DLOCKED_RECT locked = {};
		bool success = false;

		if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)))
			return false;

		if (FAILED(backBuffer->GetDesc(&desc)))
			goto cleanup;

		if (desc.Format != D3DFMT_X8R8G8B8 && desc.Format != D3DFMT_A8R8G8B8)
		{
			_WARNING("Screenshot capture skipped: unsupported backbuffer format %u", desc.Format);
			goto cleanup;
		}

		if (desc.MultiSampleType != D3DMULTISAMPLE_NONE)
		{
			if (FAILED(device->CreateRenderTarget(desc.Width, desc.Height, desc.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &resolvedSurface, nullptr)))
				goto cleanup;

			if (FAILED(device->StretchRect(backBuffer, nullptr, resolvedSurface, nullptr, D3DTEXF_NONE)))
				goto cleanup;
		}
		else
		{
			resolvedSurface = backBuffer;
			resolvedSurface->AddRef();
		}

		if (FAILED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &systemSurface, nullptr)))
			goto cleanup;

		if (FAILED(device->GetRenderTargetData(resolvedSurface, systemSurface)))
			goto cleanup;

		ComputeTargetSize(desc.Width, desc.Height, maxWidth, maxHeight, out.width, out.height);

		if (FAILED(systemSurface->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
			goto cleanup;

		success = ResizeBgra(static_cast<const UInt8*>(locked.pBits), locked.Pitch, desc.Width, desc.Height, out);
		systemSurface->UnlockRect();

	cleanup:
		SafeRelease(systemSurface);
		SafeRelease(resolvedSurface);
		SafeRelease(backBuffer);
		return success;
	}

	static bool CreateBitmapFromFrame(IWICImagingFactory* factory, const CapturedFrame& frame, IWICBitmap** outBitmap)
	{
		*outBitmap = nullptr;
		if (!factory || frame.pixels.empty())
			return false;

		return SUCCEEDED(factory->CreateBitmapFromMemory(
			frame.width,
			frame.height,
			GUID_WICPixelFormat32bppBGRA,
			frame.width * 4,
			static_cast<UINT>(frame.pixels.size()),
			const_cast<BYTE*>(frame.pixels.data()),
			outBitmap));
	}

	static bool SaveStillFrame(const std::string& path, ScreenshotFormat format, const CapturedFrame& frame)
	{
		std::wstring widePath;
		if (!PathToWide(path, widePath))
			return false;

		IWICImagingFactory* factory = nullptr;
		IWICStream* stream = nullptr;
		IWICBitmapEncoder* encoder = nullptr;
		IWICBitmapFrameEncode* frameEncoder = nullptr;
		IPropertyBag2* propertyBag = nullptr;
		IWICBitmap* bitmap = nullptr;
		IWICFormatConverter* converter = nullptr;
		WICPixelFormatGUID targetFormat = GUID_WICPixelFormat32bppBGRA;
		bool success = false;

		if (!CreateWICFactory(&factory))
			goto cleanup;

		if (FAILED(factory->CreateStream(&stream)))
			goto cleanup;
		if (FAILED(stream->InitializeFromFilename(widePath.c_str(), GENERIC_WRITE)))
			goto cleanup;
		if (FAILED(factory->CreateEncoder(ContainerForFormat(format), nullptr, &encoder)))
			goto cleanup;
		if (FAILED(encoder->Initialize(stream, WICBitmapEncoderNoCache)))
			goto cleanup;
		if (FAILED(encoder->CreateNewFrame(&frameEncoder, &propertyBag)))
			goto cleanup;
		if (FAILED(frameEncoder->Initialize(propertyBag)))
			goto cleanup;
		if (FAILED(frameEncoder->SetSize(frame.width, frame.height)))
			goto cleanup;

		if (FAILED(frameEncoder->SetPixelFormat(&targetFormat)))
			goto cleanup;

		if (!CreateBitmapFromFrame(factory, frame, &bitmap))
			goto cleanup;

		if (IsEqualGUID(targetFormat, GUID_WICPixelFormat32bppBGRA))
		{
			if (FAILED(frameEncoder->WriteSource(bitmap, nullptr)))
				goto cleanup;
		}
		else
		{
			if (FAILED(factory->CreateFormatConverter(&converter)))
				goto cleanup;
			if (FAILED(converter->Initialize(bitmap, targetFormat, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeMedianCut)))
				goto cleanup;
			if (FAILED(frameEncoder->WriteSource(converter, nullptr)))
				goto cleanup;
		}

		if (FAILED(frameEncoder->Commit()))
			goto cleanup;
		if (FAILED(encoder->Commit()))
			goto cleanup;

		success = true;

	cleanup:
		SafeRelease(converter);
		SafeRelease(bitmap);
		SafeRelease(propertyBag);
		SafeRelease(frameEncoder);
		SafeRelease(encoder);
		SafeRelease(stream);
		SafeRelease(factory);
		return success;
	}

	static void SetMetadataUInt16(IWICMetadataQueryWriter* writer, LPCWSTR name, UInt16 value)
	{
		PROPVARIANT prop = {};
		PropVariantInit(&prop);
		prop.vt = VT_UI2;
		prop.uiVal = value;
		writer->SetMetadataByName(name, &prop);
		PropVariantClear(&prop);
	}

	static void SetMetadataByte(IWICMetadataQueryWriter* writer, LPCWSTR name, UInt8 value)
	{
		PROPVARIANT prop = {};
		PropVariantInit(&prop);
		prop.vt = VT_UI1;
		prop.bVal = value;
		writer->SetMetadataByName(name, &prop);
		PropVariantClear(&prop);
	}

	static void SetGifLoopMetadata(IWICBitmapEncoder* encoder, bool loop)
	{
		if (!loop)
			return;

		IWICMetadataQueryWriter* writer = nullptr;
		if (FAILED(encoder->GetMetadataQueryWriter(&writer)))
			return;

		BYTE application[] = { 'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0' };
		BYTE data[] = { 3, 1, 0, 0, 0 };

		PROPVARIANT prop = {};
		PropVariantInit(&prop);
		prop.vt = VT_UI1 | VT_VECTOR;
		prop.caub.cElems = sizeof(application);
		prop.caub.pElems = application;
		writer->SetMetadataByName(L"/appext/application", &prop);

		PropVariantInit(&prop);
		prop.vt = VT_UI1 | VT_VECTOR;
		prop.caub.cElems = sizeof(data);
		prop.caub.pElems = data;
		writer->SetMetadataByName(L"/appext/data", &prop);

		SafeRelease(writer);
	}

	static bool SaveGif(const std::string& path, const std::vector<CapturedFrame>& frames, const GifProfile& profile)
	{
		if (frames.empty())
			return false;

		std::wstring widePath;
		if (!PathToWide(path, widePath))
			return false;

		IWICImagingFactory* factory = nullptr;
		IWICStream* stream = nullptr;
		IWICBitmapEncoder* encoder = nullptr;
		bool success = false;

		if (!CreateWICFactory(&factory))
			goto cleanup;

		if (FAILED(factory->CreateStream(&stream)))
			goto cleanup;
		if (FAILED(stream->InitializeFromFilename(widePath.c_str(), GENERIC_WRITE)))
			goto cleanup;
		if (FAILED(factory->CreateEncoder(GUID_ContainerFormatGif, nullptr, &encoder)))
			goto cleanup;
		if (FAILED(encoder->Initialize(stream, WICBitmapEncoderNoCache)))
			goto cleanup;

		SetGifLoopMetadata(encoder, profile.loop);

		for (const CapturedFrame& captured : frames)
		{
			IWICBitmapFrameEncode* frameEncoder = nullptr;
			IPropertyBag2* propertyBag = nullptr;
			IWICMetadataQueryWriter* metadata = nullptr;
			IWICBitmap* bitmap = nullptr;
			IWICPalette* palette = nullptr;
			IWICFormatConverter* converter = nullptr;
			WICPixelFormatGUID targetFormat = GUID_WICPixelFormat8bppIndexed;
			UInt16 delayCs = static_cast<UInt16>(std::max<UInt32>(1, 100 / std::max<UInt32>(1, profile.fps)));

			if (FAILED(encoder->CreateNewFrame(&frameEncoder, &propertyBag)))
				goto frame_cleanup;
			if (FAILED(frameEncoder->Initialize(propertyBag)))
				goto frame_cleanup;
			if (FAILED(frameEncoder->SetSize(captured.width, captured.height)))
				goto frame_cleanup;

			if (SUCCEEDED(frameEncoder->GetMetadataQueryWriter(&metadata)))
			{
				SetMetadataUInt16(metadata, L"/grctlext/Delay", delayCs);
				SetMetadataByte(metadata, L"/grctlext/Disposal", 2);
			}

			if (!CreateBitmapFromFrame(factory, captured, &bitmap))
				goto frame_cleanup;
			if (FAILED(factory->CreatePalette(&palette)))
				goto frame_cleanup;
			if (FAILED(palette->InitializeFromBitmap(bitmap, std::min<UInt32>(256, std::max<UInt32>(2, profile.colors)), FALSE)))
				goto frame_cleanup;
			if (FAILED(frameEncoder->SetPalette(palette)))
				goto frame_cleanup;

			if (FAILED(frameEncoder->SetPixelFormat(&targetFormat)))
				goto frame_cleanup;

			if (FAILED(factory->CreateFormatConverter(&converter)))
				goto frame_cleanup;
			if (FAILED(converter->Initialize(bitmap, targetFormat, WICBitmapDitherTypeNone, palette, 0.0, WICBitmapPaletteTypeCustom)))
				goto frame_cleanup;
			if (FAILED(frameEncoder->WriteSource(converter, nullptr)))
				goto frame_cleanup;
			if (FAILED(frameEncoder->Commit()))
				goto frame_cleanup;

			SafeRelease(converter);
			SafeRelease(palette);
			SafeRelease(bitmap);
			SafeRelease(metadata);
			SafeRelease(propertyBag);
			SafeRelease(frameEncoder);
			continue;

		frame_cleanup:
			SafeRelease(converter);
			SafeRelease(palette);
			SafeRelease(bitmap);
			SafeRelease(metadata);
			SafeRelease(propertyBag);
			SafeRelease(frameEncoder);
			goto cleanup;
		}

		if (FAILED(encoder->Commit()))
			goto cleanup;

		success = true;

	cleanup:
		SafeRelease(encoder);
		SafeRelease(stream);
		SafeRelease(factory);
		return success;
	}

	static bool PrepareGifFramesForAttempt(const std::vector<CapturedFrame>& source, const GifExportAttempt& attempt, UInt32 seconds, std::vector<CapturedFrame>& out)
	{
		out.clear();
		if (source.empty())
			return false;

		UInt32 wantedFrames = std::max<UInt32>(1, seconds * std::max<UInt32>(1, attempt.fps));
		wantedFrames = std::min<UInt32>(wantedFrames, static_cast<UInt32>(source.size()));
		out.reserve(wantedFrames);

		for (UInt32 i = 0; i < wantedFrames; ++i)
		{
			UInt32 sourceIdx = static_cast<UInt32>((static_cast<ULONGLONG>(i) * source.size()) / wantedFrames);
			sourceIdx = std::min<UInt32>(sourceIdx, static_cast<UInt32>(source.size() - 1));

			CapturedFrame frame;
			if (!ResizeCapturedFrame(source[sourceIdx], attempt.maxWidth, attempt.maxHeight, frame))
				return false;

			out.push_back(std::move(frame));
		}

		return !out.empty();
	}

	static bool SaveGifWithinBudget(const std::string& path, const std::vector<CapturedFrame>& source, const GifProfile& profile)
	{
		const GifExportAttempt attempts[] =
		{
			{ profile.maxWidth, profile.maxHeight, profile.fps, profile.colors },
			{ 720, 450, 12, 192 },
			{ 680, 425, 12, 160 },
			{ 640, 400, 10, 128 },
			{ 600, 375, 10, 96 },
			{ 560, 350, 10, 64 },
		};

		bool saved = false;
		for (UInt32 i = 0; i < sizeof(attempts) / sizeof(attempts[0]); ++i)
		{
			GifProfile attemptProfile = profile;
			attemptProfile.maxWidth = attempts[i].maxWidth;
			attemptProfile.maxHeight = attempts[i].maxHeight;
			attemptProfile.fps = attempts[i].fps;
			attemptProfile.colors = attempts[i].colors;

			std::vector<CapturedFrame> attemptFrames;
			if (!PrepareGifFramesForAttempt(source, attempts[i], profile.seconds, attemptFrames))
				continue;
			if (!SaveGif(path, attemptFrames, attemptProfile))
				continue;

			saved = true;

			ULONGLONG fileSize = 0;
			if (!GetFileSizeBytes(path, fileSize))
				return true;
			if (fileSize <= profile.targetBytes)
				return true;
		}

		if (saved)
		{
			ULONGLONG fileSize = 0;
			if (GetFileSizeBytes(path, fileSize) && fileSize > profile.maxBytes)
				_WARNING("GIF screenshot saved but exceeds Nexus-safe budget: %s (%I64u bytes)", path.c_str(), fileSize);
		}

		return saved;
	}

	static void CaptureStill(ScreenshotFormat format)
	{
		CapturedFrame frame;
		if (!CaptureBackBuffer(frame, 0, 0))
			return;

		std::string path = MakeScreenshotPath(FormatToString(format));
		if (!SaveStillFrame(path, format, frame))
			_WARNING("Screenshot capture failed for %s", path.c_str());
	}

	static void StartGifCapture()
	{
		if (s_recorder.active)
			return;

		s_recorder.active = true;
		s_recorder.frames.clear();
		s_recorder.path = MakeScreenshotPath("gif");
		s_recorder.frameIntervalMs = std::max<UInt32>(1, 1000 / std::max<UInt32>(1, s_gifProfile.fps));
		s_recorder.targetFrames = std::max<UInt32>(1, s_gifProfile.seconds * s_gifProfile.fps);
		s_recorder.attemptedFrames = 0;
		s_recorder.nextCaptureTick = GetTickCount();
		s_recorder.frames.reserve(s_recorder.targetFrames);
	}

	static void TriggerScreenshot()
	{
		switch (s_format)
		{
			case kScreenshotFormat_Gif:
				StartGifCapture();
				break;

			case kScreenshotFormat_Bmp:
			case kScreenshotFormat_Png:
			case kScreenshotFormat_Jpg:
			case kScreenshotFormat_Tiff:
				s_pendingStillFormat = s_format;
				s_pendingStillCapture = true;
				break;

			default:
				break;
		}
	}
}

void Screenshot_HandleInput(OSInputGlobalsEx* input)
{
	if (!input)
		return;

	bool current = KeyIsDown(input, s_screenshotKey, false);
	bool previous = KeyIsDown(input, s_screenshotKey, true);

	ApplyEnginePrintScreenMapping(input, current, previous);
}

void Screenshot_Tick()
{
	if (s_pendingStillCapture)
	{
		s_pendingStillCapture = false;
		CaptureStill(s_pendingStillFormat);
	}

	if (!s_recorder.active)
		return;

	UInt32 now = GetTickCount();
	if (now < s_recorder.nextCaptureTick)
		return;

	CapturedFrame frame;
	if (CaptureBackBuffer(frame, s_gifProfile.maxWidth, s_gifProfile.maxHeight))
		s_recorder.frames.push_back(std::move(frame));

	++s_recorder.attemptedFrames;
	s_recorder.nextCaptureTick += s_recorder.frameIntervalMs;

	if (s_recorder.attemptedFrames >= s_recorder.targetFrames)
	{
		if (!s_recorder.frames.empty() && !SaveGifWithinBudget(s_recorder.path, s_recorder.frames, s_gifProfile))
			_WARNING("GIF screenshot capture failed for %s", s_recorder.path.c_str());

		s_recorder.frames.clear();
		s_recorder.active = false;
	}
}

void __cdecl Screenshot_HandleEngineRequest(UInt32 multiFlag)
{
	if (s_format == kScreenshotFormat_Engine)
	{
		s_vanillaTakeScreenshot(multiFlag);
		return;
	}

	TriggerScreenshot();
	Screenshot_Tick();
}

bool Screenshot_SetKey(UInt32 keycode)
{
	if (keycode >= kMaxMacros)
		return false;

	s_screenshotKey = static_cast<UInt16>(keycode);
	return true;
}

UInt16 Screenshot_GetKey()
{
	return s_screenshotKey;
}

bool Screenshot_SetFormat(const char* format)
{
	if (IsFormatName(format, "engine") || IsFormatName(format, "default"))
		s_format = kScreenshotFormat_Engine;
	else if (IsFormatName(format, "bmp"))
		s_format = kScreenshotFormat_Bmp;
	else if (IsFormatName(format, "png"))
		s_format = kScreenshotFormat_Png;
	else if (IsFormatName(format, "jpg") || IsFormatName(format, "jpeg"))
		s_format = kScreenshotFormat_Jpg;
	else if (IsFormatName(format, "gif"))
		s_format = kScreenshotFormat_Gif;
	else if (IsFormatName(format, "tif") || IsFormatName(format, "tiff"))
		s_format = kScreenshotFormat_Tiff;
	else
		return false;

	return true;
}

const char* Screenshot_GetFormat()
{
	return FormatToString(s_format);
}
