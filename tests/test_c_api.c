#include "../janus_lib.h"
#include <stdio.h>

int main(void) {
    if (!janus_version_string() || !*janus_version_string()) return 1;
    if (janus_compress_file(NULL, "unused", NULL, 0) != 0) return 2;
    if (!*janus_last_error()) return 3;
    if (janus_decompress_file(NULL, "unused", NULL) != 0) return 4;
    if (janus_compress_file("unused", "unused", NULL, -1) != 0) return 5;
    puts("C API validation passed");
    return 0;
}