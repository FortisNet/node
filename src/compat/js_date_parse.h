#pragma once

#include <stddef.h>
#include <stdint.h>

// parity-only, see compat-decisions.md#date-parse
// `new Date(string).getTime()` as V8 12.4.254.21 (Node.js 22.23.4)
// computes it: a port of
// src/date/dateparser.h, dateparser-inl.h and dateparser.cc (ES5 ISO
// date-time strings, then the legacy Safari-compatible parser), MakeDay /
// MakeTime / MakeDate from src/date/date.cc and ParseDateTimeString from
// src/builtins/builtins-date.cc, at v8 tag 12.4.254.21.
// Local time is UTC. Input is UTF-16
// code units; as in V8 a NUL code unit ends the input. Guest-safe.
namespace fortis::compat {

// False when V8 answers NaN.
bool JsDateParse(const uint16_t* text, size_t length, double* epoch_ms);

}  // namespace fortis::compat
