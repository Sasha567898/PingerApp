CC = gcc
CFLAGS = -Wall

pinger: pinger.cpp
	$(CC) $(CFLAGS) -o pinger pinger.cpp

clean:
	rm -f pinger