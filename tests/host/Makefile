SRC    = ../../src
CFLAGS = -std=c11 -O1 -g -Wall -Wextra -I$(SRC) -fsanitize=address,undefined -pthread

OBJS = test.c port_host.c $(SRC)/client.c $(SRC)/cache.c $(SRC)/config.c $(SRC)/upcase.c $(SRC)/log.c

test: $(OBJS) $(SRC)/a0s.h $(SRC)/proto.h
	$(CC) $(CFLAGS) -o $@ $(OBJS)

clean:
	rm -f test
