CC = gcc
CFLAGS = -Wall -g
LIBS = -lpcap

all: server client

server: udp_receiver.cpp
	$(CC) $(CFLAGS) -o udp_server udp_receiver.cpp $(LIBS)

client: udp_sender.cpp
	$(CC) $(CFLAGS) -o udp_client udp_sender.cpp $(LIBS)

clean:
	rm -f udp_server udp_client 

.PHONY: all clean
