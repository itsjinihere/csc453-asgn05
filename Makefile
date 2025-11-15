CC      = gcc
CFLAGS  = -Wall -ansi -pedantic -g
LDFLAGS = 

OBJS    = minix_fs.o
PROGS   = minls minget

all: $(PROGS)

minix_fs.o: minix_fs.c minix_fs.h
	$(CC) $(CFLAGS) -c minix_fs.c

minls.o: minls.c minix_fs.h
	$(CC) $(CFLAGS) -c minls.c

minget.o: minget.c minix_fs.h
	$(CC) $(CFLAGS) -c minget.c

minls: minls.o minix_fs.o
	$(CC) $(CFLAGS) -o minls minls.o minix_fs.o $(LDFLAGS)

minget: minget.o minix_fs.o
	$(CC) $(CFLAGS) -o minget minget.o minix_fs.o $(LDFLAGS)

clean:
	rm -f *.o *~ TAGS $(PROGS)
