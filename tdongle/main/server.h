// server.h - the addresses Sapphire calls, plain HTTP on port 80 with the
// board's key (docs/SATELLITE-PROTOCOL.md): /health, /led and its kin, /power,
// /screen {"text", "seconds"} for a line of hers, /firmware, and the card:
// /storage, /storage/{name}, /storage/format.
#pragma once

void server_start(void);
