CC = gcc
CFLAGS = -O2 -I/usr/local/include -Wall -Wextra

TARGET = castty

all: $(TARGET)

castty: castty.o io.o audio.o
	$(CC) $(CFLAGS) -o castty castty.o io.o audio.o -L/usr/local/lib -lportaudio

clean:
	rm -f *.o $(TARGET) 
