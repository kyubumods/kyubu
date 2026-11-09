# Network

Atlas page for the `network` subsystem: the multiplayer protocol, the client's connection in Cube.exe, and the
server, its connections and packet loops in Server.exe. Reference pages:
[docs/game/network.md](../../docs/game/network.md).

## Purpose

Multiplayer is TCP on port 12345 (`Net::connectPortSite`, verified). Every packet starts with a uint32 id, and the
ids handled by the two binaries match cuwo's lists (`cuwo/packet.py`) exactly: the client handles 0 to 5, 10 and 15
in its receive loop and 16 to 18 while connecting; the server handles 0 and 6 to 12 in each connection's loop and 17
while accepting. The main traffic is the creature update (id 0): each side sends the creature data it owns, as a
bit mask followed by the fields that changed, compressed with zlib.

## Key classes

- [`cube::Server`](../../docs/game/types/cube_Server.md) (verified as a class, RTTI; Server.exe only): 84 bytes,
  built by `main`. Its constructor and destructor are below.
- [`cube::Connection`](../../docs/game/types/cube_Connection.md) (verified as a class, RTTI; Server.exe only): 176
  bytes, one per client, built by the accept loop.
- Cube.exe has neither class (6.2): the client's network state lives in the game controller, with the socket at
  GameController+0x8006C4, the receive and send thread handles at +0x800588 and +0x80058C, and the connected flag
  at +0x80057D (the receive loop runs while it is set).
- [`cube::CreatureData`](../../docs/game/types/cube_CreatureData.md) (from cuwo): the payload of the creature
  update, read field by field under the mask (0x4CCD40 from a buffer; 0x4CD180 reads the same format straight from
  the socket, its caller is not found yet).

## Entry points

All candidate unless noted.

Cube.exe:

- [client_connect](../../docs/game/network.md) (0x46FE90): on the game controller, with the host name. Called by
  `cube::GameController::onKeyDown()` (/connect, [input](input.md)) and by 0x481AB0 (the start menu's join, which
  reads its edit field). It resolves the host, connects (`Net::connectPortSite` is inside), sends the client version
  and reads the answer: the join (16), "Error: Server has different version" (17), "Error: Server is full." (18) or
  anything else, "Connection error". A vanilla `Server.exe` answers a wrong version with a bare 3 and never sends 17, so
  a modded client sees "Connection error" there ([protocol-version](../probes/protocol-version.toml)).
  Then it starts the two threads.
- [client_receive_loop](../../docs/game/network.md) (0x46BBA0): the receive thread. It reads the id and switches on
  0 to 5, 10 and 15 (the jump table at 0x46D5E0; any other id is ignored).
  - `Net::clientReceiveNextSite` (0x46D0DD, verified, add-skills-unleashed-multiplayer 10.4): the loop test that every
    case and the default reach, once per packet. The packet id is at [EBP-0x1220], the disconnected byte at [EBP-0x11D1],
    and [EBX] is the controller. An unknown id comes here with nothing more read, so its body is still on the socket.
- [client_send_loop](../../docs/game/network.md) (0x46A070): the send thread (`send` and `Sleep` under the game
  lock). For the creature update it copies the player's `cube::CreatureData` and writes the packet with
  [creature_update_write](../../docs/game/network.md), passing the copy it last sent. The pacing is not read yet.
  - `Net::clientSendPassSite` (0x46A376, verified, 10.4): once per pass, after all of the pass's game packets are out.
    [EDI] is the controller, the failed byte is at [EBP-0x2425]; the socket is `cube::GameController.socket`.
- [creature_update_write](../../docs/game/network.md) (0x4CD840, 2026-09-30):
  - It compares **every** field of the creature data with the last sent copy (all of them on a full update) and writes
    a u64 mask plus the changed fields, in cuwo's order.
  - The fields cuwo calls unused are compared and sent too: not_used_1 to 6, not_used7/8, unknown_or_not_used1/2/4,
    not_used19 and not_used20.
- [creature_update_read](../../docs/game/network.md) (0x4CCD40): the buffer reader of the same format.
- [client_disconnect](../../docs/game/network.md) (0x471C40): closes the socket and waits for both threads. Called
  by /disconnect, by `cube::GameController::onMouseUp()` and by `cube::GameController::update()`.

Server.exe:

- `cube::Server::Server()` (0x422BF0), called by `main`, and `cube::Server::~Server()` (0x4235B0, "Stopping
  server...").
- [run_server](../../docs/game/core.md) (verified, [core](core.md)): socket, bind, listen, then the threads.
- [server_accept_loop](../../docs/game/network.md) (0x425290): the accept thread. Reads the version (17), refuses a
  full server or a wrong version, sends the join, builds a `cube::Creature` and a `cube::Connection`
  (`cube::Connection::Connection()`, 0x422680), and starts that client's receive loop.
  - The version test is a single comparison with 3. The join then sends the entity id and the full 0x1168-byte
    creature data.
- [connection_receive_loop](../../docs/game/network.md) (0x425E10): one thread per client, a switch on 0 and 6 to 12.
  Case 0 reads the update with [creature_update_read](../../docs/game/network.md) (Server.exe 0x415810) into a copy of
  the player's creature data. That includes the spare fields.
  - `Net::connectionReceiveNextSite` (0x4266DD, verified, 10.4): the loop test that every case and the default reach. The
    packet id is at [EBP-0x13C8], the disconnected byte at [EBP-0x13A5]; EDI is the thread's closure (+4 the connections,
    +8 this client's slot). The client is `cube::Connection`, 0xB0 bytes: `cube::Connection.socket` and
    `cube::Connection.creature_id` (the player's creature id).
  - Case 9, the shoot packet (add-skills-unleashed-multiplayer 10.9a, design D16; probe
    [server-projectile-packet-0720](../probes/server-projectile-packet-0720.toml)): it receives the 0x70-byte
    `cube::Projectile` and, under the World lock, pushes it on `cube::Server`'s list at Server+0x44.
    `Net::receiveProjectileSite` (0x4265B3, verified) is the moment the projectile is at EBP-0x110, before the push.
- `cube::Connection::~Connection()` (0x4234E0).
- [connection_send_loop](../../docs/game/network.md) (verified in 7.5, alias `Server::simLoop`; 0x423BC0): one thread
  per client, not the simulation. The accept loop starts it through a std::function (vtable 0x55B220, whose slot 2
  jumps here). While the client's connection is open it locks the World, collects what changed, unlocks, sends it to
  that client's socket, and paces itself to 20 ms; at the end it closes the socket. It calls 0x423170, which calls 12
  functions in a row (one per queued packet type?). [connection_send_pass_site](../../docs/game/network.md) (verified,
  alias `Server::tickSite`) is the instruction after the pacing, reached once per pass.
- `cube::Server::takeBullets` (0x4281D0, candidate; the name is Infinite-Cubes'): called by the main loop at 0x54A0C2,
  after `cube::World::update` and before the broadcast. It locks the World, moves the whole list at Server+0x44 into the
  step output's projectile list (+0x20), and empties it; the broadcast sends that list to every client. Its neighbours
  0x428230 and 0x428170 fill the step input's hits and buffs before the update.

## Data flow

1. **Connecting.** The client's [client_connect](../../docs/game/network.md) and the server's
   [server_accept_loop](../../docs/game/network.md) exchange the version (17) and the join (16), which carries the
   player's entity id and creature data; a full server answers 18.
2. **Client to server.** Per cuwo: 0 creature update, 6 interact, 7 hit, 8 passive, 9 shoot, 10 chat, 11 chunk
   discovered, 12 sector discovered. [connection_receive_loop](../../docs/game/network.md) merges the creature data
   (0) into the connection's creature, queues hits, passives and shots (7 to 9) on the connection for the
   simulation, and sends chat (10) on to every connection.
   - A shot (9) is only relayed: `cube::Server::takeBullets` hands it to the broadcast, and nothing puts it in
     `cube::World.projectiles`, so the server's World never flies it. Replay server-projectile-packet-0720 run 1: one packet
     for the client's one arrow, and no pass of the projectile step in the 6 s after it. Other clients draw the relayed
     arrow; the damage comes from the shooter's own hit packets (7).
3. **Server to client.** Per cuwo: 0 creature update, 1 (unused), 2 update finished, 3 airship (unused), 4 server
   update, 5 current time, 10 chat, 15 seed. In [client_receive_loop](../../docs/game/network.md), 0 looks the
   creature up in the world (World at GameController+0x2DC), creates a `cube::Creature` (0x1E60 bytes, with
   `cube::Creature::Creature()`) for an unknown id, and applies the masked data, but never to the player's own
   creature (GameController+0x8006C8); 2 records the time between updates (0x76904C).
4. **Kyubu's own packet** (add-skills-unleashed-multiplayer 10.4, design D10; the loader's `kyubu.channel`): id 0x4B594255,
   then `u32 length, u16 name length, name, payload`, on the game's own connection. Each side reads it at its receive
   loop test (`Net::clientReceiveNextSite`, `Net::connectionReceiveNextSite`) and writes queued ones at its send pass
   (`Net::clientSendPassSite`, `connection_send_pass_site`), so it never splits a game packet. It is sent only while the
   join gate is in force (D4): a peer without the same gate mods never joins. Replay channel-transport-server run 1:
   pings, a 60,000-byte echo and a broadcast, both ways.
5. **Leaving.** [client_disconnect](../../docs/game/network.md) on the client; the server ends the connection when
   its receive fails (to confirm).

## Client and server

- The two sides share no network code: none of the entries above correlate between the builds, and only Server.exe
  has the network classes.
- Movement is client-authoritative, as the [travel](travel.md) page expected from cuwo: the client never applies a
  server update to its own creature, and the server takes each client's creature data as it comes.
- Hits (7) are sent by the attacking client and queued by the server; whether the server checks them is 9.5's
  question.
- Single-player: Cube.exe has no server classes, so the "in-process server" of 7.2 is not a `cube::Server`. How
  single-player simulates the world without one is open.

## Hook points of interest

- [connection_receive_loop](../../docs/game/network.md)'s switch (candidate): a server-side mod can inspect or drop
  client packets (movement checks, chat commands).
- [client_receive_loop](../../docs/game/network.md) (candidate): the client-side view of every server packet.
- [client_connect](../../docs/game/network.md) (candidate): the moment a client joins; a mod handshake could follow
  the version packet.
- `Net::receiveProjectileSite` (verified): a joined player's shot on the server, on the connection's thread with the World
  lock held. A mod can copy it there and act on it at the next World step (the loader's `player_shots`). Copying leaves the
  relay unchanged.
- `cube::Server::takeBullets` (candidate): the last moment before the server relays its clients' shots.
- The protocol version:
  - The client sends 3 and [server_accept_loop](../../docs/game/network.md) accepts only 3. Changing the number on both
    sides gives mod-only servers: vanilla clients and modded clients refuse each other. Probed on the client side
    (2026-10-01): a client that sends another version gets a bare 3 from a vanilla server, the server's console says "Wrong
    client version", and the client shows "Connection error", not the version text (that is for a reply 17 the server never
    sends). The version is the immediate of the store at `Net::connectVersionSite` (0x470159, +6).
  - A mod can also carry data in the spare `cube::CreatureData` fields: the client sends them and the server reads
    them. Whether the game ever overwrites one needs a probe, and they are not kept in the character save
    ([saves](saves.md)).
- A new packet id would need both switches; unknown ids are ignored by the client and the server (both switches send them to the
  loop test, which reads nothing more). The loader's channel transport uses one ([primitives](primitives.md), "Network transport").

## Open questions

- The item packets: cuwo expects `cube::ItemData` inside the interact (6) and server-update (4) packets
  ([items](items.md)); the handlers for 4 and 6 are not read yet.
- What else [client_send_loop](../../docs/game/network.md) sends, and at what rate. The creature mask is answered:
  [creature_update_write](../../docs/game/network.md).
- Who calls 0x4CD180 (the socket reader of the masked creature data) and
  [creature_update_read](../../docs/game/network.md).
- Whether any game code writes the spare creature data fields (a runtime watch on one of them while playing and
  joined).
- How a joined client receives kill XP (cuwo's kill action), see [combat](combat.md).
- The server's send side: 0x423170 and its 12 callees, and where the server update (4) is built.
- The server's handling of unknown ids is answered (10.4): the default goes to the loop test and reads nothing more, on
  both sides. The fan-out of Kyubu's packet to more than one joined client is replayed only with two clients.
- Single-player: whether a local server runs inside Cube.exe (Cube.exe's `WinMain()` references the string server),
  or the client simulates the world itself (7.2).

## Depth

reconnaissance, reviewed 2026-09-30

## Level rework sites

The `level_rework` mod keeps modded and vanilla games apart with a protocol version of its own, and the server checks a player's level
(change `add-level-rework`, design D7; tasks 3.6 and 8.3). The rules and the texts are in the `level_rework` repository.
Addresses are `Cube.exe`, then `Server.exe`; all are `candidate`.

- **Client.** The version is the immediate of the store in [client_connect](../../docs/game/network.md) (`Net::connectVersionSite`,
  0x470159 +6): the mod patches it from 3 to `0x4B4C0001` (mod symbol `level_rework:protocol_version_send`, 0x47015F). Three observation
  points follow the reply switch: `Net::connectJoinSite` (0x470286, reached once per join), `Net::connectMismatchSite` (0x4709AA, the
  version error of reply 17) and `Net::connectUnknownReplySite` (0x4701FD, "Connection error": any reply other than 16, 17 and 18).
- **Server.** In [server_accept_loop](../../docs/game/network.md):
  - `Net::acceptVersionSite` (0x42554E) is the compare of the client's version with 3. The 3 is an 8-bit immediate, so the mod cannot
    patch it. A mid hook rewrites the stack slot just before the compare instead: the mod's version becomes 3 and a vanilla 3 becomes 0.
  - `Net::acceptWrongVersionReplySite` (0x42555F) is the refusal: it sends a 4-byte 3, waits 500 ms and closes. The mod patches the
    reply (the immediate at 0x425565) to 17 and sends 8 bytes, the 17 and its version, so a vanilla client shows "Server has different
    version" in place of "Connection error".
  - `Net::acceptJoinSite` (0x4255AC) is the join, reached when the compare passes. It sends the 4-byte 0x10 reply.
- **The level check.** `creature_update_level_site` (0x415B05) is the instruction after the call in
  [creature_update_read](../../docs/game/network.md) that reads the player's level (data+0x180) into the copy of its creature data. The mod
  clamps a level outside 1 to 100 in the copy, sets the receive loop's exit flag and shuts the socket down (the flag alone leaves the
  socket open). The site was first mapped at 0x415B47, which follows the read of another field (+0x190); it was corrected on 2026-10-04.
- **Probes.** `protocol-version` (a modded client against a vanilla server: "Connection error"), `server-protocol-modded` (both modded: the
  join, and a level 105 written into the server's copy: disconnected) and `server-protocol-vanilla-client` (a vanilla client: the game's
  version text). No client sent the 105: the client has no probe for it, and its load check clamps.
