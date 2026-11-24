// The sheets that rise over the screen in use (apps/comms_ui.h): the call sheet when a mic session
// starts playing, the ring sheet when a ring starts here, the conversation sheet once per
// conversation this board isn't in, the LOST sheet when LOST's alarm starts; apps::pollComms()
// decides where they may. And the status bar's marks, and the key that silences LOST's alarm
// where its sheet can't rise.
//
// Where the mic shows while it's on (it must never be invisible): the status bar's MIC on every
// screen with chrome (the lock included); the desk face's own status bar (apps/clock.cpp); the
// Agent face and News's face live on USB, in their foot (apps/agent_face.cpp, news_edition.cpp);
// the picture viewer with its chrome hidden, a MIC tag in its corner (apps/viewer.cpp). The off
// faces on battery are drawn on the way to a power-off, which switches the mic off (power.cpp,
// comms::forceOff()): they don't show it.
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
#include "power.h"
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
constexpr uint32_t kCloseMs = 10000;       // after the last sound, and as long since a key
constexpr uint32_t kFirstWordMs = 20000;   // answered or joined: for the first word
constexpr uint32_t kRingEndShowMs = 6000;  // how a ring ended, shown this long (and since a key)
constexpr uint32_t kMessageShowMs = 30000;  // a message left, once it's all here
constexpr uint32_t kMessageWaitMs = 15000;  // the dialer said a message comes: if none does
constexpr uint32_t kConvSheetMs = 30000;    // the conversation sheet, untouched
constexpr uint32_t kQuietKeyMs = 3000;      // the conversation sheet waits this long after a key
constexpr uint32_t kBackShowMs = 10000;     // the LOST sheet once its mic is back or off
// A sheet that rose by itself takes no ● that would switch the mic on (the call sheet's MIC ON,
// the ring sheet's ANSWER) this soon after it rose: a press meant for the screen under it mustn't
// open a mic.
constexpr uint32_t kRiseGuardMs = 1000;
// A conversation this board isn't in waits this long before its sheet rises: a board that hears
// a mic open joins its conversation by itself a moment after it has seen it.
constexpr uint32_t kConvSettleMs = 5000;

// Which of these sheets is on top, if any.
enum class Kind : uint8_t { None, Call, Ring, Conversation, Lost };
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

// LOST: the mic the alarm sounded for (kept, as it may go before the sheet can rise), and
// whether its sheet still has to rise.
bool lostPending = false;
comms::OpenMic lostMic;

const comms::Board *findBoard(const std::vector<comms::Board> &bs, uint32_t id) {
  for (const comms::Board &b : bs) {
    if (b.id == id) return &b;
  }
  return nullptr;
}

// The other boards in a conversation by name: "KITCHEN + GARAGE".
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

std::string earlierKey(const std::vector<comms::Call> &calls) {
  std::string k;
  for (size_t i = 0; i < calls.size() && i < 8; ++i) k += callRowKey(calls[i]);
  return k;
}

// This board is in a conversation now (answered, joined): the call sheet for it takes the
// sheet's place; over the Comms screen, which shows it in place, the sheet just closes.
void toConversation(int group, uint32_t peer, bool toAll, const char *label, const char *how) {
  const ui::Screen *base = ui::nav::base();
  if (base && base->tag() == moduleTag("COMMS")) {
    ui::nav::pop();
    return;
  }
  ui::nav::replace(makeConversationCallSheet(group, peer, toAll, label, how));
}

// The board whose voice has just started playing: of the mics playing here, the newest session.
const comms::OpenMic *newestPlaying(const std::vector<comms::OpenMic> &mics) {
  const comms::OpenMic *best = nullptr;
  for (const comms::OpenMic &m : mics) {
    if (m.playing && (!best || m.onS < best->onS)) best = &m;
  }
  return best;
}

// --- The call sheet -----------------------------------------------------------------------------

class CallSheet : public ui::Screen {
 public:
  // For the mic session that has just started playing here.
  explicit CallSheet(const comms::OpenMic &m) : rose_(millis()) {
    comms::hold();  // while it's up the session counts as heard, and the radio stays on
    incoming_ = comms::status().incoming;
    follow(m);
    activeMs_ = millis();
    load();
  }
  // For the conversation this board has just answered (a ring), joined, or rung and had
  // answered: with the board it's with, before anyone has said a word.
  CallSheet(int group, uint32_t peer, bool toAll, const char *label, const char *how)
      : label_(label), how_(how) {
    comms::hold();
    incoming_ = comms::status().incoming;
    group_ = group;
    peer_ = peer;
    toAll_ = toAll;
    waiting_ = true;
    if (const comms::Board *b = findBoard(comms::boards(group), peer)) peerName_ = b->name;
    activeMs_ = millis();
    load();
  }
  ~CallSheet() override { comms::release(); }

  std::string tag() const override { return {}; }  // the screen below supplies the chrome
  bool overlay() const override { return true; }
  int hints(ui::Hint *out) const override {
    int n = 0;
    out[n++] = micHint();  // ● MIC ON (sticky), HOLD BOOT TALK (push)
    out[n++] = {ui::glyph::kDial, "VOL"};
    if (conv_.on && conv_.in) out[n++] = {ui::glyph::kHoldPress, "LEAVE"};
    out[n++] = {"BOOT", playing() ? "STOP" : "CLOSE"};  // closing leaves this board in it
    return n;
  }

  // Push to talk: HOLD BOOT keys the mic to the conversation the session is in, as ● would when
  // sticky.
  ui::TalkTo talkTo() const override { return micTalkTo(group_, micTarget()); }

  void onEnter() override { entered(this, Kind::Call); }
  void onLeave() override { left(this); }

  bool onKey(Key key) override {
    switch (key) {
      case Key::Up: audio::changeVolume(+5); return true;
      case Key::Down: audio::changeVolume(-5); return true;
      case Key::Select:
        // Sticky: the mic, to the conversation the session is in. Not in the sheet's first second,
        // unless it's to go off. Push: nothing (HOLD BOOT talks).
        if (!pushToTalk() && !comms::micEnabled() && millis() - rose_ < kRiseGuardMs) return true;
        return micPress(group_, micTarget());
      case Key::SelectLong:
        if (conv_.on && conv_.in) {  // out of the conversation: its voice stops here
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
    load();
    // Another session started playing while it was up: it follows that one.
    if (s.incoming != incoming_) {
      incoming_ = s.incoming;
      if (const comms::OpenMic *m = newestPlaying(mics_)) {
        follow(*m);
        rose_ = now;  // another board under the same finger
        ui::nav::talkChanged();  // HOLD BOOT's target with it (push)
      }
    }
    // A sound, theirs or this board's, keeps it up.
    if (s.hearing || mic_.sending) {
      activeMs_ = now;
      waiting_ = false;
    }
    const uint32_t wait = waiting_ ? kFirstWordMs : kCloseMs;
    if (now - activeMs_ > wait && input::idleMs() > kCloseMs) {
      ui::nav::pop();  // in a conversation, this board stays in it; its mic as it is
      return false;
    }
    return key() != drawn_;
  }

  void draw(ui::Canvas &c, const Rect &body) override {
    using namespace ui;
    drawn_ = key();
    // Over a screen without chrome, the sheet shows its keys itself.
    Hint keys[kMaxHints];
    const Screen *under = nav::base();
    const int keyCount = under && !under->chrome() ? hints(keys) : 0;
    Rect r = drawSheet(c, body, kSheetMinH, keys, keyCount);

    const Rect head = r.takeTop(kSectionH);
    const std::string err = attemptError();
    if (!err.empty()) {
      drawNotice(c, head, err);
    } else {
      drawSectionHeader(c, head, headText(), "VOL " + std::to_string(audio::status().volume),
                        {true});
    }
    drawStateRow(c, r.takeTop(kStateH), word(), meter());
    r.takeTop(drawNameBlock(c, r, label(), name(), line(), line2()) - r.y);
    drawEarlier(c, r, earlier(),
                conv_.on && conv_.in ? "CLOSES 10 S AFTER THE LAST SOUND; YOU STAY IN IT"
                                     : "CLOSES 10 S AFTER THE LAST SOUND");
  }

 private:
  // Where the mic goes from here: all of the group, or the board the session came from for a mic
  // to this board alone (to a board in the conversation is to all of it: the engine).
  uint32_t micTarget() const { return toAll_ ? 0 : peer_; }

  void follow(const comms::OpenMic &m) {
    group_ = m.group;
    peer_ = m.id;
    peerName_ = m.name;
    toAll_ = m.target == 0;
    waiting_ = false;
    label_ = "FROM";
    how_ = "";
  }

  void load() {
    mics_ = comms::openMics(group_);
    mic_ = comms::mic();
    conv_ = comms::conversation(group_);
    calls_ = comms::calls(group_, 12);
    if (const comms::OpenMic *m = findMic(mics_, group_, peer_)) {
      peerMic_ = *m;
      havePeerMic_ = true;
    } else {
      havePeerMic_ = false;
    }
    if (const comms::Board *b = findBoard(comms::boards(group_), peer_)) board_ = *b;
  }

  bool hearingPeer() const { return havePeerMic_ && peerMic_.playing; }
  bool anyPlaying() const {
    for (const comms::OpenMic &m : mics_) {
      if (m.playing) return true;
    }
    return false;
  }

  // The dialer's: its ring answered (label ANSWERED BY).
  bool dialer() const { return std::string(label_) == "ANSWERED BY"; }
  std::string headText() const {
    std::string to = toAll_ ? "TO ALL" : dialer() ? "TO " + name() : "TO YOU";
    if (conv_.on && conv_.in) to += kDot + "CONVERSATION";
    return "COMMS" + kDot + groupName(group_) + kDot + to;
  }
  // HEARING while a voice plays, MIC ON while this board's goes, else QUIET.
  std::string word() const {
    if (anyPlaying()) return "HEARING";
    if (comms::micEnabled()) return "MIC ON";
    return "QUIET";
  }
  int meter() const {
    if (anyPlaying()) return meterFill(comms::status().playLevelDb);
    if (mic_.sending) return meterFill(mic_.levelDb);
    return 0;
  }
  std::string label() const { return peer_ ? label_ : "WITH"; }
  std::string name() const {
    if (peer_) return boardName(peerName_.empty() ? peerMic_.name : peerName_, peer_);
    const std::string others = othersText(conv_);
    return others.empty() ? "ALL OF " + groupName(group_) : others;
  }
  // How it comes, and its mic: ON AIR · −52 dBm · 0:04 SO FAR (seconds while it sounds, whole
  // minutes otherwise).
  std::string line() const {
    if (!peer_) return how_;
    std::string reach = reachText(peer_, board_.route, board_.hops, board_.relayName, board_.rssi);
    if (waiting_) {
      // The dialer's "● MIC ON TO TALK" goes once its mic is on (line2 says YOUR MIC ON).
      std::string how = how_;
      if (dialer() && comms::micEnabled()) how = "ANSWERED";
      return how + (reach.empty() ? "" : kDot + reach);
    }
    std::string s = reach.empty() ? "" : reach + kDot;
    if (!havePeerMic_) {
      const comms::Call *c = shownCall();
      return s + (c ? "ITS MIC WAS ON " + lengthText(c->ms) : std::string("ITS MIC IS OFF"));
    }
    if (peerMic_.link == comms::Link::Lost) return s + "LOST " + lostText(peerMic_.linkMs);
    if (hearingPeer()) return s + "SOUND " + lengthText(peerMic_.linkMs) + " SO FAR";
    return s + "MIC ON " + minutesText(peerMic_.onS * 1000) + kDot + "QUIET";
  }
  std::string line2() const {
    std::string s;
    if (conv_.on) s = membersText(conv_) + kDot + forText(conv_.ms);
    if (comms::micEnabled()) s += (s.empty() ? "" : kDot) + std::string("YOUR MIC ON");
    return s;
  }
  // The session the sheet is about, in the log (left out of EARLIER).
  const comms::Call *shownCall() const {
    for (const comms::Call &k : calls_) {
      if (k.kind == comms::Kind::Voice && !k.outgoing && k.from == peer_) return &k;
    }
    return nullptr;
  }
  std::vector<comms::Call> earlier() const {
    int skip = -1;
    const comms::Call *s = waiting_ ? nullptr : shownCall();
    for (size_t i = 0; s && i < calls_.size(); ++i) {
      if (&calls_[i] == s) skip = static_cast<int>(i);
    }
    return earlierCalls(calls_, skip);
  }

  std::string key() const {
    return headText() + "|" + attemptError() + "|" + std::to_string(audio::status().volume) +
           "|" + word() + std::to_string(meter()) + "|" + label() + name() + "|" + line() + "|" +
           line2() + "|" + (playing() ? "p" : "") + (conv_.on && conv_.in ? "c" : "") +
           (pushToTalk() ? "t" : "") + "\n" +
           earlierKey(earlier());
  }

  uint32_t rose_ = 0;  // it rose by itself (a session's first sound); 0 when a key brought it
  uint32_t incoming_ = 0;
  int group_ = 0;
  uint32_t peer_ = 0;
  std::string peerName_;
  bool toAll_ = true;
  bool waiting_ = false;  // a conversation answered or joined, before its first sound here
  const char *label_ = "FROM";
  const char *how_ = "";  // then: "ANSWERED", "JOINED"
  std::vector<comms::OpenMic> mics_;
  comms::OpenMic peerMic_;
  bool havePeerMic_ = false;
  comms::Board board_;
  comms::Mic mic_;
  comms::Conversation conv_;
  std::vector<comms::Call> calls_;
  uint32_t activeMs_ = 0;
  std::string drawn_;
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
  int hints(ui::Hint *out) const override {
    out[0] = {ui::glyph::kPress, "JOIN"};
    out[1] = {"BOOT", playing() ? "STOP" : "LATER"};
    return 2;
  }

  void onEnter() override { entered(this, Kind::Conversation); }
  void onLeave() override { left(this); }

  bool onKey(Key key) override {
    sinceMs_ = millis();
    switch (key) {
      case Key::Select:
        if (comms::join(group_)) {
          toConversation(group_, 0, true, "WITH", "JOINED");
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
    if (conv_.in) {  // joined by itself (it heard a mic open): nothing left to ask
      ui::nav::pop();
      return false;
    }
    if (millis() - sinceMs_ > kConvSheetMs && input::idleMs() > kConvSheetMs) {
      ui::nav::pop();
      return false;
    }
    return key() != drawn_;
  }

  void draw(ui::Canvas &c, const Rect &body) override {
    using namespace ui;
    drawn_ = key();
    Hint keys[kMaxHints];
    const Screen *under = nav::base();
    const int keyCount = under && !under->chrome() ? hints(keys) : 0;
    Rect r = drawSheet(c, body, kSheetMinH, keys, keyCount);
    const std::string group = groupName(group_);
    const Rect head = r.takeTop(kSectionH);
    const std::string err = attemptError();
    if (!err.empty()) {
      drawNotice(c, head, err);
    } else {
      drawSectionHeader(c, head, "COMMS" + kDot + group + kDot + "CONVERSATION ON",
                        boardsText(static_cast<int>(conv_.members.size())), {true});
    }
    drawStateRow(c, r.takeTop(kStateH), "CONVERSATION", -1, forText(conv_.ms));
    r.takeTop(drawNameBlock(c, r, "IN IT", othersText(conv_), line(),
                            conv_.micsOn ? std::to_string(conv_.micsOn) +
                                               (conv_.micsOn == 1 ? " MIC ON IN IT" : " MICS ON IN IT")
                                         : std::string()) -
              r.y);
    drawEarlier(c, r, earlierCalls(comms::calls(group_, 12)), "CLOSES BY ITSELF IN 30 S");
  }

 private:
  std::string line() const {
    return conv_.left ? "YOU LEFT IT: ITS VOICE DOESN'T PLAY HERE"
                      : "ITS VOICE PLAYS HERE ONCE YOU JOIN";
  }
  std::string key() const {
    return attemptError() + "|" + forText(conv_.ms) + "|" + othersText(conv_) + "|" + line() +
           std::to_string(conv_.micsOn) + (playing() ? "p" : "") + "\n" +
           earlierKey(earlierCalls(comms::calls(group_, 12)));
  }

  int group_;
  uint32_t sinceMs_;  // it rose, or the last key
  comms::Conversation conv_;
  std::string drawn_;
};

// --- The ring sheet -----------------------------------------------------------------------------

class RingSheet : public ui::Screen {
 public:
  RingSheet() : rose_(millis()) {
    comms::hold();
    follow(comms::incomingRing());
  }
  ~RingSheet() override { comms::release(); }

  std::string tag() const override { return {}; }
  bool overlay() const override { return true; }
  int hints(ui::Hint *out) const override {
    int n = 0;
    if (ringing_) {
      if (pushToTalk()) out[n++] = micHint();  // HOLD BOOT TALK: answers, and talks while held
      out[n++] = {ui::glyph::kPress, "ANSWER"};
      out[n++] = {"BOOT", "IGNORE"};
      return n;
    }
    if (msg_.id && !msg_.receiving) {
      out[n++] = {ui::glyph::kPress, playingThis() ? "STOP" : "PLAY"};
    }
    out[n++] = {"BOOT", playing() ? "STOP" : "CLOSE"};
    return n;
  }

  // Push to talk, while it rings: HOLD BOOT answers and keys the mic while held (to the dialer
  // alone for a ring to this board alone), wherever the sheet rose; then it's the call sheet.
  // Sticky, or once the ring is over: the default (the agent's).
  ui::TalkTo talkTo() const override {
    return ringing_ ? ringTalkTo(group_, toAll_ ? 0 : from_) : ui::TalkTo();
  }

  void onEnter() override { entered(this, Kind::Ring); }
  void onLeave() override { left(this); }

  bool onKey(Key key) override {
    keyMs_ = millis();
    switch (key) {
      case Key::Select:
        if (ringing_) {
          // Sticky: answered to talk, this board's mic goes on in the conversation the answer
          // opens (to the dialer alone for a ring to this board alone; the MIC mark shows it).
          // Push: it stays off, and HOLD BOOT talks on the call sheet. Not in the sheet's first
          // second (a press meant for the screen under it).
          if (millis() - rose_ < kRiseGuardMs) return true;
          if (comms::answer()) {
            noteAttempt();
            micAnswered(group_, toAll_ ? 0 : from_);
            toConversation(group_, from_, toAll_, "FROM", "ANSWERED");
          }
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
      rose_ = now;
      ui::nav::talkChanged();  // HOLD BOOT's target with it (push)
      return true;
    }
    ring_ = rg;
    if (ringing_ && !rg.on) {
      ringing_ = false;
      endedMs_ = now;
      ui::nav::talkChanged();  // HOLD BOOT is the agent's again
    }
    if (!ringing_) {
      switch (rg.ended) {
        case comms::RingEnd::Answered:  // here, another way in (!commsanswer)
          toConversation(group_, from_, toAll_, "FROM", "ANSWERED");
          return false;
        case comms::RingEnd::AnsweredThere: {  // the conversation is on without this board
          const ui::Screen *base = ui::nav::base();
          if (base && base->tag() == moduleTag("COMMS")) {
            ui::nav::pop();  // the talk screen shows it
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
    }
    return key() != drawn_;
  }

  void draw(ui::Canvas &c, const Rect &body) override {
    using namespace ui;
    drawn_ = key();
    Hint keys[kMaxHints];
    const Screen *under = nav::base();
    const int keyCount = under && !under->chrome() ? hints(keys) : 0;
    Rect r = drawSheet(c, body, kSheetMinH, keys, keyCount);
    const std::string group = groupName(group_);
    const Rect head = r.takeTop(kSectionH);
    const std::string err = attemptError();
    if (!err.empty()) {
      drawNotice(c, head, err);
    } else {
      drawSectionHeader(c, head,
                        "COMMS" + kDot + group + kDot + (toAll_ ? "RINGS ALL" : "RINGS YOU"), "",
                        {true});
    }
    std::string word, right, line;
    stateTexts(word, right, line);
    drawStateRow(c, r.takeTop(kStateH), word, -1, right);
    r.takeTop(drawNameBlock(c, r, "FROM", boardName(ring_.fromName, from_), line) - r.y);
    drawEarlier(c, r, earlier(),
                ringing_ ? "IT RINGS UNTIL SOMEONE ANSWERS, FOR 1:00 AT MOST"
                : ring_.ended == comms::RingEnd::Message && msg_.id && !msg_.receiving
                    ? "KEPT AS NEW UNTIL PLAYED"
                    : "CLOSES IN A FEW SECONDS");
  }

 private:
  // The state: RINGING, MISSED, MESSAGE COMING, LEFT A MESSAGE or PLAYING, with its seconds.
  void stateTexts(std::string &word, std::string &right, std::string &line) const {
    const std::string rang = "RANG " + lengthText(ring_.ms);
    const std::string group = groupName(group_);
    if (ringing_) {
      word = "RINGING";
      right = lengthText(ring_.ms);
      std::string reach;
      if (const comms::Board *b = findBoard(comms::boards(group_), from_)) {
        reach = reachText(b->id, b->route, b->hops, b->relayName, b->rssi);
      }
      line = (reach.empty() ? "" : reach + kDot) + (toAll_ ? "TO ALL OF " + group : "TO YOU");
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
  }

  // EARLIER, without this ring and the message left for it (the block above says them).
  std::vector<comms::Call> earlier() const {
    std::vector<comms::Call> out;
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
      if (!k.live) out.push_back(k);
    }
    return out;
  }

  std::string key() const {
    std::string word, right, line;
    stateTexts(word, right, line);
    return attemptError() + "|" + word + "|" + right + "|" + line + "|" +
           boardName(ring_.fromName, from_) + (ringing_ ? "r" : "") + (playing() ? "p" : "") +
           (pushToTalk() ? "t" : "") +
           (msg_.id && !msg_.receiving ? "m" : "") + "\n" + earlierKey(earlier());
  }

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

  uint32_t rose_ = 0;  // it rose, or followed another ring
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
  std::string drawn_;
};

// --- The LOST sheet -----------------------------------------------------------------------------

// A mic heard here went silent without its off: whose, for how long, ● SILENCE (the alarm stops
// on every mic), BOOT closes and silences. Its mic heard again: BACK, then it closes; its mic
// gone (the off came after all, or the conversation is over here): OFF, then it closes.
class LostSheet : public ui::Screen {
 public:
  LostSheet() {
    comms::hold();
    mic_ = lostMic;
    load();
  }
  ~LostSheet() override { comms::release(); }

  std::string tag() const override { return {}; }
  bool overlay() const override { return true; }
  int hints(ui::Hint *out) const override {
    int n = 0;
    if (comms::status().alarm) out[n++] = {ui::glyph::kPress, "SILENCE"};
    out[n++] = {"BOOT", "CLOSE"};
    return n;
  }

  void onEnter() override { entered(this, Kind::Lost); }
  void onLeave() override { left(this); }

  bool onKey(Key key) override {
    keyMs_ = millis();
    switch (key) {
      case Key::Select: comms::ackLost(); return true;
      case Key::Back:
        comms::ackLost();
        return false;  // closes
      default: return false;
    }
  }

  bool onTick() override {
    load();
    const uint32_t now = millis();
    if (lost()) {
      endedMs_ = 0;
    } else {
      if (!endedMs_) endedMs_ = now;
      if (now - std::max(endedMs_, keyMs_) > kBackShowMs && input::idleMs() > kBackShowMs) {
        ui::nav::pop();
        return false;
      }
    }
    return key() != drawn_;
  }

  void draw(ui::Canvas &c, const Rect &body) override {
    using namespace ui;
    drawn_ = key();
    Hint keys[kMaxHints];
    const Screen *under = nav::base();
    const int keyCount = under && !under->chrome() ? hints(keys) : 0;
    Rect r = drawSheet(c, body, kSheetMinH, keys, keyCount);
    const bool alarm = comms::status().alarm;
    drawSectionHeader(c, r.takeTop(kSectionH),
                      "COMMS" + kDot + groupName(mic_.group) + kDot + "LOST",
                      alarm ? "ALARM" : lost() ? "SILENCED" : word(), {true});
    drawStateRow(c, r.takeTop(kStateH), word(), -1, right());
    r.takeTop(drawNameBlock(c, r, "MIC", boardName(mic_.name, mic_.id), line(), line2()) - r.y);
    drawEarlier(c, r, earlierCalls(comms::calls(mic_.group, 12)), foot());
  }

 private:
  void load() {
    const std::vector<comms::OpenMic> ms = comms::openMics(mic_.group);
    const comms::OpenMic *m = findMic(ms, mic_.group, mic_.id);
    there_ = m != nullptr;
    if (m) mic_ = *m;
  }
  bool lost() const { return there_ && mic_.link == comms::Link::Lost; }
  std::string word() const { return lost() ? "LOST" : there_ ? "BACK" : "OFF"; }
  std::string right() const { return lost() ? lostText(mic_.linkMs) : ""; }
  std::string line() const {
    if (lost()) {
      return "NOT HEARD FOR " + lostText(mic_.linkMs) + kDot + "ITS MIC WAS ON " +
             minutesText(mic_.onS * 1000);
    }
    if (there_) return "HEARD AGAIN" + kDot + "ITS MIC ON " + minutesText(mic_.onS * 1000);
    return "ITS MIC IS OFF NOW";
  }
  std::string line2() const {
    std::string s = mic_.usb ? std::string("ON USB")
                             : mic_.battery >= 0 ? "BATTERY " + std::to_string(mic_.battery) + "%"
                                                 : std::string();
    const std::string reach = reachText(mic_.id, mic_.route, mic_.hops, "", mic_.rssi);
    if (!reach.empty()) s = (lost() ? "LAST " : "") + reach + (s.empty() ? "" : kDot + s);
    return s;
  }
  std::string foot() const {
    if (comms::status().alarm) return "THE ALARM SOUNDS UNTIL YOU SILENCE IT";
    return lost() ? "SILENCED; LOST STAYS UNTIL IT'S HEARD AGAIN" : "CLOSES IN A FEW SECONDS";
  }
  std::string key() const {
    return std::string(comms::status().alarm ? "a" : "") + "|" + groupName(mic_.group) + "|" +
           word() + "|" + right() + "|" + boardName(mic_.name, mic_.id) + "|" + line() + "|" +
           line2() + "|" + foot() + "\n" + earlierKey(earlierCalls(comms::calls(mic_.group, 12)));
  }

  comms::OpenMic mic_;
  bool there_ = false;
  uint32_t endedMs_ = 0;  // LOST no more (back, or gone)
  uint32_t keyMs_ = 0;
  std::string drawn_;
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

// Not over the lock, an off face (but one woken for comms) or the desk face, the Recorder or a ringing alarm, the agent
// sheet while it's busy; nor, unless overComms, over the Comms screen (it shows calls and
// conversations in place).
bool mayRise(bool overComms) {
  const ui::Screen *top = ui::nav::top();
  const ui::Screen *base = ui::nav::base();
  if (!top || !base || ui::nav::locked()) return false;
  // Over an off face only while it holds for a Comms wake (power.h, reachable on battery).
  if (top->offFace() && power::reachHold() != power::Held::Stay) return false;
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

ui::MicMark micMarkNow() { return micMark(); }

// A ring that rings here with no sound (silent: audio.h): the status bar's RING, so it shows where
// its sheet can't rise.
bool silentRinging() { return audio::silent() && comms::incomingRing().on; }

// LOST's alarm where its sheet isn't on top (it couldn't rise there: the lock, a face, the
// Recorder...): the first key silences it, and does nothing else.
bool lostKeyFirst(Key) {
  if (sheetOnTop(Kind::Lost) || !comms::status().alarm) return false;
  log_i("comms: a key silenced LOST's alarm");
  comms::ackLost();
  return true;
}

}  // namespace

std::unique_ptr<ui::Screen> commsui::makeConversationCallSheet(int group, uint32_t peer,
                                                               bool toAll, const char *label,
                                                               const char *how) {
  return std::make_unique<CallSheet>(group, peer, toAll, label, how);
}

void pollComms() {
  static bool primed = false;
  static uint32_t seenIncoming = 0;
  static uint32_t raisedRing = 0;  // incomingRing().count the ring sheet rose for
  static bool alarmWas = false;
  const comms::Status s = comms::status();
  if (!primed) {
    primed = true;
    seenIncoming = s.incoming;
    convSeen = s.conversationsSeen;
    ui::nav::onConversation(conversationMark);
    ui::nav::onMic(micMarkNow);
    ui::nav::onRing(silentRinging);
    ui::nav::onKeyFirst(lostKeyFirst);
    return;
  }

  // The status bar's RING comes and goes with a silent ring: the screen under it redraws.
  static bool ringMarkWas = false;
  if (silentRinging() != ringMarkWas) {
    ringMarkWas = !ringMarkWas;
    ui::nav::invalidate();
  }

  // The mic's mode changed (the dev override, at run time): HOLD BOOT's targets change with it.
  static bool pushWas = pushToTalk();
  if (pushToTalk() != pushWas) {
    pushWas = !pushWas;
    ui::nav::talkChanged();
    ui::nav::invalidate();  // the hints
  }

  // LOST's alarm starts: its sheet, over the Comms screen too, at once or as soon as it may.
  if (s.alarm && !alarmWas) {
    for (const comms::OpenMic &m : comms::openMics(-1)) {
      if (m.alarm) {
        lostMic = m;
        lostPending = true;
        break;
      }
    }
  }
  alarmWas = s.alarm;
  if (lostPending && !sheetOnTop(Kind::Lost) && mayRise(true)) {
    lostPending = false;
    raise(std::make_unique<LostSheet>());
  }

  // A ring: the sheet rises at once (over the Comms screen too: it has no ANSWER of its own), or
  // as soon as it may while it still rings. Where it may not, the board rings by sound alone.
  const comms::Ring rg = comms::incomingRing();
  if (rg.on && rg.count != raisedRing && mayRise(true) && !sheetOnTop(Kind::Lost)) {
    raisedRing = rg.count;
    if (!sheetOnTop(Kind::Ring)) raise(std::make_unique<RingSheet>());  // else it follows
  }

  // A mic session starts playing here (once a session).
  if (s.incoming != seenIncoming) {
    seenIncoming = s.incoming;
    if (s.hearing && !sheetOnTop(Kind::Call) && !sheetOnTop(Kind::Ring) &&
        !sheetOnTop(Kind::Lost) && mayRise(false)) {
      const std::vector<comms::OpenMic> mics = comms::openMics(-1);
      if (const comms::OpenMic *m = newestPlaying(mics)) raise(std::make_unique<CallSheet>(*m));
    }
  }

  // A conversation this board isn't in: once, when it may (not straight after a key, nor over
  // another sheet or menu); dropped if it ends, this board joins, or the Comms screen shows it.
  static uint32_t convPendingMs = 0;
  if (s.conversationsSeen != convSeen) {
    convSeen = s.conversationsSeen;
    convPending = true;
    convPendingGroup = s.conversationGroup;
    convPendingMs = millis();
  }
  if (convPending) {
    const comms::Conversation c = comms::conversation(convPendingGroup);
    const ui::Screen *top = ui::nav::top();
    const ui::Screen *base = ui::nav::base();
    if (!c.on || c.in || c.left || c.id == convShownId) {
      convPending = false;
    } else if (base && base->tag() == moduleTag("COMMS")) {
      convPending = false;  // the talk screen shows it
    } else if (top && !top->overlay() && input::idleMs() >= kQuietKeyMs &&
               millis() - convPendingMs >= kConvSettleMs && mayRise(false)) {
      convPending = false;
      raise(std::make_unique<ConversationSheet>(convPendingGroup));
    }
  }
}

}  // namespace apps

#endif  // INKDECK_COMMS
