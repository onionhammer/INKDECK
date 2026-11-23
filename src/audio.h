// Audio: MP3 and WAV playback, the alarm tone, short beeps, and voice recording. MP3 streams
// from SD and decodes with the core's libhelix; WAV (16-bit PCM) plays as is; both go through
// the ES8311 and the NS4150B speaker amp. Runs in its own task on core 0, so playback continues
// while the UI (core 1) is busy refreshing the panel or loading a book. The task owns I2S and
// the codec: playback and recording take turns.
//
// The amp (AMP_EN), I2S/MCLK, the codec and its analog supply (ALDO2, shared with the mic) are
// on only while sound plays, rings or records; otherwise the codec is in power-down and the rail
// off. Pause switches them all off too and keeps the track open: play picks up where it paused.
// The same task rings alarms: ring() pauses any music and plays a beep pattern until stopped.
// Recording uses the mic through the codec's ADC, I2S receive only; the amp stays off.
// The agent uses the same two paths without files: capture and stream, below.
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
bool active();    // playing or paused: a track is open
bool playing();   // a track is playing (not paused). Any task, the input timer's included
bool outputOn();  // the amp, I2S and the codec are on (playing, ringing or a beep; not paused)
// Something the board mustn't light-sleep through: playing, ringing, recording or a beep. A
// paused player doesn't count; its output is off.
bool busy();

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
// The agent's chirps: two rising pips for a question, two falling for a session that finished
// (issue #52). Like beep(): ignored while anything plays, is paused, records, rings or listens,
// silent at volume 0. About half a second in the audio task.
void chirp(bool rising);
// Comms (issue #66): a ring, a phone's double trill (two 200 ms bursts of pips a third apart,
// about half a second), every few seconds while a board rings; and a message that came, three
// pips (high, low, high). Like chirp(), silent at volume 0 and ignored while anything records,
// rings, listens or streams; but a ring also plays over music, paused or playing: the music
// holds for the trill and plays on.
void ringChirp();
void messageChirp();

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
bool recording();  // to a file (not a capture)

// --- Voice (agent_client.h): the mic as a stream, and a reply played as it arrives. Both are mono
// 16-bit PCM through rings in PSRAM. Call these from one task only. Playback and capture still
// take turns: starting one ends the other, and stop() ends both.

// Starts the mic at kRecordRate into the capture ring (8 s deep). Returns once it is running;
// false if it couldn't start. Stops playback first. cue: a push-to-talk chirp first (three quick
// 1500 Hz pips, 0.15 s, softly at the music volume), which plays out before the mic opens: the
// speaker and the mic take turns, so the mic doesn't hear it.
bool captureStart(bool cue = false);
void captureStop();  // returns once the mic is off; what's in the ring can still be read
bool capturing();
// Takes up to max bytes from the capture ring, waiting up to timeoutMs for the first.
size_t captureRead(uint8_t *out, size_t max, uint32_t timeoutMs);
float captureLevelDb();  // loudest sample of the last second, dBFS; -96 when not capturing

// Opens the output at `rate` for a stream. Returns once it is ready; false if it couldn't. A
// stream plays 10 dB louder than a file at the same volume, through a limiter (audio.cpp).
bool streamStart(uint32_t rate);
// Queues PCM (little-endian; a sample may be split between calls). Blocks up to timeoutMs
// while the ring (5 s at 24 kHz) is full. Returns the bytes taken; 0 once the stream is off.
size_t streamWrite(const uint8_t *pcm, size_t bytes, uint32_t timeoutMs);
void streamEnd();    // no more data: what's queued plays out, then the output goes off
void streamAbort();  // output off now, the rest dropped
bool streaming();    // from streamStart() until it has played out or was stopped

}  // namespace audio
