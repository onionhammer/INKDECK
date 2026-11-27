---
name: football
description: Set up live football (soccer) match channels on Dispatcharr with English streams, manage the 901-910 slots, clean up finished matches, and list upcoming fixtures. Fixtures, Games, Matches.
---

# Football Match Channel Management

This skill finds and configures live football/soccer match streams on the Dispatcharr IPTV server. It uses the `dispatcharr` MCP tools, called by the names below (`dispatcharr_search_streams` and so on). Read `references/details.md` for competition notes, slot pitfalls and a worked example.

## A Turn Someone Is Waiting On

A turn someone is waiting on (by voice, or in the chat) gets only **6 rounds** of tool calls, and the reply is one of them. A round is the calls you make together, so make every call that doesn't need another's result **in the same round**. To set up one match the user named ("add the Chelsea game"):

1. `dispatcharr_get_fixtures(majorLeaguesOnly: true)` (no `date`), `dispatcharr_ensure_channel_group(name: "Football")` and `dispatcharr_search_streams(query: "Chelsea")`, together.
2. `dispatcharr_list_channels(channelGroupId: <id>)` and `dispatcharr_search_streams` for the other team, together.
3. For the slot you choose, `dispatcharr_update_channel` (name and `logoFromStreamId`) and `dispatcharr_set_channel_streams`, together, with the release of any finished slot in the same round.
4. Say what you set up: the channel's number and name, and how many streams.

Don't spend a round of its own on the date or the time. The fixtures come back in local time, and if you need the time now, put `run_js` with `new Date().toString()` (local time; `toISOString()` is UTC) in round 1 with the others. If the match is more than 2 hours away and you can't schedule (see When to Set Up), set it up now anyway and say so. For more than one match, or a full cleanup, use `propose_job` if you have it: a job gets many more rounds.

## Discovering Matches: dispatcharr_get_fixtures

`dispatcharr_get_fixtures` finds what's on today or this week. Start here before searching for streams.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `date` | string? | today | Start date, YYYY-MM-DD only (never "today"). Leave it out for today |
| `days` | int | 1 | Days to fetch (1–7) |
| `majorLeaguesOnly` | bool | false | Only the big European leagues and MLS |
| `leagueId` | int? | null | One league ID |
| `timezone` | string? | server's local zone | IANA zone. Leave unset so kick-offs come back in local time |

Major leagues (for `majorLeaguesOnly`): Premier League 39, La Liga 140, Serie A 135, Bundesliga 78, Ligue 1 61, Champions League 2, Europa League 3, Conference League 848, MLS 253.

```
dispatcharr_get_fixtures(majorLeaguesOnly: true)
dispatcharr_get_fixtures(days: 7, leagueId: 2)
dispatcharr_get_fixtures(date: "2026-03-01")
```

## Stream Source Hierarchy

Prefer English-language sources, in this order:

| Priority | Source pattern | Region | Notes |
|----------|---------------|--------|-------|
| 1 | `AU (STAN <NN>)` | Australia | Stan Sport, English commentary, AEST times. Usually the most reliable |
| 2 | `UK: CHAMP <TEAM>` / `UK: EPL <TEAM>` | UK | Dedicated team channels, always English |
| 3 | `Paramount+ <NN>` | US | English, reliable. Search includes date/time |
| 4 | `UEFA \| NN - <Match>` | Generic | English event feeds. Often two copies with different IDs: add both |
| 5 | `UK: D+ PPV <NN>` | UK | Disney+ UK pay-per-view, English |
| 6 | `CA: DAZN PPV <NN>` | Canada | DAZN Canada, English |

**Skip non-English sources** unless the user asks for them or no English stream exists. Examples: `Viaplay SE/NO/DK/FI/IS`, `Tv2Play NO`, `TeliaPlay SE`, `Telia FI`, `MTV FI`, `PLAY+ PPV`, `ES (VIX <NN>)`, `BR: MAX PPV`, `NO: TV2 PLAY PPV`, and streams with `COMMENTAIRES EN FRANÇAIS`. If only Norwegian streams exist, the user can understand Norwegian, so those are acceptable.

"8K EXCLUSIVE" streams are high quality, but check the region tag: `UK: D+ PPV` and `CA: DAZN PPV` are English. `SE:`, `NO:`, `FI:` (Viaplay) and `AR: TOD PPV` are not.

**Match streams over promo channels.** A team name alone is often a team's promo or highlight channel, which doesn't have the rights to the game. Prefer streams that name a match (`Team A vs Team B`), a numbered event feed (`Paramount+ 03`), or `UEFA | 05 - Team A vs Team B`. Deprioritise bare team names.

## Channel Naming

Format: `<Home Team> vs <Away Team> at <Kick-off Time> <MM/DD>`

- `Newcastle vs Qarabag at 11:45 AM 01/03`
- `Inter vs Bodo/Glimt at 2:00 PM 01/03`

The kick-off time and date are **local time** (US Central, `America/Chicago`). `dispatcharr_get_fixtures` returns local times. Never copy a time from a stream name: `AU (STAN)` is Australian time, `EPL NN:` and UK feeds are UK time, and `NEXT |` feeds are GMT or CEST. For a late-evening kick-off the local date can differ from the stream's date.

Apart from the time and date, leave out competition names and source info. Keep the name short.

## Channel Group

All football channels **must** be in the group named "Football". Call `dispatcharr_ensure_channel_group(name: "Football")`, keep the returned `id`, and pass it as `channelGroupId` to every create and list call.

Don't trust one empty filtered list as proof the group is empty. Before concluding there are no football channels, check at least one of: retry `dispatcharr_list_channels(channelGroupId: <id>)`, or list unfiltered `dispatcharr_list_channels()` and keep rows whose `channel_group_id` matches, or `dispatcharr_list_channels(search: " vs ")` and filter the same way.

## Channel Slots

There are **10 reserved channel numbers, 901–910**. Matches reuse these slots, so channel numbers and UUIDs stay the same from game to game. **Never create a match channel outside 901–910, and never delete a slot.** A slot is created the first time it's needed and kept after.

| State | Meaning | Name | Streams |
|-------|---------|------|---------|
| **Busy** | Match not finished (including future matches) | The match name | The match streams |
| **Finished** | Kick-off in the name was more than 3 hours ago | The old match name | Old streams: release it |
| **Idle** | Released, waiting for the next match | `Football <N>` (901 is `Football 1`, 910 is `Football 10`) | None |

Get the current time with `run_js`: `new Date().toString()` is local time. Don't release a slot until its kick-off is more than 3 hours past, since extra time and penalties run long.

**Release a finished slot:**
```
dispatcharr_update_channel(channelId: <slot_id>, name: "Football 1")
dispatcharr_set_channel_streams(channelId: <slot_id>, streamIds: [])
```
An idle slot keeps its old logo until the next match replaces it.

**Choosing a slot:**
1. If the match already has a slot, keep it and refresh its name and streams. If the primary stream changes, update the logo too.
2. Otherwise take the **lowest-numbered idle slot**.
3. If none is idle, create the lowest number in 901–910 with no channel yet.
4. If all ten are busy, don't go outside the block. Set up what fits, earliest kick-off first, and tell the user which matches didn't get a slot.

**A slot holds one match at a time.** A match set up early holds its slot until it finishes.

**Numbering checks:**
- Always pass `channelNumber` when creating. Without it the channel takes the provider's number, such as 34430.
- Check `channel_number` in the `dispatcharr_create_channel_from_stream` response. If the number is taken, Dispatcharr silently assigns the next free one. If that's above 910, a non-football channel holds the block: move the new channel to a free slot if one exists, and tell the user.
- `dispatcharr_update_channel` allows duplicate numbers. Before renumbering into the block, check an unfiltered `dispatcharr_list_channels()`.

**Match channels outside the block** (old provider numbers, found in the Football group): if finished, `dispatcharr_delete_channels`; if not, move into a free slot with `dispatcharr_update_channel(channelId, channelNumber: <slot>)`. Leave alone any channel whose name isn't in the match format, since the user may have made it on purpose.

## When to Set Up

Decide for each match using the current time. If the user says when they want a match set up, do that.

| Time until kick-off | Default |
|---------------------|---------|
| More than 2 hours | Schedule the setup for **30 minutes before kick-off** |
| 2 hours or less, or already started | Set it up now |

**Scheduling:** if you have a `schedule` tool, use it. Its run starts in a new session with none of this conversation, so the prompt must stand alone, for example: "Use the football skill to set up Newcastle vs Qarabag, kick-off 11:45 AM 01/25 local time." Check what is already scheduled before adding a job for the same match, and remove a job before you set that match up early or drop it. If you can't schedule, set the match up now and tell the user why.

**When a scheduled setup runs:** run the Workflow for that match only, including slot cleanup. Don't reapply the 2-hour rule. Then:
- **Kick-off has changed:** check `dispatcharr_get_fixtures`. If it's still more than 2 hours away, schedule again for the new time. Otherwise set it up with the new time in the name.
- **Postponed or cancelled:** create nothing, and tell the user.
- **No suitable streams:** retry once, 15 minutes later. If there are still none, tell the user.

## Workflow

### 0. Clean up and discover

Always start by listing the Football channels and releasing finished slots. Don't wait for the user to ask.

```
dispatcharr_ensure_channel_group(name: "Football")       → the Football group ID
dispatcharr_list_channels(channelGroupId: <id>)          → all football channels
```

Compare each channel's kick-off date and time with now. Names should use `MM/DD`, but also handle legacy `DD/MM` names such as `15/03`. Release every finished slot in 901–910, and delete or move finished or unfinished channels outside the block. Treat an empty first listing as unverified (see Channel Group) before you decide there's nothing to clean up, because a false empty result can put a new match on a number already in use.

Then discover matches with `dispatcharr_get_fixtures(majorLeaguesOnly: true)` or `dispatcharr_get_fixtures(days: 7, majorLeaguesOnly: true)`. Schedule any match more than 2 hours away (see When to Set Up) and continue with the rest.

### 1. Find streams

Search by **both** team names, since some matches appear under only one of them. Search the less common name too:
```
dispatcharr_search_streams("Newcastle")
dispatcharr_search_streams("Qarabag")
dispatcharr_search_streams("Club Brugge")
```

### 2. Select English streams

Pick stream IDs by the hierarchy above. Aim for **2–6 streams** per channel: more gives failover, with diminishing returns past six. Put the strongest English match stream first.

### 3. Assign a slot

Reuse an idle slot:
```
dispatcharr_update_channel(channelId: <slot_id>, name: "Newcastle vs Qarabag at 11:45 AM 01/25", logoFromStreamId: 4006)
dispatcharr_set_channel_streams(channelId: <slot_id>, streamIds: [4006, 9631, 14726, 39532, 12249])
```
Always pass the primary stream as `logoFromStreamId`, so the old logo doesn't stay.

Create a slot that doesn't exist yet:
```
dispatcharr_create_channel_from_stream(streamId: 4006, name: "Newcastle vs Qarabag at 11:45 AM 01/25", channelNumber: 901, channelGroupId: <id>)
→ check channel_number in the response
dispatcharr_set_channel_streams(channelId: <new_channel_id>, streamIds: [4006, 9631, 14726, 39532, 12249])
```

### 4. Confirm

```
dispatcharr_list_channels(channelGroupId: <id>)
```
It should show only idle slots and current or future matches. Check that each match's streams contain both team names and "vs", not just a promo channel.

## Post-Match Cleanup

Finished matches must be cleared out. The main place this happens is step 0 at setup time. On an explicit request, do the same from the list. Any Football match whose kick-off was more than 3 hours ago is finished: release it if it's in 901–910, or delete it if it's outside the block. Never report "no old football channels" from a single empty listing.

## Workflow Examples

- **"Set up today's Champions League matches"**: dispatcharr_ensure_channel_group → dispatcharr_list_channels → clean up → `dispatcharr_get_fixtures(leagueId: 2)` → schedule matches more than 2 hours out → dispatcharr_search_streams both names → filter English → assign slots in kick-off order → confirm.
- **"What big matches are on this week?"**: `dispatcharr_get_fixtures(days: 7, majorLeaguesOnly: true)`, then summarise by day and league.
