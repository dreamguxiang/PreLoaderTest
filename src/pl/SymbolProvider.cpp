#include "pl/SymbolProvider.h"

#include <cstdio>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <PDB.h>
#include <PDB_DBIStream.h>
#include <PDB_InfoStream.h>
#include <PDB_RawFile.h>

#include <parallel_hashmap/phmap.h>

#include "pl/internal/ApHash.h"
#include "pl/internal/FakeSymbol.h"
#include "pl/internal/Logger.h"
#include "pl/internal/MemoryFile.h"
#include "pl/internal/PdbUtils.h"
#include "pl/internal/WindowsUtils.h"

#include <Windows.h>

using std::string, std::string_view;
using std::unordered_map, std::unordered_multimap, std::vector;

using namespace pl::utils;

bool             fastDlsymState = false;
bool             initialized    = false;
static uintptr_t imageBaseAddr;

std::mutex                                  dlsymLock{};
phmap::flat_hash_map<string, int, ap_hash>* funcMap;
unordered_multimap<int, string*>*           rvaMap;

void testFuncMap() {
    return;
    // TODO: Update check symbol
    constexpr auto TEST_SYMBOL      = "?initializeLogging@DedicatedServer@@AEAAXXZ";
    auto           handle           = GetModuleHandle(nullptr);
    auto           exportedFuncAddr = GetProcAddress(handle, TEST_SYMBOL);
    void*          symDbFn          = nullptr;
    auto           iter             = funcMap->find(string(TEST_SYMBOL));
    if (iter != funcMap->end()) { symDbFn = (void*)(imageBaseAddr + iter->second); }
    if (symDbFn != exportedFuncAddr) {
        Error("Could not find critical symbol in pdb");
        fastDlsymState = false;
    }
}

void initFastDlsym(const PDB::RawFile& rawPdbFile, const PDB::DBIStream& dbiStream) {
    funcMap                                          = new phmap::flat_hash_map<string, int, ap_hash>;
    const PDB::ImageSectionStream imageSectionStream = dbiStream.CreateImageSectionStream(rawPdbFile);
    const PDB::CoalescedMSFStream symbolRecordStream = dbiStream.CreateSymbolRecordStream(rawPdbFile);
    const PDB::PublicSymbolStream publicSymbolStream = dbiStream.CreatePublicSymbolStream(rawPdbFile);

    const PDB::ArrayView<PDB::HashRecord> hashRecords = publicSymbolStream.GetRecords();

    for (const PDB::HashRecord& hashRecord : hashRecords) {
        const PDB::CodeView::DBI::Record* record = publicSymbolStream.GetRecord(symbolRecordStream, hashRecord);
        const uint32_t                    rva =
            imageSectionStream.ConvertSectionOffsetToRVA(record->data.S_PUB32.section, record->data.S_PUB32.offset);
        if (rva == 0u) continue;
        funcMap->emplace(record->data.S_PUB32.name, rva);

        auto fake = pl::fake_symbol::getFakeSymbol(record->data.S_PUB32.name);
        if (fake.has_value()) funcMap->emplace(fake.value(), rva);

        // MCVAPI
        fake = pl::fake_symbol::getFakeSymbol(record->data.S_PUB32.name, true);
        if (fake.has_value()) funcMap->emplace(fake.value(), rva);
    }

    const PDB::ModuleInfoStream moduleInfoStream = dbiStream.CreateModuleInfoStream(rawPdbFile);

    const PDB::ArrayView<PDB::ModuleInfoStream::Module> modules = moduleInfoStream.GetModules();

    for (const PDB::ModuleInfoStream::Module& module : modules) {
        if (!module.HasSymbolStream()) continue;
        const PDB::ModuleSymbolStream moduleSymbolStream = module.CreateSymbolStream(rawPdbFile);
        moduleSymbolStream.ForEachSymbol([&imageSectionStream](const PDB::CodeView::DBI::Record* record) {
            if (record->header.kind != PDB::CodeView::DBI::SymbolRecordKind::S_LPROC32) return;
            const uint32_t rva = imageSectionStream.ConvertSectionOffsetToRVA(
                record->data.S_LPROC32.section,
                record->data.S_LPROC32.offset
            );
            string name = record->data.S_LPROC32.name;
            if (name.find("lambda") != std::string::npos) { funcMap->emplace(record->data.S_LPROC32.name, rva); }
        });
    }
    fastDlsymState = true;
    testFuncMap();
    fflush(stdout);
}

void initReverseLookup() {
    rvaMap = new unordered_multimap<int, string*>(funcMap->size());
    for (auto& pair : *funcMap) { rvaMap->insert({pair.second, (string*)&pair.first}); }
}

namespace pl::symbol_provider {

void init() {
    if (initialized) return;
    const wchar_t* const pdbPath = LR"(./bedrock_server.pdb)";
    MemoryFile           pdbFile = MemoryFile::Open(pdbPath);
    if (!pdbFile.baseAddress) {
        Error("bedrock_server.pdb not found");
        return;
    }
    if (handleError(PDB::ValidateFile(pdbFile.baseAddress))) {
        MemoryFile::Close(pdbFile);
        return;
    }
    const PDB::RawFile rawPdbFile = PDB::CreateRawFile(pdbFile.baseAddress);
    if (handleError(PDB::HasValidDBIStream(rawPdbFile))) {
        MemoryFile::Close(pdbFile);
        return;
    }
    const PDB::InfoStream infoStream(rawPdbFile);
    if (infoStream.UsesDebugFastLink()) {
        MemoryFile::Close(pdbFile);
        return;
    }
    const PDB::DBIStream dbiStream = PDB::CreateDBIStream(rawPdbFile);
    if (!checkValidDBIStreams(rawPdbFile, dbiStream)) {
        MemoryFile::Close(pdbFile);
        return;
    }
    initFastDlsym(rawPdbFile, dbiStream);
    initReverseLookup();
    imageBaseAddr = (uintptr_t)GetModuleHandle(nullptr);
    initialized   = true;
}

void* pl_resolve_symbol(const char* symbolName) {
    static_assert(sizeof(HMODULE) == 8);
    std::lock_guard lock(dlsymLock);
    if (!fastDlsymState) return nullptr;
    auto iter = funcMap->find(string(symbolName));
    if (iter != funcMap->end()) {
        return (void*)(imageBaseAddr + iter->second);
    } else {
        Error("Could not find function in memory: {}", symbolName);
        Error("Plugin: {}", pl::utils::GetCallerModuleFileName());
    }
    return nullptr;
}

const char* const* pl_lookup_symbol(void* func, size_t* resultLength) {
    if (!rvaMap) {
        if (resultLength) *resultLength = 0;
        return nullptr;
    }
    vector<string> symbols;

    auto funcAddr     = reinterpret_cast<uintptr_t>(func);
    auto [begin, end] = rvaMap->equal_range(static_cast<int>(funcAddr - imageBaseAddr));
    for (auto it = begin; it != end; ++it) { symbols.push_back(*it->second); }

    size_t size = symbols.size();
    if (resultLength) *resultLength = size;
    if (!size) return nullptr;

    auto result = new char*[size + 1];
    for (int i = 0; i < symbols.size(); i++) {
        size_t len = symbols[i].length() + 1;
        result[i]  = new char[len];
        memcpy(result[i], symbols[i].c_str(), len);
    }

    result[symbols.size()] = nullptr;
    return result;
}

void pl_free_lookup_result(const char* const* result) {
    for (int i = 0; result[i] != nullptr; i++) { delete[] result[i]; }
    delete[] result;
}

} // namespace pl::symbol_provider
