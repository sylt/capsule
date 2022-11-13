all: capsule

capsule: capsule.c Makefile
	gcc $< -o $@ -D_GNU_SOURCE -Wall -Wextra -g $$(pkg-config libevdev --cflags --libs)

format:
	clang-format -i capsule.c

clean:
	rm -f capsule

.PHONY: clean format
