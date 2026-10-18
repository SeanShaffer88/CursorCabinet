#include "core.hpp"
#include <algorithm>
#include <atomic>
#include <commctrl.h>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <psapi.h>
#include <sddl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <sstream>
#include <thread>
#include <uxtheme.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(                                                                                             \
    linker,                                                                                                  \
    "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
namespace {
using namespace cc;
constexpr UINT Completed = WM_APP + 1;
bool smokeMode = false;
fs::path smokeReport;
int smokeSelections = 0;
DWORD smokeGdiStart = 0;
int smokeExitCode = 0;
bool smokeSearchPassed = false;
DWORD smokeUserStart = 0;
bool cliMode = false;
std::atomic<bool> cancelled{false};
FILETIME smokeKernel{}, smokeUser{};
ULONGLONG smokeClock = 0;
enum Id {
    Search = 101,
    Collection,
    Favorites,
    Import,
    Discover,
    Restore,
    Apply,
    Favorite,
    Source,
    List,
    Preview,
    Detail,
    Status,
    ClearSearch,
    Refresh,
    MouseSettings,
    Help,
    OpenData,
    Active
};
struct Result {
    enum Kind { Catalog, Cursors, Operation, SavedFavorites } kind = Operation;
    std::wstring error, notice;
    std::wstring activeName;
    bool restoreAvailable = false, finalClose = false;
    Preferences preferences;
    std::vector<Theme> themes;
    std::vector<std::wstring> favorites;
    std::array<Cursor, RoleCount> cursors;
    HIMAGELIST images = nullptr;
    ~Result() {
        if (images)
            ImageList_Destroy(images);
    }
};
class Worker {
    HWND window = nullptr;
    std::thread thread;
    std::mutex mutex;
    std::condition_variable ready;
    std::function<std::unique_ptr<Result>()> task;
    bool stopping = false;

  public:
    void start(HWND w) {
        window = w;
        thread = std::thread([this] {
            for (;;) {
                std::function<std::unique_ptr<Result>()> fn;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    ready.wait(lock, [&] { return stopping || static_cast<bool>(task); });
                    if (stopping && !task)
                        return;
                    fn = std::move(task);
                    task = {};
                }
                std::unique_ptr<Result> result;
                try {
                    result = fn();
                } catch (const std::exception &e) {
                    result = std::make_unique<Result>();
                    result->error = message(e);
                    try {
                        log(dataDirectory(), std::string("event=operation status=failed detail=") + e.what());
                    } catch (const std::exception &) {
                        OutputDebugStringW(L"CursorCabinet: failed to locate diagnostic folder\n");
                    }
                }
                if (!PostMessageW(window, Completed, 0, reinterpret_cast<LPARAM>(result.get())))
                    continue;
                result.release();
            }
        });
    }
    void submit(std::function<std::unique_ptr<Result>()> fn) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (task)
                throw std::runtime_error("Worker queue is full");
            task = std::move(fn);
        }
        ready.notify_one();
    }
    ~Worker() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
        }
        ready.notify_one();
        if (thread.joinable())
            thread.join();
    }
};
struct State {
    HWND window = nullptr, list = nullptr, search = nullptr, collection = nullptr, preview = nullptr,
         detail = nullptr, status = nullptr;
    HFONT font = nullptr, heading = nullptr;
    HBRUSH background = CreateSolidBrush(RGB(246, 248, 252));
    HIMAGELIST images = nullptr;
    Worker worker;
    fs::path data, base;
    std::vector<Theme> themes;
    std::vector<std::wstring> favorites;
    std::wstring activeName;
    Preferences preferences;
    std::array<Cursor, RoleCount> cursors;
    std::vector<size_t> filtered;
    std::optional<size_t> selected;
    bool busy = false, closing = false, startup = true, recoveryBlocked = false, rebuilding = false,
         restoreAvailable = false;
    int sortColumn = 0;
    int dpi = 96;
    ~State() {
        if (images)
            ImageList_Destroy(images);
        if (font)
            DeleteObject(font);
        if (heading)
            DeleteObject(heading);
        DeleteObject(background);
    }
    int px(int n) const {
        return MulDiv(n, dpi, 96);
    }
} *state = nullptr;
std::wstring text(HWND h) {
    int len = GetWindowTextLengthW(h);
    std::wstring out(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(h, out.data(), len + 1);
    out.resize(len);
    return out;
}
void status(const std::wstring &s) {
    SetWindowTextW(state->status, s.c_str());
}
bool favorite(const Theme &t) {
    return std::find(state->favorites.begin(), state->favorites.end(), t.id) != state->favorites.end();
}
void setBusy(bool busy) {
    state->busy = busy;
    for (int id : {Import, Restore, Apply, Favorite, Source, List, Collection, Search, Favorites, Refresh,
                   ClearSearch, OpenData})
        EnableWindow(GetDlgItem(state->window, id), !busy);
    if (!busy) {
        EnableWindow(GetDlgItem(state->window, Apply),
                     state->selected.has_value() && !state->recoveryBlocked);
        EnableWindow(GetDlgItem(state->window, Favorite), state->selected.has_value());
        EnableWindow(GetDlgItem(state->window, Source), state->selected.has_value());
        EnableWindow(GetDlgItem(state->window, Restore), state->restoreAvailable && !state->recoveryBlocked);
    }
}
void launch(std::function<std::unique_ptr<Result>()> task, const std::wstring &notice) {
    if (state->busy)
        return;
    setBusy(true);
    status(notice);
    state->worker.submit(std::move(task));
}
std::unique_ptr<Result> catalog(const fs::path &base, const fs::path &data, bool recoverAtStart) {
    auto out = std::make_unique<Result>();
    out->kind = Result::Catalog;
    fs::create_directories(data);
    if (!safeTree(data))
        throw std::runtime_error("Application data folder crosses a reparse point");
    if (recoverAtStart) {
        RegistryStore store;
        recover(store, data);
    }
    std::vector<std::wstring> warnings;
    out->themes = scan(base / L"Packs", warnings);
    auto imported = scan(data / L"Imported", warnings);
    out->themes.insert(out->themes.end(), std::make_move_iterator(imported.begin()),
                       std::make_move_iterator(imported.end()));
    out->favorites = readFavorites(data);
    out->preferences = readPreferences(data);
    RegistryStore store;
    out->activeName = schemeName(store.read());
    out->restoreAvailable = fs::exists(data / L"previous.ccj");
    if (out->restoreAvailable) {
        auto snapshot = readJournal(data / L"previous.ccj");
    }
    out->images =
        ImageList_Create(32, 32, ILC_COLOR32 | ILC_MASK, static_cast<int>(out->themes.size() + 1), 16);
    if (!out->images)
        throw std::runtime_error("Cannot allocate thumbnail list");
    for (auto &t : out->themes) {
        try {
            auto h = loadCursor(t.files[0]);
            if (ImageList_AddIcon(out->images, reinterpret_cast<HICON>(h.get())) < 0)
                throw std::runtime_error("Cannot add cursor thumbnail");
        } catch (const std::exception &e) {
            warnings.push_back(t.name + L": " + message(e));
            ImageList_AddIcon(out->images, LoadIconW(nullptr, IDI_APPLICATION));
        }
    }
    for (auto &w : warnings)
        log(data, "event=catalog_warning detail=" + utf8(w));
    out->notice = std::to_wstring(out->themes.size()) + L" cursor sets ready";
    if (!warnings.empty())
        out->notice += L" | " + std::to_wstring(warnings.size()) + L" notices in diagnostics.log";
    if (out->themes.empty())
        out->notice =
            L"No cursor sets found. Keep Packs beside CursorCabinet.exe or import an extracted pack.";
    return out;
}
void clearPreview() {
    state->selected.reset();
    for (auto &c : state->cursors)
        c.reset();
    SetWindowTextW(state->detail, L"Select a cursor set\r\nPreview its pointers, then apply.");
    InvalidateRect(state->preview, nullptr, TRUE);
}
void filter() {
    std::wstring previous = state->selected ? state->themes[*state->selected].id : L"";
    std::wstring query = lower(text(state->search));
    int collection = static_cast<int>(SendMessageW(state->collection, CB_GETCURSEL, 0, 0));
    std::wstring name;
    if (collection > 0) {
        auto len = SendMessageW(state->collection, CB_GETLBTEXTLEN, collection, 0);
        name.resize(static_cast<size_t>(len) + 1);
        SendMessageW(state->collection, CB_GETLBTEXT, collection, reinterpret_cast<LPARAM>(name.data()));
        name.resize(static_cast<size_t>(len));
    }
    bool only = SendMessageW(GetDlgItem(state->window, Favorites), BM_GETCHECK, 0, 0) == BST_CHECKED;
    state->rebuilding = true;
    SendMessageW(state->list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(state->list);
    state->filtered.clear();
    for (size_t i = 0; i < state->themes.size(); ++i) {
        auto &t = state->themes[i];
        if (!query.empty() && lower(t.name + L" " + t.collection).find(query) == std::wstring::npos)
            continue;
        if (collection > 0 && t.collection != name)
            continue;
        if (only && !favorite(t))
            continue;
        state->filtered.push_back(i);
    }
    std::sort(state->filtered.begin(), state->filtered.end(), [](size_t a, size_t b) {
        auto &x = state->themes[a];
        auto &y = state->themes[b];
        auto left = lower((state->sortColumn ? x.collection + L"|" : L"") + x.name),
             right = lower((state->sortColumn ? y.collection + L"|" : L"") + y.name);
        return state->preferences.descending ? left > right : left < right;
    });
    bool kept = false;
    for (size_t row = 0; row < state->filtered.size(); ++row) {
        size_t i = state->filtered[row];
        auto &t = state->themes[i];
        std::wstring label =
            (favorite(t) ? L"★ " : L"") + t.name + (t.name == state->activeName ? L"  • Current" : L"");
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM;
        item.iItem = static_cast<int>(row);
        item.pszText = label.data();
        item.iImage = static_cast<int>(i);
        item.lParam = static_cast<LPARAM>(i);
        int inserted = ListView_InsertItem(state->list, &item);
        ListView_SetItemText(state->list, inserted, 1, t.collection.data());
        if (t.id == previous && !previous.empty()) {
            state->selected = i;
            ListView_SetItemState(state->list, inserted, LVIS_SELECTED | LVIS_FOCUSED,
                                  LVIS_SELECTED | LVIS_FOCUSED);
            kept = true;
        }
    }
    if (!kept)
        clearPreview();
    state->rebuilding = false;
    SendMessageW(state->list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(state->list, nullptr, TRUE);
    setBusy(state->busy);
    if (!state->busy)
        status(std::to_wstring(state->filtered.size()) + L" of " + std::to_wstring(state->themes.size()) +
               (state->filtered.empty()
                    ? L" sets | No matches — try another search or clear the filters."
                    : L" sets | Select a set to preview. Ctrl+F searches; F1 opens help."));
}
void select() {
    if (state->busy)
        return;
    int row = ListView_GetNextItem(state->list, -1, LVNI_SELECTED);
    if (row < 0 || static_cast<size_t>(row) >= state->filtered.size())
        return;
    size_t index = state->filtered[row];
    state->selected = index;
    Theme theme = state->themes[index];
    SetWindowTextW(GetDlgItem(state->window, Favorite),
                   favorite(theme) ? L"Remove favorite" : L"Add favorite");
    auto count =
        std::count_if(theme.files.begin(), theme.files.end(), [](const auto &p) { return !p.empty(); });
    SetWindowTextW(
        state->detail,
        (theme.name + L"\r\n" + std::to_wstring(count) + L" / 17 roles | Hover below to try").c_str());
    launch(
        [theme] {
            auto r = std::make_unique<Result>();
            r->kind = Result::Cursors;
            for (size_t i = 0; i < RoleCount; ++i)
                if (!theme.files[i].empty())
                    r->cursors[i] = loadCursor(theme.files[i]);
            return r;
        },
        L"Loading cursor previews...");
}
void smokeNext() {
    if (smokeSelections == 0) {
        SendMessageW(GetDlgItem(state->window, Favorites), BM_SETCHECK, BST_UNCHECKED, 0);
        SendMessageW(state->collection, CB_SETCURSEL, 0, 0);
        SetWindowTextW(state->search, L"Fuchsia");
        bool matches = !state->filtered.empty() && state->filtered.size() < state->themes.size();
        SetWindowTextW(state->search, L"no-matching-cursor-1234567");
        bool empty = state->filtered.empty() && !IsWindowEnabled(GetDlgItem(state->window, Apply));
        SetWindowTextW(state->search, L"");
        smokeSearchPassed = matches && empty && state->filtered.size() == state->themes.size();
    }
    if (state->filtered.empty()) {
        smokeExitCode = 2;
        writeText(smokeReport, "FAIL no cursor sets for UI smoke test\n");
        DestroyWindow(state->window);
        return;
    }
    if (smokeSelections == 1) {
        smokeGdiStart = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        smokeUserStart = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    }
    if (smokeSelections < 20) {
        int row = smokeSelections % static_cast<int>(state->filtered.size());
        ++smokeSelections;
        ListView_SetItemState(state->list, -1, 0, LVIS_SELECTED);
        ListView_SetItemState(state->list, row, LVIS_SELECTED, LVIS_SELECTED);
        return;
    }
    FILETIME created{}, exited{};
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &smokeKernel, &smokeUser);
    smokeClock = GetTickCount64();
    SetTimer(state->window, 99, 2000, nullptr);
}
void smokeFinish() {
    KillTimer(state->window, 99);
    PROCESS_MEMORY_COUNTERS memory{};
    memory.cb = sizeof(memory);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory)))
        check(GetLastError(), "Read memory metrics");
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
        check(GetLastError(), "Read idle CPU metrics");
    auto ticks = [](FILETIME t) {
        return (static_cast<ULONGLONG>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
    };
    double cpuMs =
        static_cast<double>(ticks(kernel) + ticks(user) - ticks(smokeKernel) - ticks(smokeUser)) / 10000.0;
    DWORD gdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    DWORD handles = 0;
    GetProcessHandleCount(GetCurrentProcess(), &handles);
    std::ostringstream report;
    report << "sets=" << state->themes.size() << "\npreview_cycles=" << smokeSelections
           << "\nworking_set_bytes=" << memory.WorkingSetSize << "\nprivate_bytes=" << memory.PagefileUsage
           << "\nidle_interval_ms=" << (GetTickCount64() - smokeClock) << "\nidle_cpu_ms=" << cpuMs
           << "\ngdi_start=" << smokeGdiStart << "\ngdi_end=" << gdi << "\nprocess_handles=" << handles
           << "\nregistry_apply_performed=false\n";
    // Capture this test window, without reading any other application or desktop pixels.
    RECT rect{};
    GetWindowRect(state->window, &rect);
    int width = rect.right - rect.left, height = rect.bottom - rect.top;
    HDC dc = GetDC(state->window), memoryDc = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, width, height);
    HGDIOBJ old = SelectObject(memoryDc, bitmap);
    BOOL painted = PrintWindow(state->window, memoryDc, PW_RENDERFULLCONTENT);
    SelectObject(memoryDc, old);
    if (painted) {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        std::vector<BYTE> pixels(static_cast<size_t>(width) * height * 4);
        if (GetDIBits(memoryDc, bitmap, 0, height, pixels.data(), &info, DIB_RGB_COLORS)) {
            BITMAPFILEHEADER header{};
            header.bfType = 0x4D42;
            header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER);
            header.bfSize = header.bfOffBits + static_cast<DWORD>(pixels.size());
            std::vector<BYTE> file(header.bfSize);
            memcpy(file.data(), &header, sizeof(header));
            memcpy(file.data() + sizeof(header), &info.bmiHeader, sizeof(BITMAPINFOHEADER));
            memcpy(file.data() + header.bfOffBits, pixels.data(), pixels.size());
            auto screenshot = smokeReport;
            screenshot.replace_extension(L"bmp");
            atomicWrite(screenshot, file);
        }
    }
    DeleteObject(bitmap);
    DeleteDC(memoryDc);
    ReleaseDC(state->window, dc);
    report << "gdi_stable=" << (gdi <= smokeGdiStart + 2 ? "true" : "false") << "\n";
    DWORD users = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    bool stable = gdi <= smokeGdiStart + 2 && users <= smokeUserStart + 2;
    report << "user_start=" << smokeUserStart << "\nuser_end=" << users
           << "\nsearch_and_empty_state_passed=" << (smokeSearchPassed ? "true" : "false") << "\n";
    smokeExitCode = stable && smokeSearchPassed ? 0 : 1;
    writeText(smokeReport, report.str());
    DestroyWindow(state->window);
}
void openUrl(const std::wstring &url) {
    if (url.rfind(L"https://", 0) != 0)
        throw std::runtime_error("Only HTTPS artist links are supported");
    auto code = reinterpret_cast<INT_PTR>(
        ShellExecuteW(state->window, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (code <= 32)
        throw std::runtime_error("Cannot open the artist page in your browser");
}
void discover() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"Azure — MultipleWaters (free)");
    AppendMenuW(menu, MF_STRING, 2, L"Pointa — MultipleWaters (free edition)");
    AppendMenuW(menu, MF_STRING, 3, L"Kami Paper White — BIueGuy (free edition)");
    AppendMenuW(menu, MF_STRING, 4, L"Cursor-Zanate F — marnadnay");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 5, L"Search DeviantArt");
    POINT p{};
    GetCursorPos(&p);
    int id = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, p.x, p.y, 0, state->window, nullptr);
    DestroyMenu(menu);
    const wchar_t *urls[] = {
        L"https://www.deviantart.com/multiplewaters/art/Azure-Cursors-v1-0-1074059061",
        L"https://www.deviantart.com/multiplewaters/art/Pointa-Animated-Cursors-v1-0-1024319381",
        L"https://www.deviantart.com/biueguy/art/Kami-v2-Paper-White-709677029",
        L"https://www.deviantart.com/marnadnay/art/Cursor-Zanate-F-954265828",
        L"https://www.deviantart.com/search?q=free%20windows%20cursors"};
    if (id >= 1 && id <= 5)
        openUrl(urls[id - 1]);
}
fs::path chooseFolder() {
    IFileOpenDialog *dialog = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(hr))
        throw std::system_error(hr, std::system_category(), "Open folder picker");
    fs::path result;
    DWORD options = 0;
    hr = dialog->GetOptions(&options);
    if (SUCCEEDED(hr))
        hr = dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    if (SUCCEEDED(hr))
        hr = dialog->SetTitle(L"Select an extracted cursor pack with an INF mapping");
    if (SUCCEEDED(hr))
        hr = dialog->Show(state->window);
    if (SUCCEEDED(hr)) {
        IShellItem *item = nullptr;
        hr = dialog->GetResult(&item);
        if (SUCCEEDED(hr)) {
            PWSTR path = nullptr;
            hr = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
            if (SUCCEEDED(hr)) {
                result = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
    if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_CANCELLED))
        throw std::system_error(hr, std::system_category(), "Select cursor folder");
    return result;
}
void command(int id) {
    auto data = state->data;
    if (id == Discover) {
        discover();
        return;
    }
    if (state->busy)
        return;
    if (id == ClearSearch) {
        SetWindowTextW(state->search, L"");
        SetFocus(state->search);
        return;
    }
    if (id == Refresh) {
        auto base = state->base;
        launch([base, data] { return catalog(base, data, false); }, L"Refreshing cursor library...");
        return;
    }
    if (id == MouseSettings) {
        auto result = reinterpret_cast<INT_PTR>(
            ShellExecuteW(state->window, L"open", L"control.exe", L"main.cpl,,1", nullptr, SW_SHOWNORMAL));
        if (result <= 32)
            throw std::runtime_error("Cannot open Windows mouse settings");
        return;
    }
    if (id == OpenData) {
        auto result = reinterpret_cast<INT_PTR>(
            ShellExecuteW(state->window, L"open", data.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        if (result <= 32)
            throw std::runtime_error("Cannot open the application data folder");
        return;
    }
    if (id == Help) {
        MessageBoxW(
            state->window,
            L"CursorCabinet 0.2.0\nFree, open-source cursor browsing for Windows.\n\nSelect a set and hover "
            L"over its pointers to try it.\nApply saves the current scheme first. Restore previous undoes "
            L"the latest application.\n\nCtrl+F: Search     F5: Refresh library\nCtrl+D: Favorite     "
            L"Ctrl+O: Import folder\nEnter (in the list): Apply     F1: Help\nEscape (in search): Clear "
            L"search\nClick a column heading to sort; click again to reverse.\n\nImports need extracted "
            L"CUR/ANI files and a supported INF mapping.\nDeviantArt links open the artist's page; artist "
            L"terms apply.\n\nNo accounts, ads, analytics, or app download service.\nApplication: MIT "
            L"license. Artwork: see included notices.\nCopyright © 2026 Sean Shaffer.",
            L"About CursorCabinet", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (id == Search || id == Collection || id == Favorites) {
        filter();
        return;
    }
    if (id == Import) {
        auto folder = chooseFolder();
        if (folder.empty())
            return;
        auto base = state->base;
        launch(
            [folder, data, base] {
                auto imported = importPack(folder, data);
                auto r = catalog(base, data, false);
                r->notice = L"Imported cursor pack | " + r->notice;
                return r;
            },
            L"Validating and importing cursor files...");
        return;
    }
    if (id == Restore) {
        launch(
            [data] {
                RegistryStore store;
                restore(store, data);
                auto r = std::make_unique<Result>();
                r->activeName = schemeName(store.read());
                r->restoreAvailable = true;
                r->notice = L"Previous cursor scheme restored.";
                return r;
            },
            L"Restoring previous scheme...");
        return;
    }
    if (!state->selected)
        return;
    auto theme = state->themes[*state->selected];
    if (id == Source) {
        if (theme.source.empty())
            throw std::runtime_error(
                "This imported set has no artist URL. Check the pack's original license and source.");
        openUrl(theme.source);
    }
    if (id == Apply) {
        launch(
            [data, theme] {
                RegistryStore store;
                apply(store, data, theme);
                auto r = std::make_unique<Result>();
                r->activeName = schemeName(store.read());
                r->restoreAvailable = true;
                r->notice = L"Applied " + theme.name + L" | Restore previous is available";
                return r;
            },
            L"Saving previous scheme and applying cursors...");
    }
    if (id == Favorite) {
        auto favorites = state->favorites;
        auto key = theme.id;
        auto it = std::find(favorites.begin(), favorites.end(), key);
        if (it == favorites.end())
            favorites.push_back(key);
        else
            favorites.erase(it);
        launch(
            [data, favorites] {
                saveFavorites(data, favorites);
                auto r = std::make_unique<Result>();
                r->kind = Result::SavedFavorites;
                r->favorites = favorites;
                return r;
            },
            L"Saving favorite...");
    }
}
void layout() {
    RECT r{};
    GetClientRect(state->window, &r);
    auto p = [](int n) { return state->px(n); };
    int w = r.right, h = r.bottom, side = p(350), gap = p(18);
    auto place = [&](int id, int x, int y, int width, int height) {
        MoveWindow(GetDlgItem(state->window, id), x, y, width, height, TRUE);
    };
    place(Active, w - p(510), p(24), p(490), p(26));
    place(Search, gap, p(65), p(255), p(29));
    place(ClearSearch, p(279), p(65), p(30), p(29));
    place(Collection, p(325), p(65), p(225), p(300));
    place(Favorites, p(566), p(65), p(110), p(29));
    place(Import, p(690), p(64), p(105), p(31));
    place(Discover, p(810), p(64), p(130), p(31));
    place(Help, p(954), p(64), p(40), p(31));
    place(Detail, w - side, p(124), side - gap, p(65));
    place(List, gap, p(124), w - side - gap * 2, h - p(216));
    place(Preview, w - side, p(194), side - gap, std::max(p(330), h - p(350)));
    place(Apply, w - side, h - p(143), side - gap, p(36));
    place(Favorite, w - side, h - p(99), p(143), p(30));
    place(Source, w - side + p(154), h - p(99), p(177), p(30));
    place(Restore, w - side, h - p(54), side - gap, p(32));
    place(Refresh, gap, h - p(78), p(100), p(30));
    place(MouseSettings, p(130), h - p(78), p(153), p(30));
    place(OpenData, p(295), h - p(78), p(130), p(30));
    place(Status, gap, h - p(39), w - side - gap * 2, p(29));
    ListView_SetColumnWidth(state->list, 0, std::max(p(240), w - side - p(232)));
    ListView_SetColumnWidth(state->list, 1, p(175));
}
void saveAndClose() {
    state->closing = true;
    EnableWindow(state->window, FALSE);
    Preferences preferences = state->preferences;
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    if (GetWindowPlacement(state->window, &placement)) {
        preferences.maximized = placement.showCmd == SW_SHOWMAXIMIZED;
        preferences.width =
            MulDiv(placement.rcNormalPosition.right - placement.rcNormalPosition.left, 96, state->dpi);
        preferences.height =
            MulDiv(placement.rcNormalPosition.bottom - placement.rcNormalPosition.top, 96, state->dpi);
    }
    preferences.favoritesOnly =
        SendMessageW(GetDlgItem(state->window, Favorites), BM_GETCHECK, 0, 0) == BST_CHECKED;
    int index = static_cast<int>(SendMessageW(state->collection, CB_GETCURSEL, 0, 0));
    preferences.collection.clear();
    if (index > 0) {
        auto len = SendMessageW(state->collection, CB_GETLBTEXTLEN, index, 0);
        preferences.collection.resize(static_cast<size_t>(len) + 1);
        SendMessageW(state->collection, CB_GETLBTEXT, index,
                     reinterpret_cast<LPARAM>(preferences.collection.data()));
        preferences.collection.resize(static_cast<size_t>(len));
    }
    auto data = state->data;
    launch(
        [preferences, data] {
            auto r = std::make_unique<Result>();
            r->finalClose = true;
            try {
                savePreferences(data, preferences);
            } catch (const std::exception &e) {
                log(data, std::string("event=settings_save status=failed detail=") + e.what());
                r->error = message(e);
            }
            return r;
        },
        L"Saving preferences...");
}
void tips() {
    HWND tooltip =
        CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, state->window, nullptr,
                        GetModuleHandleW(nullptr), nullptr);
    if (!tooltip)
        return;
    SendMessageW(tooltip, TTM_SETMAXTIPWIDTH, 0, state->px(300));
    const std::pair<int, const wchar_t *> entries[] = {
        {Search, L"Search names and collections (Ctrl+F)"},
        {ClearSearch, L"Clear the search"},
        {Collection, L"Show one collection"},
        {Favorites, L"Show only your favorites"},
        {Import, L"Import an extracted cursor folder (Ctrl+O)"},
        {Discover, L"Open original artist pages and free-edition links"},
        {Help, L"Help, keyboard shortcuts, and license information (F1)"},
        {Apply, L"Save your current scheme and apply this set (Enter in the list)"},
        {Favorite, L"Add or remove a favorite (Ctrl+D)"},
        {Restore, L"Restore the cursor scheme saved before your most recent application"},
        {Refresh, L"Rescan downloaded and imported cursor packs (F5)"},
        {OpenData, L"Open preferences, imported sets, and bounded diagnostic logs"},
        {MouseSettings, L"Open Windows mouse pointer settings"}};
    for (const auto &entry : entries) {
        TOOLINFOW info{};
        info.cbSize = sizeof(info);
        info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        info.hwnd = state->window;
        info.uId = reinterpret_cast<UINT_PTR>(GetDlgItem(state->window, entry.first));
        info.lpszText = const_cast<LPWSTR>(entry.second);
        SendMessageW(tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
    }
}
LRESULT CALLBACK previewProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client{};
        GetClientRect(hwnd, &client);
        FillRect(dc, &client, state->background);
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, state->font);
        int cellW = client.right / 3, cellH = std::max(state->px(55), static_cast<int>(client.bottom) / 6);
        for (size_t i = 0; i < RoleCount; ++i) {
            int x = static_cast<int>(i % 3) * cellW, y = static_cast<int>(i / 3) * cellH;
            RECT cell{x + 3, y + 3, x + cellW - 3, y + cellH - 3};
            FillRect(dc, &cell, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
            if (state->cursors[i])
                DrawIconEx(dc, x + (cellW - state->px(30)) / 2, y + state->px(5), state->cursors[i].get(),
                           state->px(30), state->px(30), 0, nullptr, DI_NORMAL);
            RECT label{x + 3, y + state->px(39), x + cellW - 3, y + cellH - 3};
            SetTextColor(dc, RGB(62, 74, 94));
            DrawTextW(dc, state->cursors[i] ? RoleLabels[i] : L"Default", -1, &label,
                      DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_SETCURSOR && LOWORD(l) == HTCLIENT) {
        POINT pos{};
        GetCursorPos(&pos);
        ScreenToClient(hwnd, &pos);
        RECT rect{};
        GetClientRect(hwnd, &rect);
        int width = std::max(1, static_cast<int>(rect.right) / 3),
            row = pos.y / std::max(state->px(55), static_cast<int>(rect.bottom) / 6), col = pos.x / width;
        int index = row * 3 + col;
        if (col < 3 && index >= 0 && index < static_cast<int>(RoleCount) && state->cursors[index]) {
            SetCursor(state->cursors[index].get());
            return TRUE;
        }
    }
    return DefWindowProcW(hwnd, msg, w, l);
}
void fonts() {
    if (state->font)
        DeleteObject(state->font);
    if (state->heading)
        DeleteObject(state->heading);
    state->font =
        CreateFontW(-state->px(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    state->heading =
        CreateFontW(-state->px(25), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    for (HWND child = GetWindow(state->window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(state->font), TRUE);
}
HWND control(const wchar_t *klass, const wchar_t *label, DWORD style, int id) {
    HWND h = CreateWindowExW(0, klass, label, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, state->window,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr),
                             nullptr);
    if (!h)
        check(GetLastError(), "Create window control");
    SetWindowTheme(h, L"Explorer", nullptr);
    return h;
}
LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    try {
        switch (msg) {
        case WM_CREATE: {
            state->window = hwnd;
            state->dpi = static_cast<int>(GetDpiForWindow(hwnd));
            state->search = control(WC_EDITW, L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, Search);
            SendMessageW(state->search, EM_SETCUEBANNER, TRUE,
                         reinterpret_cast<LPARAM>(L"Search cursor sets..."));
            state->collection =
                control(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, Collection);
            control(WC_BUTTONW, L"Favorites", BS_AUTOCHECKBOX | WS_TABSTOP, Favorites);
            control(WC_BUTTONW, L"Import folder", WS_TABSTOP, Import);
            control(WC_BUTTONW, L"DeviantArt", WS_TABSTOP, Discover);
            control(WC_BUTTONW, L"Apply selected set", BS_DEFPUSHBUTTON | WS_TABSTOP, Apply);
            control(WC_BUTTONW, L"Toggle favorite", WS_TABSTOP, Favorite);
            control(WC_BUTTONW, L"Artist / source", WS_TABSTOP, Source);
            control(WC_BUTTONW, L"Restore previous", WS_TABSTOP, Restore);
            state->detail = control(WC_STATICW, L"Select a cursor set\r\nPreview its pointers, then apply.",
                                    SS_LEFT, Detail);
            state->status = control(WC_STATICW, L"Loading cursor library...", SS_LEFT, Status);
            control(WC_STATICW, L"Current scheme: checking...", SS_RIGHT | SS_PATHELLIPSIS, Active);
            control(WC_BUTTONW, L"×", WS_TABSTOP, ClearSearch);
            control(WC_BUTTONW, L"?", WS_TABSTOP, Help);
            control(WC_BUTTONW, L"Refresh", WS_TABSTOP, Refresh);
            control(WC_BUTTONW, L"Mouse settings", WS_TABSTOP, MouseSettings);
            control(WC_BUTTONW, L"App data", WS_TABSTOP, OpenData);
            state->preview = control(L"CursorCabinetPreview", L"", 0, Preview);
            state->list =
                control(WC_LISTVIEWW, L"",
                        LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP | WS_BORDER, List);
            ListView_SetExtendedListViewStyle(state->list,
                                              LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
            LVCOLUMNW col{};
            col.mask = LVCF_TEXT | LVCF_WIDTH;
            col.cx = 350;
            wchar_t first[] = L"Cursor set", second[] = L"Collection";
            col.pszText = first;
            ListView_InsertColumn(state->list, 0, &col);
            col.pszText = second;
            col.cx = 210;
            ListView_InsertColumn(state->list, 1, &col);
            fonts();
            tips();
            state->worker.start(hwnd);
            auto base = state->base, data = state->data;
            launch([base, data] { return catalog(base, data, true); },
                   L"Checking recovery journal and loading library...");
            return 0;
        }
        case WM_SIZE:
            layout();
            return 0;
        case WM_DPICHANGED: {
            state->dpi = HIWORD(w);
            auto rect = reinterpret_cast<RECT *>(l);
            SetWindowPos(hwnd, nullptr, rect->left, rect->top, rect->right - rect->left,
                         rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
            fonts();
            layout();
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto info = reinterpret_cast<MINMAXINFO *>(l);
            info->ptMinTrackSize = {state->px(1020), state->px(720)};
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(w), event = HIWORD(w);
            if (id == Search && event != EN_CHANGE)
                return 0;
            if (id == Collection && event != CBN_SELCHANGE)
                return 0;
            command(id);
            return 0;
        }
        case WM_NOTIFY: {
            auto header = reinterpret_cast<NMHDR *>(l);
            if (header->idFrom == List && header->code == LVN_COLUMNCLICK && !state->busy) {
                auto info = reinterpret_cast<NMLISTVIEW *>(l);
                if (state->sortColumn == info->iSubItem)
                    state->preferences.descending = !state->preferences.descending;
                else {
                    state->sortColumn = info->iSubItem;
                    state->preferences.descending = false;
                }
                filter();
            }
            if (header->idFrom == List && header->code == LVN_ITEMCHANGED && !state->rebuilding) {
                auto info = reinterpret_cast<NMLISTVIEW *>(l);
                if ((info->uNewState & LVIS_SELECTED) && !(info->uOldState & LVIS_SELECTED))
                    select();
            }
            return 0;
        }
        case Completed: {
            std::unique_ptr<Result> r(reinterpret_cast<Result *>(l));
            setBusy(false);
            if (r->finalClose) {
                if (!r->error.empty())
                    MessageBoxW(hwnd, r->error.c_str(), L"Could not save preferences",
                                MB_OK | MB_ICONWARNING);
                DestroyWindow(hwnd);
                return 0;
            }
            if (state->closing) {
                saveAndClose();
                return 0;
            }
            if (!r->error.empty()) {
                if (smokeMode) {
                    writeText(smokeReport, "FAIL " + utf8(r->error) + "\n");
                    smokeExitCode = 1;
                    DestroyWindow(hwnd);
                    return 0;
                }
                if (state->startup) {
                    state->recoveryBlocked = true;
                    state->startup = false;
                    setBusy(false);
                }
                status(L"Operation failed; see diagnostics.log");
                MessageBoxW(hwnd, r->error.c_str(), L"CursorCabinet", MB_OK | MB_ICONERROR);
                return 0;
            }
            bool initial = state->startup;
            state->startup = false;
            if (!r->activeName.empty()) {
                state->activeName = r->activeName;
                state->restoreAvailable = r->restoreAvailable;
            }
            if (r->kind == Result::Catalog) {
                clearPreview();
                ListView_DeleteAllItems(state->list);
                auto old = state->images;
                state->images = r->images;
                r->images = nullptr;
                ListView_SetImageList(state->list, state->images, LVSIL_SMALL);
                if (old)
                    ImageList_Destroy(old);
                state->themes = std::move(r->themes);
                state->favorites = std::move(r->favorites);
                if (initial)
                    state->preferences = r->preferences;
                SendMessageW(state->collection, CB_RESETCONTENT, 0, 0);
                SendMessageW(state->collection, CB_ADDSTRING, 0,
                             reinterpret_cast<LPARAM>(L"All collections"));
                std::vector<std::wstring> collections;
                for (auto &t : state->themes)
                    if (std::find(collections.begin(), collections.end(), t.collection) ==
                        collections.end()) {
                        collections.push_back(t.collection);
                        SendMessageW(state->collection, CB_ADDSTRING, 0,
                                     reinterpret_cast<LPARAM>(t.collection.c_str()));
                    }
                int selectedCollection = 0;
                for (size_t i = 0; i < collections.size(); ++i)
                    if (collections[i] == state->preferences.collection)
                        selectedCollection = static_cast<int>(i) + 1;
                SendMessageW(state->collection, CB_SETCURSEL, selectedCollection, 0);
                if (initial) {
                    SendMessageW(GetDlgItem(hwnd, Favorites), BM_SETCHECK,
                                 state->preferences.favoritesOnly ? BST_CHECKED : BST_UNCHECKED, 0);
                    if (!smokeMode) {
                        MONITORINFO monitor{};
                        monitor.cbSize = sizeof(monitor);
                        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor);
                        int width = std::min(state->px(state->preferences.width),
                                             static_cast<int>(monitor.rcWork.right - monitor.rcWork.left)),
                            height = std::min(state->px(state->preferences.height),
                                              static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top));
                        SetWindowPos(
                            hwnd, nullptr,
                            monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - width) / 2,
                            monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - height) / 2,
                            width, height, SWP_NOZORDER | SWP_NOACTIVATE);
                        if (state->preferences.maximized)
                            ShowWindow(hwnd, SW_SHOWMAXIMIZED);
                    }
                }
                filter();
            }
            if (!r->activeName.empty()) {
                state->activeName = r->activeName;
                state->restoreAvailable = r->restoreAvailable;
                SetWindowTextW(GetDlgItem(hwnd, Active), (L"Current scheme: " + state->activeName).c_str());
                if (r->kind == Result::Operation)
                    filter();
                setBusy(false);
            }
            if (r->kind == Result::Cursors) {
                state->cursors = std::move(r->cursors);
                InvalidateRect(state->preview, nullptr, TRUE);
            }
            if (r->kind == Result::SavedFavorites) {
                state->favorites = std::move(r->favorites);
                filter();
            }
            if (!r->notice.empty())
                status(r->notice);
            else
                status(std::to_wstring(state->filtered.size()) +
                       L" cursor sets | Hover over a pointer to try it");
            if (smokeMode && (r->kind == Result::Catalog || r->kind == Result::Cursors))
                smokeNext();
            return 0;
        }
        case WM_TIMER:
            if (smokeMode && w == 99) {
                smokeFinish();
                return 0;
            }
            break;
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(w);
            SetBkColor(dc, RGB(246, 248, 252));
            SetTextColor(dc, RGB(43, 56, 77));
            return reinterpret_cast<LRESULT>(state->background);
        }
        case WM_ERASEBKGND: {
            RECT r{};
            GetClientRect(hwnd, &r);
            FillRect(reinterpret_cast<HDC>(w), &r, state->background);
            return 1;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            SelectObject(dc, state->heading);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(28, 46, 71));
            TextOutW(dc, state->px(18), state->px(14), L"CursorCabinet", 13);
            SelectObject(dc, state->font);
            TextOutW(dc, state->px(232), state->px(26), L"A new point of view.", 20);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_CLOSE:
            if (state->busy) {
                state->closing = true;
                EnableWindow(hwnd, FALSE);
                status(L"Finishing the current operation safely...");
                return 0;
            }
            saveAndClose();
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
    } catch (const std::exception &e) {
        MessageBoxW(hwnd, message(e).c_str(), L"CursorCabinet", MB_OK | MB_ICONERROR);
    }
    return DefWindowProcW(hwnd, msg, w, l);
}
int verify(const fs::path &base, const fs::path &report) {
    std::vector<std::wstring> warnings;
    auto themes = scan(base / L"Packs", warnings);
    size_t mapped = 0, failures = 0;
    std::ostringstream text;
    for (auto &theme : themes)
        for (auto &file : theme.files)
            if (!file.empty()) {
                if (cancelled.load())
                    return 130;
                ++mapped;
                try {
                    auto cursor = loadCursor(file);
                } catch (const std::exception &e) {
                    ++failures;
                    text << "FAIL " << utf8(file.wstring()) << ": " << e.what() << '\n';
                }
            }
    text << themes.size() << " sets; " << mapped << " mapped cursors; " << failures << " load failures; "
         << warnings.size() << " catalog notices\n";
    for (auto &note : warnings)
        text << "NOTICE " << utf8(note) << '\n';
    writeText(report, text.str());
    DWORD written = 0;
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (output && output != INVALID_HANDLE_VALUE) {
        auto content = text.str();
        WriteFile(output, content.data(), static_cast<DWORD>(content.size()), &written, nullptr);
    }
    return themes.empty() ? 2 : (failures ? 1 : 0);
}
BOOL WINAPI cancelHandler(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT) {
        cancelled.store(true);
        return TRUE;
    }
    return FALSE;
}
} // namespace
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr))
        return 3;
    int code = 0;
    HANDLE mutex = nullptr;
    try {
        int argc = 0;
        LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv)
            check(GetLastError(), "Read command line");
        std::vector<std::wstring> args;
        for (int i = 1; i < argc; ++i)
            args.emplace_back(argv[i]);
        LocalFree(argv);
        cliMode = !args.empty();
        if (cliMode) {
            AttachConsole(ATTACH_PARENT_PROCESS);
            SetConsoleCtrlHandler(cancelHandler, TRUE);
        }
        auto base = exeDirectory();
        if (!args.empty() && args[0] == L"--verify") {
            code = verify(base, args.size() > 1 ? fs::path(args[1]) : base / L"verification.txt");
            CoUninitialize();
            return code;
        }
        if (!args.empty() && args[0] == L"--smoke-test") {
            smokeMode = true;
            smokeReport = args.size() > 1 ? fs::path(args[1]) : base / L"smoke-test.txt";
            show = SW_HIDE;
        } else if (!args.empty())
            throw std::runtime_error(
                "Unknown argument. Supported: --verify [report-path] or --smoke-test [report-path]");
        State app;
        state = &app;
        app.base = base;
        app.data = dataDirectory();
        // A user-specific mutex also prevents two portable copies from racing on recovery journals.
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
            check(GetLastError(), "Resolve current user token");
        DWORD count = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &count);
        std::vector<BYTE> userData(count);
        if (!GetTokenInformation(token, TokenUser, userData.data(), count, &count)) {
            DWORD error = GetLastError();
            CloseHandle(token);
            check(error, "Read current user identity");
        }
        CloseHandle(token);
        LPWSTR sid = nullptr;
        if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(userData.data())->User.Sid, &sid))
            check(GetLastError(), "Format current user identity");
        std::wstring mutexName = L"Global\\CursorCabinet-" + std::wstring(sid);
        LocalFree(sid);
        mutex = CreateMutexW(nullptr, FALSE, mutexName.c_str());
        if (!mutex)
            check(GetLastError(), "Create application lock");
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            HWND existing = FindWindowW(L"CursorCabinetWindow", L"CursorCabinet");
            if (existing && !cliMode) {
                ShowWindow(existing, IsIconic(existing) ? SW_RESTORE : SW_SHOW);
                SetForegroundWindow(existing);
                CloseHandle(mutex);
                CoUninitialize();
                return 0;
            }
            throw std::runtime_error("CursorCabinet is already running for this Windows account.");
        }
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
        if (!InitCommonControlsEx(&controls))
            throw std::runtime_error("Cannot initialize Windows controls");
        WNDCLASSEXW klass{};
        klass.cbSize = sizeof(klass);
        klass.hInstance = instance;
        klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        klass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
        klass.hIconSm =
            static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(101), IMAGE_ICON, 16, 16, LR_SHARED));
        klass.lpszClassName = L"CursorCabinetWindow";
        klass.lpfnWndProc = windowProc;
        if (!RegisterClassExW(&klass))
            check(GetLastError(), "Register main window");
        klass.lpszClassName = L"CursorCabinetPreview";
        klass.lpfnWndProc = previewProc;
        if (!RegisterClassExW(&klass))
            check(GetLastError(), "Register preview window");
        HWND window =
            CreateWindowExW(0, L"CursorCabinetWindow", L"CursorCabinet", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                            CW_USEDEFAULT, 1220, 850, nullptr, nullptr, instance, nullptr);
        if (!window)
            check(GetLastError(), "Create main window");
        ShowWindow(window, show);
        UpdateWindow(window);
        MSG msg{};
        BOOL got;
        while ((got = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
            bool handled = false;
            if (msg.message == WM_KEYDOWN) {
                bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                if (ctrl && msg.wParam == 'F') {
                    SetFocus(app.search);
                    SendMessageW(app.search, EM_SETSEL, 0, -1);
                    handled = true;
                } else if (!app.busy) {
                    int action = 0;
                    if (ctrl && msg.wParam == 'D')
                        action = Favorite;
                    else if (ctrl && msg.wParam == 'O')
                        action = Import;
                    else if (msg.wParam == VK_F5)
                        action = Refresh;
                    else if (msg.wParam == VK_F1)
                        action = Help;
                    else if (msg.wParam == VK_RETURN && GetFocus() == app.list)
                        action = Apply;
                    else if (msg.wParam == VK_ESCAPE && GetFocus() == app.search)
                        action = ClearSearch;
                    if (action) {
                        SendMessageW(window, WM_COMMAND, action, 0);
                        handled = true;
                    }
                }
            }
            if (!handled && !IsDialogMessageW(window, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        if (got < 0)
            check(GetLastError(), "Read window message");
        if (smokeMode)
            code = smokeExitCode;
    } catch (const std::exception &e) {
        if (cliMode) {
            HANDLE error = GetStdHandle(STD_ERROR_HANDLE);
            DWORD written = 0;
            std::string text = std::string(e.what()) + "\n";
            if (error && error != INVALID_HANDLE_VALUE)
                WriteFile(error, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
        } else
            MessageBoxW(nullptr, message(e).c_str(), L"CursorCabinet", MB_OK | MB_ICONERROR);
        code = 3;
    }
    if (mutex)
        CloseHandle(mutex);
    CoUninitialize();
    return code;
}
