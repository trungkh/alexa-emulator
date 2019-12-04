PROG = alexa

STRIP = strip
CFINC = inc
PORTAUDIOINC := portaudio/install/include
PORTAUDIOLIBS := portaudio/install/lib/libportaudio.a

SOURCES += json/jsmn.c \
			configini/configini.c
SOURCES += alexa.cc

ifeq ($(shell uname), Darwin)
	CXX := clang++
	CXXFLAGS = -I$(CFINC) -I./mpg123-1.23.8/src/libmpg123 -g3 -Wall -Wno-sign-compare -Winit-self -I$(PORTAUDIOINC)
	LDLIBS = -lmpg123 -lcurl -ldl -lm -lpthread -framework Accelerate -framework CoreAudio \
		-framework AudioToolbox -framework AudioUnit -framework CoreServices \
		$(PORTAUDIOLIBS) -L/usr/local/lib

	SNOWBOYDETECTLIBFILE := lib/macos/libsnowboy-detect.a
else ifeq ($(shell uname), Linux)
	CXX := g++
	CXXFLAGS = -I$(CFINC) -std=c++0x -g3 -W -Wall -Wno-unused-function -Wno-sign-compare \
			 -Wno-unused-local-typedefs -Winit-self -rdynamic -I$(PORTAUDIOINC)

	LDLIBS = -lmpg123 -lcurl -ldl -lm -Wl,-Bstatic -Wl,-Bdynamic -lrt -lpthread $(PORTAUDIOLIBS) \
				-L/usr/lib/atlas-base -lf77blas -lcblas -llapack_atlas -latlas
      
	ifneq ($(wildcard $(PORTAUDIOINC)/pa_linux_alsa.h),)
		LDLIBS += -lasound
	endif
	ifneq ($(wildcard $(PORTAUDIOINC)/pa_jack.h),)
		LDLIBS += -ljack
	endif

	ifneq (,$(findstring arm,$(shell uname -m)))
		SNOWBOYDETECTLIBFILE := lib/rpi/libsnowboy-detect.a
	else
		SNOWBOYDETECTLIBFILE := lib/ubuntu64/libsnowboy-detect.a	
	endif
endif

all: $(PROG)

$(PROG): $(PORTAUDIOLIBS) $(SOURCES) $(SNOWBOYDETECTLIBFILE)

#alexa: $(SOURCES)
#	$(CC) $(SOURCES) -o $@ $(CFLAGS) $(LDFLAGS)
#	$(STRIP) $@

clean:
	rm -rf *.gc* *.dSYM *.exe *.obj *.o a.out $(PROG)
