CC = gcc
CFLAGS = -Wall -Wextra -g

all: udp_client udp_server

udp_client: udp_client.o
	$(CC) $(CFLAGS) udp_client.o -o udp_client

udp_server: udp_server.o
	$(CC) $(CFLAGS) udp_server.o -o udp_server

udp_client.o: udp_client.c
	$(CC) $(CFLAGS) -c udp_client.c

udp_server.o: udp_server.c
	$(CC) $(CFLAGS) -c udp_server.c

clean:
	rm -f *.o udp_client udp_server
