#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BUFFER_SIZE (1024 * 1024)

int main(int argc, char *argv[]) {
    if (argc != 3) {
        const char *usage = "Usage: ./head <k> <filename>\n";
        write(STDERR_FILENO, usage, strlen(usage));
        return 1;
    }

    char *endptr;
    long k = strtol(argv[1], &endptr, 10);

    if (*endptr != '\0' || k < 0) {
        write(STDERR_FILENO, "invalid k\n", strlen("invalid k\n"));
        return 1;
    }

    if (k == 0) {
        return 0;
    }

    const char *filename = argv[2];
    int fd = open(filename, O_RDONLY);
    if (fd == -1) {
        write(STDERR_FILENO, "invalid file\n", strlen("invalid file\n"));
        return 1;
    }

    char *buffer = malloc(BUFFER_SIZE);
    if (!buffer) {
        close(fd);
        return 1;
    }

    ssize_t bytes_read;
    long line_count = 0;

    while ((bytes_read = read(fd, buffer, BUFFER_SIZE)) > 0) {
        ssize_t start = 0;

        for (ssize_t i = 0; i < bytes_read; i++) {
            if (buffer[i] == '\n') {
                line_count++;

                ssize_t len = i - start + 1;
                if (write(STDOUT_FILENO, buffer + start, len) == -1) {
                    free(buffer);
                    close(fd);
                    return 0;
                }

                start = i + 1;

                if (line_count == k) {
                    free(buffer);
                    close(fd);
                    return 0;
                }
            }
        }

        if (start < bytes_read) {
            if (write(STDOUT_FILENO, buffer + start, bytes_read - start) == -1) {
                free(buffer);
                close(fd);
                return 0;
            }
        }
    }
    free(buffer);
    close(fd);
    return 0;
}
