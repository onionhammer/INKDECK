---
name: dispatcharr
description: Interact with a Dispatcharr IPTV management server. Manage channels, streams, EPG, playlists, recordings, and accounts via the Dispatcharr REST API.
metadata:
  dispatcharrRoot: http://onionserve.onionnet:9191/
---

# Dispatcharr Skill

You have access to a Dispatcharr IPTV management server through the `dispatcharr` MCP server. Call its tools by the names in the table below.

## Authentication

- The MCP tools handle authentication **automatically**: the server logs in on first use and refreshes the token. Call the tools directly; don't call `dispatcharr_login` first.
- Credentials are kept by the MCP server, not in this folder. Never ask the user for them or write them into a file.

## Available MCP Tools

| Tool | Key Parameters | Description |
|------|---------------|-------------|
| `dispatcharr_login` | `dispatcharrRoot?` | Authenticate and retrieve JWT tokens (only if you need a token for a direct REST call) |
| `dispatcharr_search_streams` | `query`, `page?`, `pageSize?` | Search streams by name (paginated) |
| `dispatcharr_list_channel_groups` | | List all channel groups (IDs, names, channel counts) |
| `dispatcharr_ensure_channel_group` | `name` | Get or create a channel group by name; returns the group ID |
| `dispatcharr_list_channels` | `search?`, `channelGroupId?` | List channels, optionally filtered by name or group |
| `dispatcharr_create_channel` | `name`, `channelNumber?`, `channelGroupId?` | Create a channel (use dispatcharr_set_channel_streams to attach streams after) |
| `dispatcharr_create_channel_from_stream` | `streamId`, `name?`, `channelNumber?`, `channelGroupId?` | Create a channel from a single stream |
| `dispatcharr_update_channel` | `channelId`, `name?`, `channelNumber?`, `channelGroupId?`, `logoFromStreamId?` | Rename, renumber, change the logo to a stream's logo, or move a channel to a different group |
| `dispatcharr_set_channel_streams` | `channelId`, `streamIds` | Set/replace all streams on a channel (for backup/failover) |
| `dispatcharr_delete_channel` | `channelId` | Delete a single channel |
| `dispatcharr_delete_channels` | `channelIds` | Bulk delete multiple channels |
| `dispatcharr_get_fixtures` | `date?`, `days?`, `majorLeaguesOnly?`, `leagueId?`, `timezone?` | Football fixtures (see the football skill) |

## Typical Workflow

1. **Search**: `dispatcharr_search_streams` with a team or match name to find stream IDs.
2. **Create**: `dispatcharr_create_channel_from_stream` with the primary stream, or `dispatcharr_create_channel` then `dispatcharr_set_channel_streams`.
3. **Add backups**: `dispatcharr_set_channel_streams` with all stream IDs (primary + backups) for redundancy.
4. **Verify**: `dispatcharr_list_channels` to confirm.
5. **Cleanup**: `dispatcharr_delete_channel` or `dispatcharr_delete_channels` after the event.

## Guidelines

- Prefer the MCP tools. They handle auth, pagination and error formatting.
- The API also covers EPG, playlists, recordings, integrations and accounts, and has endpoints the tools don't wrap (for example `GET /api/channels/channels/summary/`, `POST /api/channels/channels/edit/bulk-regex/`, and API keys under `/api/accounts/api-keys/`). These need direct REST calls, with a token from `dispatcharr_login`. The full OpenAPI spec isn't in this folder, so check the endpoint with the user before you rely on a path you haven't seen work.
- Don't delete or renumber channels the user didn't ask about. Check `dispatcharr_list_channels` before a bulk change.

## Service Operations

- Dispatcharr runs in a Docker container named `media-dispatcharr`. Restarting it needs the user's go-ahead.
- To verify a restart, check `docker ps --filter name=media-dispatcharr` and `docker logs --tail 100 media-dispatcharr`.
