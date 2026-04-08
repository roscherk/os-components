obj-m += backdoor.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all: module test_backdoor

module:
	make -C $(KDIR) M=$(PWD) modules

test_backdoor: test_backdoor.c
	gcc -o test_backdoor.o test_backdoor.c

clean:
	make -C $(KDIR) M=$(PWD) clean
	rm -f test_backdoor.o