#ifndef DGUS_H
#define DGUS_H

#include <Arduino.h>
#include <HardwareSerial.h>

// DWIN DGUS Graphic Commands
#define DGUS_CMD_WRITE_VAR    0x82

// DWIN DGUS Variable Pointers (VP)
#define DGUS_VP_PAGE_SWITCH   0x0084
#define DGUS_VP_GRAPHICS      0x5000  // VP address assigned to Basic Graphic control

// Page indices
#define PAGE_QR_CODE               0
#define PAGE_CLEANING_STARTED      1
#define PAGE_UV_STERILIZATION      2
#define PAGE_DOOR_UNLOCKED         3
#define PAGE_HELMET_DISINFECTION   4
#define PAGE_TAKE_HELMET           5
#define PAGE_PAYMENT_SUCCESS       6
#define PAGE_CLOSE_DOOR            7
#define PAGE_THANK_YOU             8
#define PAGE_DUST_REMOVAL          9
#define PAGE_DRY_HELMET           10
#define PAGE_SANITIZING           11
#define PAGE_WELCOME              12

// Color definitions (16-bit RGB565)
#define COLOR_WHITE 0xFFFF
#define COLOR_BLACK 0x0000

// Struct representing a single rectangle drawing command data
typedef struct {
  uint16_t xs;
  uint16_t ys;
  uint16_t xe;
  uint16_t ye;
  uint16_t color;
} DGUSRect;

// Initialize DWIN UART communication
void dgusInit(HardwareSerial &serial, int rxPin, int txPin, long baudRate = 115200);

// Switch DWIN page
void dgusShowPage(uint8_t pageIndex);

// Clear the QR drawing area (fills the 250x250 region at X:275, Y:115 with White)
void dgusClearQrArea();

// Draw a single filled rectangle on DWIN (immediate draw)
void dgusDrawFilledRect(uint16_t xs, uint16_t ys, uint16_t xe, uint16_t ye, uint16_t color);

// Draw multiple filled rectangles on DWIN using RAM buffering for persistent multi-shape drawing
void dgusDrawRects(const DGUSRect *rects, uint16_t count);

#endif // DGUS_H
