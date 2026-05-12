all: xdp_firewall.bpf.o xdp_firewall_loader

xdp_firewall.bpf.o: xdp_firewall.bpf.c
	clang -g -O2 -Wall -Wextra -target bpf -c $< -o $@

xdp_firewall_loader: xdp_firewall_loader.c
	gcc -O2 -Wall -Wextra $< -lbpf -o $@

clean:
	rm -f xdp_firewall.bpf.o xdp_firewall_loader
