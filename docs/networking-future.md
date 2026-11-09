# Modded multiplayer: the planned design

Status: **designed, not built.** This release has no Kyubu network transport. Everything here builds on pieces that do exist and are tested locally: logical sides, side-checked API calls, channels over the loopback transport, registry snapshots with hashes, per-save id maps, and the session manifest. This page is the plan from design decision D10 of the `add-mod-loader` change, written out so the local pieces can be checked against it.

## Goals

1. A vanilla client or server never receives a byte that the vanilla game would not send.
2. A Kyubu client and a Kyubu server agree on the mods and on every registry number before the world loads.
3. Mod code does not change: the same channel API works over loopback and over the network.

## Host

The same proxy loads into `Server.exe`. The server keeps the vanilla simulation; Kyubu adds mod code on the server side. What is missing today is a verified server symbol map (the tick and model functions of `Server.exe` are candidates), which is reverse-engineering work, not a design change. [cuwo](https://github.com/matpow2/cuwo), an open re-implementation of the server, remains an alternative host whose packet documentation Kyubu can reuse.

## Discovery: the meta port

A Kyubu server listens on **game port + 1** (default 12346) for a meta query, next to the game's port 12345.

1. Before the game connects, the client's `connect` hook opens a TCP connection to the meta port with a **1 s timeout**.
2. The client sends one line: `KYUBU-META 1\n` (protocol version).
3. The server answers with one line of JSON and closes:

```json
{ "kyubu": 1, "session": "<random 128-bit token, hex>", "manifest": { ...session manifest... } }
```

No answer, a refused connection or anything else within the timeout means **vanilla server**: the client applies the vanilla rules from [sides-and-networking.md](sides-and-networking.md) (block `requires_server` mods, `KY_E_NO_REMOTE` for channels) and connects normally.

## Handshake: comparing manifests

The `manifest` is exactly what `api->session_manifest()` returns today:

| Field | Compared how |
|---|---|
| `build` | Must be a build the client can talk to (same game protocol; v0.1.1 client and server builds pair up). |
| `loader_api` | Same major version. |
| `protocol` | Same Kyubu network protocol version (1). |
| `mods[]` (`id`, `version`, `sides`, `requires_server`) | Every server mod that has a client side must be present on the client with a version that satisfies the server's. Client mods with `requires_server = true` must be on the server. Client-only cosmetic mods are allowed. |
| `registries` (name → hash) | Compared after the id map is applied (below). A mismatch means the two sides would number content differently; the client refuses to join and names the registry. |

When the mod lists do not match, the client shows the differences (missing mods, version conflicts) and does not connect. It never tries to download mods.

## Binding the game connection

Only after a successful handshake may Kyubu bytes travel on the game connection, and only on a connection known to belong to a Kyubu peer:

1. The client connects to the game port as usual and sends the vanilla handshake.
2. Its first Kyubu packet is `BIND(session token)`. The server matches the token from the meta exchange to this socket. An unknown token closes the connection.
3. From then on both sides may send **extension packets**. They use a packet id outside the range vanilla uses, so they are identified before the game's parser sees them: the `net.recv_raw` path strips them out of the stream and hands them to Kyubu; the game only ever sees vanilla packets.

A vanilla client never sends `BIND`, so the server never sends it extension packets.

## Id map sync

The server is the authority for numbering. After binding:

1. The server sends its current world's id map, the same JSON Kyubu writes next to single-player saves today:

```json
{ "format": 1, "registries": { "kyubu:models": { "mymod:crate": 2569 }, "kyubu:text": { "mymod:greeting": 0 } } }
```

2. The client applies it with the same code that applies a save's id map (`Registries::apply_idmap`): entries keep the server's numbers, entries the client does not have become placeholders, new client-only entries move to free numbers.
3. The client recomputes its registry hashes and compares them with the server's manifest. Equal hashes → continue; the client fires `registry.remapped` if numbers moved, then `lifecycle.world_enter`.
4. This happens before the client loads the world, so no numeric id is used before it is agreed.

## Channels over the network

Channel semantics stay the ones the loopback transport already enforces, so single-player-tested mods behave the same online:

| Rule (from the loopback transport) | Network implementation |
|---|---|
| Delivered on the receiver's next update, never synchronously | Received extension packets are queued and pumped at the start of the next frame / tick. |
| Ordered per channel | One TCP stream per connection keeps order. |
| ≤ 64 KiB payload | Frames larger than one packet are split and reassembled. |
| Client → server, server → client or all | Server fans out to every bound connection for `KY_CONN_ALL_CLIENTS`; unbound (vanilla) clients are skipped. |
| Connection ids | The server assigns ids from 2 up per bound connection (0 = server, 1 = the local client in single-player). |

Extension frame layout: `u16 packet id (Kyubu), u32 length, u16 channel name length, channel name (UTF-8), payload`.

## What exists today

| Piece | State |
|---|---|
| Logical sides, host modes, side-checked API | Implemented and tested. |
| Channels with loopback transport, ordering, size limit, no synchronous delivery | Implemented and tested. |
| Session manifest | Implemented; logged on every world enter. |
| Registry snapshots, hashes, per-save id maps with placeholders | Implemented and tested. |
| `requires_server` blocking against vanilla servers | Implemented and tested against a local vanilla `Server.exe`. |
| Channels between `Server.exe` and a joined `Cube.exe` | Built (add-skills-unleashed-multiplayer 10.4): one Kyubu packet on the game connection, packet id `0x4B594255`, then `u32 length, u16 name length, name, payload`; read at each receive loop's default case, written by each send thread between game packets, delivered at the next frame or tick with the loopback rules. Only with a join gate in force ([sides-and-networking.md](sides-and-networking.md), "Joined games"). Tested in unit tests and one join replay (`channel-transport-server`), with one client. |
| Handshake | The join gate stands in for it: a client and a server with different gate mods never connect, so a vanilla peer never gets a Kyubu packet. |
| Meta port, `BIND`, extension packets beside vanilla clients, id map push | Not built. A mixed server (Kyubu and vanilla clients on one game) needs them. |
| Verified `Server.exe` symbols for the World step, hits, movement, the network loops and player shots | Verified (probes with `client = "join"`): `cube::World::worldStepSite`, `hitCreature`, `aiTickSite`, `moveStepSite`, `Net::connectionReceiveNextSite`, `connection_send_pass_site`, `Net::receiveProjectileSite`. Models on `Server.exe`: candidates only. |
