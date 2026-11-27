# Football: competition notes and detail

Read this when a match is from a particular competition, or when a slot question isn't answered by SKILL.md.

## UEFA Champions League
- Kick-offs at 5:45 PM or 8:00 PM UK time (two slots). In Central time that's usually 11:45 AM or 2:00 PM. Name channels with the local time.
- `UEFA | NN -` streams are English event feeds numbered by match.
- Paramount+ and DAZN Canada carry every UCL match.

## English Premier League
- Dedicated team channels: `UK: EPL <TEAM>` (for example `UK: EPL NEWCASTLE UTD`).
- US: Peacock / NBC streams. Search by team name plus "Peacock".
- Some stream names carry future dates. Check the date matches today.

## English Championship
- Dedicated team channels: `UK: CHAMP <TEAM>` (for example `UK: CHAMP WREXHAM`).
- Paramount+ carries Championship matches in the US.

## Slot example

A Newcastle vs Qarabag match, kicking off 11:45 AM on 01/25, with the English streams 4006 (AU STAN, primary), 9631, 14726, 39532 and 12249:

1. `dispatcharr_list_channels(channelGroupId: <id>)` shows slot 901 idle (`Football 1`, channel id 92).
2. `dispatcharr_update_channel(channelId: 92, name: "Newcastle vs Qarabag at 11:45 AM 01/25", logoFromStreamId: 4006)`
3. `dispatcharr_set_channel_streams(channelId: 92, streamIds: [4006, 9631, 14726, 39532, 12249])`
4. `dispatcharr_list_channels(channelGroupId: <id>)` to confirm.

If slot 901 is busy with a match that started 4 hours ago, release it first (it's finished), then take it.

## Numbering rules, for reference
- Without `channelNumber`, a new channel takes the provider's number (such as 34430), outside the block. Always pass it.
- If the number is taken, `dispatcharr_create_channel_from_stream` silently assigns the next free one. Check `channel_number` in the response.
- `dispatcharr_update_channel` allows duplicate numbers. Check unfiltered `dispatcharr_list_channels()` before renumbering into the block.
