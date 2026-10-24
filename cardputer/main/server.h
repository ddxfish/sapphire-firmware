// server.h - the board's inbound HTTP server (see server.c). Sapphire POSTs her
// spoken replies here (/audio/speak) and reads /health to see the speaker.
// Started once WiFi is up.
#pragma once

void server_start(void);   // idempotent; needs net + settings.key
