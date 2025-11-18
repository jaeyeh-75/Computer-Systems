#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>

#define MAX_LINE 4096
#define MAX_ARGS 128
#define MAX_CMDS 16

int last_exit_status = 0;

static char *trim(char *str) {
    char *end;
    while (*str == ' ' || *str == '\t') {
        str++;
    }
    if (*str == 0) {
        return str;
    }

    end = str + strlen(str) - 1;
    while (end > str && (*end == ' ' || *end == '\t')) {
        end--;
    }

    *(end + 1) = 0;
    return str;
}

static int parse_args(char *cmd, char **args) {
    int nargs = 0;
    char *token = strtok(cmd, " \t");
    while (token != NULL && nargs < MAX_ARGS - 1) {
        args[nargs++] = token;
        token = strtok(NULL, " \t");
    }
    args[nargs] = NULL;
    return nargs;
}

static int split_pipeline(char *input, char **cmds) {
    int ncmds = 0;
    char *token = strtok(input, "|");
    while (token != NULL && ncmds < MAX_CMDS) {
        cmds[ncmds++] = trim(token);
        token = strtok(NULL, "|");
    }
    return ncmds;
}

static void execute_pipeline(char **cmds, int ncmds) {
    int i, j;
    int pipes[MAX_CMDS - 1][2];
    pid_t pids[MAX_CMDS];

    for (i = 0; i < ncmds - 1; i++) {
        if (pipe(pipes[i]) == -1) {
            perror("pipe");
            exit(1);
        }
    }

    for (i = 0; i < ncmds; i++) {
        pid_t pid = fork();
        if (pid == -1) {
            perror("fork");
            exit(1);
        } else if (pid == 0) {
            if (i > 0) {
                dup2(pipes[i - 1][0], STDIN_FILENO);
            }

            if (i < ncmds - 1) {
                dup2(pipes[i][1], STDOUT_FILENO);
            }

            for (j = 0; j < ncmds - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            char *args[MAX_ARGS];
            parse_args(cmds[i], args);
            execvp(args[0], args);
            fprintf(stderr, "command not found: %s\n", args[0]);
            _exit(1);
        } else {
            pids[i] = pid;
        }
    }

    for (i = 0; i < ncmds - 1; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }

    for (i = 0; i < ncmds; i++) {
        int status;
        waitpid(pids[i], &status, 0);
        if (i == ncmds - 1) {
            if (WIFEXITED(status)) {
                last_exit_status = WEXITSTATUS(status);
            } else {
                last_exit_status = 1;
            }
        }
    }
}

int main(void) {
    char buf[MAX_LINE + 2];

    while (1) {
        printf("slugterm> ");
        fflush(stdout);

        if (!fgets(buf, sizeof(buf), stdin)) {
            if (feof(stdin)) {
                break;
            } else {
                perror("Error reading input");
                break;
            }
        }

        size_t len = strlen(buf);
        if (len > 0 && buf[len - 1] == '\n') {
            buf[len - 1] = '\0';
        }

        char *trimmed = trim(buf);

        if (strlen(trimmed) == 0) {
            continue;
        }

        if (strcmp(trimmed, "errcode") == 0) {
            printf("%d\n", last_exit_status);
            fflush(stdout);
            last_exit_status = 0;
            continue;
        }

        len = strlen(trimmed);

        if (trimmed[0] == '|' || trimmed[len - 1] == '|') {
            fprintf(stderr, "command malformed\n");
            last_exit_status = 1;
            continue;
        }

        if (strstr(trimmed, "||") != NULL) {
            fprintf(stderr, "command malformed\n");
            last_exit_status = 1;
            continue;
        }

        char *cmds[MAX_CMDS];
        int ncmds = split_pipeline(trimmed, cmds);

        if (ncmds <= 0) {
            fprintf(stderr, "command malformed\n");
            last_exit_status = 1;
            continue;
        }

        execute_pipeline(cmds, ncmds);
    }

    return 0;
}
