// RadioLink.h — shared-memory contract between the RoN game mod (writer)
// and the TeamSpeak 3 plugin (reader). Include from BOTH projects.
// Layout is packed and versioned; bump RTR_VERSION on any change.
#pragma once
#include <stdint.h>

#define RTR_SHM_NAME    "Local\\RoNTacticalRadio"
// UDP loopback transport: same struct sent as one datagram per tick.
// Needed when game (Proton/Wine) and TeamSpeak (native Linux) can't share
// Windows shared memory; also serves as a fallback on Windows.
#define RTR_UDP_PORT    39440
#define RTR_TALK_UDP_PORT 39441
#define RTR_TALK_MAX      8
#define RTR_MAGIC       0x31525452u /* "RTR1" */
#define RTR_VERSION     2u
#define RTR_MAX_PLAYERS 32
#define RTR_NAME_LEN    64
#define RTR_MAX_RADIOS  4

#pragma pack(push, 1)

typedef struct RtrVec3 {
    float x, y, z; // meters, UE world axes (UE cm / 100)
} RtrVec3;

typedef struct RtrPlayer {
    char    name[RTR_NAME_LEN]; // in-game player name, UTF-8, NUL-terminated
    RtrVec3 pos;
    uint8_t alive;              // 0 = dead/incapacitated (future: dead can't talk)
    uint8_t _pad[3];
} RtrPlayer;

typedef struct RtrSharedState {
    uint32_t magic;      // RTR_MAGIC
    uint32_t version;    // RTR_VERSION
    // Seqlock: writer sets odd before writing fields below, even when done.
    // Reader: read seq (retry if odd), copy struct, re-read seq, retry if changed.
    volatile uint32_t sequence;
    uint32_t _pad0;
    uint64_t timestampMs;   // writer clock; reader treats data stale after ~1s

    uint8_t  inGame;        // 0 in menus -> plugin passes audio through untouched
    uint8_t  radioPtt;      // radio PTT key held
    uint8_t  voicePtt;      // proximity-voice key held (gates TS mic)
    uint8_t  activeRadio;   // index into radioFreqKhz
    uint32_t radioFreqKhz[RTR_MAX_RADIOS]; // 0 = slot unused

    char     localName[RTR_NAME_LEN]; // this client's in-game name
    RtrVec3  listenerPos;   // camera position
    RtrVec3  listenerFwd;   // camera forward (unit)
    RtrVec3  listenerUp;    // camera up (unit)

    uint32_t playerCount;   // entries in players[] (includes local player)
    RtrPlayer players[RTR_MAX_PLAYERS];
} RtrSharedState;

// Reverse channel (TS plugin -> game mod, RTR_TALK_UDP_PORT): who is audible
// right now and how loud, so the game mod can drive each character's mouth
// (VoipMouthAlpha) in sync with actual TeamSpeak speech.
typedef struct RtrTalkSpeaker {
    char  name[RTR_NAME_LEN]; // in-game player name
    float amplitude;          // 0..1 smoothed loudness
} RtrTalkSpeaker;

typedef struct RtrTalkMsg {
    uint32_t magic;    // RTR_MAGIC
    uint32_t version;  // RTR_VERSION
    uint32_t count;    // entries in speakers[]
    RtrTalkSpeaker speakers[RTR_TALK_MAX];
} RtrTalkMsg;

#pragma pack(pop)
