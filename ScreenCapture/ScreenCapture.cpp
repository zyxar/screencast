#include <atlcomcli.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <algorithm>
#include <gdiplus.h>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include "ScreenCapture.h"

#pragma comment(lib, "gdiplus.lib")

namespace {
const UINT targetWidth = 960;
const ULONG targetQuality = 60;
static int GetEncoderClsid(const WCHAR* format, CLSID* pClsid)
{
	UINT num = 0; // number of image encoders
	UINT size = 0; // size of the image encoder array in bytes
	Gdiplus::ImageCodecInfo* pImageCodecInfo = nullptr;
	Gdiplus::GetImageEncodersSize(&num, &size);
	if (size == 0)
	{
		return -1;
	}

	pImageCodecInfo = (Gdiplus::ImageCodecInfo*)(malloc(size));
	if (pImageCodecInfo == nullptr)
	{
		return -1;
	}

	GetImageEncoders(num, size, pImageCodecInfo);
	for (UINT j = 0; j < num; ++j)
	{
		if (wcscmp(pImageCodecInfo[j].MimeType, format) == 0)
		{
			*pClsid = pImageCodecInfo[j].Clsid;
			free(pImageCodecInfo);
			return j;
		}
	}

	free(pImageCodecInfo);
	return -1;
}

static std::wstring GetTempDir()
{
	TCHAR path[MAX_PATH];
	auto len = GetTempPathW(MAX_PATH, path);
	return std::wstring(path, len);
}

static const std::wstring uuid()
{
	static boost::uuids::random_generator uuidGen;
	return boost::uuids::to_wstring(uuidGen());
}

static bool SaveBitmapOnDisk(Gdiplus::Bitmap *srcImage, const wchar_t* fmt = L"image/jpeg")
{
	static std::wstring destDir = GetTempDir();
	CLSID clsid;
	if (GetEncoderClsid(fmt, &clsid) == -1)
	{
		delete srcImage;
		return false;
	}
	auto destfile = destDir + uuid();
	Gdiplus::EncoderParameters encoderParameters;
	encoderParameters.Count = 1;
	encoderParameters.Parameter[0].Guid = Gdiplus::EncoderQuality;
	encoderParameters.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
	encoderParameters.Parameter[0].NumberOfValues = 1;
	encoderParameters.Parameter[0].Value = (void*)&targetQuality;
	UINT srcWidth = srcImage->GetWidth();
	if (targetWidth < srcWidth)
	{
		UINT srcHeight = srcImage->GetHeight();
		float rate = (float)targetWidth/srcWidth;
		UINT targetHeight = (UINT)(srcHeight * rate);
		Gdiplus::Bitmap image(targetWidth, targetHeight, srcImage->GetPixelFormat());
		Gdiplus::Graphics graphic(&image);
		graphic.ScaleTransform(rate, rate);
		graphic.DrawImage(srcImage, 0, 0, srcWidth, srcHeight);
		image.Save(destfile.c_str(), &clsid, &encoderParameters);
	}
	else
	{
		srcImage->Save(destfile.c_str(), &clsid, &encoderParameters);
	}
	screencast::ScreenCapturer::FileList.push_back(destfile);
	delete srcImage;
	return true;
}
} // namespace anonymous

namespace screencast {

class DynamicLib
{
public:
	static DynamicLib* Load(const TCHAR* libname)
	{
		std::unique_ptr<DynamicLib> instance(new DynamicLib(libname));
		return instance->module_ == 0 ? nullptr : instance.release();
	}
	~DynamicLib();
	FARPROC GetProcAddress(const char* proc) const;

protected:
	DynamicLib() = delete;
	explicit DynamicLib(const TCHAR* libname);
	HMODULE module_;
};

template<class Object>
std::shared_ptr<Object> SingletonOf()
{
	static std::weak_ptr<Object> refObject;
	std::shared_ptr<Object> object = refObject.lock();
	if (nullptr == object)
	{
		object = std::make_shared<Object>();
		refObject = object;
	}
	return object;
}

class GdiPlusHandle
{
public:
	explicit GdiPlusHandle()
		: handle_{0}
	{
		Gdiplus::GdiplusStartupInput gdiplusStartupInput;
		Gdiplus::GdiplusStartup(&handle_, &gdiplusStartupInput, nullptr);
	}

	~GdiPlusHandle()
	{
		if (handle_)
		{
			Gdiplus::GdiplusShutdown(handle_);
		}
	}
private:
	ULONG_PTR handle_;
};

class WinScreenCapturer : public ScreenCapturer
{
public:
	virtual ~WinScreenCapturer() {}
	WinScreenCapturer()
		: gdiplus_{SingletonOf<GdiPlusHandle>()}
	{
	}
protected:
	std::shared_ptr<GdiPlusHandle> gdiplus_;
};

class DxgiScreenCapturer : public WinScreenCapturer
{
	typedef HRESULT (WINAPI *D3D11CreateDeviceFuncType)(
		_In_opt_ IDXGIAdapter* pAdapter,
		D3D_DRIVER_TYPE DriverType,
		HMODULE Software,
		UINT Flags,
		_In_reads_opt_( FeatureLevels ) CONST D3D_FEATURE_LEVEL* pFeatureLevels,
		UINT FeatureLevels,
		UINT SDKVersion,
		_Out_opt_ ID3D11Device** ppDevice,
		_Out_opt_ D3D_FEATURE_LEVEL* pFeatureLevel,
		_Out_opt_ ID3D11DeviceContext** ppImmediateContext );
public:
	virtual ~DxgiScreenCapturer();
	bool Capture() override;
	bool Init();

protected:
	friend std::_Ref_count_obj<screencast::DxgiScreenCapturer>;
	explicit DxgiScreenCapturer();
	bool capture();
	static const D3D_DRIVER_TYPE DriverTypes[];
	static const UINT NumDriverTypes;
	static const D3D_FEATURE_LEVEL FeatureLevels[];
	static const UINT NumFeatureLevels;
private:
	std::unique_ptr<DynamicLib> d3dlib_;
	CComPtr<ID3D11Device> device_;
	CComPtr<ID3D11DeviceContext> context_;
	CComPtr<IDXGIOutputDuplication> dupl_;
#ifdef RENDER_CURSOR_
	CComPtr<ID3D11Texture2D> gdiImage_;
#endif
	CComPtr<ID3D11Texture2D> destImage_;
	// DXGI_OUTPUT_DESC outputDesc_;
	DXGI_OUTDUPL_DESC duplDesc_;
};

const D3D_DRIVER_TYPE DxgiScreenCapturer::DriverTypes[] = { D3D_DRIVER_TYPE_HARDWARE };
const D3D_FEATURE_LEVEL DxgiScreenCapturer::FeatureLevels[] = {
	D3D_FEATURE_LEVEL_11_0,
	D3D_FEATURE_LEVEL_10_1,
	D3D_FEATURE_LEVEL_10_0,
	D3D_FEATURE_LEVEL_9_1 };
const UINT DxgiScreenCapturer::NumDriverTypes = ARRAYSIZE(DxgiScreenCapturer::DriverTypes);
const UINT DxgiScreenCapturer::NumFeatureLevels = ARRAYSIZE(DxgiScreenCapturer::FeatureLevels);

DxgiScreenCapturer::DxgiScreenCapturer()
	: d3dlib_{DynamicLib::Load(_T("d3d11.dll"))}
{}

DxgiScreenCapturer::~DxgiScreenCapturer()
{}

bool DxgiScreenCapturer::Capture()
{
	auto r = capture();
	dupl_->ReleaseFrame();
	return r;
}

bool DxgiScreenCapturer::capture()
{
	CComPtr<IDXGIResource> desktopResource;
	DXGI_OUTDUPL_FRAME_INFO frameInfo;
	int cnt = 4;
	HRESULT hr(E_FAIL);
	do
	{
		Sleep(100);
		hr = dupl_->AcquireNextFrame(250, &frameInfo, &desktopResource);
		if (SUCCEEDED(hr)) break;
		if (hr == DXGI_ERROR_WAIT_TIMEOUT)
		{
			continue;
		}
		else if (FAILED(hr))
		{
			break;
		}
	} while (--cnt > 0);

	if (FAILED(hr)) return false;
	// QI for ID3D11Texture2D
	CComPtr<ID3D11Texture2D> acquiredImage;
	hr = desktopResource->QueryInterface(IID_PPV_ARGS(&acquiredImage));
	if (FAILED(hr)) return false;
	desktopResource.Release();
	if (acquiredImage == nullptr) return false;
#ifdef RENDER_CURSOR_
	// Copy image into GDI drawing texture
	context_->CopyResource(gdiImage_, acquiredImage);
	CComPtr<IDXGISurface1> surface;
	hr = gdiImage_->QueryInterface(IID_PPV_ARGS(&surface));
	if (FAILED(hr)) return false;
	CURSORINFO cursoInfo = {0};
	cursoInfo.cbSize = sizeof(cursoInfo);
	if (GetCursorInfo(&cursoInfo) == TRUE)
	{
		if (cursoInfo.flags == CURSOR_SHOWING)
		{
			auto cursorPosition = cursoInfo.ptScreenPos;
			// auto cursorSize = cursoInfo.cbSize;
			HDC dc;
			surface->GetDC(FALSE, &dc);
			DrawIconEx(dc, cursorPosition.x, cursorPosition.y,
					cursoInfo.hCursor, 0, 0, 0, 0,
					DI_NORMAL | DI_DEFAULTSIZE);
			surface->ReleaseDC(nullptr);
		}
	}
	// Copy image into CPU access texture
	context_->CopyResource(destImage_, gdiImage_);
#else
	// Copy image into CPU access texture
	context_->CopyResource(destImage_, acquiredImage);
#endif
	// Copy from CPU access texture to bitmap buffer
	D3D11_MAPPED_SUBRESOURCE resource;
	UINT subresource = D3D11CalcSubresource(0, 0, 0);
	context_->Map(destImage_, subresource, D3D11_MAP_READ, 0, &resource);

	BITMAPINFO bmpInfo; // BMP 32 bpp
	ZeroMemory(&bmpInfo, sizeof(BITMAPINFO));
	bmpInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmpInfo.bmiHeader.biBitCount = 32;
	bmpInfo.bmiHeader.biCompression = BI_RGB;
	bmpInfo.bmiHeader.biWidth = duplDesc_.ModeDesc.Width;
	bmpInfo.bmiHeader.biHeight = duplDesc_.ModeDesc.Height;
	bmpInfo.bmiHeader.biPlanes = 1;
	bmpInfo.bmiHeader.biSizeImage = duplDesc_.ModeDesc.Width * duplDesc_.ModeDesc.Height * 4;
	std::unique_ptr<BYTE> pBuf(new BYTE[bmpInfo.bmiHeader.biSizeImage]);
	UINT bmpRowPitch = duplDesc_.ModeDesc.Width * 4;
	BYTE* sptr = reinterpret_cast<BYTE*>(resource.pData);
	BYTE* dptr = pBuf.get() + bmpInfo.bmiHeader.biSizeImage - bmpRowPitch;
	UINT rowPitch = std::min<UINT>(bmpRowPitch, resource.RowPitch);
	for (size_t h = 0; h < duplDesc_.ModeDesc.Height; ++h)
	{
		memcpy_s(dptr, bmpRowPitch, sptr, rowPitch);
		sptr += resource.RowPitch;
		dptr -= bmpRowPitch;
	}
	context_->Unmap(destImage_, 0);
	return SaveBitmapOnDisk(new Gdiplus::Bitmap(&bmpInfo, pBuf.get()));
}

bool DxgiScreenCapturer::Init()
{
	if (!d3dlib_)
	{
		return false;
	}
	D3D11CreateDeviceFuncType d3d11CreateDeviceFunc;
	d3d11CreateDeviceFunc = (D3D11CreateDeviceFuncType)d3dlib_->GetProcAddress("D3D11CreateDevice");
	if (d3d11CreateDeviceFunc == 0)
	{
		return false;
	}

	D3D_FEATURE_LEVEL featureLevel;
	HRESULT hr(E_FAIL);
	// Create device
	for (UINT DriverTypeIndex = 0; DriverTypeIndex < NumDriverTypes; ++DriverTypeIndex)
	{
		hr = d3d11CreateDeviceFunc(nullptr, DriverTypes[DriverTypeIndex], nullptr,
									0, FeatureLevels, NumFeatureLevels,
									D3D11_SDK_VERSION, &device_, &featureLevel,
									&context_);
		if (SUCCEEDED(hr))
		{
			break;
		}
		device_.Release();
		context_.Release();
	}

	if (FAILED(hr)) return false;
	Sleep(100);
	if (device_ == nullptr) return false;

	// Get DXGI device
	CComPtr<IDXGIDevice> dxgiDevice;
	hr = device_->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
	if (FAILED(hr)) return false;
	// Get DXGI adapter
	CComPtr<IDXGIAdapter> dxgiAdapter;
	hr = dxgiDevice->GetParent(__uuidof(IDXGIAdapter), reinterpret_cast<void**>(&dxgiAdapter));

	if (FAILED(hr)) return false;
	dxgiDevice.Release();
	// Get output
	UINT Output = 0;
	CComPtr<IDXGIOutput> dxgiOutput;
	hr = dxgiAdapter->EnumOutputs(Output, &dxgiOutput);
	if (FAILED(hr)) return false;
	dxgiAdapter.Release();
	// hr = dxgiOutput->GetDesc(&outputDesc_);
	// if (FAILED(hr)) return false;
	// QI for Output 1
	CComPtr<IDXGIOutput1> dxgiOutput1;
	hr = dxgiOutput->QueryInterface(IID_PPV_ARGS(&dxgiOutput1));
	if (FAILED(hr)) return false;
	dxgiOutput.Release();
	// Create desktop duplication
	hr = dxgiOutput1->DuplicateOutput(device_, &dupl_);
	if (FAILED(hr)) return false;
	dxgiOutput1.Release();
	// Create GUI drawing texture
	dupl_->GetDesc(&duplDesc_);
	D3D11_TEXTURE2D_DESC desc;
#ifdef RENDER_CURSOR_
	desc.Width = duplDesc_.ModeDesc.Width;
	desc.Height = duplDesc_.ModeDesc.Height;
	desc.Format = duplDesc_.ModeDesc.Format;
	desc.ArraySize = 1;
	desc.BindFlags = D3D11_BIND_FLAG::D3D11_BIND_RENDER_TARGET;
	desc.MiscFlags = D3D11_RESOURCE_MISC_GDI_COMPATIBLE;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;
	desc.MipLevels = 1;
	desc.CPUAccessFlags = 0;
	desc.Usage = D3D11_USAGE_DEFAULT;
	hr = device_->CreateTexture2D(&desc, nullptr, &gdiImage_);
	if (FAILED(hr)) return false;
	if (gdiImage_ == nullptr) return false;
#endif
	// Create CPU access texture
	desc.Width = duplDesc_.ModeDesc.Width;
	desc.Height = duplDesc_.ModeDesc.Height;
	desc.Format = duplDesc_.ModeDesc.Format;
	desc.ArraySize = 1;
	desc.BindFlags = 0;
	desc.MiscFlags = 0;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;
	desc.MipLevels = 1;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	desc.Usage = D3D11_USAGE_STAGING;
	hr = device_->CreateTexture2D(&desc, nullptr, &destImage_);
	if (FAILED(hr)) return false;
	if (destImage_ == nullptr) return false;
	return true;
}


class GdiScreenCapturer : public WinScreenCapturer
{
public:
	~GdiScreenCapturer();
	bool Capture() override;
protected:
	explicit GdiScreenCapturer();
	friend std::_Ref_count_obj<screencast::GdiScreenCapturer>;
private:
	HDC screen_;
};

GdiScreenCapturer::GdiScreenCapturer()
	: screen_{GetDC(nullptr)}
{
}

bool GdiScreenCapturer::Capture()
{
	EnumDisplayMonitors(screen_, nullptr, [](
		_In_ HMONITOR hMonitor,
		_In_ HDC      hdcMonitor,
		_In_ LPRECT   lprcMonitor,
		_In_ LPARAM   dwData) -> BOOL
	{
		int width = lprcMonitor->right - lprcMonitor->left;
		int height = lprcMonitor->bottom - lprcMonitor->top;
		HDC dc = CreateCompatibleDC(hdcMonitor);
		HBITMAP bmp = CreateCompatibleBitmap(hdcMonitor, width, height);
		HGDIOBJ oldObj = SelectObject(dc, bmp);
		if (0 != StretchBlt(dc, 0, 0, width, height, hdcMonitor,
			lprcMonitor->left, lprcMonitor->top, width, height, SRCCOPY))
		{
			SaveBitmapOnDisk(Gdiplus::Bitmap::FromHBITMAP(bmp, nullptr));
		}
		SelectObject(dc, oldObj);
		DeleteDC(dc);
		DeleteObject(bmp);
		return TRUE;
	}, 0);
	return true;
}

GdiScreenCapturer::~GdiScreenCapturer()
{
	ReleaseDC(nullptr, screen_);
}

// static
std::deque<std::wstring> ScreenCapturer::FileList;
std::shared_ptr<ScreenCapturer> ScreenCapturer::Instance()
{
	static auto dxgiCap = SingletonOf<DxgiScreenCapturer>();
	static bool dxgiSupported = dxgiCap->Init();
	if (dxgiSupported)
	{
		return dxgiCap;
	}
	{
		auto user32lib = std::unique_ptr<DynamicLib>(DynamicLib::Load(_T("user32.dll")));
		if (user32lib)
		{
			BOOL (WINAPI *setProcessDPIAwareFunc)(void) = nullptr;
			setProcessDPIAwareFunc = user32lib->GetProcAddress("SetProcessDPIAware");
			if (setProcessDPIAwareFunc != 0)
			{
				setProcessDPIAwareFunc();
			}
		}
	}
	return SingletonOf<GdiScreenCapturer>();
}

FARPROC DynamicLib::GetProcAddress(const char* proc) const
{
	return ::GetProcAddress(module_, proc);
}

DynamicLib::DynamicLib(const TCHAR* libname)
	: module_{0}
{
	module_ = LoadLibrary(libname);
}

DynamicLib::~DynamicLib()
{
	if (module_ != 0)
	{
		FreeLibrary(module_);
	}
}

} // namespace screencast
