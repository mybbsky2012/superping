CC=gcc
CFLAGS=-Wall -ggdb -O

superping: superping.c
	$(CC) $(CFLAGS) -o superping superping.c

.PHONY: clean
clean:
	rm -f superping
