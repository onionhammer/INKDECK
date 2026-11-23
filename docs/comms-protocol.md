# Comms protocol

How INKDECK boards talk to each other over the radio (COMMS, issue #66): open mics in groups,
rings, conversations, messages, and relaying for each other as a mesh. The board's side is
`src/comms.h` and `src/comms.cpp`; the packet format is `src/comms_wire.h`, conversations and the
message reassembly `src/comms_calls.h`, the mic's packets, gate, history and mixer
`src/comms_mic.h`, the keys `src/comms_crypto.h`, the codec `src/adpcm.h`, messages on the card
`src/comms_store.h`, the links packet and the relay rule `src/comms_mesh.h`. Keep this file and
those in step. Version 1, written 2026-11-22; rings, conversations and messages added the same
day; on 2026-11-23 the open mic replaced push-to-talk (types 2, 3, 4 and 15's bodies changed, type
7 retired), and later that day the long-range rate and relaying only when needed (the link group,
ext type 3) came, all without a new version number ([Versions](#versions)).

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

**The rate.** A board sends at 1 Mbps (802.11b, long preamble), or with `mesh_range = long` at
Espressif's long-range rate, 500 kbps (`esp_now_set_peer_rate_config()` on the broadcast peer,
`WIFI_PHY_MODE_LR`, `WIFI_PHY_RATE_LORA_500K`): further and through more walls, each packet twice
as long on the air. Measured 2026-11-23: a 220-byte voice frame is 2.25 ms on the air at 1 Mbps (an
air survey: 50 a second took 11.3% of the time); at long range its send callback comes about
2.1 ms later (5.9 to 6.4 ms after `esp_now_send()`, against 3.3 to 4.4 at 1 Mbps, the rest being
the wait for a clear channel), its 263 bytes at half the rate. The 250 kbps rate isn't
used: a voice frame every 20 ms would fill the air. Every board hears both: the station takes the
long-range protocol on top of 802.11b/g/n (`WIFI_PROTOCOL_LR`), set before every join, since
changing the protocols while joined drops the connection; an ordinary access point is joined and
talked to as before. Verified on the boards 2026-11-23: frames sent at the long-range rate come
with rx_ctrl's rate 26 (1 Mbps ones with 0), and the sender's send callback says
`WIFI_PHY_RATE_LORA_500K`; a board without the long-range protocol hears none of them (and can't
set the rate: `ESP_ERR_ESPNOW_ARG`), while one with it still hears 1 Mbps frames; with it the
board joins the home network, the agent and the tailnet connect, and the network's signal is the
same (−55 to −58 dBm with it and without). A board's presence says which rate it sends at
(`0x08`).

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
| `2` | mic | the board's mic is on: every 2 s while it is |
| `3` | voice | 20 ms of an open mic |
| `4` | mic off | the mic went off |
| `5` | ping | `!commsping` |
| `6` | pong | the answer to a ping |
| `7` | (unused) | was push-to-talk's heard receipt; not sent, relayed unread like a later type |
| `8` | ring | a dialer rings all of a group or one board of it, about once a second |
| `9` | ringing | a board rings for a ring (or has stopped): the dialer counts them |
| `10` | answer | a ring answered: it floods, so every board it rang stops |
| `11` | ring end | the dialer gave up, rang 60 s, or leaves a message instead |
| `12` | message | a numbered 20 ms frame of a message |
| `13` | message end | a message's frame count and checksum, or that it was dropped |
| `14` | message ask | a board asks for the frames of a message it missed, or has them all |
| `15` | extended | the last 4-bit type: the body's byte after sentAt is a second type ([Type 15](#type-15-extended)): a mic's history |

## Bodies

Every body starts with **sentAt** (4 bytes): the sender's clock, UTC seconds, or 0 if it isn't
set. Then, by type:

**Presence** (1): flags (1: `0x01` on USB power, `0x02` relays for others, `0x04` its mic is on,
`0x08` it sends at the long-range rate, `0x80` hello: answer with your presence), battery (1: percent, `0xFF` none), name length
(1, up to 16), name (ASCII, upper case), then the conversation the board is in: its id (4, `0` for
none), and only if it isn't 0: on for (2, seconds), quiet for (2, seconds since its last word),
member count (1, up to 8), members (4 each: the other boards in it that this one has heard say so
themselves). Each board sends one in each of its groups when it starts listening (with the hello
flag), then every 30 s (plus up to 3 s at random), every 4 s (plus up to 1 s) while it is in the
group's conversation, and within 0.1 s of joining or leaving one. A board that hears a hello
answers within 0.1 to 0.6 s, at most once in 5 s. A board heard from in no packet for 95 s is
away. A presence that ends after the name is a board from before conversations: it says nothing
about them.

**Mic** (2), the still-here: flags (1: `0x01` sending: its gate is open, `0x02` on USB power,
`0x04` paused: something else has the mic for now, `0x08` opening: this session opened the
conversation), battery (1: percent, `0xFF` none), threshold (1: dB below full scale, 0 to 60), peak
(1: the loudest frame since the last still-here, a level), session (2: the board's counter, one more
each time its mic goes on, from a random number each boot), on for (4: seconds), bar (2: the
2-minute bar now, numbered from the session's start), bar peak (1: that bar's loudest frame so far,
a level), pre-roll (1: in 20 ms frames, 8), target (4: 0 for all of the group, else the one board it's
to), conversation (4). Sent when the mic goes on, every 2 s (plus up to 0.2 s) while it's on, paused
or not, and at once when the threshold changes. 62 bytes on the air. A level is a byte: how many dB
the loudest sample of a 20 ms frame is below full scale, 0 to 96 (96: silence or quieter), `0xFF`
for none.

**Voice** (3): target (4: 0 for all of the group, else the one board), conversation (4), session (2),
sound (2: the gate's counter, one more each sound, going on across sessions from a random number
each boot), frame (2: within the sound, from 0), level (1: this frame's, as the mic measured it before
encoding), flags (1: `0x01` opening, as the still-here's `0x08`), predictor (2, signed), step index
(1, 0 to 88), byte count (1, up to 160), then the IMA ADPCM bytes. 64 kbps: 16 kHz mono, two samples a
byte, the first in the low nibble, the standard IMA step and index tables; 160 bytes are 320 samples,
20 ms. Predictor and step index are the encoder's state before the first sample, so each frame
decodes on its own: a lost frame is a 20 ms gap. A byte count of 0 is 20 ms of silence sent on
purpose: a frame the gate sends while it's open that the board's own speaker could explain (its
echo, [The mic](#the-mic)); such a frame is 60 bytes on the air, a full one 220. Each frame is sent
160 ms after it was captured (the pre-roll). A mic switched off while a sound goes out sends the
frames it still holds back first (up to 8, those the sound's hang covers, each as the gate would
have sent it), 20 ms apart, at the pace they'd have gone at: a burst would overrun a receiver's
8-frame jitter buffer.

**Mic off** (4): session (2), reason (1: `1` switched off, `2` the board is powering off), on for (4:
seconds). Sent three times, 40 ms apart (at once, back to back, for a power-off). After a sound's
last frames (above), the first goes 40 ms after the last of them: a voice frame of the session
that came after its off would start the mic again on the receiver, and it would go LOST there.

**Ping** (5): ping id (4), the pinger's millisecond clock (4). Every board that opens it answers.

**Pong** (6): to (4: the pinger), ping id (4), the clock value echoed (4), RSSI of the ping as
received (1, signed dBm; 0 if it came through relays), hops the ping had passed (1), held (2: the
milliseconds the answer was held back, a random 5 to 155, so boards in range don't all answer at
once). The round trip less `held` is the time on the air and in the two boards.

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
as it does any packet ([Taking a packet](#taking-a-packet)).

| Ext | Name | |
| --- | --- | --- |
| `1` | history ask | a board asks a mic's board for its 2 hours of levels |
| `2` | history | the levels, to the board that asked |
| `3` | links | the boards a board hears straight over the air, and how well; only in the link group ([Relaying](#relaying)) |

Ext types 3 to 7 are for relaying, 8 and up for later features.
| `8` | poll | a sleeping board asks whether anything waits for it ([Reachable on battery](#reachable-on-battery)) |
| `9` | answer | an awake board says what does, to the sleeper |

**History ask** (1): board (4: the mic's). Sent by a board that hears a mic which has been on more
than 2 minutes (its still-here's bar isn't 0), once it hears it, again 2 s apart until the history
comes (three at most). Only a board whose mic is on, in that group, answers.

**History** (2): to (4: the board that asked), session (2), first bar (2), count (1, up to 60), then
that many levels, the bars first to first + count - 1, the last the bar now. 110 bytes on the air
with 60 bars.

**Links** (3), in the link group only: flags (1: `0x01` the board relays for others, `0x02` it
sends at the long-range rate), count (1, up to 32), then for each board it heard straight in the
last 20 s: its id (4), and the share (1, a percent) of that board's last 64 sequence numbers (fewer
while fewer have gone) that came straight from it. Sent with ttl 0 (never relayed), every 5 s plus
up to 1 s while the board listens, and 0.3 to 1 s after it starts. 53 bytes on the air with two
boards listed, 203 with 32.
**Poll** (8): to (4: always 0, for all), flags (1: `0x01` echo: boards on USB answer even with
nothing, `0x02` one of a sweep of the channels), battery (1: percent, `0xFF` none), interval (2:
seconds between its polls), name length (1, up to 16), name. Sent with ttl 0 (never relayed), one
in each of the sleeper's groups, every interval. 57 bytes on the air with a 7-letter name.

**Answer** (9): to (4: the sleeper), what (1: `0x01` a ring that includes it is on, `0x02` voice it
would hear sounds now, `0x04` a message for it is kept to be asked for; 0 for nothing, an echo's
answer), channel (1: the answering board's), flags (1: its presence flags), dialer (4) and ring (2)
for a ring, then message sender (4), message (4), to (4) and frames (2) for a message (zeros when
not). Sent with ttl 0, at once, by every board that hears the poll and has something to say. 68
bytes on the air.

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

**The link group** carries the links packets every board reads, whatever its groups: HKDF as
above with the secret `inkdeck links` and the salt `inkdeck comms 1 links`, so its key is public
and no `mesh_group` line or token gives it. Its key is
`25f4f013d525701f32742171b454d3ecbd8786bb6da2a59ebc0b37be22b1576b` and its id `57c9ed26`. Sealing
it keeps the frame format the same as every other packet's; it hides nothing.

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
- In that group, a mic still-here (type 2) from sender `11223344`, boot 7, seq 3, ttl 3, sentAt 0:
  flags 3 (sending, on USB), no battery, threshold 30, peak 24, session 5, on for 600 s, bar 5, bar
  peak 18, pre-roll 8, to all, conversation `a1b2c3d4` (body
  `0000000003ff1e180500580200000500120800000000d4c3b2a1`), is the 62-byte frame
  `c7120300e13e368e443322110700000003000000e74688b99260814b63cfeaa8142b8f33b65b204e035360b1582729fcbda72e8993accf2e6124db819103`.
- In that group, voice (type 3) from sender `11223344`, boot 7, seq 4, ttl 3, sentAt 0: to all,
  conversation `a1b2c3d4`, session 5, sound 2, frame 9, level 21, flags 1 (opening), predictor
  -1234, step index 40, the 8 bytes `0102030405060708` (body
  `0000000000000000d4c3b2a105000200090015012efb28080102030405060708`), is the 68-byte frame
  `c7130300e13e368e443322110700000004000000a8c23f21818170d60048ac825824a4e2a8b2f438ffdc362dda38f9995ef899d129751713f97265f70f5fe1782ab0339a`.
- In that group, a mic off (type 4) from sender `11223344`, boot 7, seq 5, ttl 3, sentAt 0: session 5,
  switched off, on for 600 s (body `0000000005000158020000`), is the 47-byte frame
  `c7140300e13e368e4433221107000000050000003ddb4c9843cacf766c0a6f4f4f5c99a236200cdcbdbf1bb3fd7296`.
- In the link group, links (type 15, ext 3) from sender `11223344`, boot 7, seq 6, ttl 0, sentAt 0:
  flags 1 (relays), board `55667788` at 97% and `99aabbcc` at 64% (body
  `000000000301028877665561ccbbaa9940`), is the 53-byte frame
  `c71f000026edc957443322110700000006000000bf4af235269e1c3c6f1fb0bde60c7219243837f43cb76b8bd8c0fffdffd68006b1`.
- In that group, a poll (type 15, ext 8) from sender `11223344`, boot 7, seq 6, ttl 0, sentAt 0:
  echo, battery 87, interval 5 s, name `KITCHEN` (body `00000000080000000001570500074b49544348454e`),
  is the 57-byte frame
  `c71f0000e13e368e443322110700000006000000ba7508f5bb634115d8976b9dfb518dfe82b0aa36a4596ae029c81bbf82a42dd2dc4ad8cd21`.
- In that group, an answer (type 15, ext 9) from sender `11223344`, boot 7, seq 7, ttl 0, sentAt 0: to
  `99aabbcc`, a ring, channel 6, flags 1 (USB), dialer `11223344`, ring 9 (body
  `0000000009ccbbaa990106014433221109000000000000000000000000000000`), is the 68-byte frame
  `c71f0000e13e368e443322110700000007000000de01bdf96a08528e1d7874d08729caf35fc9bb4e4dbf3a72b58a79b6b9fe7ee7a37fade383044b5b2c494fa1d1a7841f`.

## Taking a packet

In this order:

1. **The header.** A frame that doesn't parse, or from this board's own id (its own packet, relayed
   back), is dropped.
2. **Straight from its board** (hops 0), whatever its group: its MAC address is learnt as that
   board's (a relay's copies name it), and its sequence number counts toward the share of that
   board's packets heard straight ([Relaying](#relaying)). A packet in the link group then goes to
   [Relaying](#relaying) and no further: it's never relayed, and needs no replay window (a spoofed
   one can only make a board relay more or less).
3. **Another group's.** A board passes on a packet whose group it isn't in by the header alone,
   unread: once (by the seen-cache below), if ttl isn't 0 and RELAY FOR OTHERS is on, and for voice
   and message frames by the relay rule (the type is in the header).
4. **Seen before.** The seen-cache holds the last 256 packets taken, by group, sender, boot and seq.
   The mesh floods, so a packet comes once from each relay in range: only the first is handled.
5. **It opens** with the group's key, or is dropped (and not relayed).
6. **The replay window**, per sender: the boot it's on and which of its last 64 sequence numbers
   have been taken. Dropped: a copy, one older than the window, one from an earlier boot. The
   first packet from a sender, or from a later boot of it, must be fresh: its sentAt within 30
   minutes of this board's clock, when both clocks are set. Only then does it go into the
   seen-cache.
7. **Handled**, by type, and how the sender is reached is noted: hops 0 is straight over the air
   (its RSSI, and its MAC address is learnt as that board's), more is through relays (the MAC it
   came from names the last relay, if that board's own packets have been heard).
8. **Relayed**, if ttl isn't 0 and RELAY FOR OTHERS is on, 1 to 6 ms later, unchanged but for
   hops and ttl, unless it was for this board alone: the first field after sentAt names it (voice,
   a pong, a ring, ringing, a ring end or a message ask to this board), or for a message or message
   end the second (`to`); for type 15 the first field after the ext type (a history ask's mic, a
   history's asker, an answer's sleeper; a poll's is 0, for all, and both go with ttl 0 anyway). Presence, a mic's still-here and off, a ping and an answer are never for one
   board alone: every board of the group follows whose mic is on, and an answer floods so every
   board the ring rang stops. A type (or ext type) this board doesn't know is relayed unread. A
   packet starts with ttl 3: three relays, four hops over the air at most. Voice (3) and message
   (12) frames go on only by the relay rule ([Relaying](#relaying)).

Every board in range of a sender relays each of its other packets once, so with n boards in range
the air carries such a packet up to n times. Voice and message frames, nearly all of the air a
board uses, go on only where they're needed.

## Relaying

Before 2026-11-23 every board relayed every packet it hadn't seen, so two boards in one room
doubled each other's voice on the air: each relayed the other's frames back to it, where they were
dropped as its own. Now a voice or message frame is relayed only when a board that this one hears
straight would otherwise miss it (`Neighbours` in `src/comms_mesh.h`):

- **Neighbours.** A board heard straight (any packet at hops 0, any group's, links included) in the
  last 20 s. For each, the share of its last 64 sequence numbers that came straight: a board's
  sequence numbers run across all its groups, so the gaps are its packets this board missed.
- **Links.** Every board sends its neighbours and their shares every 5 to 6 s (ext type 3 in the link
  group, never relayed): readable by boards of every group, since RELAY FOR OTHERS relays for groups a
  board isn't in. A neighbour's links count for 16 s (about three).
- **The rule.** A frame from origin O, as this board got it from P (the board whose radio sent that
  copy: O itself at hops 0, else the relay its MAC address names; unknown if that board's own
  packets haven't been heard), goes on if some neighbour X, not O and not P, has no links that
  count, or lists neither P nor O at 80% or more. Otherwise it isn't relayed (`!comms` counts both).
  So two boards that hear each other never relay each other's voice; a third that one of them
  doesn't hear (or hears under 80%) gets it relayed; a board with no links (an older board, or its
  links lost) counts as needing everything, which is the flooding from before.
- **Spoofing.** The link group's key is public, so anyone can send links in any board's name. That
  can only make boards relay more (claiming not to hear) or less (claiming to hear boards it
  doesn't): the same as jamming or flooding the channel, which anyone can do anyway.
- Two relays that both hear a board that needs a frame both relay it (no counting of copies yet);
  a weak link heard at 80% or more isn't helped.
- **Measured** 2026-11-23 with two boards on USB in different rooms (−69 to −75 dBm, home Wi-Fi
  channel 6, which carried frames 29 to 50% of the time before any comms traffic), 20 s runs of a
  mic held open by a test tone, loss from the gaps in sequence numbers (`!commsloss`): with the rule
  neither board relayed a voice frame (`!comms`: 1,232 not relayed in a one-mic run); flooding,
  the other board relayed every one. At 1 Mbps the rule made little difference (one mic: 2.2% lost
  against 2.0% flooding; both mics: 1.7 to 2.8% and 4.3 to 9.9% each way, against 2.6 to 2.8% and 5.4
  to 6.9%). At long range with both mics, flooding lost 11.2 to 17.4%, the rule 1.4 to 6.0%: four
  4-ms packets every 20 ms on top of the channel's own traffic swamp it. Not tested on the air: a
  third board, so no frame was ever needed (the rule's third-board cases are in `!commstest`).


## Voice

- **Whose voice plays.** Voice with a conversation id plays on a board that is in that
  conversation; on one it's addressed to alone (which is then in it), unless that board left it;
  and, the walkie-talkie, on every listening board when its session opened the conversation (flag
  `0x01`), which puts each board that plays it in. A board joins the same way from the still-here
  alone (a mic opened in a quiet room is heard from its first sound). Any other voice is not
  played.
- **Not over** the Recorder, a ringing alarm, the agent listening or speaking, or a message this
  board records: such voice isn't played (it's still logged). Voice stops music, which plays on
  once comms lets the speaker go.
- **The mixer.** Each board heard has its own jitter buffer (8 frames). A sound starts to play once
  two of its frames are in (or one has waited 40 ms); then a frame a turn, at this board's pace: a
  frame missing when its turn comes plays as the one before at half level (a second in a row as
  silence) and is dropped if it comes later. A board with nothing for three turns rests until its
  frames come again, and is gone after 2 s. Up to 4 boards play at once, summed and held under full
  scale; a fifth board's voice waits for a free place. A board's buffer more than 8 frames ahead
  (its clock a little faster than this board's, over a long sound) skips its oldest. The mix is
  kept 30 ms ahead of the speaker.
- **PLAY LAST** plays the last 60 s heard (the mixed frames that had voice in them, the quiet
  between left out), over any voice that plays, as does a message.

## Conversations

A group has one conversation at a time, with a random 32-bit id. It's on from an answered ring
(the ring's id) or a mic switched on (a mic on with none on opens one, flag `0x08`/`0x01`), while
any mic in it is on (each still-here is a word in it), and until 60 s after its last word once
none is, or until nobody is in it. Each board keeps its own view of it (`ConvState` in
`src/comms_calls.h`):

- **Members.** A board is in it once it answered, joined, switched its mic on in it, or heard the
  mic that opened it (or a mic to it alone). Its presence then says so every 4 s, with when it began, its
  quiet and the members it has heard itself. A board counts as a member those that said so in the
  last 13 s (three presences), plus, as listed, those another member lists, until 13 s pass without
  that board saying so itself. A board whose presence carries conversation 0 is out (it left, or
  the conversation ended there), and another member's list doesn't bring it back for 13 s.
- **Words.** A voice packet in it is a word: its last word is the latest any board knows (voice
  heard, or a member's "quiet for"). Its start is the earliest known.
- **Leaving.** A board that leaves stops playing its voice at once, and its own mic goes off, until
  it joins again; it still sees it on, with who is in it.
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
  its mic is on (it sends no ringing either: the dialer doesn't count it), rings for another ring,
  or is ringing that group itself. It answers each ring packet with a ringing packet and chirps
  every 3 s (not over the Recorder, an alarm, the agent listening or speaking; over music, which
  holds for half a second, and over voice, mixed in). It stops when the dialer's ring
  end comes, when any board's answer to that ring comes, when it's answered or ignored here, or
  after 5 s without a ring packet (the dialer went out of range or off). A ring that ended here
  doesn't ring again, except one lost that way.
- **Answering** sends an answer (and again for each ring packet that still comes); the answering
  board and the dialer are in the conversation, and every other board learns it's on, with both
  in it, from the answer itself. The answering board's mic stays off.

## Messages

From 5 s into a ring with no answer (or once it's NO ANSWER) the dialer may leave a message instead.

- **Recording.** The ring stops first (a ring end, reason 3: the boards rang stop ringing). The mic
  opens with the push-to-talk cue (an open mic on the dialer pauses meanwhile, and voice doesn't
  play there); each 20 ms frame is kept and sent at once as a message packet,
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

## The mic

A board's mic is on or off. Several can be on at once, in one group or more, and speak at once:
each board plays the others' voices ([Voice](#voice)) while its own mic captures (the speaker and
the mic at once: audio's duplex path).

- **On this board only.** `micOn()` on the board itself switches it on; no packet does, and a
  history ask gets an answer only from a mic that's on. It doesn't survive a restart. It goes off
  with `micOff()` (its off packet), a power-off (the off first), leaving the conversation, or the
  radio going (groups changed).
- **The gate.** The mic captures all the time. Each 20 ms frame's level is its loudest sample
  (dBFS), compared with the threshold (-60 to 0 dBFS in 3 dB steps, -30 by default; a board keeps
  its last in NVS). Every frame is kept 160 ms (8 frames) and encoded as it comes; a frame goes out
  160 ms after it was captured, and only if a frame over the threshold came within the 160 ms after
  it or the 2 s before it. So a sound goes out with the 160 ms before it (pre-roll: the start of a
  word isn't cut), and on for 2 s after the level drops (hang), all 160 ms late; two sounds closer
  than 2.2 s are one. Nothing goes out while it's quiet but the still-here. The first 240 ms after
  the speaker and mic start are dropped: the codec's start-up thump (100 ms of zeros, then -39 dBFS
  falling to a quiet room's -56 to -64 over about 150 ms, measured 2026-11-23).
- **The echo.** Speaker and mic share a small case and there's no echo canceller. The speaker's
  output is known: each 10 ms the mic captures carries the loudest the speaker played into the room
  around then (the level written, after the volume, from the blocks written 50 to 140 ms before it:
  the loopback delay, 60 ms measured, and 80 ms of room). A frame no louder than that plus the
  coupling (-22 dB) plus a margin (9 dB) could be the echo: it doesn't count as over the threshold,
  and a frame the gate sends anyway (pre-roll, hang) goes as silence (a voice frame of 0 bytes). A
  voice over the echo, louder than the echo could be, goes as it is. Measured 2026-11-23 with a 1 kHz
  tone through the speaker: it came back at the mic 26 dB down at -20 dBFS and VOL 100, 25 dB down at
  -10, 22 dB down at full scale, 25 dB down at full scale and VOL 50; its abrupt start came back only
  15 dB down, hence the margin. With a -1 dBFS voice playing, the gate is at -14 dBFS: a voice at the
  mic has to be louder than that to get through over it.
- **Still here** every 2 s while it's on, with the threshold, the loudest frame since the last one
  (so a meter elsewhere moves when nothing is sent), the session, power, the bar now, whom it's to,
  the conversation and whether it's paused; three off packets when it goes off, after the last
  frames of a sound it was sending. Its presence carries `0x04` while it's on, and until those have
  gone. A board whose presence lacks `0x04` (newer than the mic's last packet, or
  from another boot) has its mic off: its off was missed.
- **Paused.** Something else on the board may take the mic for a while: the agent listening or
  speaking, the Recorder, a ringing alarm, music started there, a message it records. The mic then
  captures nothing and the still-here says paused; it picks up by itself 0.4 s after.
- **Quiet in the room.** A board whose mic is on doesn't ring (nor send ringing) and doesn't chirp.
  It plays the conversation's voice: that's how a room is talked to.
- **Heard elsewhere.** Every board of the group follows each mic that's on: QUIET (on, nothing over
  its threshold), SOUND (its voice comes; QUIET 0.6 s after the last frame), and **LOST** when nothing
  (still-here or voice) has come from it for 20 s without its off. LOST sounds an alarm (a siren's two
  notes, at half volume at least) every 2.5 s on a board that heard it (its voice is for that board,
  which is in the conversation) until that board silences it or the link is back; on a board whose
  own mic is on, only for a mic that was on before its own (the board left in a room doesn't alarm
  when the board listening to it goes). The outage is kept with the mic's events and its log entry.
  A clean off is never an alarm; a mic on longer than 5 minutes chirps once (soft, two falling pips)
  on the boards that heard it as it goes off. A sound 12 dB or more over the mic's threshold is LOUD;
  with the volume at 30 or below, a sound turning LOUD chirps once at half volume at least. A board
  whose own mic is on chirps for neither.
- **History.** The mic's board keeps 60 bars of 2 minutes, the loudest frame of each, numbered from
  the session's start, and sends them to a board that asks; the bar now comes in every still-here,
  so a board that came late has the whole chart within a packet or two.
- **The log:** one entry per session (on to off), on the mic's board and on each board that heard it,
  with its length, its sounds, the frames lost and whether it went LOST.
- **The delay** from a mic to the other board's speaker is about 282 ms (2026-11-23, from measured
  parts): the capture 22 ms (a 10 ms DMA block, the frame filling, the comms task's turn), the
  pre-roll 160, the air 5 (half of a ping's 9 ms), the mixer's priming 2 to 30 (20 typical), its queue
  25 on average, the speaker's DMA 50.
- **Who may hear.** The packets are sealed with the group's key like every other: every board of
  the group can follow a mic that's on and hear it (by joining the conversation). A mic to one board
  is addressed to it, not encrypted for it. A room meant for a few boards wants a group whose secret
  only they have (a `mesh_group` line on those boards alone). There are no keys per board.
- **Tested** (2026-11-23, two boards in different rooms on channel 6, -73 to -79 dBm, both ways,
  with `!commstone` adding a 1 kHz tone to the mic: nobody was in either room): a mic to all and to
  one board; a tone over the threshold with its pre-roll and hang (2 s at -20 dBFS: a 4,160 ms
  sound), nothing under it; 1.5 to 11% of frames lost as the signal moved, 5.6 and 8.6% while both
  boards sent (each relayed the other's back: four packets each 20 ms, before the relay rule); both mics on with tones on
  both, each board's echo held back (no sound opened from echo on either), a -10 dBFS tone through a
  -14 dBFS echo gate; live voice and PLAY LAST mixed; LOST 20 s after a vanish, its alarm until
  silenced, the link back; a clean off, the chirp after a long session and none after a short one;
  a board with its mic on not ringing; ring, answer, mics on; leaving mid-sound; a message; the
  alarm, the Recorder and the agent pausing a mic, each left whole. Not tested: by ear, three
  boards at once on the air, on battery, a power-off's off, more than one hop.

## Reachable on battery

A board on battery that would power off (`mesh_reachable` set, a group, the battery at 15% or
more) sleeps instead: light sleep with Wi-Fi stopped, the off face on the panel as before
(`src/power.h`). The state behind it is plain C++ in `src/comms_reach.h`, which `!commstest`
drives with a made-up clock.

- **The poll.** Every `mesh_reachable` seconds (2 to 30, `on` is 5) the sleeper starts Wi-Fi,
  sends a poll in each of its groups (at its `mesh_range` rate), listens 12 ms after the send has
  gone (20 ms for an echo poll; to the first answer with something waiting), and stops Wi-Fi again. The first poll goes as it falls asleep, so the
  boards know at once. It wakes for nothing else but PWR (BOOT and the dial don't wake it) and for
  what its off face needs (the clock's minute, a check-in, an alarm: the board restarts for those as
  the RTC alarm would boot it, and goes back to sleep after).
- **The answer.** Every board that listens and hears the poll answers at once, if it knows of:
  a **ring** that includes the sleeper (its own, or one whose packets go by: rings flood, so every
  listening board of the group knows, and the dialer needn't be in range then; a ring is known for
  3.5 s after its last packet, and is over at its answer or end, or 60 s after it began); **voice**
  it would hear (a mic on in the group, to the sleeper alone, or to all of the group with the
  session that opened the conversation, which every board that hears joins) that made a sound
  within the sleeper's interval plus 2 s; a **message** for it (to all, or to it) that this board
  sent and keeps for asks (5 minutes), unless the sleeper has said it has it whole. A board on USB
  answers a poll with the echo flag even with nothing (what 0).
- **Asleep, reachable.** A board whose last packet was a poll, within three of its intervals (plus
  5 s), shows as asleep in the boards list, with its interval and battery from the poll. A presence
  (it woke) ends that.
- **Woken.** An answer with something waiting ends the sleep: the board goes on booting, listens
  as if a screen held it, and the ring, the voice or the message comes the usual way (a ring packet
  within a second; the still-here or the voice of a mic that opened its conversation, which it then
  joins; for a message, it begins it as one whose every frame it missed, and asks for the end, then
  the frames). The face comes back, holds, and the sheets rise over it. With no key it goes back to
  sleep 30 s after the ring, the sound or the transfer ends, 60 s after a sound while the mic that
  made it stays on (an open mic in a quiet room doesn't keep a sleeping board up all night), and
  15 s after the wake if nothing came. A key makes it an ordinary board that's on.
- **LOST doesn't alarm on a sleeping board.** A sleeping board isn't listening: it forgets the mics
  it heard, without an alarm, each time it goes back to sleep. A board that must sound LOST's alarm
  stays on USB.
- **The channel.** A sleeping board isn't on a network. It polls on the channel it last heard the
  boards on straight over the air (kept in NVS as it listens; `mesh_channel` before it ever has). The
  first poll and one every 3 minutes carry the echo flag; three unanswered in a row (and no other
  answer meanwhile), and it sweeps: a poll with the echo and sweep flags on each of channels 1 to 13,
  20 ms each, and it moves to the first that answers. A sweep that finds nobody (no board on USB in
  range) keeps the channel, and the next waits 30 minutes. Counted in polls, not milliseconds: the
  restarts for the off face's wakes keep the plan in RTC memory.
- **Neighbours.** A sleeping board sends no links packets ([Relaying](#relaying)), so a board in
  range that last heard its links long ago relays voice for it, as for a board it can't vouch for;
  a woken board sends its links within a second of listening again.
- **Timing** (2026-11-23, real light sleep, `!reachbench`): from the wake, Wi-Fi up in about 2 ms,
  the poll's send done at 4.5 ms, the answer at 7.8 ms on average (6.9 to 11), so a poll with an
  answer keeps the board awake about 8 ms; without one, the 12 ms window and Wi-Fi's stop make it
  19 ms.
- **Tested** (2026-11-23, two boards on USB, the sleeper pretending to be on battery with a delay
  for each light sleep): a ring, a mic's voice and a message each woke it at its next poll and it
  went back to sleep after; PWR; the Clock face's minute wakes; a sweep from the wrong channel. Not
  tested: the loop in real light sleep, on battery, three boards.
- **The battery log** (`!reachlog`, System's SLEEP row): polls, answers, echoes, sweeps, wakes,
  restarts, PWR, the time the radio was up, asleep and up after a wake, the battery at the start and
  at the last look. In RTC memory while it sleeps, in NVS every 10 minutes and at each restart; it
  starts again at the first sleep after the board was on USB.

## Versions

The long-range rate and the link group (2026-11-23) kept version 1. A board from before them
ignores links packets (another group's, ttl 0: dropped, not relayed) and floods as before; the
newer boards see it as a neighbour without links and relay for it. A board from before doesn't
take the long-range protocol, so it hears nothing from a board with `mesh_range = long`; the newer
one hears it.

Rings, conversations and messages kept version 1: their packets are new types, and what they add to
presence and voice comes after the fields a board from before reads, which ignores it. A board from
before them drops the new types (its parser refused types it didn't know, so it doesn't relay them
either); it still hears and plays the newer boards' calls, and theirs plays on them as a call
outside any conversation. From now on a board relays types it doesn't know, so later types can be
added the same way. Type 15, the last 4-bit number, is an extended type whose body carries a
second type byte: later features add ext types, and a board relays ext types it doesn't know. The
open mic (2026-11-23) changed the bodies of types 2, 3 and 4 and of type 15's ext types, and
retired type 7, without a new version number: only the two test boards ever ran the bodies before
it, and they were flashed together.

## Not built yet

- **`via`**: `air` and `both` both go over the radio; `net` sends nothing, since the inkharness
  link (and the hub, the rendezvous id's use) is a later phase.
- **ESP-NOW power save** for open mics (a board on battery with its mic on, or hearing one, waking
  in windows instead of keeping its radio on), keys per board, and mics over inkharness.
- **Holding messages for a sleeping board**: a board on USB keeping a message it received and
  serving a sleeper's asks once the sender's 5 minutes are over (it would need packets that carry
  the message's sender, since a message's frames are known by the board that sends them), and
  inkharness holding rings and messages. A sleeping board gets a message only while its sender
  keeps it ([Reachable on battery](#reachable-on-battery)); rings need no holding, since every
  listening board knows of a ring that's on.
- **Tested** with two boards (2026-11-22, push-to-talk, before the open mic): presence, ping, calls
  both ways, the heard receipt, PLAY LAST, the duplicate and replay drops, and relaying a group a
  board isn't in; then rings to
  all and to one board (answered, given up, NO ANSWER at 60 s, the dialer gone silent: stopped
  5.0 s after its last ring packet), conversations (from an answer and from a first call; a talk
  each way in one; leave and join; the end 60 s after the last word on both boards) and messages
  (above; one kept on the card through a restart and played). Not tested: relaying through a
  third board to one out of the sender's range (flooding over more than one hop), a conversation
  of three or more boards and two meeting, two boards answering one ring, a board off Wi-Fi on
  `mesh_channel`, range, and current draw.
