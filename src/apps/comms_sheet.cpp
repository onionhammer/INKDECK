// The sheets that rise over the screen in use (apps/comms_ui.h): the call sheet when a call starts
// playing, the ring sheet when a ring starts here, the conversation sheet once per conversation
// this board isn't in; apps::pollComms() decides where they may. And the status bar's mark.
#include "features.h"

#if INKDECK_COMMS  // the whole file (features.h)

#include <Arduino.h>

#include <algorithm>
#include <string>
#include <vector>

#include "apps/clock.h"
#include "apps/comms_ui.h"
#include "apps/home.h"
#include "audio.h"
#include "input.h"
#if INKDECK_AGENT
#include "agent_client.h"
#include "apps/agent_sheet.h"
#endif

namespace apps {
namespace {

using namespace commsui;
using ui::Key;
using ui::Rect;

const std::string kDot = std::string(" ") + ui::glyph::kDot + " ";

constexpr int kSheetMinH = 384;  // the comms sheets alike (the ringing sheet's too)
constexpr int kStateH = 52;  // the state word beside the meter
constexpr uint32_t kCloseMs = 10000;       // after the last word, and as long since a key
constexpr uint32_t kFirstWordMs = 20000;   // answered or joined: for the first word
constexpr uint32_t kRingEndShowMs = 6000;  // how a ring ended, shown this long (and since a key)
constexpr uint32_t kMessageShowMs = 30000;  // a message left, once it's all here
constexpr uint32_t kMessageWaitMs = 15000;  // the dialer said a message comes: if none does
constexpr uint32_t kConvSheetMs = 30000;    // the conversation sheet, untouched
constexpr uint32_t kQuietKeyMs = 3000;      // the conversation sheet waits this long after a key

// Which of these sheets is on top, if any.
enum class Kind : uint8_t { None, Call, Ring, Conversation };
const ui::Screen *onTop = nullptr;
Kind onTopKind = Kind::None;

void entered(const ui::Screen *s, Kind k) {
  onTop = s;
  onTopKind = k;
}
void left(const ui::Screen *s) {
  if (onTop == s) {
    onTop = nullptr;
    onTopKind = Kind::None;
  }
}
bool sheetOnTop(Kind k) {
  const ui::Screen *top = ui::nav::top();
  return top && top == onTop && onTopKind == k;
}

// A conversation for the sheet to rise for (pollComms() raises it once), and the one it last
// rose for or was handed (by the ring sheet: answered on another board).
bool convPending = false;
int convPendingGroup = 0;
uint32_t convSeen = 0;     // status().conversationsSeen as last taken
uint32_t convShownId = 0;  // that conversation's id

// A board as the sheets say how it's reached: "ON AIR · −52 dBm", "VIA KITCHEN · 1 HOP"; "" if
// not known.
std::string reachText(comms::Route route, int hops, const std::string &relay, int rssi) {
  if (route == comms::Route::Relayed) {
    return "VIA " + (relay.empty() ? std::string("A RELAY") : relay) + kDot +
           std::to_string(hops) + (hops == 1 ? " HOP" : " HOPS");
  }
  if (route == comms::Route::Air) {
    return "ON AIR" +
           (rssi ? kDot + ui::signedNumber(static_cast<float>(rssi), 0) + " dBm" : std::string());
  }
  return {};
}

const comms::Board *findBoard(const std::vector<comms::Board> &bs, uint32_t id) {
  for (const comms::Board &b : bs) {
    if (b.id == id) return &b;
  }
  return nullptr;
}

// The other boards in a conversation by name, for a reply to all of it: "KITCHEN + GARAGE".
std::string othersText(const comms::Conversation &c) {
  std::string s;
  for (size_t i = 0; i < c.members.size(); ++i) {
    if (!s.empty()) s += " + ";
    s += boardName(i < c.names.size() ? c.names[i] : std::string(), c.members[i]);
  }
  return s;
}

// The group's calls not live, but for the one at `skip` (-1: none).
std::vector<comms::Call> earlierCalls(const std::vector<comms::Call> &calls, int skip = -1) {
  std::vector<comms::Call> out;
  for (size_t i = 0; i < calls.size(); ++i) {
    if (static_cast<int>(i) != skip && !calls[i].live) out.push_back(calls[i]);
  }
  return out;
}

// This board is in a conversation now (answered, joined, a reply): the call sheet for it takes
// the sheet's place; over the Comms screen, which shows it in place, the sheet just closes.
void toConversation(int group, uint32_t peer, bool toAll) {
  const ui::Screen *base = ui::nav::base();
  if (base && base->tag() == moduleTag("COMMS")) {
    ui::nav::pop();
    return;
  }
  ui::nav::replace(makeConversationCallSheet(group, peer, toAll));
}

// --- The call sheet -----------------------------------------------------------------------------

class CallSheet : public ui::Screen {
 public:
  // For the call that has just started playing.
  CallSheet() {
    comms::hold();  // while it's up the call counts as heard, and the radio stays on
    follow(comms::status());
    activeMs_ = millis();
    conv_ = comms::conversation(group_);
  }
  // For the conversation this board has just answered (a ring) or joined: with the board it's
  // with, before anyone has said a word.
  CallSheet(int group, uint32_t peer, bool toAll, const char *how) : how_(how) {
    comms::hold();
    incoming_ = comms::status().incoming;
    group_ = group;
    peer_ = peer;
    toAll_ = toAll;
    waiting_ = true;
    if (const comms::Board *b = findBoard(comms::boards(group), peer)) {
      peerName_ = b->name;
      route_ = b->route;
      hops_ = b->hops;
      relayName_ = b->relayName;
      rssi_ = b->rssi;
    }
    activeMs_ = millis();
    conv_ = comms::conversation(group_);
  }
  ~CallSheet() override { comms::release(); }

  std::string tag() const override { return {}; }  // the screen below supplies the chrome
  bool overlay() const override { return true; }
  // HOLD BOOT replies the way the call came: to all of the group, or to the caller alone (in a
  // conversation, to all of it: the caller is in it).
  ui::TalkTo talkTo() const override {
    ui::TalkTo to;
    to.comms = true;
    to.group = group_;
    to.board = toAll_ ? 0 : peer_;
    return to;
  }
  int hints(ui::Hint *out) const override {
    const bool in = conv_.on && conv_.in;
    out[0] = {"HOLD BOOT", in ? "TALK" : "REPLY"};
    out[1] = {ui::glyph::kDial, "VOL"};
    int n = 2;
    if (in) out[n++] = {ui::glyph::kPress, "LEAVE"};
    out[n++] = {"BOOT", playing() ? "STOP" : "CLOSE"};  // closing leaves this board in it
    return n;
  }

  void onEnter() override { entered(this, Kind::Call); }
  void onLeave() override { left(this); }

  bool onKey(Key key) override {
    switch (key) {
      case Key::Up: audio::changeVolume(+5); return true;
      case Key::Down: audio::changeVolume(-5); return true;
      case Key::Select:
        if (conv_.on && conv_.in) {  // leaves the conversation: its voice stops here
          comms::leave(group_);
          ui::nav::pop();
        }
        return true;
      case Key::Back: return stopIfPlaying();  // else it closes, and this board stays in
      default: return false;
    }
  }

  bool onTick() override {
    const comms::Status s = comms::status();
    const uint32_t now = millis();
    // Another call came while it was up: it follows that one.
    if (s.incoming != incoming_ && s.state == comms::State::Hearing && !s.replay) {
      follow(s);
      ui::nav::talkChanged();
    }
    if (s.state == comms::State::Talking || s.state == comms::State::Hearing ||
        comms::talking()) {
      activeMs_ = now;
    }
    if (s.state == comms::State::Talking) {
      replied_ = true;
      waiting_ = false;
    }
    if (s.state == comms::State::Hearing && !s.replay && s.peer == peer_) {  // as it comes now
      route_ = s.route;
      hops_ = s.hops;
      relayName_ = s.relayName;
      rssi_ = s.rssi;
    }
    conv_ = comms::conversation(group_);
    const uint32_t wait = waiting_ ? kFirstWordMs : kCloseMs;
    if (now - activeMs_ > wait && input::idleMs() > kCloseMs) {
      ui::nav::pop();  // in a conversation, this board stays in it
      return false;
    }
    return comms::generation() != drawn_ || attemptError() != drawnError_ ||
           extra() != drawnExtra_;
  }

  void draw(ui::Canvas &c, const Rect &body) override {
    using namespace ui;
    drawn_ = comms::generation();
    drawnError_ = attemptError();
    drawnExtra_ = extra();
    const comms::Status s = comms::status();
    conv_ = comms::conversation(group_);
    const bool inConv = conv_.on && conv_.in;
    const bool talking = s.state == comms::State::Talking;
    const std::vector<comms::Call> calls = comms::calls(group_, 12);

    // Over a screen without chrome, the sheet shows its keys itself.
    Hint keys[kMaxHints];
    const Screen *under = nav::base();
    const int keyCount = under && !under->chrome() ? hints(keys) : 0;
    Rect r = drawSheet(c, body, kSheetMinH, keys, keyCount);

    // What the big block shows: the reply while it goes and once it's sent, else the call (none
    // yet in a conversation just answered or joined).
    const bool showReply = talking || replied_;
    int shownAt = -1;  // its record in `calls`, left out of EARLIER
    for (size_t i = 0; i < calls.size() && shownAt < 0 && (showReply || !waiting_); ++i) {
      const comms::Call &k = calls[i];
      if (k.kind != comms::Kind::Voice) continue;
      if (showReply ? k.outgoing : !k.outgoing && k.from == peer_) shownAt = static_cast<int>(i);
    }
    const comms::Call *shown = shownAt >= 0 ? &calls[shownAt] : nullptr;

    const std::string group = groupName(group_);
    const Rect head = r.takeTop(kSectionH);
    if (!drawnError_.empty()) {
      drawNotice(c, head, drawnError_);
    } else {
      std::string to;
      if (inConv) {
        to = "CONVERSATION";
      } else if (showReply) {
        const uint32_t target = talking ? s.peer : shown ? shown->to : 0;
        const std::string name = talking ? s.peerName : shown ? shown->toName : "";
        to = target ? "TO " + boardName(name, target) : "TO ALL";
      } else {
        to = toAll_ ? "TO ALL" : "TO YOU";
      }
      drawSectionHeader(c, head, "COMMS" + kDot + group + kDot + to,
                        "VOL " + std::to_string(audio::status().volume), {true});
    }

    // The state, and the meter beside it.
    const bool live = talking || s.state == comms::State::Hearing;
    drawStateRow(c, r.takeTop(kStateH), stateWord(s.state), live ? meterFill(s.levelDb) : 0);

    // FROM (or TO) and the name in large type, how it comes and how long; in a conversation,
    // who is in it.
    std::string label, name, line;
    if (showReply) {
      label = "TO";
      const uint32_t target = talking ? s.peer : shown ? shown->to : 0;
      const std::string tname = talking ? s.peerName : shown ? shown->toName : "";
      const std::string others = inConv ? othersText(conv_) : std::string();
      name = target           ? boardName(tname, target)
             : !others.empty() ? others
                               : "ALL OF " + group;
      if (talking) {
        line = (s.channel ? "AIR CH " + std::to_string(s.channel) + kDot : "") +
               lengthText(s.ms) + " SO FAR";
      } else if (shown) {
        line = lengthText(shown->ms) + kDot +
               (shown->heardBy > 0 ? std::to_string(shown->heardBy) + " HEARD IT"
                                   : std::string("SENT"));
      }
    } else if (!peer_) {  // joined: with whoever is in it
      label = "WITH";
      const std::string others = othersText(conv_);
      name = others.empty() ? "ALL OF " + group : others;
      line = how_;
    } else {
      label = "FROM";
      name = boardName(peerName_, peer_);
      const bool hearing = s.state == comms::State::Hearing && !s.replay && s.peer == peer_;
      line = reachText(route_, hops_, relayName_, rssi_);
      if (waiting_) {
        line = std::string(how_) + (line.empty() ? "" : kDot + line);
      } else if (hearing) {
        line += kDot + lengthText(s.ms) + " SO FAR";
      } else if (shown) {
        line += kDot + lengthText(shown->ms);
      }
    }
    const std::string line2 =
        conv_.on ? membersText(conv_) + kDot + forText(conv_.ms) : std::string();
    r.takeTop(drawNameBlock(c, r, label, name, line, line2) - r.y);

    // EARLIER: the group's other calls, as many as fit over the foot.
    drawEarlier(c, r, earlierCalls(calls, shownAt),
                inConv ? "CLOSES 10 S AFTER THE LAST WORD; YOU STAY IN IT"
                       : "CLOSES 10 S AFTER THE LAST WORD");
  }

 private:
  void follow(const comms::Status &s) {
    incoming_ = s.incoming;
    group_ = s.group;
    peer_ = s.peer;
    peerName_ = s.peerName;
    toAll_ = s.toAll;
    route_ = s.route;
    hops_ = s.hops;
    relayName_ = s.relayName;
    rssi_ = s.rssi;
    replied_ = false;
    waiting_ = false;
  }

  // What changes the sheet besides generation(): the conversation's minutes, a replay playing.
  std::string extra() const {
    return (conv_.on ? forText(conv_.ms) : std::string()) + (playing() ? "\x01" : "");
  }

  uint32_t incoming_ = 0;
  int group_ = 0;
  uint32_t peer_ = 0;
  std::string peerName_;
  bool toAll_ = true;
  // How the call came, as last seen while it played (status() forgets once it's over).
  comms::Route route_ = comms::Route::Air;
  int hops_ = 0;
  std::string relayName_;
  int rssi_ = 0;
  bool replied_ = false;  // HOLD BOOT talked from the sheet since this call
  bool waiting_ = false;  // a conversation answered or joined, before its first word here
  const char *how_ = "";  // then: "ANSWERED", "JOINED"
  comms::Conversation conv_;
  uint32_t activeMs_ = 0;
  uint32_t drawn_ = 0;
  std::string drawnError_;
  std::string drawnExtra_;
};

// --- The conversation sheet ---------------------------------------------------------------------

class ConversationSheet : public ui::Screen {
 public:
  explicit ConversationSheet(int group) : group_(group), sinceMs_(millis()) {
    comms::hold();
    conv_ = comms::conversation(group_);
    convShownId = conv_.id;
  }
  ~ConversationSheet() override { comms::release(); }

  std::string tag() const override { return {}; }
  bool overlay() const override { return true; }
  // HOLD BOOT joins it and talks at once (talkStart() joins the group's conversation).
  ui::TalkTo talkTo() const override {
    ui::TalkTo to;
    to.comms = true;
    to.group = group_;
    return to;
  }
  int hints(ui::Hint *out) const override {
    out[0] = {"HOLD BOOT", "TALK"};
    out[1] = {ui::glyph::kPress, "JOIN"};
    out[2] = {"BOOT", playing() ? "STOP" : "LATER"};
    return 3;
  }

  void onEnter() override { entered(this, Kind::Conversation); }
  void onLeave() override { left(this); }

  bool onKey(Key key) override {
    sinceMs_ = millis();
    switch (key) {
      case Key::Select:
        if (comms::join(group_)) {
          toConversation(group_, 0, true);
        } else {
          noteAttempt();  // it ended a moment ago
        }
        return true;
      case Key::Back: return stopIfPlaying();  // else later: it closes
      default: return false;
    }
  }

  bool onTick() override {
    conv_ = comms::conversation(group_);
    if (!conv_.on) {
      ui::nav::pop();  // over before anyone joined here
      return false;
    }
    if (conv_.in) {  // joined: HOLD BOOT talked into it
      toConversation(group_, 0, true);
      return false;
    }
    if (millis() - sinceMs_ > kConvSheetMs && input::idleMs() > kConvSheetMs &&
        comms::status().state != comms::State::Talking) {
      ui::nav::pop();
      return false;
    }
    return comms::generation() != drawn_ || attemptError() != drawnError_ ||
           forText(conv_.ms) != drawnFor_;
  }

  void draw(ui::Canvas &c, const Rect &body) override {
    using namespace ui;
    drawn_ = comms::generation();
    drawnError_ = attemptError();
    drawnFor_ = forText(conv_.ms);
    Hint keys[kMaxHints];
    const Screen *under = nav::base();
    const int keyCount = under && !under->chrome() ? hints(keys) : 0;
    Rect r = drawSheet(c, body, kSheetMinH, keys, keyCount);
    const std::string group = groupName(group_);
    const Rect head = r.takeTop(kSectionH);
    if (!drawnError_.empty()) {
      drawNotice(c, head, drawnError_);
    } else {
      drawSectionHeader(c, head, "COMMS" + kDot + group + kDot + "CONVERSATION ON",
                        boardsText(static_cast<int>(conv_.members.size())), {true});
    }
    drawStateRow(c, r.takeTop(kStateH), "CONVERSATION", -1, drawnFor_);
    const std::string line = conv_.left ? "YOU LEFT IT: ITS VOICE DOESN'T PLAY HERE"
                                        : "ITS VOICE PLAYS HERE ONCE YOU JOIN";
    r.takeTop(drawNameBlock(c, r, "IN IT", othersText(conv_), line) - r.y);
    drawEarlier(c, r, earlierCalls(comms::calls(group_, 12)), "CLOSES BY ITSELF IN 30 S");
  }

 private:
  int group_;
  uint32_t sinceMs_;  // it rose, or the last key
  comms::Conversation conv_;
  uint32_t drawn_ = 0;
  std::string drawnError_;
  std::string drawnFor_;
};

// --- The ring sheet -----------------------------------------------------------------------------

class RingSheet : public ui::Screen {
 public:
  RingSheet() {
    comms::hold();
    follow(comms::incomingRing());
  }
  ~RingSheet() override { comms::release(); }

  std::string tag() const override { return {}; }
  bool overlay() const override { return true; }
  // HOLD BOOT answers and talks at once; once the ring is over, it replies the way it came.
  ui::TalkTo talkTo() const override {
    ui::TalkTo to;
    to.comms = true;
    to.group = group_;
    to.board = toAll_ ? 0 : from_;
    to.answer = true;
    return to;
  }
  int hints(ui::Hint *out) const override {
    int n = 0;
    if (ringing_) {
      out[n++] = {"HOLD BOOT", "TALK"};
      out[n++] = {ui::glyph::kPress, "ANSWER"};
      out[n++] = {"BOOT", "IGNORE"};
      return n;
    }
    out[n++] = {"HOLD BOOT", "REPLY"};
    if (msg_.id && !msg_.receiving) {
      out[n++] = {ui::glyph::kPress, playingThis() ? "STOP" : "PLAY"};
    }
    out[n++] = {"BOOT", playing() ? "STOP" : "CLOSE"};
    return n;
  }

  void onEnter() override { entered(this, Kind::Ring); }
  void onLeave() override { left(this); }

  bool onKey(Key key) override {
    keyMs_ = millis();
    switch (key) {
      case Key::Select:
        if (ringing_) {
          if (comms::answer()) toConversation(group_, from_, toAll_);
          return true;
        }
        if (msg_.id && !msg_.receiving) {
          if (playingThis()) {
            comms::stopPlaying();
          } else {
            if (playing()) comms::stopPlaying();
            noteAttempt();
            comms::playMessage(msg_.id);
          }
        }
        return true;
      case Key::Back:
        if (stopIfPlaying()) return true;
        if (ringing_) comms::ignoreRing();  // it stops ringing here only
        return false;                       // closes
      default: return false;
    }
  }

  bool onTick() override {
    const uint32_t now = millis();
    const comms::Ring rg = comms::incomingRing();
    if (rg.on && rg.count != count_) {  // another ring: it follows that one
      follow(rg);
      ui::nav::talkChanged();
      return true;
    }
    ring_ = rg;
    if (ringing_ && !rg.on) {
      ringing_ = false;
      endedMs_ = now;
    }
    if (ringing_) return comms::generation() != drawn_;
    switch (rg.ended) {
      case comms::RingEnd::Answered:  // here: HOLD BOOT, or another way in
        toConversation(group_, from_, toAll_);
        return false;
      case comms::RingEnd::AnsweredThere: {  // the conversation is on without this board
        const ui::Screen *base = ui::nav::base();
        if (base && base->tag() == moduleTag("COMMS")) {
          ui::nav::pop();  // the talk screen shows it, with JOIN
        } else {
          ui::nav::replace(std::make_unique<ConversationSheet>(group_));
        }
        return false;
      }
      case comms::RingEnd::Ignored:
        ui::nav::pop();
        return false;
      default: break;
    }
    if (comms::status().state == comms::State::Talking) {  // a reply: the call sheet shows it
      toConversation(group_, from_, toAll_);
      return false;
    }
    if (rg.ended == comms::RingEnd::Message) findMessage();
    const uint32_t quiet = std::min(now - std::max(endedMs_, keyMs_), input::idleMs());
    bool close;
    if (playing()) {
      close = false;
      keyMs_ = now;  // shown a while after it has played
    } else if (rg.ended != comms::RingEnd::Message) {
      close = quiet > kRingEndShowMs;  // MISSED
    } else if (!msg_.id) {
      close = now - endedMs_ > kMessageWaitMs;  // dropped by the dialer, or lost on the way
    } else if (msg_.receiving) {
      close = false;  // it comes (3 minutes at most, comms.h)
    } else {
      if (!readyMs_) readyMs_ = now;
      close = std::min(quiet, now - readyMs_) > kMessageShowMs;
    }
    if (close) {
      ui::nav::pop();  // a message not played stays new
      return false;
    }
    return comms::generation() != drawn_ || attemptError() != drawnError_ ||
           playing() != drawnPlaying_;
  }

  void draw(ui::Canvas &c, const Rect &body) override {
    using namespace ui;
    drawn_ = comms::generation();
    drawnError_ = attemptError();
    drawnPlaying_ = playing();
    Hint keys[kMaxHints];
    const Screen *under = nav::base();
    const int keyCount = under && !under->chrome() ? hints(keys) : 0;
    Rect r = drawSheet(c, body, kSheetMinH, keys, keyCount);
    const std::string group = groupName(group_);
    const Rect head = r.takeTop(kSectionH);
    if (!drawnError_.empty()) {
      drawNotice(c, head, drawnError_);
    } else {
      drawSectionHeader(c, head,
                        "COMMS" + kDot + group + kDot + (toAll_ ? "RINGS ALL" : "RINGS YOU"), "",
                        {true});
    }

    // The state: RINGING, MISSED, MESSAGE COMING, LEFT A MESSAGE or PLAYING, with its seconds.
    std::string word, right, line;
    const std::string rang = "RANG " + lengthText(ring_.ms);
    if (ringing_) {
      word = "RINGING";
      right = lengthText(ring_.ms);
    } else if (ring_.ended == comms::RingEnd::Message) {
      if (playingThis()) {
        word = "PLAYING";
        right = lengthText(msg_.ms);
      } else if (msg_.id && !msg_.receiving) {
        word = "LEFT A MESSAGE";
        right = lengthText(msg_.ms);
      } else {
        word = "MESSAGE COMING";
        right = msg_.id ? lengthText(msg_.ms) : "";
      }
      line = rang + kDot +
             (msg_.id && !msg_.receiving && !msg_.complete ? "SOME OF IT LOST" : "A MESSAGE INSTEAD");
    } else {
      word = "MISSED";
      right = lengthText(ring_.ms);
      line = rang + kDot +
             (ring_.ended == comms::RingEnd::GaveUp     ? "THEY GAVE UP"
              : ring_.ended == comms::RingEnd::NoAnswer ? "NOBODY ANSWERED"
                                                        : "THEY WENT OUT OF REACH");
    }
    drawStateRow(c, r.takeTop(kStateH), word, -1, right);
    if (ringing_) {
      std::string reach;
      if (const comms::Board *b = findBoard(comms::boards(group_), from_)) {
        reach = reachText(b->route, b->hops, b->relayName, b->rssi);
      }
      line = (reach.empty() ? "" : reach + kDot) + (toAll_ ? "TO ALL OF " + group : "TO YOU");
    }
    r.takeTop(drawNameBlock(c, r, "FROM", boardName(ring_.fromName, from_), line) - r.y);
    // EARLIER, without this ring and the message left for it (the block above says them).
    std::vector<comms::Call> earlier;
    bool ringSkipped = false, messageSkipped = ring_.ended != comms::RingEnd::Message;
    for (const comms::Call &k : comms::calls(group_, 12)) {
      if (!k.outgoing && k.from == from_) {
        if (!ringSkipped && k.kind == comms::Kind::Ring) {
          ringSkipped = true;
          continue;
        }
        if (!messageSkipped && k.kind == comms::Kind::Message) {
          messageSkipped = true;
          continue;
        }
      }
      if (!k.live) earlier.push_back(k);
    }
    drawEarlier(c, r, earlier,
                ringing_ ? "IT RINGS UNTIL SOMEONE ANSWERS, FOR 1:00 AT MOST"
                : ring_.ended == comms::RingEnd::Message && msg_.id && !msg_.receiving
                    ? "KEPT AS NEW UNTIL PLAYED"
                    : "CLOSES IN A FEW SECONDS");
  }

 private:
  void follow(const comms::Ring &rg) {
    ring_ = rg;
    count_ = rg.count;
    group_ = rg.group;
    from_ = rg.from;
    toAll_ = rg.toAll;
    ringing_ = rg.on;
    openedMs_ = millis();
    endedMs_ = keyMs_ = readyMs_ = 0;
    msg_ = comms::Message();
  }

  // The message the dialer left: the newest from it in the group, begun since the sheet rose.
  void findMessage() {
    const uint32_t ageS = (millis() - openedMs_) / 1000 + 2;
    for (const comms::Message &m : comms::messages(group_)) {
      if (m.from == from_ && m.agoS <= ageS) {
        msg_ = m;
        return;
      }
    }
  }

  bool playingThis() const {
    return msg_.id && comms::status().playingMessage == msg_.id;
  }

  comms::Ring ring_;
  uint32_t count_ = 0;
  int group_ = 0;
  uint32_t from_ = 0;
  bool toAll_ = true;
  bool ringing_ = false;
  uint32_t openedMs_ = 0;
  uint32_t endedMs_ = 0;  // the ring stopped
  uint32_t keyMs_ = 0;
  uint32_t readyMs_ = 0;  // the message was all here
  comms::Message msg_;
  uint32_t drawn_ = 0;
  std::string drawnError_;
  bool drawnPlaying_ = false;
};

// --- Where they rise ----------------------------------------------------------------------------

#if INKDECK_AGENT
// The agent sheet listening, thinking or speaking: a sheet over it would take its keys.
bool agentTalking() {
  if (!agentSheetOnTop()) return false;
  const agent::Status a = agent::status();
  return a.mic || a.state == agent::State::Connecting || a.state == agent::State::Listening ||
         a.state == agent::State::Thinking || a.state == agent::State::Speaking;
}
#endif

// Not over the lock, an off face or the desk face, the Recorder or a ringing alarm, the agent
// sheet while it's busy; nor, unless overComms, over the Comms screen (it shows calls and
// conversations in place).
bool mayRise(bool overComms) {
  const ui::Screen *top = ui::nav::top();
  const ui::Screen *base = ui::nav::base();
  if (!top || !base || ui::nav::locked() || top->offFace()) return false;
  if (!top->allowsTalk() || !base->allowsTalk()) return false;  // the Recorder, a ringing alarm
  if (deskShowing()) return false;
  if (!overComms && base->tag() == moduleTag("COMMS")) return false;
#if INKDECK_AGENT
  if (agentTalking()) return false;
#endif
  return true;
}

void raise(std::unique_ptr<ui::Screen> sheet) {
  ui::nav::push(std::move(sheet));
  ui::nav::flush();  // now, even over a Reader about to nap again
}

// The status bar's speech bubble: the boards in a conversation on in one of this board's groups
// that this board isn't in; 0 for none.
int conversationMark() {
  for (int g = 0; g < comms::kMaxGroups; ++g) {
    const comms::Conversation c = comms::conversation(g);
    if (c.on && !c.in && !c.members.empty()) return static_cast<int>(c.members.size());
  }
  return 0;
}

}  // namespace

std::unique_ptr<ui::Screen> commsui::makeConversationCallSheet(int group, uint32_t peer,
                                                               bool toAll) {
  return std::make_unique<CallSheet>(group, peer, toAll, peer ? "ANSWERED" : "JOINED");
}

void pollComms() {
  static bool primed = false;
  static uint32_t seenIncoming = 0;
  static uint32_t raisedRing = 0;  // incomingRing().count the ring sheet rose for
  const comms::Status s = comms::status();
  if (!primed) {
    primed = true;
    seenIncoming = s.incoming;
    convSeen = s.conversationsSeen;
    ui::nav::onConversation(conversationMark);
    return;
  }

  // A ring: the sheet rises at once (over the Comms screen too: it has no ANSWER of its own), or
  // as soon as it may while it still rings. Where it may not, the board rings by sound alone.
  const comms::Ring rg = comms::incomingRing();
  if (rg.on && rg.count != raisedRing && mayRise(true)) {
    raisedRing = rg.count;
    if (!sheetOnTop(Kind::Ring)) raise(std::make_unique<RingSheet>());  // else it follows
  }

  // A call starts playing.
  if (s.incoming != seenIncoming) {
    seenIncoming = s.incoming;
    if (s.state == comms::State::Hearing && !s.replay && !sheetOnTop(Kind::Call) &&
        !sheetOnTop(Kind::Ring) && mayRise(false)) {
      raise(std::make_unique<CallSheet>());
    }
  }

  // A conversation this board isn't in: once, when it may (not straight after a key, nor over
  // another sheet or menu); dropped if it ends, this board joins, or the Comms screen shows it.
  if (s.conversationsSeen != convSeen) {
    convSeen = s.conversationsSeen;
    convPending = true;
    convPendingGroup = s.conversationGroup;
  }
  if (convPending) {
    const comms::Conversation c = comms::conversation(convPendingGroup);
    const ui::Screen *top = ui::nav::top();
    const ui::Screen *base = ui::nav::base();
    if (!c.on || c.in || c.left || c.id == convShownId) {
      convPending = false;
    } else if (base && base->tag() == moduleTag("COMMS")) {
      convPending = false;  // the talk screen shows it, with JOIN
    } else if (top && !top->overlay() && input::idleMs() >= kQuietKeyMs && mayRise(false)) {
      convPending = false;
      raise(std::make_unique<ConversationSheet>(convPendingGroup));
    }
  }
}

}  // namespace apps

#endif  // INKDECK_COMMS
