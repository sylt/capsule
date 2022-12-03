all: capsule

capsule: capsule.c Makefile
	@pkg-config --exists libevdev \
		|| (>&2 echo "Error: Can't build since libevdev not found. Try \"apt install libevdev-dev\"." && false)
	gcc $< -o $@ -D_GNU_SOURCE -O2 -Wall -Wextra -g $$(pkg-config libevdev --cflags --libs)

format:
	clang-format -i capsule.c

clean:
	rm -f capsule

.PHONY: clean format
