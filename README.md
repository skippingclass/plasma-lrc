<div align="center">

# Plasma LRC (KDE Plasma 6)

<p align="center">
  <strong>Fast, lightweight real-time karaoke &amp; synchronized lyrics widget for the KDE Plasma 6 panel</strong>
  <br />
  <em>10 multi-source providers · Syllable-level TTML, QRC, KRC, YRC &amp; Spicy sync · 1 fetch per track · 0% CPU at idle</em>
</p>

[![English](https://img.shields.io/badge/Language-English-blue?style=for-the-badge)](#)
[![Русский](https://img.shields.io/badge/Language-Русский-red?style=for-the-badge)](README.ru.md)

<br/>

[![KDE Plasma 6](https://img.shields.io/badge/KDE_Plasma-6.0+-blue?style=flat-square&logo=kde)](https://kde.org/)
[![Qt 6 / QML](https://img.shields.io/badge/Qt-6.5+-green?style=flat-square&logo=qt)](https://www.qt.io/)
[![C++17](https://img.shields.io/badge/C++-17-00599C?style=flat-square&logo=c%2B%2B)](https://en.cppreference.com/w/cpp/17)
[![CMake & Ninja](https://img.shields.io/badge/Build-CMake_%7C_Ninja-064F8C?style=flat-square&logo=cmake)](https://cmake.org/)
[![Build and test](https://github.com/skippingclass/plasma-lrc/actions/workflows/ci.yml/badge.svg)](https://github.com/skippingclass/plasma-lrc/actions/workflows/ci.yml)
[![AUR](https://img.shields.io/aur/v/plasma6-applet-lrc-git?label=AUR&logo=archlinux&style=flat-square)](https://aur.archlinux.org/packages/plasma6-applet-lrc-git)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow?style=flat-square)](LICENSE)

<br/>

<img src="assets/demo.gif" alt="Plasma LRC live lyrics demo with contributor credits" width="85%" />

<sub><i>Real-time word sync · Native multi-codec decoder · Instant MPRIS tracking · 0% CPU at idle</i></sub>

</div>

---

## Key Features

- **Syllable & Word-by-Word Karaoke**: Highlights each sung syllable and word in real time on your Plasma panel with zero stutter or layout shifting. Supports Apple Music TTML (`lrc.red`, `AMLL`), QQ Music QRC, KuGou KRC, NetEase YRC, and Spicy Lyrics.
- **Interactive Candidate Browser ("Choose another match…")**: Direct dialog to search and pick alternative matches across all enabled online providers. Features live sync indicators (**Word**, **Line**, **None**), rate-limit resilience, multi-tier caching (5 min fresh / 30 min stale reserve), and manual session locking.
- **10-Source Resilient Pipeline**: Fully customizable priority chain with automatic failover across 10 providers: Local Files, lrc.red, LRCLIB, NetEase Cloud Music, KuGou, Unison, Spicy Lyrics, AMLL TTML DB, QQ Music, and Custom HTTP Endpoints.
- **Hardened Security & Anti-Bomb Protection**: Strict incoming payload guards abort oversized responses in-flight (> 8 MiB) and prevent decompression bombs (> 16 MiB decompressed, > 10,000 lines, > 100,000 words, > 16,384 characters per line).
- **Sub-Millisecond Interpolation (0% Idle CPU)**: Fetches lyrics **once** upon track change. Time tracking runs via high-resolution monotonic `QElapsedTimer` with MPRIS latency compensation formula (`Base + Elapsed - Lead - audioLead - Offset`), consuming 0% CPU when paused or idle.
- **Secondary Backing Vocals**: Simultaneous secondary vocal line rendered with independent word sync, dim-to-bright white color ramp, and adaptive compact panel layouts.
- **In-Depth Synchronization Diagnostics**: Built-in telemetry window (**Diagnostics…**) inspecting live MPRIS latency formulas, provider states, candidate match verification, cache tiers, and vocal cursors with safe clipboard export.
- **Service Notice & Credit Filtering**: Automatically filters non-sung headers (`[Verse]`, `[Chorus]`, `[Куплет]`, `[Припев]`) and credits (`Written by:`, `作词：`, etc.) while preserving invisible time boundaries.
- **Smart Intros & Instrumental Solos**: Auto-detects song intros (`♪ Intro ♪`) and instrumental interludes (`♪ Instrumental ♪`), optionally showing musical note indicators or keeping the panel clean.

---

## Supported Lyrics Providers

Plasma LRC integrates 10 modular provider adapters with customizable priority ordering:

| Provider | Sync Quality | Formats | Auth Required | Description |
|:---|:---:|:---:|:---:|:---|
| **Local Files** | Word / Line | `.ttml`, `.lrc` | None | Local `.lrc` / `.ttml` next to audio, `~/.lyrics`, `~/Music/lyrics`, or custom folder. |
| **lrc.red** | Word (Syllable) | TTML, LRC | None | Community Apple Music TTML database with syllable sync, ISRC lookup, and LRC fallback. |
| **LRCLIB** | Line | Synced LRC | None | Fast, open-source community lyrics database with fuzzy track and duration matching. |
| **NetEase Cloud Music** | Word / Line | YRC, LRC | None | Cloud music database with syllable-level YRC sync and standard LRC fallback. |
| **KuGou Music** | Word / Line | KRC, LRC | None | Kugou lyrics engine with proprietary compressed KRC syllable sync and fallback. |
| **Unison** | Word / Line | TTML, LRC | None | Unison (`boidu.dev`) community API supporting TTML word sync and LRC. |
| **Spicy Lyrics** | Word (Syllable) | Native API | Free Client Key | Spotify community syllable-level synchronization with strict 30-day cache retention. |
| **AMLL TTML DB** | Word (Syllable) | TTML | None | Apple Music Like Lyrics database via global jsDelivr CDN mirrors. |
| **QQ Music** | Word / Line | QRC, LRC | None | QQ Music library with DES-encrypted QRC syllable sync and fallback LRC. |
| **Custom Endpoint** | Configurable | JSON / Text | Optional | Custom HTTP API supporting `{title}`, `{artist}`, `{album}`, `{durationMs}`, and `{spotifyId}` macros. |

---

## Architecture

```
┌────────────────────────────────────────────────────────────────────────┐
│                      MPRIS2 Media Player (D-Bus)                       │
│             Spotify · Firefox · Chromium · Strawberry · Cider          │
└───────────────────────────────────┬────────────────────────────────────┘
                                    │ Track change / Position (400ms)
┌───────────────────────────────────▼────────────────────────────────────┐
│                         LrcApplet (C++ Core)                           │
├────────────────────────────────────────────────────────────────────────┤
│ 1. MPRIS Engine: Active player scoring, URL decoding, title cleaner   │
│ 2. Lyrics Pipeline Manager (Configurable Priority Chain):             │
│    ├── 1. Local Files    ├── 5. KuGou Music    ├── 9. QQ Music         │
│    ├── 2. lrc.red        ├── 6. Unison         └── 10. Custom URL      │
│    ├── 3. LRCLIB         ├── 7. Spicy Lyrics                           │
│    └── 4. NetEase Music  └── 8. AMLL TTML DB                           │
│ 3. Native Decoders: TTML (XML), QRC (DES), KRC (XOR/Zlib), YRC, LRC    │
│ 4. Safety Guard: 8 MiB stream abort, 16 MiB decompression limit        │
│ 5. Time Engine: Monotonic QElapsedTimer local clock (0% idle CPU)      │
└───────────────────────────────────┬────────────────────────────────────┘
                                    │ Line text, word timings, progress
┌───────────────────────────────────▼────────────────────────────────────┐
│                         QML UI (KDE Plasma 6)                          │
│     Panel Widget · Interactive Match Dialog · Full Reader · Telemetry  │
└────────────────────────────────────────────────────────────────────────┘
```

---

## Requirements & Dependencies

| Component | Minimum Version | Purpose |
|:---|:---:|:---|
| **KDE Plasma** | 6.0+ | Desktop environment & Plasmoid shell |
| **KDE Frameworks** | 6.0+ (`KCoreAddons`, `KConfig`, `KI18n`) | System integration & localization |
| **Qt** | 6.5+ (`QtCore`, `QtGui`, `QtQml`, `QtNetwork`, `QtDBus`) | Application logic & QML rendering |
| **Compiler** | C++17 compliant (GCC 11+, Clang 14+) | Core engine compilation |
| **Build System** | CMake 3.20+ & Ninja | Project build and test automation |

---

## Installation

### Arch Linux (AUR)

```bash
yay -S plasma6-applet-lrc-git
# or with paru
paru -S plasma6-applet-lrc-git
```

### Quick Install (One-Line Script)

```bash
git clone https://github.com/skippingclass/plasma-lrc.git
cd plasma-lrc
./install.sh                # build, install system-wide, and restart plasmashell
./install.sh --no-restart   # build & install without restarting plasmashell
./uninstall.sh              # completely remove widget and config
```

### Manual Build with CMake & Ninja

```bash
git clone https://github.com/skippingclass/plasma-lrc.git
cd plasma-lrc
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
sudo cmake --install build
systemctl --user restart plasma-plasmashell.service
```

### Run Unit Tests (CTest)

Install Python 3 for the packaging integration test.

```bash
cmake -S . -B build -G Ninja -DPLASMA_LRC_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

### Adding Widget to Panel
1. Right-click your Plasma panel → **Add Widgets…**
2. Search for **Plasma LRC** and drag it to your preferred location on the panel.

---

## Context Menu Actions

Right-click the panel widget to access actions:

| Action | Description |
|:---|:---|
| **Choose another match…** | Picks alternative lyrics. Check **Remember for this song** to reuse the match on later plays. |
| **Use automatic selection** | Clears the manual match and returns to the automatic provider pipeline. |
| **Open song lyrics…** | Opens the full lyrics from the beginning in a scrollable reader. |
| **Import lyrics for this song…** | Previews a local `.lrc` file, saves a copy for the current track, and applies a song-specific timing offset. |
| **Remove imported lyrics** | Removes the widget's copy of the imported file. |
| **Forget saved lyrics settings for this song** | Resets the remembered match and song-specific timing offset. |
| **Copy current lyric line** | Copies the active sung lyric line to the clipboard (native LRCLIB / local sources). |
| **Discard lyrics for this track** | Temporarily dismisses lyrics for the current song without editing the blacklist. |
| **Restore lyrics for this track** | Restores temporarily dismissed lyrics from cache. |
| **Blacklist current track lyrics** | Permanently blacklists the current song or artist from lyrics retrieval. |
| **Diagnostics…** | Opens the comprehensive Search & Synchronization telemetry window. |
| **Configure Plasma LRC…** | Opens the configuration dialog and provider pipeline manager. |

Set **Song offset** in the song lyrics window while the displayed song is current and choose **Save for this song** to keep the correction across restarts. Positive values delay the lyrics. Imported copies are stored in `$XDG_DATA_HOME/plasma-lrc/imports` (normally `~/.local/share/plasma-lrc/imports`) and used when Local Files is enabled. If a remembered online match is unavailable, the widget falls back to the imported file or automatic search.

---

## Configuration Reference

Right-click the widget → **Configure Plasma LRC…**:

### Appearance Tab

| Setting | Default | Description |
|:---|:---:|:---|
| **Sung word** | `Underlined` | Karaoke highlight style: `Underlined`, `Bold`, `Highlight color`, `None` (line-by-line reading), or `Sung words become white` (dimmed unplayed lyrics light up word by word). |
| **Highlight color** | *(System Accent)* | Color for active sung words. Presets: Spotify Green (`#1ed760`), Cyan (`#00d4ff`), Purple (`#b342f5`), Yellow (`#ffd600`), Coral (`#ff4d4d`), White (`#ffffff`), or custom HEX. |
| **Transitions** | `Enabled` | Smooth fade animations between lines with highlight retention. |
| **Backing vocals** | `Disabled` | Displays secondary vocal line in parentheses with independent word-level sync. |
| **Alignment** | `Center` | Text alignment on the panel: `Center` or `Left`. |
| **Credit layout** | `Right of lyrics` | Attribution placement on the panel for Spicy sources: `Right of lyrics` or `Below lyrics`. |
| **Maximum length** | `40 characters` | Character truncation ceiling (0 = no limit). |
| **No lyrics text** | `♪` | Idle/placeholder glyph when no track or lyrics are active. |
| **Pauses & intros** | `Disabled` | Show `♪ Intro ♪` and `♪ Instrumental ♪` placeholders during breaks. |
| **Icon & Tooltip** | `Enabled` | Toggles the musical note panel icon and detailed MPRIS track tooltip. |

### Lyrics & Playback Tab

| Setting | Default | Description |
|:---|:---:|:---|
| **Update every** | `2000 ms` | MPRIS polling interval for player detection (500–10000 ms). |
| **Favourite player** | *(empty)* | Prefer this player if multiple are running (e.g., `spotify`, `cider`, `chromium`). |
| **Lock to player** | *(empty)* | Exclusively listen to one player and ignore all others. |
| **Lyric offset** | `0 ms` | Timing adjustment (−2000…+2000 ms) for early or late lyrics. |
| **Never ask** | *(empty)* | Comma-separated blacklist of MPRIS service names to ignore. |
| **Idle** | `Disabled` | Cease polling while nothing is playing to achieve 0% idle CPU usage. |
| **Pause delay** | `5 seconds` | Delay before lyrics fade out when paused (0 = never hide). |
| **Left / Middle click** | `Show popup` / `Play/Pause` | Assignable click triggers: `Show popup`, `Play / Pause`, `Next track`, or `None`. |
| **Timestamps** | `Disabled` | Displays `[mm:ss]` prefix before the active lyric line. |

### Source Tab & Provider Pipeline

| Setting | Default | Description |
|:---|:---:|:---|
| **Provider Pipeline** | `lrc.red → LRCLIB → NetEase → KuGou → Unison → Spicy → AMLL → QQ Music` | Drag-and-drop priority reordering and individual enable/disable checkboxes for all 10 providers. |
| **Spicy Lyrics API key** | *(empty)* | Optional personal client key (`sl_pk_…`) from the Spicy Lyrics catalog. |
| **Local lyrics folder** | *(empty)* | Additional custom directory searched for `.lrc` and `.ttml` files. |
| **Custom lyrics URL** | *(empty)* | Custom HTTP template supporting `{title}`, `{artist}`, `{album}`, `{durationMs}`, and `{spotifyId}` macros. |

---

## Security, Privacy & Cache Policy

- **Streaming Size Limits**: All incoming network payloads are wrapped in size-limited streams, aborting transfers exceeding **8 MiB** immediately to protect network and memory.
- **Decompression Bomb Protection**: Zlib and archive decoders reject expanded data exceeding **16 MiB**, preventing compression bombs.
- **Strict 30-Day Cache Retention**: Cached lyrics have a hard expiration deadline of 30 days maximum, automatically purged on startup and during maintenance passes.
- **Zero Credential Leaks**: Diagnostics export and logs mask all sensitive tokens, secret keys, and authorization headers. Full lyric texts are never dumped into diagnostic clipboard reports.

---

## Troubleshooting

<details>
<summary><strong>Panel is empty while music is playing</strong></summary>

1. Ensure your media player supports MPRIS2 (e.g. Spotify, Strawberry, Cider, VLC, Firefox, Chrome).
2. If multiple players are open, specify your preferred player under **Configure Plasma LRC → Lyrics → Favourite player** (e.g. `spotify`).
3. Check **Right-click → Diagnostics…** to verify that metadata is reaching the applet.
</details>

<details>
<summary><strong>Word-by-word karaoke is not highlighting</strong></summary>

1. Check if the song has syllable/word timings available. Look at **Right-click → Choose another match…** to see if providers list **Word** sync for the song.
2. If only **Line** sync is available, Plasma LRC will smoothly display the line-synced text.
3. For Spotify-specific word sync via Spicy Lyrics, verify that your client key is configured in the **Source** tab.
</details>

<details>
<summary><strong>Lyrics are slightly ahead or behind audio</strong></summary>

Open **Configure Plasma LRC → Lyrics** and adjust the **Lyric offset** slider (−2000 ms to +2000 ms) in 50 ms increments to achieve perfect audio alignment.
</details>

---

## License

Distributed under the [MIT License](LICENSE).
