# Plasma LRC: app template reference & resubmission draft

> **Status:** DRAFT (pending v1.6.0 release confirmation by repository owner).
> The previous template submission was refused; no resubmission has taken place yet, and approval has not been granted. Reviewer approval is required before a direct link will be available in the catalog. Approval is not guaranteed.
> Reference release: v1.6.0 — https://github.com/skippingclass/plasma-lrc/releases/tag/v1.6.0
> Visual demonstrations remain unembedded text; screenshots or logos without full credits must not be included.

Copy the English text below into the catalog template form. Per ToS 12 guidelines, catalog fields use neutral wording ("the lyrics API" / "community synchronization provider") rather than third-party service branding. The user count is left for the maintainer to fill in. Do not submit keys.

---

### Catalog Fields

**Name**

Plasma LRC

**One-line description**

Synchronized lyrics and word-by-word highlighting for the KDE Plasma 6 panel.

**Description**

Plasma LRC is an open-source widget for KDE Plasma 6. It displays the currently sung lyric line in the desktop panel and highlights words when word-level timings are available.

The widget follows local MPRIS-compatible music players. For Spotify tracks, it connects to the lyrics API using the user's personal client key obtained via the template, falling back to LRCLIB when synchronized lyrics are unavailable.

It runs locally on each user's computer without a shared backend or bundled secret. Complete attribution is always visible across the panel, popup, and song lyrics reader: the provider name is always displayed, and clickable links to the sync maker and uploader are shown whenever provided by the community synchronization source (where source=spicy_lyrics, for Spotify and Apple Music providers). Attribution cannot be hidden or disabled. (Native LRCLIB attribution is displayed in the popup and reader only).

Lyrics are rendered directly in the widget and song lyrics reader for display purposes; no automated download, file export, or raw API response endpoints exist. Standard on-screen text selection and manual keyboard copy (Ctrl+C) remain functional, but dedicated copy buttons ("Copy all lyrics", "Copy selection", and context menu "Copy current lyric line") are reserved strictly for native LRCLIB lyrics.

All stored API responses have a strict maximum lifetime of 30 days. Payloads with unknown or non-positive expiration times are rejected. Stored responses are automatically purged on widget startup, on access, and during periodic maintenance passes while the application is running (in-memory cache entries and open-reader snapshots expire under the same policy; no background deletion occurs while the application or computer is stopped). Line-synced fallbacks are refreshed after 2 days to pick up newly added community syllable syncs.

After adding this application from the template, paste the issued client key into the widget's settings under Source → API key (or set SPICY_LYRICS_SECRET_KEY; the environment variable preserves its legacy name for backward compatibility and accepts client keys, without requiring a secret key).

**Category**

Desktop apps

**Project URL / Source code URL**

https://github.com/skippingclass/plasma-lrc

**Requested key**

Client key. Allow requests with no Origin header: this is a native Qt desktop widget, not a browser application. Requests are made directly by Qt's network client without an Origin header.

**Why this needs to be a template**

This is a publicly distributed, open-source KDE Plasma desktop widget. Each installation makes requests to the lyrics API directly from the user's computer; there is no central backend or shared API key.

Each user therefore needs their own client key and individual rate limit. The template enables users to generate their own client keys directly upon approval, rather than creating standalone developer applications.

Approximate number of users: [insert your estimate, or state that it is unknown].

---

### Maintainer Correspondence Drafts

#### Response to suspension notice (DRAFT)

*Note: For maintainer reference. Label as DRAFT until release v1.6.0 is confirmed by root.*

Hi! Thanks for clarifying, and sorry I missed the notice — I didn't receive an email notification. I'll submit Plasma LRC as an app template and update the setup instructions so users obtain their own application and keys through the approved template once available. I'll also remove any instructions asking users to create a standalone application. Could you please review the suspension once these changes are in place? Thanks!

#### Reviewer response draft (resubmission draft — v1.6.0)

*Label: DRAFT — pending v1.6.0 release confirmation by repository owner. No guarantee of approval.*
*Release URL:* https://github.com/skippingclass/plasma-lrc/releases/tag/v1.6.0

Hi! In release v1.6.0 (https://github.com/skippingclass/plasma-lrc/releases/tag/v1.6.0), Plasma LRC has been updated to address all review requirements:

- **Mandatory complete attribution:** Provider attribution is always visible across the panel, popup, and full song lyrics reader. The provider name is always displayed, and individual clickable links for both sync maker and uploader are shown whenever provided by the community synchronization source (`source=spicy_lyrics`, Spotify and Apple Music providers). Any setting or mode to hide attribution (including legacy `compactPanel`) has been completely removed. Configurable panel layouts ("Right of lyrics" and "Below lyrics") maintain permanent attribution visibility. (Native LRCLIB attribution is shown in popup/reader only).
- **Frontend fixes & tooltip:** The panel tooltip no longer repeats the sung lyric line (eliminating uncredited lyric display in tooltips) and displays the active media player and provider name. Long lyric lines use single-line styled text with hardware clipping and guaranteed floor width so panel space cannot squeeze out or obscure the attribution block.
- **Copying and export limits:** The reader window supports all sources for display, but dedicated copy buttons ("Copy all lyrics", "Copy selection", and context menu "Copy current lyric line") are reserved strictly for native LRCLIB lyrics. For Spicy Lyrics API tracks, users may only manually select on-screen text with the cursor and copy via Ctrl+C; no bulk copy buttons, automated export, file download, or raw API response endpoints exist.
- **Hard cache deadline & expiry:** All stored API payloads have a hard deadline of 30 days maximum, and responses with unknown or non-positive expiration deadlines are rejected. Expired entries are automatically purged on widget startup, on access, and during periodic maintenance timer passes while the widget is running. In-memory cache entries and open-reader snapshots expire under the same strict policy (no background deletion occurs while the applet or computer is stopped). Line-synced fallbacks are refreshed every 2 days.
- **Backend suspension behavior:** When a known `application_suspended` response is received, the widget immediately blocks serving cached API lyrics until a fresh authorized HTTP 200 response is obtained from the service. Ordinary network failures and rate limits (HTTP 429) continue to permit serving valid unexpired cache. Native LRCLIB continues to operate independently. Any existing raw cache remains strictly local, is never shared, and expires normally under the retention policy.
- **Native template distribution:** The native Qt desktop client requests no Origin header and bundles no shared secret key. Each user is onboarded to obtain their own personal client key (`sl_pk_...`) through the approved app template upon approval. The legacy environment variable `SPICY_LYRICS_SECRET_KEY` accepts client keys for backward compatibility without exposing or requiring private developer secrets.

These updates are implemented in v1.6.0. We respectfully submit this draft for review and hope it satisfies all requirements for template approval.
