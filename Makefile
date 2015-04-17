
labdns : labdns.c
	gcc -o labdns labdns.c -lrt -pedantic -Wall -Werror -std=c99

clean :
	rm labdns
