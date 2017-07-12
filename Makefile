
all: server.cpp
	$(CXX) -g server.cpp -o server

run: all
	./server 6969
