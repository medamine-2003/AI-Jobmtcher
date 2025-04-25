
all: main

# Compile sqlite3.c as C code into an object file
sqlite3.o: sqlite3.c
	gcc -c sqlite3.c -o sqlite3.o

# Compile main.cpp as C++ code into an object file
main.o: main.cpp
	g++ -std=c++17 -c main.cpp -o main.o $(shell pkg-config --cflags sqlite3)

# Link the object files into the final executable
main: main.o sqlite3.o
	g++ main.o sqlite3.o -o main $(shell pkg-config --libs sqlite3) -ldl -lpthread

clean:
	rm -f main main.o sqlite3.o
