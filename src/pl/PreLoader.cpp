#include "pl/PreLoader.h"

#include <filesystem>
#include <set>
#include <string>

#include "pl/internal/Logger.h"
#include "pl/internal/StringUtils.h"

#include <windows.h>

#include <consoleapi2.h>

using namespace pl::utils;
using namespace std::filesystem;
using std::string;
using std::wstring;

constexpr const int MAX_PATH_LENGTH = 8192;

std::set<std::string> preloadList;

namespace pl {

void init() {
    pl::symbol_provider::init();
}
} // namespace pl


[[maybe_unused]] BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call != DLL_PROCESS_ATTACH) return TRUE;

    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);

    DWORD mode;
    GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode);
    SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    // For #683, Change CWD to current module path
    auto buffer = new wchar_t[MAX_PATH];
    GetModuleFileNameW(hModule, buffer, MAX_PATH);
    std::wstring path(buffer);
    auto         cwd = path.substr(0, path.find_last_of('\\'));
    SetCurrentDirectoryW(cwd.c_str());
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOALIGNMENTFAULTEXCEPT);

    pl::init();
    return TRUE;
}
