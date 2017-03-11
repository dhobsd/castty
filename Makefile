CC = gcc
CFLAGS = -O2 -I/usr/local/include -Wall -Wextra -Wshadow

TARGET = castty

all: $(TARGET)

castty: audio.o castty.o input.o output.o shell.o xwrap.o
	$(CC) $(CFLAGS) -o castty $^ -L/usr/local/lib -lportaudio -lpthread

clean:
	rm -f *.o $(TARGET) 
