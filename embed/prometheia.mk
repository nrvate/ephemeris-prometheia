# SPDX-License-Identifier: GPL-2.0-or-later
#
# libprometheia built from source by a parent project's own makefiles
# (docs/EMBEDDING.md). GNU make 3.81 or later; no CMake, no pkg-config.
#
#   PROMETHEIA_DIR := third_party/prometheia      # the checkout (required)
#   PROMETHEIA_OBJDIR := obj-prometheia           # where its objects go
#   include $(PROMETHEIA_DIR)/embed/prometheia.mk
#   ifneq ($(PROMETHEIA_EMBED_CONTRACT),1)
#   $(error Prometheia embed contract is $(PROMETHEIA_EMBED_CONTRACT); this build knows 1)
#   endif
#   CPPFLAGS += $(PROMETHEIA_CPPFLAGS)
#   app: app.o $(PROMETHEIA_LIB)
#   	$(CXX) -o $@ app.o $(PROMETHEIA_LIB) $(PROMETHEIA_LDLIBS)
#
# It defines, and changes nothing else in the parent's build:
#   PROMETHEIA_EMBED_CONTRACT  the "contract N" line of embed/sources.txt
#   PROMETHEIA_SRCS            the "src/" lines, as paths under PROMETHEIA_DIR
#   PROMETHEIA_CPPFLAGS        -I for <prometheia/prometheia.h>; the parent's
#                              own sources that include it need it too
#   PROMETHEIA_CXXFLAGS        what the library's numbers depend on; not
#                              optional, and the parent's CXXFLAGS are NOT
#                              applied to the library, so a -ffast-math or
#                              -Ofast there cannot reach it
#   PROMETHEIA_OPTFLAGS        ?= -O2 (override for a debug or sanitizer build)
#   PROMETHEIA_EXTRA_CXXFLAGS  ?= empty (e.g. -g, -fsanitize=..., -fPIC)
#   PROMETHEIA_ZSTD_CFLAGS     ?= empty (e.g. -I for a zstd.h off the default path)
#   PROMETHEIA_ZSTD_LIBS       ?= -lzstd
#   PROMETHEIA_LDLIBS          what linking the archive needs
# and, when PROMETHEIA_OBJDIR is set, PROMETHEIA_OBJS, PROMETHEIA_LIB
# ($(PROMETHEIA_OBJDIR)/libprometheia.a) and the rules that build them. Give
# PROMETHEIA_OBJDIR a directory of its own: its pattern rule is
# $(PROMETHEIA_OBJDIR)/%.o, and the library's object names (engine.o, time.o,
# ...) are generic.

ifeq ($(PROMETHEIA_DIR),)
$(error PROMETHEIA_DIR must name the Prometheia checkout before including prometheia.mk)
endif

PROMETHEIA_MANIFEST := $(PROMETHEIA_DIR)/embed/sources.txt
PROMETHEIA_EMBED_CONTRACT := $(shell sed -n 's/^contract  *//p' '$(PROMETHEIA_MANIFEST)')
ifeq ($(PROMETHEIA_EMBED_CONTRACT),)
$(error $(PROMETHEIA_MANIFEST): no "contract N" line (is PROMETHEIA_DIR the checkout?))
endif
PROMETHEIA_SRCS := $(addprefix $(PROMETHEIA_DIR)/,$(shell grep '^src/' '$(PROMETHEIA_MANIFEST)'))

PROMETHEIA_CPPFLAGS := -I$(PROMETHEIA_DIR)/include
# No fused multiply-adds: clang contracts a*b+c by default and arm64 (Apple
# silicon) has the instruction, so without this the numbers would depend on
# the target. CMakeLists.txt passes the same flag for the same reason.
PROMETHEIA_CXXFLAGS := -std=c++20 -ffp-contract=off
PROMETHEIA_OPTFLAGS ?= -O2
PROMETHEIA_EXTRA_CXXFLAGS ?=
PROMETHEIA_ZSTD_CFLAGS ?=
PROMETHEIA_ZSTD_LIBS ?= -lzstd
PROMETHEIA_LDLIBS := $(PROMETHEIA_ZSTD_LIBS) -lm

ifneq ($(PROMETHEIA_OBJDIR),)
PROMETHEIA_OBJS := $(patsubst $(PROMETHEIA_DIR)/src/%.cpp,$(PROMETHEIA_OBJDIR)/%.o,$(PROMETHEIA_SRCS))
PROMETHEIA_LIB := $(PROMETHEIA_OBJDIR)/libprometheia.a

# An included file's first explicit target would become the parent's default
# goal; keep whatever the parent had (or nothing, so its next target wins).
prometheia_saved_goal := $(.DEFAULT_GOAL)

$(PROMETHEIA_OBJDIR)/%.o: $(PROMETHEIA_DIR)/src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(PROMETHEIA_CXXFLAGS) $(PROMETHEIA_OPTFLAGS) $(PROMETHEIA_EXTRA_CXXFLAGS) \
	    $(PROMETHEIA_CPPFLAGS) $(PROMETHEIA_ZSTD_CFLAGS) -MMD -MP -c $< -o $@

$(PROMETHEIA_LIB): $(PROMETHEIA_OBJS)
	@rm -f $@
	$(AR) rcs $@ $(PROMETHEIA_OBJS)

-include $(PROMETHEIA_OBJS:.o=.d)

.DEFAULT_GOAL := $(prometheia_saved_goal)
endif
