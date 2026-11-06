# Launcher attribution

Adapted from the user-supplied `E:\bloodborne_pc-windows\launcher\bbport_launcher_win.py`.
Source project identified by its own release URL: https://github.com/Supermedo/bloodborne_pc .
Source file carries SPDX-License-Identifier: GPL-2.0-or-later. The accompanying license is retained as LICENSE.

Reused methods: px, check_images, style, next_row, row, note, section. Theme palette changed to purple. The remaining Bayonetta launcher code is new and connects these UI helpers to ScarletNX. Bloodborne game execution, updater, online services, patches, artwork, and credentials are not imported.

Original source SHA256: ac6d9b17c183fc60942e070f7519de7a47cf7b27949f36f2668ff53fb739187c

## The launcher window (web_launcher.py, ui/)

`web_launcher.py` and `ui/index.html` are new, B3-Express's own (GPL-3.0-or-later). The window is drawn by Microsoft
Edge WebView2, which Windows provides, through these Python packages, unpacked unchanged into the bundled Python's
`Lib/site-packages`, each file checked against the SHA-256 PyPI publishes. Their licence files are in their
`.dist-info` folders:

| package | version | licence |
|---|---|---|
| pywebview | 6.2.1 | BSD-3-Clause |
| pythonnet | 3.2.1 | MIT |
| clr_loader | 0.3.1 | MIT |
| cffi | 2.1.1 | MIT-0 |
| pycparser | 3.12 | BSD-3-Clause |
| proxy_tools | 0.1.0 | MIT (from its source package) |
| bottle | 0.13.4 | MIT |
| typing_extensions | 4.16.0 | PSF-2.0 |

Fonts in `ui/fonts`: Italiana (Santiago Orozco), Manrope (The Manrope Project Authors) and IBM Plex Mono (IBM),
the Latin subsets Google Fonts serves, under the SIL Open Font License 1.1 (`ui/fonts/OFL.txt`).
