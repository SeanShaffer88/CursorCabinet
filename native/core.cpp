#include "core.hpp"
#include <algorithm>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <map>
#include <objbase.h>
#include <shlobj.h>
#include <sstream>
#include <system_error>

namespace cc {
const std::array<const wchar_t *, RoleCount + 2> ValueNames = {
    L"Arrow", L"Help",   L"AppStarting", L"Wait",     L"Crosshair",    L"IBeam",   L"NWPen",
    L"No",    L"SizeNS", L"SizeWE",      L"SizeNWSE", L"SizeNESW",     L"SizeAll", L"UpArrow",
    L"Hand",  L"Pin",    L"Person",      L"",         L"Scheme Source"};
const std::array<const wchar_t *, RoleCount> RoleLabels = {L"Normal select",
                                                           L"Help",
                                                           L"Working",
                                                           L"Busy",
                                                           L"Precision",
                                                           L"Text",
                                                           L"Handwriting",
                                                           L"Unavailable",
                                                           L"Vertical resize",
                                                           L"Horizontal resize",
                                                           L"Diagonal resize 1",
                                                           L"Diagonal resize 2",
                                                           L"Move",
                                                           L"Alternate",
                                                           L"Link",
                                                           L"Location",
                                                           L"Person"};
void check(LSTATUS code, const char *action) {
    if (code != ERROR_SUCCESS)
        throw std::system_error(code, std::system_category(), action);
}
std::wstring lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return s;
}
std::string utf8(const std::wstring &s) {
    if (s.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr,
                                0, nullptr, nullptr);
    if (!n)
        check(GetLastError(), "Encode text");
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}
std::wstring wide(const std::string &s) {
    if (s.empty())
        return {};
    int n =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (!n) {
        n = MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring out(n, L'\0');
        MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
        return out;
    }
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}
std::wstring message(const std::exception &e) {
    return wide(e.what());
}
fs::path exeDirectory() {
    std::vector<wchar_t> buf(32768);
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (!n || n >= buf.size())
        throw std::runtime_error("Cannot resolve application folder");
    return fs::path(std::wstring(buf.data(), n)).parent_path();
}
fs::path dataDirectory() {
    PWSTR raw = nullptr;
    HRESULT hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &raw);
    if (FAILED(hr))
        throw std::system_error(hr, std::system_category(), "Locate local application data");
    fs::path path(raw);
    CoTaskMemFree(raw);
    return path / L"CursorCabinet";
}
class Handle {
    HANDLE h = INVALID_HANDLE_VALUE;

  public:
    explicit Handle(HANDLE value) : h(value) {
        if (h == INVALID_HANDLE_VALUE)
            check(GetLastError(), "Open file");
    }
    ~Handle() {
        if (h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
    }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    operator HANDLE() const {
        return h;
    }
};
bool safeTree(const fs::path &path) {
    fs::path current = fs::absolute(path).lexically_normal();
    while (!current.empty()) {
        DWORD attr = GetFileAttributesW(current.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT))
            return false;
        if (current == current.root_path())
            break;
        current = current.parent_path();
    }
    return true;
}
void atomicWrite(const fs::path &destination, const std::vector<BYTE> &bytes) {
    const auto path = fs::absolute(destination).lexically_normal();
    if (!safeTree(path))
        throw std::runtime_error("Refusing a reparse point in application data path");
    fs::create_directories(path.parent_path());
    fs::path tmp = path;
    tmp += L".tmp";
    if (!safeTree(tmp))
        throw std::runtime_error("Refusing redirected staging file");
    try {
        {
            Handle h(CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr));
            DWORD written = 0;
            if (!WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr))
                check(GetLastError(), "Write file (check disk space and permissions)");
            if (written != bytes.size())
                throw std::runtime_error("Incomplete file write; original data retained");
            if (!FlushFileBuffers(h))
                check(GetLastError(), "Flush file");
        }
        if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            check(GetLastError(), "Commit file");
    } catch (...) {
        DeleteFileW(tmp.c_str());
        throw;
    }
}
void writeText(const fs::path &p, const std::string &text) {
    atomicWrite(p, std::vector<BYTE>(text.begin(), text.end()));
}
std::vector<BYTE> readBytes(const fs::path &path, size_t limit) {
    if (!safeTree(path))
        throw std::runtime_error("Refusing to read through a reparse point");
    Handle h(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    LARGE_INTEGER len{};
    if (!GetFileSizeEx(h, &len))
        check(GetLastError(), "Read file size");
    if (len.QuadPart < 0 || static_cast<unsigned long long>(len.QuadPart) > limit)
        throw std::runtime_error("File exceeds supported size");
    std::vector<BYTE> out(static_cast<size_t>(len.QuadPart));
    DWORD got = 0;
    if (!ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr))
        check(GetLastError(), "Read file");
    if (got != out.size())
        throw std::runtime_error("File changed during read");
    return out;
}
void log(const fs::path &data, const std::string &text) noexcept {
    try {
        fs::create_directories(data);
        auto path = data / L"diagnostics.log";
        if (!safeTree(path))
            return;
        if (fs::exists(path) && fs::file_size(path) > 256 * 1024) {
            auto old = data / L"diagnostics.previous.log";
            if (!MoveFileExW(path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING))
                return;
        }
        std::ofstream out(path, std::ios::app | std::ios::binary);
        SYSTEMTIME t{};
        GetSystemTime(&t);
        out << t.wYear << '-' << t.wMonth << '-' << t.wDay << 'T' << t.wHour << ':' << t.wMinute << ':'
            << t.wSecond << "Z " << text.substr(0, 2048) << '\n';
    } catch (const std::exception &) {
        OutputDebugStringW(L"CursorCabinet: diagnostic logging unavailable\n");
    }
}
std::wstring trim(std::wstring s) {
    size_t a = s.find_first_not_of(L" \t\r\n\ufeff");
    if (a == s.npos)
        return {};
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}
std::wstring unquote(std::wstring s) {
    s = trim(s);
    if (s.size() >= 2 && s.front() == L'"' && s.back() == L'"')
        s = s.substr(1, s.size() - 2);
    return s;
}
std::vector<std::wstring> csv(const std::wstring &line) {
    std::vector<std::wstring> out;
    std::wstring token;
    bool quoted = false;
    for (size_t i = 0; i < line.size(); ++i) {
        wchar_t c = line[i];
        if (c == L'"') {
            if (quoted && i + 1 < line.size() && line[i + 1] == L'"') {
                token += c;
                ++i;
            } else
                quoted = !quoted;
        } else if (c == L',' && !quoted) {
            out.push_back(trim(token));
            token.clear();
        } else if (c == L';' && !quoted)
            break;
        else
            token += c;
    }
    if (quoted)
        throw std::runtime_error("Unterminated INF string");
    out.push_back(trim(token));
    return out;
}
std::wstring readText(const fs::path &path) {
    auto bytes = readBytes(path);
    if (bytes.size() >= 2 && bytes[0] == 255 && bytes[1] == 254) {
        if (bytes.size() % 2)
            throw std::runtime_error("Invalid UTF-16 file");
        std::wstring out((bytes.size() - 2) / 2, L'\0');
        memcpy(out.data(), bytes.data() + 2, bytes.size() - 2);
        return out;
    }
    return wide(std::string(bytes.begin(), bytes.end()));
}
std::vector<Theme> scan(const fs::path &root, std::vector<std::wstring> &warnings) {
    std::vector<Theme> result;
    if (!fs::exists(root))
        return result;
    if (!safeTree(root))
        throw std::runtime_error("Cursor library root is a reparse point");
    std::error_code ec;
    fs::recursive_directory_iterator it(root, fs::directory_options::none, ec), end;
    if (ec)
        throw std::system_error(ec, "Open cursor library");
    for (; it != end; it.increment(ec)) {
        if (ec)
            throw std::system_error(ec, "Read cursor library");
        const auto path = it->path();
        DWORD attr = GetFileAttributesW(path.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) {
            warnings.push_back(L"Unavailable: " + path.wstring());
            continue;
        }
        if (attr & FILE_ATTRIBUTE_REPARSE_POINT) {
            it.disable_recursion_pending();
            warnings.push_back(L"Skipped reparse point: " + path.wstring());
            continue;
        }
        if (it->is_directory() || lower(path.extension().wstring()) != L".inf")
            continue;
        try {
            if (result.size() >= 10000)
                throw std::runtime_error("Library limit is 10,000 sets");
            auto content = readText(path);
            std::wistringstream stream(content);
            std::wstring line, section;
            std::map<std::wstring, std::wstring> strings;
            std::vector<std::vector<std::wstring>> rows;
            while (std::getline(stream, line)) {
                line = trim(line);
                if (line.empty() || line[0] == L';')
                    continue;
                if (line.front() == L'[') {
                    section = lower(line);
                    continue;
                }
                if (section == L"[strings]") {
                    auto eq = line.find(L'=');
                    if (eq != line.npos)
                        strings[lower(trim(line.substr(0, eq)))] = unquote(line.substr(eq + 1));
                } else if (lower(line.substr(0, 4)) == L"hkcu")
                    rows.push_back(csv(line));
            }
            Theme theme;
            theme.folder = path.parent_path();
            theme.name =
                strings.count(L"scheme_name") ? strings[L"scheme_name"] : theme.folder.filename().wstring();
            theme.collection = L"Imported";
            theme.id = theme.folder.wstring();
            for (auto parent = theme.folder; !parent.empty(); parent = parent.parent_path()) {
                if (fs::exists(parent / L"SOURCE.txt")) {
                    auto text = readText(parent / L"SOURCE.txt");
                    theme.source = trim(text.substr(0, text.find(L'\n')));
                    theme.collection = parent.filename().wstring();
                    theme.id = theme.source + L"|" + theme.folder.lexically_relative(parent).wstring();
                    break;
                }
                if (parent == root || parent == parent.root_path())
                    break;
            }
            auto mappedFile = [&](std::wstring value) {
                for (size_t pos = 0; (pos = value.find(L'%', pos)) != value.npos;) {
                    auto last = value.find(L'%', pos + 1);
                    if (last == value.npos)
                        break;
                    auto key = lower(value.substr(pos + 1, last - pos - 1));
                    auto found = strings.find(key);
                    if (found != strings.end()) {
                        value.replace(pos, last - pos + 1, found->second);
                        pos += found->second.size();
                    } else
                        pos = last + 1;
                }
                auto file = theme.folder / fs::path(trim(value)).filename();
                auto ext = lower(file.extension().wstring());
                return ((ext == L".cur" || ext == L".ani") && fs::is_regular_file(file) && safeTree(file))
                           ? file
                           : fs::path{};
            };
            for (auto &row : rows) {
                if (row.size() < 5)
                    continue;
                if (lower(row[1]) == L"control panel\\cursors") {
                    for (size_t r = 0; r < RoleCount; ++r)
                        if (lower(row[2]) == lower(ValueNames[r]))
                            theme.files[r] = mappedFile(row[4]);
                } else if (lower(row[1]) == L"control panel\\cursors\\schemes") {
                    auto mappings = csv(row[4]);
                    for (size_t r = 0; r < std::min(RoleCount, mappings.size()); ++r)
                        if (theme.files[r].empty())
                            theme.files[r] = mappedFile(mappings[r]);
                }
            }
            if (theme.files[0].empty()) {
                warnings.push_back(L"No mapped normal pointer: " + path.wstring());
                continue;
            }
            const std::pair<const wchar_t *, const wchar_t *> collections[] = {
                {L"ful1e5_Bibata_Cursor", L"Bibata"},
                {L"ful1e5_fuchsia-cursor", L"Fuchsia"},
                {L"ful1e5_Google_Cursor", L"GoogleDot"},
                {L"ful1e5_apple_cursor", L"Apple Cursor"},
                {L"ful1e5_XCursor-pro", L"XCursor Pro"},
                {L"rose-pine_cursors", L"Rosé Pine"},
                {L"SakibShahariar_material-bibata-cursor", L"Material Bibata"}};
            for (const auto &item : collections)
                if (theme.collection == item.first) {
                    theme.collection = item.second;
                    break;
                }
            if (std::none_of(result.begin(), result.end(),
                             [&](const Theme &t) { return t.folder == theme.folder; }))
                result.push_back(std::move(theme));
        } catch (const std::exception &e) {
            warnings.push_back(path.wstring() + L": " + message(e));
        }
    }
    if (ec)
        throw std::system_error(ec, "Enumerate cursor library");
    std::sort(result.begin(), result.end(),
              [](const Theme &a, const Theme &b) { return lower(a.name) < lower(b.name); });
    return result;
}
Cursor loadCursor(const fs::path &path) {
    if (!safeTree(path))
        throw std::runtime_error("Cursor path crosses a reparse point");
    std::wstring filename = fs::absolute(path).lexically_normal().wstring();
    fs::path staged;
    // The USER32 cursor loader does not consistently accept extended-length paths.
    // Read deep source files through the file API, then load a short-lived, short
    // app-owned staging path. CUR hotspots and ANI frames remain untouched.
    if (filename.size() >= MAX_PATH || filename.rfind(L"\\\\?\\", 0) == 0) {
        GUID id{};
        HRESULT result = CoCreateGuid(&id);
        if (FAILED(result))
            throw std::system_error(result, std::system_category(), "Create preview staging identifier");
        wchar_t name[40]{};
        StringFromGUID2(id, name, 40);
        staged = dataDirectory() / L"Preview" / (std::wstring(name) + path.extension().wstring());
        if (staged.wstring().size() >= MAX_PATH)
            throw std::runtime_error("Windows cursor loading requires a shorter application data path");
        atomicWrite(staged, readBytes(path, 16 * 1024 * 1024));
        filename = staged.wstring();
    }
    SetLastError(ERROR_SUCCESS);
    HCURSOR h = static_cast<HCURSOR>(
        LoadImageW(nullptr, filename.c_str(), IMAGE_CURSOR, 0, 0, LR_DEFAULTSIZE | LR_LOADFROMFILE));
    DWORD error = GetLastError();
    Cursor cursor(h);
    if (!staged.empty() && !DeleteFileW(staged.c_str()))
        check(GetLastError(), "Remove preview staging file");
    if (!h)
        check(error ? error : ERROR_INVALID_DATA, "Load cursor");
    return cursor;
}
struct RegKey {
    HKEY h = nullptr;
    ~RegKey() {
        if (h)
            RegCloseKey(h);
    }
};
Snapshot RegistryStore::read() {
    RegKey key;
    check(RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Cursors", 0, KEY_QUERY_VALUE, &key.h),
          "Read cursor settings");
    Snapshot out;
    for (size_t i = 0; i < out.size(); ++i) {
        DWORD len = 0, type = 0;
        LSTATUS s = RegQueryValueExW(key.h, ValueNames[i], nullptr, &type, nullptr, &len);
        if (s == ERROR_FILE_NOT_FOUND)
            continue;
        check(s, "Read cursor value");
        if (len > 65536)
            throw std::runtime_error("Cursor registry value is too large");
        out[i].bytes.resize(len);
        check(RegQueryValueExW(key.h, ValueNames[i], nullptr, &type, out[i].bytes.data(), &len),
              "Read cursor data");
        out[i].bytes.resize(len);
        out[i].type = type;
        out[i].exists = true;
    }
    return out;
}
void RegistryStore::write(const Snapshot &values) {
    RegKey key;
    check(RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Cursors", 0, KEY_SET_VALUE, &key.h),
          "Open cursor settings for writing");
    for (size_t i = 0; i < values.size(); ++i) {
        auto &v = values[i];
        if (v.exists)
            check(RegSetValueExW(key.h, ValueNames[i], 0, v.type, v.bytes.data(),
                                 static_cast<DWORD>(v.bytes.size())),
                  "Write cursor setting");
        else {
            LSTATUS s = RegDeleteValueW(key.h, ValueNames[i]);
            if (s != ERROR_FILE_NOT_FOUND)
                check(s, "Remove cursor setting");
        }
    }
    check(RegFlushKey(key.h), "Flush cursor settings");
    if (read() != values)
        throw std::runtime_error("Cursor settings verification failed");
}
void RegistryStore::reload() {
    if (!SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, SPIF_SENDCHANGE))
        check(GetLastError() ? GetLastError() : ERROR_GEN_FAILURE, "Reload Windows cursors");
}
void append32(std::vector<BYTE> &bytes, uint32_t n) {
    for (int i = 0; i < 4; ++i)
        bytes.push_back(static_cast<BYTE>(n >> (i * 8)));
}
uint32_t take32(const std::vector<BYTE> &bytes, size_t &pos) {
    if (pos + 4 > bytes.size())
        throw std::runtime_error("Truncated rollback journal");
    uint32_t n = 0;
    for (int i = 0; i < 4; ++i)
        n |= static_cast<uint32_t>(bytes[pos++]) << (i * 8);
    return n;
}
uint32_t checksum(const std::vector<BYTE> &bytes, size_t count) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < count; ++i) {
        h ^= bytes[i];
        h *= 16777619u;
    }
    return h;
}
void writeJournal(const fs::path &path, const Snapshot &snapshot) {
    std::vector<BYTE> out;
    append32(out, 0x314a4343);
    append32(out, static_cast<uint32_t>(snapshot.size()));
    for (const auto &v : snapshot) {
        append32(out, v.exists ? 1 : 0);
        append32(out, v.type);
        append32(out, static_cast<uint32_t>(v.bytes.size()));
        out.insert(out.end(), v.bytes.begin(), v.bytes.end());
    }
    append32(out, checksum(out, out.size()));
    atomicWrite(path, out);
    if (readJournal(path) != snapshot)
        throw std::runtime_error("Rollback journal read-back failed");
}
Snapshot readJournal(const fs::path &path) {
    auto bytes = readBytes(path, 2 * 1024 * 1024);
    size_t pos = 0;
    if (take32(bytes, pos) != 0x314a4343 || take32(bytes, pos) != ValueNames.size())
        throw std::runtime_error("Unsupported rollback journal");
    Snapshot out;
    for (auto &v : out) {
        uint32_t exists = take32(bytes, pos);
        if (exists > 1)
            throw std::runtime_error("Invalid rollback entry");
        v.exists = exists != 0;
        v.type = take32(bytes, pos);
        auto count = take32(bytes, pos);
        if (count > 65536 || count > bytes.size() - pos)
            throw std::runtime_error("Invalid rollback value length");
        v.bytes.assign(bytes.begin() + pos, bytes.begin() + pos + count);
        pos += count;
    }
    size_t end = pos;
    uint32_t expected = take32(bytes, pos);
    if (pos != bytes.size() || checksum(bytes, end) != expected)
        throw std::runtime_error("Rollback journal checksum mismatch");
    return out;
}
void DiskJournal::prepare(const Snapshot &s) {
    writeJournal(directory / L"pending.ccj", s);
}
void DiskJournal::commit(const Snapshot &s, bool remember) {
    if (remember)
        writeJournal(directory / L"previous.ccj", s);
}
void DiskJournal::clear() {
    if (!DeleteFileW((directory / L"pending.ccj").c_str())) {
        auto err = GetLastError();
        if (err != ERROR_FILE_NOT_FOUND)
            check(err, "Clear completed rollback journal");
    }
}
void transact(Store &store, Journal &journal, const Snapshot &target, bool remember) {
    const auto before = store.read();
    journal.prepare(before);
    try {
        store.write(target);
        store.reload();
        journal.commit(before, remember);
        journal.clear();
    } catch (const std::exception &original) {
        std::string reason = original.what();
        try {
            store.write(before);
            store.reload();
            journal.clear();
        } catch (const std::exception &rollback) {
            throw std::runtime_error(reason + "; rollback incomplete: " + rollback.what() +
                                     ". Pending journal retained; use recovery at next launch.");
        }
        throw std::runtime_error(reason + "; previous settings restored.");
    }
}
void recover(Store &store, const fs::path &data) {
    auto p = data / L"pending.ccj";
    if (!fs::exists(p))
        return;
    auto snapshot = readJournal(p);
    store.write(snapshot);
    store.reload();
    DiskJournal(data).clear();
    log(data, "event=recovery status=restored");
}
Value textValue(const std::wstring &text, DWORD type = REG_EXPAND_SZ) {
    Value v;
    v.exists = true;
    v.type = type;
    v.bytes.resize((text.size() + 1) * sizeof(wchar_t));
    memcpy(v.bytes.data(), text.c_str(), v.bytes.size());
    return v;
}
fs::path uniqueFolder(const fs::path &base) {
    GUID id{};
    HRESULT h = CoCreateGuid(&id);
    if (FAILED(h))
        throw std::system_error(h, std::system_category(), "Create staging identifier");
    wchar_t buffer[40]{};
    StringFromGUID2(id, buffer, 40);
    return base / buffer;
}
void apply(Store &store, const fs::path &data, const Theme &theme) {
    recover(store, data);
    auto dest = uniqueFolder(data / L"Installed");
    if (!safeTree(dest))
        throw std::runtime_error("Installed folder is redirected");
    fs::create_directories(dest);
    Snapshot target;
    bool journalStarted = false;
    try {
        for (size_t i = 0; i < RoleCount; ++i) {
            if (theme.files[i].empty()) {
                target[i] = textValue(L"");
                continue;
            }
            auto bytes = readBytes(theme.files[i], 16 * 1024 * 1024);
            auto path = dest / (std::to_wstring(i) + theme.files[i].extension().wstring());
            atomicWrite(path, bytes);
            auto cursor = loadCursor(path);
            target[i] = textValue(path.wstring());
        }
        target[RoleCount] = textValue(theme.name, REG_SZ);
        target[RoleCount + 1].exists = true;
        target[RoleCount + 1].type = REG_DWORD;
        target[RoleCount + 1].bytes.assign(4, 0);
        DiskJournal journal(data);
        journalStarted = true;
        transact(store, journal, target, true);
        log(data, "event=apply status=success");
    } catch (...) {
        if (!journalStarted) {
            std::error_code ec;
            fs::remove_all(dest, ec);
            if (ec)
                log(data, "event=staging_cleanup status=failed code=" + std::to_string(ec.value()));
        }
        throw;
    }
}
void restore(Store &store, const fs::path &data) {
    recover(store, data);
    auto target = readJournal(data / L"previous.ccj");
    DiskJournal journal(data);
    transact(store, journal, target, false);
    log(data, "event=restore status=success");
}
fs::path importPack(const fs::path &source, const fs::path &data) {
    if (!fs::is_directory(source) || !safeTree(source))
        throw std::runtime_error("Select a local folder without reparse points");
    auto sourceRoot = fs::weakly_canonical(source);
    auto dataRoot = fs::weakly_canonical(data);
    auto inside = dataRoot.lexically_relative(sourceRoot);
    if (!inside.empty() && *inside.begin() != L"..")
        throw std::runtime_error("Import source cannot contain the application data folder");
    auto dest = uniqueFolder(data / L"Imported");
    if (!safeTree(dest))
        throw std::runtime_error("Import destination is redirected");
    fs::create_directories(dest);
    uint64_t total = 0;
    size_t files = 0;
    try {
        for (auto it = fs::recursive_directory_iterator(source); it != fs::recursive_directory_iterator();
             ++it) {
            auto p = it->path();
            if (!safeTree(p))
                throw std::runtime_error("Import contains a symlink or junction");
            if (it->is_directory())
                continue;
            auto ext = lower(p.extension().wstring());
            if (ext != L".cur" && ext != L".ani" && ext != L".inf" && ext != L".txt")
                continue;
            auto len = fs::file_size(p);
            total += len;
            if (++files > 20000 || total > 1024ull * 1024 * 1024)
                throw std::runtime_error("Import exceeds the 1 GB / 20,000 file limit");
            auto target = dest / p.lexically_relative(source);
            atomicWrite(target, readBytes(p, 16 * 1024 * 1024));
        }
        std::vector<std::wstring> warnings;
        auto themes = scan(dest, warnings);
        if (themes.empty())
            throw std::runtime_error(
                "No supported cursor sets found. An INF mapping and normal pointer are required.");
        for (auto &theme : themes)
            for (auto &p : theme.files)
                if (!p.empty()) {
                    auto cursor = loadCursor(p);
                }
        return dest;
    } catch (...) {
        std::error_code ec;
        fs::remove_all(dest, ec);
        if (ec)
            log(data, "event=import_cleanup status=failed code=" + std::to_string(ec.value()));
        throw;
    }
}
std::vector<std::wstring> readFavorites(const fs::path &data) {
    std::vector<std::wstring> out;
    auto file = data / L"favorites.txt";
    if (fs::exists(file)) {
        std::wistringstream lines(readText(file));
        std::wstring line;
        while (std::getline(lines, line))
            if (!trim(line).empty())
                out.push_back(trim(line));
    }
    return out;
}
void saveFavorites(const fs::path &data, const std::vector<std::wstring> &values) {
    std::string text;
    for (auto &v : values)
        text += utf8(v) + "\n";
    writeText(data / L"favorites.txt", text);
}
Preferences readPreferences(const fs::path &data) {
    Preferences p;
    auto file = data / L"settings.ini";
    if (!fs::exists(file))
        return p;
    std::wistringstream lines(readText(file));
    std::wstring line;
    while (std::getline(lines, line)) {
        auto eq = line.find(L'=');
        if (eq == line.npos)
            continue;
        auto key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        if (key == L"width" || key == L"height") {
            try {
                int n = std::stoi(value);
                if (key == L"width")
                    p.width = std::clamp(n, 1020, 3840);
                else
                    p.height = std::clamp(n, 800, 2160);
            } catch (const std::exception &) {
                log(data, "event=settings invalid=window_size");
            }
        } else if (key == L"maximized")
            p.maximized = value == L"1";
        else if (key == L"favorites")
            p.favoritesOnly = value == L"1";
        else if (key == L"descending")
            p.descending = value == L"1";
        else if (key == L"collection")
            p.collection = value;
    }
    return p;
}
void savePreferences(const fs::path &data, const Preferences &p) {
    std::ostringstream text;
    text << "width=" << p.width << "\nheight=" << p.height << "\nmaximized=" << p.maximized
         << "\nfavorites=" << p.favoritesOnly << "\ndescending=" << p.descending
         << "\ncollection=" << utf8(p.collection) << "\n";
    writeText(data / L"settings.ini", text.str());
}
std::wstring schemeName(const Snapshot &values) {
    const auto &v = values[RoleCount];
    if (!v.exists || (v.type != REG_SZ && v.type != REG_EXPAND_SZ) || v.bytes.size() % 2)
        return L"Custom Windows scheme";
    std::wstring name(v.bytes.size() / 2, L'\0');
    memcpy(name.data(), v.bytes.data(), v.bytes.size());
    auto zero = name.find(L'\0');
    if (zero != name.npos)
        name.resize(zero);
    return name.empty() ? L"Windows default" : name;
}
} // namespace cc
