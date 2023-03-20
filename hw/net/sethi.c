/*
 * FZI Sethi (2nd generation) ethernet controller emulation
 *
 * Copyright (c) 2023 FZI Forschungszentrum Informatik
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdint.h>

#include "qemu/osdep.h"
#include "hw/core/cpu.h"

#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/net/sethi.h"
#include "hw/net/tlmdio.h"
#include "net/checksum.h"
#include "net/net.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/queue.h"
#include "system/address-spaces.h"

#define FZI_SETHI(obj) OBJECT_CHECK(sethiState21, (obj), TYPE_FZI_SETHI)

#define SEND_DUMMY_PCK 0

#define ERROR_ENABLE 0
#define ERROR(...)                 \
    do {                           \
        if (ERROR_ENABLE)          \
            qemu_log(__VA_ARGS__); \
    } while (0)

#define DEBUG_ENABLE 0
#define DEBUG(...)                 \
    do {                           \
        if (DEBUG_ENABLE)          \
            qemu_log(__VA_ARGS__); \
    } while (0)

#define TRACE_ENABLE 0
#define TRACE(...)                 \
    do {                           \
        if (TRACE_ENABLE)          \
            qemu_log(__VA_ARGS__); \
    } while (0)

static inline void eth_sync_irq(sethiState21 *s)
{
    qemu_set_irq(s->irq, (s->irq_state & ~s->irq_mask) != 0);
}

static inline void eth_trigger_irq(sethiState21 *s, uint32_t flags)
{
    if (flags & s->irq_mask) {
        s->irq_state |= flags;
        TRACE("MASKED IRQ cur: 0x%x state: 0x%x mask: 0x%x\n", flags,
              s->irq_state, s->irq_mask);
        return;
    }

    if (s->irq_state & flags) {
        TRACE("ALREADY IRQ cur: 0x%x state: 0x%x mask: 0x%x\n", flags,
              s->irq_state, s->irq_mask);
        return;
    }

    s->irq_state |= flags;
    if (s->irq_state & ~s->irq_mask) {
        TRACE("TRIGGER IRQ cur: 0x%x state: 0x%x mask: 0x%x\n", flags,
              s->irq_state, s->irq_mask);
        assert(s->irq != NULL);
        qemu_irq_raise(s->irq);
    } else {
        TRACE("ALREADY TRIGGERED IRQ cur: 0x%x state: 0x%x mask: 0x%x\n", flags,
              s->irq_state, s->irq_mask);
    }
}

static inline void send_pck(sethiState21 *s)
{
    DEBUG("SENDING %x:%x:%x:%x:%x:%x <- %x:%x:%x:%x:%x:%x\n",
          s->tx.pck_buffer[0], s->tx.pck_buffer[1], s->tx.pck_buffer[2],
          s->tx.pck_buffer[3], s->tx.pck_buffer[4], s->tx.pck_buffer[5],
          s->tx.pck_buffer[6], s->tx.pck_buffer[7], s->tx.pck_buffer[8],
          s->tx.pck_buffer[9], s->tx.pck_buffer[10], s->tx.pck_buffer[11]);
    qemu_send_packet(qemu_get_queue(s->nic), (void *)s->tx.pck_buffer,
                     s->tx.pck_len);
    qemu_flush_queued_packets(qemu_get_queue(s->nic));
}

static inline uint64_t bd_get_addr(struct sethi21_bd bd)
{
    return ((uint64_t)bd.addr_hi << 32L) | bd.addr_lo;
}

static void handle_tx_csum(sethi21_rs *tx_ring, struct sethi21_bd *bd)
{
    const bool csum_flag =
        (bd->shared_flags & SETHI21_BD_SHARED_FLAG_CSUM) != 0;
    const bool eof = (bd->cpu_flags & SETHI21_BD_CPU_FLAG_EOF) != 0;
    const bool sof = tx_ring->csum.frame_pos == 0;

    if (sof && (!csum_flag || bd->csum_start == 0)) {
        return; // No csum for this frame
    }

    if (sof && csum_flag) {
        tx_ring->csum.start = bd->csum_start;
    }

    if (eof && csum_flag) {
        tx_ring->csum.offset = bd->csum_offset;
    }

    if (tx_ring->csum.start > 0) {
        const int start_distance =
            tx_ring->csum.start - tx_ring->csum.frame_pos;
        if (start_distance >= 0) {
            if (start_distance < bd->buf_len) {
                // Start within current fragment
                tx_ring->csum.temp_sum = net_checksum_add_cont(
                    bd->buf_len - start_distance,
                    &tx_ring->pck_buffer[tx_ring->csum.start], 0);
            }
        } else {
            tx_ring->csum.temp_sum += net_checksum_add_cont(
                bd->buf_len, &tx_ring->pck_buffer[tx_ring->csum.frame_pos],
                tx_ring->csum.frame_pos - tx_ring->csum.start);
        }

        tx_ring->csum.frame_pos += bd->buf_len;
    }

    if (eof) {
        if (tx_ring->csum.offset != 0 &&
            tx_ring->csum.offset < tx_ring->pck_len) {
            const uint16_t checksum =
                net_checksum_finish_nozero(tx_ring->csum.temp_sum);
            TRACE("Injecting checksum 0x%x at offset 0x%x\n", checksum,
                  tx_ring->csum.offset);
            tx_ring->pck_buffer[tx_ring->csum.offset] =
                (uint8_t)(checksum >> 8);
            if (tx_ring->pck_len > (tx_ring->csum.offset + 1)) {
                tx_ring->pck_buffer[tx_ring->csum.offset + 1] =
                    (uint8_t)(checksum & 0xFF);
            }
        }

        tx_ring->csum.offset = 0;
        tx_ring->csum.start = 0;
        tx_ring->csum.frame_pos = 0;
    }
}

static bool check_rx_checksum(const uint8_t *rx_pkt, size_t len)
{
    if (len <= SETHI21_CSUM_ETH_HEADER_SIZE)
        return false;

    const uint16_t ethtype =
        be16toh(*((uint16_t *)&rx_pkt[SETHI21_CSUM_ETHTYPE_OFFSET]));
    TRACE("Checksum for %x\n", ethtype);

    uint16_t hdr_ofs;
    uint32_t sum = 0;
    if (ethtype == SETHI21_CSUM_ETHTYPE_IPV4) {
        hdr_ofs = SETHI21_CSUM_IPV4_HEADER_SIZE + SETHI21_CSUM_ETH_HEADER_SIZE;
        if (len <= hdr_ofs) {
            return false;
        }
        uint8_t protocol = rx_pkt[SETHI21_CSUM_IPV4_PROTOCOL];
        TRACE("PROTOCOL IPV4 ");
        if (protocol == SETHI21_PROTOCOL_V4_ICMP) {
            TRACE("ICMP\n");
        } else if (protocol == SETHI21_PROTOCOL_V4_TCP ||
                   protocol == SETHI21_PROTOCOL_V4_UDP) {
            if (protocol == SETHI21_PROTOCOL_V4_TCP)
                TRACE("TCP\n");
            else
                TRACE("UDP\n");

            /* Add pseudo header */
            /* Add sender/receiver */
            sum = net_checksum_add(8, (uint8_t *)&rx_pkt[hdr_ofs - 8]);
            sum += protocol;
            /* Add length */
            sum += be16toh(*((uint16_t *)&rx_pkt[SETHI21_CSUM_IPV4_LENGTH])) -
                   SETHI21_CSUM_IPV4_HEADER_SIZE;
        } else {
            TRACE(" Unknown protocol 0x%x\n", protocol);
            return false;
        }
    } else if (ethtype == SETHI21_CSUM_ETHTYPE_IPV6) {
        hdr_ofs = SETHI21_CSUM_IPV6_HEADER_SIZE + SETHI21_CSUM_ETH_HEADER_SIZE;
        if (len <= hdr_ofs) {
            return false;
        }
        uint8_t protocol = rx_pkt[SETHI21_CSUM_IPV6_PROTOCOL];
        TRACE("PROTOCOL IPV6 ");
        if (protocol == SETHI21_PROTOCOL_V6_ICMP ||
            protocol == SETHI21_PROTOCOL_V6_TCP ||
            protocol == SETHI21_PROTOCOL_V6_UDP) {
            if (protocol == SETHI21_PROTOCOL_V6_ICMP)
                TRACE("ICMP\n");
            else if (protocol == SETHI21_PROTOCOL_V6_TCP)
                TRACE("TCP\n");
            else
                TRACE("UDP\n");

            /* Add pseudo header */
            /* Add sender/receiver */
            sum = net_checksum_add(32, (uint8_t *)&rx_pkt[hdr_ofs - 32]);
            sum += protocol;
            /* Add length */
            sum += be16toh(*((uint16_t *)&rx_pkt[SETHI21_CSUM_IPV6_LENGTH]));
        } else {
            TRACE(" Unknown protocol 0x%x\n", protocol);
            return false;
        }
    } else if (ethtype == SETHI21_CSUM_ETHTYPE_ARP) {
        TRACE("PROTOCOL ARP - Skipping\n");
        return false;
    } else {
        return false;
    }

    if (len <= hdr_ofs) {
        return false;
    }

#ifdef RX_HEXDUMP_PACKET
    int l = 0;
    for (uint8_t *p = (uint8_t *)&rx_pkt[l]; p < &rx_pkt[len]; ++p) {
        if (l % 16 == 0)
            qemu_log("%04x ", l);
        qemu_log("%02x ", *p);
        if (l % 8 == 7)
            qemu_log(" ");
        if (l % 16 == 15)
            qemu_log("\n");
        ++l;
    }
    qemu_log("\n");
#endif

    sum += net_checksum_add_cont(len - hdr_ofs, (uint8_t *)&rx_pkt[hdr_ofs],
                                 hdr_ofs);
    bool res = (net_checksum_finish(sum) == 0);
    if (res) {
        TRACE("Checksum success\n");
    } else {
        qemu_log("QEMU sethi21: Checksum failed");
    }
    return res;
}


#ifdef SEND_DUMMY_PCK
static void fzi_sethi_send_dummy_packet(sethiState21 *s)
{
    // send dummy packet so the tap device knows our mac
    uint8_t dummyPck[] = {
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        s->conf.macaddr.a[0],
        s->conf.macaddr.a[1],
        s->conf.macaddr.a[2],
        s->conf.macaddr.a[3],
        s->conf.macaddr.a[4],
        s->conf.macaddr.a[5],
        0x08,
        0x06, // ARP
        0x00,
        0x01,
        0x08,
        0x00,
        0x06,
        0x04,
        0x00,
        0x01, // Ethernet (1) IPv6 6 4 request (1)
        s->conf.macaddr.a[0],
        s->conf.macaddr.a[1],
        s->conf.macaddr.a[2],
        s->conf.macaddr.a[3],
        s->conf.macaddr.a[4],
        s->conf.macaddr.a[5],
        192,
        168,
        0,
        10, // sender ip
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00, // recv mac
        192,
        168,
        0,
        16, // recv ip
    };

    qemu_send_packet(qemu_get_queue(s->nic), (void *)dummyPck,
                     sizeof(dummyPck));
    qemu_flush_queued_packets(qemu_get_queue(s->nic));
}
#endif

static void parse_desc(sethiState21 *s, bool force, bool tx)
{
    sethi21_rs *rs = tx ? &s->tx : &s->rx;
    bool fetchDone = false;
    bool descDone = false;
    bool frameDone = false;

    if (!(rs->ring_ctrl & SETHI_NIC_RING_ENABLE))
        return;

    if (!force && rs->ring_write == rs->ring_delimiter)
        return;

    if (tx) {
        TRACE("T");
    } else {
        TRACE("R");
    }
    TRACE("X MEM READ %p %p %ld\n", (uint8_t *)rs->base_addr,
          (uint8_t *)rs->desc, sizeof(rs->desc));

    address_space_read(&address_space_memory, (hwaddr)rs->base_addr,
                       MEMTXATTRS_UNSPECIFIED, rs->desc, sizeof(rs->desc));
    uint8_t valid = 0;
    uint8_t len = 0;

    uint8_t ring_size = MIN(s->max_ring_size, rs->ring_entries);

    if (ring_size == 0) {
        return;
    }

    if (force)
        len = ring_size;
    else
        len = ((rs->ring_delimiter + ring_size) - rs->ring_write) % ring_size;

    for (int i = 0; i < len; ++i) {
        uint8_t cur = rs->ring_write;
        TRACE("%d (0x%lx): addr: %" PRIx64
              " buffer: %x cpu_flags: %x tag: %x act_len %x shared %x\n",
              cur, rs->base_addr + cur * sizeof(rs->desc[cur]),
              bd_get_addr(rs->desc[cur]), rs->desc[cur].buf_len,
              rs->desc[cur].cpu_flags, rs->desc[cur].tag, rs->desc[cur].act_len,
              rs->desc[cur].shared_flags);

        // SHOULD CHECK IF REALLY VALID
        valid++;
        rs->desc_valid[rs->ring_write] = true;
        rs->ring_write = (rs->ring_write + 1) % ring_size;

        if (tx == true) {
            hwaddr shared_offset =
                ((hwaddr) & (rs->desc[cur].act_len)) - ((hwaddr)rs->desc);
            hwaddr shared_regs = rs->base_addr + shared_offset;

            if (rs->desc[cur].shared_flags & SETHI21_BD_SHARED_FLAG_READY) {
                /* Input for sent buffer */
                uint16_t buf_len = rs->desc[cur].buf_len;

                if (rs->pck_len + buf_len <= SETHI21_MAX_PACKET_SIZE) {
                    address_space_read(&address_space_memory,
                                       (hwaddr)bd_get_addr(rs->desc[cur]),
                                       MEMTXATTRS_UNSPECIFIED,
                                       rs->pck_buffer + rs->pck_len, buf_len);

                    rs->frame_cntr++;
                    rs->pck_len += buf_len;
                    TRACE("CPU FLAGS: 0x%x\n", rs->desc[cur].cpu_flags);
                    handle_tx_csum(rs, &rs->desc[cur]);
                    if (rs->desc[cur].cpu_flags & SETHI21_BD_CPU_FLAG_EOF) {
                        send_pck(s);
                        TRACE("Sent packet %d\n", rs->pck_len);
                        rs->desc[cur].act_len = rs->pck_len;
                        rs->pck_len = 0;
                        frameDone = true;
                    } else {
                        TRACE("Waiting for more packet data %d\n", rs->pck_len);
                    }

                    if (rs->desc[cur].cpu_flags & SETHI21_BD_CPU_FLAG_INT) {
                        descDone = true;
                    }

                    rs->desc[cur].shared_flags &= ~SETHI21_BD_SHARED_FLAG_READY;
                    rs->desc[cur].shared_flags |= SETHI21_BD_SHARED_FLAG_EOF;
                    rs->desc[cur].shared_flags |= SETHI21_BD_SHARED_FLAG_CRC;
                } else {
                    rs->pck_len = 0;
                    rs->desc[cur].shared_flags |= SETHI21_BD_SHARED_FLAG_HWERR;
                }
            }

            // write shared register back
            address_space_write(&address_space_memory, shared_regs,
                                MEMTXATTRS_UNSPECIFIED, &rs->desc[cur].act_len,
                                sizeof(rs->desc[cur].act_len) +
                                    sizeof(rs->desc[cur].shared_flags));

            fetchDone = true;
        } else {
            fetchDone = true;
        }
        // #define SETHI21_BD_CPU_FLAG_EOF         (1 << 2)
        // #define SETHI21_BD_CPU_FLAG_CRC         (1 << 1)
        // #define SETHI21_BD_CPU_FLAG_INT         (1 << 0)

        // #define SETHI21_BD_SHARED_FLAG_READY    (1 << 4)
        // #define SETHI21_BD_SHARED_FLAG_EOF      (1 << 3)
        // #define SETHI21_BD_SHARED_FLAG_CRC      (1 << 2)
        // #define SETHI21_BD_SHARED_FLAG_HWERR    (1 << 1)
        // #define SETHI21_BD_SHARED_FLAG_CORRUPT  (1 << 0)
    }

    rs->desc_cntr += valid;

    TRACE("DONE: FETCH %d DESC %d FRAME %d\n", fetchDone, descDone, frameDone);
    if (fetchDone)
        tx ? eth_trigger_irq(s, SETHI_NIC_TX_IRQ_FETCH_DONE) :
             eth_trigger_irq(s, SETHI_NIC_RX_IRQ_FETCH_DONE);
    if (descDone)
        tx ? eth_trigger_irq(s, SETHI_NIC_TX_IRQ_DESC_DONE) :
             eth_trigger_irq(s, SETHI_NIC_RX_IRQ_DESC_DONE);
    if (frameDone)
        tx ? eth_trigger_irq(s, SETHI_NIC_TX_IRQ_FRAME_DONE) :
             eth_trigger_irq(s, SETHI_NIC_RX_IRQ_FRAME_DONE);

    if (tx)
        TRACE("T");
    else
        TRACE("R");
    TRACE("X ctrl: %x frame: %d desc: %d base_addr: %p ring_ctrl: %x entries: "
          "%d delimiter: %d cur: %d\n",
          rs->state, rs->frame_cntr, rs->desc_cntr, (uint8_t *)rs->base_addr,
          rs->ring_ctrl, rs->ring_entries, rs->ring_delimiter, rs->ring_write);
}

static uint64_t eth_read(void *opaque, hwaddr addr, unsigned int size)
{
    uint32_t r = 0;
    sethiState21 *s = opaque;
    uint8_t mask = addr & 0b11;
    addr &= ~mask;

    switch (addr) {
    case SETHI_NIC_PERIPHIDENT:
        r = SETHI_NIC_PERIPHIDENT_VAL;
        break;
    case SETHI_NIC_FEATURES:
        r = SETHI_NIC_MII_SUPPORT | SETHI_NIC_GMII_SUPPORT |
            SETHI_NIC_MAC_WRITABLE | 0x1000 | 0x270000;
        break;
    case SETHI_NIC_IRQ_STATE:
        r = s->irq_state;
        break;
    case SETHI_NIC_IRQ_MASK:
        r = s->irq_mask;
        break;
    case SETHI_NIC_TX_STATE:
        r = s->tx.state;
        break;
    case SETHI_NIC_RX_STATE:
        r = s->rx.state;
        break;
    case SETHI_NIC_DMA_CTRL:
        r = s->dma_ctrl;
        break;
    case SETHI_NIC_TX_FRAME_COUNTER:
        r = s->tx.frame_cntr;
        break;
    case SETHI_NIC_RX_FRAME_COUNTER:
        r = s->rx.frame_cntr;
        break;
    case SETHI_NIC_TX_DESC_COUNTER:
        r = s->tx.desc_cntr;
        break;
    case SETHI_NIC_RX_DESC_COUNTER:
        r = s->rx.desc_cntr;
        break;
    case SETHI_NIC_TX_RING_CTRL:
        r = s->tx.ring_ctrl;
        break;
    case SETHI_NIC_TX_BASE_ADDR0:
        r = s->tx.base_addr & 0xFFFFFFFF;
        break;
    case SETHI_NIC_TX_BASE_ADDR1:
        r = s->tx.base_addr >> 32;
        break;
    case SETHI_NIC_TX_RING_ENTRIES:
        r = s->tx.ring_entries;
        break;
    case SETHI_NIC_TX_RING_DELIMITER:
        r = s->tx.ring_delimiter;
        break;
    case SETHI_NIC_RX_RING_CTRL:
        r = s->rx.ring_ctrl;
        break;
    case SETHI_NIC_RX_BASE_ADDR0:
        r = s->rx.base_addr & 0xFFFFFFFF;
        break;
    case SETHI_NIC_RX_BASE_ADDR1:
        r = s->rx.base_addr >> 32;
        break;
    case SETHI_NIC_RX_RING_ENTRIES:
        r = s->rx.ring_entries;
        break;
    case SETHI_NIC_RX_RING_DELIMITER:
        r = s->rx.ring_delimiter;
        break;
    case SETHI_NIC_MODE:
        r = s->mode;
        break;
    case SETHI_NIC_MAC0:
        r = s->mac_addr & 0xFFFFFFFF;
        break;
    case SETHI_NIC_MAC1:
        r = s->mac_addr >> 32;
        break;
    default:
        break;
    }

    r = r >> (8 * mask);
    switch (size) {
    case 1:
        r &= 0xFF;
        break;
    case 2:
        r &= 0xFFFF;
        break;
    case 4:
        r &= 0xFFFFFFFF;
        break;
    }

    TRACE("%s addr=" HWADDR_FMT_plx " val=%x size=%x\n", __func__, addr, r,
          size);
    return r;
}

static void eth_write(void *opaque, hwaddr addr, uint64_t val64,
                      unsigned int size)
{
    sethiState21 *s = opaque;
    uint32_t value = val64;
    bool force_tx_read = false;
    bool force_rx_read = false;

    uint8_t mask = addr & 0b11;
    if (mask) {
        qemu_log("UNALIGNED WRITE %s addr=" HWADDR_FMT_plx
                 " val=%x -- Crashing\n",
                 __func__, addr, value);
        assert(false);
        return;
    }

    TRACE("%s addr=" HWADDR_FMT_plx " val=%x size=%x\n", __func__, addr, value,
          size);
    switch (addr) {
    case SETHI_NIC_PERIPHIDENT:
        break;
    case SETHI_NIC_FEATURES:
        break;
    case SETHI_NIC_IRQ_STATE:
        s->irq_state &= ~value;
        // eth_trigger_irq(s, 0x00);
        break;
    case SETHI_NIC_IRQ_MASK:
        s->irq_mask = (value & SETHI_NIC_IRQ_MASK_MASK);
        if (~s->irq_mask & s->irq_state) {
            qemu_irq_raise(s->irq);
        }
        break;
    case SETHI_NIC_TX_STATE:
        s->tx.state = (value & 0xF);
        if (s->tx.state & SETHI_NIC_FRONTEND_ENABLE) {
            if (!(s->tx.state & SETHI_NIC_UP)) {
                eth_trigger_irq(s, SETHI_NIC_TX_IRQ_FRONTEND);
            }
            s->tx.state |= SETHI_NIC_UP;
        } else {
            if (s->tx.state & SETHI_NIC_UP) {
                eth_trigger_irq(s, SETHI_NIC_TX_IRQ_FRONTEND);
            }
            s->tx.state &= ~SETHI_NIC_UP;
        }
        break;
    case SETHI_NIC_RX_STATE:
        s->rx.state = (value & 0x3F);
        if (s->rx.state & SETHI_NIC_FRONTEND_ENABLE) {
            if (!(s->rx.state & SETHI_NIC_UP)) {
                eth_trigger_irq(s, SETHI_NIC_RX_IRQ_FRONTEND);

                s->rx.state |= SETHI_NIC_UP;
#ifdef SEND_DUMMY_PCK
                // if (s->conf.peers.ncs[0] &&
                //     strcmp(s->conf.peers.ncs[0]->model , "tap") == 0) {
                fzi_sethi_send_dummy_packet(s);
                //}
#endif
            }
        } else {
            if (s->rx.state & SETHI_NIC_UP) {
                eth_trigger_irq(s, SETHI_NIC_RX_IRQ_FRONTEND);
                s->rx.state &= ~SETHI_NIC_UP;
            }
        }
        break;
    case SETHI_NIC_DMA_CTRL:
        s->irq_state &= ~(0b1001);
        s->irq_state |= (value & 0b1001);
        s->irq_state &= ~(value & 0b100);
        break;
    case SETHI_NIC_TX_FRAME_COUNTER:
        s->tx.frame_cntr = 0;
        break;
    case SETHI_NIC_RX_FRAME_COUNTER:
        s->rx.frame_cntr = 0;
        break;
    case SETHI_NIC_TX_DESC_COUNTER:
        s->tx.desc_cntr = 0;
        break;
    case SETHI_NIC_RX_DESC_COUNTER:
        s->rx.desc_cntr = 0;
        break;
    case SETHI_NIC_TX_RING_CTRL:
        s->tx.ring_ctrl &= ~SETHI_NIC_RING_ENABLE;
        s->tx.ring_ctrl |= (value & (SETHI_NIC_RING_ENABLE));
        s->tx.ring_ctrl &= ~(value & SETHI_NIC_RING_ERROR);

        if (s->tx.ring_ctrl & SETHI_NIC_RING_ENABLE) {
            s->tx.ring_ctrl |= SETHI_NIC_RING_ACTIVE;
        } else {
            s->tx.ring_ctrl &= ~SETHI_NIC_RING_ACTIVE;
        }
        break;
    case SETHI_NIC_TX_BASE_ADDR0:
        s->tx.base_addr =
            (s->tx.base_addr & 0xFFFFFFFF00000000) | (value & 0xFFFFFFF0);
        break;
    case SETHI_NIC_TX_BASE_ADDR1:
        s->tx.base_addr =
            (((uint64_t)value & 0x7FF) << 32) | (s->tx.base_addr & 0xFFFFFFF0);
        break;
    case SETHI_NIC_TX_RING_ENTRIES:
        s->tx.ring_entries = (value & 0x1F);
        break;
    case SETHI_NIC_TX_RING_DELIMITER:
        s->tx.ring_delimiter = (value & 0x1F);

        force_tx_read = value & SETHI_NIC_RING_TRIGGER;
        break;
    case SETHI_NIC_RX_RING_CTRL:
        s->rx.ring_ctrl &=
            ~(SETHI_NIC_RING_ENABLE | SETHI_NIC_DISCARD_CORRUPTED);
        s->rx.ring_ctrl |=
            (value & (SETHI_NIC_RING_ENABLE | SETHI_NIC_DISCARD_CORRUPTED));
        s->rx.ring_ctrl &= ~(value & SETHI_NIC_RING_ERROR);

        if (s->rx.ring_ctrl & SETHI_NIC_RING_ENABLE) {
            s->rx.ring_ctrl |= SETHI_NIC_RING_ACTIVE;
        } else {
            s->rx.ring_ctrl &= ~SETHI_NIC_RING_ACTIVE;
        }
        break;
    case SETHI_NIC_RX_BASE_ADDR0:
        s->rx.base_addr =
            (s->rx.base_addr & 0xFFFFFFFF00000000) | (value & 0xFFFFFFF0);
        break;
    case SETHI_NIC_RX_BASE_ADDR1:
        s->rx.base_addr =
            (((uint64_t)value & 0x7FF) << 32) | (s->rx.base_addr & 0xFFFFFFF0);
        break;
    case SETHI_NIC_RX_RING_ENTRIES:
        s->rx.ring_entries = (value & 0x1F);
        break;
    case SETHI_NIC_RX_RING_DELIMITER:
        s->rx.ring_delimiter = (value & 0x1F);

        force_rx_read = value & SETHI_NIC_RING_TRIGGER;
        break;
    case SETHI_NIC_MODE:
        s->mode = (value & 0x7);
        break;
    case SETHI_NIC_MAC0:
        s->mac_addr = (s->mac_addr & 0xFFFFFFFF00000000) | value;
        break;
    case SETHI_NIC_MAC1:
        s->mac_addr =
            (((uint64_t)value & 0xFFFF) << 32) | (s->mac_addr & 0xFFFFFFFF);
        break;
    default:
        break;
    }

    parse_desc(s, force_rx_read, false);
    parse_desc(s, force_tx_read, true);

    eth_sync_irq(s);
}

static const MemoryRegionOps eth_ops = {
    .read = eth_read,
    .write = eth_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = false },
    .impl = { .min_access_size = 4, .max_access_size = 4, .unaligned = false }
};

static bool siethi21_can_receive(NetClientState *nc)
{
    sethiState21 *s = qemu_get_nic_opaque(nc);
    bool can = (s->rx.state & SETHI_NIC_FRONTEND_ENABLE);
    return can;
}


static ssize_t eth_rx(NetClientState *nc, const uint8_t *buf, size_t size)
{
    sethiState21 *s = qemu_get_nic_opaque(nc);
    ssize_t processed = 0;
    TRACE("Recv\n");

    if (!(siethi21_can_receive(nc))) {
        TRACE("%ld %ld\n", size, size);
        return size;
    }

    if (!(s->mode & SETHI_NIC_MODE_FCS_TRUNC)) {
        qemu_log("No FCS not implemented\n");
        assert(false);
    }

    static const MACAddr broadcast = { .a = { 0xff, 0xff, 0xff, 0xff, 0xff,
                                              0xff } };
    const uint8_t *dest_addr = &buf[0];
    bool is_for_me = (memcmp(dest_addr, &s->mac_addr, sizeof(broadcast)) == 0);
    bool is_broadcast = (memcmp(dest_addr, &broadcast, sizeof(broadcast)) == 0);

    TRACE("Recv package for %02x:%02x:%02x:%02x:%02x:%02x mode %x",
          dest_addr[0], dest_addr[1], dest_addr[2], dest_addr[3], dest_addr[4],
          dest_addr[5], s->mode);

    if (is_for_me || (is_broadcast && (s->mode & SETHI_NIC_MODE_BROADCASTS)) ||
        (s->mode & SETHI_NIC_MODE_PROM)) {
        TRACE(" A package? For me?\n");

        sethi21_rs *rs = &s->rx;
        uint8_t ring_size = MIN(s->max_ring_size, rs->ring_entries);

        while (ring_size != 0 && processed < size) {
            int i = rs->ring_read;
            if (s->rx.desc_valid[i]) {
                hwaddr act_len_addr = rs->base_addr +
                                      ((hwaddr) & (rs->desc[i].act_len)) -
                                      ((hwaddr)rs->desc);
                hwaddr shared_addr = rs->base_addr +
                                     ((hwaddr) & (rs->desc[i].shared_flags)) -
                                     ((hwaddr)rs->desc);

                s->rx.desc_valid[i] = false;
                uint16_t cp_size = MIN(size - processed, rs->desc[i].buf_len);

                TRACE(
                    "USING RX desc %d (0x%lx): addr: %" PRIx64
                    " buffer: %x cpu_flags: %x tag: %x act_len %x shared %x\n",
                    i, rs->base_addr + i * sizeof(rs->desc[i]),
                    bd_get_addr(rs->desc[i]), rs->desc[i].buf_len,
                    rs->desc[i].cpu_flags, rs->desc[i].tag, rs->desc[i].act_len,
                    rs->desc[i].shared_flags);

                address_space_write(
                    &address_space_memory, bd_get_addr(rs->desc[i]),
                    MEMTXATTRS_UNSPECIFIED, buf + processed, cp_size);
                processed += cp_size;

                rs->desc[i].shared_flags &= ~SETHI21_BD_SHARED_FLAG_READY;
                bool frame_done = (processed == size);
                if (frame_done) {
                    if (check_rx_checksum(buf, size)) {
                        TRACE("Valid internet checksum detected\n");
                        rs->desc[i].shared_flags |= SETHI21_BD_SHARED_FLAG_CSUM;
                    } else {
                        TRACE("Invalid internet checksum detected\n");
                        rs->desc[i].shared_flags &=
                            ~SETHI21_BD_SHARED_FLAG_CSUM;
                    }
                    rs->desc[i].shared_flags |=
                        SETHI21_BD_SHARED_FLAG_CRC | SETHI21_BD_SHARED_FLAG_EOF;
                }

                rs->desc[i].act_len = size;
                address_space_write(&address_space_memory, shared_addr,
                                    MEMTXATTRS_UNSPECIFIED,
                                    &rs->desc[i].shared_flags,
                                    sizeof(rs->desc[i].shared_flags));
                address_space_write(
                    &address_space_memory, act_len_addr, MEMTXATTRS_UNSPECIFIED,
                    &rs->desc[i].act_len, sizeof(rs->desc[i].act_len));

                rs->desc_cntr++;
                rs->frame_cntr++;
                rs->ring_read = (rs->ring_read + 1) % ring_size;
                eth_trigger_irq(s, SETHI_NIC_RX_IRQ_DESC_DONE);
                if (frame_done) {
                    eth_trigger_irq(s, SETHI_NIC_RX_IRQ_FRAME_DONE);
                }

            } else {
                ERROR("QEMU sethi21: Dropped package\n");
                eth_trigger_irq(s, SETHI_NIC_RX_IRQ_FIFO_OVERFLOW);
                break;
            }
        }
    } else {
        TRACE("\n");
    }

    TRACE("Processed size %ld process %ld\n", size, processed);
    return size;
}

static void fzi_sethi_reset(DeviceState *dev)
{
    sethiState21 *s = FZI_SETHI(dev);

    s->irq_state = 0x0;
    s->irq_mask = 0x0;
    s->dma_ctrl = 0x0;
    s->mode = 0x0;

    s->tx.state = 0x0;
    s->tx.frame_cntr = 0x0;
    s->tx.desc_cntr = 0x0;
    s->tx.ring_ctrl = 0x0;
    s->tx.base_addr = 0x0;
    s->tx.ring_entries = 0x0;
    s->tx.ring_delimiter = 0x0;
    memset(s->tx.desc, 0, sizeof(s->tx.desc));
    memset(s->tx.desc_valid, 0, sizeof(s->tx.desc_valid));
    s->tx.ring_read = 0;
    s->tx.ring_write = 0;
    memset(s->tx.pck_buffer, 0, sizeof(s->tx.pck_buffer));
    s->tx.pck_len = 0;
    memset(&s->tx.csum, 0, sizeof(s->tx.csum));

    s->rx.state = 0x0;
    s->rx.frame_cntr = 0x0;
    s->rx.desc_cntr = 0x0;
    s->rx.ring_ctrl = 0x0;
    s->rx.base_addr = 0x0;
    s->rx.ring_entries = 0x0;
    s->rx.ring_delimiter = 0x0;
    memset(s->rx.desc, 0, sizeof(s->rx.desc));
    memset(s->rx.desc_valid, 0, sizeof(s->rx.desc_valid));
    s->rx.ring_read = 0;
    s->rx.ring_write = 0;
    memset(s->rx.pck_buffer, 0, sizeof(s->rx.pck_buffer));
    s->rx.pck_len = 0;
    memset(&s->rx.csum, 0, sizeof(s->rx.csum));

    eth_sync_irq(s);
}

static NetClientInfo net_fzi_sethi_info = {
    .type = NET_CLIENT_DRIVER_NIC, .size = sizeof(NICState), .receive = eth_rx,
    .can_receive = siethi21_can_receive
};

static void fzi_sethi_realize(DeviceState *dev, Error **errp)
{
    sethiState21 *s = FZI_SETHI(dev);
    if (SETHI21_MAX_RING_SIZE < s->max_ring_size) {
        qemu_log("QEMU sethi21: MAX_RING_SIZE >16 (%d)! Defaulting to 16",
                 s->max_ring_size);
        s->max_ring_size = SETHI21_MAX_RING_SIZE;
    }

    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&net_fzi_sethi_info, &s->conf,
                          object_get_typename(OBJECT(dev)), dev->id,
                          &dev->mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
    // Use the one qemu found
    // s->mac_addr = SETHI_NIC_DEFAULT_MAC;
    s->mac_addr = (uint64_t)s->conf.macaddr.a[0] << 0 |
                  (uint64_t)s->conf.macaddr.a[1] << 8 |
                  (uint64_t)s->conf.macaddr.a[2] << 16 |
                  (uint64_t)s->conf.macaddr.a[3] << 24 |
                  (uint64_t)s->conf.macaddr.a[4] << 32 |
                  (uint64_t)s->conf.macaddr.a[5] << 40;

    if (s->phyMdio) {
        // TODO: Implement this
    }

    qdev_init_gpio_out(dev, &s->irq, 1);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);

    if (s->conf.peers.ncs[0] &&
        strcmp(s->conf.peers.ncs[0]->model, "tap") == 0) {
        qemu_log("QEMU sethi21: using a tap - MAC %x:%x:%x:%x:%x:%x\n",
                 s->conf.macaddr.a[5], s->conf.macaddr.a[4],
                 s->conf.macaddr.a[3], s->conf.macaddr.a[2],
                 s->conf.macaddr.a[1], s->conf.macaddr.a[0]);
    }
}

static void fzi_sethi_init(Object *obj)
{
    sethiState21 *s = FZI_SETHI(obj);

    memory_region_init_io(&s->mmio, obj, &eth_ops, s, TYPE_FZI_SETHI,
                          SETHI_NIC_MAX);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static const Property fzi_sethi_properties[] = {
    DEFINE_NIC_PROPERTIES(sethiState21, conf),
    DEFINE_PROP_LINK("phy-mdio", sethiState21, phyMdio, TYPE_FZI_MDIO,
                     MdioState *),
    DEFINE_PROP_UINT16("max-ring-size", sethiState21, max_ring_size, 16)
};

static void fzi_sethi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    device_class_set_legacy_reset(dc, fzi_sethi_reset);
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);

    dc->realize = fzi_sethi_realize;
    device_class_set_props(dc, fzi_sethi_properties);
}

static const TypeInfo fzi_sethi_info = {
    .name = TYPE_FZI_SETHI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(sethiState21),
    .instance_init = fzi_sethi_init,
    .class_init = fzi_sethi_class_init,
};

static void fzi_sethi_register_types(void)
{
    type_register_static(&fzi_sethi_info);
}

type_init(fzi_sethi_register_types);

/*
 * Create sethi NIC device.
 */
sethiState21 *sethi_if_create(MemoryRegion *address_space, hwaddr base,
                              qemu_irq irq)
{
    DeviceState *dev = qdev_new(TYPE_FZI_SETHI);
    sethiState21 *s = (sethiState21 *)dev;
    NICInfo *nd;

    s->irq = irq;

    while ((nd = qemu_find_nic_info(TYPE_FZI_SETHI, true, NULL))) {
        qdev_set_nic_properties(dev, nd);
    }
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, base);
    return s;
}
