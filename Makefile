all: xs

xs: xs.c
	gcc -std=gnu99 -o xs -g xs.c

clean:
	rm xs
