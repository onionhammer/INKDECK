#include "mp3info.h"

#include <esp32-hal-log.h>
#include <math.h>
#include <mp3dec.h>
#include <string.h>

#include <algorithm>
#include <memory>

#include "text.h"

namespace mp3info {
namespace {

uint32_t syncsafe(const uint8_t *p) { return (p[0] << 21) | (p[1] << 14) | (p[2] << 7) | p[3]; }
uint32_t be32(const uint8_t *p) {
  return (uint32_t(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

// An ID3 text frame body (encoding byte + text) as trimmed UTF-8.
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
      text::appendUtf8(out, (unit >= 0xD800 && unit < 0xE000) ? '?' : unit);
    }
  } else {
    std::string body = raw.substr(1);
    body.resize(strnlen(body.c_str(), body.size()));
    if (enc == 3) {
      out = body;  // already UTF-8
    } else {       // ISO-8859-1: bytes are code points
      for (const char c : body) text::appendUtf8(out, static_cast<uint8_t>(c));
    }
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

struct Frame {
  uint32_t sampleRate;
  uint16_t bitrateKbps;
  uint16_t samplesPerFrame;
  uint32_t length;  // bytes, including the header
  uint8_t channels;
  bool mpeg1;
};

// Parses an MPEG-1/2/2.5 Layer III frame header.
bool parseFrame(const uint8_t *h, Frame &f) {
  if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) return false;
  const int version = (h[1] >> 3) & 3;  // 0 = 2.5, 2 = 2, 3 = 1
  const int layer = (h[1] >> 1) & 3;    // 1 = Layer III
  const int bitrateIndex = h[2] >> 4;
  const int rateIndex = (h[2] >> 2) & 3;
  if (version == 1 || layer != 1 || bitrateIndex == 0 || bitrateIndex == 15 || rateIndex == 3) {
    return false;
  }
  static const uint16_t kRatesV1[] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224,
                                      256, 320};
  static const uint16_t kRatesV2[] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128,
                                      144, 160};
  static const uint32_t kSampleRates[] = {44100, 48000, 32000};
  f.mpeg1 = version == 3;
  f.bitrateKbps = (f.mpeg1 ? kRatesV1 : kRatesV2)[bitrateIndex];
  f.sampleRate = kSampleRates[rateIndex] >> (f.mpeg1 ? 0 : version == 2 ? 1 : 2);
  f.channels = (h[3] >> 6) == 3 ? 1 : 2;
  f.samplesPerFrame = f.mpeg1 ? 1152 : 576;
  f.length = (f.mpeg1 ? 144 : 72) * f.bitrateKbps * 1000 / f.sampleRate + ((h[2] >> 1) & 1);
  return true;
}

// VBR files start with a frame carrying the frame count ("Xing"/"Info" after the side info,
// or Fraunhofer's "VBRI" 32 bytes in). Returns 0 if there's none.
uint32_t vbrFrameCount(const uint8_t *frame, size_t avail, const Frame &f) {
  const size_t side = f.mpeg1 ? (f.channels == 1 ? 17 : 32) : (f.channels == 1 ? 9 : 17);
  const size_t x = 4 + side;
  if (avail >= x + 12 && (!memcmp(frame + x, "Xing", 4) || !memcmp(frame + x, "Info", 4))) {
    const uint32_t flags = be32(frame + x + 4);
    return (flags & 1) ? be32(frame + x + 8) : 0;
  }
  if (avail >= 36 + 18 && !memcmp(frame + 36, "VBRI", 4)) return be32(frame + 36 + 14);
  return 0;
}

}  // namespace

long readTags(FILE *f, std::string &title, std::string &artist) {
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

bool probe(const std::string &path, Meta &out) {
  out = Meta{};
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) return false;
  out.audioStart = readTags(f, out.title, out.artist);
  fseek(f, 0, SEEK_END);
  out.audioBytes = ftell(f) - out.audioStart;
  if (out.title.empty()) {
    const size_t slash = path.rfind('/');
    out.title = path.substr(slash + 1);
    const size_t dot = out.title.rfind('.');
    if (dot != std::string::npos) out.title.resize(dot);
  }

  // First frame: a valid header whose successor is valid too (skips stray 0xFF bytes).
  constexpr size_t kScan = 8192;
  std::unique_ptr<uint8_t[]> buf(new uint8_t[kScan]);
  fseek(f, out.audioStart, SEEK_SET);
  const size_t n = fread(buf.get(), 1, kScan, f);
  fclose(f);
  for (size_t i = 0; i + 4 <= n; ++i) {
    Frame fr, next;
    if (!parseFrame(buf.get() + i, fr)) continue;
    if (i + fr.length + 4 <= n && !parseFrame(buf.get() + i + fr.length, next)) continue;
    out.sampleRate = fr.sampleRate;
    out.channels = fr.channels;
    out.bitrateKbps = fr.bitrateKbps;
    const uint32_t frames = vbrFrameCount(buf.get() + i, n - i, fr);
    if (frames) {
      out.vbr = true;
      out.durationMs = static_cast<uint32_t>(uint64_t(frames) * fr.samplesPerFrame * 1000 /
                                             fr.sampleRate);
      if (out.durationMs) {
        out.bitrateKbps = static_cast<uint16_t>(uint64_t(out.audioBytes) * 8 / out.durationMs);
      }
    } else {
      out.durationMs = static_cast<uint32_t>(uint64_t(out.audioBytes) * 8 / fr.bitrateKbps);
    }
    return true;
  }
  return false;
}

// Offset of the first real frame in buf: a valid header followed by another one exactly a
// frame later. A bare sync search often stops on 0xFFE bit patterns inside audio data.
int findFrame(const uint8_t *buf, int n) {
  for (int i = 0; i + 4 <= n; ++i) {
    Frame a, b;
    if (!parseFrame(buf + i, a)) continue;
    const int next = i + static_cast<int>(a.length);
    if (next + 4 <= n && parseFrame(buf + next, b)) return i;
  }
  return -1;
}

bool waveform(const std::string &path, const Meta &meta, int bins, std::vector<uint8_t> &out) {
  out.assign(bins, 0);
  if (meta.audioBytes <= 0 || bins <= 0) return false;
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) return false;
  HMP3Decoder decoder = MP3InitDecoder();
  constexpr int kChunk = 6144;  // ~5 frames at 320 kbps, plenty at lower rates
  std::unique_ptr<uint8_t[]> in(new uint8_t[kChunk]);
  std::unique_ptr<int16_t[]> pcm(new int16_t[MAX_NCHAN * MAX_NGRAN * MAX_NSAMP]);
  if (!decoder) {
    fclose(f);
    return false;
  }
  for (int b = 0; b < bins; ++b) {
    const long at = meta.audioStart + static_cast<long>(meta.audioBytes * (b + 0.5) / bins);
    if (fseek(f, at, SEEK_SET) != 0) break;
    const int n = static_cast<int>(fread(in.get(), 1, kChunk, f));
    const int start = findFrame(in.get(), n);
    if (start < 0) continue;
    unsigned char *ptr = in.get() + start;
    int left = n - start;
    double sum = 0;
    uint32_t samples = 0;
    // The first frame after a seek lacks its bit-reservoir data; the following ones decode.
    for (int frame = 0; frame < 5 && left > 0; ++frame) {
      const int err = MP3Decode(decoder, &ptr, &left, pcm.get(), 0);
      if (err == ERR_MP3_INDATA_UNDERFLOW) break;
      if (err == ERR_MP3_MAINDATA_UNDERFLOW) continue;
      if (err) {  // lost sync: find the next real frame
        const int skip = findFrame(ptr + 1, left - 1);
        if (skip < 0) break;
        ptr += skip + 1;
        left -= skip + 1;
        continue;
      }
      MP3FrameInfo info;
      MP3GetLastFrameInfo(decoder, &info);
      for (int i = 0; i < info.outputSamps; ++i) sum += double(pcm[i]) * pcm[i];
      samples += info.outputSamps;
    }
    if (samples) {
      const double rms = sqrt(sum / samples) / 32768.0;
      const double db = rms > 0 ? 20 * log10(rms) : -100;  // 0 dB = full scale
      out[b] = static_cast<uint8_t>(std::clamp((db + 60) / 60, 0.0, 1.0) * 255);
    }
  }
  MP3FreeDecoder(decoder);
  fclose(f);
  return true;
}

}  // namespace mp3info
