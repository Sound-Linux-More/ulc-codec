.phony: common
.phony: encodetool
.phony: decodetool
.phony: clean

#----------------------------#
# Directories
#----------------------------#

OBJDIR := build

INCDIR := include
COMMON_SRCDIR := fourier libulc
ENCODETOOL_SRCDIR := tools
DECODETOOL_SRCDIR := tools

#----------------------------#
# Cross-compilation, compile flags
#----------------------------#

# Alternatively, try "-march=native" for ARCHFLAGS
ARCHCROSS :=
ARCHFLAGS := -msse -msse2 -mavx -mavx2 -mfma

CCFLAGS := $(ARCHFLAGS) -fno-math-errno -ffast-math -O2 -Wall -Wextra $(foreach dir, $(INCDIR), -I$(dir))
LDFLAGS := -static -s -lm

#----------------------------#
# Tools
#----------------------------#

CC := $(ARCHCROSS)gcc
LD := $(ARCHCROSS)gcc

#----------------------------#
# Files
#----------------------------#

COMMON_SRC     := $(foreach dir, $(COMMON_SRCDIR), $(wildcard $(dir)/*.c))
ENCODETOOL_SRC := $(filter-out $(ENCODETOOL_SRCDIR)/ulcdecodetool.c, $(wildcard $(ENCODETOOL_SRCDIR)/*.c))
DECODETOOL_SRC := $(filter-out $(DECODETOOL_SRCDIR)/ulcencodetool.c, $(wildcard $(DECODETOOL_SRCDIR)/*.c))
COMMON_OBJ     := $(addprefix $(OBJDIR)/, $(notdir $(COMMON_SRC:.c=.o)))
ENCODETOOL_OBJ := $(addprefix $(OBJDIR)/, $(notdir $(ENCODETOOL_SRC:.c=.o)))
DECODETOOL_OBJ := $(addprefix $(OBJDIR)/, $(notdir $(DECODETOOL_SRC:.c=.o)))
ENCODETOOL_EXE := ulcencodetool
DECODETOOL_EXE := ulcdecodetool

DFILES := $(wildcard $(OBJDIR)/*.d)

VPATH := $(COMMON_SRCDIR) $(ENCODETOOL_SRCDIR) $(DECODETOOL_SRCDIR)

#----------------------------#
# General rules
#----------------------------#

$(OBJDIR)/%.o : %.c
	@echo $(notdir $<)
	@$(CC) $(CCFLAGS) -c -o $@ $< -MMD -MP -MF $(OBJDIR)/$*.d

#----------------------------#
# make all
#----------------------------#

all : common encodetool decodetool

$(OBJDIR) :; mkdir -p $@

#----------------------------#
# make common
#----------------------------#

common : $(COMMON_OBJ)

$(COMMON_OBJ) : $(COMMON_SRC) | $(OBJDIR)

#----------------------------#
# make encodetool
#----------------------------#

encodetool : $(ENCODETOOL_EXE)

$(ENCODETOOL_OBJ) : $(ENCODETOOL_SRC) | $(OBJDIR)

$(ENCODETOOL_EXE) : $(COMMON_OBJ) $(ENCODETOOL_OBJ)
	$(LD) $^ $(LDFLAGS) -o $@

#----------------------------#
# make decodetool
#----------------------------#

decodetool : $(DECODETOOL_EXE)

$(DECODETOOL_OBJ) : $(DECODETOOL_SRC) | $(OBJDIR)

$(DECODETOOL_EXE) : $(COMMON_OBJ) $(DECODETOOL_OBJ)
	$(LD) $^ $(LDFLAGS) -o $@

#----------------------------#
# make clean
#----------------------------#

clean :; rm -rf $(OBJDIR) $(ENCODETOOL_EXE) $(DECODETOOL_EXE)

#----------------------------#
# Dependencies
#----------------------------#

include $(wildcard $(DFILES))

#----------------------------#
