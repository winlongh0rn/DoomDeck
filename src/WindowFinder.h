#pragma once

#include <windows.h>
#include <string>

class WindowFinder
{
public:
    static HWND FindProcessWindow(const std::string& executableName);

private:
    static BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam);
};