
all:
	gcc -c respawn.c -o respawn.o
	gcc respawn.o -o respawn

clean:
	rm respawn
	rm respawn.o

install: all
	cp respawn /usr/local/bin
