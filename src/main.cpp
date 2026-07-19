#include <iostream>

#include <winrt/base.h>

#include "WindowFinder.h"
#include "CaptureEngine.h"

int main()
{
    winrt::init_apartment();

    HWND hwnd = WindowFinder::FindProcessWindow("uzdoom.exe");

    if (!hwnd)
    {
        std::cout << "Couldn't find UZDoom.\n";
        return 1;
    }

    CaptureEngine capture(hwnd);

    if (!capture.Initialize())
    {
        std::cout << "Capture initialization failed.\n";
        return 1;
    }

    std::cout << "Capture initialized successfully.\n";

    return 0;
}