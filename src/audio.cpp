#include "audio.h"

#include <Arduino.h>
#include <driver/i2s_std.h>
#include <errno.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <mp3dec.h>
#include <stdio.h>
#include <string.h>

#include <atomic>

#include "board_pins.h"
#include "es8311.h"
#include "text.h"

namespace audio {
namespace {

// ---------------------------------------------------------------------------------------------
// Configuration

constexpr int kDefaultVolume = 60;
constexpr int kInputBufferBytes = 16 * 1024;           // compressed data, ~1 s at 128 kbps
constexpr int kRefillBelow = 2 * MAINBUF_SIZE;
constexpr int kPcmSamples = MAX_NCHAN * MAX_NGRAN * MAX_NSAMP;  // one frame, interleaved
// 8 x 511 frames ~= 93 ms at 44.1 kHz: rides out the FATFS lock while the UI reads a chapter.
constexpr uint32_t kDmaDescriptors = 8;
constexpr uint32_t kDmaFrames = 511;

enum class Cmd : uint8_t { Play, TogglePause, Next, Stop, Volume };
struct Command {
  Cmd cmd;
  int arg;
};

// ---------------------------------------------------------------------------------------------
// Shared state (UI task <-> audio task)

QueueHandle_t commands = nullptr;
SemaphoreHandle_t lock = nullptr;  // guards `shared` and `playlist`
Status shared;
std::vector<std::string> playlist;
std::atomic<uint32_t> elapsedMs{0};
std::atomic<uint32_t> generation{0};
std::atomic<int> volume{kDefaultVolume};
std::atomic<uint32_t> underruns{0};
std::atomic<bool> countUnderruns{false};

struct Locked {
  Locked() { xSemaphoreTake(lock, portMAX_DELAY); }
  ~Locked() { xSemaphoreGive(lock); }
};

void send(Cmd cmd, int arg = 0) {
  const Command c{cmd, arg};
  xQueueSend(commands, &c, portMAX_DELAY);
}

// ---------------------------------------------------------------------------------------------
// Output: I2S (TX only, master, MCLK = 256 fs) + codec + amp

i2s_chan_handle_t tx = nullptr;
uint32_t txRate = 0;

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
  es8311::begin();
  es8311::setVolume(volume.load());
  digitalWrite(board::AMP_EN, HIGH);
  delay(20);
  es8311::mute(false);
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
// ID3v2 (title/artist + where the audio starts)

uint32_t syncsafe(const uint8_t *p) { return (p[0] << 21) | (p[1] << 14) | (p[2] << 7) | p[3]; }
uint32_t be32(const uint8_t *p) { return (uint32_t(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

std::string id3Text(const std::string &raw) {
  if (raw.empty()) return {};
  const uint8_t enc = raw[0];
  std::string out;
  if (enc == 1 || enc == 2) {  // UTF-16 with BOM / UTF-16BE
    bool bigEndian = enc == 2;
    size_t i = 1;
    if (enc == 1 && raw.size() >= 3) {
      bigEndian = static_cast<uint8_t>(raw[1]) == 0xFE;
      i = 3;
    }
    for (; i + 1 < raw.size(); i += 2) {
      const uint8_t a = raw[i], b = raw[i + 1];
      const uint32_t unit = bigEndian ? (a << 8) | b : (b << 8) | a;
      if (unit == 0) break;
      text::appendAscii(out, (unit >= 0xD800 && unit < 0xE000) ? '?' : unit);
    }
  } else {
    std::string body = raw.substr(1);
    body.resize(strnlen(body.c_str(), body.size()));
    if (enc == 3) {
      out = text::toAscii(body);
    } else {  // ISO-8859-1: bytes are code points
      for (const char c : body) text::appendAscii(out, static_cast<uint8_t>(c));
    }
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

// Reads ID3v2.3/2.4 TIT2/TPE1 and returns the offset of the first audio byte.
long parseId3(FILE *f, std::string &title, std::string &artist) {
  uint8_t h[10];
  fseek(f, 0, SEEK_SET);
  if (fread(h, 1, 10, f) != 10 || memcmp(h, "ID3", 3) != 0) return 0;
  const uint8_t version = h[3];
  const uint8_t flags = h[5];
  const uint32_t tagEnd = 10 + syncsafe(h + 6);
  const long audioStart = tagEnd + ((flags & 0x10) ? 10 : 0);
  if (version != 3 && version != 4) return audioStart;

  long pos = 10;
  if (flags & 0x40) {  // extended header
    uint8_t ext[4];
    if (fread(ext, 1, 4, f) != 4) return audioStart;
    pos += version == 4 ? syncsafe(ext) : be32(ext) + 4;
  }
  while (pos + 10 <= static_cast<long>(tagEnd) && (title.empty() || artist.empty())) {
    uint8_t fh[10];
    if (fseek(f, pos, SEEK_SET) != 0 || fread(fh, 1, 10, f) != 10 || fh[0] == 0) break;
    const uint32_t size = version == 4 ? syncsafe(fh + 4) : be32(fh + 4);
    if (size == 0 || pos + 10 + static_cast<long>(size) > static_cast<long>(tagEnd)) break;
    const bool isTitle = memcmp(fh, "TIT2", 4) == 0;
    const bool isArtist = memcmp(fh, "TPE1", 4) == 0;
    if (isTitle || isArtist) {
      std::string raw(std::min<uint32_t>(size, 512), '\0');
      if (fread(&raw[0], 1, raw.size(), f) == raw.size()) (isTitle ? title : artist) = id3Text(raw);
    }
    pos += 10 + size;
  }
  return audioStart;
}

// ---------------------------------------------------------------------------------------------
// Player (audio task only)

struct Player {
  HMP3Decoder decoder = nullptr;
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
  long audioBytes = 0;
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
  const long start = parseId3(p.file, title, artist);
  fseek(p.file, 0, SEEK_END);
  p.path = path;
  p.fileSize = ftell(p.file);
  p.filePos = start;
  p.readFailures = 0;
  p.audioBytes = p.fileSize - start;
  fseek(p.file, start, SEEK_SET);
  if (title.empty()) {
    const size_t slash = path.rfind('/');
    title = text::toAscii(path.substr(slash + 1));
    const size_t dot = title.rfind('.');
    if (dot != std::string::npos) title.resize(dot);
  }

  p.decoder = MP3InitDecoder();
  if (!p.decoder) {
    log_e("audio: MP3InitDecoder out of memory");
    closeTrack();
    return false;
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
      setState(State::Playing);
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

void handle(const Command &c) {
  State state;
  {
    Locked l;
    state = shared.state;
  }
  switch (c.cmd) {
    case Cmd::Play:
      playFrom(c.arg);
      break;
    case Cmd::TogglePause:
      if (state == State::Playing) {
        es8311::mute(true);
        setState(State::Paused);
      } else if (state == State::Paused) {
        es8311::mute(false);
        setState(State::Playing);
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
      if (tx) es8311::setVolume(c.arg);
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
    if (!tx) {
      if (!outputStart(info.samprate)) {
        closeTrack();
        setState(State::Stopped);
        return;
      }
    }
    const uint32_t duration =
        info.bitrate > 0 ? static_cast<uint32_t>(uint64_t(p.audioBytes) * 8000 / info.bitrate) : 0;
    {
      Locked l;
      shared.durationMs = duration;
    }
    log_i("audio: %d Hz, %d ch, %d kbps", info.samprate, info.nChans, info.bitrate / 1000);
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
    const bool playing = state == State::Playing && p.decoder;
    Command c;
    if (xQueueReceive(commands, &c, playing ? 0 : portMAX_DELAY) == pdTRUE) {
      handle(c);
      continue;
    }
    if (playing) step();
  }
}

}  // namespace

void begin() {
  pinMode(board::AMP_EN, OUTPUT);
  digitalWrite(board::AMP_EN, LOW);
  commands = xQueueCreate(8, sizeof(Command));
  lock = xSemaphoreCreateMutex();
  p.in = static_cast<uint8_t *>(heap_caps_malloc(kInputBufferBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  p.pcm = static_cast<int16_t *>(
      heap_caps_malloc(kPcmSamples * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!p.in || !p.pcm) {
    log_e("audio: buffer allocation failed");
    return;
  }
  xTaskCreatePinnedToCore(task, "audio", 8192, nullptr, 5, nullptr, 0);
}

void play(std::vector<std::string> list, int index) {
  {
    Locked l;
    playlist = std::move(list);
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
  {
    Locked l;
    s = shared;
  }
  s.elapsedMs = elapsedMs.load();
  s.generation = generation.load();
  s.volume = volume.load();
  return s;
}

bool active() {
  Locked l;
  return shared.state != State::Stopped;
}

}  // namespace audio
