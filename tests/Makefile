# SPDX-License-Identifier: GPL-2.0-or-later
SRC := ../package/meshd/src

.PHONY: test clean
test: test_protocol
	./test_protocol

test_protocol: test_protocol.c $(SRC)/protocol.c $(SRC)/protocol.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=gnu11 -Wall -Wextra -Werror -I$(SRC) \
		test_protocol.c $(SRC)/protocol.c $(LDFLAGS) -o $@

clean:
	rm -f test_protocol
