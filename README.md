# CursorCabinet

**Find your next favorite cursor.**

CursorCabinet is a Windows desktop app for browsing free cursor sets, previewing their pointers, and applying a new look in a few clicks.

## Features

- **138 bundled schemes** across seven projects, including color, size, and handedness variants.
- Separate gallery cards show all mapped pointers together for each set, with collection and role counts. Search, collection filters, and favorites narrow the gallery.
- A selected-set inspector labels every cursor role. Switch between light and dark preview surfaces and hover over a pointer to try it inside the app.
- Apply a set to your Windows account and restore the immediately preceding scheme.
- Import supported cursor folders and discover more sets on DeviantArt.
- Browse downloaded collections offline.
- Sort by name or collection, keep favorites, and remember window size and filters.
- See the current Windows scheme, open mouse settings, and access local diagnostic logs.
- Keyboard shortcuts, tooltips, DPI-aware layout, and an installer with optional desktop shortcuts.
- Durable rollback journals recover interrupted changes automatically on the next launch.

## Installer and portable editions

Both editions launch through **CursorCabinet.exe**. The app is native C++/Win32 with a statically linked C++ runtime. End users need no .NET, Python, terminal, or runtime installation.

| Edition | Experience | Current status |
| --- | --- | --- |
| Installer | Run the versioned `CursorCabinet-…-Setup.exe`, then open CursorCabinet from the Start menu. Installs for the current user. | Built with Inno Setup. |
| Portable | Extract the versioned `CursorCabinet-…-Portable.zip` and double-click `CursorCabinet.exe`. Keep its `Packs` folder beside it. | No installation required. |

Get packages from [GitHub Releases](https://github.com/SeanShaffer88/CursorCabinet/releases). SHA256 checksum files accompany the packages. Initial native releases are previews while the compatibility matrix is expanded.

Targets Windows 10 version 1809 or newer and Windows 11. Native x64 and ARM64 build configurations are included. Local execution tests were performed on x64; ARM64 binaries were cross-compiled and need native hardware validation. The portable app stores preferences and installed cursors in the current user's application data folder, so settings do not travel with the executable.

## Getting started

1. Open CursorCabinet.exe.
2. Browse or search the library and select a set.
3. Preview its cursor roles and choose **Apply selected set**.
4. Use **Restore previous** to return to the scheme saved before the latest application.

Applying a set affects the current Windows account without requiring administrator access. Missing roles use Windows defaults. Restore previous keeps one snapshot, rather than a full history.

| Shortcut | Action |
| --- | --- |
| Ctrl+F | Focus and select the search text |
| Ctrl+D | Toggle the selected set as a favorite |
| Ctrl+O | Import an extracted cursor folder |
| F5 | Refresh the library |
| Enter in the cursor list | Apply the selected set |
| Escape in search | Clear the search |
| F1 | Open help and license information |

Click a column heading to sort; click it again to reverse the order. Hover over a preview to try its cursor inside the app. Unavailable custom roles use Windows defaults.

### Importing sets

Download a cursor pack from its creator, extract it, then choose **Import folder**. Supported packs contain an INF mapping cursor roles to adjacent CUR/ANI files. Other layouts and packs without a supported mapping may not import. Installer scripts are never executed.

The DeviantArt menu links to artist pages for Azure, Pointa, Kami Paper White, and Cursor-Zanate F, plus a broader search. These downloads are separate from the bundled library and may require an account. Choose the free edition where offered and follow the creator's usage terms. No DeviantArt artwork is redistributed in the packages.

## Collections and credits

The initial collection includes:

- [Bibata](https://github.com/ful1e5/Bibata_Cursor)
- [Material Bibata](https://github.com/SakibShahariar/material-bibata-cursor)
- [Fuchsia](https://github.com/ful1e5/fuchsia-cursor)
- [Rose Pine](https://github.com/rose-pine/cursors)
- [GoogleDot](https://github.com/ful1e5/Google_Cursor)
- [Apple Cursor](https://github.com/ful1e5/apple_cursor)
- [XCursor Pro](https://github.com/ful1e5/XCursor-pro)

Application code is [MIT licensed](LICENSE). Artwork belongs to its creators and retains its upstream licenses. Packages include collection LICENSE.txt and SOURCE.txt files, hash-pinned source archives in `Sources/`, and [third-party notices](THIRD_PARTY_NOTICES.md). Free downloads do not automatically grant unrestricted redistribution rights.

## Development

### Building the executable

Requires Visual Studio C++ Build Tools with a Windows 10/11 SDK. The ARM64 target additionally needs the matching ARM64 compiler and C++ libraries. PowerShell is used only for development and packaging.

    ./build-native.ps1 -Architecture x64 -Test
    ./scripts/download-packs.ps1 -Destination ./dist/x64
    ./dist/x64/CursorCabinet.exe

To build for ARM64 with its compiler installed:

    ./build-native.ps1 -Architecture arm64 -Test

The downloader verifies every archive against `packaging/packs.lock.json`, including exact release tags and SHA256 hashes. App startup never contacts the network. Generated executables, download caches, and packs are excluded from Git.

### Packaging

Install or supply the Inno Setup compiler, then run:

    ./package.ps1 -Architecture x64 -InnoCompiler 'C:/Path/To/ISCC.exe'

Use `-SkipInstaller` for a portable-only build. Output is written to `releases/`. Each package contains the cursor files, artwork source archives, licenses, and application source. Existing staging folders are preserved; use a new version or explicitly remove an obsolete stage before repackaging.

The installer creates a Start menu shortcut and offers an optional desktop shortcut. Uninstalling removes packaged application files but preserves application data because Windows may still be using cursor files there. Restore your previous scheme before manually deleting installed cursor data.

| Path | Contents |
| --- | --- |
| native/ | Win32 interface, cursor catalog, registry adapter, recovery journals |
| tests/ | Isolated core tests and explicit live apply/restore tests |
| build-native.ps1 | x64 / ARM64 native compiler entry point |
| scripts/download-packs.ps1 | Hash-verified cursor and corresponding-source downloader |
| packaging/ | Installer definition and pinned artwork manifest |
| assets/, tools/brand.cpp | App icon, installer branding, native asset generator |
| package.ps1 | Portable ZIP, installer, source staging, and checksums |
| dist/x64/, dist/arm64/ | Generated native executables and packs |

### Verification

Run `CursorCabinet.exe --verify [report-path]` to check every bundled cursor without applying a scheme. Run `--smoke-test [report-path]` to exercise the hidden native window, search, empty results, and repeated previews and record CPU/memory/handle metrics. Verification returns 0 on success, 1 on cursor failures, 2 for an empty library, 3 on an operational error, or 130 on cancellation. Tests fail through exit codes, not hidden error dialogs.

The native catalog validates **138 schemes and 2,196 mapped cursor files with zero load failures**. Core tests cover partial registry writes, disk-full staging failures, reload failure, failed rollback, journal corruption, Unicode, preferences, supported INF layouts, and isolated registry read-back. An explicit integration test applies a real set and verifies byte-for-byte restoration of the original settings and crash recovery.

    ./build/x64/IntegrationTests.exe ./dist/x64/Packs ./build/integration-report.txt

This integration command briefly changes the current user's pointers and restores them before returning. It is deliberately separate from routine unit tests. See [VALIDATION.md](VALIDATION.md) for measured results and remaining compatibility limits.

### Local data

Data lives under `%LOCALAPPDATA%\CursorCabinet`: settings.ini, favorites.txt, Imported/, Installed/, previous.ccj, and bounded local diagnostic logs. Preview/ holds temporary copies when Windows cannot load a deeply nested cursor path; those files are removed immediately after loading. A pending.ccj journal is retained if rollback cannot finish. Applied files are copied there so moving or uninstalling the app does not break the active scheme. The earlier C# prototype's CursorLibrary data folder is not migrated.

CursorCabinet contains no accounts, ads, trials, license checks, analytics, or background update service. Logs remain local. External artist links open only when requested.
