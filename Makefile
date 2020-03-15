all: xs

xs: xs.c
	gcc -std=gnu99 -o xs xs.c

clean:
	rm xs
