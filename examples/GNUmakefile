CC = gcc
CFLAGS = -I.. -g -O2 -Wall -Wextra
LDFLAGS = -L../.libs -lumem -lpthread

all: umem_palloc_demo

umem_palloc_demo: umem_palloc.c umem_palloc.h
	$(CC) $(CFLAGS) -o $@ umem_palloc.c $(LDFLAGS)

clean:
	rm -f umem_palloc_demo

.PHONY: all clean
