# Cursor artwork and build tools

CursorCabinet application source is MIT licensed. The separate cursor artwork collections retain their upstream licenses; bundling them does not relicense their artwork under MIT.

The release contains upstream LICENSE.txt and SOURCE.txt files in each Packs collection. Corresponding source archives, including upstream copyright notices and build resources, are in Sources/. Exact release tags, download URLs, and SHA256 hashes are recorded in packs.lock.json.

| Collection | Upstream | License |
| --- | --- | --- |
| Bibata | ful1e5/Bibata_Cursor | GPL-3.0 |
| Fuchsia | ful1e5/fuchsia-cursor | GPL-3.0 |
| Rose Pine | rose-pine/cursors | GPL-3.0 |
| GoogleDot | ful1e5/Google_Cursor | GPL-3.0 |
| Apple Cursor | ful1e5/apple_cursor | GPL-3.0 |
| XCursor Pro | ful1e5/XCursor-pro | GPL-3.0 |
| Material Bibata | SakibShahariar/material-bibata-cursor | Upstream includes MIT for scripts and GPL-3.0 for Bibata-derived artwork; consult its included notices. |

Some upstream release archives contain installation scripts. CursorCabinet never runs them; it reads supported INF cursor mappings as data.

DeviantArt entries are external artist links, not bundled downloads. Artist terms, accounts, and free-edition availability apply.

The app uses Windows system libraries and the statically linked Microsoft C++ runtime. Installer builds use the open-source Inno Setup compiler (https://github.com/jrsoftware/issrc), a build-time tool not required by portable users. Its installer engine includes its upstream copyright notices. No analytics, advertising, account services, or network downloads run inside the application.
