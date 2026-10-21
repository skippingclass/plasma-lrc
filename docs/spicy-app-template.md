# Plasma LRC: app template submission

Copy the English text below into the Spicy Lyrics template form. The user
count is deliberately left for the maintainer to fill in. Do not submit keys.

**Name**

Plasma LRC

**One-line description**

Synchronized lyrics and word-by-word highlighting for the KDE Plasma 6 panel.

**Description**

Plasma LRC is an open-source widget for KDE Plasma 6. It displays the currently
sung lyric line in the desktop panel and highlights words when word-level
timings are available.

The widget follows local MPRIS-compatible music players. For Spotify tracks,
it uses the API with the user's own key and falls back to LRCLIB when
synchronized lyrics are unavailable.

It runs locally on each user's computer, without a shared backend. API responses
are cached locally for playback, and lyrics are displayed inside the widget
rather than offered as downloads.

After adding this application, paste the issued client key into the widget's
settings under Source → API key.

**Category**

Desktop apps

**Project URL / Source code URL**

https://github.com/skippingclass/plasma-lrc

**Requested key**

Client key. Allow requests with no Origin header: this is a native Qt desktop
widget, not a browser application. Requests are made directly by Qt's network
client without an Origin header.

**Why this needs to be a template**

This is a publicly distributed, open-source KDE Plasma desktop widget. Each
installation makes API requests directly from the user's computer; there is no
central backend or shared API key.

Each user therefore needs their own client key and rate limit. The template
will replace the previous setup instructions that asked users to create a
standalone developer application, as requested in your review of my application.

Approximate number of users: [insert your estimate, or state that it is unknown].

**Response to the suspension notice**

Hi! Thanks for clarifying, and sorry I missed the notice — I didn't receive an
email notification. I'll submit Plasma LRC as an app template and update the
setup instructions so users obtain their own application and keys through the
approved template. I'll also remove the instructions asking users to create a
standalone application. Could you please review the suspension once these
changes are in place? Thanks!
