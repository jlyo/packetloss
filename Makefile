CFLAGS = -Wall -Wextra -Werror -std=gnu99 -g
TARGET = packetloss

all: $(TARGET)

clean:
	-rm -f $(TARGET)

.PHONY: all clean
