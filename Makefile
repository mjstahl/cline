all: cline

cline: cline.c
	$(CC) -o cline cline.c -Wall -W -pedantic -std=c99

clean:
	rm cline