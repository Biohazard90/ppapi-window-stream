
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
}

class WindowStreamInstance : public pp::Instance
{
	int lastStreamError;
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
		bitmapH(-1),
		sharedMemoryHandle(nullptr),
		allocatedSharedSize(0),
		sharedMemory(nullptr),
		defaultColor(0xFFAABBCC),
		lastStreamError(0)
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
	
	void ReadBackBufferFromSharedMemory(pp::ImageData &image, int w, int h)
	{
		if (!CanStream())
		{
			PaintError(image, "Stream memory not initializable.");
			return;
		}
		
		unsigned short *data = (unsigned short*)CreateSharedMemory(w, h);
		
		if (data == nullptr)
		{
			stringstream errorMessage;
			errorMessage << "Failed creating shared memory for stream: " << dec << (lastStreamError);
			PaintError(image, errorMessage.str().c_str());
		}
		else
		{
			// incoming data
			int wSrc = data[0];
			int hSrc = data[1];

			// expected data
			data[2] = w;
			data[3] = h;
			
			if (wSrc == w && hSrc == h)
			{
				data += 5;
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
				PaintError(image, "Stream size mismatching (try resizing the window).");
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
					*image.GetAddr32(pp::Point(x, y)) = defaultColor;
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
			else if (elems[0] == "setStreamName" && !elems[1].empty())
			{
				streamName = "Local\\";
				streamName += elems[1];
			}
			else if (elems[0] == "setDefaultColor" && !elems[1].empty())
			{
				defaultColor = strtoul(elems[1].c_str(), nullptr, 0);
			}
		}
	}
	
	bool CanStream()
	{
		return !streamName.empty();
	}
	
	void DestroySharedMemory()
	{
		if (sharedMemory != nullptr)
		{
			// Set destruction flag
			((unsigned short *)sharedMemory)[4] = 1;
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
		int requiredSize = w * h * 4 + 10;
		if (requiredSize <= allocatedSharedSize)
		{
			return sharedMemory;
		}
		
		DestroySharedMemory();
		
		//sharedMemoryHandle = CreateFileMapping(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, requiredSize, L"Local\\WallpaperEngineBackBufferMem");
		sharedMemoryHandle = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, requiredSize, streamName.c_str());
		
		if (sharedMemoryHandle != nullptr)
		{
			sharedMemory = MapViewOfFile(sharedMemoryHandle, FILE_MAP_ALL_ACCESS, 0, 0, requiredSize);
			if (sharedMemory != nullptr)
			{
				((unsigned short *)sharedMemory)[4] = 0;
				allocatedSharedSize = requiredSize;
			}
			else
			{
				lastStreamError = GetLastError();
			}
		}
		else
		{
			lastStreamError = GetLastError();
		}
		
		return sharedMemory;
	}
	
	HANDLE sharedMemoryHandle;
	int allocatedSharedSize;
	void *sharedMemory;
	string streamName;
	unsigned long defaultColor;

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
