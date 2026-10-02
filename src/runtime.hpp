#pragma once

#include <Windows.h>

namespace luminex {

void SetModuleHandle(HMODULE module);
DWORD WINAPI RuntimeThread(void*);
void Shutdown();

} // namespace luminex

