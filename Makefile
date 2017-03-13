CC = gcc

WARNINGS := -Wall -Wextra -Wshadow

CPPFLAGS = -I/usr/local/include
CFLAGS = -O2 -std=c11 -MMD -MP $(WARNINGS)
LDFLAGS = -L/usr/local/lib
LDLIBS = -lsoundio -lpthread -lmp3lame

TARGET := castty
OBJ := audio.o castty.o input.o output.o shell.o xwrap.o

all: $(TARGET)
$(TARGET): $(OBJ)

clean:
	$(RM) $(OBJ) $(OBJ:.o=.d) $(TARGET)

-include $(OBJ:.o=.d)

.PHONY: all clean
