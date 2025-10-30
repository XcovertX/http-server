CC = gcc
CFLAGS = -O2 -Wall -Wextra -pedantic -std=c11
# On MSYS2 MinGW, -pthread pulls in winpthreads; keep it cross-platform-friendly
LDFLAGS = -pthread
LDLIBS =

# Add ws2_32 on Windows builds (MSYS2 defines __MINGW32__ / __MINGW64__)
ifeq ($(OS),Windows_NT)
  LDLIBS += -lws2_32
endif

all: httpd
httpd: server.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

clean:
	rm -f httpd *.o