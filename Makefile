# Makefile para Servidor CSV RESTful
# IC6600 - Principios de Sistemas Operativos

CC = gcc
CFLAGS = -Wall -Wextra -g
TARGET = csv_server
SRC = csv_server.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

clean:
	rm -f $(TARGET)

run: $(TARGET)
	./$(TARGET)

.PHONY: all clean run
