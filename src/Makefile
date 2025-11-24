#
#  This file is part of mqttv4.
#  Copyright (c) 2018-2019 Davide Maggioni.
# 

-include ../config.mak

TARGET = mqttv4

LIBS += -L../lib
ifdef USE_MOSQUITTO
MQTT_LIB = "-DUSE_MOSQUITTO"
LIBS += -lmosquitto
else
LIBS += -lwolfmqtt \
        -lwolfssl
endif
LIBS += -lrt \
        -lpthread

CFLAGS += -Os \
          -Wall \
          -I../include 

.PHONY: default all clean

default: $(TARGET)
all: default

OBJECTS = $(patsubst %.c, %.o, $(wildcard *.c))
HEADERS = $(wildcard *.h)

%.o: %.c $(HEADERS)
	$(CC) $(MQTT_LIB) $(CFLAGS) -c $< -o $@

.PRECIOUS: $(TARGET) $(OBJECTS)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -Wall $(LIBS) -o $@
	$(STRIP) $(TARGET)

clean:
	rm -f *.o
	rm -f $(TARGET)
