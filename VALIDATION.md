# Native release validation

Validation date: October 18, 2026. Version: 0.2.0 preview.

## Completed checks

- MSVC C++17 x64 build, static C++ runtime, `/W4 /WX`, stack protection, control-flow guard, ASLR, high-entropy ASLR, and DEP.
- Only Windows system DLL imports; no CLR, interpreter, or separately installed runtime dependency.
- 29 core assertions passed, including partial registry write, disk full, reload failure, failed rollback, journal corruption, Unicode favorites, settings, INF parsing, and isolated registry read-back.
- All 138 bundled schemes and 2,196 mapped cursor files loaded successfully, with zero catalog notices.
- Deep portable extraction regression: the same 2,196 files loaded from a nested path exceeding the USER32 filename limit. Temporary app-owned staging preserves CUR hotspots and ANI bytes and deletes each staging file after loading.
- Hidden native window test: search and empty-result behavior passed; 20 preview selections completed with stable GDI and USER object counts.
- Real x64 apply/restore integration: applied an original pack, verified registry read-back, restored all touched values byte-for-byte, then validated replay of a durable recovery journal. The original Windows cursor scheme was restored.
- Native ARM64 application and test executables cross-compiled successfully. An ARM64 GitHub Actions job is included for execution on Windows ARM64.
- Installer and portable package checks are automated by `scripts/test-packages.ps1`; the release check report records the tested artifacts.

## Performance sample

The x64 native smoke test with 138 schemes recorded zero process CPU time over a two-second idle interval. Private committed memory was approximately 4.7 MiB and working set approximately 16.2 MiB. GDI and USER object counts remained stable over 20 preview selections.

The working set exceeds the requested 15 MiB target. This is a measured limitation, not a claimed pass; no forced working-set trimming or artificial metric cap is used. Windows version, theme, DPI, antivirus hooks, and system DLL residency can change the measurements. Production mode has no idle timer; the two-second timer is test-only.

## Compatibility and release limits

- Local execution was tested on the current x64 Windows host. Windows 10 1809 and ARM64 hardware execution require additional validation. Cross-compilation and CI configuration are not substitutes for those checks.
- Application and setup artifacts are unsigned; no code-signing identity was supplied.
- Installer data is per user. Active cursor files and restore data are deliberately retained outside the install directory during uninstall.
- The portable edition does not install itself, but stores preferences and active cursor copies in `%LOCALAPPDATA%\CursorCabinet`; settings are not fully travel-with-the-folder portable.
- Import supports adjacent CUR/ANI files mapped through supported INF registry records or scheme-order mappings. Unsupported layouts are reported instead of executing installer scripts.
- Original artwork source archives and licensing notices are included with both distribution formats. DeviantArt content is linked and not redistributed.
- Native packaging is a preview release until the remaining compatibility and memory-target work is complete.
