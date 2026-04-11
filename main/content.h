#ifndef CONTENT_H
#define CONTENT_H

const char chapter_title[] = "CHAPTER II:\nTHE VOID";

const char chapter_content[] =
    "The S3 processor hummed silently as the pixels transitioned. "
    "In the realm of AMOLED, black is not a color-it is an absence. "
    "Every diode turned off, saving power and preserving the deep, "
    "infinite darkness of the display.\n\n"
    "This is the advantage of the SH8601. When the driver is tuned correctly, "
    "the text appears to float in the physical air of the room, unburdened "
    "by the backlight glow of traditional LCD panels.\n\n"
    "By relying entirely on the native LVGL Dark Theme, we allow the "
    "framework to handle the styling autonomously. The internal memory "
    "buffers remain pristine, preventing unnecessary styling overrides "
    "from clogging the CPU cycles before the DMA fires.\n\n"
    "To properly test the bounds of this interface, we must extend "
    "our data payload. As you drag your finger across the capacitive "
    "glass, the FT3168 touch controller interprets the disruption in "
    "its electromagnetic field. It calculates the X and Y coordinates "
    "in real-time, firing off an interrupt over the I2C bus.\n\n"
    "The ESP32 catches this signal, processes it through the input "
    "device driver, and translates the raw capacitive data into a "
    "fluid, kinetic scroll. \n\n"
    "If everything is calibrated perfectly, the transition should be "
    "seamless. The pixels will ignite and extinguish at 80MHz, pushing "
    "data across the Quad-SPI bus fast enough to trick the human eye "
    "into seeing motion where there is only light and the void.";

#endif