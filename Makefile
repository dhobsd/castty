CC = gcc
CFLAGS = -O2 -I/usr/local/include -Wall -Wextra -Wshadow

TARGET = castty

all: $(TARGET)

castty: castty.o audio.o
	$(CC) $(CFLAGS) -o castty castty.o audio.o -L/usr/local/lib -lportaudio -lpthread

clean:
	rm -f *.o $(TARGET) 
