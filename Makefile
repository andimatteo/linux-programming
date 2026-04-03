CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra
LDFLAGS ?=
LDLIBS_SERVER ?= -luring -pthread
LDLIBS_CLIENT ?=
SERVER := server
CLIENT := client
SERVER_SRC := server.c
CLIENT_SRC := client.c
.PHONY: all clean run-server run-client
all: $(SERVER) $(CLIENT)
$(SERVER): $(SERVER_SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS_SERVER)
$(CLIENT): $(CLIENT_SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS_CLIENT)
run-server: $(SERVER)
	./$(SERVER)
run-client: $(CLIENT)
	./$(CLIENT)
clean:
	rm -f $(SERVER) $(CLIENT)
