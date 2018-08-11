src = $(wildcard ./*.cpp)
obj = $(patsubst %.cpp, %.o, $(src))
EXEC_DIR = ./
CC = g++
CFLAGS = -Wall -g
LFLAGS = -lpthread -levent
target = test
all:$(obj)
	@mkdir -p $(EXEC_DIR)
	$(CC)  $(obj) -o  $(EXEC_DIR)$(target) $(LFLAGS) 

%.o:%.c
	$(CC) -Wall -g -c $< -o $@ 
	
.PHONY:clean
clean:
	-rm -f $(obj) $(EXEC_DIR)$(target)
