-include ../config.mk

CC = gcc
PYTHON = python3
WARNINGS := -Wall -Wextra -Wshadow -Werror

CPPFLAGS = -I/usr/local/include -D_XOPEN_SOURCE=600
CFLAGS = -O2 -I../include -std=c11 -MMD -MP $(WARNINGS)
LDFLAGS = -O2 -L/usr/local/lib
LDLIBS = -lsoundio -lmp3lame -lsndfile -lavformat -lavcodec -lavutil -lvterm -lpthread -lm

TEST_OBJ := ../src/audio.o ../src/cast.o ../src/json.o ../src/xwrap.o ../src/audio/pcm.o ../src/audio/writer-lame.o ../src/audio/writer-m4a.o ../src/audio/writer-raw.o ../src/audio/writer-sndfile.o ../src/tui/hud.o ../src/tui/screen.o

TARGET := test_unit

all: test

$(TARGET): test_unit.o $(TEST_OBJ)
	$(CC) $(LDFLAGS) -o $@ test_unit.o $(TEST_OBJ) $(LDLIBS)

test: $(TARGET)
	./$(TARGET)
	$(PYTHON) run_tests.py

clean:
	$(RM) $(TARGET) test_unit.o test_unit.d

-include test_unit.d

.PHONY: all test clean
