// config.example.h: your own keys and settings for the E!nkster builds.
// Copy this file to config.h (same folder) and fill it in. config.h is listed in .gitignore: it never goes into git.
// prepare.py (the 154) and 397/prepare397.py (the 397) copy config.h into the sketch as secrets.h.
// No config.h? Build the editions with no keys: EK_EDITION=invisibleocean (154) or EK_NO_SECRETS=1 (397);
// Wi-Fi is then set up on the device, from a phone, and the tiles that need a relay stay quiet.
// The text values are C strings: keep the quotes.

// Wi-Fi: one or more networks, separated by |  (the one that worked last is tried first).
#define EK_WIFI_SSID  "network-one|network-two"
#define EK_WIFI_PASS  "password-one|password-two"

// Your relay: the small server your board talks through for the Ask, Der Die Das, Oracle and Jar tiles
// and the spoken weather question.
// It holds your own AI key; the board only ever holds the relay's address and this shared key.
// Bring your own: the relay of InvisibleChaos is private and not part of this repository.
#define EK_BRAIN_URL  "https://your-relay.example"
#define EK_RELAY_KEY  "a-long-random-string-shared-with-your-relay"

// Optional: the Kaffeesatz channel code (the daily edito tile). Leave empty to hide the tile's content.
#define EK_KS_CHANNEL ""

// Optional: your business card for the Catalog, "name|email|phone". Leave empty for none.
#define EK_CARD       ""
