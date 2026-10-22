CC := gcc
CFLAGS := -O0 -std=gnu11 -Wall -Werror -Wno-unused-parameter -g

# Include path for audit.h
INCLUDES := -I../

all: audit_reader restore

audit_reader: audit_reader.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $<

clean:
	rm -f audit_reader restore

.PHONY: all clean
