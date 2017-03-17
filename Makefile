-include config.mk

CC = gcc

WARNINGS := -Wall -Wextra -Wshadow

CPPFLAGS = -I/usr/local/include -D_XOPEN_SOURCE=600
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
	CPPFLAGS += -D_DARWIN_C_SOURCE
endif

CFLAGS = -O2 -std=c11 -MMD -MP $(WARNINGS)
LDFLAGS = -L/usr/local/lib
LDLIBS = -lsoundio -lpthread -lmp3lame

ifeq ($(DEBUG),1)
	CFLAGS += -O0 -g3
endif

TARGET := castty
OBJ := audio.o castty.o input.o output.o shell.o xwrap.o

all: $(TARGET)
$(TARGET): $(OBJ)

clean:
	$(RM) $(OBJ) $(OBJ:.o=.d) $(TARGET)

-include $(OBJ:.o=.d)

.PHONY: all clean
