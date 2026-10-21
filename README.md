<div align="center">

# Plasma LRC (KDE Plasma 6)

<p align="center">
  <strong>Fast, lightweight lyrics &amp; real-time karaoke widget for the KDE Plasma 6 panel with Spicy Lyrics word-by-word sync and universal LRC support</strong>
</p>

[![English](https://img.shields.io/badge/Language-English-blue?style=for-the-badge)](#)
[![Русский](https://img.shields.io/badge/Language-Русский-red?style=for-the-badge)](README.ru.md)

<br/>

[![KDE Plasma 6](https://img.shields.io/badge/KDE_Plasma-6.0+-blue?style=flat-square&logo=kde)](https://kde.org/)
[![Qt 6 / QML](https://img.shields.io/badge/Qt-6.5+-green?style=flat-square&logo=qt)](https://www.qt.io/)
[![C++17](https://img.shields.io/badge/C++-17-00599C?style=flat-square&logo=c%2B%2B)](https://en.cppreference.com/w/cpp/17)
[![AUR](https://img.shields.io/aur/v/plasma6-applet-lrc-git?label=AUR&logo=archlinux&style=flat-square)](https://aur.archlinux.org/packages/plasma6-applet-lrc-git)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow?style=flat-square)](LICENSE)

<br/>

<img src="assets/demo.gif" alt="Plasma LRC Live Karaoke Demo" />

<sub><i>Real-time word sync · Native LRC parser · 0% CPU at idle · 1 fetch per track</i></sub>

</div>

---

## Features

- **Real-Time Word-by-Word Karaoke**:  
  Highlights every sung word in real time on your Plasma panel using the Spicy Lyrics API (Spotify). Syllable groups are smoothly merged with zero stutter or text jumping.
- **Customizable Highlight Palette**:  
  Customize your lyric highlight color with built-in presets (Spotify Green, Cyan, Purple, Yellow, Coral, White) or any custom `#RRGGBB` hex code. Choose between highlight color, bold, or underline styles.
- **1 Fetch Per Song Architecture**:  
  Instead of polling external tools multiple times per second, the widget fetches lyrics once on track change (native LRCLIB client / Spicy API) and runs local time interpolation using C++ `QElapsedTimer`. CPU usage drops to 0% at idle.
- **Intelligent MPRIS Engine**:  
  Auto-detects active music players, avoids browser media tab lock-in, strips video noise from titles (`[Official Music Video]`, `4K`, etc.), and prioritizes your favorite player.
- **Clean Instrumental Breaks**:  
  During musical solos and song intros, the panel stays clean instead of displaying outdated phantom lines. Configurable pause hide delay gently hides text when playback is paused.
- **Customizable Mouse Actions**:  
  Configure Left-click (Popup / Play-Pause) and Middle-click (None / Play-Pause / Next Track) right from your panel.
- **Smooth Visual Transitions**:  
  Subtle fade animations between lyric lines without any flickering during word-to-word singing.

---

## Architecture

```
┌────────────────────────────────────────────────────────┐
│               MPRIS2 Media Player (D-Bus)              │
│       Spotify / Firefox / Chromium / Strawberry        │
└───────────────────────────▲────────────────────────────┘
                            │ Track change / Metadata / Position (400ms)
┌───────────────────────────┴────────────────────────────┐
│                  LrcApplet (C++ Core)                  │
├────────────────────────────────────────────────────────┤
│ 1. Track detection & active player scoring             │
│ 2. Single-shot lyrics fetch per track:                 │
│    ├── Spotify + API Key  ──►  Spicy Lyrics HTTP API   │
│    └── Other players      ──►  Native LRCLIB API       │
│ 3. LrcParser (C++): Fast in-memory [mm:ss.xx] parser   │
│ 4. QElapsedTimer: Local monotonic line & word timing   │
└───────────────────────────┬────────────────────────────┘
                            │ Text, active word, progress
┌───────────────────────────▼────────────────────────────┐
│                 QML UI (KDE Plasma 6)                  │
│       Compact Panel Label + Rich Popup Card View       │
└────────────────────────────────────────────────────────┘
```

---

## Requirements

| Component | Requirement | Purpose |
|---|---|---|
| **KDE Plasma** | 6.0+ (Frameworks 6) | Desktop environment |
| **Qt / C++** | Qt 6.5+, C++17 | Applet binary & QML engine |
| **Spicy Lyrics Key** | Optional | Required for word-by-word karaoke on Spotify |

---

## Installation

### Arch Linux (AUR)

```bash
yay -S plasma6-applet-lrc-git
```

### Quick Install (One-Line Script)

```bash
git clone https://github.com/skippingclass/plasma-lrc.git
cd plasma-lrc
./install.sh                # build, install system-wide, and restart plasmashell
./install.sh --no-restart   # build & install without restarting plasmashell
./uninstall.sh              # completely remove widget and config
```

### Manual Build with CMake

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
sudo cmake --install build
systemctl --user restart plasma-plasmashell.service
```

### Adding Widget to Panel
1. Right-click your Plasma panel → **Add Widgets...**
2. Search for **Plasma LRC** and drag it to your panel.

---

## Settings Reference

Right-click the widget on your panel → **Configure Plasma LRC...**:

### Appearance Tab

| Setting | Default | Description |
|---|---|---|
| **Sung word** | `Underlined` | Karaoke highlight style: `Underlined`, `Bold`, or `Highlight color`. |
| **Highlight color** | *(System accent)* | Custom color for active word. Presets: Spotify (`#1ed760`), Cyan (`#00d4ff`), Purple (`#b342f5`), Yellow (`#ffd600`), Coral (`#ff4d4d`), White (`#ffffff`). |
| **Transitions** | `Enabled` | Smooth fade animation when switching lines. |
| **Alignment** | `Center` | Text alignment on the panel: `Center` or `Left`. |
| **Panel** | `Standard` | Compact mode leaves the panel to lyrics alone (moving contributor credits to popup). |
| **Maximum length** | `40 characters` | Truncates text after N characters to preserve panel space (0 = no limit). |
| **No lyrics text** | `♪` | Placeholder text when no track or lyrics are active. |
| **Icon** | `Enabled` | Shows music note icon next to lyrics. |
| **Tooltip** | `Enabled` | Shows current track title and artist in the tooltip. |

### Lyrics Tab

| Setting | Default | Description |
|---|---|---|
| **Update every** | `2000 ms` | Polling interval for player detection (500–10000 ms). |
| **Favourite player** | *(empty)* | Prefer this player if multiple players are running (e.g. `spotify`). |
| **Lock to player** | *(empty)* | Exclusively listen to one player and ignore all others. |
| **Lyric offset** | `0 ms` | Timing adjustment (−2000…+2000 ms) for early or late lyrics. |
| **Never ask** | *(empty)* | Comma-separated blacklist of MPRIS players to ignore. |
| **Idle** | `Disabled` | Stop polling while nothing is playing to reduce CPU usage to 0%. |
| **Pause delay** | `5 seconds` | Seconds before lyrics fade out while paused (0 = never hide). |
| **Left click** | `Show popup` | Action on left click: `Show popup` or `Play / Pause`. |
| **Middle click** | `Play / Pause` | Action on middle click: `None`, `Play / Pause`, or `Next track`. |
| **Timestamps** | `Disabled` | Shows timestamp prefix `[mm:ss]` before line. |

### Blacklist Tab

| Setting | Default | Description |
|---|---|---|
| **Blacklisted tracks** | *(empty)* | Semicolon-separated list of song titles or `Artist - Title` to block from lyric fetching. Can also be toggled via context menu or popup. |

### Source Tab

| Setting | Default | Description |
|---|---|---|
| **Spicy Lyrics** | `Enabled` | Enables community word-level sync for Spotify tracks. |
| **API key** | *(empty)* | Your personal client key `sl_pk_…` from the app template. |

---

## Spicy Lyrics Setup (Word-by-Word Karaoke)

[Spicy Lyrics](https://spicylyrics.org) provides synchronized word-level timings for Spotify tracks:

1. Add Plasma LRC through its approved app template in the [Spicy Lyrics catalog](https://developers.spicylyrics.org/catalog), then copy the issued **client key** (`sl_pk_…`). A direct template link will be published here after approval. Do not create a standalone developer application for the widget.
2. Paste your key in **Configure Plasma LRC → Source**, or export it in your environment:

```bash
export SPICY_LYRICS_SECRET_KEY=sl_pk_...
```

*The environment variable keeps its legacy name and accepts client keys too. It takes precedence over GUI settings. For a native desktop client, the template must allow requests without an Origin header.*

If the API reports that the application is suspended, check the developer dashboard. Changing the token does not lift the suspension. Previously cached lyrics and the LRCLIB fallback may still work.

---

## Troubleshooting

- **Panel is empty while music is playing:**  
  Verify that your player supports MPRIS2 and is currently playing audio. If your player is not picked up automatically, enter its name in **Favourite player** (e.g. `spotify` or `chromium`).
- **Word-by-word highlight is not active:**  
  Word-by-word highlighting is provided for Spotify tracks via Spicy Lyrics. Other players and tracks without Spicy sync automatically use clean line-by-line synced LRC via LRCLIB.
- **Lyrics are slightly ahead or behind audio:**  
  Open settings → **Lyrics** → adjust **Lyric offset** slider (−2000ms to +2000ms).

---

## License

Distributed under the [MIT License](LICENSE).
