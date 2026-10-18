#include "core.hpp"
#include <iostream>
using namespace cc;

// Explicit integration executable: briefly applies one real set, then restores every
// registry value it touched. It never runs automatically as part of unit tests.
int wmain(int argc, wchar_t **argv) {
    if (argc != 3) {
        std::cerr << "Usage: IntegrationTests.exe <Packs folder> <report path>\n";
        return 2;
    }
    RegistryStore store;
    Snapshot original;
    fs::path folder;
    bool captured = false, restored = false;
    std::string report;
    int result = 0;
    try {
        original = store.read();
        captured = true;
        folder = dataDirectory() / (L"IntegrationTest-" + std::to_wstring(GetCurrentProcessId()));
        if (fs::exists(folder))
            throw std::runtime_error("Integration test folder already exists");
        fs::create_directories(folder);
        writeJournal(folder / L"initial.ccj", original);
        std::vector<std::wstring> warnings;
        auto themes = scan(fs::path(argv[1]), warnings);
        if (themes.empty())
            throw std::runtime_error("No real cursor set is available");
        apply(store, folder, themes.front());
        auto applied = store.read();
        if (applied == original || schemeName(applied) != themes.front().name)
            throw std::runtime_error("Applied scheme did not match expected values");
        report += "PASS real scheme applied and registry read-back verified\n";
        restore(store, folder);
        if (store.read() != original)
            throw std::runtime_error("Restore did not reproduce all original values");
        restored = true;
        report += "PASS original scheme restored byte-for-byte, including value types and absent values\n";
        writeJournal(folder / L"pending.ccj", original);
        store.write(applied);
        restored = false;
        recover(store, folder);
        if (store.read() != original || fs::exists(folder / L"pending.ccj"))
            throw std::runtime_error("Live crash recovery failed");
        restored = true;
        report += "PASS durable crash-recovery journal restored the live scheme\n";
    } catch (const std::exception &e) {
        result = 1;
        report += "FAIL " + std::string(e.what()) + "\n";
        if (captured) {
            try {
                store.write(original);
                store.reload();
                restored = store.read() == original;
            } catch (const std::exception &recovery) {
                report += "RECOVERY FAILED " + std::string(recovery.what()) + "\n";
            }
        }
    }
    if (restored && !folder.empty() && folder.parent_path() == dataDirectory() &&
        folder.filename().wstring().rfind(L"IntegrationTest-", 0) == 0) {
        std::error_code ec;
        fs::remove_all(folder, ec);
        if (ec) {
            report += "FAIL test folder cleanup: " + ec.message() + "\n";
            result = 1;
        }
    }
    report += "original_scheme_restored=" + std::string(restored ? "true" : "false") + "\n";
    try {
        writeText(fs::absolute(argv[2]), report);
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        result = 1;
    }
    std::cout << report;
    return result;
}
