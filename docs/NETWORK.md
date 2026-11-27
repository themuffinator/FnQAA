# Online play in retail QAA

How retail QAA (`default.xex`, image at 0x82000000) runs Player Match,
Ranked Match and System Link games, so that M8 can put LAN and internet play
in place of Xbox Live sessions ([PLAN.md](PLAN.md)). The game modules take
part only through the Live traps ([ABI.md](ABI.md), section 5).

Each claim is marked **code** (read from the named retail function, by its
address), **observed** (seen, not proven) or **inferred** (reasoned from the
code, not read from it). Engine function names are identified by their role
and strings against Quake III 1.32; QNET's are the retail strings' own.

## Summary

- **Client-server, with one console as the server.** QAA keeps Quake III
  1.32's netcode. The hosting console runs a listen server (`map`) and the
  others connect to it as Quake III clients. There is no host migration: the
  match ends when the host leaves (`$hostquit_msg`,
  `QNET_E_SESSION_ENDED_BY_HOST`; no migration code or strings, **observed**).
- **Xbox Live sets up the session; Quake III carries the game.** A layer the
  strings call QNET runs the Live or System Link session (lobby, invites,
  QoS, voice). When the session starts, a client turns the host's `XNADDR`
  into an IP address and sends Quake III's own `connect` to it.
- **One UDP socket on port 1010.** QNET's socket carries both its own traffic
  and Quake III's packets. On the Xbox 360, title UDP between consoles goes
  through XNet's security associations, so only consoles holding the
  session key can reach each other (**inferred**, from the XNet calls below
  and how XNet works).
- **Authentication is Xbox Live's, not Quake III's.** The CD-key check is
  gone: the server answers `getchallenge` at once and never asks an
  authorize server. Who a player is comes from Live sign-in, and whether
  their packets arrive at all from the session key.

## QNET

QNET's globals start at 0x82C01E20 (**code**): +0x0C is the QNET object
(called through its vtable), +0x1294 and +0x129C the session's `XNKID` and
`XNKEY`, +0x20BC the socket. A block of event handles hangs off the pointer
at 0x82C01C04; the strings name them `h_IsLocalPlayerHost`,
`h_QNETInLobbyState`, `h_QNETInPlayGameState`, `h_LiveQOSSpeedCheckCompleted`
and others, but which handle sits at which offset is not yet mapped.

Session states, from the name lookup at 0x8217EF90 (**code**):

| Value | State |
|---|---|
| 0 | `QNET_STATE_IDLE` |
| 1 | `QNET_STATE_SESSION_HOSTING` |
| 2 | `QNET_STATE_SESSION_JOINING` |
| 3 | `QNET_STATE_GAME_LOBBY` |
| 4 | `QNET_STATE_SESSION_REGISTERING` |
| 5 | `QNET_STATE_SESSION_STARTING` |
| 6 | `QNET_STATE_GAME_PLAY` |
| 7 | `QNET_STATE_SESSION_ENDING` |
| 8 | `QNET_STATE_SESSION_LEAVING` |
| 9 | `QNET_STATE_SESSION_DELETING` |

Errors are `QNET_E_*` strings (`SESSION_FULL`, `NOT_SIGNED_IN_TO_LIVE`,
`DISJOINT_CONNECTIVITY`, `VERSION_MISMATCH` ...; **observed**).

## Hosting (0x8217D8D8)

**code** unless marked:

1. Without a signed-in profile it prints `[QNET] Attempting to create a host
   session without a valid profile logged in.` and fails with 0x80004005.
2. It reads `ui_sessionType`, `ui_gametype`, `ui_currentMap`,
   `g_weaponStay`, `fraglimit` or `capturelimit`, `timelimit`, `g_spSkill`,
   `ui_minBotPlayers`, `ui_privateSlots` and `sv_maxclients` into the
   session's context and property records. The first two contexts are
   0x800A and 0x800B, Xbox Live's game type and game mode; game type is 0
   when `ui_sessionType` is 3 and 1 otherwise, which in the XDK's values
   makes session type 3 Ranked (**inferred**).
3. When the cvar named `ui_gametype` has integer 1, it unregisters the old
   key at +0x1294 if one is registered, then makes a new one with
   `XNetCreateKey` and registers it with `XNetRegisterKey` (+0x1294 `XNKID`,
   +0x129C `XNKEY`). On failure it fires `HostServerFailed` to the menus.
   Why this depends on `ui_gametype` is open.

## Starting the match (0x82180B28)

The handler sets r15 when the event at +0x34 of the block at 0x82C01C04 is
signalled. Read with the branches below, that event is "the local player is
the host" (**inferred**). When the state moves from 4 or 5 to 6 (0x8218162C,
**code**):

- **Host** (r15 set): 0x8219F5D8 sets `g_gametype`, `ui_sessionType`,
  `bot_enable`, `bot_minplayers` and `bot_nochat` from the menu's settings
  (`ui_gametype`, `ui_minBotPlayers`; the exact mapping is **observed**),
  then queues `wait ; wait ; map %s` with `ui_currentMap`: a Quake III
  listen server.
- **Client**: calls GetPlayerInAddr.

The same handler fires `JoinServer`, `JoinServerFailed` and `ServerStart`
to the menus and reads `xb_joiningInvite` for invites (**observed**).

## GetPlayerInAddr (0x8217F760)

**code**:

1. Calls QNET object vtable +0x30 for a player (`player is NULL in
   GetPlayerInAddr.` if none), then that player's vtable +0x1C for its
   `XNADDR` (`Could not get player XNADDR in GetPlayerInAddr.`).
2. `XNetXnAddrToInAddr(xnaddr, &qnet[+0x1294] /* XNKID */, &inaddr)`
   (`Could not convert player XNADDR to IN_ADDR in GetPlayerInAddr.`), and
   stores the address at 0x82A6B7E8.
3. Formats `%d.%d.%d.%d:%d` with port 1010 and queues `connect %s\n` with
   Cbuf_AddText (0x82119708).

Only the client branch calls it (its one caller is 0x821816B8), so the player
is the host (**inferred**; which method vtable +0x30 is has not been read).

## The socket (0x82179840)

QNET's network start-up (0x82179BC8) registers `net_port` with default
`1010` and flag 0x20 (`CVAR_LATCH`), then calls 0x82179840 with it
(**code**). That opens `socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)`, binds it
to `net_port` on any address, makes it non-blocking, sets `SO_BROADCAST`
(for System Link) and the send and receive buffer sizes (`setsockopt`
0x20, 0x1002 and 0x1001). The start-up then passes four bytes from
`XNetRandom` to 0x821DCE68, probably a random seed (**code**; the seed is
**inferred**).

It is the engine's only socket (the other `socket` callers, 0x82471828 and
0x82489698, are in the linked Xbox libraries). Quake III's sends reach it:
NET_SendPacket (0x82128F68), called by Netchan_Transmit (0x82128628), the
fragment sender (0x821284C8) and NET_OutOfBandPrint (0x82129050), and
NET_OutOfBandData (0x82129100), called by CL_CheckForResend (0x82109180)
for `connect`, all go out through QNET's sendto (0x82179750) (**code**).

## Authentication

- **No CD key.** SV_GetChallenge (0x8212DBF8, called by
  SV_ConnectionlessPacket 0x82135A88) stores a new challenge and answers
  `challengeResponse %i` at once (**code**). Quake III 1.32's request to the
  authorize server is gone: the engine holds no `getIpAuthorize` string.
  SV_Init (0x82134BA8) still registers `sv_strictAuth`, which nothing reads
  there. SV_GetChallenge answers nothing while `ui_singlePlayerActive` is
  set.
- `authorize.quake3arena.com` is used only by `banUser` (SV_Ban_f,
  0x8212D080), which would resolve it with `XNetDnsLookup` (0x82129240), and
  `Awaiting CD key authorization` is a leftover client message
  (**observed**).
- Players must be signed in for QNET to host or join (step 1 under Hosting,
  `QNET_E_NOT_SIGNED_IN_TO_LIVE`). The console and the user prove who they
  are to Live at sign-in, and the session key decides which consoles can
  exchange packets (**inferred**, platform behaviour).
- Ranked results go to Live leaderboards (`g_lbRanked`, game trap 66).
  No arbitration strings were found, so the host's results are probably
  trusted as reported; not confirmed.

## XNet imports

The engine calls XAM's network exports through one-instruction wrappers
that insert the caller type 1 (`NetDll_*`). Ordinals are named from
xenia's table, checked against CL_InitUI (0x8218E490) calling
`XNetGetEthernetLinkStatus` as [UI.md](UI.md) records (**code**).

| Wrapper | Ordinal | Function | Engine callers |
|---|---|---|---|
| 0x82412B48 | 0x36 | `XNetCreateKey` | 0x8217D8D8 |
| 0x82412B58 | 0x37 | `XNetRegisterKey` | 0x8217D8D8; library 0x8247E230 |
| 0x82412B68 | 0x38 | `XNetUnregisterKey` | 0x8217D8D8, 0x82177500 |
| 0x82412B78 | 0x39 | `XNetXnAddrToInAddr` | 0x8217F760 |
| 0x82412BD8 | 0x43 | `XNetDnsLookup` | 0x82129240 |
| 0x82412C70 | 0x47 | `XNetQosServiceLookup` | 0x82176370 |
| 0x82412CA8 | 0x4B | `XNetGetEthernetLinkStatus` | 0x8218E490, 0x82174E38 |
| 0x82412D90 | 0x03 | `socket` | 0x82179840 |
| 0x82412DF0 | 0x0B | `bind` | 0x82179840 |
| 0x82412EC0 | 0x18 | `sendto` | 0x82179750 |

Every other XNet caller (`XNetQosListen`, `XNetQosLookup`, `XNetConnect`,
`XNetInAddrToXnAddr`, `XNetGetTitleXnAddr` ...) is at 0x8246xxxx-0x8249xxxx,
in the statically linked Xbox Live libraries, which also hold the
`/xmatch/`, `/xstats/` and `/xuacs/` service paths (**observed**).

## Open

- Which method QNET object vtable +0x30 is, and which event handle sits at
  +0x34 of the block at 0x82C01C04.
- Why the host's key creation depends on `ui_gametype`, and whether the
  Live library (0x8247E230) registers the session key as well.
- The XAM session imports (ordinals 0x1A0-0x3D1) are not named, so
  arbitration is unconfirmed.

## For FnQAA (M8)

The protocol under QNET is Quake III 1.32's (ABI.md, section 6), so FnQL's
server and client can replace steps 3 and 4 of the match start as they are:
`map` on the host and `connect ip:port` on the clients. What has to be
replaced is the session layer: hosting, finding and joining a match, the
lobby, invites and leaderboards. Each part becomes a documented PC deviation.
Keeping 1010 as the default `net_port` would match retail; whether to do that
or keep FnQL's 27960 is a choice for M8.
