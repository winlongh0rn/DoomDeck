#pragma once

#include <windows.h>

class CaptureEngine
{
public:
    explicit CaptureEngine(HWND hwnd);

    bool Initialize();

private:
    HWND m_hwnd;
};