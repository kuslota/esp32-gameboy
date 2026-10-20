#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    ROM_FILE_OK, ROM_FILE_MISSING, ROM_FILE_SIZE, ROM_FILE_IO,
} rom_file_status_t;

/* Read exactly capacity bytes, rejecting shorter/longer files. Never writes. */
rom_file_status_t rom_file_read(const char *path, uint8_t *buffer, size_t capacity);
/* Read a non-empty file of at most capacity bytes; *length receives its size. Never writes. */
rom_file_status_t rom_file_read_upto(const char *path, uint8_t *buffer, size_t capacity, size_t *length);
