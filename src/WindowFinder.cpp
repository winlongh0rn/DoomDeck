#include "WindowFinder.h"

#include <psapi.h>

#pragma comment(lib, "psapi.lib")

namespace
{
    struct SearchData
    {
        std::string executableName;
        HWND result = nullptr;
    };
}

BOOL CALLBACK WindowFinder::EnumWindowsProc(HWND hwnd, LPARAM lParam)
{
    auto* data = reinterpret_cast<SearchData*>(lParam);

    if (!IsWindowVisible(hwnd))
    {
        return TRUE;
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);

    if (processId == 0)
    {
        return TRUE;
    }

    HANDLE process = OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
        FALSE,
        processId
    );

    if (!process)
    {
        return TRUE;
    }

    char exeName[MAX_PATH]{};

    if (GetModuleBaseNameA(
            process,
            nullptr,
            exeName,
            MAX_PATH))
    {
        if (_stricmp(exeName, data->executableName.c_str()) == 0)
        {
            data->result = hwnd;
            CloseHandle(process);
            return FALSE;
        }
    }

    CloseHandle(process);

    return TRUE;
}

HWND WindowFinder::FindProcessWindow(const std::string& executableName)
{
    SearchData data;
    data.executableName = executableName;

    EnumWindows(
        EnumWindowsProc,
        reinterpret_cast<LPARAM>(&data));

    return data.result;
}