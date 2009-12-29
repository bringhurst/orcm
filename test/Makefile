PROGS = spinners/cm_spin spinners/cm_spin2 pnp/talker pnp/listener spinners/spin pnp/client pnp/server

all: $(PROGS)

CC = orcmcc

%: %.c
	$(CC) -g -o $@ $<

# Simplistic install target for use by the ebuild system
INSTALL_DIR = .
install: $(PROGS)
	mkdir -p $(INSTALL_DIR)/bin
	cp -p $(PROGS) $(INSTALL_DIR)/bin

clean:
	rm -f $(PROGS) *~
