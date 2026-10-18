#include "core.hpp"
#include <cstring>
#include <fstream>
#include <iostream>
using namespace cc;
namespace {
int passed = 0, failed = 0;
void expect(bool condition, const char *name) {
    if (condition) {
        ++passed;
        std::cout << "PASS " << name << '\n';
    } else {
        ++failed;
        std::cerr << "FAIL " << name << '\n';
    }
}
template <class F> bool throws(F fn) {
    try {
        fn();
        return false;
    } catch (const std::exception &) {
        return true;
    }
}
Snapshot sample(BYTE tag) {
    Snapshot s;
    for (auto &v : s) {
        v.exists = true;
        v.type = REG_BINARY;
        v.bytes = {tag, 0, 7, 255};
    }
    s[2].exists = false;
    s[2].bytes.clear();
    return s;
}
struct FakeStore final : Store {
    Snapshot values = sample(1);
    int writes = 0, reloads = 0, failWrite = 0, failReload = 0;
    bool alwaysFail = false;
    Snapshot read() override {
        return values;
    }
    void write(const Snapshot &target) override {
        ++writes;
        values[0] = target[0];
        if (alwaysFail || writes == failWrite)
            throw std::runtime_error("injected permission denied after partial write");
        values = target;
    }
    void reload() override {
        ++reloads;
        if (reloads == failReload)
            throw std::runtime_error("injected reload failure");
    }
};
struct FakeJournal final : Journal {
    bool pending = false, failPrepare = false, failCommit = false, failClear = false;
    Snapshot saved, previous;
    void prepare(const Snapshot &s) override {
        if (failPrepare)
            throw std::runtime_error("injected disk full");
        saved = s;
        pending = true;
    }
    void commit(const Snapshot &s, bool remember) override {
        if (failCommit)
            throw std::runtime_error("injected commit failure");
        if (remember)
            previous = s;
    }
    void clear() override {
        if (failClear)
            throw std::runtime_error("injected journal deletion failure");
        pending = false;
    }
};
} // namespace
int wmain() {
    fs::path temp;
    try {
        auto base = fs::temp_directory_path();
        temp = base / (L"CursorCabinet-Tests-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                       std::to_wstring(GetTickCount64()));
        fs::create_directory(temp);
        {
            FakeStore s;
            FakeJournal j;
            auto before = s.values;
            transact(s, j, sample(2), true);
            expect(s.values == sample(2) && j.previous == before && !j.pending,
                   "transaction commit and previous snapshot");
        }
        {
            FakeStore s;
            FakeJournal j;
            auto before = s.values;
            j.failPrepare = true;
            expect(throws([&] { transact(s, j, sample(2), true); }) && s.writes == 0 && s.values == before,
                   "disk full before mutation leaves settings untouched");
        }
        {
            FakeStore s;
            FakeJournal j;
            auto before = s.values;
            s.failWrite = 1;
            expect(throws([&] { transact(s, j, sample(2), true); }) && s.values == before && !j.pending,
                   "partial registry write rolls back");
        }
        {
            FakeStore s;
            FakeJournal j;
            auto before = s.values;
            s.failReload = 1;
            expect(throws([&] { transact(s, j, sample(2), true); }) && s.values == before && !j.pending,
                   "reload failure rolls back");
        }
        {
            FakeStore s;
            FakeJournal j;
            auto before = s.values;
            j.failCommit = true;
            expect(throws([&] { transact(s, j, sample(2), true); }) && s.values == before && !j.pending,
                   "commit failure rolls back");
        }
        {
            FakeStore s;
            FakeJournal j;
            s.alwaysFail = true;
            expect(throws([&] { transact(s, j, sample(2), true); }) && j.pending,
                   "failed rollback retains pending journal");
        }
        {
            FakeStore s;
            FakeJournal j;
            j.failClear = true;
            expect(throws([&] { transact(s, j, sample(2), true); }) && j.pending && s.values == sample(1),
                   "journal deletion failure preserves recovery data");
        }
        {
            FakeStore s;
            FakeJournal j;
            j.previous = sample(3);
            transact(s, j, sample(2), false);
            expect(j.previous == sample(3), "restore preserves previous snapshot");
        }
        auto journal = temp / L"roundtrip.ccj";
        auto original = sample(77);
        writeJournal(journal, original);
        expect(readJournal(journal) == original,
               "binary snapshot round trip preserves registry types and absent values");
        auto bytes = readBytes(journal);
        bytes[20] ^= 1;
        atomicWrite(journal, bytes);
        expect(throws([&] { readJournal(journal); }), "journal corruption detected");
        bytes.resize(11);
        atomicWrite(journal, bytes);
        expect(throws([&] { readJournal(journal); }), "truncated journal rejected");
        writeText(temp / L"atomic.txt", "first");
        writeText(temp / L"atomic.txt", "second");
        auto content = readBytes(temp / L"atomic.txt");
        expect(std::string(content.begin(), content.end()) == "second" &&
                   !fs::exists(temp / L"atomic.txt.tmp"),
               "atomic replacement removes scratch file");
        expect(throws([&] { readBytes(temp / L"atomic.txt", 1); }),
               "bounded file reader rejects oversized input");
        {
            FakeStore store;
            writeJournal(temp / L"pending.ccj", sample(5));
            recover(store, temp);
            expect(store.values == sample(5) && !fs::exists(temp / L"pending.ccj"),
                   "startup recovery replays durable snapshot");
            recover(store, temp);
            expect(store.writes == 1, "recovery is idempotent");
        }
        auto pack = temp / L"Fixture";
        fs::create_directory(pack);
        writeText(pack / L"pointer.cur", "fixture");
        writeText(
            pack / L"install.inf",
            "[Version]\nsignature=\"$CHICAGO$\"\n[Wreg]\nHKCU,\"Control "
            "Panel\\Cursors\",Arrow,0x00020000,\"%10%\\%CUR_DIR%\\%pointer%\"\nHKCU,\"Control "
            "Panel\\Cursors\",IBeam,0x00020000,\"%10%\\%CUR_DIR%\\%missing%\"\n[Strings]\nSCHEME_NAME=\"Test,"
            " Cursor\"\nCUR_DIR=\"Cursors\\Test\"\npointer=\"pointer.cur\"\nmissing=\"missing.cur\"\n");
        std::vector<std::wstring> warnings;
        auto themes = scan(pack, warnings);
        expect(themes.size() == 1 && themes[0].name == L"Test, Cursor" &&
                   themes[0].files[0] == pack / L"pointer.cur" && themes[0].files[5].empty(),
               "INF mapping parses quoted names and ignores missing roles");
        writeText(pack / L"install2.inf", "[Strings]\nSCHEME_NAME=\"No pointer\"\n");
        themes = scan(pack, warnings);
        expect(themes.size() == 1 && !warnings.empty(), "malformed or unmapped packs produce notices");
        expect(throws([&] { importPack(pack, temp / L"Data"); }), "invalid cursor import fails validation");
        expect(fs::is_empty(temp / L"Data" / L"Imported"), "failed import removes its staged folder");
        saveFavorites(temp, {L"C:\\Unicode\\雪", L"D:\\Other"});
        expect(readFavorites(temp) == std::vector<std::wstring>{L"C:\\Unicode\\雪", L"D:\\Other"},
               "Unicode favorites round trip");
        expect(utf8(wide("CursorCabinet")) == "CursorCabinet", "text conversion round trip");
        {
            Preferences preferences;
            preferences.width = 1400;
            preferences.height = 900;
            preferences.maximized = true;
            preferences.favoritesOnly = true;
            preferences.descending = true;
            preferences.collection = L"Rosé Pine";
            savePreferences(temp, preferences);
            auto actual = readPreferences(temp);
            expect(actual.width == 1400 && actual.height == 900 && actual.maximized && actual.favoritesOnly &&
                       actual.descending && actual.collection == preferences.collection,
                   "preferences round trip preserves filters and window state");
            writeText(temp / L"settings.ini", "width=99999\nheight=garbage\nmaximized=0\n");
            actual = readPreferences(temp);
            expect(actual.width == 3840 && actual.height == 850 && !actual.maximized,
                   "invalid preferences are bounded and recoverable");
        }
        {
            auto scheme = sample(1);
            std::wstring name = L"Example";
            scheme[RoleCount].type = REG_SZ;
            scheme[RoleCount].bytes.resize((name.size() + 1) * sizeof(wchar_t));
            memcpy(scheme[RoleCount].bytes.data(), name.c_str(), scheme[RoleCount].bytes.size());
            expect(schemeName(scheme) == name, "scheme display reads UTF-16 registry names");
            scheme[RoleCount].bytes = {1};
            expect(schemeName(scheme) == L"Custom Windows scheme", "invalid scheme names degrade gracefully");
        }
        expect(throws([&] { importPack(temp, temp / L"NestedData"); }),
               "import rejects a source containing its destination");
        {
            auto mapped = temp / L"SchemeOnly";
            fs::create_directory(mapped);
            writeText(mapped / L"normal.cur", "fixture");
            writeText(mapped / L"install.inf",
                      "[Scheme.Reg]\nHKCU,\"Control "
                      "Panel\\Cursors\\Schemes\",\"%SCHEME_NAME%\",,\"%10%\\normal.cur,,,,,,,,,,,,,,\"\n["
                      "Strings]\nSCHEME_NAME=\"Scheme-only fixture\"\n");
            std::vector<std::wstring> notes;
            auto found = scan(mapped, notes);
            expect(found.size() == 1 && !found[0].files[0].empty(),
                   "scheme-only INF cursor-order mapping supported");
        }
        // Redirect HKCU only inside this test process. No live cursor settings are changed.
        HKEY sandbox = nullptr;
        auto keyName = L"Software\\CursorCabinet\\Tests\\" + std::to_wstring(GetCurrentProcessId());
        check(RegCreateKeyExW(HKEY_CURRENT_USER, keyName.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr,
                              &sandbox, nullptr),
              "Create registry sandbox");
        check(RegOverridePredefKey(HKEY_CURRENT_USER, sandbox), "Redirect test registry");
        try {
            HKEY cursors = nullptr;
            check(RegCreateKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Cursors", 0, nullptr, 0, KEY_ALL_ACCESS,
                                  nullptr, &cursors, nullptr),
                  "Create sandbox cursors");
            RegCloseKey(cursors);
            RegistryStore store;
            auto target = sample(9);
            store.write(target);
            expect(store.read() == target, "registry backend exact read-back in isolated sandbox");
            Snapshot empty;
            store.write(empty);
            expect(store.read() == empty, "registry backend restores absent values");
        } catch (...) {
            RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
            RegCloseKey(sandbox);
            RegDeleteTreeW(HKEY_CURRENT_USER, keyName.c_str());
            throw;
        }
        check(RegOverridePredefKey(HKEY_CURRENT_USER, nullptr), "Release test registry override");
        RegCloseKey(sandbox);
        check(RegDeleteTreeW(HKEY_CURRENT_USER, keyName.c_str()), "Remove registry sandbox");
    } catch (const std::exception &e) {
        ++failed;
        std::cerr << "ERROR " << e.what() << '\n';
    }
    if (!temp.empty() && temp.parent_path() == fs::temp_directory_path() &&
        temp.filename().wstring().rfind(L"CursorCabinet-Tests-", 0) == 0) {
        std::error_code ec;
        fs::remove_all(temp, ec);
        if (ec) {
            ++failed;
            std::cerr << "ERROR temporary test cleanup\n";
        }
    }
    std::cout << passed << " passed; " << failed << " failed\n";
    return failed ? 1 : 0;
}
