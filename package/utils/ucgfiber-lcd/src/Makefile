CFLAGS ?= -O2
CFLAGS += -Wall -Wextra

all: ucgfiber-lcd

ucgfiber-lcd: ucgfiber-lcd.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -f ucgfiber-lcd
