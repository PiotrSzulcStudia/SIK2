CC = gcc
CFLAGS = -Wall -O2
TARGETS = player

all: $(TARGETS)

err.o: err.c err.h

player.o: player.c err.h

player: player.o err.o

clean:
	rm -f *.o $(TARGETS)
