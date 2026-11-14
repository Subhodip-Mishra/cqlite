CC = gcc
CFLAGS = -Wall -Wextra -Werror -std=c11 -D_POSIX_C_SOURCE=200809L -g
SRCS = main.c pager.c table.c btree.c
HDRS = pager.h table.h btree.h
TARGET = cqlite

all: $(TARGET)

$(TARGET): $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) $(SRCS) -o $(TARGET)

clean:
	rm -f $(TARGET) *.db

.PHONY: all clean

bench: pager.c table.c btree.c bench.c $(HDRS)
	$(CC) -O3 -Wall -Wextra -Werror -std=c11 -D_POSIX_C_SOURCE=200809L pager.c table.c btree.c bench.c -o bench
	./bench