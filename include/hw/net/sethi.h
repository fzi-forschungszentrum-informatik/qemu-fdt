/*
 * FZI Sethi (2nd generation) ethernet controller emulation
 *
 * Copyright (c) 2023 FZI Forschungszentrum Informatik
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_SETHI_H
#define HW_SETHI_H

#include "hw/net/tlmdio.h"
#include "net/net.h"
#include "qemu/queue.h"
#include <stdint.h>

#define SETHI21_MAX_RING_SIZE 16
#define SETHI21_MAX_PACKET_SIZE 2000

#define SETHI21_BD_CPU_FLAG_EOF (1 << 2)
#define SETHI21_BD_CPU_FLAG_CRC (1 << 1)
#define SETHI21_BD_CPU_FLAG_INT (1 << 0)

#define SETHI21_BD_SHARED_FLAG_CSUM (1 << 5)
#define SETHI21_BD_SHARED_FLAG_READY (1 << 4)
#define SETHI21_BD_SHARED_FLAG_EOF (1 << 3)
#define SETHI21_BD_SHARED_FLAG_CRC (1 << 2)
#define SETHI21_BD_SHARED_FLAG_HWERR (1 << 1)
#define SETHI21_BD_SHARED_FLAG_CORRUPT (1 << 0)

#define SETHI21_CSUM_ETH_HEADER_SIZE 14
#define SETHI21_CSUM_ETHTYPE_OFFSET 12
#define SETHI21_CSUM_ETHTYPE_IPV4 0x0800
#define SETHI21_CSUM_ETHTYPE_IPV6 0x86DD
#define SETHI21_CSUM_ETHTYPE_ARP 0x0806
#define SETHI21_CSUM_IPV4_HEADER_SIZE 20
#define SETHI21_CSUM_IPV6_HEADER_SIZE 40

#define SETHI21_CSUM_IPV4_LENGTH 0x10
#define SETHI21_CSUM_IPV4_PROTOCOL 0x17
#define SETHI21_PROTOCOL_V4_ICMP 0x01
#define SETHI21_PROTOCOL_V4_TCP 0x06
#define SETHI21_PROTOCOL_V4_UDP 0x11

#define SETHI21_CSUM_IPV6_LENGTH 0x12
#define SETHI21_CSUM_IPV6_PROTOCOL 0x14
#define SETHI21_PROTOCOL_V6_ICMP 0x3a
#define SETHI21_PROTOCOL_V6_TCP 0x06
#define SETHI21_PROTOCOL_V6_UDP 0x11


enum {
    SETHI_NIC_PERIPHIDENT = 0x00, // R 32
    SETHI_NIC_FEATURES = 0x10, // R 32
    SETHI_NIC_IRQ_STATE = 0x50, // RW SETHI_NIC_IRQ_MASK
    SETHI_NIC_IRQ_MASK = 0x54, // RW SETHI_NIC_IRQ_MASK
    SETHI_NIC_TX_STATE = 0x60, // RW 4
    SETHI_NIC_RX_STATE = 0x64, // RW 6
    SETHI_NIC_DMA_CTRL = 0x70, // RW 6
    SETHI_NIC_TX_FRAME_COUNTER = 0x80, // RW 32 CLEAR
    SETHI_NIC_RX_FRAME_COUNTER = 0x88, // RW 32 CLEAR
    SETHI_NIC_TX_DESC_COUNTER = 0x90, // RW 32 CLEAR
    SETHI_NIC_RX_DESC_COUNTER = 0x98, // RW 32 CLEAR
    SETHI_NIC_TX_RING_CTRL = 0x100, // RW 32
    SETHI_NIC_TX_BASE_ADDR0 = 0x108, // RW 28
    SETHI_NIC_TX_BASE_ADDR1 = 0x10C, // RW 28
    SETHI_NIC_TX_RING_ENTRIES = 0x110, // RW 5
    SETHI_NIC_TX_RING_DELIMITER = 0x118, // RW 32
    SETHI_NIC_RX_RING_CTRL = 0x180, // RW 5
    SETHI_NIC_RX_BASE_ADDR0 = 0x188, // RW 28
    SETHI_NIC_RX_BASE_ADDR1 = 0x18C, // RW 28
    SETHI_NIC_RX_RING_ENTRIES = 0x190, // RW 5
    SETHI_NIC_RX_RING_DELIMITER = 0x198, // RW 32
    SETHI_NIC_MODE = 0x200, // RW 32
    SETHI_NIC_MAC0 = 0x208, // RW 32
    SETHI_NIC_MAC1 = 0x20C, // RW 32
    SETHI_NIC_MAX = 0x1000
};

enum {
    SETHI_NIC_MII_SUPPORT = 0x000001, // FEATURES R
    SETHI_NIC_GMII_SUPPORT = 0x000002, // FEATURES R
    SETHI_NIC_MAC_WRITABLE = 0x000010, // FEATURES R
    SETHI_NIC_MAX_RING = 0x00FF00, // FEATURES R
    SETHI_NIC_DMA_ADDR_BITS = 0xFF0000, // FEATURES R

    SETHI_NIC_TX_IRQ_FRONTEND = 0x0001, // IRQ_STATE ONE_TO_CLEAR
    SETHI_NIC_TX_IRQ_HALTED = 0x0002, // IRQ_STATE ONE_TO_CLEAR
    SETHI_NIC_TX_IRQ_FETCH_DONE = 0x0004, // IRQ_STATE ONE_TO_CLEAR
    SETHI_NIC_TX_IRQ_DESC_DONE = 0x0008, // IRQ_STATE ONE_TO_CLEAR
    SETHI_NIC_TX_IRQ_FRAME_DONE = 0x0010, // IRQ_STATE ONE_TO_CLEAR
    SETHI_NIC_TX_IRQ_DESC_ERROR = 0x0020, // IRQ_STATE ONE_TO_CLEAR
    SETHI_NIC_RX_IRQ_FRONTEND = 0x0100, // IRQ_STATE ONE_TO_CLEAR
    SETHI_NIC_RX_IRQ_HALTED = 0x0200, // IRQ_STATE ONE_TO_CLEAR
    SETHI_NIC_RX_IRQ_FETCH_DONE = 0x0400, // IRQ_STATE ONE_TO_CLEAR
    SETHI_NIC_RX_IRQ_DESC_DONE = 0x0800, // IRQ_STATE ONE_TO_CLEAR
    SETHI_NIC_RX_IRQ_FRAME_DONE = 0x1000, // IRQ_STATE ONE_TO_CLEAR
    SETHI_NIC_RX_IRQ_DESC_ERROR = 0x2000, // IRQ_STATE ONE_TO_CLEAR
    SETHI_NIC_RX_IRQ_FIFO_OVERFLOW = 0x4000, // IRQ_STATE ONE_TO_CLEAR
    SETHI_NIC_RX_IRQ_DATA_ERROR = 0x8000, // IRQ_STATE ONE_TO_CLEAR
    SETHI_NIC_IRQ_DMA_ERROR = 0x10000, // IRQ_STATE ONE_TO_CLEAR
    SETHI_NIC_IRQ_MASK_MASK = 0x1FF3F,

    SETHI_NIC_DMA_PAUSE = 0x01, // RW
    SETHI_NIC_DMA_BUSY = 0x02, // R
    SETHI_NIC_DMA_ERROR = 0x04, // RW ONE_TO_CLEAR
    SETHI_NIC_DMA_ABORT = 0x08, // RW
    SETHI_NIC_DMA_TX_DESC_PATH = 0x10, // R
    SETHI_NIC_DMA_RX_DESC_PATH = 0x20, // R

    SETHI_NIC_FRONTEND_ENABLE = 0x01, // RW
    SETHI_NIC_NRESET_ENABLE = 0x02, // RW
    SETHI_NIC_MII_MODE = 0x04, // RW
    SETHI_NIC_UP = 0x08, // R
    SETHI_NIC_OVERFLOW = 0x10, // RW
    SETHI_NIC_HALT_ON_OVERFLOW = 0x20, // RW

    SETHI_NIC_RING_ENABLE = 0x000001, // RW
    SETHI_NIC_RING_ACTIVE = 0x000002, // R
    SETHI_NIC_RING_ERROR = 0x000004, // RW ONE_TO_CLEAR
    SETHI_NIC_DISCARD_CORRUPTED = 0x000010, // RW
    SETHI_NIC_RING_DESC_TAG = 0x00FF00, // R
    SETHI_NIC_RING_DESC_VALID = 0x010000, // R

    SETHI_NIC_RING_ENTRIES = 0x01F, // RW
    SETHI_NIC_RING_DELIMITER = 0x01F, // RW
    SETHI_NIC_RING_TRIGGER = 0x80000000, // W

    SETHI_NIC_MODE_FCS_TRUNC = 0x01, // RW
    SETHI_NIC_MODE_BROADCASTS = 0x02, // RW
    SETHI_NIC_MODE_PROM = 0x04 // RW
};

enum {
    SETHI_NIC_PERIPHIDENT_VAL = 0x49544553, // "SETI"
    SETHI_NIC_DEFAULT_MAC = 0x21715E66FECA // "SETI"
};


#define TYPE_FZI_SETHI "fzi.sethi21"

typedef struct sethi21_bd {
    uint32_t addr_lo;
    uint16_t addr_hi;
    uint8_t csum_start;
    uint8_t csum_offset;
    uint16_t buf_len;
    uint8_t cpu_flags;
    uint8_t tag;
    uint16_t act_len;
    uint16_t shared_flags;
} __attribute__((packed, aligned(16))) sethi21_bd_t;

typedef struct sethi21_csum_state {
    unsigned int frame_pos;
    uint32_t temp_sum;
    uint8_t start;
    uint8_t offset;
} sethi21_csum_state_t;

typedef struct sethi21_rs {
    uint8_t state;
    uint32_t frame_cntr;
    uint32_t desc_cntr;
    uint32_t ring_ctrl;
    uint64_t base_addr;
    uint8_t ring_entries;
    uint8_t ring_delimiter;

    sethi21_bd_t desc[SETHI21_MAX_RING_SIZE];
    bool desc_valid[SETHI21_MAX_RING_SIZE];
    uint8_t ring_read;
    uint8_t ring_write;

    uint8_t pck_buffer[SETHI21_MAX_PACKET_SIZE];
    uint16_t pck_len;

    sethi21_csum_state_t csum;
} sethi21_rs;

typedef struct sethiState21 {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    qemu_irq irq;

    MemoryRegion mmio;
    NICState *nic;
    NICConf conf;

    MdioState *phyMdio;

    uint32_t irq_state;
    uint32_t irq_mask;
    uint8_t dma_ctrl;
    uint8_t mode;
    uint64_t mac_addr;
    uint16_t max_ring_size;

    sethi21_rs rx;
    sethi21_rs tx;
} sethiState21;

sethiState21 *sethi_if_create(MemoryRegion *address_space, hwaddr base,
                              qemu_irq irq);
#endif
