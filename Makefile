TARGET := MiiTune

DEVKITPRO ?= /opt/devkitpro
DEVKITPPC ?= $(DEVKITPRO)/devkitPPC
WUT_ROOT  ?= $(DEVKITPRO)/wut

PREFIX ?= $(DEVKITPPC)/bin/powerpc-eabi-
CC     := $(PREFIX)gcc
CXX    := $(PREFIX)g++
LD     := $(PREFIX)g++

ELF2RPL  := $(DEVKITPRO)/tools/bin/elf2rpl
WUHBTOOL := $(DEVKITPRO)/tools/bin/wuhbtool

BUILD   := build
SOURCES := src
INCLUDES:= src

MACHDEP := -mcpu=750 -meabi -mhard-float

INCLUDE := -I$(CURDIR)/$(SOURCES) \
           -I$(DEVKITPRO)/portlibs/wiiu/include \
           -I$(DEVKITPRO)/portlibs/ppc/include \
           -I$(WUT_ROOT)/include

LIBPATHS:= -L$(DEVKITPRO)/portlibs/wiiu/lib \
           -L$(DEVKITPRO)/portlibs/ppc/lib \
           -L$(WUT_ROOT)/lib

LIBS    := -lSDL2_image -lwebp -lsharpyuv \
           -lSDL2_ttf -lSDL2 \
           -lharfbuzz \
           -lfreetype -lpng16 -lpng -lbz2 -ljpeg \
           -lcurl \
           -lbrotlidec -lbrotlicommon \
           -lmbedtls -lmbedx509 -lmbedcrypto \
           -ljansson \
           -lavformat -lavcodec -lswresample -lavutil \
           -lz -lwut -lm -lpthread

CFLAGS   := -g -O2 -Wall $(MACHDEP) $(INCLUDE) -DESPRESSO -D__WIIU__ -D__WUT__
CXXFLAGS := $(CFLAGS) -std=c++17
LDFLAGS  := -specs=$(WUT_ROOT)/share/wut.specs $(MACHDEP) -Wl,-Map,$(BUILD)/$(TARGET).map $(LIBPATHS)

SRCS_CPP := $(wildcard $(SOURCES)/*.cpp)
SRCS_C   := $(wildcard $(SOURCES)/*.c)
OBJS     := $(patsubst $(SOURCES)/%.cpp,$(BUILD)/%.o,$(SRCS_CPP)) \
            $(patsubst $(SOURCES)/%.c,$(BUILD)/%.o,$(SRCS_C))

APP_NAME      := MiiTune
APP_SHORTNAME := MiiTune
APP_AUTHOR    := KatsuMaki1
APP_ICON      := icon.png
APP_TV        := tv.png
APP_DRC       := drc.png
APP_CONTENT   := content

.PHONY: all clean wuhb dist

all: $(TARGET).wuhb

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/%.o: $(SOURCES)/%.cpp | $(BUILD)
	@echo "  CXX  $<"
	@$(CXX) $(CXXFLAGS) -c $< -o $@

# Embedded binary assets: rebuild when they change
$(BUILD)/embedded_assets.o: content/font.ttf icon.png assets/icons_atlas.png

$(BUILD)/%.o: $(SOURCES)/%.c | $(BUILD)
	@echo "  CC   $<"
	@$(CC) $(CFLAGS) -c $< -o $@

$(TARGET).elf: $(OBJS)
	@echo "  LD   $@"
	@$(LD) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

$(TARGET).rpx: $(TARGET).elf
	@echo "  ELF2RPL $@"
	@$(ELF2RPL) $< $@

$(TARGET).wuhb: $(TARGET).rpx $(APP_ICON) $(APP_TV) $(APP_DRC)
	@echo "  WUHBTOOL $@"
	@$(WUHBTOOL) $(TARGET).rpx $@ \
		--name="$(APP_NAME)" \
		--short-name="$(APP_SHORTNAME)" \
		--author="$(APP_AUTHOR)" \
		--icon=$(APP_ICON) \
		--tv-image=$(APP_TV) \
		--drc-image=$(APP_DRC) \
		--content=$(APP_CONTENT)
	@echo ">>> Build completed successfully: $(TARGET).wuhb <<<"

# Folder to copy as-is to the root of the SD card: Aroma (.wuhb) AND Homebrew Launcher (.rpx + meta.xml)
dist: all
	@rm -rf dist
	@mkdir -p dist/wiiu/apps/MiiTune
	@cp $(TARGET).wuhb dist/wiiu/apps/
	@cp $(TARGET).rpx icon.png dist/wiiu/apps/MiiTune/
	@cp meta.xml dist/wiiu/apps/MiiTune/meta.xml
	@echo ">>> dist/ ready: copy the 'wiiu' folder to the root of the SD card <<<"

clean:
	@echo "Cleaning build artifacts..."
	@rm -rf $(BUILD) dist $(TARGET).elf $(TARGET).rpx $(TARGET).wuhb
