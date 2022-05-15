/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2017 "Eric Poulsen" <eric@zyxod.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "py/runtime.h"
#include "py/stream.h"
#include "py/mphal.h"
#include "extmod/machine_spi.h"
#include "modmachine.h"

#include "driver/spi_master.h"

// SPI mappings by device, naming used by IDF old/new
// upython   | ESP32     | ESP32S2   | ESP32S3 | ESP32C3
// ----------+-----------+-----------+---------+---------
// SPI(id=1) | HSPI/SPI2 | FSPI/SPI2 | SPI2    | SPI2
// SPI(id=2) | VSPI/SPI3 | HSPI/SPI3 | SPI3    | err


#define MP_HW_SPI_MAX_XFER_BYTES (4092)
#define MP_HW_SPI_MAX_XFER_BITS (MP_HW_SPI_MAX_XFER_BYTES * 8) // Has to be an even multiple of 8

#if CONFIG_IDF_TARGET_ESP32C3
#define HSPI_HOST SPI2_HOST
#elif CONFIG_IDF_TARGET_ESP32S3
#define HSPI_HOST SPI3_HOST
#define FSPI_HOST SPI2_HOST
#endif

typedef struct _machine_hw_spi_obj_t {
    mp_obj_base_t base;
    spi_host_device_t host;
    uint32_t baudrate;
    uint8_t polarity;
    uint8_t phase;
    uint8_t bits;
    uint8_t firstbit;
    spi_device_handle_t spi;
    enum {
        MACHINE_HW_SPI_STATE_NONE,
        MACHINE_HW_SPI_STATE_INIT,
        MACHINE_HW_SPI_STATE_DEINIT
    } state;
} machine_hw_spi_obj_t;

STATIC void machine_hw_spi_deinit_internal(machine_hw_spi_obj_t *self) {
    switch (spi_bus_remove_device(self->spi)) {
        case ESP_ERR_INVALID_ARG:
            mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("invalid configuration"));
            return;

        case ESP_ERR_INVALID_STATE:
            mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("SPI device already freed"));
            return;
    }
}

STATIC void machine_hw_spi_init_internal(
    machine_hw_spi_obj_t *self,
    int8_t host,
    int32_t baudrate,
    int8_t polarity,
    int8_t phase,
    int8_t bits,
    int8_t firstbit) {

    // if we're not initialized, then we're
    // implicitly 'changed', since this is the init routine
    bool changed = self->state != MACHINE_HW_SPI_STATE_INIT;

    esp_err_t ret;

    machine_hw_spi_obj_t old_self = *self;

    if (host != -1 && host != self->host) {
        self->host = host;
        changed = true;
    }

    if (baudrate != -1) {
        // calculate the actual clock frequency that the SPI peripheral can produce
        baudrate = spi_get_actual_clock(APB_CLK_FREQ, baudrate, 0);
        if (baudrate != self->baudrate) {
            self->baudrate = baudrate;
            changed = true;
        }
    }

    if (polarity != -1 && polarity != self->polarity) {
        self->polarity = polarity;
        changed = true;
    }

    if (phase != -1 && phase != self->phase) {
        self->phase = phase;
        changed = true;
    }

    if (bits != -1 && bits != self->bits) {
        self->bits = bits;
        changed = true;
    }

    if (firstbit != -1 && firstbit != self->firstbit) {
        self->firstbit = firstbit;
        changed = true;
    }

    if (self->host != HSPI_HOST
        #ifdef FSPI_HOST
        && self->host != FSPI_HOST
        #endif
        #ifdef VSPI_HOST
        && self->host != VSPI_HOST
        #endif
        ) {
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("SPI(%d) doesn't exist"), self->host);
    }

    if (changed) {
        if (self->state == MACHINE_HW_SPI_STATE_INIT) {
            self->state = MACHINE_HW_SPI_STATE_DEINIT;
            machine_hw_spi_deinit_internal(&old_self);
        }
    } else {
        return; // no changes
    }

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = self->baudrate,
        .mode = self->phase | (self->polarity << 1),
        .spics_io_num = -1, // No CS pin
        .queue_size = 2,
        .flags = self->firstbit == MICROPY_PY_MACHINE_SPI_LSB ? SPI_DEVICE_TXBIT_LSBFIRST | SPI_DEVICE_RXBIT_LSBFIRST : 0,
        .pre_cb = NULL
    };

    ret = spi_bus_add_device(self->host, &devcfg, &self->spi);
    switch (ret) {
        case ESP_ERR_INVALID_ARG:
            mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("invalid configuration"));
            spi_bus_free(self->host);
            return;

        case ESP_ERR_NO_MEM:
            mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("out of memory"));
            spi_bus_free(self->host);
            return;

        case ESP_ERR_NOT_FOUND:
            mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("no free slots"));
            spi_bus_free(self->host);
            return;
    }
    self->state = MACHINE_HW_SPI_STATE_INIT;
}

STATIC void machine_hw_spi_deinit(mp_obj_base_t *self_in) {
    machine_hw_spi_obj_t *self = (machine_hw_spi_obj_t *)self_in;
    if (self->state == MACHINE_HW_SPI_STATE_INIT) {
        self->state = MACHINE_HW_SPI_STATE_DEINIT;
        machine_hw_spi_deinit_internal(self);
    }
}

STATIC mp_uint_t gcd(mp_uint_t x, mp_uint_t y) {
    while (x != y) {
        if (x > y) {
            x -= y;
        } else {
            y -= x;
        }
    }
    return x;
}

STATIC void machine_hw_spi_transfer(mp_obj_base_t *self_in, size_t len, const uint8_t *src, uint8_t *dest) {
    machine_hw_spi_obj_t *self = MP_OBJ_TO_PTR(self_in);

    if (self->state == MACHINE_HW_SPI_STATE_DEINIT) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("transfer on deinitialized SPI"));
        return;
    }

    // Round to nearest whole set of bits
    int bits_to_send = len * 8 / self->bits * self->bits;

    if (!bits_to_send) {
        mp_raise_ValueError(MP_ERROR_TEXT("buffer too short"));
    }

    if (len <= 4) {
        spi_transaction_t transaction = { 0 };

        if (src != NULL) {
            memcpy(&transaction.tx_data, src, len);
        }

        transaction.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
        transaction.length = bits_to_send;
        spi_device_transmit(self->spi, &transaction);

        if (dest != NULL) {
            memcpy(dest, &transaction.rx_data, len);
        }
    } else {
        int offset = 0;
        int bits_remaining = bits_to_send;
        int optimum_word_size = 8 * self->bits / gcd(8, self->bits);
        int max_transaction_bits = MP_HW_SPI_MAX_XFER_BITS / optimum_word_size * optimum_word_size;
        spi_transaction_t *transaction, *result, transactions[2];
        int i = 0;

        spi_device_acquire_bus(self->spi, portMAX_DELAY);

        while (bits_remaining) {
            transaction = transactions + i++ % 2;
            memset(transaction, 0, sizeof(spi_transaction_t));

            transaction->length =
                bits_remaining > max_transaction_bits ? max_transaction_bits : bits_remaining;

            if (src != NULL) {
                transaction->tx_buffer = src + offset;
            }
            if (dest != NULL) {
                transaction->rx_buffer = dest + offset;
            }

            spi_device_queue_trans(self->spi, transaction, portMAX_DELAY);
            bits_remaining -= transaction->length;

            if (offset > 0) {
                // wait for previously queued transaction
                MP_THREAD_GIL_EXIT();
                spi_device_get_trans_result(self->spi, &result, portMAX_DELAY);
                MP_THREAD_GIL_ENTER();
            }

            // doesn't need ceil(); loop ends when bits_remaining is 0
            offset += transaction->length / 8;
        }

        // wait for last transaction
        MP_THREAD_GIL_EXIT();
        spi_device_get_trans_result(self->spi, &result, portMAX_DELAY);
        MP_THREAD_GIL_ENTER();
        spi_device_release_bus(self->spi);
    }
}

/******************************************************************************/
// MicroPython bindings for hw_spi

STATIC void machine_hw_spi_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    machine_hw_spi_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "SPI(id=%u, baudrate=%u, polarity=%u, phase=%u, bits=%u, firstbit=%u",
        self->host, self->baudrate, self->polarity,
        self->phase, self->bits, self->firstbit);
}

STATIC void machine_hw_spi_init(mp_obj_base_t *self_in, size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    machine_hw_spi_obj_t *self = (machine_hw_spi_obj_t *)self_in;

    enum { ARG_id, ARG_baudrate, ARG_polarity, ARG_phase, ARG_bits, ARG_firstbit, ARG_sck, ARG_mosi, ARG_miso };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_id,       MP_ARG_INT, {.u_int = -1} },
        { MP_QSTR_baudrate, MP_ARG_INT, {.u_int = -1} },
        { MP_QSTR_polarity, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = -1} },
        { MP_QSTR_phase,    MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = -1} },
        { MP_QSTR_bits,     MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = -1} },
        { MP_QSTR_firstbit, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = -1} },
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args),
        allowed_args, args);

    machine_hw_spi_init_internal(self, args[ARG_id].u_int, args[ARG_baudrate].u_int,
        args[ARG_polarity].u_int, args[ARG_phase].u_int, args[ARG_bits].u_int,
        args[ARG_firstbit].u_int);
}

mp_obj_t machine_hw_spi_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    MP_MACHINE_SPI_CHECK_FOR_LEGACY_SOFTSPI_CONSTRUCTION(n_args, n_kw, all_args);

    enum { ARG_id, ARG_baudrate, ARG_polarity, ARG_phase, ARG_bits, ARG_firstbit, ARG_sck, ARG_mosi, ARG_miso };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_id,       MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = -1} },
        { MP_QSTR_baudrate, MP_ARG_INT, {.u_int = 500000} },
        { MP_QSTR_polarity, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_phase,    MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_bits,     MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 8} },
        { MP_QSTR_firstbit, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = MICROPY_PY_MACHINE_SPI_MSB} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    machine_hw_spi_obj_t *self = m_new_obj(machine_hw_spi_obj_t);
    
    self->base.type = &machine_hw_spi_type;

    machine_hw_spi_init_internal(
        self,
        args[ARG_id].u_int,
        args[ARG_baudrate].u_int,
        args[ARG_polarity].u_int,
        args[ARG_phase].u_int,
        args[ARG_bits].u_int,
        args[ARG_firstbit].u_int
        );

    return MP_OBJ_FROM_PTR(self);
}

STATIC const mp_machine_spi_p_t machine_hw_spi_p = {
    .init = machine_hw_spi_init,
    .deinit = machine_hw_spi_deinit,
    .transfer = machine_hw_spi_transfer,
};

const mp_obj_type_t machine_hw_spi_type = {
    { &mp_type_type },
    .name = MP_QSTR_SPI,
    .print = machine_hw_spi_print,
    .make_new = machine_hw_spi_make_new,
    .protocol = &machine_hw_spi_p,
    .locals_dict = (mp_obj_dict_t *)&mp_machine_spi_locals_dict,
};