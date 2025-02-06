MGF := -L/home/dcslab/MDC-based-Redis/deps/malloc_group/lib -I/home/dcslab/MDC-based-Redis/deps/malloc_group/include -Wl,--rpath=/home/dcslab/MDC-based-Redis/deps/malloc_group/lib -Wl,--dynamic-linker=/home/dcslab/MDC-based-Redis/deps/malloc_group/lib/ld-linux-x86-64.so.2

all: testtest malloc_group
	g++ -o testtest test.o malloc_group.o $(MGF)
testtest:
	gcc -c test.c
malloc_group:
	gcc -c malloc_group.c #-I../../deps/malloc_group/include/ -L../../deps/malloc_group/lib/
