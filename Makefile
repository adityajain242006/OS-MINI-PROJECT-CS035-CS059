# CampusCompute - OS Mini-Project (UE25CS242A)
# Team: Amudan Desikan (PES1UG25CS059), Aditya Jain (PES1UG25CS035)
#
#   make            build ./campuscompute
#   make test       run every feature once (quick smoke test)
#   make run-demo   run the integrated demonstration
#   make zip        create the submission archive
#   make clean      remove object files and the executable
#   make distclean  clean + remove generated logs and the archive

CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -Wpedantic -O2 -D_GNU_SOURCE -Iinclude -pthread
LDLIBS  := -pthread -lrt

TARGET  := campuscompute
SRCS    := $(wildcard src/*.c)
OBJS    := $(patsubst src/%.c,build/%.o,$(SRCS))
HDRS    := $(wildcard include/*.h)

ZIPNAME := CampusCompute_PES1UG25CS059_PES1UG25CS035.zip

.PHONY: all clean distclean test run-demo zip zipcheck

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

build/%.o: src/%.c $(HDRS) | build
	$(CC) $(CFLAGS) -c $< -o $@

build:
	mkdir -p build

test: $(TARGET)
	./$(TARGET) help
	./$(TARGET) jobs tests/jobs.txt
	./$(TARGET) schedule fcfs tests/jobs.txt
	./$(TARGET) schedule rr tests/jobs.txt -q 2
	./$(TARGET) schedule priority tests/jobs.txt
	./$(TARGET) schedule compare tests/jobs.txt
	./$(TARGET) logger
	./$(TARGET) ipc
	./$(TARGET) banker tests/banker_safe.txt
	./$(TARGET) banker tests/banker_unsafe.txt
	./$(TARGET) workers
	./$(TARGET) workers --fail 2
	./$(TARGET) rlimit
	@echo "--- expected failure: the malformed job file must be rejected ---"
	./$(TARGET) jobs tests/jobs_invalid.txt || echo "(rejected as expected, exit status $$?)"
	./$(TARGET) cleanup --check

run-demo: $(TARGET)
	./$(TARGET) demo

zip:
	rm -rf $(ZIPNAME) .pkg
	mkdir -p .pkg/CampusCompute
	cp -r Makefile README.md include src tests .pkg/CampusCompute/
	cd .pkg && zip -q -r ../$(ZIPNAME) CampusCompute
	rm -rf .pkg
	@echo "created $(ZIPNAME) ($$(du -h $(ZIPNAME) | cut -f1))"

zipcheck:
	unzip -l $(ZIPNAME)
	unzip -tq $(ZIPNAME)

clean:
	rm -rf build $(TARGET)

distclean: clean
	rm -rf logs .pkg $(ZIPNAME)
