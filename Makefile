CC = gcc
CFLAGS = -Wall -Wextra -Werror -std=c11 -D_POSIX_C_SOURCE=200809L -g
SRCS = main.c pager.c table.c btree.c wal.c tokenizer.c
HDRS = pager.h table.h btree.h wal.h tokenizer.h
TARGET = cqlite

$(TARGET): $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) $(SRCS) -o $(TARGET)

bench: pager.c table.c btree.c wal.c bench.c $(HDRS)
	$(CC) -O3 -Wall -Wextra -Werror -std=c11 -D_POSIX_C_SOURCE=200809L pager.c table.c btree.c wal.c bench.c -o bench

clean:
	rm -f $(TARGET) bench *.db *.wal
