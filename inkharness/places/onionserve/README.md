# onionserve

The folder `inkharness serve` runs from on onionserve, the home server: what it picks up there
besides its `inkharness.toml` (the token, kept out of git by `.gitignore`).

| Path | |
| --- | --- |
| `skills/football/` | Live football match channels on Dispatcharr: English streams, the 901–910 slots, cleanup, fixtures. Laid out so one named match fits a spoken turn's rounds |
| `skills/dispatcharr/` | The Dispatcharr MCP tools and the server around them |
| `skills/ipviewer/` | IPViewer, the IPTV and music player: channels, Jellyfin films and music, casting to the TVs and speakers |
| `.mcp.json` | The Dispatcharr MCP server (`mcp/tv.cs` in the onionserve.tunarr-ai checkout) and IPViewer's (`ipviewer --mcp`) |

The skills came from onionserve.tunarr-ai's Claude Code skills, with inkharness's tool names
(`dispatcharr_get_fixtures`, `ipviewer_search_library`) and without what only Claude Code has
(ToolSearch, CronCreate, subagents). The paths, TVs and speakers are this home's.
