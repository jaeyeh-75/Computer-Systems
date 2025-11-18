CC = clang
CFLAGS = -Wall -Wextra -Werror -pedantic
FORMAT = clang-format -i -style=file
SRC = $(wildcard *.c)
HEADERS = $(wildcard *.h)

all: format slugterm

format:
	@$(FORMAT) $(SRC) $(HEADERS)

slugterm: $(SRC)
	$(CC) $(CFLAGS) -o slugterm $(SRC)

clean:
	rm -f slugterm *.o
