.PHONY: all
all:
	gcc -Wno-int-conversion -Wno-error -o code main.c buddy.c