// Audio: MP3 and WAV playback, the alarm tone, short beeps, and voice recording. MP3 streams
// from SD and decodes with the core's libhelix; WAV (16-bit PCM) plays as is; both go through
// the ES8311 and the NS4150B speaker amp. Runs in its own task on core 0, so playback continues
// while the UI (core 1) is busy refreshing the panel or loading a book. The task owns I2S and
// the codec: playback and recording take turns.
//
// The amp (AMP_EN) and I2S/MCLK are only on during a playback session; pause just mutes the DAC.
// The same task rings alarms: ring() pauses any music and plays a beep pattern until stopped.
// Recording uses the mic through the codec's ADC, I2S receive only; the amp stays off.
#pragma once
#include <stdint.h>

#include <string>
#include <vector>

namespace audio {

enum class State : uint8_t { Stopped, Playing, Paused };

struct Status {
  State state = State::Stopped;
  std::string path;
  std::string title;   // ID3 title (UTF-8), else the file name
  std::string artist;  // ID3 artist (UTF-8), may be empty
  int track = 0;       // index into the playlist
  int trackCount = 0;
  uint32_t elapsedMs = 0;
  uint32_t durationMs = 0;  // estimate from the first frame's bitrate; 0 = unknown
  uint32_t sampleRate = 0;  // from the first decoded frame; 0 until then
  int bitrateKbps = 0;
  int channels = 0;
  int volume = 0;
  uint32_t generation = 0;  // changes on every track, state or format change (not time/volume)
};

void begin();  // the calls below do nothing (or report stopped) until it has run

// Plays playlist[index], then the following entries (MP3 or WAV), then stops. startMs > 0
// starts the first track that far in (resuming a podcast); the position is estimated from the
// bitrate.
// The queue keeps playlist[index] and up to 63 entries after it.
void play(std::vector<std::string> playlist, int index, uint32_t startMs = 0);
// Pause/resume; when stopped at the end of the queue, plays the last track again.
void togglePause();
void next();
void stop();  // playback, the alarm tone and any recording (finished properly)
void changeVolume(int delta);

Status status();
bool active();  // playing or paused (the amp and I2S are on)

// Paths of playlist entries [from, from + count), clipped to the playlist.
std::vector<std::string> queue(int from, int count);

// Alarm tone: four short beeps, a pause, repeated, at the music volume but never below half.
// Music that was playing stays paused afterwards.
void ring(bool on);
bool ringing();

// One short tone at the music volume (the Level's "level now" cue; silent at volume 0).
// Ignored while anything plays, is paused, records or rings. Takes about a third of a second
// in the audio task, the amp on throughout.
void beep(uint32_t ms = 80, uint32_t hz = 2000);

// --- Recording: 16 kHz, mono, 16-bit WAV. record() stops playback first. The file is
// finished (its header's sizes filled in) by stopRecording(), or by stop(), which power-off
// calls. A file cut off by a power loss keeps sizes of 0, which the player reads as "to the
// end of the file".
constexpr uint32_t kRecordRate = 16000;
constexpr int kRecordBins = 72;  // waveform overview: one peak per second, the last 72 s

struct RecordStatus {
  bool on = false;
  bool paused = false;
  std::string path;
  uint32_t elapsedMs = 0;  // audio in the file so far
  uint32_t bytes = 0;      // file size so far, header included
  float levelDb = -96;     // loudest sample in the last second, dBFS (-96 = silence)
  int gainDb = 0;          // total mic gain: PGA plus digital
  std::vector<uint8_t> bins;  // per-second peaks, 0 (-60 dBFS or less) to 255 (full scale)
  std::string error;       // why the last recording stopped early ("" if it didn't)
  uint32_t generation = 0;  // changes when a recording starts, pauses, resumes or ends
};

// Starts recording to path (the folder must exist). Errors show in recordStatus().error.
void record(const std::string &path);
void stopRecording();
void togglePauseRecording();
// Mic gain in 6 dB steps, kept between recordings (RAM only).
void changeMicGain(int deltaDb);
RecordStatus recordStatus();
bool recording();

}  // namespace audio
