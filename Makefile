CC = gcc
CFLAGS = -Wall -Wextra -O2 -pedantic -std=c11
LDFLAGS = -pthread

all: httpd

httpd: server.o
	$(CC) $(CFLAGS) -o httpd server.o $(LDFLAGS)

server.o: server.c

clean:
	rm -f httpd server.o