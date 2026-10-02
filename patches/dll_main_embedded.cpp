/*
 * Luminex runtime-only ReShade entry point.
 *
 * This deliberately performs no API hooking and is initialized explicitly by
 * Luminex after LoadLibrary has completed.
 */

#ifdef RESHADE_EMBEDDED_LIBRARY

#include "dll_log.hpp"
#include "input.hpp"
#include "runtime.hpp"
#include <Windows.h>
#include <filesystem>

extern HMODULE g_module_handle;
extern std::filesystem::path g_reshade_dll_path;
extern std::filesystem::path g_reshade_base_path;
extern std::filesystem::path g_target_executable_path;

extern std::filesystem::path get_module_path(HMODULE module);

extern "C" __declspec(dllexport) bool LuminexInitializeReShadeRuntime(const wchar_t *base_path)
{
	HMODULE module = nullptr;
	if (!GetModuleHandleExW(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&LuminexInitializeReShadeRuntime), &module))
		return false;

	g_module_handle = module;
	g_reshade_dll_path = get_module_path(module);
	g_target_executable_path = get_module_path(nullptr);
	g_reshade_base_path = (base_path != nullptr && *base_path != L'\0') ?
		std::filesystem::path(base_path) : g_reshade_dll_path.parent_path();

	std::error_code ec;
	std::filesystem::create_directories(g_reshade_base_path, ec);
	const auto diagnostics_path = g_reshade_base_path / L"Config" / L"Diagnostics.ini";
	if (GetPrivateProfileIntW(L"Diagnostics", L"SaveLogs", 1, diagnostics_path.c_str()) != 0)
		reshade::log::open_log_file(g_reshade_base_path / L"ReShade.log", ec);
	return true;
}

extern "C" __declspec(dllexport) bool LuminexReShadeHandleWindowMessage(const MSG *message)
{
	return message != nullptr && reshade::input::handle_window_message(message);
}

extern "C" __declspec(dllexport) bool LuminexReShadeIsOverlayOpen(reshade::api::effect_runtime *runtime)
{
	return runtime != nullptr && static_cast<reshade::runtime *>(runtime)->is_overlay_open_for_luminex();
}

extern "C" __declspec(dllexport) void LuminexReShadeBeginResize(reshade::api::effect_runtime *runtime)
{
	if (runtime != nullptr)
		static_cast<reshade::runtime *>(runtime)->on_reset();
}

extern "C" __declspec(dllexport) bool LuminexReShadeEndResize(reshade::api::effect_runtime *runtime)
{
	return runtime != nullptr && static_cast<reshade::runtime *>(runtime)->on_init(true);
}

#endif
