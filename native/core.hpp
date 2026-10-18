#pragma once
#ifndef UNICODE
#define UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <windows.h>

namespace cc {
namespace fs = std::filesystem;
inline constexpr size_t RoleCount = 17;
extern const std::array<const wchar_t *, RoleCount + 2> ValueNames;
extern const std::array<const wchar_t *, RoleCount> RoleLabels;
struct Theme {
    std::wstring name, collection, source, id;
    fs::path folder;
    std::array<fs::path, RoleCount> files;
};
struct CursorDeleter {
    void operator()(std::remove_pointer_t<HCURSOR> *h) const noexcept {
        if (h)
            DestroyCursor(h);
    }
};
using Cursor = std::unique_ptr<std::remove_pointer_t<HCURSOR>, CursorDeleter>;
struct Value {
    bool exists = false;
    DWORD type = REG_SZ;
    std::vector<BYTE> bytes;
    bool operator==(const Value &other) const {
        return exists == other.exists && (!exists || (type == other.type && bytes == other.bytes));
    }
};
using Snapshot = std::array<Value, RoleCount + 2>;
struct Store {
    virtual ~Store() = default;
    virtual Snapshot read() = 0;
    virtual void write(const Snapshot &) = 0;
    virtual void reload() = 0;
};
class RegistryStore final : public Store {
  public:
    Snapshot read() override;
    void write(const Snapshot &) override;
    void reload() override;
};
struct Journal {
    virtual ~Journal() = default;
    virtual void prepare(const Snapshot &) = 0;
    virtual void commit(const Snapshot &, bool remember) = 0;
    virtual void clear() = 0;
};
class DiskJournal final : public Journal {
    fs::path directory;

  public:
    explicit DiskJournal(fs::path path) : directory(std::move(path)) {}
    void prepare(const Snapshot &) override;
    void commit(const Snapshot &, bool remember) override;
    void clear() override;
};
std::wstring lower(std::wstring);
std::string utf8(const std::wstring &);
std::wstring wide(const std::string &);
std::wstring message(const std::exception &);
void check(LSTATUS, const char *);
fs::path exeDirectory();
fs::path dataDirectory();
void atomicWrite(const fs::path &, const std::vector<BYTE> &);
void writeText(const fs::path &, const std::string &);
std::vector<BYTE> readBytes(const fs::path &, size_t limit = 1024 * 1024);
void log(const fs::path &, const std::string &) noexcept;
bool safeTree(const fs::path &);
std::vector<Theme> scan(const fs::path &, std::vector<std::wstring> &warnings);
Cursor loadCursor(const fs::path &);
Snapshot readJournal(const fs::path &);
void writeJournal(const fs::path &, const Snapshot &);
void transact(Store &, Journal &, const Snapshot &target, bool remember);
void recover(Store &, const fs::path &);
void apply(Store &, const fs::path &, const Theme &);
void restore(Store &, const fs::path &);
fs::path importPack(const fs::path &, const fs::path &data);
std::vector<std::wstring> readFavorites(const fs::path &);
void saveFavorites(const fs::path &, const std::vector<std::wstring> &);
struct Preferences {
    int width = 1220, height = 850;
    bool maximized = false, favoritesOnly = false, descending = false;
    std::wstring collection;
};
Preferences readPreferences(const fs::path &);
void savePreferences(const fs::path &, const Preferences &);
std::wstring schemeName(const Snapshot &);
} // namespace cc
