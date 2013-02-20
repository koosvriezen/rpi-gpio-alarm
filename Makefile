OBJECTS=main.o loop.o

alarm: $(OBJECTS)
	g++ -s -o $@ $^

%.o: %.c
	gcc -c -O2 -Wall $< -o $@

%.o: %.cpp
	g++ -c -O2 -Wall $< -o $@
