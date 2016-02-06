
#include "ppapi/cpp/graphics_2d.h"
#include "ppapi/cpp/image_data.h"
#include "ppapi/cpp/instance.h"
#include "ppapi/cpp/module.h"
#include "ppapi/cpp/size.h"
#include "ppapi/utility/completion_callback_factory.h"

#include <sstream>
#include <Windows.h>

using namespace std;

namespace
{
	const int updateInterval = 30;

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
public:
	explicit WindowStreamInstance(PP_Instance instance)
		: pp::Instance(instance),
		callbackFactory(this),
		windowHandle(nullptr),
		pendingPaint(false),
		waitingForFlushCompletion(false),
		pixelBuffer(nullptr),
		currentPixelBufferSize(0)
	{
	}

	virtual ~WindowStreamInstance()
	{
		if (pixelBuffer != nullptr)
		{
			VirtualFree(pixelBuffer, 0, MEM_RELEASE);
			pixelBuffer = nullptr;
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
		ScheduleNextPaint();
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

		if (!IsWindow(windowHandle) || !IsWindowVisible(windowHandle))
		{
			return image;
		}

		RECT window_rectangle;
		GetClientRect(windowHandle, &window_rectangle);
		const int width = window_rectangle.right - window_rectangle.left;
		const int height = window_rectangle.bottom - window_rectangle.top;

		if (width < 1 || height < 1)
			return image;

		HDC hdc = GetDC(windowHandle);
		if (hdc == nullptr)
			return image;

		HDC hdcScreen = GetDC(0);
		if (hdcScreen == nullptr)
		{
			ReleaseDC(windowHandle, hdc);
			return image;
		}

		HDC hDest = CreateCompatibleDC(hdc);
		if (hDest == nullptr)
		{
			ReleaseDC(NULL, hdcScreen);
			ReleaseDC(windowHandle, hdc);
			return image;
		}

		HBITMAP bitmap = CreateCompatibleBitmap(hdcScreen, width, height);

		// Copy window contents into local device context
		HBITMAP hOldBitmap = (HBITMAP) SelectObject(hDest, bitmap);
		BitBlt(hDest, 0, 0, width, height, hdc, 0, 0, SRCCOPY);
		bitmap = (HBITMAP) SelectObject(hDest, hOldBitmap);

		BITMAPINFO bmpInfo;
		bmpInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bmpInfo.bmiHeader.biWidth = width;
		bmpInfo.bmiHeader.biHeight = -height;
		bmpInfo.bmiHeader.biPlanes = 1;
		bmpInfo.bmiHeader.biBitCount = 32;
		bmpInfo.bmiHeader.biCompression = BI_RGB;
		bmpInfo.bmiHeader.biSizeImage = 0;

		const int requiredPixelBufferSize = width * height * 4;
		// Allocate buffer of minimum size
		if (requiredPixelBufferSize > currentPixelBufferSize || pixelBuffer == nullptr)
		{
			if (pixelBuffer != nullptr)
			{
				VirtualFree(pixelBuffer, 0, MEM_RELEASE);
			}

			pixelBuffer = (int*) VirtualAlloc(0, requiredPixelBufferSize, MEM_COMMIT, PAGE_READWRITE);
			currentPixelBufferSize = requiredPixelBufferSize;
		}

		// Copy image data from bitmap into array
		GetDIBits(hDest, bitmap, 0, height, pixelBuffer, &bmpInfo, DIB_RGB_COLORS);

		// Insert the entire image if the width matches. Alpha is not modified here.
		if (width == size.width())
		{
			int imageSize = size.height() * size.width() * 4;
			memcpy(image.data(), pixelBuffer, imageSize < currentPixelBufferSize ? imageSize : currentPixelBufferSize);
		}
		else
		{
			// Remap each pixel individually and overwrite alpha.
			for (int y = 0; y < size.height(); ++y)
			{
				for (int x = 0; x < size.width(); ++x)
				{
					*image.GetAddr32(pp::Point(x, y)) = pixelBuffer[x + y * width] | 0xFF000000;
				}
			}
		}

		DeleteObject(bitmap);
		ReleaseDC(windowHandle, hdc);
		ReleaseDC(NULL, hdcScreen);
		DeleteDC(hDest);
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
				if (IsWindow(newWindow))
				{
					windowHandle = newWindow;
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
