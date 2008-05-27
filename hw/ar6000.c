/*
 * ROCm Atheros AR600X Wireless Ethernet SDIO cards.  Firmware 1.3.
 *
 * Copyright (c) 2007 OpenMoko, Inc.
 * Author: Andrzej Zaborowski <andrew@openedhand.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 *
 * BIG TODO: Merge all the generic SDIO emulation back into sd.c and
 * allow hooks for card specific functions to be registered.  This file
 * then would solely provide callbacks for these hooks.
 */

#include "hw.h"
#include "net.h"
#include "qemu-timer.h"
#include "sd.h"
#include "pcmcia.h"

typedef enum {
    sd_r0 = 0,	/* no response */
    sd_r1,	/* normal response command */
    sd_r2_i,	/* CID register */
    sd_r2_s,	/* CSD register */
    sd_r3,	/* OCR register */
    sd_r4,	/* SDIO OCR register */
    sd_r5,	/* SDIO direct I/O */
    sd_r6,	/* Published RCA response */
    sd_r7,	/* Operating voltage */
    sd_r1b = -1,
} sd_rsp_type_t;

struct sdio_s {
    enum sd_state_e state;
    uint32_t ioocr;
    uint16_t rca;
    uint32_t card_status;

    struct {
        uint8_t revision;
        uint8_t io_enable;
        uint8_t intr_enable;
        uint8_t intr;
        uint8_t bus;
        uint8_t e4mi;
        uint8_t power;
        uint8_t speed;
    } cccr;
    struct {
        uint8_t stdfn;
        uint8_t ext_stdfn;
        uint8_t power;
        int cis_offset;
        uint32_t csa_addr;
        void (*csa_wr)(struct sdio_s *sd, uint8_t data);
        uint8_t (*csa_rd)(struct sdio_s *sd);
    } fbr[7];
    const uint8_t *cis;
    int cislen;

    qemu_irq *func_irq;

    int spi;
    int sdio_ok;
    int current_cmd;
    int next_cmd;
    uint16_t blk_len[8];
    struct {
        uint8_t func;
        int dir;
        int blk_len;
        int blk_num;
        int step;
        uint32_t data_start;
        uint32_t data_offset;
        uint8_t data[2048];
    } transfer;		/* TODO Move to per function struct to support sspnd */

    void (*write[8])(struct sdio_s *sd, uint32_t addr, uint8_t *data, int len);
    void (*read[8])(struct sdio_s *sd, uint32_t addr, uint8_t *data, int len);
    void (*reset)(struct sdio_s *sd);

    struct sd_card_s card;
};

#define SDIO_SIZE	0x20000
#define SDIO_ADDR_MASK	(SDIO_SIZE - 1)

static const sd_cmd_type_t sd_cmd_type[64] = {
    sd_bc,   sd_none, sd_bcr,  sd_bcr,  sd_none, sd_none, sd_none, sd_ac,
    sd_bcr,  sd_ac,   sd_ac,   sd_adtc, sd_ac,   sd_ac,   sd_none, sd_ac,
    sd_ac,   sd_adtc, sd_adtc, sd_none, sd_none, sd_none, sd_none, sd_none,
    sd_adtc, sd_adtc, sd_adtc, sd_adtc, sd_ac,   sd_ac,   sd_adtc, sd_none,
    sd_ac,   sd_ac,   sd_none, sd_none, sd_none, sd_none, sd_ac,   sd_none,
    sd_none, sd_none, sd_bc,   sd_none, sd_none, sd_none, sd_none, sd_none,
    sd_none, sd_none, sd_none, sd_none, sd_none, sd_none, sd_none, sd_ac,
    sd_adtc, sd_none, sd_none, sd_none, sd_none, sd_none, sd_none, sd_none,
};

static const int sd_cmd_class[64] = {
    0,  0,  0,  0,  0,  9, 10,  0,  0,  0,  0,  1,  0,  0,  0,  0,
    2,  2,  2,  2,  3,  3,  3,  3,  4,  4,  4,  4,  6,  6,  6,  6,
    5,  5, 10, 10, 10, 10,  5,  9,  9,  9,  7,  7,  7,  7,  7,  7,
    7,  7, 10,  7,  9,  9,  9,  8,  8, 10,  8,  8,  8,  8,  8,  8,
};

static void sd_set_ioocr(struct sdio_s *sd)
{
    /* 2.0 - 3.6 V, No memory present, One function only */
    sd->ioocr = 0x00ffff00;
}

static void sd_set_rca(struct sdio_s *sd)
{
    sd->rca += 0x4567;
}

static void sd_set_cardstatus(struct sdio_s *sd)
{
    sd->card_status = 0x00001e00;
}

static int sd_req_crc_validate(struct sd_request_s *req)
{
    uint8_t buffer[5];
    buffer[0] = 0x40 | req->cmd;
    buffer[1] = (req->arg >> 24) & 0xff;
    buffer[2] = (req->arg >> 16) & 0xff;
    buffer[3] = (req->arg >> 8) & 0xff;
    buffer[4] = (req->arg >> 0) & 0xff;
    return 0;
    return sd_crc7(buffer, 5) != req->crc;	/* TODO */
}

static void sd_response_r1_make(struct sdio_s *sd,
                                uint8_t *response, uint32_t last_status)
{
    uint32_t mask = CARD_STATUS_B ^ ILLEGAL_COMMAND;
    uint32_t status;

    status = (sd->card_status & ~mask) | (last_status & mask);
    sd->card_status &= ~CARD_STATUS_C;

    response[0] = (status >> 24) & 0xff;
    response[1] = (status >> 16) & 0xff;
    response[2] = (status >> 8) & 0xff;
    response[3] = (status >> 0) & 0xff;
}

static void sd_response_r4_make(struct sdio_s *sd, uint8_t *response)
{
    response[0] = (sd->ioocr >> 24) & 0xff;
    response[1] = (sd->ioocr >> 16) & 0xff;
    response[2] = (sd->ioocr >> 8) & 0xff;
    response[3] = (sd->ioocr >> 0) & 0xff;
    if (sd->sdio_ok)
        response[0] |= 1 << 7;
}

static void sd_response_r5_make(struct sdio_s *sd, uint8_t *response)
{
    int byte = 0;
    uint8_t status, state;

    switch (sd->state) {
    case sd_initialization_state:
    case sd_standby_state:
    case sd_inactive_state:
    default:
        state = 0x00;
        break;
    case sd_command_state:
        state = 0x01;
        break;
    case sd_transfer_state:
        state = 0x02;
        break;
    }

    if (sd->spi) {
        status = ((sd->card_status & 0xb7380003) ? (1 << 6) : 0) |
                ((sd->card_status & ADDRESS_ERROR) ? (1 << 4) : 0) |
                ((sd->card_status & COM_CRC_ERROR) ? (1 << 3) : 0) |
                ((sd->card_status & ILLEGAL_COMMAND) ? (1 << 2) : 0) |
                ((state == 0x00) ? (1 << 0) : 0);
    } else {
        status = ((sd->card_status & COM_CRC_ERROR) ? (1 << 7) : 0) |
                ((sd->card_status & ILLEGAL_COMMAND) ? (1 << 6) : 0) |
                (state << 4) |
                ((sd->card_status & 0x37380003) ? (1 << 3) : 0) |
                ((sd->card_status & ADDRESS_ERROR) ? (1 << 1) : 0) |
                ((sd->card_status & OUT_OF_RANGE) ? (1 << 0) : 0);

        response[byte ++] = 0;
        response[byte ++] = 0;
    }
    sd->card_status &= ~0xf7f80003;			/* TODO check */

    response[byte ++] = status;
    response[byte ++] = sd->transfer.data[sd->transfer.data_offset];
}

static void sd_response_r6_make(struct sdio_s *sd, uint8_t *response)
{
    uint16_t arg;
    uint16_t status;

    arg = sd->rca;
    status =
            ((sd->card_status & SD_ERROR)        ? (1 << 13) : 0) |
            ((sd->card_status & ILLEGAL_COMMAND) ? (1 << 14) : 0) |
            ((sd->card_status & COM_CRC_ERROR)   ? (1 << 15) : 0);

    response[0] = (arg >> 8) & 0xff;
    response[1] = arg & 0xff;
    response[2] = (status >> 8) & 0xff;
    response[3] = 0;
}

static void sdio_intr_update(struct sdio_s *sd)
{
    int level;

    if (!(sd->cccr.intr_enable & 1) ||				/* IENM */
                    ((sd->cccr.bus & 3) == 2 &&			/* BusWidth */
                     !sd->cccr.e4mi && sd->state == sd_transfer_state) ||
                    (sd->spi && !((sd->cccr.bus & (1 << 6)) &&	/* SCSI */
                                  (sd->cccr.bus & (1 << 5)))))	/* ECSI */
        level = 0;
    else
        level = !!((sd->cccr.intr << 1) & sd->cccr.intr_enable);

    qemu_set_irq(sd->card.irq, level);
}

static void sdio_reset(struct sdio_s *sd)
{
    int i;

    sd->state = sd_initialization_state;
    sd->rca = 0x0000;
    sd->sdio_ok = 0;
    sd_set_ioocr(sd);
    sd_set_cardstatus(sd);

    memset(&sd->cccr, 0, sizeof(sd->cccr));
    memset(&sd->fbr,  0, sizeof(sd->fbr));
    for (i = 0; i < 8; i ++)
        sd->blk_len[i] = 0;
    for (i = 0; i < 7; i ++) {
        sd->fbr[i].stdfn &= 0x4f;
        sd->fbr[i].power = 0;
    }
    if (sd->reset)
        sd->reset(sd);

    /* TODO: should preserve CDDisable (sd->cccr.bus & (1 << 7)) */
}

static void sdio_transfer_done(struct sdio_s *sd)
{
    sd->state = sd_command_state;

    /* Must check interrupts because of 4-wire mode Interrupt Period.  */
    if ((sd->cccr.bus & 3) == 2)			/* BusWidth */
        sdio_intr_update(sd);
}

static sd_rsp_type_t sdio_normal_command(struct sdio_s *sd,
                struct sd_request_s req)
{
    uint32_t rca = 0x0000;
    uint32_t addr;
    uint8_t fun;

    if (sd_cmd_type[req.cmd] == sd_ac || sd_cmd_type[req.cmd] == sd_adtc)
        rca = req.arg >> 16;

    switch (req.cmd) {
    /* Basic commands (Class 0) */
    case 0:	/* CMD0:   GO_IDLE_STATE */
        /* XXX: Used to switch to SPI mode and back */
        printf("%s: Bus mode switch attempt\n", __FUNCTION__);
        break;

    case 3:	/* CMD3:   SEND_RELATIVE_ADDR */
        if (sd->spi || !sd->sdio_ok)
            goto bad_cmd;
        switch (sd->state) {
        case sd_initialization_state:
        case sd_standby_state:
            sd->state = sd_standby_state;
            sd_set_rca(sd);
            return sd_r6;

        default:
            break;
        }
        break;

    /* I/O mode commands (Class 9) */
    case 5:	/* CMD5:   IO_SEND_OP_COND */
        switch (sd->state) {
        case sd_initialization_state:
            /* We accept any voltage.  10000 V is nothing.  */
            if (req.arg) {
                sd->state = sd_initialization_state;
                sd->sdio_ok = 1;
            }

            return sd_r4;

        default:
            break;
        }
        break;

    /* Basic commands (Class 0) */
    case 7:	/* CMD7:   SELECT/DESELECT_CARD */
        if (sd->spi)
            goto bad_cmd;
        switch (sd->state) {
        case sd_standby_state:
            if (sd->rca != rca)
                return sd_r0;

            sd->state = sd_command_state;
            return sd_r1b;

        case sd_command_state:
            if (sd->rca != rca)
                sd->state = sd_standby_state;

            return sd_r1b;

        default:
            break;
        }
        break;

    case 15:	/* CMD15:  GO_INACTIVE_STATE */
        if (sd->spi)
            goto bad_cmd;
        switch (sd->state) {
        case sd_initialization_state:
        case sd_standby_state:
        case sd_command_state:
            if (sd->rca && sd->rca != rca)
                return sd_r0;

            sd->state = sd_inactive_state;
            return sd_r0;

        default:
            break;
        }
        break;

    /* I/O mode commands (Class 9) */
    case 52:	/* CMD52:  IO_RW_DIRECT */
        switch (sd->state) {
        case sd_transfer_state:
            /* A transfer is active on DAT lines, don't break it.  */
            sd->next_cmd = sd->current_cmd;

            /* Fall through.  */
        case sd_command_state:
            fun = (req.arg >> 28) & 7;
            addr = (req.arg >> 9) & SDIO_ADDR_MASK;
            sd->transfer.data_offset = 0;
            sd->transfer.step = 1;

            if (unlikely(fun > ((sd->ioocr >> 28) & 7))) {
                sd->card_status |= ADDRESS_ERROR;
                sd->transfer.data[sd->transfer.data_offset] = req.arg & 0xff;
                return sd_r5;
            }

            if ((req.arg >> 31) & 1) {				/* R/W */
                sd->transfer.data[sd->transfer.data_offset] = req.arg & 0xff;
                sd->write[fun](sd, addr, sd->transfer.data +
                                sd->transfer.data_offset, 1);
            }

            if (((~req.arg >> 31) & 1) ||			/* !R/W */
                            ((req.arg >> 27) & 1))		/* RAW */
                sd->read[fun](sd, addr, sd->transfer.data +
                                sd->transfer.data_offset, 1);

            return sd_r5;

        default:
            break;
        }
        break;

    case 53:	/* CMD53:  IO_RW_EXTENDED */
        switch (sd->state) {
        case sd_command_state:
            fun = (req.arg >> 28) & 7;
            addr = (req.arg >> 9) & SDIO_ADDR_MASK;

            if (unlikely(fun > ((sd->ioocr >> 28) & 7))) {
                sd->card_status |= ADDRESS_ERROR;
                return sd_r5;
            }

            sd->transfer.dir = (req.arg >> 31) & 1;		/* R/W */
            sd->transfer.step = (req.arg >> 26) & 1;		/* OPCode */
            sd->transfer.func = fun;
            sd->transfer.data_start = addr;
            sd->transfer.data_offset = 0;
            if ((req.arg >> 27) & 1) {				/* BlockMode */
                if (sd->blk_len[fun] < 1 || sd->blk_len[fun] > 2048)
                    return sd_r1;

                sd->transfer.blk_len = sd->blk_len[fun];
            } else
                sd->transfer.blk_len = 1;
            sd->transfer.blk_num = ((req.arg >> 0) & 0x1ff) ?:
                    ((req.arg >> 27) & 1) ? -1 : 0x200;		/* BlockMode */

            /* XXX The R5 on real cards indicates command state for some
             * reason.  Is that because the transfer hasn't started yet or
             * because it has already finished when the response is made?  */
            sd->state = sd_transfer_state;
            sd->transfer.data[0] = 0x00;
            return sd_r5;

        default:
            break;
        }
        break;

    /* Basic commands (Class 0) */
    case 59:	/* CMD59:  CRC_ON_OFF */
        if (!sd->spi)
            goto bad_cmd;
        /* TODO */
        return sd_r1;

    default:
    bad_cmd:
        sd->card_status |= ILLEGAL_COMMAND;

        printf("%s: Unknown CMD%i\n", __FUNCTION__, req.cmd);
        return sd->spi ? sd_r1 : sd_r0;
    }

    sd->card_status |= ILLEGAL_COMMAND;
    printf("%s: CMD%i in a wrong state\n", __FUNCTION__, req.cmd);
    return sd->spi ? sd_r1 : sd_r0;
}

static int sdio_do_command(struct sdio_s *sd, struct sd_request_s *req,
                uint8_t *response)
{
    uint32_t last_status = sd->card_status;
    sd_rsp_type_t rtype;
    int rsplen;

    if (sd_req_crc_validate(req)) {
        sd->card_status &= ~COM_CRC_ERROR;
        return 0;
    }

    sd->card_status &= ~(COM_CRC_ERROR | ILLEGAL_COMMAND);	/* B type */

    sd->next_cmd = req->cmd;
    rtype = sdio_normal_command(sd, *req);

    sd->current_cmd = sd->next_cmd;

    switch (rtype) {
    case sd_r1:
    case sd_r1b:
        sd_response_r1_make(sd, response, last_status);
        rsplen = 4;
        break;

    case sd_r4:
        sd_response_r4_make(sd, response);
        rsplen = 4;
        break;

    case sd_r5:
        sd_response_r5_make(sd, response);
        rsplen = sd->spi ? 2 : 4;
        break;

    case sd_r6:
        sd_response_r6_make(sd, response);
        rsplen = 4;
        break;

    case sd_r0:
    default:
        rsplen = 0;
        break;
    }

    if (sd->card_status & ILLEGAL_COMMAND)
        rsplen = 0;

    return rsplen;
}

static void sdio_write_data(struct sdio_s *sd, uint8_t value)
{
    if (sd->state != sd_transfer_state) {
        printf("%s: not in Transfer state\n", __FUNCTION__);
        return;
    }

    if (sd->card_status & (ADDRESS_ERROR | OUT_OF_RANGE | SD_ERROR))
        return;

    switch (sd->current_cmd) {
    case 53:	/* CMD53:  IO_RW_EXTENDED */
        if (!sd->transfer.dir)
            goto bad_cmd;

        sd->transfer.data[sd->transfer.data_offset ++] = value;
        if (sd->transfer.data_offset >= sd->transfer.blk_len) {
            /* TODO: Check CRC before committing */
            sd->write[sd->transfer.func](sd, sd->transfer.data_start,
                            sd->transfer.data, sd->transfer.blk_len);

            if (sd->transfer.blk_num >= 0)
                if (!-- sd->transfer.blk_num) {
                    sdio_transfer_done(sd);
                    break;
                }

            sd->transfer.data_start +=
                    sd->transfer.blk_len * sd->transfer.step;
            sd->transfer.data_offset = 0;
        }
        break;

    default:
    bad_cmd:
        printf("%s: unknown command\n", __FUNCTION__);
        break;
    }
}

static uint8_t sdio_read_data(struct sdio_s *sd)
{
    /* TODO: Append CRCs */
    uint8_t ret;

    if (sd->state != sd_transfer_state) {
        printf("%s: not in Transfer state\n", __FUNCTION__);
        return 0x00;
    }

    if (sd->card_status & (ADDRESS_ERROR | OUT_OF_RANGE | SD_ERROR))
        return 0x00;

    switch (sd->current_cmd) {
    case 53:	/* CMD53:  IO_RW_EXTENDED */
        if (sd->transfer.dir)
            goto bad_cmd;

        if (!sd->transfer.data_offset)
            sd->read[sd->transfer.func](sd, sd->transfer.data_start,
                            sd->transfer.data, sd->transfer.blk_len);

        ret = sd->transfer.data[sd->transfer.data_offset ++];

        if (sd->transfer.data_offset >= sd->transfer.blk_len) {
            if (sd->transfer.blk_num >= 0)
                if (!-- sd->transfer.blk_num) {
                    sdio_transfer_done(sd);
                    break;
                }

            sd->transfer.data_start +=
                    sd->transfer.blk_len * sd->transfer.step;
            sd->transfer.data_offset = 0;
        }
        break;

    default:
    bad_cmd:
        printf("%s: unknown command\n", __FUNCTION__);
        return 0x00;
    }

    return ret;
}

static int sdio_data_ready(struct sdio_s *sd)
{
    return sd->state == sd_transfer_state;
}

#define SDIO_CIS_START	0x1000

static void sdio_cccr_write(struct sdio_s *sd, uint32_t offset, uint8_t value)
{
    switch (offset) {
    case 0x02:	/* I/O Enable */
        sd->cccr.io_enable =
                value & (((1 << ((sd->ioocr >> 28) & 7)) - 1) << 1);
        /* TODO: reset the disabled functions */
        break;

    case 0x04:	/* Int Enable */
        sd->cccr.intr_enable = value;
        sdio_intr_update(sd);
        break;

    case 0x06:	/* I/O Abort */
        if ((value >> 3) & 1)				/* RES */
            sdio_reset(sd);
        else if ((value >> 0) & 7) {			/* ASx */
            if (sd->state == sd_transfer_state &&
                            sd->transfer.func == (value & 7) && !sd->spi)
                sdio_transfer_done(sd);
            else
                fprintf(stderr, "%s: no transfer to abort for function %i\n",
                                __FUNCTION__, value & 7);
        }

        break;

    case 0x07:	/* Bus Interface Control */
        sd->cccr.bus = (value & 0xe3) | 0x40;
        if (value & 1)
            fprintf(stderr, "%s: wrong bus-width selected\n", __FUNCTION__);
        if (sd->spi)
            sdio_intr_update(sd);
        /* XXX Possibly toggle some SD_DETECT gpio on CDDisable change */
        break;

    case 0x08:	/* Card Capability */
        sd->cccr.e4mi = value & 0x20;
        break;

    /* Since SBS == 0, Function Select and Bus Suspend are R/O.  */

    case 0x0f:	/* Ready Flags */
        break;

    case 0x10:	/* Fn0 Block Size LSB */
        sd->blk_len[0] &= 0xff << 8;
        sd->blk_len[0] |= value;
        break;
    case 0x11:	/* Fn0 Block Size MSB */
        sd->blk_len[0] &= 0xff;
        sd->blk_len[0] |= value << 8;
        break;

    case 0x12:	/* Power Control */
        sd->cccr.power |= value & 0x02;				/* EMPC */
        break;

    case 0x13:	/* High-Speed */
        sd->cccr.speed |= value & 0x02;				/* EHS */
        break;

    default:
        printf("%s: unknown register %02x\n", __FUNCTION__, offset);
    }
}

static uint8_t sdio_cccr_read(struct sdio_s *sd, uint32_t offset)
{
    switch (offset) {
    case 0x00:	/* CCCR/SDIO Revison */
        return sd->cccr.revision;

    case 0x01:	/* SD Specification Revision */
        return 0x02;	/* SD Physical Specification Version 2.00 (May 2006) */

    case 0x02:	/* I/O Enable */
        return sd->cccr.io_enable;

    case 0x03:	/* I/O Ready */
        return sd->cccr.io_enable;

    case 0x04:	/* Int Enable */
        return sd->cccr.intr_enable;

    case 0x05:	/* Int Pending */
        return sd->cccr.intr << 1;

    case 0x07:	/* Bus Interface Control */
        return sd->cccr.bus;

    case 0x08:	/* Card Capability */
        /* XXX: need to add ReadWait support too (RWC (04)) */

        /* SDC | SMB | S4MI | E4MI | Full-Speed */
        return 0x13 | sd->cccr.e4mi;

    case 0x09:	/* Common CIS Pointer */
        return (SDIO_CIS_START >>  0) & 0xff;
    case 0x0a:	/* Common CIS Pointer */
        return (SDIO_CIS_START >>  8) & 0xff;
    case 0x0b:	/* Common CIS Pointer */
        return (SDIO_CIS_START >> 16) & 0xff;

    case 0x1c:	/* Bus Suspend */
        return 0x00;

    case 0x1d:	/* Function Select */
        return sd->transfer.func;

    case 0x0e:	/* Exec Flags */
        return 0x00;

    case 0x0f:	/* Ready Flags */
        return 0x00;

    case 0x10:	/* Fn0 Block Size LSB */
        return (sd->blk_len[0] >> 0) & 0xff;
    case 0x11:	/* Fn0 Block Size MSB */
        return (sd->blk_len[0] >> 8) & 0xff;

    case 0x12:	/* Power Control */
        return sd->cccr.power | 0x01;

    case 0x13:	/* High-Speed */
        return sd->cccr.speed | 0x01;

    default:
        printf("%s: unknown register %02x\n", __FUNCTION__, offset);
        return 0;
    }
}

static void sdio_fbr_write(struct sdio_s *sd,
                int fn, uint32_t offset, uint8_t value)
{
    typeof(*sd->fbr) *func = &sd->fbr[fn - 1];

    switch (offset) {
    case 0x00:	/* Standard SDIO Function interface code */
        if ((func->stdfn & (1 << 6)) && (value & (1 << 7)))	/* CSASupport */
            func->stdfn |= 1 << 7;				/* CSAEnable */
        else
            func->stdfn &= ~(1 << 7);				/* CSAEnable */
        break;

    case 0x02:	/* Power Selection */
        sd->cccr.power |= value & 0x02;				/* EPS */
        break;

    case 0x0c:	/* Function CSA Pointer */
        func->csa_addr &= 0xffff00;
        func->csa_addr |= value <<  0;
        break;
    case 0x0d:	/* Function CSA Pointer */
        func->csa_addr &= 0xff00ff;
        func->csa_addr |= value <<  8;
        break;
    case 0x0e:	/* Function CSA Pointer */
        func->csa_addr &= 0x00ffff;
        func->csa_addr |= value << 16;
        break;

    case 0x0f:	/* Data access window to function's CSA */
        if (func->stdfn & (1 << 7)) {				/* CSAEnable */
            if (func->csa_wr)
                func->csa_wr(sd, value);
            func->csa_addr ++;
            break;
        }
        goto bad_reg;

    case 0x10:	/* I/O Block Size LSB */
        sd->blk_len[fn] &= 0xff << 8;
        sd->blk_len[fn] |= value;
        break;
    case 0x11:	/* I/O Block Size MSB */
        sd->blk_len[fn] &= 0xff;
        sd->blk_len[fn] |= value << 8;
        break;

    default:
    bad_reg:
        printf("%s: unknown register %02x\n", __FUNCTION__, offset);
    }
}

static uint8_t sdio_fbr_read(struct sdio_s *sd,
                int fn, uint32_t offset)
{
    typeof(*sd->fbr) *func = &sd->fbr[fn - 1];

    switch (offset) {
    case 0x00:	/* Standard SDIO Function interface code */
        return func->stdfn;

    case 0x01:	/* Extended standard SDIO Function interface code */
        return func->ext_stdfn;

    case 0x02:	/* Power Selection */
        return func->power | 0x01;

    case 0x09:	/* Function CIS Pointer */
        return ((SDIO_CIS_START + func->cis_offset) >>  0) & 0xff;
    case 0x0a:	/* Function CIS Pointer */
        return ((SDIO_CIS_START + func->cis_offset) >>  8) & 0xff;
    case 0x0b:	/* Function CIS Pointer */
        return ((SDIO_CIS_START + func->cis_offset) >> 16) & 0xff;

    case 0x0c:	/* Function CSA Pointer */
        return (func->csa_addr >>  0) & 0xff;
    case 0x0d:	/* Function CSA Pointer */
        return (func->csa_addr >>  8) & 0xff;
    case 0x0e:	/* Function CSA Pointer */
        return (func->csa_addr >> 16) & 0xff;

    case 0x0f:	/* Data access window to function's CSA */
        if ((func->stdfn & (1 << 7)) && func->csa_rd)		/* CSAEnable */
            return func->csa_rd(sd);
        if (func->stdfn & (1 << 7))
            func->csa_addr ++;
        return 0x00;

    case 0x10:	/* I/O Block Size LSB */
        return (sd->blk_len[fn] >> 0) & 0xff;
    case 0x11:	/* I/O Block Size MSB */
        return (sd->blk_len[fn] >> 8) & 0xff;

    default:
        printf("%s: unknown register %02x\n", __FUNCTION__, offset);
        return 0;
    }
}

static void sdio_cia_write(struct sdio_s *sd,
                uint32_t addr, uint8_t *data, int len)
{
    /* CCCR */
    for (; len && addr < 0x100; len --, addr += sd->transfer.step)
        sdio_cccr_write(sd, addr, *data ++);

    /* FBR */
    for (; len && addr < 0x800; len --, addr += sd->transfer.step)
        sdio_fbr_write(sd, addr >> 8, addr & 0xff, *data ++);

    if (len)
        fprintf(stderr, "%s: bad write at %x (%i bytes)\n",
                        __FUNCTION__, addr, len);
}

static void sdio_cia_read(struct sdio_s *sd,
                uint32_t addr, uint8_t *data, int len)
{
    int llen;

    /* CCCR */
    for (; len && addr < 0x100; len --, addr += sd->transfer.step)
        *data ++ = sdio_cccr_read(sd, addr);

    /* FBR */
    for (; len && addr < 0x800; len --, addr += sd->transfer.step)
        *data ++ = sdio_fbr_read(sd, addr >> 8, addr & 0xff);

    /* RFU */
    if (len && unlikely(addr < SDIO_CIS_START)) {
        llen = sd->transfer.step ? MIN(len, SDIO_CIS_START - addr) : len;
        memset(data, 0, llen);
        data += llen;
        len -= llen;
        addr = SDIO_CIS_START;
    }

    /* CIS */
    addr -= SDIO_CIS_START;
    if (len && addr < sd->cislen) {
        llen = MIN(len, sd->cislen - addr);
        memcpy(data, sd->cis + addr, llen);
        data += llen;
        len -= llen;
    }

    /* RFU */
    if (len)
        memset(data, 0, len);
}

static void sdio_dummy_write(struct sdio_s *sd,
                uint32_t addr, uint8_t *data, int len)
{
    fprintf(stderr, "%s: writing %i bytes at %x\n", __FUNCTION__, len, addr);
}

static void sdio_dummy_read(struct sdio_s *sd,
                uint32_t addr, uint8_t *data, int len)
{
    fprintf(stderr, "%s: reading %i bytes at %x\n", __FUNCTION__, len, addr);
    memset(data, 0, sd->transfer.step ? len : 1);
}

static void sdio_set_irq(void *opaque, int line, int level)
{
    struct sdio_s *s = opaque;

    if (level)
        s->cccr.intr |= 1 << line;
    else
        s->cccr.intr &= ~(1 << line);

    sdio_intr_update(s);
}

struct sd_card_s *sdio_init(struct sdio_s *s)
{
    int i;

    for (i = 0; i < 8; i ++) {
        s->write[i] = i ? sdio_dummy_write : sdio_cia_write;
        s->read[i]  = i ? sdio_dummy_read  : sdio_cia_read;
    }

    s->func_irq = qemu_allocate_irqs(sdio_set_irq, s, 7);

    /* Default: SDIO Specification Version 2.00, CCCR/FBR V 1.20 */
    s->cccr.revision = 0x32;

    s->card.opaque = s;
    s->card.do_command = (void *) sdio_do_command;
    s->card.write_data = (void *) sdio_write_data;
    s->card.read_data  = (void *) sdio_read_data;
    s->card.data_ready = (void *) sdio_data_ready;

    sdio_reset(s);

    return &s->card;
}

/* WMI (Wireless Module Interface) */

struct wmi_s {
    NICInfo *nd;

    void (*send)(void *opaque, const uint8_t *buffer, int len);
    void *opaque;

    QEMUTimer *alive;
};

#define WMI_MSG	__attribute__((packed))

struct wmi_msg_s {
    uint16_t id;
    uint8_t data[128];
} WMI_MSG;

static void wmi_make_event(struct wmi_s *s, uint16_t id,
		uint8_t *data, int len)
{
    struct wmi_msg_s msg;

    msg.id = id;
    memcpy(msg.data, data, len);

    s->send(s->opaque, (void *) &msg, sizeof(msg.id) + len);
}

enum {
    WMI_READY_EVENTID		= 0x1001,
    WMI_CONNECT_EVENTID,
    WMI_DISCONNECT_EVENTID,
    WMI_BSSINFO_EVENTID,
    WMI_CMDERROR_EVENTID,
    WMI_REGDOMAIN_EVENTID,
    WMI_PSTREAM_TIMEOUT_EVENTID,
    WMI_NEIGHBOR_REPORT_EVENTID,
    WMI_TKIP_MICERR_EVENTID,
    WMI_SCAN_COMPLETE_EVENTID,
    WMI_REPORT_STATISTICS_EVENTID,
    WMI_RSSI_THRESHOLD_EVENTID,
    WMI_ERROR_REPORT_EVENTID,
    WMI_OPT_RX_FRAME_EVENTID,
    WMI_REPORT_ROAM_TBL_EVENTID,
    WMI_EXTENSION_EVENTID,
    WMI_CAC_EVENTID,
    WMI_SNR_THRESHOLD_EVENTID,
    WMI_LQ_THRESHOLD_EVENTID,
    WMI_TX_RETRY_ERR_EVENTID,
    WMI_REPORT_ROAM_DATA_EVENTID,
};

enum {
    WMI_11A_CAPABILITY	= 1,
    WMI_11G_CAPABILITY	= 2,
    WMI_11AG_CAPABILITY	= 3,
};

struct wmi_ready_event_s {
    uint8_t macaddr[6];
    uint8_t phy_capability;
} WMI_MSG;

static void wmi_ready_event(struct wmi_s *s)
{
    struct wmi_ready_event_s ev;

    memcpy(ev.macaddr, s->nd->macaddr, 6);
    ev.phy_capability = WMI_11AG_CAPABILITY;

    /* TODO: request buf, fill in, submit */
    wmi_make_event(s, WMI_READY_EVENTID, (void *) &ev, sizeof(ev));
}

/* The interface is alive */
static void wmi_alive_tick(void *opaque)
{
    struct wmi_s *s = (void *) opaque;

    /* TODO: reschedule if we're in the middle of a transfer or other
     * activity.  Do the same for other timers we may need in WMI.  */
    qemu_free_timer(s->alive);

    /* Send the initial event */
    wmi_ready_event(s);
}

static __attribute__((malloc)) struct wmi_s *wmi_init(NICInfo *nd,
                void *opaque,
                void (*send)(void *opaque, const uint8_t *buffer, int len))
{
    struct wmi_s *s = qemu_mallocz(sizeof(*s));

    s->nd = nd;
    s->opaque = opaque;
    s->send = send;

    s->alive = qemu_new_timer(vm_clock, wmi_alive_tick, s);
    qemu_mod_timer(s->alive, qemu_get_clock(vm_clock) + (ticks_per_sec << 1));

    return s;
}

static void wmi_done(struct wmi_s *wmi)
{
    free(wmi);
}

/* Atheros AR600x */

struct ar6k_s {
    struct sdio_s sd;
    NICInfo *nd;

    struct {
        uint8_t host_int_stat;
        uint8_t cpu_int_stat;
        uint8_t error_int_stat;
        uint8_t counter_int_stat;
        uint8_t mbox_frame;
        uint8_t rx_la_valid;
        uint32_t rx_la[4];
        uint8_t int_stat_ena;
        uint8_t cpu_int_stat_ena;
        uint8_t err_int_stat_ena;
        uint8_t cnt_int_stat_ena;
        uint8_t cnt[4];
        uint32_t cnt_tx[4];
        uint8_t scratch[8];
        uint8_t wlan_int;

        uint8_t mbox[0x800 * 4];
        int mbox_count[4];
    } hif;
    struct {
        int done;
    } bmi;

    struct wmi_s *wmi;

    QEMUTimer *cnt_irq_update;

    uint8_t cis[0];
};

static inline void ar6k_hif_intr_update(struct ar6k_s *s)
{
    qemu_set_irq(s->sd.func_irq[0],
                    !!(s->hif.host_int_stat & s->hif.int_stat_ena));
}

static void ar6k_hif_error_intr_update(struct ar6k_s *s)
{
    uint8_t orig = s->hif.host_int_stat;

    if (s->hif.error_int_stat & s->hif.err_int_stat_ena)
        s->hif.host_int_stat |= 1 << 7;			/* STATUS_ERROR */
    else
        s->hif.host_int_stat &= ~(1 << 7);		/* STATUS_ERROR */

    if (orig != s->hif.host_int_stat)
        ar6k_hif_intr_update(s);
}

static void ar6k_hif_cpu_intr_update(struct ar6k_s *s)
{
    uint8_t orig = s->hif.host_int_stat;

    if (s->hif.cpu_int_stat & s->hif.cpu_int_stat_ena)
        s->hif.host_int_stat |= 1 << 6;			/* STATUS_CPU */
    else
        s->hif.host_int_stat &= ~(1 << 6);		/* STATUS_CPU */

    if (orig != s->hif.host_int_stat)
        ar6k_hif_intr_update(s);
}

static void ar6k_hif_counter_intr_update(struct ar6k_s *s)
{
    uint8_t orig = s->hif.host_int_stat;

    if (s->hif.counter_int_stat & s->hif.cnt_int_stat_ena)
        s->hif.host_int_stat |= 1 << 4;			/* STATUS_COUNTER */
    else
        s->hif.host_int_stat &= ~(1 << 4);		/* STATUS_COUNTER */

    if (orig != s->hif.host_int_stat)
        ar6k_hif_intr_update(s);
}

static void ar6k_hif_counter_intr_sched(struct ar6k_s *s)
{
    qemu_mod_timer(s->cnt_irq_update, qemu_get_clock(vm_clock) +
                    (ticks_per_sec >> 6));
}

static void ar6k_hif_cnt_irq_tick(void *opaque)
{
    struct ar6k_s *s = (void *) opaque;

    ar6k_hif_counter_intr_update(s);
}

/* Atheros BMI (Bootloader Messaging Interface) */

static void ar6k_bmi_reset(struct ar6k_s *s)
{
    int i;

    if (s->wmi) {
        wmi_done(s->wmi);
        s->wmi = 0;
    }

    for (i = 0; i < 4; i ++) {
        s->hif.cnt[i] = 0x00;
        s->hif.cnt_tx[i] = 0xff;
        s->hif.mbox_count[i] = 0;
    }

    s->bmi.done = 0;

    s->hif.host_int_stat = 0x00;
    s->hif.cpu_int_stat = 0x00;
    s->hif.error_int_stat = 0x00;
    s->hif.counter_int_stat = 0xf0;
    s->hif.int_stat_ena = 0x00;
    s->hif.cpu_int_stat_ena = 0x00;
    s->hif.err_int_stat_ena = 0x00;
    s->hif.cnt_int_stat_ena = 0x00;

    ar6k_hif_cpu_intr_update(s);
    ar6k_hif_error_intr_update(s);
    ar6k_hif_counter_intr_update(s);
}

enum {
    bmi_no_command = 0,
    bmi_done,			/* Host is done using BMI */
    bmi_read_memory,		/* Host reads AR6K memory */
    bmi_write_memory,		/* Host writes AR6K memory */
    bmi_execute,		/* Causes AR6K to execute code */
    bmi_set_app_start,		/* Set Target application starting address */
    bmi_read_soc_register,	/* Read a 32-bit Target SOC register */
    bmi_write_soc_register,	/* Write a 32-bit Target SOC register */
    bmi_get_target_id,		/* Fetch the 4-byte Target information */
    bmi_rompatch_install,	/* Install a ROM Patch */
    bmi_rompatch_uninstall,	/* Uninstall a previously-installed ROM Patch */
    bmi_rompatch_activate,	/* Activate a list of installed ROM Patches */
    bmi_rompatch_deactivate,	/* Deactivate a list of active ROM Patches */
};

#define BMI_TARGET_VERSION_SENTINAL	0xffffffff
#define BMI_TARGET_VERSION_ID		0x11000044
#define BMI_TARGET_TYPE_AR6001		1
#define BMI_TARGET_TYPE_AR6002		2

static void ar6k_bmi_write(struct ar6k_s *s, uint8_t *data, int len)
{
    uint32_t cmd;
    uint32_t *cmdp, *resp;
    int rlen = 0;

    if (len < 4) {
        fprintf(stderr, "%s: short command (%ib)\n", __FUNCTION__, len);
        return;
    }

    cmdp = (void *) data + 0x800 - len;
    cmd = le32_to_cpup(cmdp ++);

    switch (cmd) {
    case bmi_no_command:
        break;
    case bmi_done:
        s->bmi.done = 1;
        break;
    case bmi_get_target_id:
#ifndef NEW_FIRMWARE
        rlen = 4;
        resp = (void *) data + 0x800 - rlen;
        cpu_to_le32wu(resp, BMI_TARGET_VERSION_ID);
#else
        rlen = 16;
        resp = (void *) data + 0x800 - rlen;
        cpu_to_le32wu(resp ++, BMI_TARGET_VERSION_SENTINAL);
        cpu_to_le32wu(resp ++, 0x0000000c);	/* target_info_byte_count */
        cpu_to_le32wu(resp ++, BMI_TARGET_VERSION_ID);	/* target_ver */
        cpu_to_le32wu(resp ++, BMI_TARGET_TYPE_AR6001);	/* target_type */
#endif
        break;
    default:
        fprintf(stderr, "%s: bad command (%i)\n", __FUNCTION__, cmd);
        return;
    }

    s->hif.cnt[0] = rlen;
}

/* Atheros HTC/HIF */

enum {
    WMI_CONTROL_MBOX = 0,
    WMI_BEST_EFFORT_MBOX,
    WMI_LOW_PRIORITY_MBOX,
    WMI_HIGH_PRIORITY_MBOX,
};

static void ar6k_hif_txcredit_reset(struct ar6k_s *s, int mbox)
{
    s->hif.cnt_tx[mbox] = 0;
    if (!(s->hif.counter_int_stat & (1 << mbox))) {
        s->hif.counter_int_stat |= 1 << mbox;
        ar6k_hif_counter_intr_sched(s);
    }
}

static void ar6k_hif_txcredit_grant(struct ar6k_s *s, int mbox)
{
    s->hif.cnt_tx[mbox] = 0xff;
    if (!(s->hif.counter_int_stat & (1 << (mbox + 4)))) {
        s->hif.counter_int_stat |= 1 << (mbox + 4);
        ar6k_hif_counter_intr_sched(s);
    }
}

#define AR6K_HTC_HEADER_LEN	2

static void ar6k_hif_wmi_event(void *opaque, const uint8_t *buffer, int len)
{
    struct ar6k_s *s = (void *) opaque;

    int mbox = WMI_CONTROL_MBOX;
    uint16_t *datap;

    s->hif.rx_la[mbox] = len;
    s->hif.rx_la_valid |= 1 << mbox;

    datap = (void *) s->hif.mbox + ((mbox + 1) << 11) -
            (len + AR6K_HTC_HEADER_LEN);

    /* Prepend the HTC frame header */
    cpu_to_le16wu(datap ++, len);

    memcpy(datap, buffer, len);

    if (!(s->hif.host_int_stat & (1 << mbox))) {	/* STATUS_MBOX_DATA */
        s->hif.host_int_stat |= 1 << mbox;		/* STATUS_MBOX_DATA */

        ar6k_hif_intr_update(s);
    }
}

#define AR6K_HOST_INT_STAT		0x400
#define AR6K_CPU_INT_STAT		0x401
#define AR6K_ERROR_INT_STAT		0x402
#define AR6K_COUNTER_INT_STAT		0x403
#define AR6K_MBOX_FRAME			0x404
#define AR6K_RX_LOOKAHEAD_VALID		0x405
#define AR6K_RX_LOOKAHEAD0		0x408
#define AR6K_RX_LOOKAHEAD1		0x40c
#define AR6K_RX_LOOKAHEAD2		0x410
#define AR6K_RX_LOOKAHEAD3		0x414
#define AR6K_INT_STAT_ENABLE		0x418
#define AR6K_CPU_INT_STAT_ENABLE	0x419
#define AR6K_ERROR_STAT_ENABLE		0x41a
#define AR6K_COUNTER_INT_STAT_ENABLE	0x41b
#define AR6K_COUNT			0x420
#define AR6K_COUNT_RESET		0x440
#define AR6K_COUNT_DEC			0x450
#define AR6K_SCRATCH			0x460
#define AR6K_FIFO_TIMEOUT		0x468
#define AR6K_FIFO_TIMEOUT_ENABLE	0x469
#define AR6K_DISABLE_SLEEP		0x46a
#define AR6K_LOCAL_BUS_ENDIAN		0x46e
#define AR6K_LOCAL_BUS			0x470
#define AR6K_INT_WLAN			0x472
#define AR6K_WINDOW_DATA		0x474
#define AR6K_WRITE_ADDR			0x478
#define AR6K_READ_ADDR			0x47c
#define AR6K_SPI_CONFIG			0x480
#define AR6K_SPI_STATUS			0x481
#define AR6K_CIS_WINDOW			0x600
#define AR6K_HIF_MBOX_BASE		0x800
#define AR6K_HIF_MBOX0_BASE		0x800
#define AR6K_HIF_MBOX1_BASE		0x1000
#define AR6K_HIF_MBOX2_BASE		0x1800
#define AR6K_HIF_MBOX3_BASE		0x2000
#define AR6K_HIF_MBOX_END		0x27ff

static void ar6k_hif_write(struct ar6k_s *s, uint32_t addr, uint8_t value)
{
    int mbox;

    switch (addr) {
    case AR6K_HOST_INT_STAT:
        if (s->hif.host_int_stat & value) {
            s->hif.host_int_stat &= ~value;
            ar6k_hif_intr_update(s);
        }
        break;
    case AR6K_CPU_INT_STAT:
        if (s->hif.cpu_int_stat & value) {
            s->hif.cpu_int_stat &= ~value;
            ar6k_hif_cpu_intr_update(s);
        }
        break;
    case AR6K_ERROR_INT_STAT:
        if (s->hif.error_int_stat & value) {
            s->hif.error_int_stat &= ~value;
            ar6k_hif_error_intr_update(s);
        }
        break;
    case AR6K_COUNTER_INT_STAT:
        if (s->hif.counter_int_stat & value) {
            s->hif.counter_int_stat &= ~value;
            ar6k_hif_counter_intr_update(s);
        }
        break;

    case AR6K_MBOX_FRAME:
        s->hif.mbox_frame = value;
        break;

    case AR6K_INT_STAT_ENABLE:
        if (s->hif.int_stat_ena ^ value) {
            s->hif.int_stat_ena = value;
            ar6k_hif_intr_update(s);
        }
        break;
    case AR6K_CPU_INT_STAT_ENABLE:
        if (s->hif.cpu_int_stat_ena ^ value) {
            s->hif.cpu_int_stat_ena = value;
            ar6k_hif_cpu_intr_update(s);
        }
        break;
    case AR6K_ERROR_STAT_ENABLE:
        if (s->hif.err_int_stat_ena ^ value) {
            s->hif.err_int_stat_ena = value;
            ar6k_hif_error_intr_update(s);
        }
        break;
    case AR6K_COUNTER_INT_STAT_ENABLE:
        if (s->hif.cnt_int_stat_ena ^ value) {
            s->hif.cnt_int_stat_ena = value;
            ar6k_hif_counter_intr_sched(s);
        }
        break;

    case AR6K_SCRATCH ... (AR6K_SCRATCH + 7):
        s->hif.scratch[addr - AR6K_SCRATCH] = value;
        break;

    case AR6K_FIFO_TIMEOUT:
    case AR6K_FIFO_TIMEOUT_ENABLE:
    case AR6K_DISABLE_SLEEP:
    case AR6K_LOCAL_BUS_ENDIAN:
    case AR6K_LOCAL_BUS:
        goto bad_reg;

    case AR6K_INT_WLAN:
        s->hif.wlan_int = value;
        if (value && !s->wmi) {
            /* Initialisation sequence is complete (?).  */
            s->wmi = wmi_init(s->nd, s, ar6k_hif_wmi_event);
        }
        break;

    case AR6K_WINDOW_DATA:
    case AR6K_WRITE_ADDR:
    case AR6K_READ_ADDR:
    case AR6K_SPI_CONFIG:
        goto bad_reg;

    case AR6K_HIF_MBOX_BASE ... AR6K_HIF_MBOX_END:
        mbox = (addr - AR6K_HIF_MBOX_BASE) >> 11;
        s->hif.mbox[addr - AR6K_HIF_MBOX_BASE] = value;
        s->hif.mbox_count[mbox] ++;
        /* XXX how else do we know when a command is executed?  */
        if ((addr & 0x7ff) == 0x7ff) {
            ar6k_hif_txcredit_reset(s, mbox);
            if (mbox == 0 && !s->bmi.done) {
                ar6k_bmi_write(s, s->hif.mbox + (mbox << 11),
                                s->hif.mbox_count[mbox]);
            } else {
                /* TODO */
            }
            s->hif.mbox_count[mbox] = 0;
            ar6k_hif_txcredit_grant(s, mbox);
        }
        break;
    default:
    bad_reg:
        printf("%s: unknown register %02x\n", __FUNCTION__, addr);
        break;
    }
}

static uint8_t ar6k_hif_read(struct ar6k_s *s, uint32_t addr)
{
    int mbox = 0;

    switch (addr) {
    case AR6K_HOST_INT_STAT:
        return s->hif.host_int_stat;
    case AR6K_CPU_INT_STAT:
        return s->hif.cpu_int_stat;
    case AR6K_ERROR_INT_STAT:
        return s->hif.error_int_stat;
    case AR6K_COUNTER_INT_STAT:
        return s->hif.counter_int_stat;

    case AR6K_MBOX_FRAME:
        return s->hif.mbox_frame;

    case AR6K_RX_LOOKAHEAD_VALID:
        return s->hif.rx_la_valid;
    case AR6K_RX_LOOKAHEAD3 ... (AR6K_RX_LOOKAHEAD3 + 3): mbox ++;
    case AR6K_RX_LOOKAHEAD2 ... (AR6K_RX_LOOKAHEAD2 + 3): mbox ++;
    case AR6K_RX_LOOKAHEAD1 ... (AR6K_RX_LOOKAHEAD1 + 3): mbox ++;
    case AR6K_RX_LOOKAHEAD0 ... (AR6K_RX_LOOKAHEAD0 + 3):
        /* XXX when is the bit reset? */
        s->hif.rx_la_valid &= ~(1 << mbox);
        /* XXX when is the bit reset? */
        s->hif.host_int_stat &= ~(1 << mbox);		/* STATUS_MBOX_DATA */

        ar6k_hif_intr_update(s);
        return (s->hif.rx_la[mbox] >> ((addr & 3) << 3)) & 0xff;

    case AR6K_INT_STAT_ENABLE:
        return s->hif.int_stat_ena;
    case AR6K_CPU_INT_STAT_ENABLE:
        return s->hif.cpu_int_stat_ena;
    case AR6K_ERROR_STAT_ENABLE:
        return s->hif.err_int_stat_ena;
    case AR6K_COUNTER_INT_STAT_ENABLE:
        return s->hif.cnt_int_stat_ena;

    case (AR6K_COUNT + 0x4) ... (AR6K_COUNT + 0x7):
        /* XXX What's at (AR6K_COUNT + 0x0) ... (AR6K_COUNT + 0x3)? */
        /* FIXME clear some interrupts etc */
        return s->hif.cnt[addr - AR6K_COUNT - 4];

    case AR6K_COUNT_RESET + 0xc: mbox ++;
    case AR6K_COUNT_RESET + 0x8: mbox ++;
    case AR6K_COUNT_RESET + 0x4: mbox ++;
    case AR6K_COUNT_RESET + 0x0:
        if (s->hif.counter_int_stat & (1 << mbox)) {
            s->hif.counter_int_stat &= ~(1 << mbox);
            ar6k_hif_counter_intr_update(s);
        }
        return s->hif.cnt_tx[mbox];

    case AR6K_COUNT_DEC + 0xc: mbox ++;
    case AR6K_COUNT_DEC + 0x8: mbox ++;
    case AR6K_COUNT_DEC + 0x4: mbox ++;
    case AR6K_COUNT_DEC + 0x0:
        return s->hif.cnt_tx[mbox] --;

    case AR6K_SCRATCH ... (AR6K_SCRATCH + 7):
        return s->hif.scratch[addr - AR6K_SCRATCH];

    case AR6K_FIFO_TIMEOUT:
    case AR6K_FIFO_TIMEOUT_ENABLE:
    case AR6K_DISABLE_SLEEP:
    case AR6K_LOCAL_BUS_ENDIAN:
    case AR6K_LOCAL_BUS:
        goto bad_reg;

    case AR6K_INT_WLAN:
        return s->hif.wlan_int;

    case AR6K_WINDOW_DATA:
    case AR6K_WRITE_ADDR:
    case AR6K_READ_ADDR:
    case AR6K_SPI_CONFIG:
    case AR6K_SPI_STATUS:
        goto bad_reg;

    case AR6K_CIS_WINDOW ... (AR6K_CIS_WINDOW + 0x1ff):
        if (addr >= AR6K_CIS_WINDOW + s->sd.cislen)
            goto bad_reg;

        return s->sd.cis[addr - AR6K_CIS_WINDOW];

    case AR6K_HIF_MBOX_BASE ... AR6K_HIF_MBOX_END:
        return s->hif.mbox[addr - AR6K_HIF_MBOX_BASE];
    default:
    bad_reg:
        printf("%s: unknown register %02x\n", __FUNCTION__, addr);
        break;
    }

    return 0;
}

static void ar6k_fn1_write(struct sdio_s *sd,
                uint32_t addr, uint8_t *data, int len)
{
    struct ar6k_s *s = (void *) sd;

    for (; len; len --, addr += sd->transfer.step)
        ar6k_hif_write(s, addr, *data ++);
}

static void ar6k_fn1_read(struct sdio_s *sd,
                uint32_t addr, uint8_t *data, int len)
{
    struct ar6k_s *s = (void *) sd;

    for (; len; len --, addr += sd->transfer.step)
        *data ++ = ar6k_hif_read(s, addr);
}

static void ar6k_set_ioocr(struct sdio_s *sd)
{
    /* 2.9 - 3.6 V, No memory present, Two functions only */
    sd->ioocr = 0x10fe0000;
}

static void ar6k_reset(struct ar6k_s *s)
{
    ar6k_set_ioocr(&s->sd);
    ar6k_bmi_reset(s);
}

/* TODO: dump real values from an Atheros AR6001 - need hw access! */
const static uint8_t ar6k_cis[] = {
    CISTPL_DEVICE, 3,		/* Not SDIO standard */
    0x00, 0x00, 0x00,		/* TODO */

    CISTPL_MANFID, 4,
    0x71, 0x02,			/* SDIO Card manufacturer code */
    0x0a, 0x01,			/* Manufacturer information (Part No, Rev) */

    CISTPL_FUNCID, 2,
    0x0c,			/* Card funcion code: SDIO */
    0x00,			/* System initialization mask */

    CISTPL_FUNCE, 4,
    0x00,			/* Type of extended data: Function 0 */
    0x00, 0x08,			/* Max. block size/byte count for Fn0: 2048 */
    0x32,			/* Max. transfer rate per line: 25 Mb/Sec */

    CISTPL_END, 0xff,
};

const static uint8_t ar6k_fn1_cis[] = {
    CISTPL_MANFID, 4,
    0x71, 0x02,			/* SDIO Card manufacturer code */
    0x0a, 0x01,			/* Manufacturer information (Part No, Rev) */

    CISTPL_FUNCID, 2,
    0x0c,			/* TODO Card funcion code: SDIO */
    0x00,			/* TODO System initialization mask */

    CISTPL_FUNCE, 42,
    0x01,			/* Type of extended data: Function 1-7 */
    0x01,			/* Function information bitmask: has Wake-up */
    0x11,			/* Application Specification version level */
    0x00, 0x00, 0x00, 0x00,	/* Product Serial Number: unsupported */
    0x00, 0x00, 0x00, 0x00,	/* CSA space size: no CSA */
    0x00,			/* CSA space properties: no CSA */
    0x00, 0x08,			/* Maximum block size/byte count: 2048 */
    0x00, 0x00, 0xff, 0x00,	/* OCR value: 2.8 - 3.6 V */
    0x00,			/* Minimum required current: above 200mA */
    0x00,			/* Average required current: above 200mA */
    0x00,			/* Maximum required current: above 200mA */
    0x00,			/* Minimum standby current: none */
    0x01,			/* Average standby current: 1mA */
    0x0a,			/* Maximum standby current: 10mA */
    0x00, 0x00,			/* Minimum transfer bandwidth: no minimum */
    0x00, 0x00,			/* Optimum transfer bandwidth: no optimum */
    0x00, 0x00,			/* Ready timeout: no timeout */
    0x00, 0x00,			/* Average required current: above 200mA */
    0x00, 0x00,			/* Maximum required current: above 200mA */
    0x01, 0x01,			/* Average HC-mode current: 256mA */
    0x00, 0x01,			/* Maximum HC-mode current: 256mA */
    0x00, 0x01,			/* Average LC-mode current: 256mA */
    0x00, 0x01,			/* Maximum LC-mode current: 256mA */

    CISTPL_END, 0xff,
};

struct sd_card_s *ar6k_init(NICInfo *nd)
{
    struct ar6k_s *s = (struct ar6k_s *) qemu_mallocz(sizeof(struct ar6k_s) +
			    sizeof(ar6k_cis) + sizeof(ar6k_fn1_cis));
    struct sd_card_s *ret = sdio_init(&s->sd);

    s->nd = nd;
    s->cnt_irq_update = qemu_new_timer(vm_clock, ar6k_hif_cnt_irq_tick, s);
    s->sd.reset = (void *) ar6k_reset;
    s->sd.fbr[0].stdfn = 0 | sdio_fn_none;
    s->sd.fbr[0].ext_stdfn = sdio_ext_fn_none;
    s->sd.cccr.revision = 0x11;	/* Dumb down to 1.10 */

    s->sd.cis = s->cis;
    s->sd.cislen = sizeof(ar6k_cis) + sizeof(ar6k_fn1_cis);
    s->sd.fbr[0].cis_offset = sizeof(ar6k_cis);

    memcpy(s->cis + 0, ar6k_cis, sizeof(ar6k_cis));
    memcpy(s->cis + s->sd.fbr[0].cis_offset,
                    ar6k_fn1_cis, sizeof(ar6k_fn1_cis));

    s->sd.write[1] = ar6k_fn1_write;
    s->sd.read[1]  = ar6k_fn1_read;

    ar6k_reset(s);

    return ret;
}
