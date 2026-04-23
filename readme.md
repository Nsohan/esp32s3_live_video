to run this on ESP-IDF Terminal
cmd: idf.py build flash -p COM6 monitor (COM6 is the serial port of USB BUS)

Enable PSRAM in menuconfig
cmd: idf.py menuconfig

Navigate using arrow keys:
    Go to Component config → press Enter
    Go to ESP PSRAM → press Enter
    Enable Support for external, SPI-connected RAM → press Space to enable
    Go into SPI RAM config → press Enter
    Make sure Mode is set to Octal Mode PSRAM (your board has N16R8 = Octal PSRAM)
    Press S to save, then Q to quit

Then rebuild and flash:
    idf.py build flash -p COM6 (COM6 is the serial port of USB BUS)

