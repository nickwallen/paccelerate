all: src

src:
	cd src; make

clean:
	cd src; make clean

.PHONY: all src clean
