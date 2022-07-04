CXX = g++
CXXFLAGS = -Wall -Wextra -std=c++17 -O2
LDFLAGS =
LIBS = 

SRCS = common.cpp err.cpp screen-worms-server.cpp screen-worms-client.cpp
OBJS = $(subst .cpp,.o, $(SRCS))

all: screen-worms-server screen-worms-client

screen-worms-client: $(OBJS)
		$(CXX) $(LDFLAGS) -o screen-worms-client err.o common.o screen-worms-client.o $(LIBS)

screen-worms-server: $(OBJS)
		$(CXX) $(LDFLAGS) -o screen-worms-server err.o common.o screen-worms-server.o $(LIBS)

err.o: err.cpp err.h
	   $(CXX) $(CXXFLAGS) -c $<
	   
common.o: common.cpp common.h
	   $(CXX) $(CXXFLAGS) -c $<

screen-worms-server.o: screen-worms-server.cpp err.h common.h
		$(CXX) $(CXXFLAGS) -c $<
		
screen-worms-client.o: screen-worms-client.cpp err.h common.h
		$(CXX) $(CXXFLAGS) -c $<

clean:
	rm -f $(OBJS) screen-worms-server screen-worms-client
