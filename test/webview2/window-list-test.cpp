#include <Windows.h>
#include <obs.h>
#include <util/windows/window-helpers.h>

#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace {
constexpr wchar_t classA[] = L"OBSWindowListRegressionA";
constexpr wchar_t classB[] = L"OBSWindowListRegressionB";
constexpr wchar_t title[] = L"OBS window list regression # : same label";

int fixtureWindows(const wchar_t *readyName, const wchar_t *stopName)
{
	const auto ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, readyName);
	const auto stop = OpenEventW(SYNCHRONIZE, FALSE, stopName);
	if (!ready || !stop) return 2;
	const auto instance = GetModuleHandleW(nullptr);
	for (const auto *name : {classA, classB}) {
		WNDCLASSW cls{};
		cls.lpfnWndProc = DefWindowProcW;
		cls.hInstance = instance;
		cls.lpszClassName = name;
		if (!RegisterClassW(&cls)) return 3;
	}
	std::vector<HWND> windows;
	for (const auto *name : {classA, classA, classB}) {
		// Visible to native enumeration, outside the desktop and never activated.
		const auto window = CreateWindowExW(WS_EX_NOACTIVATE, name, title, WS_POPUP | WS_VISIBLE,
			-32000, -32000, 80, 40, nullptr, nullptr, instance, nullptr);
		if (!window) return 4;
		windows.push_back(window);
	}
	SetEvent(ready);
	WaitForSingleObject(stop, 60000);
	for (const auto window : windows) DestroyWindow(window);
	CloseHandle(ready);
	CloseHandle(stop);
	return 0;
}

bool onlyFixture(const char *, const char *windowClass, const char *)
{
	return strcmp(windowClass, "OBSWindowListRegressionA") == 0 ||
	       strcmp(windowClass, "OBSWindowListRegressionB") == 0;
}
}

int wmain(int argc, wchar_t **argv)
{
	if (argc == 4 && std::wstring(argv[1]) == L"--windows") return fixtureWindows(argv[2], argv[3]);
	wchar_t executable[MAX_PATH]{};
	GetModuleFileNameW(nullptr, executable, MAX_PATH);
	const auto suffix = std::to_wstring(GetCurrentProcessId());
	const auto readyName = L"Local\\OBSWindowListReady" + suffix;
	const auto stopName = L"Local\\OBSWindowListStop" + suffix;
	const auto ready = CreateEventW(nullptr, TRUE, FALSE, readyName.c_str());
	const auto stop = CreateEventW(nullptr, TRUE, FALSE, stopName.c_str());
	std::wstring command = L"\"" + std::wstring(executable) + L"\" --windows " + readyName + L" " + stopName;
	STARTUPINFOW startup{sizeof(startup)};
	PROCESS_INFORMATION process{};
	if (!ready || !stop || !CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
		CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) return 2;
	if (WaitForSingleObject(ready, 10000) != WAIT_OBJECT_0) {
		SetEvent(stop);
		WaitForSingleObject(process.hProcess, 5000);
		return 3;
	}
	int failures = 0;
	const auto check = [&](bool passed, const char *message) {
		std::cout << (passed ? "PASS " : "FAIL ") << message << '\n';
		if (!passed) ++failures;
	};
	const auto properties = obs_properties_create();
	const auto list = obs_properties_add_list(properties, "window", "Window", OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_STRING);
	ms_fill_window_list(list, INCLUDE_MINIMIZED, onlyFixture);
	const auto count = obs_property_list_item_count(list);
	std::set<std::string> labels, identities;
	for (size_t i = 0; i < count; ++i) {
		labels.emplace(obs_property_list_item_name(list, i));
		identities.emplace(obs_property_list_item_string(list, i));
	}
	check(count == 2, "duplicate HWNDs produce one row per persisted window selector");
	check(identities.size() == 2 && labels.size() == 1,
		"same labels with different window classes remain independently selectable");
	if (!identities.empty()) {
		const auto selected = *identities.begin();
		check(selected.find("#22") != std::string::npos && selected.find("#3A") != std::string::npos,
			"selector keeps escaped title identity");
		const auto settings = obs_data_create();
		obs_data_set_string(settings, "window", selected.c_str());
		check(!ms_check_window_property_setting(properties, list, settings, "window", 0) &&
			selected == obs_data_get_string(settings, "window"), "selected window value remains valid and unchanged");
		obs_data_release(settings);
		ms_fill_window_list(list, INCLUDE_MINIMIZED, onlyFixture);
		check(obs_property_list_item_count(list) == 2,
			"refresh does not append selectors already present in the property list");
	}
	obs_properties_destroy(properties);
	SetEvent(stop);
	WaitForSingleObject(process.hProcess, 5000);
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	CloseHandle(ready);
	CloseHandle(stop);
	return failures ? 1 : 0;
}
