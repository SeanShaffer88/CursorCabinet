# CursorCabinet engineering rules

- Keep the application native C++17/Win32 with no managed or third-party runtime requirement. Build tools may use PowerShell and the Windows SDK.
- Preserve permanently free, open-source operation: no analytics, ads, accounts, trials, license checks, or background download service.
- Run as the current standard user. Registry writes are restricted to the documented cursor values in `HKCU\Control Panel\Cursors`. Installer bookkeeping remains application-scoped.
- Keep file/registry operations on the bounded background worker. Do not poll at idle. Reclaim Windows resources deterministically.
- Save and verify a durable rollback snapshot before cursor-setting changes. Keep a recovery journal if rollback fails. Never silently discard filesystem or registry failures.
- Reject path traversal, junctions, and symlinks when importing or staging files. Keep temporary and installed data under application-owned directories.
- Cursor artwork has separate licenses. Preserve pinned hashes, source archives, and notices when packaging; never silently replace a pinned release with a latest download.
- Dependency order for architectural changes: types and interfaces -> core/Win32 adapters -> UI -> tests -> packaging.
- Run `./build-native.ps1 -Test` for relevant core changes. Live integration tests briefly modify real user cursors; preserve byte-for-byte restoration and keep these tests separate from default unit tests.
- Run catalog verification and native UI smoke tests before packaging. Validate installer install/upgrade/uninstall and extracted portable artifacts before release.
- After two fixes/build attempts fail with the same symptoms, stop speculative retries and report the root cause.
- Keep measured compatibility, performance, signing, and test limitations explicit in VALIDATION.md. Do not claim ARM64 runtime validation from cross-compilation alone.
