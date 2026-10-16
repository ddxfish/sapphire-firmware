// server.h - the addresses Sapphire calls (docs/SATELLITE-PROTOCOL.md):
//   GET  /health                     what this board is and how it is
//   POST /audio/speak                a 16 kHz mono 16 bit WAV, played as it arrives
//   GET  /audio/listen?max_seconds=  a recording, until the speaker stops
//   GET/POST/DELETE /capture?seconds=&chunk=&test=   the room in pieces to Sapphire's room door, from its own task (capture.c)
//   GET  /wakeword  POST /wakeword?enabled=true|false
//   POST /power?action=restart|shutdown
//   GET  /volume  POST /volume?level=0..100   the speaker, kept across restarts
//   POST /led                        {"color", "animation", "duration_s", ...} or {"state": "off"|"idle"}
//   GET  /led/spec                   colors, animations, the resting look, the hours
//   GET  /led/baseline  PUT /led/baseline      the resting look
//   PUT  /led/looks                  every state's look and the active hours, from Settings > Devices
//   GET  /sounds                     none are stored yet
//   POST /screen                     {"text", "seconds"} a line across the glass, or {"clear": true}
//   POST /screen/picture?w=&h=&seconds=    RGB565 big-endian body: the whole glass until a tap or the time
//   POST /screen/background?w=&h=&name=    the same body, kept as the scene behind everything; an empty body clears
//   GET/POST /screen/power?on=        the backlight; a touch turns it back on
//   GET/POST /mute?speaker=on|off     the speaker's mute (the wake word's is /wakeword)
//   PUT  /firmware                   a program over the air, on its own task
//   GET  /camera/snap?b64=true&size=800x600   a JPEG from the camera as base64 JSON; the camera is powered only for it
// Every request carries "Authorization: Bearer <the key Sapphire sends>".
#pragma once
#include <stdbool.h>

void server_start(void);
bool server_speaking(void);           // a sentence is playing now
bool server_talking(void);            // a reply is being spoken: a sentence plays, waits, or ended a moment ago
bool server_stop_speech(void);        // a stop made on the board: cut it, and answer "stopped" to the rest of that reply; true if something played
bool server_stop_by_hand(void);        // the same from a tap or a button, and Sapphire is told: her turn ends
void server_reply_over(void);         // that reply is over (her turn ended, or a question went to her): sentences play again
