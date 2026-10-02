#include "runtime.hpp"

#include <Windows.h>

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);
        luminex::SetModuleHandle(module);
        if (HANDLE thread = CreateThread(nullptr, 0, luminex::RuntimeThread, nullptr, 0, nullptr))
            CloseHandle(thread);
    }
    else if (reason == DLL_PROCESS_DETACH && reserved == nullptr)
    {
        // Only perform explicit unload cleanup. During process termination,
        // Windows is already tearing down threads and modules while holding
        // the loader lock; disabling hooks and ImGui there can stall exit.
        luminex::Shutdown();
    }
    return TRUE;
}
