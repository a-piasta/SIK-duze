CC = gcc
CFLAGS = -Wall -Wextra -O2 -g
TARGETS = radio-proxy radio-client

all: $(TARGETS)

client_protocol.o: client_protocol.c client_protocol.h

http_connection.o: http_connection.c utils.h

radio-proxy.o: radio-proxy.c client_protocol.h utils.h

radio-client.o: radio-client.c utils.h telnet.h

utils.o: utils.c utils.h

radio-proxy: radio-proxy.o http_connection.o client_protocol.o utils.o
	$(CC) $(CFLAGS) $^ -o $@ -pthread

radio-client: radio-client.o utils.o client_protocol.o
	$(CC) $(CFLAGS) $^ -o $@ -pthread

clean:
	rm -f *.o *~ $(TARGETS)
