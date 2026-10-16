// server.h - the addresses Sapphire calls, plain HTTP on port 80 with the
// board's key (docs/SATELLITE-PROTOCOL.md). This board has no mic and no
// speaker yet, so: /health, /led, /led/spec, /led/baseline, /led/looks, /power
// and two of its own: /screen {"text", "seconds"} shows a line across the top,
// /screen/picture?w=&h=&seconds= paints the body (RGB565, big-endian) on the glass.
#pragma once

void server_start(void);
