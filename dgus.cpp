#include "dgus.h"

static HardwareSerial *dwinSerial = NULL;

void dgusInit(HardwareSerial &serial, int rxPin, int txPin, long baudRate) {
  dwinSerial = &serial;
  dwinSerial->begin(baudRate, SERIAL_8N1, rxPin, txPin);
}

void dgusShowPage(uint8_t pageIndex) {
  if (dwinSerial == NULL) return;
  
  uint8_t cmd[] = {
    0x5A, 0xA5,      // Header
    0x07,            // Length of payload (7 bytes follow)
    0x82,            // Write command
    0x00, 0x84,      // VP address for Page Selection
    0x5A, 0x01,      // Page Select Code
    0x00, pageIndex  // Page Index (0-255)
  };
  
  dwinSerial->write(cmd, sizeof(cmd));
  dwinSerial->flush();
}

void dgusClearQrArea() {
  // Clear the 200x200 drawing area with White
  // Box coordinates: X: 312 -> 512, Y: 182 -> 382
  dgusDrawFilledRect(312, 182, 512, 382, COLOR_WHITE);
}

void dgusDrawFilledRect(uint16_t xs, uint16_t ys, uint16_t xe, uint16_t ye, uint16_t color) {
  if (dwinSerial == NULL) return;

  uint8_t cmd[] = {
    0x5A, 0xA5,                         // Header
    0x13,                               // Length (19 bytes follow)
    0x82,                               // Write command
    (uint8_t)(DGUS_VP_GRAPHICS >> 8),   // VP High Byte
    (uint8_t)(DGUS_VP_GRAPHICS & 0xFF), // VP Low Byte
    0x00, 0x04,                         // Geometric shape function (0x0004 = Filled Rectangle)
    0x00, 0x01,                         // Count of shapes (1 shape)
    (uint8_t)(xs >> 8), (uint8_t)(xs & 0xFF),
    (uint8_t)(ys >> 8), (uint8_t)(ys & 0xFF),
    (uint8_t)(xe >> 8), (uint8_t)(xe & 0xFF),
    (uint8_t)(ye >> 8), (uint8_t)(ye & 0xFF),
    (uint8_t)(color >> 8), (uint8_t)(color & 0xFF),
    0xFF, 0x00                          // End marker
  };

  dwinSerial->write(cmd, sizeof(cmd));
  dwinSerial->flush();
}

void dgusDrawRects(const DGUSRect *rects, uint16_t count) {
  if (dwinSerial == NULL || count == 0) return;

  // 1. Write shapes to display variable RAM in batches of 20 shapes (200 bytes)
  uint16_t shapesSent = 0;
  while (shapesSent < count) {
    uint16_t batchSize = count - shapesSent;
    if (batchSize > 20) {
      batchSize = 20;
    }

    // Word address: base VP + 2 (header offset) + shapesSent * 5 (each shape is 5 words)
    uint16_t batchVP = DGUS_VP_GRAPHICS + 2 + shapesSent * 5;
    uint16_t payloadLen = 3 + batchSize * 10; // Write CMD (1) + VP (2) + shape data (batchSize * 10)

    dwinSerial->write(0x5A);
    dwinSerial->write(0xA5);
    dwinSerial->write((uint8_t)payloadLen);
    dwinSerial->write(DGUS_CMD_WRITE_VAR);
    dwinSerial->write((uint8_t)(batchVP >> 8));
    dwinSerial->write((uint8_t)(batchVP & 0xFF));

    for (uint16_t i = 0; i < batchSize; i++) {
      uint16_t idx = shapesSent + i;
      dwinSerial->write((uint8_t)(rects[idx].xs >> 8));
      dwinSerial->write((uint8_t)(rects[idx].xs & 0xFF));
      dwinSerial->write((uint8_t)(rects[idx].ys >> 8));
      dwinSerial->write((uint8_t)(rects[idx].ys & 0xFF));
      dwinSerial->write((uint8_t)(rects[idx].xe >> 8));
      dwinSerial->write((uint8_t)(rects[idx].xe & 0xFF));
      dwinSerial->write((uint8_t)(rects[idx].ye >> 8));
      dwinSerial->write((uint8_t)(rects[idx].ye & 0xFF));
      dwinSerial->write((uint8_t)(rects[idx].color >> 8));
      dwinSerial->write((uint8_t)(rects[idx].color & 0xFF));
    }

    dwinSerial->flush();
    delay(8); // Small delay to prevent UART buffer overflow on the screen controller
    shapesSent += batchSize;
  }

  // 2. Write End Marker (0xFF00) to the address immediately following the last shape
  uint16_t endVP = DGUS_VP_GRAPHICS + 2 + count * 5;
  uint8_t endCmd[] = {
    0x5A, 0xA5,
    0x05,
    DGUS_CMD_WRITE_VAR,
    (uint8_t)(endVP >> 8), (uint8_t)(endVP & 0xFF),
    0xFF, 0x00
  };
  dwinSerial->write(endCmd, sizeof(endCmd));
  dwinSerial->flush();
  delay(8);

  // 3. Write Graphic Header (Shape Type and Total Shape Count) to base VP to trigger redrawing
  uint8_t headerCmd[] = {
    0x5A, 0xA5,
    0x07,
    DGUS_CMD_WRITE_VAR,
    (uint8_t)(DGUS_VP_GRAPHICS >> 8), (uint8_t)(DGUS_VP_GRAPHICS & 0xFF),
    0x00, 0x04, // Filled Rectangle Shape Type
    (uint8_t)(count >> 8), (uint8_t)(count & 0xFF) // Total shape count
  };
  dwinSerial->write(headerCmd, sizeof(headerCmd));
  dwinSerial->flush();
}

// ============================================================
// NEW: Write string to native QR Code control
// ============================================================
void dgusSetQrContent(const char *text) {
  if (dwinSerial == NULL) return;

  if (text == NULL) {
    text = "";
  }

  size_t len = strlen(text);
  if (len > 60) return; // safety limit

  // Format 1: Write with 2-byte length prefix (standard T5L Text display variable layout)
  {
    uint8_t buffer[64];
    memset(buffer, 0x00, sizeof(buffer));
    buffer[0] = 0x00;
    buffer[1] = (uint8_t)len;
    if (len > 0) {
      memcpy(&buffer[2], text, len);
    }

    uint8_t payloadLen = 3 + 64; // 67 bytes

    dwinSerial->write(0x5A);
    dwinSerial->write(0xA5);
    dwinSerial->write(payloadLen);
    dwinSerial->write(DGUS_CMD_WRITE_VAR);
    dwinSerial->write((uint8_t)(DGUS_VP_QR_CONTENT >> 8));
    dwinSerial->write((uint8_t)(DGUS_VP_QR_CONTENT & 0xFF));
    dwinSerial->write(buffer, sizeof(buffer));
    dwinSerial->flush();
  }

  // Small delay for display processing
  delay(20);

  // Format 2: Write as raw ASCII terminated by 0xFFFF (alternate DWIN QR layout)
  {
    uint8_t buffer[64];
    memset(buffer, 0xFF, sizeof(buffer));
    if (len > 0) {
      memcpy(buffer, text, len);
    }

    uint8_t payloadLen = 3 + 64; // 67 bytes

    dwinSerial->write(0x5A);
    dwinSerial->write(0xA5);
    dwinSerial->write(payloadLen);
    dwinSerial->write(DGUS_CMD_WRITE_VAR);
    dwinSerial->write((uint8_t)(DGUS_VP_QR_CONTENT >> 8));
    dwinSerial->write((uint8_t)(DGUS_VP_QR_CONTENT & 0xFF));
    dwinSerial->write(buffer, sizeof(buffer));
    dwinSerial->flush();
  }
}

