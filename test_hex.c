#include <stdio.h>
#include <stdint.h>
#include <string.h>

static int hex_char_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int hex_to_bin_fast(const char *hex, uint8_t *bin, int max_len) {
    int len = 0;
    while (hex[len] && hex[len] != '\r' && hex[len] != '\n' && hex[len] != ' ') len++;
    if (len % 2 != 0 || (len / 2) > max_len) return -1;
    for (int i = 0; i < len / 2; i++) {
        int hi = hex_char_val(hex[2 * i]);
        int lo = hex_char_val(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        bin[i] = (uint8_t)((hi << 4) | lo);
    }
    return len / 2;
}
