---
name: ipviewer
description: Control the user's IPViewer IPTV and music player (ipviewer_* tools). Use to play, cast or stop channels, films, shows or music on a TV or speaker, skip, pause, change volume, turn a TV on or off, see what's on, or add a playlist or Jellyfin server.
---

# Using IPViewer's MCP tools

IPViewer is the user's IPTV and music player. Its MCP server is `ipviewer`, so the tools are `ipviewer_<name>` (for example `ipviewer_list_channels`). The tool names in the tables below are the bare names. Use the argument names below; don't guess them.

If there are no `ipviewer_` tools at all, the server isn't connected. It's the `ipviewer` command (a .NET global tool) run as `ipviewer --mcp`, set up in the folder's `.mcp.json`. Tell the user it isn't connected, and don't try to play anything another way.

## Rules

1. **Find, then play by id.** Never invent an id or a name.
   - **Music** (an artist, an album, a song) and **films, shows and episodes**: `search_library`, never `list_channels`. Channels are live TV only. See [Music](#music-a-jellyfin-server) and [Films and episodes](#films-and-episodes-a-jellyfin-server).
   - **Live TV** (a channel, a game, a match): `list_channels`, then pass the channel's `id` (`ch-` and 12 hex digits) as `play`'s `id` argument. `list_channels` checks the playlists first, so a channel added just now is listed. If its `note` says a playlist is still being checked, wait a few seconds and call it again.
2. **Search one word.** In `list_channels`, `search` matches words in a channel's name, number or group, or in the programme on now; in `search_library`, words in a title or an artist's name. Use one distinctive word: a team (`Fulham`), a network (`beIN`), a show. For a game, search one team, not "Aston Villa vs Fulham".
3. **Empty right after starting?** The playlists are still loading. Wait 2 seconds and search again, up to 5 times, before telling the user nothing matched.
4. **Several matches:** pick the one whose `name` or `onNow` fits the request, and say which you picked. If none clearly fits, list them and ask.
5. **Errors say what to do.** A failed call comes back as a tool error that explains what to do next. Do that; never repeat the same call unchanged. See [When something went wrong](#when-something-went-wrong).
6. **No addresses.** Never ask for or show stream or playlist addresses. The tools never return them, and `add_source` takes only an address the user gave you.
7. **Say only what you've checked.** Say something is playing once `status` says `"state": "playing"`, and that's IPViewer's view: you can't see the screen. Say a TV is off only if `list_tvs` says `"off": true`.

## Where to play

These are the user's defaults, for every `play`, even when IPViewer's window is open. Anywhere else the user names, use that instead.

| The user wants | and says | `cast_to` with `tv` set to |
|---|---|---|
| Video (a film, show, episode, channel or game) | nowhere | `Living Room TV` |
| Music | nowhere | `Home Speakers` |
| Anything | "upstairs" | `Master Bedroom TV` |
| Video | "basement" or "entertainment room" | `Basement TV` |
| Music | "basement" or "entertainment room" | `Entertainment Room` |

`Entertainment Room` is the Yamaha receiver ("the Yamaha"). It plays sound only, so video is refused on it: the TV in that room is `Basement TV`.

Use this PC only if the user asks for it ("here", "on this PC").

## Arguments

Any argument not marked required is optional.

| Tool | Arguments |
|---|---|
| `list_channels` | `search`, `group`, `favorites`, `limit` |
| `whats_on` | `channel` |
| `play` | `id` (required), `fromStart` |
| `skip_forward` | `seconds` |
| `set_rate` | `rate` (required) |
| `set_volume` | `volume` (required) |
| `mute` | `muted` |
| `cast_to` | `tv` (required) |
| `tv_power` | `tv` (required), `on` (required) |
| `event_log` | `lines` |
| `add_source` | `url` (required) |
| `search_library` | `search`, `type`, `within`, `limit` |

The other tools take no arguments.

## Recipes

### Play on a TV ("on the living room TV")

Also use this whenever `status` says `"windowless": true`: that copy of IPViewer has no window and plays on a TV or speaker only.

1. `list_tvs`. Match the user's words to a TV's `name` ("living room" → "Living Room TV"), or take the name from [Where to play](#where-to-play). If nothing matches, list the names and ask.
2. `cast_to` with `tv` set to that exact name. A TV with `"off": true` is turned on first, which can take up to a minute: tell the user you're turning it on. If it fails, the TV can't be woken: ask the user to turn it on with its remote, then start again at step 1. A Google Cast TV that's off doesn't show as off, can turn itself on when cast to, and can come up loud, so don't pick one the user didn't ask for.
3. `list_channels` with `search`, then `play` with `id` set to the channel's id.
4. `status` every 2 seconds until `"state": "playing"` (up to 40 seconds). A DLNA TV takes about 5 seconds.

Always `cast_to` before `play`. `cast_to` only picks the TV when nothing is playing, so a success doesn't mean anything is on screen yet. A restart of the server forgets the TV: if `status` says `"castingTo": null`, call `cast_to` again.

### Play on this PC

Only when the user asks for this PC and `status` says `"windowless": false` (IPViewer's window is open): `list_channels`, then `play` with the id, then `status` until `"state": "playing"`.

### Stop

| The user says | Call |
|---|---|
| "stop", "turn it off", while casting to a TV | `stop_casting` (a Samsung TV cast to over DLNA turns off too) |
| "stop, but leave the TV on" | `stop` |
| "stop", playing on this PC | `stop` |
| "turn off the TV", for a TV nothing is cast to | `tv_power` with `on: false` (see below) |

### Pause and catch up

`pause`, `resume`, `skip_forward` (`seconds`, default 30), `go_live`, `set_rate` (1, 2 or 4; not while casting).

### Volume

- `set_volume` (0 to 100) sets this PC's volume, or the device's own while casting to one that takes it. TV volumes are usually low (5 to 20), so ask before going above 30. Moving it up unmutes.
- Entertainment Room (the Yamaha): whenever you cast to it, call `set_volume` 20 right after `cast_to`, so it starts at 20. Don't wait for the user to ask.
- `mute` (true or false) mutes this PC, or the device while casting to one that takes it. If it refuses, `set_volume` 0 quiets it instead.
- "Louder" or "a bit quieter": read the current level first. While casting, `status` has the device's own `tvVolume` and `tvMuted`, read from it just then (its remote may have changed them); they're `null` if it doesn't say them. `volume` and `muted` are always this PC's. Go 3 to 5 steps at a time on a TV.

### Turn a TV on or off

`tv_power` with `tv` (a name from `list_tvs`) and `on` (true or false). No channel needed. `list_tvs` says what each TV can do: `canTurnOn`, `canTurnOff`.

- **On** works for DLNA TVs. One that's off is woken, which can take up to a minute: tell the user you're turning it on. A Samsung's screen comes on too. Another TV may stay dark until something is cast to it; the result says so. If waking fails, ask the user to turn it on with its remote.
- **Off** works for Samsung TVs only. Turning off the TV being cast to stops casting first.
- **The first time** with a Samsung, the TV asks on screen whether to allow IPViewer. Tell the user someone at the TV has to press Allow (within 30 seconds).
- A Chromecast's TV, and off for any other TV, take the TV's own remote.
- To watch something, don't call `tv_power` first: `cast_to` turns the TV on itself.

### What's on

`whats_on` with a channel id gives now and next; `list_channels` shows `onNow` for each channel.

### Films and episodes (a Jellyfin server)

1. `search_library` with one distinctive word of the title (`search`, optionally `type`: `film`, `show` or `episode`), or with no arguments for "what was I watching" / "the next episode": what the user stopped part way, then each show's next episode. Each item has an `id`, whether it's `watched`, and `resumeAt` where the user stopped.
2. Unless the user asked for this PC, `list_tvs` then `cast_to` first, as in [Play on a TV](#play-on-a-tv-on-the-living-room-tv). Then `play` with that `id`. It starts where the user stopped; `fromStart: true` starts over. A show plays its next episode.
3. `status` shows the `item` and its `position`. `pause`, `resume`, `stop` and the TV recipes work as for channels.
4. While an episode plays, `play_next` and `play_previous` go to the next and previous episode of the show, across seasons. A film has neither.

### Music (a Jellyfin server)

1. `search_library` with one distinctive word (`search`, optionally `type`: `artist`, `album` or `track`). Each item has an `id`, a `type`, and for albums and tracks the `artist` (and a track's `album`). Songs are found by their own title and albums by theirs, so for "something by The Strokes" find the artist first.
2. `within` set to an artist's `id` lists their albums, newest first; set to an album's `id` it lists its tracks in order.
3. **Where.** See [Where to play](#where-to-play). For this PC, go on. For a speaker or a TV, `list_tvs` then `cast_to` first, as in [Play on a TV](#play-on-a-tv-on-the-living-room-tv). `playsVideo: false` is a speaker: it plays music only, so a channel or film is refused on it.
4. `play` with the album's or track's `id`. An album starts at its first track; a track starts from its beginning and plays on through its album. An artist can't be played: pick one of their albums.
5. `status` every 2 seconds until `"state": "playing"` (up to 40 seconds). It gives `track`, `artist`, `album`, `queuePlace` (`"3 of 12"`) and `position`.
6. `play_next` and `play_previous` move through the album. `play_previous` is the Previous button: more than 3 seconds into a track, it goes back to that track's start, so call it twice for the one before. `pause`, `resume`, `stop`, `stop_casting` and the volume tools work as for channels. `skip_forward` is a skip in time for channels and doesn't change track.

Only album order exists: there's no shuffle, repeat or adding to the queue.

### Add a playlist or a Jellyfin server

`add_source` with the address the user gave you. For a Jellyfin server it returns a Quick Connect code: give it to the user to approve in a Jellyfin app they're signed in to (user menu › Quick Connect). The server is added once they do.

### When something went wrong

`status` shows `state` (`idle`, `connecting`, `playing`, `unstable`, `reconnecting`, `failing_over`, `offline`, `stopped`), the `channel`, `castingTo` and a `detail` (`null` when there's nothing to say). `event_log` (`lines`, default 20) shows what happened, newest first. Quote the reason to the user.

Work out which kind of failure it is before you explain it:

| What you see | What it means | What to do |
|---|---|---|
| `<tool> didn't run: it needs ...` or `it doesn't take ...` | Your call was wrong; nothing happened | Fix the arguments as the error says and call again. Don't mention it as a fault in IPViewer |
| `<tool> failed inside IPViewer (<type>)` | A fault in IPViewer | Tell the user the tool and the type. Check `event_log`. Don't retry more than once |
| `An error occurred invoking '<tool>'`, with nothing more | An older IPViewer that doesn't say why. It may well be your arguments | Check your arguments against the tool's schema before anything else |
| Any other tool error | IPViewer refused, and says why | Do what it says |
| The call worked, then `status` shows `offline`, `reconnecting` or `failing_over` | The stream or the TV | Quote `detail` and `event_log` |

Don't blame the TV, the network or the install, or ask the user to change them, until the call has reached the player. A call that reached it leaves an `Agent: played ...` line in `event_log`; without that line, look at your call first. Other lines in the log can be old and unrelated to this failure, so check their times.

## Reporting back

One or two sentences: what is playing and where, or what failed and why, in the tool's words. Example: "Aston Villa vs Fulham is playing on the Living Room TV."
