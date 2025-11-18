CC = clang
CFLAGS = -Wall -Wextra -Werror -pedantic
FORMAT = clang-format -i -style=file
SRC = $(wildcard *.c *.h)

all: format head tail

format:
	@$(FORMAT) $(SRC)

head: head.c
	$(CC) $(CFLAGS) -o head head.c

tail: tail.c
	$(CC) $(CFLAGS) -o tail tail.c

clean:
	rm -f head tail *.o
