# Comms protocol

How INKDECK boards talk to each other over the radio (COMMS, issue #66): push-to-talk voice in
groups, rings, conversations, messages, the baby monitor, and relaying for each other as a mesh.
The board's side is `src/comms.h` and `src/comms.cpp`; the packet format is `src/comms_wire.h`,
conversations and the message reassembly `src/comms_calls.h`, the baby monitor's packets and state
`src/comms_monitor.h`, the keys `src/comms_crypto.h`, the codec `src/adpcm.h`, messages on the
card `src/comms_store.h`. Keep this file and those in step. Version 1, written 2026-11-22; rings,
conversations and messages added the same day, the baby monitor (type 15) on 2026-11-23, all
without a new version number ([Versions](#versions)).

Nothing else speaks it yet. The link through inkharness (phases 5 and 6 of the issue) will carry
these same packets, still sealed with the group's key, so a daemon or a hub passes them on
without reading them.

## Transport

[ESP-NOW](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-reference/network/esp_now.html)
broadcasts (to `FF:FF:FF:FF:FF:FF`), board to board, no router, no ESP-NOW encryption (it allows
7 peers at most, and none for broadcasts). Every frame is at most **250 bytes**, ESP-NOW v1's
limit, which v2 boards take too. ESP-NOW's own retries don't apply to broadcasts: a frame is sent
once, and the air loses some (about 4% of voice frames between two boards at −76 dBm on a busy
channel, measured 2026-11-22).

The radio is on one Wi-Fi channel at a time: the network's while the board is joined to one, else
`mesh_channel` (`SD:/inkdeck.ini`, 6 by default). Boards on different channels don't hear each
other. Receiving needs Wi-Fi's modem sleep off.

## Frame

Multi-byte numbers are little-endian, except where noted.

| Bytes | Field | |
| --- | --- | --- |
| 1 | magic | `0xC7` |
| 1 | version, type | version (`1`) in the high 4 bits, the type in the low 4 |
| 1 | hops, ttl | relays passed in the high 4 bits, relays left in the low 4. A relay subtracts 1 from ttl and adds 1 to hops; it sends nothing on with ttl 0 |
| 1 | reserved | `0` |
| 4 | group id | which group's key seals it ([Keys](#keys)) |
| 4 | sender | the board's id: random, made once, kept in its NVS; never 0 |
| 4 | boot | the sender's boot number: counts its starts, kept beside its id |
| 4 | seq | the sender's packet counter this boot, from 1, across all its groups |
| n | body | AES-256-GCM ciphertext of the body ([Bodies](#bodies)) |
| 16 | tag | AES-GCM's tag |

The header is 20 bytes. A receiver drops a frame that is shorter than 36 bytes or longer than
250, has another magic, a version it doesn't know (a later version changes the number), a nonzero
reserved byte, type 0, or sender 0. A type it doesn't know it opens and passes on like any other
packet ([Taking a packet](#taking-a-packet)), without reading it, so a later type crosses boards
that don't know it; since type 15 took the last number, later types are ext types
([Type 15](#type-15-extended)), relayed the same way.

**Sealing.** The key is the group's. The 12-byte nonce is sender, boot and seq, 4 bytes each,
little-endian, as in the header. The associated data is the 20-byte header with its hops/ttl byte
set to 0, so a relay can count hops without the key and everything else in the header is
authenticated. The nonce never repeats: the boot number goes up at every start (if NVS won't keep
it, the board uses a random one with the top bit set), and a board whose NVS is erased also gets
a new id.

| Type | Name | |
| --- | --- | --- |
| `1` | presence | "here": name, flags, battery; a hello asks the others to answer |
| `2` | voice start | a call begins |
| `3` | voice | 20 ms of the call |
| `4` | voice end | a call ended |
| `5` | ping | `!commsping` |
| `6` | pong | the answer to a ping |
| `7` | heard | a call played here |
| `8` | ring | a dialer rings all of a group or one board of it, about once a second |
| `9` | ringing | a board rings for a ring (or has stopped): the dialer counts them |
| `10` | answer | a ring answered: it floods, so every board it rang stops |
| `11` | ring end | the dialer gave up, rang 60 s, or leaves a message instead |
| `12` | message | a numbered 20 ms frame of a message |
| `13` | message end | a message's frame count and checksum, or that it was dropped |
| `14` | message ask | a board asks for the frames of a message it missed, or has them all |
| `15` | extended | the last 4-bit type: the body's byte after sentAt is a second type ([Type 15](#type-15-extended)): the baby monitor's packets |

## Bodies

Every body starts with **sentAt** (4 bytes): the sender's clock, UTC seconds, or 0 if it isn't
set. Then, by type:

**Presence** (1): flags (1: `0x01` on USB power, `0x02` relays for others, `0x04` monitoring its
room, `0x80` hello: answer with your presence), battery (1: percent, `0xFF` none), name length
(1, up to 16), name (ASCII, upper case), then the conversation the board is in: its id (4, `0` for
none), and only if it isn't 0: on for (2, seconds), quiet for (2, seconds since its last word),
member count (1, up to 8), members (4 each: the other boards in it that this one has heard say so
themselves). Each board sends one in each of its groups when it starts listening (with the hello
flag), then every 30 s (plus up to 3 s at random), every 4 s (plus up to 1 s) while it is in the
group's conversation, and within 0.1 s of joining or leaving one. A board that hears a hello
answers within 0.1 to 0.6 s, at most once in 5 s. A board heard from in no packet for 95 s is
away. A presence that ends after the name is a board from before conversations: it says nothing
about them.

**Voice start** (2): target (4: 0 for all of the group, else the one board's id), call (2: the
caller's call counter), codec (1: `1` is the one below), conversation (4), voice flags (1: `0x01`
this call opened its conversation).

**Voice** (3): target (4), call (2), frame (2: 0, 1, 2... within the call), predictor (2, signed),
step index (1, 0 to 88), byte count (1, up to 160), then the IMA ADPCM bytes, then conversation (4)
and voice flags (1). 64 kbps: 16 kHz mono, two samples a byte, the first in the low nibble, the
standard IMA step and index tables; 160 bytes are 320 samples, 20 ms. Predictor and step index are
the encoder's state before the first sample, so each frame decodes on its own: a lost frame is a
20 ms gap. A voice frame is 217 bytes on the air (212 from a board before conversations, which
sends neither of the last two fields: its call plays as it always did, outside any conversation).
Only the last frame of a call may be short.

**Voice end** (4): target (4), call (2), frames (2: how many voice frames the call had),
conversation (4), voice flags (1). Sent three times, 40 ms apart (three packets, three sequence
numbers).

**Ping** (5): ping id (4), the pinger's millisecond clock (4). Every board that opens it answers.

**Pong** (6): to (4: the pinger), ping id (4), the clock value echoed (4), RSSI of the ping as
received (1, signed dBm; 0 if it came through relays), hops the ping had passed (1), held (2: the
milliseconds the answer was held back, a random 5 to 155, so boards in range don't all answer at
once). The round trip less `held` is the time on the air and in the two boards.

**Heard** (7): caller (4), call (2). Sent once, 50 to 250 ms after a call addressed to all or to
this board played out of the speaker; the caller counts the boards that sent one.

**Ring** (8): target (4: 0 for all of the group, else the one board), ring (2: the dialer's ring
counter, which starts at a random number each boot), conversation (4: the one an answer puts the
boards in: the group's conversation if one is on, else a new random id), rung (4: milliseconds
since the ring began). Sent every second until it ends.

**Ringing** (9): dialer (4), ring (2), state (1: `1` ringing, `2` stopped here: ignored). Sent 20
to 200 ms after each ring packet a board rings for, and once when it's ignored there.

**Answer** (10): dialer (4), ring (2), conversation (4: the ring's). Sent once by the board that
answers, and again for each ring packet that still comes (the dialer missed it).

**Ring end** (11): target (4), ring (2), reason (1: `1` given up, `2` 60 s with no answer, `3` a
message follows). Sent three times, 40 ms apart.

**Message** (12): message (4: a random id), to (4: 0 for all of the group, else the one board),
frame (2: from 0), predictor (2, signed), step index (1), byte count (1, up to 160), then the IMA
ADPCM bytes, as a voice frame's. 214 bytes on the air at most.

**Message end** (13): message (4), to (4), frames (2: how many), checksum (4: FNV-1a, 32-bit,
over the frames as stored, below), flags (1: `0x01` dropped: the receivers drop what came), ring
(2: the ring it follows, 0 if none). Sent three times, 40 ms apart, and again after each round of
frames sent again.

**Message ask** (14): sender (4: the message's), message (4), flags (1: `0x01` I have it all,
`0x02` I have no end packet yet), frames held (2), from (2: the first frame the map covers), span
(2: how many frames it covers), then the map: a bit a frame, frame `from` in the lowest bit of the
first byte, set for a frame missing. Up to 195 bytes of map, 1,560 frames: the ask is then 250
bytes. "I have it all" has span 0.

A body that ends before its fields do is dropped. Bytes after the fields are ignored, so a later
version can add fields at the end without a new version number.

### Type 15, extended

The type is 4 bits, and 15 is the last number, so type 15's body says its own type: sentAt (4),
the **ext type** (1), then the fields below. A board opens and relays an ext type it doesn't know,
as it does any packet ([Taking a packet](#taking-a-packet)). All six are the baby monitor's
([Baby monitor](#baby-monitor)). A level is a byte: how many dB the loudest sample of a 20 ms
frame is below full scale, 0 to 96 (96: silence or quieter), `0xFF` for none. A listener list is
a count (1, up to 8; `0`: all of the group) and that many board ids (4 each).

| Ext | Name | |
| --- | --- | --- |
| `1` | monitor audio | 20 ms of the room's sound, to its listeners |
| `2` | still here | the room is on: every 2 s |
| `3` | monitor off | the room stopped |
| `4` | listening | a listener says it listens to the room, about every 4 s (or has stopped) |
| `5` | history ask | a listener asks the room for its 2 hours of levels |
| `6` | history | the room's levels, to the listener that asked |

**Monitor audio** (1): listeners (the list), sound (2: the room's counter, one more each sound),
frame (2: within the sound, from 0), level (1: this frame's, as the room measured it before
encoding), predictor (2, signed), step index (1), byte count (1, up to 160), then the IMA ADPCM
bytes, as a voice frame's. 243 bytes on the air with 8 listeners and a full frame, 211 to all of
the group. Each frame is sent half a second after it was captured ([the gate](#baby-monitor)).

**Still here** (2): flags (1: `0x01` sending a sound now, `0x02` on USB power, `0x04` the mic is
off: a talk back plays, or an alarm, the Recorder, music or the agent has it), battery (1:
percent, `0xFF` none), threshold (1: dB below full scale, 0 to 60), peak (1: the loudest frame
since the last still-here, a level), on for (4: seconds since it started), bar (2: the 2-minute
bar now, numbered from the start), bar peak (1: that bar's loudest frame so far, a level), pre-roll
(1: in 20 ms frames, 25), listeners (the list). Sent every 2 s (plus up to 0.2 s), and at once
when the threshold changes. 86 bytes on the air with 8 listeners.

**Monitor off** (3): reason (1: `1` stopped, `2` the board is powering off). Sent three times,
40 ms apart (at once, back to back, for a power-off).

**Listening** (4): room (4: the room's id), flags (1: `0x01` stopped listening). Sent when a board
starts listening and every 4 s (plus up to 0.5 s) after, and once with `0x01` when it stops.

**History ask** (5): room (4). Sent when a board starts listening and again 2 s apart until the
history comes (three at most), and again once the link is back after NO LINK.

**History** (6): to (4: the listener that asked), first bar (2), count (1, up to 60), then that
many levels, the bars first to first + count - 1, the last the bar now. 108 bytes on the air with
60 bars.

## Keys

Per group, HKDF-SHA256 (RFC 5869) from the secret: the group line's secret as UTF-8 bytes as the
input keying material, one salt, and one `info` label per output. Each output is the first bytes
of the first expand block.

| Output | info | Bytes |
| --- | --- | --- |
| AES-256-GCM key | `key` | 32 |
| group id | `group id` | 4 |
| rendezvous id | `rendezvous id` | 16 |

The salt is `inkdeck comms 1` for a `mesh_group` line. The default group, which a board with no
`mesh_group` line but an `agent_token` is in, takes the token as its secret and the salt
`inkdeck comms 1 agent_token`: its key is neither the agent link's (`docs/voice-protocol.md`) nor
any `mesh_group` line's. The group id goes in the header as the little-endian number whose
big-endian bytes HKDF gave, so its 4 hex digits on screen (`7F3A`) are HKDF's first two bytes. The
rendezvous id is for daemons to meet at a hub; nothing uses it yet.

Every member holds the key, so any member can pose as any other, and removing a board means a new
secret for every other one. Outsiders see that encrypted ESP-NOW traffic flows, from which MAC
address, of what type and size; not what it says or which board of the group sent it, beyond the
id.

Reference values (`!commstest` checks them):

- secret `correct horse battery staple`: key `304cb698315df95320f1c497cdc6a392940c46af8009bb2883438fa2568baf2f`,
  group id `8e363ee1` (tag `8E36`), rendezvous id `9ad83e8a1a71e3ed30be224b4403b778`. As the
  agent_token group: id `098b2a7b`.
- In that group, a presence from sender `11223344`, boot 7, seq 1, ttl 3, sentAt 0, flags 3,
  battery 87, name `KITCHEN`, is the 50-byte frame
  `c7110300e13e368e443322110700000001000000496daff1a20747187f7b647e498edf8dee5b8ed3d6177c50ccef8ca429a1`.
- In that group, a ring from sender `11223344`, boot 7, seq 2, ttl 3, sentAt 0, to all, ring 9,
  conversation `a1b2c3d4`, rung 4200 ms, is the 54-byte frame
  `c7180300e13e368e44332211070000000200000095184362b8f980d1bcabd37b688b9e0f2160c1a57c8f182d6add0b5f6239176f7ac4`.
- In that group, a still-here from sender `11223344`, boot 7, seq 3, ttl 3, sentAt 0: flags 3
  (sending, on USB), no battery, threshold 30, peak 24, on for 600 s, bar 5, bar peak 18,
  pre-roll 25, for board `55667788` alone (body `000000000203ff1e1858020000050012190188776655`),
  is the 58-byte frame
  `c71f0300e13e368e443322110700000003000000e74688b9939c604d7e97b0aa142e8a21bd52a8396506401237bb8bb8155b460b4a4194992ab6`.
- In that group, monitor audio from sender `11223344`, boot 7, seq 4, ttl 3, sentAt 0: to all,
  sound 2, frame 9, level 21, predictor -1234, step index 40, the 8 bytes `0102030405060708`
  (body `00000000010002000900152efb28080102030405060708`), is the 59-byte frame
  `c71f0300e13e368e443322110700000004000000a8c23f21808172d6dd8b0b0da60caee3a3b1e53cd7201641f9f72080f0d7effe32e380e4eaf1e9`.

## Taking a packet

In this order:

1. **The header.** A frame that doesn't parse, or from this board's own id (its own packet, relayed
   back), is dropped.
2. **Another group's.** A board passes on a packet whose group it isn't in by the header alone,
   unread: once (by the seen-cache below), if ttl isn't 0 and RELAY FOR OTHERS is on.
3. **Seen before.** The seen-cache holds the last 256 packets taken, by group, sender, boot and seq.
   The mesh floods, so a packet comes once from each relay in range: only the first is handled.
4. **It opens** with the group's key, or is dropped (and not relayed).
5. **The replay window**, per sender: the boot it's on and which of its last 64 sequence numbers
   have been taken. Dropped: a copy, one older than the window, one from an earlier boot. The
   first packet from a sender, or from a later boot of it, must be fresh: its sentAt within 30
   minutes of this board's clock, when both clocks are set. Only then does it go into the
   seen-cache.
6. **Handled**, by type, and how the sender is reached is noted: hops 0 is straight over the air
   (its RSSI, and its MAC address is learnt as that board's), more is through relays (the MAC it
   came from names the last relay, if that board's own packets have been heard).
7. **Relayed**, if ttl isn't 0 and RELAY FOR OTHERS is on, 1 to 6 ms later, unchanged but for
   hops and ttl, unless it was for this board alone: the first field after sentAt names it (a call,
   pong, heard, ring, ringing, ring end or message ask to this board), or for a message or message
   end the second (`to`); for type 15 the first field after the ext type (a listening packet's or
   history ask's room, a history's listener), or monitor audio whose list is this board alone. An
   answer is never for one board alone: it floods so every board the ring rang stops. A type (or
   ext type) this board doesn't know is relayed unread. A packet starts with ttl 3: three relays,
   four hops over the air at most.

Every board in range of a sender relays each of its packets once, so with n boards in range the
air carries a voice packet up to n times. With two boards each relays the other's packets back
to it, where they're dropped as its own.

## Calls

- **Half-duplex.** While a board talks (or records a message) it ignores voice packets. Of two
  calls at once, the one heard first plays; the other is only logged.
- **Whose call plays.** A call with a conversation id plays on a board that is in that
  conversation; on one it's addressed to alone (which is then in it), unless that board left it;
  and, the walkie-talkie, on every listening board when it's the call that opened the
  conversation (voice flag `0x01`), which puts each board that plays it in. Any other call of a
  conversation is neither played nor logged. A call without one (an older board's) plays as
  before.
- **Not over** the Recorder, a ringing alarm, the agent listening or speaking, or a call already
  playing: such a call is logged as not played. Music stops for a call and plays on afterwards.
- **The receiver** keeps up to 8 frames (160 ms) in a jitter buffer and plays them in order. A
  missing frame is given up once a frame 3 further on has come (60 ms), or at the call's end, and
  plays as the frame before it at half level (a second one in a row as silence). A frame older
  than the one playing is dropped; a jump of more than 50 frames (1 s) starts again there. A call
  with no packet for 0.8 s has ended. The audio stream buffers 200 ms before it plays
  (`audio.h`).
- **A call stops** after 60 s. The last call heard is kept (its first 60 s), for PLAY LAST.

## Conversations

A group has one conversation at a time, with a random 32-bit id. It's on from an answered ring
(the ring's id) or the first voice (a call from a board in none opens one, flag `0x01`) until 60 s
after its last word, or until nobody is in it. Each board keeps its own view of it (`ConvState` in
`src/comms_calls.h`):

- **Members.** A board is in it once it answered, joined, talked in it, or played the call that
  opened it (or a call to it alone). Its presence then says so every 4 s, with when it began, its
  quiet and the members it has heard itself. A board counts as a member those that said so in the
  last 13 s (three presences), plus, as listed, those another member lists, until 13 s pass without
  that board saying so itself. A board whose presence carries conversation 0 is out (it left, or
  the conversation ended there), and another member's list doesn't bring it back for 13 s.
- **Words.** A voice packet in it is a word: its last word is the latest any board knows (voice
  heard, or a member's "quiet for"). Its start is the earliest known.
- **Leaving.** A board that leaves stops playing its voice (a call of it that plays stops at once)
  until it joins again, but still sees it on, with who is in it.
- **Two at once.** If a board hears of a conversation with another id in the same group (two boards
  opened one each at the same moment), the lower id wins: it keeps its members, starts when the
  earlier began, and a board that was in the other is in it.
- **Ending.** 60 s after its last word, or when no board is in it any more, it ends. A report of it
  just after (a member whose clock runs a little behind) doesn't bring it back unless it knows a
  word more than 2 s later; voice in it does.
- **Seen.** Every board of the group, in it or not, follows it from the members' presences and its
  voice. A board that isn't in it, and didn't leave it, counts it once (`status().conversationsSeen`):
  the screens offer to join.

## Rings

- **The dialer** sends a ring packet every second, to all of the group or one board, with the
  conversation an answer will put the boards in. It stops when an answer comes (ANSWERED), when it
  gives up (a ring end, reason 1), or after 60 s (a ring end, reason 2: NO ANSWER). It counts the
  boards whose ringing packet came in the last 3.5 s.
- **A board rings** for a ring to all or to it, unless it is in the group's conversation already,
  rings for another ring, or is ringing that group itself. It answers each ring packet with a
  ringing packet and chirps every 3 s (not over the Recorder, an alarm, the agent listening or
  speaking, or a call; over music, which holds for half a second). It stops when the dialer's ring
  end comes, when any board's answer to that ring comes, when it's answered or ignored here, or
  after 5 s without a ring packet (the dialer went out of range or off). A ring that ended here
  doesn't ring again, except one lost that way.
- **Answering** sends an answer (and again for each ring packet that still comes); the answering
  board and the dialer are in the conversation, and every other board learns it's on, with both
  in it, from the answer itself.

## Messages

From 5 s into a ring with no answer (or once it's NO ANSWER) the dialer may leave a message instead.

- **Recording.** The ring stops first (a ring end, reason 3: the boards rang stop ringing). The mic
  opens with the push-to-talk cue; each 20 ms frame is kept and sent at once as a message packet,
  numbered from 0, so the transfer is done when the dialer lets go. Up to 60 s (3,000 frames).
  Then three message end packets with the count and the checksum (dropped: three with flag
  `0x01`, and the receivers forget it).
- **The checksum** is FNV-1a (32-bit; offset basis 2166136261, prime 16777619) over the frames as
  boards keep them: 164 bytes a frame, predictor (2, little-endian), step index, byte count, the
  bytes, zero-filled to 160.
- **Complete.** A message isn't live, so each receiving board (all of the group for a ring to all,
  else the one board) asks again for what it missed: once the end has come and frames have stopped
  coming for 0.7 s (or 2 s with no frame and no end: then with flag `0x02`, and the sender sends the
  end again), a message ask with the map of the frames missing from the first gap, 10 to 100 ms
  later (boards don't all ask at once). The sender sends every frame asked for again, 100 a second,
  then the end again; the next ask waits for that: 1 s plus 10 ms a frame asked for, at most 5 s.
  Every board in range takes the frames sent again for any of them. A board that has every frame,
  with the checksum right, sends "I have it all" (and again if another end packet comes); the
  sender counts those boards. After 6 asks in a row that brought nothing, or 3 minutes, a board
  keeps what it has, the missing frames as silence, as not complete. The sender keeps the message
  5 minutes for asks.
- **Kept** on the card (`SD:/.inkdeck/comms/`: `messages.tsv`, a line each, and
  `<sender>-<message>.adp`, the frames as kept), in PSRAM without one (1.5 MB at most, about 3
  minutes). Until played, and for a day after; at most 12, the oldest played one going first. A
  board chirps once when one is in. Messages never play by themselves.
- **Tested** (2026-11-22, two boards in a room on channel 6): 8 s with every 7th message frame
  dropped on the receiver (66 dropped) complete after 3 asks (84, 14, 2 frames), checksum alike on
  both; 5 s with every 5th dropped, complete after 3 asks; 10 s with none dropped, no ask.

## Baby monitor

A board left in a room (the **room**) sends that room's sound to boards that listen
(**listeners**), over the radio like the rest, in its group.

- **Started on the room's board only.** No packet starts it, and nothing a listener sends switches
  a mic on: a listening packet or a history ask only gets an answer from a room that is on. A
  talk back plays in the room and pauses its mic. It doesn't survive a restart on either side.
- **The gate.** The room's mic captures all the time. Each 20 ms frame's level is its loudest
  sample (dBFS), compared with the threshold (-60 to 0 dBFS in 3 dB steps, -30 by default; a
  board keeps its last in NVS). Every frame is kept half a second (25 frames) and encoded as it
  comes (IMA ADPCM, voice's 16 kHz); a frame goes out half a second after it was captured, and
  only if a frame at or over the threshold came within the half second after it or the 2 s
  before it. So a sound goes out with the half second before it (pre-roll: the start of a cry
  isn't cut), and on for 2 s after the level drops (hang), all half a second late; two sounds
  closer than 2.5 s are one. A sound gets the next sound number (they go on across starts, from a
  random one each boot); its frames are numbered from 0. Nothing goes out while it's quiet but
  the still-here. The first 200 ms after the mic starts are dropped: its start-up thump (3
  frames of zeros, then to -39 dBFS over 4 more, measured 2026-11-23) isn't a sound.
- **Still here** every 2 s while on, with the threshold, the loudest frame since the last one (so
  a listener's meter moves when nothing is sent), power, the bar now and who may listen; three
  "off" packets when stopped. The room's presence carries the `0x04` flag while it's on.
- **History.** The room keeps 60 bars of 2 minutes, the loudest frame of each, numbered from its
  start, and sends them to a listener that asks; the bar now comes in every still-here, so a
  listener that opened late has the whole chart within a packet or two.
- **The room stays silent.** It doesn't ring for rings (it sends no ringing either, so a dialer
  doesn't count it), doesn't chirp, and doesn't play calls to all of the group or a
  conversation's; it doesn't join conversations. It plays only a call to it alone (target its
  id) from a board it lets listen: **talk back**. Its mic stops before that plays and starts
  again 0.4 s after the speaker has played it out, so the talk back isn't sent back; the
  still-here's `0x04` says the mic is off meanwhile.
- **A listener** lists the rooms whose still-here (in the last 20 s) lets it in, and listens to one
  at a time. It says so to the room every 4 s, and asks for the history. The sound plays as it
  comes, through a jitter buffer like a call's, at the set volume. **SOUND** while frames come,
  **QUIET** 0.6 s after the last; a sound is **LOUD** if a frame of it is 12 dB or more over the
  room's threshold, else **SOFT**; with the volume at 30 or below, a sound turning LOUD chirps
  once at half volume at least. **NO LINK** when nothing (still-here or audio) has come from the
  room for 20 s: an alarm (a siren's two notes, at half volume at least) every 2.5 s until the
  board silences it or the link is back. An "off" ends listening as **STOPPED**, without an alarm.
- **The delay** from the room's mic to a listener's speaker is the pre-roll (0.5 s), the speaker's
  buffer (0.2 s, audio.cpp's stream) and a frame: about 0.72 s, computed rather than measured.
- **Who plays what on a listener.** A call (live or PLAY LAST), a message, this board's own talk
  (talk back is a call to the room alone) and a ring's trill take the speaker over the monitor's
  sound; the sound plays on with its next frame after (a chirp holds it off 1 s). Monitor audio
  isn't a call: it isn't logged, doesn't count as unheard and doesn't raise the call sheet.
- **What the allow-list is worth.** The packets are sealed with the group's key like every other,
  so the listener list is an address filter: boards not on it don't list the room and don't play
  it, but a modified board holding the group's secret could decode every frame. A baby monitor
  wants a group whose secret only its listeners have (a `mesh_group` line on those boards alone).
  There are no keys per board.
- **Tested** (2026-11-23, two boards in different rooms on channel 6, -69 to -74 dBm, each as
  the room and as the listener, with `!commsmontone` in place of the mic): an allow-list and all
  of the group; a board not let in lists nothing and plays nothing; a tone over the threshold
  arrives with its pre-roll and hang (3 s at -20 dBFS: 276 frames, 25 + 150 + 100 + 1), 1.4 to
  2.2 % of frames lost on the air (9 % in one run just after a restart); under it nothing goes; SOFT and LOUD, and the chirp at volume
  25; still-heres, the listener shown on the room; the history to a listener 2 minutes late;
  NO LINK 20 s after the last packet, the alarm, its silencing and the link back; a clean stop;
  talk back in the room with nothing sent back; a ring and a call to all leaving the room silent;
  a message played on a listener over the sound. Not tested: by ear, a real call or ring on a
  listener during the sound, on battery, a power-off's off, more than two boards.

## Versions

Rings, conversations and messages kept version 1: their packets are new types, and what they add to
presence and voice comes after the fields a board from before reads, which ignores it. A board from
before them drops the new types (its parser refused types it didn't know, so it doesn't relay them
either); it still hears and plays the newer boards' calls, and theirs plays on them as a call
outside any conversation. From now on a board relays types it doesn't know, so later types can be
added the same way. The baby monitor took type 15, the last 4-bit number, as an extended type
whose body carries a second type byte: later features add ext types, and a board relays ext types
it doesn't know. A board from before it relays type 15 unread (it relays types it doesn't know)
and ignores the room's presence flag.

## Not built yet

- **`via`**: `air` and `both` both go over the radio; `net` sends nothing, since the inkharness
  link (and the hub, the rendezvous id's use) is a later phase.
- **The baby monitor's** ESP-NOW power save (a listener or a room on battery waking in windows
  instead of keeping its radio on), keys per board (the allow-list is an address filter only), and
  the monitor over inkharness.
- **Reachable on battery** (a sleeping board polling), boards on USB holding rings or messages for
  sleeping boards, and inkharness holding them: a board that isn't listening when a ring or a
  message goes gets nothing.
- **Tested** with two boards (2026-11-22): presence, ping, calls both ways, the heard receipt,
  PLAY LAST, the duplicate and replay drops, and relaying a group a board isn't in; then rings to
  all and to one board (answered, given up, NO ANSWER at 60 s, the dialer gone silent: stopped
  5.0 s after its last ring packet), conversations (from an answer and from a first call; a talk
  each way in one; leave and join; the end 60 s after the last word on both boards) and messages
  (above; one kept on the card through a restart and played). Not tested: relaying through a
  third board to one out of the sender's range (flooding over more than one hop), a conversation
  of three or more boards and two meeting, two boards answering one ring, a board off Wi-Fi on
  `mesh_channel`, range, and current draw.
