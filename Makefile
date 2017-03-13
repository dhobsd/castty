CC = gcc
CFLAGS = -O2 -I/usr/local/include -Wall -Wextra -Wshadow -std=c11 -MMD -MP

TARGET = castty
OBJ := audio.o castty.o input.o output.o shell.o xwrap.o

all: $(TARGET)

castty: $(OBJ)
	$(CC) $(CFLAGS) -o castty $^ -L/usr/local/lib -lsoundio -lpthread -lmp3lame

clean:
	rm -f *.o *.d $(TARGET)

-include $(OBJ:.o=.d)
