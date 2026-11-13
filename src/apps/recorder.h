// Recorder: voice memos from the onboard mic, as 16 kHz mono WAV files in SD:/Recordings
// (audio::record()). Laid out as on the Recorder artboard: the state and a big timer, the
// waveform, the input meter, stat cells, then the memos with NEW RECORDING first.
//
// Keys, by state:
//   ready, NEW RECORDING selected   ▲▼ MEMO   ● REC
//   ready, a memo selected          ▲▼ MEMO   ● PLAY   HOLD ● DELETE (asks first)
//   recording                       ▲▼ GAIN   ● STOP   HOLD ● PAUSE / RESUME
//   playing                         ▲▼ VOL    ● STOP
// The meter under the waveform follows: the mic's level while recording (or with NEW RECORDING
// selected), the playback volume while a memo plays (or is selected).
// BOOT goes back; leaving the screen finishes a recording. Memos are named by the local time
// they started ("2026-11-13 091502.wav") and listed newest first.
#pragma once
#include <memory>
#include <string>

#include "ui/nav.h"

namespace apps {

std::unique_ptr<ui::Screen> makeRecorder();

// For Home's preview.
struct MemoSummary {
  int count = 0;
  std::string last;  // "13 NOV 09:15 · 00:42", or "" with none
};
MemoSummary memoSummary();

constexpr const char *kRecordingsDir = "/sdcard/Recordings";

}  // namespace apps
