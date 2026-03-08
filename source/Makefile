CXX = g++
CXXFLAGS = -std=c++11 -Wall -Wextra -O2 -pthread

TARGET = tnc_test
SRC = main.cpp

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SRC)

clean:
	rm -f $(TARGET)

# Alvo para execução rápida (ajuste as variaveis conforme necessário)
TARGET_CALL ?= G2UGK-1
PORT ?= /tmp/ttyKISS
BAUD ?= 9600

run: $(TARGET)
	./$(TARGET) $(PORT) $(BAUD) $(TARGET_CALL)

