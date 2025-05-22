CC = gcc
CFLAGS = -Wall -Wextra -g -O3 -std=c11
LDFLAGS = -pthread

TARGETS = watchdog load_balancer reverse_proxy server client

all: $(TARGETS)

watchdog: watchdog.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

load_balancer: load_balancer.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

reverse_proxy: reverse_proxy.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

server: server.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) -lm

client: client.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGETS) *.o

.PHONY: all clean