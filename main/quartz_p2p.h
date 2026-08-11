/**
 * Quartz Gateway P2P — TCP Gossip Protocol
 *
 * Gateway nodes (VPS, home server, Raspberry Pi) connect to each other
 * over TCP to form the backbone of the Quartz network.
 *
 * The gateway P2P layer handles:
 *   - Peer discovery (DNS seeds, hardcoded bootnodes, gossip)
 *   - Block relay (new blocks broadcast to all peers)
 *   - Transaction relay (mempool sync)
 *   - Chain sync (headers-first, then block bodies)
 *   - Fork choice (longest chain wins, tie → first seen)
 *
 * This runs on the NODE side (Python reference node), not the ESP32.
 * ESP32 miners reach gateways via WiFi (HTTP API) or LoRa mesh.
 *
 * Protocol version: 1
 * Transport: TCP with length-prefixed JSON messages
 *
 * License: MIT
 */

#ifndef QUARTZ_P2P_H
#define QUARTZ_P2P_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * This is a specification header.
 *
 * The actual implementation lives in the reference node (Python).
 * See: reference-node/quartz/p2p.py
 *
 * Protocol specification below.
 */

// ============================================================
// Message Types
// ============================================================

typedef enum {
    QZ_P2P_HELLO        = 1,   // Handshake: version, height, capabilities
    QZ_P2P_HELLO_ACK    = 2,   // Handshake response
    QZ_P2P_GET_HEADERS  = 3,   // Request block headers
    QZ_P2P_HEADERS      = 4,   // Block headers response (batch, up to 500)
    QZ_P2P_GET_BLOCK    = 5,   // Request single block by hash
    QZ_P2P_BLOCK        = 6,   // Full block data
    QZ_P2P_NEW_BLOCK    = 7,   // Announce newly mined block (gossip)
    QZ_P2P_NEW_TX       = 8,   // Announce new transaction (gossip)
    QZ_P2P_PING         = 9,   // Keepalive
    QZ_P2P_PONG         = 10,  // Keepalive response
    QZ_P2P_GET_PEERS    = 11,  // Request peer list
    QZ_P2P_PEERS        = 12,  // Peer list response
    QZ_P2P_BYE          = 13,  // Graceful disconnect
    QZ_P2P_FORK_ALERT   = 14,  // Potential fork detected
} qz_p2p_msg_type_t;

// ============================================================
// Protocol Constants
// ============================================================

#define QZ_P2P_PROTOCOL_VERSION   1
#define QZ_P2P_DEFAULT_PORT       21100
#define QZ_P2P_MAX_PEERS          32
#define QZ_P2P_MAX_MSG_SIZE       (4 * 1024 * 1024)  // 4MB
#define QZ_P2P_PING_INTERVAL_SEC  30
#define QZ_P2P_PEER_TIMEOUT_SEC   120
#define QZ_P2P_CONNECT_TIMEOUT_MS 5000

// ============================================================
// Hello Message
// ============================================================

/*
{
    "type": "hello",
    "version": 1,
    "chain_height": 1581,
    "best_hash": "0000001a2b3c...",
    "capabilities": ["full_node", "miner_api", "lora_bridge"],
    "user_agent": "quartz-node/0.1.0",
    "port": 21100
}
*/

// ============================================================
// Wire Format
// ============================================================

/*
All messages are length-prefixed JSON:
    [4 bytes: payload length (big-endian uint32)]
    [N bytes: JSON payload]

Maximum message size: 4MB (for full block batches).
Messages larger than 4MB are rejected and peer disconnected.

The 4-byte length prefix prevents partial-read confusion and
allows pre-allocating receive buffers.
*/

#ifdef __cplusplus
}
#endif

#endif // QUARTZ_P2P_H
