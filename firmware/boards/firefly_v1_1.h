// FireFly V1.1 LED controller (RP2040), see ~/firefly-led-controller/FireFlyV1_1.
// Pico SDK board headers must be preprocessor macros; tvlight code uses the
// constexpr pin names in src/config.h instead.
//
//   GPIO0-4   BOARD_ID_0..4 (hard-tied on the PCB; inputs only, never drive)
//   GPIO10    STATUS_LED (active high, 220R)
//   GPIO14-16 LED_OUT_1..3 (strip data, 3.3 V, no level shifter)
//   GPIO22    BUTTON, GPIO23/24 ENC_A/ENC_B, GPIO25 ENC_SW
//   GPIO26    MIC_ADC, GPIO27 POT_ADC
//   W25Q32JV  4 MB flash, 12 MHz crystal
#ifndef _BOARDS_FIREFLY_V1_1_H
#define _BOARDS_FIREFLY_V1_1_H

#define FIREFLY_V1_1

// No default UART/LED: pins 0/1 are board ID straps and GPIO25 is the encoder switch.
#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1
#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (4 * 1024 * 1024)
#endif
#ifndef PICO_RP2040_B0_SUPPORTED
#define PICO_RP2040_B0_SUPPORTED 1
#endif
#ifndef PICO_XOSC_STARTUP_DELAY_MULTIPLIER
#define PICO_XOSC_STARTUP_DELAY_MULTIPLIER 64
#endif

#endif
