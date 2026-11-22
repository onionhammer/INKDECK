# Comms protocol

How INKDECK boards talk to each other over the radio (COMMS, issue #66): push-to-talk voice in
groups, and relaying for each other as a mesh. The board's side is `src/comms.h` and
`src/comms.cpp`; the packet format is `src/comms_wire.h`, the keys `src/comms_crypto.h`, the codec
`src/adpcm.h`. Keep this file and those in step. Version 1, written 2026-11-22.

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
reserved byte, a type it doesn't know, or sender 0.

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

## Bodies

Every body starts with **sentAt** (4 bytes): the sender's clock, UTC seconds, or 0 if it isn't
set. Then, by type:

**Presence** (1): flags (1: `0x01` on USB power, `0x02` relays for others, `0x04` monitoring its
room, `0x80` hello: answer with your presence), battery (1: percent, `0xFF` none), name length
(1, up to 16), name (ASCII, upper case). Each board sends one in each of its groups when it starts
listening (with the hello flag), then every 30 s (plus up to 3 s at random). A board that hears a
hello answers within 0.1 to 0.6 s, at most once in 5 s. A board heard from in no packet for 95 s is
away.

**Voice start** (2): target (4: 0 for all of the group, else the one board's id), call (2: the
caller's call counter), codec (1: `1` is the one below).

**Voice** (3): target (4), call (2), frame (2: 0, 1, 2... within the call), predictor (2, signed),
step index (1, 0 to 88), byte count (1, up to 160), then the IMA ADPCM bytes. 64 kbps: 16 kHz mono,
two samples a byte, the first in the low nibble, the standard IMA step and index tables; 160 bytes
are 320 samples, 20 ms. Predictor and step index are the encoder's state before the first sample,
so each frame decodes on its own: a lost frame is a 20 ms gap. A voice frame is 212 bytes on the
air. Only the last frame of a call may be short.

**Voice end** (4): target (4), call (2), frames (2: how many voice frames the call had). Sent three
times, 40 ms apart (three packets, three sequence numbers).

**Ping** (5): ping id (4), the pinger's millisecond clock (4). Every board that opens it answers.

**Pong** (6): to (4: the pinger), ping id (4), the clock value echoed (4), RSSI of the ping as
received (1, signed dBm; 0 if it came through relays), hops the ping had passed (1), held (2: the
milliseconds the answer was held back, a random 5 to 155, so boards in range don't all answer at
once). The round trip less `held` is the time on the air and in the two boards.

**Heard** (7): caller (4), call (2). Sent once, 50 to 250 ms after a call addressed to all or to
this board played out of the speaker; the caller counts the boards that sent one.

A body that ends before its fields do is dropped. Bytes after the fields are ignored, so a later
version can add fields at the end without a new version number.

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
   hops and ttl, unless it was for this board alone (a call or pong or heard to this board). A
   packet starts with ttl 3: three relays, four hops over the air at most.

Every board in range of a sender relays each of its packets once, so with n boards in range the
air carries a voice packet up to n times. With two boards each relays the other's packets back
to it, where they're dropped as its own.

## Calls

- **Half-duplex.** While a board talks it ignores voice packets. Of two calls at once, the one
  heard first plays; the other is only logged.
- **Not over** the Recorder, a ringing alarm, the agent listening or speaking, or a call already
  playing: such a call is logged as not played. Music stops for a call and plays on afterwards.
- **The receiver** keeps up to 8 frames (160 ms) in a jitter buffer and plays them in order. A
  missing frame is given up once a frame 3 further on has come (60 ms), or at the call's end, and
  plays as the frame before it at half level (a second one in a row as silence). A frame older
  than the one playing is dropped; a jump of more than 50 frames (1 s) starts again there. A call
  with no packet for 0.8 s has ended. The audio stream buffers 200 ms before it plays
  (`audio.h`).
- **A call stops** after 60 s. The last call heard is kept (its first 60 s), for PLAY LAST.

## Not built yet

- **`via`**: `air` and `both` both go over the radio; `net` sends nothing, since the inkharness
  link (and the hub, the rendezvous id's use) is a later phase.
- **The baby monitor** (the room's flag is in presence, unused).
- **Tested** with two boards (2026-11-22): presence, ping, calls both ways, the heard receipt,
  PLAY LAST, the duplicate and replay drops, and relaying a group a board isn't in. Not tested:
  relaying through a third board to one out of the sender's range (flooding over more than one
  hop), a board off Wi-Fi on `mesh_channel`, range, and current draw.
