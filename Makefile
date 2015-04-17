
labdns : labdns.c
	gcc -o labdns labdns.c -lrt

clean :
	rm labdns
