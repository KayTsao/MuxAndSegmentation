CC = @gcc

BIN_PATH_1 = /home/kaytsao/Desktop/gpac/gpac/bin/gcc
BIN_PATH_2 = /home/kaytsao/Desktop/gpac/gpac/extra_lib/lib/gcc
SRC_PATH = /home/kaytsao/Desktop/gpac/gpac
HEADER_PATH = /home/kaytsao/Desktop/gpac/gpac/include

OPTFLAGS = -O3  -Wall -fno-strict-aliasing -Wno-pointer-sign -fPIC -DPIC -msse2 -DNDEBUG -DGPAC_HAVE_CONFIG_H -I$(SRC_PATH) -fvisibility="hidden"
LDFLAGS = -Wl,--warn-common -Wl,-z,defs
CFLAGS = $(OPTFLAGS) -DGPAC_HAVE_CONFIG_H -I$(HEADER_PATH)
LINKFLAGS = -L$(BIN_PATH_1) -L$(BIN_PATH_2)
LINKFLAGS += -lgpac

PROG = TEST#MP4Box

OBJS = main2.o fileimport.o live.o	#main.o filedump.o 
SRCS := $(OBJS:.o=.c) 

all: $(PROG) clean_obj

$(PROG): $(OBJS)
	$(CC) -o $@ $(OBJS) $(LINKFLAGS) $(LDFLAGS)

.PHONY:clean_obj

clean_obj: 
	rm -f $(OBJS)

clean: 
	rm -f $(OBJS) $(PROG)

