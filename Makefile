
TARGETS=labdns ssh_scanner

all : $(TARGETS)

% : %.c
	gcc -o $@ $^ -pthread -lrt -pedantic -Wall -Werror -std=c99

clean :
	rm $(TARGETS)
