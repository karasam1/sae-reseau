CC = gcc
CFLAGS = -Wall -Wextra
LDFLAGS = -pthread

all: server_thread server_select

server_thread: server_thread.c
	$(CC) $(CFLAGS) server_thread.c -o server_thread $(LDFLAGS)

server_select: server_select.c
	$(CC) $(CFLAGS) server_select.c -o server_select

clean:
	rm -f server_thread server_select
