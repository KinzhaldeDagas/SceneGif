#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "windowscodecs.lib")

using UInt8 = unsigned char;
using UInt16 = unsigned short;
using UInt32 = unsigned int;

struct PluginInfo
{
	enum { kInfoVersion = 3 };

	UInt32 infoVersion;
	const char* name;
	UInt32 version;
};

struct OBSEInterface
{
	UInt32 obseVersion;
	UInt32 oblivionVersion;
	UInt32 editorVersion;
	UInt32 isEditor;
	bool (*RegisterCommand)(void* info);
	void (*SetOpcodeBase)(UInt32 opcode);
	void* (*QueryInterface)(UInt32 id);
	UInt32 (*GetPluginHandle)();
	bool (*RegisterTypedCommand)(void* info, UInt32 retnType);
	const char* (*GetOblivionDirectory)();
	bool (*GetPluginLoaded)(const char* pluginName);
	UInt32 (*GetPluginVersion)(const char* pluginName);
};

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

	struct MouseState
	{
		LONG lX;
		LONG lY;
		LONG lZ;
		BYTE rgbButtons[8];
	};

	struct NiDX9RendererLite
	{
		UInt8 pad000[0x280];
		IDirect3DDevice9* device;
	};

	static constexpr UInt32 kOblivionVersion_1_2_416 = 0x010201A0;
	static constexpr UInt32 kInputQueryScreenshotCallAddr = 0x0040D706;
	static constexpr UInt32 kFrameTickCallAddr = 0x0040D6F1;
	static constexpr UInt32 kScreenshotCallAddr = 0x0040D72F;
	static constexpr UInt32 kOriginalQueryControlStateAddr = 0x00403520;
	static constexpr UInt32 kOriginalFrameTickAddr = 0x007B8400;
	static constexpr UInt32 kOriginalTakeScreenshotAddr = 0x00411750;
	static constexpr UInt32 kRendererSingletonAddr = 0x00B3F928;

	static constexpr UInt16 DIK_SYSRQ_VALUE = 0xB7;
	static constexpr UInt16 DIK_U_VALUE = 0x16;

	static std::string s_oblivionDir = ".\\";
	static UInt16 s_screenshotKey = DIK_U_VALUE;
	static ScreenshotFormat s_format = kScreenshotFormat_Gif;
	static GifProfile s_gifProfile;
	static GifRecorder s_recorder;
	static bool s_pendingStillCapture = false;
	static ScreenshotFormat s_pendingStillFormat = kScreenshotFormat_Png;

	static UInt32 (__thiscall* const s_originalQueryControlState)(void*, UInt32, UInt32) =
		reinterpret_cast<UInt32 (__thiscall*)(void*, UInt32, UInt32)>(kOriginalQueryControlStateAddr);
	static void (__cdecl* const s_originalFrameTick)() =
		reinterpret_cast<void (__cdecl*)()>(kOriginalFrameTickAddr);
	static void (__cdecl* const s_vanillaTakeScreenshot)(UInt32) =
		reinterpret_cast<void (__cdecl*)(UInt32)>(kOriginalTakeScreenshotAddr);

	template <class T>
	void SafeRelease(T*& ptr)
	{
		if (ptr)
		{
			ptr->Release();
			ptr = nullptr;
		}
	}

	void Log(const char* fmt, ...)
	{
		char path[MAX_PATH] = { 0 };
		sprintf_s(path, sizeof(path), "%sData\\OBSE\\Plugins\\SceneGIFs.log", s_oblivionDir.c_str());

		FILE* file = nullptr;
		if (fopen_s(&file, path, "ab") != 0 || !file)
			return;

		va_list args;
		va_start(args, fmt);
		vfprintf(file, fmt, args);
		va_end(args);
		fputs("\r\n", file);
		fclose(file);
	}

	void SafeWrite8(UInt32 addr, UInt8 data)
	{
		DWORD oldProtect = 0;
		VirtualProtect(reinterpret_cast<void*>(addr), 1, PAGE_EXECUTE_READWRITE, &oldProtect);
		*reinterpret_cast<UInt8*>(addr) = data;
		VirtualProtect(reinterpret_cast<void*>(addr), 1, oldProtect, &oldProtect);
	}

	void SafeWrite32(UInt32 addr, UInt32 data)
	{
		DWORD oldProtect = 0;
		VirtualProtect(reinterpret_cast<void*>(addr), 4, PAGE_EXECUTE_READWRITE, &oldProtect);
		*reinterpret_cast<UInt32*>(addr) = data;
		VirtualProtect(reinterpret_cast<void*>(addr), 4, oldProtect, &oldProtect);
	}

	void WriteRelCall(UInt32 callSrc, UInt32 callTgt)
	{
		SafeWrite8(callSrc, 0xE8);
		SafeWrite32(callSrc + 1, callTgt - callSrc - 5);
	}

	std::string MakeOblivionPath(const char* relativePath)
	{
		std::string root = s_oblivionDir.empty() ? ".\\" : s_oblivionDir;
		if (!root.empty() && root.back() != '\\' && root.back() != '/')
			root += '\\';

		return root + relativePath;
	}

	void EnsurePluginDirectory()
	{
		CreateDirectory(MakeOblivionPath("Data").c_str(), nullptr);
		CreateDirectory(MakeOblivionPath("Data\\OBSE").c_str(), nullptr);
		CreateDirectory(MakeOblivionPath("Data\\OBSE\\Plugins").c_str(), nullptr);
	}

	void WriteDefaultIniIfMissing(const std::string& path)
	{
		if (GetFileAttributes(path.c_str()) != INVALID_FILE_ATTRIBUTES)
			return;

		EnsurePluginDirectory();

		static const char* kDefaultIni =
			"; SceneGIFs OBSE plugin configuration\r\n"
			"; Key accepts a single letter, F1-F12, PrintScreen, or a DirectInput scan code. U = 22.\r\n"
			"; Format supports gif, png, jpg, bmp, tiff, or engine.\r\n"
			"\r\n"
			"[Screenshot]\r\n"
			"Key=U\r\n"
			"Format=gif\r\n"
			"\r\n"
			"[GIF]\r\n"
			"; Production default: 720 px / 12 FPS / 5 seconds / under 3 MB target.\r\n"
			"MaxWidth=720\r\n"
			"MaxHeight=450\r\n"
			"Seconds=5\r\n"
			"FPS=12\r\n"
			"Colors=256\r\n"
			"TargetMB=3\r\n"
			"MaxMB=5\r\n"
			"Loop=1\r\n";

		FILE* file = nullptr;
		if (fopen_s(&file, path.c_str(), "wb") != 0 || !file)
			return;

		fwrite(kDefaultIni, 1, strlen(kDefaultIni), file);
		fclose(file);
	}

	std::string TrimCopy(const char* text)
	{
		if (!text)
			return "";

		const char* start = text;
		while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n')
			++start;

		const char* end = start + strlen(start);
		while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n'))
			--end;

		return std::string(start, end);
	}

	bool TryParseUInt32(const char* text, UInt32& out)
	{
		std::string value = TrimCopy(text);
		if (value.empty())
			return false;

		char* end = nullptr;
		unsigned long parsed = strtoul(value.c_str(), &end, 0);
		if (!end || *end)
			return false;

		out = static_cast<UInt32>(parsed);
		return true;
	}

	bool TryParseKeyValue(const char* text, UInt32& out)
	{
		std::string value = TrimCopy(text);
		if (value.empty())
			return false;

		if (TryParseUInt32(value.c_str(), out))
			return true;

		if (_strnicmp(value.c_str(), "DIK_", 4) == 0)
			value = value.substr(4);

		if (_stricmp(value.c_str(), "PRINTSCREEN") == 0 || _stricmp(value.c_str(), "SYSRQ") == 0 || _stricmp(value.c_str(), "SYSREQ") == 0)
		{
			out = DIK_SYSRQ_VALUE;
			return true;
		}

		if (value.size() == 1)
		{
			static const UInt32 kLetterKeys[] =
			{
				0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
				0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C
			};

			char c = value[0];
			if (c >= 'a' && c <= 'z')
				c = static_cast<char>(c - ('a' - 'A'));
			if (c >= 'A' && c <= 'Z')
			{
				out = kLetterKeys[c - 'A'];
				return true;
			}
		}

		if ((value[0] == 'F' || value[0] == 'f') && value.size() > 1)
		{
			UInt32 functionKey = 0;
			if (TryParseUInt32(value.c_str() + 1, functionKey) && functionKey >= 1 && functionKey <= 12)
			{
				static const UInt32 kFunctionKeys[] =
				{
					0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x57, 0x58
				};

				out = kFunctionKeys[functionKey - 1];
				return true;
			}
		}

		return false;
	}

	bool IsFormatName(const char* lhs, const char* rhs)
	{
		if (!lhs || !rhs)
			return false;

		while (*lhs == '.')
			++lhs;
		while (*rhs == '.')
			++rhs;

		return _stricmp(lhs, rhs) == 0;
	}

	const char* FormatToString(ScreenshotFormat format)
	{
		switch (format)
		{
			case kScreenshotFormat_Engine: return "engine";
			case kScreenshotFormat_Bmp: return "bmp";
			case kScreenshotFormat_Png: return "png";
			case kScreenshotFormat_Jpg: return "jpg";
			case kScreenshotFormat_Gif: return "gif";
			case kScreenshotFormat_Tiff: return "tiff";
			default: return "gif";
		}
	}

	bool SetFormat(const char* format)
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

	UInt32 ClampIniUInt(UInt32 value, UInt32 minValue, UInt32 maxValue)
	{
		return std::min<UInt32>(std::max<UInt32>(value, minValue), maxValue);
	}

	UInt32 ReadIniUInt(const std::string& path, const char* section, const char* key, UInt32 fallback, UInt32 minValue, UInt32 maxValue)
	{
		char buffer[64] = { 0 };
		GetPrivateProfileString(section, key, "", buffer, sizeof(buffer), path.c_str());

		UInt32 value = fallback;
		if (!TryParseUInt32(buffer, value))
			value = fallback;

		return ClampIniUInt(value, minValue, maxValue);
	}

	bool ReadIniBool(const std::string& path, const char* section, const char* key, bool fallback)
	{
		char buffer[64] = { 0 };
		GetPrivateProfileString(section, key, fallback ? "1" : "0", buffer, sizeof(buffer), path.c_str());

		std::string value = TrimCopy(buffer);
		if (_stricmp(value.c_str(), "true") == 0 || _stricmp(value.c_str(), "yes") == 0 || _stricmp(value.c_str(), "on") == 0)
			return true;
		if (_stricmp(value.c_str(), "false") == 0 || _stricmp(value.c_str(), "no") == 0 || _stricmp(value.c_str(), "off") == 0)
			return false;

		UInt32 numeric = 0;
		if (TryParseUInt32(value.c_str(), numeric))
			return numeric != 0;

		return fallback;
	}

	void LoadConfig()
	{
		std::string path = MakeOblivionPath("Data\\OBSE\\Plugins\\SceneGIFs.ini");
		WriteDefaultIniIfMissing(path);

		char keyBuffer[64] = { 0 };
		GetPrivateProfileString("Screenshot", "Key", "U", keyBuffer, sizeof(keyBuffer), path.c_str());

		UInt32 key = DIK_U_VALUE;
		if (!TryParseKeyValue(keyBuffer, key) || key >= 266)
		{
			Log("Invalid Screenshot.Key in %s; using U", path.c_str());
			key = DIK_U_VALUE;
		}

		s_screenshotKey = static_cast<UInt16>(key);

		char formatBuffer[64] = { 0 };
		GetPrivateProfileString("Screenshot", "Format", "gif", formatBuffer, sizeof(formatBuffer), path.c_str());
		if (!SetFormat(formatBuffer))
		{
			Log("Invalid Screenshot.Format in %s; using gif", path.c_str());
			SetFormat("gif");
		}

		s_gifProfile.maxWidth = ReadIniUInt(path, "GIF", "MaxWidth", 720, 1, 4096);
		s_gifProfile.maxHeight = ReadIniUInt(path, "GIF", "MaxHeight", 450, 1, 4096);
		s_gifProfile.seconds = ReadIniUInt(path, "GIF", "Seconds", 5, 1, 60);
		s_gifProfile.fps = ReadIniUInt(path, "GIF", "FPS", 12, 1, 60);
		s_gifProfile.colors = ReadIniUInt(path, "GIF", "Colors", 256, 2, 256);
		s_gifProfile.loop = ReadIniBool(path, "GIF", "Loop", true);

		UInt32 targetMB = ReadIniUInt(path, "GIF", "TargetMB", 3, 1, 100);
		UInt32 maxMB = ReadIniUInt(path, "GIF", "MaxMB", 5, 1, 100);
		if (targetMB > maxMB)
			targetMB = maxMB;

		s_gifProfile.targetBytes = static_cast<ULONGLONG>(targetMB) * 1024ULL * 1024ULL;
		s_gifProfile.maxBytes = static_cast<ULONGLONG>(maxMB) * 1024ULL * 1024ULL;

		Log("Config loaded: key=%u format=%s gif=%ux%u %us %ufps %u colors",
			s_screenshotKey,
			FormatToString(s_format),
			s_gifProfile.maxWidth,
			s_gifProfile.maxHeight,
			s_gifProfile.seconds,
			s_gifProfile.fps,
			s_gifProfile.colors);
	}

	bool KeyIsDown(void* input, UInt16 keycode, bool previous)
	{
		if (!input)
			return false;

		UInt8* base = static_cast<UInt8*>(input);
		if (keycode < 256)
		{
			UInt8* state = base + (previous ? 0x19F4 : 0x18F4);
			return state[keycode] == 0x80;
		}

		if (keycode >= 256 && keycode < 264)
		{
			UInt16 mouseButton = keycode - 256;
			UInt32 oldMouseButtonSwap = *reinterpret_cast<UInt32*>(base + 0x1B48);
			if (oldMouseButtonSwap && mouseButton == 0)
				mouseButton = 1;
			else if (oldMouseButtonSwap && mouseButton == 1)
				mouseButton = 0;

			MouseState* state = reinterpret_cast<MouseState*>(base + (previous ? 0x1B34 : 0x1B20));
			return state->rgbButtons[mouseButton] == 0x80;
		}

		if (keycode == 264 || keycode == 265)
		{
			MouseState* state = reinterpret_cast<MouseState*>(base + (previous ? 0x1B34 : 0x1B20));
			return keycode == 264 ? state->lZ > 0 : state->lZ < 0;
		}

		return false;
	}

	const GUID& ContainerForFormat(ScreenshotFormat format)
	{
		switch (format)
		{
			case kScreenshotFormat_Bmp: return GUID_ContainerFormatBmp;
			case kScreenshotFormat_Png: return GUID_ContainerFormatPng;
			case kScreenshotFormat_Jpg: return GUID_ContainerFormatJpeg;
			case kScreenshotFormat_Tiff: return GUID_ContainerFormatTiff;
			default: return GUID_ContainerFormatPng;
		}
	}

	std::string MakeScreenshotPath(const char* extension)
	{
		for (UInt32 idx = 0; idx < 10000; ++idx)
		{
			char filename[MAX_PATH] = { 0 };
			sprintf_s(filename, sizeof(filename), "%sScreenShot%u.%s", s_oblivionDir.c_str(), idx, extension);
			if (GetFileAttributes(filename) == INVALID_FILE_ATTRIBUTES)
				return filename;
		}

		char fallback[MAX_PATH] = { 0 };
		sprintf_s(fallback, sizeof(fallback), "%sScreenShot.%s", s_oblivionDir.c_str(), extension);
		return fallback;
	}

	bool PathToWide(const std::string& path, std::wstring& out)
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

	bool GetFileSizeBytes(const std::string& path, ULONGLONG& outSize)
	{
		WIN32_FILE_ATTRIBUTE_DATA data = {};
		if (!GetFileAttributesEx(path.c_str(), GetFileExInfoStandard, &data))
			return false;

		outSize = (static_cast<ULONGLONG>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
		return true;
	}

	bool CreateWICFactory(IWICImagingFactory** outFactory)
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

	void ComputeTargetSize(UInt32 srcWidth, UInt32 srcHeight, UInt32 maxWidth, UInt32 maxHeight, UInt32& outWidth, UInt32& outHeight)
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

	UInt8 SampleChannel(const UInt8* src, UInt32 pitch, UInt32 width, UInt32 height, UInt32 x, UInt32 y, UInt32 channel)
	{
		x = std::min(x, width - 1);
		y = std::min(y, height - 1);
		return *(src + y * pitch + x * 4 + channel);
	}

	bool ResizeBgra(const UInt8* src, UInt32 pitch, UInt32 srcWidth, UInt32 srcHeight, CapturedFrame& out)
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

	bool ResizeCapturedFrame(const CapturedFrame& src, UInt32 maxWidth, UInt32 maxHeight, CapturedFrame& out)
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

	bool CaptureBackBuffer(CapturedFrame& out, UInt32 maxWidth, UInt32 maxHeight)
	{
		NiDX9RendererLite** rendererSingleton = reinterpret_cast<NiDX9RendererLite**>(kRendererSingletonAddr);
		if (!rendererSingleton || !*rendererSingleton || !(*rendererSingleton)->device)
			return false;

		IDirect3DDevice9* device = (*rendererSingleton)->device;
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
			goto cleanup;

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

	bool CreateBitmapFromFrame(IWICImagingFactory* factory, const CapturedFrame& frame, IWICBitmap** outBitmap)
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

	bool SaveStillFrame(const std::string& path, ScreenshotFormat format, const CapturedFrame& frame)
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
		bool success = false;
		WICPixelFormatGUID targetFormat = GUID_WICPixelFormat32bppBGRA;

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
		if (FAILED(frameEncoder->WriteSource(bitmap, nullptr)))
			goto cleanup;
		if (FAILED(frameEncoder->Commit()))
			goto cleanup;
		if (FAILED(encoder->Commit()))
			goto cleanup;

		success = true;

	cleanup:
		SafeRelease(bitmap);
		SafeRelease(propertyBag);
		SafeRelease(frameEncoder);
		SafeRelease(encoder);
		SafeRelease(stream);
		SafeRelease(factory);
		return success;
	}

	void SetMetadataUInt16(IWICMetadataQueryWriter* writer, LPCWSTR name, UInt16 value)
	{
		PROPVARIANT prop = {};
		PropVariantInit(&prop);
		prop.vt = VT_UI2;
		prop.uiVal = value;
		writer->SetMetadataByName(name, &prop);
		PropVariantClear(&prop);
	}

	void SetMetadataByte(IWICMetadataQueryWriter* writer, LPCWSTR name, UInt8 value)
	{
		PROPVARIANT prop = {};
		PropVariantInit(&prop);
		prop.vt = VT_UI1;
		prop.bVal = value;
		writer->SetMetadataByName(name, &prop);
		PropVariantClear(&prop);
	}

	void SetGifLoopMetadata(IWICBitmapEncoder* encoder, bool loop)
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

	bool SaveGif(const std::string& path, const std::vector<CapturedFrame>& frames, const GifProfile& profile)
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

	bool PrepareGifFramesForAttempt(const std::vector<CapturedFrame>& source, const GifExportAttempt& attempt, UInt32 seconds, std::vector<CapturedFrame>& out)
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

	bool SaveGifWithinBudget(const std::string& path, const std::vector<CapturedFrame>& source, const GifProfile& profile)
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
				Log("GIF saved but exceeds configured max: %s (%I64u bytes)", path.c_str(), fileSize);
		}

		return saved;
	}

	void CaptureStill(ScreenshotFormat format)
	{
		CapturedFrame frame;
		if (!CaptureBackBuffer(frame, 0, 0))
			return;

		std::string path = MakeScreenshotPath(FormatToString(format));
		if (!SaveStillFrame(path, format, frame))
			Log("Still screenshot capture failed for %s", path.c_str());
	}

	void StartGifCapture()
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

	void TriggerScreenshot()
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

	void ScreenshotTick()
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
				Log("GIF screenshot capture failed for %s", s_recorder.path.c_str());

			s_recorder.frames.clear();
			s_recorder.active = false;
		}
	}

	UInt32 __fastcall QueryScreenshotControlHook(void* input, void*, UInt32 control, UInt32 query)
	{
		if (control == 0x1F && query == 1)
		{
			bool current = KeyIsDown(input, s_screenshotKey, false);
			bool previous = KeyIsDown(input, s_screenshotKey, true);
			return current && !previous ? 1 : 0;
		}

		return s_originalQueryControlState(input, control, query);
	}

	void __cdecl FrameTickHook()
	{
		s_originalFrameTick();
		ScreenshotTick();
	}

	void __cdecl ScreenshotRequestHook(UInt32 multiFlag)
	{
		if (s_format == kScreenshotFormat_Engine)
		{
			s_vanillaTakeScreenshot(multiFlag);
			return;
		}

		TriggerScreenshot();
		ScreenshotTick();
	}

	void InstallHooks()
	{
		WriteRelCall(kInputQueryScreenshotCallAddr, reinterpret_cast<UInt32>(&QueryScreenshotControlHook));
		WriteRelCall(kFrameTickCallAddr, reinterpret_cast<UInt32>(&FrameTickHook));
		WriteRelCall(kScreenshotCallAddr, reinterpret_cast<UInt32>(&ScreenshotRequestHook));
		FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
		Log("Hooks installed");
	}
}

extern "C"
{
	__declspec(dllexport) bool OBSEPlugin_Query(const OBSEInterface* obse, PluginInfo* info)
	{
		info->infoVersion = PluginInfo::kInfoVersion;
		info->name = "SceneGIFs";
		info->version = 100;

		if (!obse || obse->isEditor)
			return false;

		if (obse->oblivionVersion != kOblivionVersion_1_2_416)
			return false;

		return true;
	}

	__declspec(dllexport) bool OBSEPlugin_Load(const OBSEInterface* obse)
	{
		if (!obse)
			return false;

		if (obse->GetOblivionDirectory)
			s_oblivionDir = obse->GetOblivionDirectory();

		if (s_oblivionDir.empty())
			s_oblivionDir = ".\\";
		if (s_oblivionDir.back() != '\\' && s_oblivionDir.back() != '/')
			s_oblivionDir += '\\';

		EnsurePluginDirectory();
		Log("SceneGIFs loading");
		LoadConfig();
		InstallHooks();
		return true;
	}
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
		DisableThreadLibraryCalls(instance);

	return TRUE;
}
