//Note I used LLM to debug this code and help me through some issues.
#include <stdio.h>
#include <stdlib.h>

#define BUFFER_SIZE (100 * 1024)

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: ./tail <k> <filename>\n");
        return 1;
    }

    char *endptr;
    long k = strtol(argv[1], &endptr, 10);
    if (*endptr != '\0' || k < 0) {
        fprintf(stderr, "invalid k\n");
        return 1;
    }

    if (k == 0) {
        return 0;
    }

    FILE *file = fopen(argv[2], "rb");
    if (!file) {
        fprintf(stderr, "invalid file\n");
        return 1;
    }

    long *positions = malloc((k + 1) * sizeof(long));
    if (!positions) {
        fclose(file);
        return 1;
    }

    unsigned char *buffer = malloc(BUFFER_SIZE);
    if (!buffer) {
        free(positions);
        fclose(file);
        return 1;
    }

    long total_newlines = 0;
    long file_pos = 0;
    size_t bytes_read;

    while ((bytes_read = fread(buffer, 1, BUFFER_SIZE, file)) > 0) {
        for (size_t i = 0; i < bytes_read; i++) {
            if (buffer[i] == '\n') {
                positions[total_newlines % (k + 1)] = file_pos + i;
                total_newlines++;
            }
        }
        file_pos += bytes_read;
    }

    if (file_pos > 0) {
        fseek(file, -1, SEEK_END);
        int last_char = fgetc(file);
        if (last_char != '\n') {
            total_newlines++;
        }
    }

    long start_pos = 0;
    if (total_newlines > k) {
        long target = total_newlines - k - 1;
        start_pos = positions[target % (k + 1)] + 1;
    }

    if (fseek(file, start_pos, SEEK_SET) != 0) {
        free(buffer);
        fclose(file);
        return 1;
    }

    while ((bytes_read = fread(buffer, 1, BUFFER_SIZE, file)) > 0) {
        fwrite(buffer, 1, bytes_read, stdout);
    }

    free(positions);
    free(buffer);
    fclose(file);
    return 0;
}
