/**
 * quartz_storage.h — Layered Storage Architecture for Quartz Full Nodes
 *
 * Three-tier storage designed for reliability on ESP32 hardware:
 *
 *   Tier 0: On-chip flash  — Firmware + keys (NVS, encrypted)
 *   Tier 1: FRAM (SPI)     — Chain tip, UTXO commitment, recent headers
 *   Tier 2: USB Flash OTG  — Full block history (bulk storage)
 *
 * FRAM is the anchor — atomic writes, zero wear, instant commit.
 * USB flash is the archive — high capacity, rebuildable from mesh.
 *
 * Storage Layout (FRAM — 256KB W25Q256 or FM25V256):
 *
 *   Offset  Size     Content
 *   ------  -------  ------------------------------------------
 *   0x0000  32       Chain tip hash (SHA-256 of best block header)
 *   0x0020  4        Chain height (uint32_t LE)
 *   0x0024  4        Total chain work (uint32_t LE, log2 difficulty sum)
 *   0x0028  32       UTXO set commitment (Merkle root of UTXO tree)
 *   0x0048  4        UTXO count (uint32_t LE)
 *   0x004C  2        Storage magic (0x515A = "QZ")
 *   0x004E  2        Storage version (uint16_t LE)
 *   0x0050  4        FRAM write sequence (incremented each commit)
 *   0x0054  12       Reserved (zeros)
 *   0x0060  80×256   Last 256 block headers (20 KB)
 *   0x5060  64×4     Last 64 block difficulties (256 bytes)
 *   0x5160  ~190KB   UTXO snapshot (sorted, hashed for commitment)
 *
 *   Total used: ~210 KB of 256 KB
 *
 * USB Flash Layout (FAT32 or littlefs partition):
 *
 *   /quartz/blocks/     — Raw block files (blocks_000000.bin, etc.)
 *   /quartz/headers/    — Header-only files for SPV (80 bytes each)
 *   /quartz/utxo/       — UTXO set snapshots (periodic)
 *   /quartz/peers/      — Peer list + banlist
 *   /quartz/index/      — Block height → file offset index
 *
 * SPV Mode (no USB flash):
 *   FRAM stores 256 headers (~20KB). Miner validates headers only.
 *   Full validation delegated to paired phone or Tier 3 node.
 *
 * Pruned Mode (USB flash):
 *   Keeps last 2016 blocks (one retarget period) in full.
 *   Older blocks → headers only. UTXO set kept current.
 *   Can serve historical headers but not historical txs.
 *
 * Full Mode (USB flash, 64GB+):
 *   All blocks since genesis. True full node.
 *   10-year projection: ~5.3 GB (avg 10 txs/block).
 */

#ifndef QUARTZ_STORAGE_H
#define QUARTZ_STORAGE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "quartz.h"

/* ============ Constants ============ */

#define QZ_STORAGE_MAGIC      0x515A   /* "QZ" */
#define QZ_STORAGE_VERSION    1

/* FRAM layout */
#define FRAM_ADDR_TIP_HASH    0x0000
#define FRAM_ADDR_HEIGHT      0x0020
#define FRAM_ADDR_CHAIN_WORK  0x0024
#define FRAM_ADDR_UTXO_ROOT   0x0028
#define FRAM_ADDR_UTXO_COUNT  0x0048
#define FRAM_ADDR_MAGIC       0x004C
#define FRAM_ADDR_VERSION     0x004E
#define FRAM_ADDR_SEQ         0x0050
#define FRAM_ADDR_RESERVED    0x0054
#define FRAM_ADDR_HEADERS     0x0060   /* 256 headers × 80 bytes = 20 KB */
#define FRAM_ADDR_DIFFS       0x5060   /* 64 difficulties × 4 bytes */
#define FRAM_ADDR_UTXO        0x5160   /* UTXO snapshot region */

#define FRAM_MAX_HEADERS      256
#define FRAM_MAX_DIFFS        64
#define FRAM_HEADER_SIZE      80
#define FRAM_UTXO_REGION_SIZE (256 * 1024 - FRAM_ADDR_UTXO)  /* ~190 KB */

/* Storage modes */
typedef enum {
    QZ_STORAGE_SPV = 0,       /* Headers only (FRAM, no USB) */
    QZ_STORAGE_PRUNED = 1,    /* Last 2016 blocks + all headers (FRAM + USB) */
    QZ_STORAGE_FULL = 2,      /* All blocks since genesis (FRAM + USB) */
} qz_storage_mode_t;

/* Node sync state */
typedef enum {
    QZ_SYNC_BOOT = 0,         /* Just started, no data */
    QZ_SYNC_LOADING = 1,      /* Reading FRAM snapshot */
    QZ_SYNC_CATCHUP = 2,      /* Syncing headers from mesh */
    QZ_SYNC_SYNCED = 3,       /* At chain tip, mining */
    QZ_SYNC_STALE = 4,        /* Behind tip, need resync */
} qz_sync_state_t;

/* ============ FRAM Commit Structure ============ */

typedef struct {
    uint8_t  tip_hash[32];        /* Best block hash */
    uint32_t height;              /* Chain height */
    uint32_t chain_work;          /* Cumulative log2 work */
    uint8_t  utxo_root[32];       /* UTXO set Merkle root */
    uint32_t utxo_count;          /* Number of UTXO entries */
    uint16_t magic;               /* QZ_STORAGE_MAGIC */
    uint16_t version;             /* Schema version */
    uint32_t write_seq;           /* Incremented each commit */
} qz_fram_meta_t;

#define QZ_FRAM_META_SIZE (32 + 4 + 4 + 32 + 4 + 2 + 2 + 4)  /* 84 bytes */

/* ============ API ============ */

/**
 * Initialize storage subsystem.
 * Detects FRAM + USB flash, loads metadata.
 *
 * @param spi_host  SPI host for FRAM (HSPI or VSPI)
 * @param cs_pin    GPIO pin for FRAM chip select
 * @return QZ_OK on success, error code on failure
 */
qz_err_t quartz_storage_init(int spi_host, int cs_pin);

/**
 * Check if USB flash is connected and mounted.
 * @return true if USB storage available
 */
bool quartz_storage_usb_mounted(void);

/**
 * Get current storage mode based on available hardware.
 * SPV if no USB, PRUNED or FULL if USB present.
 */
qz_storage_mode_t quartz_storage_mode(void);

/**
 * Commit chain state to FRAM (atomic).
 * Writes tip hash, height, UTXO root, increments sequence.
 * This is the ONLY function that writes to FRAM metadata region.
 *
 * @param tip_hash   32-byte best block hash
 * @param height     Chain height
 * @param utxo_root  32-byte UTXO set commitment
 * @param utxo_count Number of UTXOs
 * @return QZ_OK on success
 */
qz_err_t quartz_storage_commit(
    const uint8_t *tip_hash,
    uint32_t height,
    const uint8_t *utxo_root,
    uint32_t utxo_count
);

/**
 * Read current chain state from FRAM.
 * @param meta  Output metadata structure
 * @return QZ_OK if valid, QZ_ERR_NOT_FOUND if uninitialized
 */
qz_err_t quartz_storage_get_meta(qz_fram_meta_t *meta);

/**
 * Store a block header in FRAM ring buffer.
 * Automatically wraps at FRAM_MAX_HEADERS.
 *
 * @param height   Block height
 * @param header   80-byte raw header
 * @return QZ_OK on success
 */
qz_err_t quartz_storage_store_header(uint32_t height, const uint8_t *header);

/**
 * Get a block header from FRAM.
 * Only available for last 256 blocks.
 *
 * @param height   Block height
 * @param header   Output 80-byte buffer
 * @return QZ_OK if in FRAM, QZ_ERR_NOT_FOUND if too old
 */
qz_err_t quartz_storage_get_header(uint32_t height, uint8_t *header);

/**
 * Store full block to USB flash (bulk storage).
 * No-op in SPV mode.
 *
 * @param height  Block height
 * @param data    Serialized block
 * @param len     Block size in bytes
 * @return QZ_OK on success, QZ_ERR_NO_USB if unavailable
 */
qz_err_t quartz_storage_store_block(
    uint32_t height,
    const uint8_t *data,
    size_t len
);

/**
 * Read full block from USB flash.
 *
 * @param height    Block height
 * @param buf       Output buffer
 * @param buf_len   Buffer capacity
 * @param out_len   Actual block size
 * @return QZ_OK on success
 */
qz_err_t quartz_storage_get_block(
    uint32_t height,
    uint8_t *buf,
    size_t buf_len,
    size_t *out_len
);

/**
 * Check if we have a full block (vs header only).
 * In SPV mode, always returns false.
 * In PRUNED mode, true for last 2016 blocks.
 * In FULL mode, true for all blocks.
 *
 * @param height Block height
 * @return true if full block data available
 */
bool quartz_storage_has_block(uint32_t height);

/**
 * Get sync state.
 */
qz_sync_state_t quartz_storage_sync_state(void);

/**
 * Set sync state (called by networking/mining code).
 */
void quartz_storage_set_sync_state(qz_sync_state_t state);

/**
 * Wipe all storage (factory reset).
 * Erases FRAM metadata + USB flash /quartz/ directory.
 * Requires physical button hold (enforced by caller).
 *
 * @return QZ_OK on success
 */
qz_err_t quartz_storage_wipe_all(void);

/**
 * Verify USB flash integrity.
 * Scans block files, checks against FRAM height.
 * Called on boot if USB was uncleanly unmounted.
 *
 * @param[out] last_good_height  Highest verified block
 * @return QZ_OK if consistent, QZ_ERR_CORRUPT if rebuild needed
 */
qz_err_t quartz_storage_verify_usb(uint32_t *last_good_height);

/**
 * Rebuild USB flash index from block files.
 * Called after corruption detected or new USB inserted.
 *
 * @return QZ_OK on success
 */
qz_err_t quartz_storage_rebuild_index(void);

/**
 * Get storage statistics.
 */
typedef struct {
    qz_storage_mode_t mode;
    uint32_t total_headers;     /* Headers in FRAM */
    uint32_t total_blocks;      /* Full blocks on USB */
    uint32_t usb_free_kb;       /* Free space on USB */
    uint32_t fram_used_bytes;   /* FRAM bytes used */
    uint32_t fram_total_bytes;  /* FRAM total size */
    uint32_t write_sequence;    /* FRAM commit count */
} qz_storage_stats_t;

void quartz_storage_get_stats(qz_storage_stats_t *stats);

#endif /* QUARTZ_STORAGE_H */
