# makefile - xcespproc
# Project: XCESP

include PROJECT

# Detect Linux and enable epoll backend by default (override with DISABLE_EPOLL=1)
OS := $(shell uname -s)
ifeq ($(OS),Linux)
    ifeq ($(DISABLE_EPOLL),1)
        EPOLL_FLAG :=
    else
        EPOLL_FLAG := -DEV_EPOLL_SUPPORT
    endif
else
    EPOLL_FLAG :=
endif

CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -I include -I src -I src/objects -I src/links -I ext/include \
            -DPRJNAME=\"$(PRJNAME)\" -DPRJVERSION=\"$(PRJVERSION)\" \
            $(EPOLL_FLAG)
LDFLAGS  := -L lib -L ext/lib -levapplication -llogservice -liniconfig -largconfig -lpthread

SRCDIR   := src
BUILDDIR := build
BINDIR   := bin
TESTDIR  := test
TESTSRC  := src/test

# Automated file searching — includes src/objects/ and src/links/ sources
SRCS     := $(wildcard $(SRCDIR)/*.cpp) $(wildcard $(SRCDIR)/objects/*.cpp) $(wildcard $(SRCDIR)/links/*.cpp)
OBJS     := $(patsubst $(SRCDIR)/%.cpp,$(BUILDDIR)/%.o,$(SRCS))
TSRCS    := $(wildcard $(TESTSRC)/*.cpp)
TOBJS    := $(patsubst $(TESTSRC)/%.cpp,$(BUILDDIR)/test_%.o,$(TSRCS))
TBINS    := $(patsubst $(TESTSRC)/%.cpp,$(TESTDIR)/%,$(TSRCS))

# Exclude main.o when linking test binaries
MAIN_OBJ    := $(BUILDDIR)/main.o
NOMAIN_OBJS := $(filter-out $(MAIN_OBJ),$(OBJS))

# Subproject source directories in exsrc
SUBPROJECTS := $(wildcard exsrc/*/makefile)

# Application binary target
TARGET   := $(BINDIR)/xcespproc

.PHONY: all clean install test subprojects

all: subprojects $(TARGET)

# Build each dependency, then copy its library and headers into our lib/ and include/
subprojects:
	@for mkf in $(SUBPROJECTS); do \
		dir=$$(dirname $$mkf); \
		$(MAKE) -C $$dir; \
		if [ -d $$dir/lib ]; then cp -u $$dir/lib/*.a lib/ 2>/dev/null || true; fi; \
		if [ -d $$dir/include ]; then cp -u $$dir/include/*.h include/ 2>/dev/null || true; fi; \
	done

$(TARGET): $(OBJS) | $(BINDIR)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# Rule for src/*.cpp → build/%.o
$(BUILDDIR)/%.o: $(SRCDIR)/%.cpp | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# Rule for src/objects/*.cpp → build/objects/%.o
$(BUILDDIR)/objects/%.o: $(SRCDIR)/objects/%.cpp | $(BUILDDIR)/objects
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# Test targets
test: subprojects all $(TBINS)
	@for t in $(TBINS); do \
		echo "--- Running $$t ---"; \
		./$$t || exit 1; \
	done

$(TESTDIR)/%: $(BUILDDIR)/test_%.o $(NOMAIN_OBJS)
	@mkdir -p $(TESTDIR)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILDDIR)/test_%.o: $(TESTSRC)/%.cpp | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# Install - application project; no library to propagate to parent
install: all

# Create output directories if missing
$(BINDIR) $(BUILDDIR):
	mkdir -p $@

$(BUILDDIR)/objects:
	mkdir -p $@

# Rule for src/links/*.cpp → build/links/%.o
$(BUILDDIR)/links/%.o: $(SRCDIR)/links/%.cpp | $(BUILDDIR)/links
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILDDIR)/links:
	mkdir -p $@

clean:
	rm -rf $(BUILDDIR) $(BINDIR)/xcespproc $(TESTDIR)/xcespproc
