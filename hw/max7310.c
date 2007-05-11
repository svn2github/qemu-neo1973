/*
 * MAX7310 8-port GPIO expansion chip.
 *
 * Copyright (c) 2006 Openedhand Ltd.
 * Written by Andrzej Zaborowski <balrog@zabor.org>
 *
 * This file is licensed under GNU GPL.
 */

#include "vl.h"

struct max7310_s {
    i2c_slave i2c;
    int i2c_command_byte;
    int len;

    uint8_t level;
    uint8_t direction;
    uint8_t polarity;
    uint8_t status;
    uint8_t command;
    struct {
        gpio_handler_t fn;
        void *opaque;
    } handler[8];
};

void max7310_reset(i2c_slave *i2c)
{
    struct max7310_s *s = (struct max7310_s *) i2c;
    s->level &= s->direction;
    s->direction = 0xff;
    s->polarity = 0xf0;
    s->status = 0x01;
    s->command = 0x00;
}

static int max7310_rx(i2c_slave *i2c)
{
    struct max7310_s *s = (struct max7310_s *) i2c;

    switch (s->command) {
    case 0x00:	/* Input port */
        return s->level ^ s->polarity;
        break;

    case 0x01:	/* Output port */
        return s->level & ~s->direction;
        break;

    case 0x02:	/* Polarity inversion */
        return s->polarity;

    case 0x03:	/* Configuration */
        return s->direction;

    case 0x04:	/* Timeout */
        return s->status;
        break;

    case 0xff:	/* Reserved */
        return 0xff;

    default:
#ifdef VERBOSE
        printf("%s: unknown register %02x\n", __FUNCTION__, s->command);
#endif
        break;
    }
    return 0xff;
}

static int max7310_tx(i2c_slave *i2c, uint8_t data)
{
    struct max7310_s *s = (struct max7310_s *) i2c;
    uint8_t diff;
    int line;

    if (s->len ++ > 1) {
#ifdef VERBOSE
        printf("%s: message too long (%i bytes)\n", __FUNCTION__, s->len);
#endif
        return 1;
    }

    if (s->i2c_command_byte) {
        s->command = data;
        s->i2c_command_byte = 0;
        return 0;
    }

    switch (s->command) {
    case 0x01:	/* Output port */
        for (diff = (data ^ s->level) & ~s->direction; diff;
                        diff &= ~(1 << line)) {
            line = ffs(diff) - 1;
            if (s->handler[line].fn)
                s->handler[line].fn(line, (data >> line) & 1,
                                s->handler[line].opaque);
        }
        s->level = (s->level & s->direction) | (data & ~s->direction);
        break;

    case 0x02:	/* Polarity inversion */
        s->polarity = data;
        break;

    case 0x03:	/* Configuration */
        s->level &= ~(s->direction ^ data);
        s->direction = data;
        break;

    case 0x04:	/* Timeout */
        s->status = data;
        break;

    case 0x00:	/* Input port - ignore writes */
	break;
    default:
#ifdef VERBOSE
        printf("%s: unknown register %02x\n", __FUNCTION__, s->command);
#endif
        return 1;
    }

    return 0;
}

static void max7310_event(i2c_slave *i2c, enum i2c_event event)
{
    struct max7310_s *s = (struct max7310_s *) i2c;
    s->len = 0;

    switch (event) {
    case I2C_START_SEND:
        s->i2c_command_byte = 1;
        break;
    case I2C_FINISH:
        if (s->len == 1)
#ifdef VERBOSE
            printf("%s: message too short (%i bytes)\n", __FUNCTION__, s->len);
#endif
        break;
    default:
        break;
    }
}

/* MAX7310 is SMBus-compatible (can be used with only SMBus protocols),
 * but also accepts sequences that are not SMBus so return an I2C device.  */
struct i2c_slave *max7310_init(i2c_bus *bus)
{
    struct max7310_s *s = (struct max7310_s *)
            i2c_slave_init(bus, 0, sizeof(struct max7310_s));
    s->i2c.event = max7310_event;
    s->i2c.recv = max7310_rx;
    s->i2c.send = max7310_tx;

    max7310_reset(&s->i2c);
    return &s->i2c;
}

void max7310_gpio_set(i2c_slave *i2c, int line, int level)
{
    struct max7310_s *s = (struct max7310_s *) i2c;
    if (line >= sizeof(s->handler) / sizeof(*s->handler) || line  < 0)
        cpu_abort(cpu_single_env, "bad GPIO line");

    if (level)
        s->level |= s->direction & (1 << line);
    else
        s->level &= ~(s->direction & (1 << line));
}

void max7310_gpio_handler_set(i2c_slave *i2c, int line,
                gpio_handler_t handler, void *opaque)
{
    struct max7310_s *s = (struct max7310_s *) i2c;
    if (line >= sizeof(s->handler) / sizeof(*s->handler) || line  < 0)
        cpu_abort(cpu_single_env, "bad GPIO line");

    s->handler[line].fn = handler;
    s->handler[line].opaque = opaque;
}
