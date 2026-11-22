// A streaming HTTP(S) GET for bodies too big to hold (podcast episodes) or worth cutting short
// (long RSS feeds): follows redirects (podcast links usually pass through one or two tracking
// hosts), can resume from a byte offset, and hands the body over in chunks. HTTPS uses the
// core's certificate bundle. Call from a background task with Wi-Fi up (wlan.h). A URL whose
// host is on the tailnet holds it from open() to close() (tailnet.h).
#pragma once
#include <stddef.h>
#include <stdint.h>

#include <string>

namespace net {

// Lets TLS put its buffers (about 40 KB a connection) in PSRAM. The core's mbedTLS allocates
// from internal RAM only, which runs short once the player and a few tasks are going. Call once
// in setup(), before any HTTPS.
void begin();

// Downloads a feed (RSS or Atom) into body until it holds maxItems complete items or entries,
// maxBytes, or the end: some feeds carry every article whole and run to megabytes. A connection
// lost after an item has come keeps what came. False if nothing came: error says why.
bool getFeed(const std::string &url, std::string &body, std::string &error, size_t maxBytes,
             int maxItems);

class Get {
 public:
  Get() = default;
  ~Get() { close(); }
  Get(const Get &) = delete;
  Get &operator=(const Get &) = delete;

  // Sends the request and reads the headers. from > 0 asks for the body from that byte on, but
  // only if the file is still the one `ifRange` (a validator() from the first request) names:
  // check status(), 206 if the server sent the rest, 200 if it sent the whole (changed) file.
  // Podcast hosts that stitch in ads can serve a different file each time, so a resume without
  // a validator would splice two of them. False on a network error or a status outside 200-299
  // and 416; error then says why in a few words.
  bool open(const std::string &url, std::string &error, int64_t from = 0,
            const std::string &ifRange = "");

  // Before open(): the User-Agent to send, else one that names a podcast client. Kept, not
  // copied.
  void setUserAgent(const char *ua) { userAgent_ = ua; }
  // After open(): where the request ended up, past redirects.
  std::string url() const;

  int status() const { return status_; }
  int64_t length() const { return length_; }  // of this response's body; -1 if not given
  // What identifies this version of the file: its ETag, else its Last-Modified; "" if neither.
  std::string validator() const { return etag_.empty() ? lastModified_ : etag_; }

  // Up to len bytes of the body: > 0 read, 0 at the end, < 0 an error.
  int read(char *buf, int len);

  void close();

 private:
  static int onEvent(void *event);  // esp_http_client_event_t: collects the validators

  void *client_ = nullptr;  // esp_http_client_handle_t
  bool tailnet_ = false;    // this request holds the tailnet
  const char *userAgent_ = nullptr;
  int status_ = 0;
  int64_t length_ = -1;
  std::string etag_;
  std::string lastModified_;
};

}  // namespace net
