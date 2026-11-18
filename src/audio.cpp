#include "audio.h"

#include <Arduino.h>
#include <driver/i2s_std.h>
#include <errno.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/stream_buffer.h>
#include <freertos/idf_additions.h>
#include <mp3dec.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <atomic>

#include "board_pins.h"
#include "es8311.h"
#include "mp3info.h"
#include "pmic.h"
#include "text.h"

namespace audio {
namespace {

// ---------------------------------------------------------------------------------------------
// Configuration

constexpr int kDefaultVolume = 100;  // the DAC at 0 dB (es8311::setVolume()); RAM only
constexpr size_t kMaxQueue = 64;  // tracks kept by play()
constexpr int kInputBufferBytes = 16 * 1024;           // compressed data, ~1 s at 128 kbps
constexpr int kRefillBelow = 2 * MAINBUF_SIZE;
constexpr int kPcmSamples = MAX_NCHAN * MAX_NGRAN * MAX_NSAMP;  // one frame, interleaved
// 8 x 511 frames ~= 93 ms at 44.1 kHz: rides out the FATFS lock while the UI reads a chapter.
constexpr uint32_t kDmaDescriptors = 8;
constexpr uint32_t kDmaFrames = 511;

enum class Cmd : uint8_t {
  Play, TogglePause, Next, Stop, Volume, Ring, Beep, Record, RecordStop, RecordPause, MicGain,
  Capture, CaptureStop, StreamStart, StreamAbort
};
struct Command {
  Cmd cmd;
  int arg;
  int arg2 = 0;
};

// ---------------------------------------------------------------------------------------------
// Shared state (UI task <-> audio task)

QueueHandle_t commands = nullptr;
SemaphoreHandle_t lock = nullptr;  // guards `shared` and `playlist`
Status shared;
std::vector<std::string> playlist;
uint32_t playStartMs = 0;  // where the next Play command starts its first track
std::atomic<uint32_t> elapsedMs{0};
std::atomic<uint32_t> generation{0};
std::atomic<int> volume{kDefaultVolume};
std::atomic<uint32_t> underruns{0};
std::atomic<bool> countUnderruns{false};
std::atomic<bool> ringOn{false};
std::atomic<bool> outputLive{false};  // amp, I2S TX and the codec on
// Recording: the UI reads these; `recShared` (under `lock`) holds the strings and the bins.
std::string recordPath;  // where the next Record command writes (under `lock`)
RecordStatus recShared;
std::atomic<bool> recOn{false};
std::atomic<bool> recPaused{false};
std::atomic<uint32_t> recBytes{0};  // audio data bytes so far
std::atomic<int> recLevelCenti{-9600};  // levelDb x 100
std::atomic<int> micDigitalDb{12};  // on top of the PGA's 30 dB: 42 dB in all to start
std::atomic<uint32_t> recGeneration{0};
// Voice streams: PCM rings in PSRAM between the audio task and the one task that calls
// capture*() and stream*() (voice_client.cpp). One writer and one reader each.
constexpr size_t kCaptureBytes = 256 * 1024;  // 8 s of the mic: rides out a Wi-Fi join
constexpr size_t kStreamBytes = 256 * 1024;   // 5.3 s of a reply at 24 kHz
StreamBufferHandle_t captureBuf = nullptr;
StreamBufferHandle_t streamBuf = nullptr;
SemaphoreHandle_t acked = nullptr;  // given once a command sent with sendAndWait() is handled
std::atomic<bool> ackOk{false};
std::atomic<bool> captureOn{false};
std::atomic<uint32_t> captureDropped{0};  // bytes lost because nobody read the ring in time
std::atomic<bool> streamOn{false};
std::atomic<bool> streamEnded{false};

struct Locked {
  Locked() { xSemaphoreTake(lock, portMAX_DELAY); }
  ~Locked() { xSemaphoreGive(lock); }
};

void send(Cmd cmd, int arg = 0, int arg2 = 0) {
  if (!commands) return;  // begin() hasn't run (a standby wake powers off without audio)
  const Command c{cmd, arg, arg2};
  xQueueSend(commands, &c, portMAX_DELAY);
}

// Sends a command and waits until the audio task has handled it. True if it succeeded there.
bool sendAndWait(Cmd cmd, int arg = 0) {
  if (!commands) return false;
  xSemaphoreTake(acked, 0);  // an ack nobody waited for
  send(cmd, arg);
  if (xSemaphoreTake(acked, pdMS_TO_TICKS(3000)) != pdTRUE) return false;
  return ackOk.load();
}

void ack(bool ok) {
  ackOk = ok;
  xSemaphoreGive(acked);
}

// ---------------------------------------------------------------------------------------------
// Output: I2S (TX only, master, MCLK = 256 fs) + codec + amp

i2s_chan_handle_t tx = nullptr;
uint32_t txRate = 0;

// The codec and its analog supply. ALDO2 feeds the codec's AVDD and the mic (AGENTS.md, Power
// architecture), so both are on only for an output or recording session: power-down mode and the
// rail off otherwise. Its digital side stays on 3V3, so the I2C writes work either way.
void codecOn() {
  pmic::setAudioRail(true);  // waits for the rail
  es8311::begin();
}

void codecOff() {
  es8311::powerDown();
  pmic::setAudioRail(false);
}

bool IRAM_ATTR onSendQueueOverflow(i2s_chan_handle_t, i2s_event_data_t *, void *) {
  if (countUnderruns.load(std::memory_order_relaxed)) underruns.fetch_add(1, std::memory_order_relaxed);
  return false;
}

i2s_std_config_t stdConfig(uint32_t rate) {
  i2s_std_config_t cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
      .gpio_cfg =
          {
              .mclk = static_cast<gpio_num_t>(board::I2S_MCLK),
              .bclk = static_cast<gpio_num_t>(board::I2S_BCLK),
              .ws = static_cast<gpio_num_t>(board::I2S_WS),
              .dout = static_cast<gpio_num_t>(board::I2S_DOUT),
              .din = I2S_GPIO_UNUSED,
              .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
          },
  };
  cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  return cfg;
}

bool outputStart(uint32_t rate) {
  i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan.dma_desc_num = kDmaDescriptors;
  chan.dma_frame_num = kDmaFrames;
  chan.auto_clear = true;  // silence, not a repeated buffer, when we fall behind
  if (i2s_new_channel(&chan, &tx, nullptr) != ESP_OK) {
    log_e("audio: i2s_new_channel failed");
    tx = nullptr;
    return false;
  }
  const i2s_std_config_t cfg = stdConfig(rate);
  i2s_event_callbacks_t callbacks = {};
  callbacks.on_send_q_ovf = onSendQueueOverflow;
  if (i2s_channel_init_std_mode(tx, &cfg) != ESP_OK ||
      i2s_channel_register_event_callback(tx, &callbacks, nullptr) != ESP_OK ||
      i2s_channel_enable(tx) != ESP_OK) {
    log_e("audio: I2S setup failed");
    i2s_del_channel(tx);
    tx = nullptr;
    return false;
  }
  txRate = rate;
  delay(5);  // MCLK running before the codec's state machine starts
  codecOn();
  es8311::setVolume(volume.load());
  digitalWrite(board::AMP_EN, HIGH);
  delay(20);
  es8311::mute(false);
  outputLive = true;
  log_i("audio: output on at %lu Hz", (unsigned long)rate);
  return true;
}

void outputStop() {
  if (!tx) return;
  es8311::mute(true);
  delay(20);
  digitalWrite(board::AMP_EN, LOW);
  i2s_channel_disable(tx);
  i2s_del_channel(tx);
  tx = nullptr;
  txRate = 0;
  codecOff();
  outputLive = false;
  log_i("audio: output off");
}

void outputSetRate(uint32_t rate) {
  if (!tx || rate == txRate) return;
  es8311::mute(true);
  const i2s_std_config_t cfg = stdConfig(rate);
  i2s_channel_disable(tx);
  i2s_channel_reconfig_std_clock(tx, &cfg.clk_cfg);
  i2s_channel_enable(tx);
  txRate = rate;
  es8311::mute(false);
  log_i("audio: rate %lu Hz", (unsigned long)rate);
}

// ---------------------------------------------------------------------------------------------
// Player (audio task only)

struct Player {
  HMP3Decoder decoder = nullptr;
  bool wav = false;  // a WAV track: PCM straight from the file, no decoder
  uint32_t wavRate = 0;
  int wavChannels = 0;
  FILE *file = nullptr;
  std::string path;
  long fileSize = 0;
  long filePos = 0;  // offset of the next byte to read
  int readFailures = 0;
  uint8_t *in = nullptr;
  int16_t *pcm = nullptr;
  uint8_t *readPtr = nullptr;
  int bytesLeft = 0;
  bool eof = false;
  bool needRefill = false;
  long audioStart = 0;  // after the ID3 tag
  long audioBytes = 0;
  uint32_t seekMs = 0;  // start here once the first frame gives the bitrate
  uint64_t samplesPlayed = 0;
  uint32_t rate = 0;
  bool firstFrame = true;
  // stats
  uint32_t frames = 0;
  uint32_t decodeUs = 0;
  uint32_t decodeErrors = 0;
} p;

void setState(State s) {
  {
    Locked l;
    shared.state = s;
  }
  countUnderruns = s == State::Playing;
  generation++;
}

void logStats(const char *why) {
  if (!p.frames) return;
  const uint32_t audioMs = p.rate ? p.samplesPlayed * 1000 / p.rate : 0;
  log_i("audio: %s after %lu ms audio at byte %ld/%ld, %lu frames, decode %lu us/frame "
        "(%lu%% CPU), %lu decode errors, %lu DMA underruns",
        why, (unsigned long)audioMs, p.filePos, p.fileSize, (unsigned long)p.frames,
        (unsigned long)(p.decodeUs / p.frames),
        (unsigned long)(audioMs ? p.decodeUs / 10 / audioMs : 0), (unsigned long)p.decodeErrors,
        (unsigned long)underruns.load());
}

void closeTrack() {
  if (p.file) fclose(p.file);
  p.file = nullptr;
  if (p.decoder) MP3FreeDecoder(p.decoder);
  p.decoder = nullptr;
  p.wav = false;
}

bool hasExtension(const std::string &path, const char *ext) {
  const size_t n = strlen(ext);
  return path.size() > n && strcasecmp(path.c_str() + path.size() - n, ext) == 0;
}

uint32_t le32(const uint8_t *b) {
  return b[0] | b[1] << 8 | b[2] << 16 | static_cast<uint32_t>(b[3]) << 24;
}

// Finds a WAV file's format and its data chunk. False unless it's 16-bit PCM, 1 or 2
// channels, 8-48 kHz. dataBytes is what the header says (0 for a recording cut off early).
bool parseWav(FILE *f, uint32_t &rate, int &channels, long &dataStart, long &dataBytes) {
  uint8_t h[16];
  if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "WAVE", 4) != 0) {
    return false;
  }
  bool haveFormat = false;
  for (int chunks = 0; chunks < 32; ++chunks) {
    if (fread(h, 1, 8, f) != 8) return false;
    const uint32_t size = le32(h + 4);
    const long body = ftell(f);
    if (memcmp(h, "fmt ", 4) == 0) {
      if (size < 16 || fread(h, 1, 16, f) != 16) return false;
      const int format = h[0] | h[1] << 8;
      channels = h[2] | h[3] << 8;
      rate = le32(h + 4);
      const int bits = h[14] | h[15] << 8;
      if ((format != 1 && format != 0xFFFE) || bits != 16 || channels < 1 || channels > 2 ||
          rate < 8000 || rate > 48000) {
        return false;
      }
      haveFormat = true;
    } else if (memcmp(h, "data", 4) == 0) {
      if (!haveFormat) return false;
      dataStart = body;
      dataBytes = size;
      return true;
    }
    if (fseek(f, body + size + (size & 1), SEEK_SET) != 0) return false;
  }
  return false;
}

bool openTrack(int index) {
  closeTrack();
  std::string path;
  int count;
  {
    Locked l;
    count = static_cast<int>(playlist.size());
    if (index < 0 || index >= count) return false;
    path = playlist[index];
  }
  p.file = fopen(path.c_str(), "rb");
  if (!p.file) {
    log_w("audio: can't open %s", path.c_str());
    return false;
  }
  std::string title, artist;
  long start = 0, wavBytes = 0;
  p.wav = hasExtension(path, ".wav");
  if (p.wav) {
    if (!parseWav(p.file, p.wavRate, p.wavChannels, start, wavBytes)) {
      log_w("audio: %s isn't a 16-bit PCM WAV", path.c_str());
      closeTrack();
      return false;
    }
  } else {
    start = mp3info::readTags(p.file, title, artist);
  }
  fseek(p.file, 0, SEEK_END);
  p.path = path;
  p.fileSize = ftell(p.file);
  p.filePos = start;
  p.audioStart = start;
  p.seekMs = 0;
  p.readFailures = 0;
  p.audioBytes = p.fileSize - start;
  if (p.wav && wavBytes > 0 && wavBytes < p.audioBytes) p.audioBytes = wavBytes;
  fseek(p.file, start, SEEK_SET);
  if (title.empty()) {
    const size_t slash = path.rfind('/');
    title = path.substr(slash + 1);
    const size_t dot = title.rfind('.');
    if (dot != std::string::npos) title.resize(dot);
  }

  if (!p.wav) {
    p.decoder = MP3InitDecoder();
    if (!p.decoder) {
      log_e("audio: MP3InitDecoder out of memory");
      closeTrack();
      return false;
    }
  }
  p.readPtr = p.in;
  p.bytesLeft = 0;
  p.eof = false;
  p.needRefill = false;
  p.samplesPlayed = 0;
  p.firstFrame = true;
  p.frames = p.decodeUs = p.decodeErrors = 0;
  underruns = 0;
  elapsedMs = 0;
  {
    Locked l;
    shared.path = path;
    shared.title = title;
    shared.artist = artist;
    shared.track = index;
    shared.trackCount = count;
    shared.durationMs = 0;
    shared.sampleRate = 0;
    shared.bitrateKbps = 0;
    shared.channels = 0;
  }
  generation++;
  log_i("audio: track %d/%d %s (\"%s\" / \"%s\")", index + 1, count, path.c_str(), title.c_str(),
        artist.c_str());
  return true;
}

// Opens the next playable track at or after `index`; stops at the end of the playlist.
void playFrom(int index) {
  int count;
  {
    Locked l;
    count = static_cast<int>(playlist.size());
  }
  for (; index < count; ++index) {
    if (openTrack(index)) {
      setState(State::Playing);  // a skip while paused plays: step() switches the output on
      return;
    }
  }
  closeTrack();
  outputStop();
  setState(State::Stopped);
}

void advance() {
  int current;
  {
    Locked l;
    current = shared.track;
  }
  playFrom(current + 1);
}

void trackFinished() {
  logStats("track done");
  advance();
}

// ---------------------------------------------------------------------------------------------
// Alarm tone (audio task only)

constexpr uint32_t kRingRate = 16000;  // when the output isn't already on for music
constexpr int kRingMinVolume = 50;
constexpr float kRingHz = 2000;  // small speakers are loudest around 2 kHz
// One cycle: four 120 ms beeps 80 ms apart, then 600 ms of quiet.
constexpr uint32_t kBeepMs = 120, kGapMs = 80, kBeeps = 4, kQuietMs = 600;
constexpr uint32_t kCycleMs = kBeeps * (kBeepMs + kGapMs) + kQuietMs;
constexpr uint32_t kRampMs = 5;  // soft edges, so the beeps don't click

struct Ring {
  bool on = false;
  uint64_t sample = 0;
} ring;

void ringStart() {
  if (ring.on) return;
  State state;
  {
    Locked l;
    state = shared.state;
  }
  if (state == State::Playing) setState(State::Paused);  // rings at the music's rate
  if (!tx && !outputStart(kRingRate)) return;
  es8311::setVolume(std::max(volume.load(), kRingMinVolume));
  es8311::mute(false);
  ring.on = true;
  ring.sample = 0;
  ringOn = true;
  log_i("audio: ringing");
}

void ringStop() {
  if (!ring.on) return;
  ring.on = false;
  ringOn = false;
  outputStop();  // music it interrupted stays paused, with the output off like any pause
  log_i("audio: ring stopped");
}

// Writes about 16 ms of the pattern (blocks while the DMA buffers are full).
void ringStep() {
  constexpr int kFrames = 256;
  const uint32_t rate = txRate ? txRate : kRingRate;
  const uint32_t cycle = kCycleMs * rate / 1000;
  const uint32_t beep = kBeepMs * rate / 1000, period = (kBeepMs + kGapMs) * rate / 1000;
  const uint32_t ramp = kRampMs * rate / 1000;
  for (int i = 0; i < kFrames; ++i, ++ring.sample) {
    const uint32_t inCycle = ring.sample % cycle;
    const uint32_t inBeep = inCycle % period;
    float amp = 0;
    if (inCycle < kBeeps * period && inBeep < beep) {
      amp = 0.6f;
      if (inBeep < ramp) amp *= static_cast<float>(inBeep) / ramp;
      if (beep - inBeep < ramp) amp *= static_cast<float>(beep - inBeep) / ramp;
    }
    const float phase = 2 * static_cast<float>(M_PI) * kRingHz * (ring.sample % rate) / rate;
    const int16_t v = static_cast<int16_t>(amp * 32767 * sinf(phase));
    p.pcm[2 * i] = p.pcm[2 * i + 1] = v;
  }
  size_t written = 0;
  i2s_channel_write(tx, p.pcm, kFrames * 2 * sizeof(int16_t), &written, portMAX_DELAY);
}

// ---------------------------------------------------------------------------------------------
// Tones (audio task only): a run of tones and silences, each tone with soft edges and gliding
// from hz to hzEnd, then enough silence to push them through the DMA buffers before the output
// goes off again. Returns once they have played. At the music volume: at 0, nothing plays.

struct Tone {
  uint16_t ms;
  uint16_t hz;     // 0: silence
  uint16_t hzEnd;  // the glide's end; hz for a steady tone
};

void tonesNow(const Tone *tones, int count, float level, uint32_t rampMs) {
  constexpr uint32_t kRate = 16000;
  constexpr float kTwoPi = 2 * static_cast<float>(M_PI);
  if (volume.load() == 0) return;
  if (!outputStart(kRate)) return;
  const uint32_t ramp = std::max<uint32_t>(1, rampMs * kRate / 1000);
  constexpr int kFrames = 256;
  int n = 0;
  auto put = [&n](int16_t v) {
    p.pcm[2 * n] = p.pcm[2 * n + 1] = v;
    if (++n == kFrames) {
      size_t written = 0;
      i2s_channel_write(tx, p.pcm, n * 2 * sizeof(int16_t), &written, portMAX_DELAY);
      n = 0;
    }
  };
  for (int t = 0; t < count; ++t) {
    const Tone &tone = tones[t];
    const uint32_t total = tone.ms * kRate / 1000;
    float phase = 0;  // accumulated, so a glide stays continuous
    for (uint32_t done = 0; done < total; ++done) {
      if (tone.hz == 0) {
        put(0);
        continue;
      }
      float amp = level;
      if (done < ramp) amp *= static_cast<float>(done) / ramp;
      if (total - done < ramp) amp *= static_cast<float>(total - done) / ramp;
      const float hzNow = tone.hz + (static_cast<float>(tone.hzEnd) - tone.hz) * done / total;
      phase += kTwoPi * hzNow / kRate;
      if (phase > kTwoPi) phase -= kTwoPi;
      put(static_cast<int16_t>(amp * 32767 * sinf(phase)));
    }
  }
  for (uint32_t i = 0; i < kDmaDescriptors * kDmaFrames; ++i) put(0);  // flushes them out
  if (n > 0) {
    size_t written = 0;
    i2s_channel_write(tx, p.pcm, n * 2 * sizeof(int16_t), &written, portMAX_DELAY);
  }
  outputStop();
}

// audio::beep(): one tone.
void beepNow(uint32_t ms, uint32_t hz, uint32_t hzEnd) {
  const Tone tone{static_cast<uint16_t>(ms), static_cast<uint16_t>(hz),
                  static_cast<uint16_t>(hzEnd)};
  tonesNow(&tone, 1, 0.6f, kRampMs);
}

// The voice cue (captureStart(true)), "talk now": a push-to-talk chirp after Nextel's Direct
// Connect talk-permit tone, three 1800 Hz pips (30, 30 and 50 ms, 20 ms apart). Quieter than
// the other beeps: 1800 Hz comes through the small speaker far louder than the old 440-880 Hz
// glide did at 0.6.
constexpr Tone kCue[] = {{30, 1800, 1800}, {20, 0, 0}, {30, 1800, 1800}, {20, 0, 0},
                         {50, 1800, 1800}};
constexpr float kCueLevel = 0.3f;
constexpr uint32_t kCueRampMs = 3;  // crisp pips, still without clicks

// ---------------------------------------------------------------------------------------------
// WAV playback (audio task only): 16-bit PCM read straight into the output.

void stepWav() {
  const uint32_t bytesPerSecond = p.wavRate * p.wavChannels * 2;
  if (p.firstFrame) {
    p.firstFrame = false;
    p.rate = p.wavRate;
    if (txRate != p.wavRate) outputSetRate(p.wavRate);
    {
      Locked l;
      shared.durationMs = static_cast<uint32_t>(uint64_t(p.audioBytes) * 1000 / bytesPerSecond);
      shared.sampleRate = p.wavRate;
      shared.bitrateKbps = static_cast<int>(bytesPerSecond * 8 / 1000);
      shared.channels = p.wavChannels;
    }
    generation++;
    if (p.seekMs) {
      long offset = static_cast<long>(uint64_t(p.seekMs) * bytesPerSecond / 1000);
      offset -= offset % (2 * p.wavChannels);
      if (offset < p.audioBytes && fseek(p.file, p.audioStart + offset, SEEK_SET) == 0) {
        p.filePos = p.audioStart + offset;
        p.samplesPlayed = offset / (2 * p.wavChannels);
        elapsedMs = p.seekMs;
      }
      p.seekMs = 0;
    }
    log_i("audio: WAV %lu Hz, %d ch", (unsigned long)p.wavRate, p.wavChannels);
  }
  // The track's first step, or the first after a pause (which switched the output off).
  if (!tx && !outputStart(p.wavRate)) {
    closeTrack();
    setState(State::Stopped);
    return;
  }
  constexpr int kFramesPerStep = 576;  // p.pcm holds 1152 stereo frames
  const int frameBytes = 2 * p.wavChannels;
  long want = std::min<long>(kFramesPerStep * frameBytes, p.audioStart + p.audioBytes - p.filePos);
  want -= want % frameBytes;
  const size_t n = want > 0 ? fread(p.in, 1, want, p.file) : 0;
  p.filePos += n;
  const int frames = static_cast<int>(n) / frameBytes;
  if (frames == 0) {
    trackFinished();
    return;
  }
  const auto *s = reinterpret_cast<const int16_t *>(p.in);
  for (int i = 0; i < frames; ++i) {  // the DAC plays the left slot: mono into both
    const int16_t v = p.wavChannels == 1 ? s[i] : (int32_t(s[2 * i]) + s[2 * i + 1]) / 2;
    p.pcm[2 * i] = p.pcm[2 * i + 1] = v;
  }
  size_t written = 0;
  i2s_channel_write(tx, p.pcm, frames * 2 * sizeof(int16_t), &written, portMAX_DELAY);
  ++p.frames;
  p.samplesPlayed += frames;
  elapsedMs = static_cast<uint32_t>(p.samplesPlayed * 1000 / p.rate);
}

// ---------------------------------------------------------------------------------------------
// Stream playback (audio task only): mono PCM from streamBuf as it arrives over the network.
// Playing waits until 200 ms is buffered (again after running dry), so a Wi-Fi stall is a
// pause between words rather than a stutter.

struct Stream {
  bool on = false;
  bool primed = false;  // enough is buffered to play
  uint32_t rate = 0;
  int carry = -1;  // the first byte of a sample the ring handed over split in two
} strm;

void streamStop() {
  if (!strm.on) return;
  strm.on = false;
  streamOn = false;
  outputStop();
  log_i("audio: stream off");
}

bool streamBegin(uint32_t rate) {
  streamStop();
  closeTrack();
  outputStop();  // a paused track leaves the output on and muted
  setState(State::Stopped);
  if (!streamBuf) {
    streamBuf = xStreamBufferCreateWithCaps(kStreamBytes, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  if (!streamBuf) return false;
  xStreamBufferReset(streamBuf);
  streamEnded = false;
  if (!outputStart(rate)) return false;
  strm = Stream{};
  strm.on = true;
  strm.rate = rate;
  streamOn = true;
  return true;
}

// Everything was played: push it through the DMA buffers with silence, then the output goes off.
void streamFinish() {
  constexpr int kFrames = 256;
  memset(p.pcm, 0, kFrames * 2 * sizeof(int16_t));
  for (uint32_t done = 0; done < kDmaDescriptors * kDmaFrames; done += kFrames) {
    size_t written = 0;
    i2s_channel_write(tx, p.pcm, kFrames * 2 * sizeof(int16_t), &written, portMAX_DELAY);
  }
  streamStop();
}

void streamStep() {
  const bool ended = streamEnded.load();
  if (!strm.primed) {
    const size_t have = xStreamBufferBytesAvailable(streamBuf);
    if (have >= strm.rate * 2 / 5 || (ended && have > 0)) {
      strm.primed = true;
    } else if (ended) {
      streamFinish();
      return;
    } else {
      vTaskDelay(pdMS_TO_TICKS(10));
      return;
    }
  }
  constexpr int kFrames = 512;
  uint8_t *in = p.in;
  size_t n = 0;
  if (strm.carry >= 0) {
    in[n++] = static_cast<uint8_t>(strm.carry);
    strm.carry = -1;
  }
  n += xStreamBufferReceive(streamBuf, in + n, kFrames * 2 - n, pdMS_TO_TICKS(20));
  if (n % 2) strm.carry = in[--n];
  if (n == 0) {
    if (strm.carry < 0) strm.primed = false;  // ran dry: buffer up again (or finish, if ended)
    return;
  }
  const int frames = static_cast<int>(n / 2);
  for (int i = 0; i < frames; ++i) {  // the DAC plays the left slot: mono into both
    const int16_t v = static_cast<int16_t>(in[2 * i] | in[2 * i + 1] << 8);
    p.pcm[2 * i] = p.pcm[2 * i + 1] = v;
  }
  size_t written = 0;
  i2s_channel_write(tx, p.pcm, frames * 2 * sizeof(int16_t), &written, portMAX_DELAY);
}

// ---------------------------------------------------------------------------------------------
// Recording (audio task only): I2S receive, 16 kHz mono (the left slot, the ADC with REG44 =
// 0x08), into a WAV file on the card in 8 KB writes from internal RAM.

constexpr int kRecReadSamples = 512;            // 32 ms per read
constexpr size_t kRecWriteBytes = 8 * 1024;     // gathered in p.in before each fwrite
constexpr uint32_t kRecWindow = kRecordRate / 10;  // level-meter window: 100 ms
constexpr int kRecSkipReads = 3;                // ~100 ms of start-up thump dropped

struct Recorder {
  i2s_chan_handle_t rx = nullptr;
  FILE *file = nullptr;
  bool capture = false;  // into captureBuf instead of a file
  size_t fill = 0;  // bytes waiting in p.in
  uint32_t dataBytes = 0;
  int skip = 0;
  uint32_t windowSamples = 0;
  int windowPeak = 0;
  int windowPeaks[10] = {};  // the last second, for levelDb
  int windowIndex = 0;
  uint32_t secondSamples = 0;
  int secondPeak = 0;
} rec;

// The 44-byte header of a 16 kHz mono 16-bit WAV with `data` bytes of audio.
void wavHeader(uint8_t *h, uint32_t data) {
  auto put32 = [](uint8_t *b, uint32_t v) {
    b[0] = v; b[1] = v >> 8; b[2] = v >> 16; b[3] = v >> 24;
  };
  memcpy(h, "RIFF", 4);
  put32(h + 4, data ? data + 36 : 0);
  memcpy(h + 8, "WAVEfmt ", 8);
  put32(h + 16, 16);
  h[20] = 1; h[21] = 0;  // PCM
  h[22] = 1; h[23] = 0;  // mono
  put32(h + 24, kRecordRate);
  put32(h + 28, kRecordRate * 2);
  h[32] = 2; h[33] = 0;   // block align
  h[34] = 16; h[35] = 0;  // bits
  memcpy(h + 36, "data", 4);
  put32(h + 40, data);
}

float peakDb(int peak) { return peak <= 0 ? -96.0f : 20 * log10f(peak / 32768.0f); }

void recordFail(const char *why);

void recordFlush() {
  if (!rec.file || rec.fill == 0) return;
  if (fwrite(p.in, 1, rec.fill, rec.file) != rec.fill) {
    recordFail("CAN'T WRITE TO THE CARD");
    return;
  }
  rec.fill = 0;
}

void recordFinish() {
  if (!recOn) return;
  recordFlush();
  if (rec.file) {
    uint8_t h[44];
    wavHeader(h, rec.dataBytes);
    if (fseek(rec.file, 0, SEEK_SET) == 0) fwrite(h, 1, sizeof h, rec.file);
    fclose(rec.file);
    rec.file = nullptr;
  }
  if (rec.rx) {
    i2s_channel_disable(rec.rx);
    i2s_del_channel(rec.rx);
    rec.rx = nullptr;
    codecOff();
  }
  recOn = false;
  captureOn = false;
  recPaused = false;
  recLevelCenti = -9600;
  recGeneration++;
  log_i("audio: %s done, %lu bytes, %lu dropped", rec.capture ? "capture" : "recording",
        (unsigned long)rec.dataBytes, (unsigned long)captureDropped.load());
}

void recordFail(const char *why) {
  log_e("audio: recording stopped: %s", why);
  {
    Locked l;
    recShared.error = why;
  }
  if (rec.file && rec.fill) rec.fill = 0;  // what's buffered can't be written anyway
  recordFinish();
}

// Whatever was using the output (playback, a paused track, the alarm tone, a voice reply) gives
// the port up.
void releaseOutput() {
  if (ring.on) ringStop();
  streamStop();
  closeTrack();
  outputStop();
  setState(State::Stopped);
}

// Opens the mic: I2S receive at 16 kHz, then the codec's ADC, after releaseOutput().
bool micOpen() {
  releaseOutput();

  i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan.dma_desc_num = kDmaDescriptors;  // 8 x 511 frames = 255 ms: rides out an SD write
  chan.dma_frame_num = kDmaFrames;
  i2s_std_config_t cfg = stdConfig(kRecordRate);
  cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
  // On the S3 that macro leaves both slots in the mask, so mono would still read the right
  // slot too, at twice the rate (seen: a 19 s timer after 11 s). The mic is the left slot.
  cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
  cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
  cfg.gpio_cfg.din = static_cast<gpio_num_t>(board::I2S_DIN);
  if (i2s_new_channel(&chan, nullptr, &rec.rx) != ESP_OK ||
      i2s_channel_init_std_mode(rec.rx, &cfg) != ESP_OK || i2s_channel_enable(rec.rx) != ESP_OK) {
    if (rec.rx) i2s_del_channel(rec.rx);
    rec.rx = nullptr;
    return false;
  }
  delay(5);  // MCLK running before the codec's state machine starts
  codecOn();
  es8311::micBegin(micDigitalDb.load());
  rec.skip = kRecSkipReads;
  recBytes = 0;
  recOn = true;
  recPaused = false;
  recGeneration++;
  return true;
}

void recordStart() {
  if (recOn) return;
  std::string path;
  {
    Locked l;
    path = recordPath;
    recShared.error.clear();
    recShared.bins.clear();
    recShared.path = path;
  }
  rec = Recorder{};
  rec.file = fopen(path.c_str(), "wb");
  if (!rec.file) {
    Locked l;
    recShared.error = "CAN'T CREATE THE FILE";
    recGeneration++;
    return;
  }
  uint8_t h[44];
  wavHeader(h, 0);
  memcpy(p.in, h, sizeof h);  // written from RAM with the first audio (AGENTS.md, Gotchas)
  rec.fill = sizeof h;
  if (!micOpen()) {
    fclose(rec.file);
    rec.file = nullptr;
    remove(path.c_str());
    Locked l;
    recShared.error = "I2S WON'T START";
    recGeneration++;
    return;
  }
  log_i("audio: recording to %s, mic gain %d dB", path.c_str(),
        es8311::kMicPgaDb + micDigitalDb.load());
}

// The mic into captureBuf (a voice utterance) instead of a file.
// cue: the "listening" chirp first. The speaker and the mic take turns on the one I2S port, so it
// plays out before the mic opens, and the mic doesn't hear it.
bool captureBegin(bool cue) {
  if (recOn) recordFinish();
  if (!captureBuf) {
    captureBuf = xStreamBufferCreateWithCaps(kCaptureBytes, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  if (!captureBuf) return false;
  if (cue) {
    releaseOutput();
    tonesNow(kCue, sizeof kCue / sizeof kCue[0], kCueLevel, kCueRampMs);
  }
  xStreamBufferReset(captureBuf);
  captureDropped = 0;
  rec = Recorder{};
  rec.capture = true;
  if (!micOpen()) return false;
  captureOn = true;
  log_i("audio: capturing, mic gain %d dB", es8311::kMicPgaDb + micDigitalDb.load());
  return true;
}

void recordStep() {
  size_t got = 0;
  if (i2s_channel_read(rec.rx, p.pcm, kRecReadSamples * sizeof(int16_t), &got, 200) != ESP_OK) {
    return;
  }
  const int n = static_cast<int>(got / sizeof(int16_t));
  if (rec.skip > 0) {
    --rec.skip;
    return;
  }
  for (int i = 0; i < n; ++i) {
    const int a = std::abs(static_cast<int>(p.pcm[i]));
    rec.windowPeak = std::max(rec.windowPeak, a);
    rec.secondPeak = std::max(rec.secondPeak, a);
    if (++rec.windowSamples == kRecWindow) {
      rec.windowPeaks[rec.windowIndex] = rec.windowPeak;
      rec.windowIndex = (rec.windowIndex + 1) % 10;
      rec.windowSamples = 0;
      rec.windowPeak = 0;
      const int loudest = *std::max_element(rec.windowPeaks, rec.windowPeaks + 10);
      recLevelCenti = static_cast<int>(peakDb(loudest) * 100);
    }
    if (++rec.secondSamples == kRecordRate) {
      // -60..0 dBFS -> 0..255
      const float db = peakDb(rec.secondPeak);
      const int bin = static_cast<int>(std::clamp((db + 60) / 60, 0.0f, 1.0f) * 255);
      if (!recPaused) {
        Locked l;
        recShared.bins.push_back(static_cast<uint8_t>(bin));
        if (recShared.bins.size() > static_cast<size_t>(kRecordBins)) {
          recShared.bins.erase(recShared.bins.begin());
        }
      }
      rec.secondSamples = 0;
      rec.secondPeak = 0;
    }
  }
  if (recPaused) return;
  if (rec.capture) {
    const size_t sent = xStreamBufferSend(captureBuf, p.pcm, got, 0);
    if (sent != got) captureDropped += got - sent;
    rec.dataBytes += sent;
    recBytes = rec.dataBytes;
    return;
  }
  memcpy(p.in + rec.fill, p.pcm, got);
  rec.fill += got;
  rec.dataBytes += got;
  recBytes = rec.dataBytes;
  if (rec.fill >= kRecWriteBytes) recordFlush();
}

void handle(const Command &c) {
  if (ring.on && c.cmd != Cmd::Ring && c.cmd != Cmd::Volume && c.cmd != Cmd::Beep) ringStop();
  // Playback (or the alarm) needs the I2S port: a recording in progress is finished first.
  if (recOn && (c.cmd == Cmd::Play || c.cmd == Cmd::TogglePause || c.cmd == Cmd::Stop ||
                c.cmd == Cmd::StreamStart || (c.cmd == Cmd::Ring && c.arg))) {
    recordFinish();
  }
  // So does anything else that plays, and stop() (power-off) ends a voice reply too.
  if (strm.on && (c.cmd == Cmd::Play || c.cmd == Cmd::TogglePause || c.cmd == Cmd::Stop ||
                  c.cmd == Cmd::StreamAbort || (c.cmd == Cmd::Ring && c.arg))) {
    streamStop();
  }
  State state;
  {
    Locked l;
    state = shared.state;
  }
  switch (c.cmd) {
    case Cmd::Beep:
      if (!recOn && !ring.on && !strm.on && state == State::Stopped) beepNow(c.arg, c.arg2, c.arg2);
      break;
    case Cmd::Capture:
      ack(captureBegin(c.arg != 0));
      break;
    case Cmd::CaptureStop:
      if (rec.capture) recordFinish();
      ack(true);
      break;
    case Cmd::StreamStart:
      ack(streamBegin(static_cast<uint32_t>(c.arg)));
      break;
    case Cmd::StreamAbort:
      break;  // stopped above
    case Cmd::Record:
      recordStart();
      break;
    case Cmd::RecordStop:
      recordFinish();
      break;
    case Cmd::RecordPause:
      if (recOn) {
        recPaused = !recPaused;
        if (recPaused) recordFlush();
        recGeneration++;
      }
      break;
    case Cmd::MicGain:
      if (recOn) es8311::setMicGain(c.arg);
      break;
    case Cmd::Play: {
      uint32_t startMs;
      {
        Locked l;
        startMs = playStartMs;
        playStartMs = 0;
      }
      playFrom(c.arg);
      p.seekMs = startMs;
      break;
    }
      break;
    case Cmd::TogglePause:
      if (state == State::Playing) {
        // Amp, I2S, codec and its rail off: a paused player draws nothing, so the board can
        // nap under it. The track stays open where it stopped (less what was queued in DMA).
        outputStop();
        setState(State::Paused);
      } else if (state == State::Paused) {
        setState(State::Playing);  // step() switches the output back on
      } else {
        int track;
        {
          Locked l;
          track = shared.track;
        }
        playFrom(track);  // stopped (end of the queue): play the last track again
      }
      break;
    case Cmd::Next:
      if (state != State::Stopped) {
        logStats("skipped");
        advance();
      }
      break;
    case Cmd::Stop:
      logStats("stopped");
      closeTrack();
      outputStop();
      setState(State::Stopped);
      break;
    case Cmd::Volume:
      if (tx && !ring.on) es8311::setVolume(c.arg);
      break;
    case Cmd::Ring:
      if (c.arg) {
        ringStart();
      } else {
        ringStop();
      }
      break;
  }
}

// Tops up the input buffer. A short read before the end of the file is an SD error; FatFs
// fails every later read on that handle, so reopen the file and carry on from the same spot.
void refill() {
  memmove(p.in, p.readPtr, p.bytesLeft);
  p.readPtr = p.in;
  p.needRefill = false;
  const size_t want = kInputBufferBytes - p.bytesLeft;
  const size_t n = p.file ? fread(p.in + p.bytesLeft, 1, want, p.file) : 0;
  p.bytesLeft += n;
  p.filePos += n;
  if (n == want) {
    p.readFailures = 0;
    return;
  }
  if (p.filePos >= p.fileSize) {
    p.eof = true;
    return;
  }
  log_w("audio: short read at byte %ld/%ld (errno %d), reopening", p.filePos, p.fileSize, errno);
  if (p.file) fclose(p.file);
  vTaskDelay(pdMS_TO_TICKS(20));
  p.file = fopen(p.path.c_str(), "rb");
  if (p.file && fseek(p.file, p.filePos, SEEK_SET) != 0) {
    fclose(p.file);
    p.file = nullptr;
  }
  if (!p.file && ++p.readFailures > 5) {
    log_e("audio: giving up on %s", p.path.c_str());
    p.eof = true;
  }
}

// Decodes and plays one frame. Called only while playing.
void step() {
  if (p.wav) {
    stepWav();
    return;
  }
  if (!p.eof && (p.needRefill || p.bytesLeft < kRefillBelow)) refill();
  if (p.bytesLeft <= 0) {
    if (p.eof) trackFinished();  // otherwise a failed read: retry on the next step
    return;
  }

  const int offset = MP3FindSyncWord(p.readPtr, p.bytesLeft);
  if (offset < 0) {
    if (p.eof) {
      trackFinished();
    } else {
      p.bytesLeft = 0;  // no sync word anywhere: discard and read more
    }
    return;
  }
  p.readPtr += offset;
  p.bytesLeft -= offset;

  unsigned char *ptr = p.readPtr;
  int left = p.bytesLeft;
  const uint32_t t0 = micros();
  const int err = MP3Decode(p.decoder, &ptr, &left, p.pcm, 0);
  p.decodeUs += micros() - t0;
  if (err == ERR_MP3_INDATA_UNDERFLOW) {
    if (p.eof) {
      trackFinished();
    } else {
      p.needRefill = true;
    }
    return;
  }
  p.readPtr = ptr;
  p.bytesLeft = left;
  if (err == ERR_MP3_MAINDATA_UNDERFLOW) return;  // bit reservoir not filled yet; keep going
  if (err) {
    ++p.decodeErrors;
    if (p.bytesLeft > 0) {  // skip a byte and resync
      ++p.readPtr;
      --p.bytesLeft;
    }
    return;
  }

  MP3FrameInfo info;
  MP3GetLastFrameInfo(p.decoder, &info);
  if (info.nChans < 1 || info.samprate <= 0 || info.bitsPerSample != 16) return;
  if (p.firstFrame) {
    p.firstFrame = false;
    p.rate = info.samprate;
    const uint32_t duration =
        info.bitrate > 0 ? static_cast<uint32_t>(uint64_t(p.audioBytes) * 8000 / info.bitrate) : 0;
    {
      Locked l;
      shared.durationMs = duration;
      shared.sampleRate = info.samprate;
      shared.bitrateKbps = info.bitrate / 1000;
      shared.channels = info.nChans;
    }
    generation++;  // the screen shows the format
    if (p.seekMs && info.bitrate > 0) {
      // Resume: jump to the byte the bitrate puts at seekMs (exact for constant bitrate, close
      // for variable) and let the decoder find the next frame there. This frame isn't played.
      const long offset = p.audioStart + static_cast<long>(uint64_t(p.seekMs) * info.bitrate / 8000);
      if (offset < p.fileSize - kInputBufferBytes && fseek(p.file, offset, SEEK_SET) == 0) {
        p.filePos = offset;
        p.readPtr = p.in;
        p.bytesLeft = 0;
        p.eof = false;
        p.samplesPlayed = uint64_t(p.seekMs) * info.samprate / 1000;
        elapsedMs = p.seekMs;
        log_i("audio: resuming at %lu s (byte %ld)", (unsigned long)(p.seekMs / 1000), offset);
      }
      p.seekMs = 0;
      return;
    }
    log_i("audio: %d Hz, %d ch, %d kbps", info.samprate, info.nChans, info.bitrate / 1000);
  }
  // The track's first frame, or the first after a pause (which switched the output off).
  if (!tx && !outputStart(info.samprate)) {
    closeTrack();
    setState(State::Stopped);
    return;
  }
  if (static_cast<uint32_t>(info.samprate) != txRate) outputSetRate(info.samprate);
  p.rate = info.samprate;

  // The ES8311 DAC only plays the left slot: downmix stereo, duplicate mono.
  const int frames = info.outputSamps / info.nChans;
  if (info.nChans == 1) {
    for (int i = frames - 1; i >= 0; --i) p.pcm[2 * i] = p.pcm[2 * i + 1] = p.pcm[i];
  } else {
    for (int i = 0; i < frames; ++i) {
      const int16_t mono = (int32_t(p.pcm[2 * i]) + p.pcm[2 * i + 1]) / 2;
      p.pcm[2 * i] = p.pcm[2 * i + 1] = mono;
    }
  }
  size_t written = 0;
  i2s_channel_write(tx, p.pcm, frames * 2 * sizeof(int16_t), &written, portMAX_DELAY);
  ++p.frames;
  p.samplesPlayed += frames;
  elapsedMs = static_cast<uint32_t>(p.samplesPlayed * 1000 / p.rate);
}

void task(void *) {
  for (;;) {
    State state;
    {
      Locked l;
      state = shared.state;
    }
    const bool playing = state == State::Playing && (p.decoder || p.wav);
    const bool busy = playing || ring.on || recOn || strm.on;
    Command c;
    if (xQueueReceive(commands, &c, busy ? 0 : portMAX_DELAY) == pdTRUE) {
      handle(c);
      continue;
    }
    if (recOn) {
      recordStep();
    } else if (ring.on) {
      ringStep();
    } else if (strm.on) {
      streamStep();
    } else if (playing) {
      step();
    }
  }
}

}  // namespace

void begin() {
  pinMode(board::AMP_EN, OUTPUT);
  digitalWrite(board::AMP_EN, LOW);
  // A software reset (an update, a crash) can leave the codec running from before: power-down
  // mode and its rail off until something plays.
  codecOff();
  commands = xQueueCreate(8, sizeof(Command));
  lock = xSemaphoreCreateMutex();
  acked = xSemaphoreCreateBinary();
  p.in = static_cast<uint8_t *>(heap_caps_malloc(kInputBufferBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  p.pcm = static_cast<int16_t *>(
      heap_caps_malloc(kPcmSamples * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!p.in || !p.pcm) {
    log_e("audio: buffer allocation failed");
    return;
  }
  xTaskCreatePinnedToCore(task, "audio", 8192, nullptr, 5, nullptr, 0);
}

void play(std::vector<std::string> list, int index, uint32_t startMs) {
  // A queue of a few dozen tracks is hours of listening; a folder of hundreds would only cost
  // memory. Keep the chosen track and the ones after it.
  if (index > 0 && index < static_cast<int>(list.size())) {
    list.erase(list.begin(), list.begin() + index);
    index = 0;
  }
  if (list.size() > kMaxQueue) list.resize(kMaxQueue);
  {
    Locked l;
    playlist = std::move(list);
    playStartMs = startMs;
  }
  send(Cmd::Play, index);
}

void togglePause() { send(Cmd::TogglePause); }
void next() { send(Cmd::Next); }
void stop() { send(Cmd::Stop); }

void changeVolume(int delta) {
  const int v = constrain(volume.load() + delta, 0, 100);
  volume = v;
  send(Cmd::Volume, v);
}

Status status() {
  Status s;
  if (!lock) return s;
  {
    Locked l;
    s = shared;
  }
  s.elapsedMs = elapsedMs.load();
  s.generation = generation.load();
  s.volume = volume.load();
  return s;
}

void ring(bool on) { send(Cmd::Ring, on ? 1 : 0); }

bool ringing() { return ringOn.load(); }

void beep(uint32_t ms, uint32_t hz) { send(Cmd::Beep, static_cast<int>(ms), static_cast<int>(hz)); }

void record(const std::string &path) {
  if (!lock) return;
  {
    Locked l;
    recordPath = path;
  }
  send(Cmd::Record);
}

void stopRecording() { send(Cmd::RecordStop); }

void togglePauseRecording() { send(Cmd::RecordPause); }

void changeMicGain(int deltaDb) {
  const int db = std::clamp(micDigitalDb.load() + deltaDb, 0, es8311::kMicDigitalMaxDb);
  micDigitalDb = db;
  send(Cmd::MicGain, db);
}

RecordStatus recordStatus() {
  RecordStatus s;
  if (!lock) return s;
  {
    Locked l;
    s = recShared;
  }
  s.on = recOn.load();
  s.paused = recPaused.load();
  const uint32_t bytes = recBytes.load();
  s.elapsedMs = static_cast<uint32_t>(uint64_t(bytes) * 1000 / (kRecordRate * 2));
  s.bytes = bytes + 44;
  s.levelDb = s.on ? recLevelCenti.load() / 100.0f : -96;
  s.gainDb = es8311::kMicPgaDb + micDigitalDb.load();
  s.generation = recGeneration.load();
  return s;
}

bool recording() { return recOn.load() && !captureOn.load(); }

bool captureStart(bool cue) { return sendAndWait(Cmd::Capture, cue ? 1 : 0); }

void captureStop() { sendAndWait(Cmd::CaptureStop); }

bool capturing() { return captureOn.load(); }

size_t captureRead(uint8_t *out, size_t max, uint32_t timeoutMs) {
  if (!captureBuf) return 0;
  return xStreamBufferReceive(captureBuf, out, max, pdMS_TO_TICKS(timeoutMs));
}

float captureLevelDb() { return captureOn.load() ? recLevelCenti.load() / 100.0f : -96; }

bool streamStart(uint32_t rate) { return sendAndWait(Cmd::StreamStart, static_cast<int>(rate)); }

size_t streamWrite(const uint8_t *pcm, size_t bytes, uint32_t timeoutMs) {
  if (!streamOn.load() || !streamBuf) return 0;
  return xStreamBufferSend(streamBuf, pcm, bytes, pdMS_TO_TICKS(timeoutMs));
}

void streamEnd() { streamEnded = true; }

void streamAbort() { send(Cmd::StreamAbort); }

bool streaming() { return streamOn.load(); }

bool active() {
  if (!lock) return false;
  Locked l;
  return shared.state != State::Stopped;
}

bool playing() {
  if (!lock) return false;
  Locked l;
  return shared.state == State::Playing;
}

bool outputOn() { return outputLive.load(); }

bool busy() {
  if (!lock) return false;
  if (outputLive.load() || recOn.load() || ringOn.load()) return true;
  Locked l;
  return shared.state == State::Playing;
}

std::vector<std::string> queue(int from, int count) {
  if (!lock) return {};
  Locked l;
  std::vector<std::string> out;
  for (int i = std::max(0, from); i < from + count && i < static_cast<int>(playlist.size()); ++i) {
    out.push_back(playlist[i]);
  }
  return out;
}

}  // namespace audio
