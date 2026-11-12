/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * tests/install/consumer/consumer.c - the downstream consumer fixture.
 *
 * A few lines of C that use the installed public header of Passerelle. The
 * install check compiles it twice: once with the flags given by pkg-config,
 * once through CMake and find_package(Passerelle). If the install were
 * broken (missing header, missing library, broken export), neither build
 * would link and the program would not print its line.
 */

#include <stdint.h>
#include <stdio.h>

#include "pgw/buf.h"

int main(void)
{
    const uint8_t raw[2] = {0x34, 0x12};
    pgw_buf reader;
    uint16_t value;

    pgw_buf_init(&reader, raw, sizeof raw);
    value = pgw_get_u16(&reader);
    if (value != 0x1234u) {
        fprintf(stderr, "consumer: unexpected value 0x%04x\n", value);
        return 1;
    }
    printf("consumer: pgw_get_u16 -> 0x%04x\n", value);
    return 0;
}
