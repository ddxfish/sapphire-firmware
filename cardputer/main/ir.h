// ir.h - the onboard IR emitter (GPIO44) as a blaster. Phase 1: NEC codes out
// over the RMT peripheral. Receive/learn needs an external IR receiver (the
// board has only an emitter), so that comes later with the M5 IR Unit.
#pragma once
#include <stdbool.h>
#include <stdint.h>

bool ir_init(void);                          // bring up the RMT channel; idempotent, false if it won't start
// One NEC frame, 38 kHz. addr <= 0xFF is standard NEC (addr, ~addr, cmd, ~cmd,
// e.g. an LG TV); addr > 0xFF is NEC-extended, the 16-bit address sent as-is
// (addr_lo, addr_hi, cmd, ~cmd, e.g. a TCL Android TV at 0x57E3).
void ir_send_nec(uint16_t addr, uint8_t cmd);
