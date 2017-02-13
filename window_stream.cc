
#include "ppapi/cpp/graphics_2d.h"
#include "ppapi/cpp/image_data.h"
#include "ppapi/cpp/instance.h"
#include "ppapi/cpp/module.h"
#include "ppapi/cpp/size.h"
#include "ppapi/utility/completion_callback_factory.h"
#include "ppapi/cpp/trusted/browser_font_trusted.h"

#include <sstream>
#include <Windows.h>
#include <d3d9.h>

#define min(x, y) (((x) <= (y))?(x):(y))
#define max(x, y) (((x) >= (y))?(x):(y))
#include <gdiplus.h>

using namespace std;

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "gdiplus.lib")

namespace
{
	const int updateInterval = 22;
	//ULONG_PTR m_gdiplusToken;

	vector<string> &split(const string &str, char delimiter, vector<string> &elements)
	{
		string token;
		stringstream stream(str);
		while (getline(stream, token, delimiter))
		{
			elements.push_back(token);
		}
		return elements;
	}
	
	HANDLE sharedMemoryHandle = nullptr;
	int allocatedSharedSize = 0;
	void *sharedMemory = nullptr;
	
	void DestroySharedMemory()
	{
		if (sharedMemory != nullptr)
		{
			UnmapViewOfFile(sharedMemory);
			sharedMemory = nullptr;
		}
		
		if (sharedMemoryHandle != nullptr)
		{
			CloseHandle(sharedMemoryHandle);
			sharedMemoryHandle = nullptr;
		}
		
		allocatedSharedSize = 0;
	}
	
	void *CreateSharedMemory(int w, int h)
	{
		int requiredSize = w * h * 4 + 4;
		if (requiredSize <= allocatedSharedSize)
		{
			return sharedMemory;
		}
		
		DestroySharedMemory();
		
		sharedMemoryHandle = CreateFileMapping(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, requiredSize, L"Local\\WallpaperEngineBackBufferMem");
		
		if (sharedMemoryHandle != nullptr)
		{
			sharedMemory = MapViewOfFile(sharedMemoryHandle, FILE_MAP_READ, 0, 0, requiredSize);
			if (sharedMemory != nullptr)
			{
				allocatedSharedSize = requiredSize;
			}
		}
		
		return sharedMemory;
	}
}

class WindowStreamInstance : public pp::Instance
{
public:
	explicit WindowStreamInstance(PP_Instance instance)
		: pp::Instance(instance),
		callbackFactory(this),
		windowHandle(nullptr),
		pendingPaint(false),
		waitingForFlushCompletion(false),
		pixelBuffer(nullptr),
		currentPixelBufferSize(0),
		bitmap(nullptr),
		bitmapW(-1),
		bitmapH(-1)
	{
		//Gdiplus::GdiplusStartupInput gdiplusStartupInput;
		//Gdiplus::GdiplusStartup(&m_gdiplusToken, &gdiplusStartupInput, NULL);
	}

	virtual ~WindowStreamInstance()
	{
		DestroySharedMemory();
		
		//Gdiplus::GdiplusShutdown(m_gdiplusToken);
		
		if (pixelBuffer != nullptr)
		{
			VirtualFree(pixelBuffer, 0, MEM_RELEASE);
			pixelBuffer = nullptr;
		}
		//if (bitmap != nullptr)
		{
			//DeleteObject(bitmap);
		}
	}

	virtual bool Init(uint32_t argc, const char *argn [], const char *argv [])
	{
		ScheduleNextPaint();
		return true;
	}

	virtual void DidChangeView(const pp::Rect &position, const pp::Rect &clip)
	{
		if (position.size() != size)
		{
			size = position.size();
			deviceContext = pp::Graphics2D(this, size, false);
			if (!BindGraphics(deviceContext))
			{
				return;
			}
		}

		// Trigger repaint after view changed
		Paint();
	}

private:
	void ScheduleNextPaint()
	{
		pp::Module::Get()->core()->CallOnMainThread(updateInterval,
			callbackFactory.NewCallback(&WindowStreamInstance::OnTimer), 0);
	}

	void OnTimer(int32_t)
	{
		if (windowHandle != nullptr)
		{
			ScheduleNextPaint();
		}
		Paint();
	}

	void DidFlush(int32_t result)
	{
		waitingForFlushCompletion = false;
		if (pendingPaint)
		{
			Paint();
		}
	}

	void Paint()
	{
		if (waitingForFlushCompletion)
		{
			pendingPaint = true;
			return;
		}

		pendingPaint = false;
		if (size.IsEmpty())
		{
			return;
		}

		pp::ImageData image = PerformPaint();
		if (!image.is_null())
		{
			waitingForFlushCompletion = true;
			deviceContext.ReplaceContents(&image);
			deviceContext.Flush(callbackFactory.NewCallback(&WindowStreamInstance::DidFlush));
		}
	}
	
	void PaintError(pp::ImageData &image, string errorMessage, bool clear = true)
	{
		if (clear)
		{
			for (int y = 0; y < size.height(); y++)
			{
				for (int x = 0; x < size.width(); x++)
				{
					*image.GetAddr32(pp::Point(x, y)) = 0xFFFFFFFF;
				}
			}
		}
		pp::BrowserFontDescription desc;
		pp::BrowserFont_Trusted default_font(this, desc);
		default_font.DrawSimpleText(
			&image, errorMessage.c_str(),
			pp::Point(10, 20), 0xFF000000);
	}
	
	void StreamDXImage(pp::ImageData &image, int w, int h)
	{
		static IDirect3D9 *obj = nullptr;
		static IDirect3DDevice9 *dev = nullptr;
		
		if (obj == nullptr && w > 400 && h > 400)
		{
			HRESULT res;
			obj = Direct3DCreate9(D3D_SDK_VERSION);

			//WINASSERT(obj != nullptr);
			if (obj == nullptr)
			{
				//Error("Failed to create dx object.\n");
				PaintError(image, "Failed to create D3D object.");
			}

			D3DCAPS9 caps;
			res = obj->GetDeviceCaps(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &caps);
			//WINASSERT(res == S_OK);

			// caps.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT // check for SM 2?
			if ((caps.DevCaps & D3DPTEXTURECAPS_POW2) != 0)
			{
				//FatalError("Device does not support non power of two textures!");
				PaintError(image, "D3D device not supported.");
			}

			D3DPRESENT_PARAMETERS d3dpp;
			d3dpp.AutoDepthStencilFormat = D3DFMT_D16; //D3DFMT_D24S8;
			d3dpp.BackBufferCount = 1;
			//d3dpp->BackBufferFormat = D3DFMT_A8R8G8B8;
			d3dpp.BackBufferFormat = D3DFMT_A8R8G8B8;
			d3dpp.BackBufferHeight = h;
			d3dpp.BackBufferWidth = w;
			d3dpp.Windowed = true;
			d3dpp.EnableAutoDepthStencil = true; //D3DFMT_D24S8;
			d3dpp.Flags = 0;
			//d3dpp->Flags = D3DPRESENTFLAG_DEVICECLIP;
			d3dpp.FullScreen_RefreshRateInHz = D3DPRESENT_RATE_DEFAULT;
			d3dpp.hDeviceWindow = windowHandle;
			d3dpp.MultiSampleQuality = 0;
			d3dpp.MultiSampleType = D3DMULTISAMPLE_NONE;
			d3dpp.PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;
			d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
		
			res = obj->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, windowHandle, D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_PUREDEVICE, &d3dpp, &dev);
			if (dev == nullptr)
			{
				PaintError(image, "Failed to create D3D device.");
			}
		}
		
		if (dev != nullptr)
		{
			static IDirect3DSurface9 *surface = nullptr;
			if (surface == nullptr)
			{
				dev->CreateOffscreenPlainSurface(1920, 1080, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &surface, nullptr);
				if (surface == nullptr)
				{
					PaintError(image, "Failed to create D3D surface.");
				}
			}
			
			if (surface != nullptr)
			{
				if (image.size().width() != w || image.size().height() != h)
				{
					PaintError(image, "Mismatching sizes.");
				}
				else
				{
					HRESULT res = dev->GetFrontBufferData(0, surface);
					
					if (FAILED(res))
					{
						stringstream ss;
						ss << "Failed capturing screen with error: ";
						switch (res)
						{
							case D3DERR_DRIVERINTERNALERROR: ss << "D3DERR_DRIVERINTERNALERROR"; break;
							case D3DERR_DEVICELOST: ss << "D3DERR_DEVICELOST"; break;
							case D3DERR_INVALIDCALL: ss << "D3DERR_INVALIDCALL"; break;
							default: ss << "unknown";
						}
						
						PaintError(image, ss.str());
					}
					else
					{
						D3DLOCKED_RECT rect;
						if (SUCCEEDED(surface->LockRect(&rect, 0, 0)))
						{
							//if (rect.Pitch == width * 4)
							{
								//memcpy(rect.pBits, imageBuffer, imageSizeInBytes);
							}
							
							if (rect.pBits == nullptr)
							{
								PaintError(image, "No bits set.");
							}
							else
							{
								memcpy(image.data(), rect.pBits, w * h * 4);
								stringstream ss;
								char a = ((char*)(rect.pBits))[0];
								char b = ((char*)(rect.pBits))[1];
								char c = ((char*)(rect.pBits))[2];
								char d = ((char*)(rect.pBits))[3];
								ss << "Streaming via D3D: " << to_string(a) << "," << to_string(b) << "," << to_string(c) << "," << to_string(d);
								PaintError(image, ss.str().c_str(), false);
							}
							
							surface->UnlockRect();
							
							//PaintError(image, "Streaming via D3D.", false);
						}
					}
				}
			}
		}
	}
	
	void ReadBackBufferFromSharedMemory(pp::ImageData &image, int w, int h)
	{
		unsigned short *data = (unsigned short*)CreateSharedMemory(w, h);
		
		if (data == nullptr)
		{
			PaintError(image, "Failed creating shared memory for stream.");
		}
		else
		{
			int wSrc = data[0];
			int hSrc = data[1];
			
			if (wSrc == w && hSrc == h)
			{
				data += 2;
				memcpy(image.data(), data, w * h * 4);
				
				//unsigned char c = *(((unsigned char*)data));
				//unsigned char c1 = *(((unsigned char*)data) + 1);
				//unsigned char c2 = *(((unsigned char*)data) + 2);
				//unsigned char c3 = *(((unsigned char*)data) + 3);
				//stringstream s;
				//s << "Yo, first byte: " << to_string(c) << ", " << to_string(c1) << ", " << to_string(c2) << ", " << to_string(c3);
				//PaintError(image, s.str(), false);
				//PaintError(image, "Streaming from memory.", false);
			}
			else
			{
				PaintError(image, "Stream size mismatching.");
			}
		}
		
		//bool failed = true;
		//backBufferMutex.lock();
		//if (w == backBufferWidth && h == backBufferHeight)
		//{
		//	failed = false;
		//	
		//	memcpy(image.data(), backBufferData, w * h);
		//}
		//backBufferMutex.unlock();
		//
		//if (failed)
		//{
		//	stringstream str;
		//	str << "Failed streaming back buffer: " << backBufferWidth << ", " << backBufferHeight << ", " << w << ", " << h;
		//	PaintError(image, str.str());
		//}
	}

	pp::ImageData PerformPaint()
	{
		pp::ImageData image(this, PP_IMAGEDATAFORMAT_BGRA_PREMUL, size, false);
		if (image.is_null())
		{
			return image;
		}

		// No window handle set, render a default color.
		if (windowHandle == nullptr)
		{
			for (int y = 0; y < size.height(); y++)
			{
				for (int x = 0; x < size.width(); x++)
				{
					*image.GetAddr32(pp::Point(x, y)) = 0xFFAABBCC;
				}
			}
			return image;
		}

		if (!IsWindow(windowHandle))
		{
			PaintError(image, "No window handle.");
			return image;
		}
		
		if (!IsWindowVisible(windowHandle))
		{
			PaintError(image, "Window invisible.");
			return image;
		}

		RECT window_rectangle;
		GetClientRect(windowHandle, &window_rectangle);
		const int width = window_rectangle.right - window_rectangle.left;
		const int height = window_rectangle.bottom - window_rectangle.top;
		
		if (width < 1 || height < 1)
		{
			PaintError(image, "Width or height < 1.");
			return image;
		}
		
		//StreamDXImage(image, width, height);
		//return image;
		ReadBackBufferFromSharedMemory(image, size.width(), size.height());
		return image;

		HDC hdc = GetDC(windowHandle);
		if (hdc == nullptr)
		{
			PaintError(image, "Can't get window DC.");
			return image;
		}

		//HDC hdcScreen = GetDC(0);
		//if (hdcScreen == nullptr)
		//{
		//	PaintError(image, "Can't get screen DC.");
		//	ReleaseDC(windowHandle, hdc);
		//	return image;
		//}

		HDC hDest = CreateCompatibleDC(hdc);
		if (hDest == nullptr)
		{
			PaintError(image, "Can't create compatible DC.");
			//ReleaseDC(NULL, hdcScreen);
			ReleaseDC(windowHandle, hdc);
			return image;
		}
		
		
		if (bitmap != nullptr && (bitmapW != width || bitmapH != height))
		{
			DeleteObject(bitmap);
			bitmap = nullptr;
		}
		
		const int requiredPixelBufferSize = width * height * 4;
		if (requiredPixelBufferSize < 1)
		{
			PaintError(image, "Pixel buffer size is 0.");
			DeleteDC(hDest);
			ReleaseDC(windowHandle, hdc);
			//ReleaseDC(NULL, hdcScreen);
			return image;
		}

		//HBITMAP bitmap = CreateCompatibleBitmap(hdcScreen, width, height);
		static int *pbits=0;

		BITMAPINFO bmpInfo = { 0 };
		bmpInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bmpInfo.bmiHeader.biWidth = width;
		bmpInfo.bmiHeader.biHeight = -height;
		bmpInfo.bmiHeader.biPlanes = 1;
		bmpInfo.bmiHeader.biBitCount = 32;
		bmpInfo.bmiHeader.biCompression = BI_RGB;
		bmpInfo.bmiHeader.biSizeImage = (width * height * bmpInfo.bmiHeader.biBitCount) / 8;
		
		if (bitmap == nullptr)
		{
			pbits = nullptr;
			bitmapW = width;
			bitmapH = height;
			//bitmap = CreateCompatibleBitmap(hdcScreen, width, height);
			//bitmap = CreateCompatibleBitmap(hDest, width, height);
			bitmap=CreateDIBSection(hDest,&bmpInfo,DIB_RGB_COLORS,(void**)&pbits,0,0);
			if (bitmap == nullptr)
			{
				PaintError(image, "Can't create compatible bitmap.");
				DeleteDC(hDest);
				ReleaseDC(windowHandle, hdc);
				//ReleaseDC(NULL, hdcScreen);
				return image;
			}
		}
		
		bool hadError = false;
		int error = 0;
		SetLastError(0);

		// Copy window contents into local device context
		HBITMAP hOldBitmap = (HBITMAP) SelectObject(hDest, bitmap);
		error = GetLastError();
		if (error)
		{
			hadError = true;
			stringstream str;
			str << "Failed SelectObject with " << error;
			PaintError(image, str.str().c_str());
		}
		
      //HWND oldWindow = GetForegroundWindow();
      //SetForegroundWindow(windowHandle);
		if (BitBlt(hDest, 0, 0, width, height, hdc, 0, 0, SRCCOPY) == FALSE)
		//if (PrintWindow(windowHandle, hDest, 0) == FALSE)
		{
			hadError = true;
			error = GetLastError();
			stringstream str;
			str << "Failed BitBlt with " << error;
			PaintError(image, str.str().c_str());
		}
      //SetForegroundWindow(oldWindow);
		
		(HBITMAP) SelectObject(hDest, hOldBitmap);
		error = GetLastError();
		if (error)
		{
			hadError = true;
			stringstream str;
			str << "Failed SelectObject 2 with " << error;
			PaintError(image, str.str().c_str());
		}

		// Allocate buffer of minimum size
		if (requiredPixelBufferSize > currentPixelBufferSize || pixelBuffer == nullptr)
		{
			if (pixelBuffer != nullptr)
			{
				VirtualFree(pixelBuffer, 0, MEM_RELEASE);
			}

			pixelBuffer = (int*) VirtualAlloc(0, requiredPixelBufferSize, MEM_COMMIT, PAGE_READWRITE);
			currentPixelBufferSize = requiredPixelBufferSize;
			
			if (pixelBuffer == nullptr)
			{
				PaintError(image, "Failed allocating pixel buffer.");
				DeleteDC(hDest);
				ReleaseDC(windowHandle, hdc);
				//ReleaseDC(NULL, hdcScreen);
				DeleteObject(bitmap);
				return image;
			}
		}
		
		if(!GdiFlush())
		{
			PaintError(image, "Failed to flush GDI.");
			DeleteDC(hDest);
			ReleaseDC(windowHandle, hdc);
			DeleteObject(bitmap);
			return image;
		}
		//HANDLE hDIB = GlobalAlloc(GHND,requiredPixelBufferSize); 
		//int *lpbitmap = (int *)GlobalLock(hDIB); 

		// Copy image data from bitmap into array
		//int copyResult = GetDIBits(hDest, bitmap, 0, height, pixelBuffer, &bmpInfo, DIB_RGB_COLORS);

		// Insert the entire image if the width matches. Alpha is not modified here.
		if (width == size.width())
		{
			int imageSize = size.height() * size.width() * 4;
			memcpy(image.data(), pbits, imageSize < currentPixelBufferSize ? imageSize : currentPixelBufferSize);
		}
		else if ( pbits != nullptr )
		{
			PaintError(image, "Image size mismatch, discarding.");
			// Remap each pixel individually and overwrite alpha.
			//for (int y = 0; y < 100; ++y)
			//{
			//	for (int x = 0; x < 100; ++x)
			//	{
			//		*image.GetAddr32(pp::Point(x, y)) = pbits[x + y * width] | 0xFF000000;
			//	}
			//}
		}
		else
		{
			PaintError(image, "Buffer not available.");
		}

		// if (copyResult == ERROR_INVALID_PARAMETER || copyResult == 0)
		// {
		// 	PaintError(image, "Failed copying window bits.");
		// }
		// else
		//if (!hadError)
		{
			//PaintError(image, "Streaming...", false);
		}
		
		//GlobalUnlock(hDIB);    
		//GlobalFree(hDIB);

		DeleteDC(hDest);
		ReleaseDC(windowHandle, hdc);
		//ReleaseDC(NULL, hdcScreen);
		//DeleteObject(bitmap);
		//bitmap = nullptr;
		return image;
	}

	virtual void HandleMessage(const pp::Var& messageData)
	{
		if (messageData.is_string())
		{
			string strMessage(messageData.AsString());
			vector<string> elems;
			split(strMessage, ' ', elems);

			if (elems.size() < 2)
				return;

			if (elems[0] == "setWindowHandle")
			{
				HWND newWindow = (HWND) stoull(elems[1]);
				windowHandle = newWindow;
				if (windowHandle != nullptr)
				{
					ScheduleNextPaint();
				}
			}
		}
	}

	pp::Size size;
	pp::Graphics2D deviceContext;
	pp::CompletionCallbackFactory<WindowStreamInstance> callbackFactory;

	HWND windowHandle;

	bool pendingPaint;
	bool waitingForFlushCompletion;

	int *pixelBuffer;
	int currentPixelBufferSize;
	HBITMAP bitmap;
	int bitmapW, bitmapH;
};

class WindowStreamModule : public pp::Module
{
public:
	virtual pp::Instance *CreateInstance(PP_Instance instance)
	{
		return new WindowStreamInstance(instance);
	}
};

namespace pp
{
	Module *CreateModule()
	{
		return new WindowStreamModule();
	}
}
