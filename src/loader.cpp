#include <Windows.h>
#include <TlHelp32.h>

#include <filesystem>
#include <iostream>
#include <string>

namespace {

DWORD FindRobloxProcess()
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;

    PROCESSENTRY32W entry{ sizeof(entry) };
    DWORD foundPid = 0;
    if (Process32FirstW(snapshot, &entry))
    {
        do
        {
            if (_wcsicmp(entry.szExeFile, L"RobloxPlayerBeta.exe") == 0)
            {
                if (foundPid != 0)
                {
                    CloseHandle(snapshot);
                    return 0;
                }
                foundPid = entry.th32ProcessID;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return foundPid;
}

std::wstring ErrorMessage(DWORD code)
{
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring message = length != 0 && buffer != nullptr ? std::wstring(buffer, length) : L"Unknown error";
    if (buffer != nullptr)
        LocalFree(buffer);
    return message;
}

int LoadIntoProcess(DWORD pid, const std::filesystem::path& dllPath, bool quiet)
{
    const DWORD access = PROCESS_CREATE_THREAD | PROCESS_QUERY_LIMITED_INFORMATION |
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE;
    HANDLE process = OpenProcess(access, FALSE, pid);
    if (process == nullptr)
    {
        const DWORD error = GetLastError();
        if (!quiet)
            std::wcerr << L"OpenProcess failed (" << error << L"): " << ErrorMessage(error);
        return 5;
    }

    wchar_t imagePath[32768]{};
    DWORD imagePathLength = static_cast<DWORD>(std::size(imagePath));
    if (!QueryFullProcessImageNameW(process, 0, imagePath, &imagePathLength) ||
        _wcsicmp(std::filesystem::path(imagePath).filename().c_str(), L"RobloxPlayerBeta.exe") != 0)
    {
        if (!quiet)
            std::wcerr << L"Refusing to load into a process that is not RobloxPlayerBeta.exe.\n";
        CloseHandle(process);
        return 6;
    }

    const std::wstring pathText = dllPath.wstring();
    const SIZE_T allocationSize = (pathText.size() + 1) * sizeof(wchar_t);
    void* remotePath = VirtualAllocEx(process, nullptr, allocationSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remotePath == nullptr)
    {
        const DWORD error = GetLastError();
        if (!quiet)
            std::wcerr << L"VirtualAllocEx failed (" << error << L"): " << ErrorMessage(error);
        CloseHandle(process);
        return 7;
    }

    SIZE_T bytesWritten = 0;
    if (!WriteProcessMemory(process, remotePath, pathText.c_str(), allocationSize, &bytesWritten) ||
        bytesWritten != allocationSize)
    {
        const DWORD error = GetLastError();
        if (!quiet)
            std::wcerr << L"WriteProcessMemory failed (" << error << L"): " << ErrorMessage(error);
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return 8;
    }

    const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    const auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(kernel32, "LoadLibraryW"));
    if (loadLibrary == nullptr)
    {
        if (!quiet)
            std::wcerr << L"Could not resolve LoadLibraryW.\n";
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return 9;
    }

    HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remotePath, 0, nullptr);
    if (thread == nullptr)
    {
        const DWORD error = GetLastError();
        if (!quiet)
            std::wcerr << L"CreateRemoteThread failed (" << error << L"): " << ErrorMessage(error);
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return 10;
    }

    const DWORD waitResult = WaitForSingleObject(thread, 10000);
    DWORD moduleResult = 0;
    GetExitCodeThread(thread, &moduleResult);
    CloseHandle(thread);
    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    CloseHandle(process);

    if (waitResult != WAIT_OBJECT_0 || moduleResult == 0)
    {
        if (!quiet)
            std::wcerr << L"LoadLibraryW did not complete successfully.\n";
        return 11;
    }

    if (!quiet)
        std::wcout << L"Luminex loaded into PID " << pid << L".\n";
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    DWORD pid = 0;
    std::filesystem::path requestedDll;
    if (argc == 1 || argc == 2)
    {
        pid = FindRobloxProcess();
        if (pid == 0)
        {
            std::wcerr << L"Start exactly one RobloxPlayerBeta.exe process before running Luminex.\n";
            return 2;
        }
        requestedDll = argc == 2
            ? std::filesystem::path(argv[1])
            : std::filesystem::path(argv[0]).parent_path() / L"LuminexRuntime.dll";
    }
    else if (argc == 3)
    {
        wchar_t* end = nullptr;
        const unsigned long parsedPid = wcstoul(argv[1], &end, 10);
        if (parsedPid == 0 || end == argv[1] || *end != L'\0')
        {
            std::wcerr << L"Invalid process ID.\n";
            return 3;
        }
        pid = static_cast<DWORD>(parsedPid);
        requestedDll = argv[2];
    }
    else
    {
        std::wcerr << L"Usage: LuminexLoader.exe [<absolute-dll-path> | <pid> <absolute-dll-path>]\n";
        return 2;
    }

    std::error_code pathError;
    const std::filesystem::path dllPath = std::filesystem::canonical(requestedDll, pathError);
    if (pathError || !std::filesystem::is_regular_file(dllPath))
    {
        std::wcerr << L"DLL does not exist: " << requestedDll << L"\n";
        return 4;
    }

    return LoadIntoProcess(pid, dllPath, false);
}
