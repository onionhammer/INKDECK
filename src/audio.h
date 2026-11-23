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
// The agent uses the same two paths without files: capture and stream, below. Comms (issue #66)
// uses a third, duplex: the speaker and the mic at once on the one I2S port (below).
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
// Comms's open mics (comms.h): the alarm when a board whose mic this board hears goes silent
// (LOST), a siren's two notes twice (about 0.8 s; comms repeats it), and one chirp for a LOUD
// sound while the volume is low, three rising pips. Like ringChirp(), over music too and ignored
// while anything records, rings, listens or streams; but at the music volume and never below
// half, so neither goes unheard at a low volume (as the alarm tone).
void lostAlarm();
void loudChirp();
// Comms's mic: the push-to-talk cue's three pips as it switches on, and two soft falling pips on
// a board that hears a mic switch off after a long session. Like chirp().
void micCue();
void micOffChirp();
// While the duplex path (below) is on, every chirp, the alarms' too, plays into it, over the
// voice; beep() is ignored.

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

// --- Duplex (comms.h): the speaker and the mic at once, TX and RX on the one I2S port, the
// ES8311's DAC and ADC both running, at 16 kHz mono. The mic goes into a queue of 10 ms blocks,
// each with what the speaker was putting into the room as it was captured (for comms's echo
// gate); the speaker plays what's queued with duplexWrite(), as a stream does (10 dB up, through
// the limiter), and silence when nothing is: the caller paces it (duplexQueuedMs()). The amp is
// on only while there's sound to play, and for 1.5 s after. Small DMA buffers keep the delay
// short (audio.cpp: 6 x 10 ms each way).
// Anything else that plays or records ends it: music, a stream (the agent speaking), a capture
// (the agent listening), the Recorder, the alarm, stop(); comms sees duplexOn() go false. Call
// these from one task only (comms's).
constexpr uint32_t kDuplexRate = 16000;
constexpr size_t kDuplexBlock = 160;  // samples: 10 ms
struct DuplexBlock {
  int16_t pcm[kDuplexBlock];
  // The loudest sample played into the room around the time this block was captured (the
  // loopback delay apart, plus 80 ms of room), in dBFS after the volume (the DAC's output: VOL
  // 100 is 0 dB); -96 while the speaker is silent or its amp off.
  float echoDb = -96;
  uint32_t seq = 0;  // blocks since duplexStart()
};
// Returns once it runs; false if it couldn't: the alarm rings, a recording or a capture runs, a
// stream plays (none of them is stopped for it), or the codec. Music, playing or paused, stops.
bool duplexStart();
void duplexStop();   // returns once it's off
bool duplexOn();
// The mic's blocks go to the queue from now (on: emptied first), or are dropped (off).
void duplexMic(bool on);
// The next block of the mic, waiting up to timeoutMs; false if none came.
bool duplexRead(DuplexBlock &b, uint32_t timeoutMs);
// Queues mono PCM for the speaker; never waits. Returns the samples taken (fewer when 1 s is
// queued already).
size_t duplexWrite(const int16_t *pcm, size_t samples);
uint32_t duplexQueuedMs();  // queued and not yet handed to the DMA
struct DuplexStats {
  bool on = false;
  bool ampOn = false;
  uint32_t blocks = 0;       // 10 ms blocks since it started
  uint32_t underruns = 0;    // blocks the speaker had to pad with silence mid-sound
  uint32_t micDropped = 0;   // mic blocks the queue had no room for
  float speakerDb = -96;     // the loudest sample played in the last 200 ms, after the volume
  int loopbackBlocks = 0;    // the echo delay the gate assumes, in blocks
  uint32_t stackFree = 0;    // the audio task's stack never used, bytes (any path's, since boot)
  float queuedMs = 0;        // queued behind each block taken while sound played, on average
};
DuplexStats duplexStats();
// The loopback test (comms's !commsecho): with the duplex path on and nothing else playing, a
// 1 kHz tone at levelDb (dBFS into the DAC, before the volume) for ms, after 200 ms of silence;
// the mic's blocks are compared with what was played. Waits for it (up to ms + 1.5 s).
struct LoopbackResult {
  bool ok = false;
  float noiseDb = -96;      // the mic's loudest block before the tone
  float micDb = -96;        // its median block while the tone played (from its 5th block)
  float playedDb = -96;     // what the DAC put out: levelDb plus the volume's dB
  float couplingDb = 0;     // micDb - playedDb: what comms adds to the speaker's level
  int delayBlocks = -1;     // from the tone's first block written to the first block heard
  int volume = 0;
};
LoopbackResult duplexLoopback(float levelDb, uint32_t ms);
// The echo delay, in 10 ms blocks, the echo reference assumes (the loopback test's result).
void setDuplexLoopback(int blocks);

}  // namespace audio
