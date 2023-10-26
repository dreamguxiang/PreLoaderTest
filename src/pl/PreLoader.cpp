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
    pl::init();
    return TRUE;
}
