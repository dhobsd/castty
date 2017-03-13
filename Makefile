CC = gcc
CFLAGS = -O2 -I/usr/local/include -Wall -Wextra -Wshadow -std=c11

TARGET = castty

all: $(TARGET)

castty: audio.o castty.o input.o output.o shell.o xwrap.o
	$(CC) $(CFLAGS) -o castty $^ -L/usr/local/lib -lsoundio -lpthread -lmp3lame

clean:
	rm -f *.o $(TARGET) 
