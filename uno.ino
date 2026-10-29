#include <FastLED.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <avr/pgmspace.h>

// ---------- Top-Level User Settings ----------
uint16_t bgmSpeed = 1300;                // Piezo playback speed (lower = faster, higher = slower)
bool ghost_block = true;                 // Enable/disable landing preview ghost block
CRGB ghost_block_color = CRGB(20, 8, 2); // Low-brightness brown (R, G, B)

// ---------- Hardware & Pin Mapping ----------
#define SCREEN_W 8
#define SCREEN_H 32
#define NUM_LEDS (SCREEN_W * SCREEN_H)

#define QUIT_HOLD_MS 3000 // Hold Drop button duration to quit/forfeit

// FastLED Power Control Parameters
#define LED_BRIGHTNESS 45  // Maximum target brightness (0-255)
#define MAX_POWER_VOLTS 5  // Operating voltage (5V)
#define MAX_POWER_MA 1500  // Current cap in milliamps

#define P1_LED_PIN 3  // P1 Matrix Data Pin
#define P2_LED_PIN 4  // P2 Matrix Data Pin
#define BUZZER_PIN 11 // Piezo Buzzer Pin

#define MATRIX_SERPENTINE 1 // 1 = Standard Wokwi 8x32 serpentine layout
#define LOCK_DELAY_MS 500   // Lock delay window when landing on bottom/stack

// Global flag to freeze 2P capability until the next reboot
bool is2PEnabled = true;

// Shared FastLED buffer to preserve SRAM
CRGB leds[NUM_LEDS];

// Separate FastLED controllers for dual output
CLEDController* p1Controller = nullptr;
CLEDController* p2Controller = nullptr;

// ---------- Color Palette ----------
const uint32_t COLOR_PALETTE[] PROGMEM = {
  0x000000, // 0: Empty
  0x00FFFF, // 1: I - Cyan
  0xFFFF00, // 2: O - Yellow
  0x800080, // 3: T - Purple
  0xFF7F00, // 4: L - Orange
  0x0000FF, // 5: J - Blue
  0x00FF00, // 6: S - Green
  0xFF0000  // 7: Z - Red
};

uint16_t getPixelIndex(int8_t x, int8_t y) {
  if (x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H) return 0;

  int8_t mappedY = SCREEN_H - 1 - y;

#if MATRIX_SERPENTINE
  if (mappedY & 1) return (mappedY * SCREEN_W) + (SCREEN_W - 1 - x); // Odd rows go LEFT
  else             return (mappedY * SCREEN_W) + x;                  // Even rows go RIGHT
#else
  return (mappedY * SCREEN_W) + x;
#endif
}

// ---------- BGM Audio Engine ----------
const uint16_t tetrisMelody[] PROGMEM = {
  1318, 988, 1047, 1175, 1047, 988, 890, 890, 1047, 1318, 1175, 1047, 988, 988, 1047, 1175, 1318, 1047, 890, 890, 0,
  0, 1175, 1397, 1760, 1568, 1397, 1318, 1318, 1047, 1318, 1175, 1047, 988, 988, 1047, 1175, 1318, 1047, 890, 890, 0
};
const uint8_t tetrisDurations[] PROGMEM = {
  4, 8, 8, 4, 8, 8, 4, 8, 8, 4, 8, 8, 8, 4, 8, 4, 4, 4, 4, 4, 4,
  8, 4, 8, 4, 8, 8, 8, 4, 8, 4, 8, 8, 8, 4, 8, 4, 4, 4, 4, 4, 4
};
const int numNotes = sizeof(tetrisMelody) / sizeof(tetrisMelody[0]);

int currentNoteIndex = 0;
uint16_t noteStartTime = 0;
bool isNotePlaying = false;

void stopHardwareTone() {
  TIMSK2 = 0;
  TCCR2A = 0;
  TCCR2B = 0;
  digitalWrite(BUZZER_PIN, LOW);
}

void playHardwareTone(uint16_t frequency) {
  if (frequency == 0) {
    stopHardwareTone();
    return;
  }

  pinMode(BUZZER_PIN, OUTPUT);

  uint8_t prescalerBits = 0;
  uint16_t N = 1;

  if      (frequency >= 31250UL) { N = 1;    prescalerBits = _BV(CS20); }
  else if (frequency >= 3907UL)  { N = 8;    prescalerBits = _BV(CS21); }
  else if (frequency >= 1954UL)  { N = 32;   prescalerBits = _BV(CS21) | _BV(CS20); }
  else if (frequency >= 977UL)   { N = 64;   prescalerBits = _BV(CS22); }
  else if (frequency >= 489UL)   { N = 128;  prescalerBits = _BV(CS22) | _BV(CS20); }
  else if (frequency >= 245UL)   { N = 256;  prescalerBits = _BV(CS22) | _BV(CS21); }
  else                           { N = 1024; prescalerBits = _BV(CS22) | _BV(CS21) | _BV(CS20); }

  uint8_t top = (F_CPU / (2UL * N * frequency)) - 1;

  TIMSK2 = 0;
  TCCR2A = 0;
  TCCR2B = 0;
  TCNT2 = 0;
  OCR2A = top;
  TCCR2A = _BV(COM2A0) | _BV(WGM21);
  TCCR2B = prescalerBits;
}

void updateBGM(bool shouldPlay) {
  if (!shouldPlay) {
    if (isNotePlaying) { stopHardwareTone(); isNotePlaying = false; currentNoteIndex = 0; }
    return;
  }
  uint16_t now = (uint16_t)millis();
  uint8_t durationDivider = pgm_read_byte(&(tetrisDurations[currentNoteIndex]));
  uint16_t noteDuration = bgmSpeed / durationDivider;
  uint16_t playDuration = (noteDuration * 85U) / 100U;

  if (!isNotePlaying) {
    uint16_t note = pgm_read_word(&(tetrisMelody[currentNoteIndex]));
    if (note != 0) playHardwareTone(note);
    else stopHardwareTone();
    noteStartTime = now;
    isNotePlaying = true;
  } else {
    uint16_t elapsed = now - noteStartTime;
    if (elapsed >= playDuration && elapsed < noteDuration) {
      stopHardwareTone();
    } else if (elapsed >= noteDuration) {
      currentNoteIndex = (currentNoteIndex + 1) % numNotes;
      isNotePlaying = false;
    }
  }
}

// ---------- Shared Host (PC App) Serial Link ----------
uint16_t gameCounter = 0;
uint16_t pongCount = 0;
char hostRxBuf[12];
uint8_t hostRxIdx = 0;

void pollHostSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (hostRxIdx > 0) {
        hostRxBuf[hostRxIdx] = '\0';
        for (uint8_t i = 0; i < hostRxIdx; i++) {
          if (hostRxBuf[i] >= 'a' && hostRxBuf[i] <= 'z') hostRxBuf[i] -= 32;
        }
        if (strstr(hostRxBuf, "PONG") != NULL) {
          pongCount++;
        }
      }
      hostRxIdx = 0;
    } else if (hostRxIdx < sizeof(hostRxBuf) - 1) {
      hostRxBuf[hostRxIdx++] = c;
    }
  }
}

// ---------- Memory-Optimized Bit-Packed Button Handler ----------
struct DebouncedButton {
  uint8_t pin;
  uint8_t state : 1;
  uint8_t lastReading : 1;
  uint8_t pressedTrigger : 1;
  uint16_t lastDebounceTime;

  void init(uint8_t p) {
    pin = p;
    pinMode(pin, INPUT_PULLUP);
    state = 1; lastReading = 1;
    lastDebounceTime = 0; pressedTrigger = 0;
  }

  void update() {
    bool reading = digitalRead(pin);
    uint16_t now16 = (uint16_t)millis();
    if (reading != lastReading) lastDebounceTime = now16;
    if ((uint16_t)(now16 - lastDebounceTime) > 30) {
      if (reading != state) {
        state = reading;
        if (state == LOW) pressedTrigger = 1;
      }
    }
    lastReading = reading;
  }

  bool wasPressed() {
    if (pressedTrigger) { pressedTrigger = 0; return true; }
    return false;
  }
  bool isDown() { return state == LOW; }
  void clear() { pressedTrigger = 0; }
};

// ---------- PROGMEM Stored Shapes & Wall Kicks ----------
const int8_t I_SHAPE[2][4][2] PROGMEM = { {{0,0},{0,1},{0,2},{0,3}}, {{-1,1},{0,1},{1,1},{2,1}} };
const int8_t O_SHAPE[1][4][2] PROGMEM = { {{0,0},{1,0},{0,1},{1,1}} };

const int8_t T_SHAPE[4][4][2] PROGMEM = {
  {{1,0},{0,1},{1,1},{2,1}},
  {{1,0},{1,1},{1,2},{2,1}},
  {{0,1},{1,1},{2,1},{1,2}},
  {{1,0},{1,1},{1,2},{0,1}}
};

const int8_t L_SHAPE[4][4][2] PROGMEM = { {{0,0},{0,1},{0,2},{1,2}}, {{0,0},{1,0},{2,0},{0,1}}, {{0,0},{1,0},{1,1},{1,2}}, {{2,0},{0,1},{1,1},{2,1}} };
const int8_t J_SHAPE[4][4][2] PROGMEM = { {{1,0},{1,1},{1,2},{0,2}}, {{0,0},{0,1},{1,1},{2,1}}, {{0,0},{1,0},{0,1},{0,2}}, {{0,0},{1,0},{2,0},{2,1}} };
const int8_t S_SHAPE[2][4][2] PROGMEM = { {{1,0},{2,0},{0,1},{1,1}}, {{1,0},{1,1},{2,1},{2,2}} };
const int8_t Z_SHAPE[2][4][2] PROGMEM = { {{0,0},{1,0},{1,1},{2,1}}, {{2,0},{1,1},{2,1},{1,2}} };

const int8_t KICK_OFFSETS[8][2] PROGMEM = {
  { 0, 0}, {-1, 0}, { 1, 0}, { 0, -1},
  {-2, 0}, { 2, 0}, {-1, -1}, { 1, -1}
};

// 5x5 Matrix Text Bitmaps
const uint8_t BITMAP_W[5] PROGMEM = { 0x11, 0x11, 0x15, 0x1B, 0x11 };
const uint8_t BITMAP_I[5] PROGMEM = { 0x1F, 0x04, 0x04, 0x04, 0x1F };
const uint8_t BITMAP_N[5] PROGMEM = { 0x11, 0x19, 0x15, 0x13, 0x11 };

const uint8_t BITMAP_L[5] PROGMEM = { 0x10, 0x10, 0x10, 0x10, 0x1F };
const uint8_t BITMAP_O[5] PROGMEM = { 0x0E, 0x11, 0x11, 0x11, 0x0E };
const uint8_t BITMAP_S[5] PROGMEM = { 0x0F, 0x10, 0x0E, 0x01, 0x1E };
const uint8_t BITMAP_E[5] PROGMEM = { 0x1F, 0x10, 0x1E, 0x10, 0x1F };

const uint8_t BITMAP_G[5] PROGMEM = { 0x0E, 0x10, 0x17, 0x11, 0x0E };
const uint8_t BITMAP_A[5] PROGMEM = { 0x0E, 0x11, 0x1F, 0x11, 0x11 };
const uint8_t BITMAP_M[5] PROGMEM = { 0x11, 0x1B, 0x15, 0x11, 0x11 };
const uint8_t BITMAP_V[5] PROGMEM = { 0x11, 0x11, 0x11, 0x0A, 0x04 };
const uint8_t BITMAP_R[5] PROGMEM = { 0x1E, 0x11, 0x1E, 0x14, 0x11 };

struct Block {
  const int8_t* shapePtr;
  int8_t x, y;
  uint8_t len;
  uint8_t rotation;
  uint8_t colorId;
  char type;
};

enum GameState {
  STATE_MENU,
  STATE_WAITING_2P,
  STATE_PLAYING_1P,
  STATE_PLAYING_2P,
  STATE_GAMEOVER,
  STATE_NAME_ENTRY,
  STATE_SUBMIT_PING,
  STATE_SUBMIT_WAIT,
  STATE_SUBMIT_RESULT
};

enum Mode2P {
  MODE_2P_SURVIVAL,
  MODE_2P_SCORE
};

extern Mode2P active2PMode;
extern bool scoreMatchEvaluated;

class TetrisGame {
private:
  uint8_t playerNum;
  CLEDController* ledController;
  LiquidCrystal_I2C lcd;

  DebouncedButton btnLeft, btnRight, btnUp, btnDown, btnDrop;

  uint8_t field[SCREEN_H][SCREEN_W / 2];
  Block current;

  GameState gameState = STATE_MENU;
  uint8_t menuSelection = 0;

  // Packed Booleans
  uint8_t twoPlayerGame : 1;
  uint8_t wonGame : 1;
  uint8_t isGrounded : 1;
  uint8_t lastClearWasDifficult : 1;
  uint8_t lastRotationWasTSpin : 1;
  uint8_t diedIn2PFlag : 1;
  uint8_t isHoldingDrop : 1;

  uint32_t score = 0;
  uint16_t linesClearedForLevel = 0;
  uint8_t speedLevel = 1;

  // 16-bit Millis Timers
  uint16_t lockStartTime = 0;
  uint16_t gameOverStartTime = 0;
  uint16_t dropHoldStartTime = 0;
  uint16_t submitTimer = 0;
  uint16_t lastDrop = 0;

  uint16_t gameId = 0;
  char playerName[10] = {'A','*','*','*','*','*','*','*','*','*'};
  uint8_t nameCursor = 0;
  uint16_t pongSeen = 0;

  uint8_t getCell(uint8_t y, uint8_t x) const {
    uint8_t val = field[y][x >> 1];
    return (x & 1) ? (val & 0x0F) : (val >> 4);
  }

  void setCell(uint8_t y, uint8_t x, uint8_t color) {
    uint8_t &val = field[y][x >> 1];
    if (x & 1) val = (val & 0xF0) | (color & 0x0F);
    else       val = (val & 0x0F) | ((color & 0x0F) << 4);
  }

  void drawChar5x5(int8_t startX, int8_t startY, const uint8_t* charProgmem, CRGB color) {
    for (uint8_t row = 0; row < 5; row++) {
      uint8_t line = pgm_read_byte(charProgmem + row);
      for (uint8_t col = 0; col < 5; col++) {
        if (line & (1 << (4 - col))) {
          int8_t px = startX + col;
          int8_t py = startY + row;
          if (px >= 0 && px < SCREEN_W && py >= 0 && py < SCREEN_H) {
            leds[getPixelIndex(px, py)] = color;
          }
        }
      }
    }
  }

  void sendLive() {
    Serial.print(F("{\"event\":\"LIVE\",\"player\":"));
    Serial.print(playerNum);
    Serial.print(F(",\"game\":"));
    Serial.print(gameId);
    Serial.print(F(",\"mode\":\""));
    if (twoPlayerGame) Serial.print(F("2P")); else Serial.print(F("1P"));
    Serial.print(F("\",\"score\":"));
    Serial.print(score);
    Serial.print(F(",\"level\":"));
    Serial.print(speedLevel);
    Serial.println(F("}"));
  }

  void sendForceQuitEvent() {
    Serial.print(F("{\"event\":\"FORCE_QUIT\",\"player\":"));
    Serial.print(playerNum);
    Serial.print(F(",\"game\":"));
    Serial.print(gameId);
    Serial.print(F(",\"mode\":\""));
    if (twoPlayerGame) Serial.print(F("2P")); else Serial.print(F("1P"));
    Serial.println(F("\"}"));
  }

  void renderMenuLCD() {
    lcd.clear();
    if (!is2PEnabled) {
      lcd.setCursor(0, 0); lcd.print(F("> 1 Player Mode"));
      lcd.setCursor(0, 1); lcd.print(F(" (2P Disabled) "));
      return;
    }

    if (menuSelection < 2) {
      lcd.setCursor(0, 0); lcd.print(menuSelection == 0 ? F("> 1 Player") : F(" 1 Player"));
      lcd.setCursor(0, 1); lcd.print(menuSelection == 1 ? F("> 2P Survival") : F(" 2P Survival"));
    } else {
      lcd.setCursor(0, 0); lcd.print(F(" 2P Survival"));
      lcd.setCursor(0, 1); lcd.print(F("> 2P Score Match"));
    }
  }

  void renderScoreLCD() {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(gameState == STATE_PLAYING_1P ? F("1P MODE SCORE:") : F("2P MODE SCORE:"));
    lcd.setCursor(0, 1);
    lcd.print(score);
  }

  void renderNameEntryLCD() {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(F("NAME: "));
    for (uint8_t i = 0; i < 10; i++) {
      lcd.print(playerName[i]);
    }
    lcd.setCursor(0, 1);
    for (uint8_t i = 0; i < 6 + nameCursor; i++) {
      lcd.print(' ');
    }
    lcd.print('^');
  }

  bool checkCollision(int8_t nx, int8_t ny, const int8_t* customShapePtr = nullptr) {
    const int8_t* s = customShapePtr ? customShapePtr : current.shapePtr;
    for (uint8_t i = 0; i < current.len; i++) {
      int8_t shapeX = (int8_t)pgm_read_byte(s + (i * 2) + 0);
      int8_t shapeY = (int8_t)pgm_read_byte(s + (i * 2) + 1);
      int8_t xx = nx + shapeX;
      int8_t yy = ny + shapeY;
      if (xx < 0 || xx >= SCREEN_W || yy >= SCREEN_H) return true;
      if (yy >= 0 && getCell(yy, xx) != 0) return true;
    }
    return false;
  }

  bool isTSpin() {
    if (current.type != 'T' || !lastRotationWasTSpin) return false;
    int8_t cx = current.x + 1;
    int8_t cy = current.y + 1;
    uint8_t cornersOccupied = 0;
    int8_t corners[4][2] = {{-1,-1}, {1,-1}, {-1,1}, {1,1}};
    for (uint8_t i = 0; i < 4; i++) {
      int8_t xx = cx + corners[i][0];
      int8_t yy = cy + corners[i][1];
      if (xx < 0 || xx >= SCREEN_W || yy >= SCREEN_H || (yy >= 0 && getCell(yy, xx) != 0)) {
        cornersOccupied++;
      }
    }
    return (cornersOccupied >= 3);
  }

  void triggerGameOver(bool won) {
    if (gameState == STATE_PLAYING_2P && !won) diedIn2PFlag = 1;

    wonGame = won ? 1 : 0;
    gameState = STATE_GAMEOVER;
    gameOverStartTime = (uint16_t)millis();
    updateGameOverLCD();
  }

  void spawnBlock() {
    int r = random(7);
    int8_t sx = SCREEN_W / 2 - 2;
    current.rotation = 0;
    lastRotationWasTSpin = 0;
    isGrounded = 0;

    switch (r) {
      case 0: current = {(const int8_t*)I_SHAPE[0], sx, 0, 4, 0, 1, 'I'}; break;
      case 1: current = {(const int8_t*)O_SHAPE[0], sx, 0, 4, 0, 2, 'O'}; break;
      case 2: current = {(const int8_t*)T_SHAPE[0], sx, 0, 4, 0, 3, 'T'}; break;
      case 3: current = {(const int8_t*)L_SHAPE[0], sx, 0, 4, 0, 4, 'L'}; break;
      case 4: current = {(const int8_t*)J_SHAPE[0], sx, 0, 4, 0, 5, 'J'}; break;
      case 5: current = {(const int8_t*)S_SHAPE[0], sx, 0, 4, 0, 6, 'S'}; break;
      case 6: current = {(const int8_t*)Z_SHAPE[0], sx, 0, 4, 0, 7, 'Z'}; break;
    }

    if (checkCollision(current.x, current.y)) {
      triggerGameOver(false);
    }
  }

  void placeBlock() {
    bool tSpin = isTSpin();

    for (uint8_t i = 0; i < current.len; i++) {
      int8_t shapeX = (int8_t)pgm_read_byte(current.shapePtr + (i * 2) + 0);
      int8_t shapeY = (int8_t)pgm_read_byte(current.shapePtr + (i * 2) + 1);
      int8_t xx = current.x + shapeX;
      int8_t yy = current.y + shapeY;
      if (yy >= 0 && yy < SCREEN_H && xx >= 0 && xx < SCREEN_W) {
        setCell(yy, xx, current.colorId);
      }
    }

    uint8_t clearedCount = 0;
    for (int y = 0; y < SCREEN_H; y++) {
      bool lineFull = true;
      for (int x = 0; x < SCREEN_W; x++) {
        if (getCell(y, x) == 0) { lineFull = false; break; }
      }
      if (lineFull) {
        clearedCount++;
        for (int j = y; j > 0; j--) {
          for (int x = 0; x < SCREEN_W; x++) setCell(j, x, getCell(j - 1, x));
        }
        for (int x = 0; x < SCREEN_W; x++) setCell(0, x, 0);
      }
    }

    if (clearedCount > 0 || tSpin) {
      uint32_t basePoints = 0;
      bool isDifficult = false;

      if (tSpin) {
        if (clearedCount == 2) { basePoints = 1200; isDifficult = true; }
        else if (clearedCount == 3) { basePoints = 1600; isDifficult = true; }
      } else {
        if (clearedCount == 1) basePoints = 100;
        else if (clearedCount == 2) basePoints = 300;
        else if (clearedCount == 3) basePoints = 500;
        else if (clearedCount == 4) { basePoints = 800; isDifficult = true; }
      }

      uint32_t earned = basePoints;
      if (isDifficult && lastClearWasDifficult) {
        earned = (earned * 3) / 2;
      }
      lastClearWasDifficult = isDifficult ? 1 : 0;

      score += earned * speedLevel;
      linesClearedForLevel += clearedCount;

      if (linesClearedForLevel >= (speedLevel * 10) && speedLevel < 10) {
        linesClearedForLevel -= (speedLevel * 10);
        speedLevel++;
      }

      if (earned > 0) sendLive();

      if (gameState == STATE_PLAYING_1P && speedLevel == 10 && linesClearedForLevel >= 100) {
        triggerGameOver(true);
        return;
      }

      renderScoreLCD();
    }

    isGrounded = 0;
    spawnBlock();
  }

  void rotateBlock() {
    uint8_t limit = (current.type == 'I' || current.type == 'S' || current.type == 'Z') ? 2
                  : (current.type == 'O' ? 1 : 4);
    uint8_t nr = (current.rotation + 1) % limit;
    const int8_t* ns = nullptr;
    if      (current.type == 'I') ns = (const int8_t*)I_SHAPE[nr];
    else if (current.type == 'O') ns = (const int8_t*)O_SHAPE[0];
    else if (current.type == 'T') ns = (const int8_t*)T_SHAPE[nr];
    else if (current.type == 'L') ns = (const int8_t*)L_SHAPE[nr];
    else if (current.type == 'J') ns = (const int8_t*)J_SHAPE[nr];
    else if (current.type == 'S') ns = (const int8_t*)S_SHAPE[nr];
    else if (current.type == 'Z') ns = (const int8_t*)Z_SHAPE[nr];

    for (uint8_t k = 0; k < 8; k++) {
      int8_t kx = (int8_t)pgm_read_byte(&(KICK_OFFSETS[k][0]));
      int8_t ky = (int8_t)pgm_read_byte(&(KICK_OFFSETS[k][1]));
      if (!checkCollision(current.x + kx, current.y + ky, ns)) {
        current.x += kx;
        current.y += ky;
        current.shapePtr = ns;
        current.rotation = nr;
        lastRotationWasTSpin = (current.type == 'T') ? 1 : 0;

        if (checkCollision(current.x, current.y + 1)) {
          lockStartTime = (uint16_t)millis();
        }
        break;
      }
    }
  }

  bool stepDown() {
    if (!checkCollision(current.x, current.y + 1)) {
      current.y++;
      isGrounded = 0;
      return true;
    } else {
      if (!isGrounded) {
        isGrounded = 1;
        lockStartTime = (uint16_t)millis();
      }
      return false;
    }
  }

  void hardDrop() {
    while ((gameState == STATE_PLAYING_1P || gameState == STATE_PLAYING_2P) && !checkCollision(current.x, current.y + 1)) {
      current.y++;
    }
    placeBlock();
  }

public:
  TetrisGame(uint8_t pNum, uint8_t lcdAddr)
    : playerNum(pNum), ledController(nullptr), lcd(lcdAddr, 16, 2) {}

  void setController(CLEDController* controller) {
    ledController = controller;
  }

  void setup(uint8_t pLeft, uint8_t pRight, uint8_t pUp, uint8_t pDown, uint8_t pDrop) {
    btnLeft.init(pLeft);
    btnRight.init(pRight);
    btnUp.init(pUp);
    btnDown.init(pDown);
    btnDrop.init(pDrop);

    lcd.init();
    lcd.backlight();
    resetToMenu();
  }

  uint8_t getMenuSelection() const { return menuSelection; }
  uint32_t getScore() const { return score; }
  bool getWonStatus() const { return wonGame != 0; }

  void setWonStatus(bool won) {
    wonGame = won ? 1 : 0;
    if (gameState == STATE_GAMEOVER) {
      updateGameOverLCD();
    }
  }

  void renderWaitingLCD() {
    lcd.clear();
    lcd.setCursor(0, 0);
    if (menuSelection == 1)      lcd.print(F(" 2P SURVIVAL "));
    else if (menuSelection == 2) lcd.print(F(" 2P SCORE MATCH"));

    lcd.setCursor(0, 1);
    if (playerNum == 1) lcd.print(F("Waiting P2... "));
    else                lcd.print(F("Waiting P1... "));
  }

  void showModeMismatchLCD() {
    lcd.setCursor(0, 1);
    lcd.print(F("MODE MISMATCH! "));
  }

  void updateGameOverLCD() {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(wonGame ? F(" YOU WIN! ") : F(" GAME OVER! "));
    lcd.setCursor(0, 1);
    lcd.print(F("SCORE: "));
    lcd.print(score);
  }

  void resetToMenu() {
    memset(field, 0, sizeof(field));
    btnLeft.clear(); btnRight.clear(); btnUp.clear(); btnDown.clear(); btnDrop.clear();

    gameState = STATE_MENU;
    menuSelection = 0;
    diedIn2PFlag = 0;
    score = 0;
    linesClearedForLevel = 0;
    speedLevel = 1;
    lastClearWasDifficult = 0;
    wonGame = 0;
    twoPlayerGame = 0;
    isGrounded = 0;
    isHoldingDrop = 0;

    playerName[0] = 'A';
    for (uint8_t i = 1; i < 10; i++) playerName[i] = '*';
    nameCursor = 0;

    renderMenuLCD();
  }

  void start1PGame() {
    randomSeed(micros() ^ millis() ^ analogRead(A6));
    for (uint8_t i = 0; i < (micros() & 0x0F); i++) random(7);

    memset(field, 0, sizeof(field));
    btnLeft.clear(); btnRight.clear(); btnUp.clear(); btnDown.clear(); btnDrop.clear();

    gameId = ++gameCounter;
    twoPlayerGame = 0;
    wonGame = 0;
    gameState = STATE_PLAYING_1P;

    lastDrop = (uint16_t)millis();
    renderScoreLCD();
    sendLive();
    spawnBlock();
  }

  void start2PGame(uint16_t id) {
    randomSeed(micros() ^ millis() ^ analogRead(A6));
    for (uint8_t i = 0; i < (micros() & 0x0F); i++) random(7);

    memset(field, 0, sizeof(field));
    btnLeft.clear(); btnRight.clear(); btnUp.clear(); btnDown.clear(); btnDrop.clear();

    gameId = id;
    twoPlayerGame = 1;
    wonGame = 0;
    gameState = STATE_PLAYING_2P;

    lastDrop = (uint16_t)millis();
    renderScoreLCD();
    sendLive();
    spawnBlock();
  }

  GameState getState() { return gameState; }
  bool isPlaying() { return (gameState == STATE_PLAYING_1P || gameState == STATE_PLAYING_2P); }

  bool justDiedIn2P() {
    if (diedIn2PFlag) {
      diedIn2PFlag = 0;
      return true;
    }
    return false;
  }

  void forceGameOver(bool won) { triggerGameOver(won); }

  void drawMatrix() {
    if (ledController == nullptr) return;

    fill_solid(leds, NUM_LEDS, CRGB::Black);

    if (gameState == STATE_MENU) {
      CRGB bootColor = (playerNum == 1) ? CRGB(0, 0, 255) : CRGB(255, 0, 0);
      for (int i = 0; i < SCREEN_W; i++) {
        leds[getPixelIndex(i, 0)] = bootColor;
        leds[getPixelIndex(i, SCREEN_H - 1)] = bootColor;
      }
    } else if (gameState == STATE_GAMEOVER) {
      uint16_t elapsed = (uint16_t)(millis() - gameOverStartTime);
      if (elapsed < 3000) {
        uint8_t pulsePhase = (elapsed / 500) % 2;
        if (pulsePhase != 0) {
          for (int y = 0; y < SCREEN_H; y++) {
            for (int x = 0; x < SCREEN_W; x++) {
              uint8_t cell = getCell(y, x);
              if (cell != 0) {
                uint32_t col = pgm_read_dword(&(COLOR_PALETTE[cell]));
                leds[getPixelIndex(x, y)] = CRGB(col);
              }
            }
          }
        }
      } else {
        uint16_t textElapsed = elapsed - 3000;

        // 2P Score Match before scores are evaluated
        if (twoPlayerGame && active2PMode == MODE_2P_SCORE && !scoreMatchEvaluated) {
          if (textElapsed < 1000) { // Single "GAME" -> "OVER" transition without cycling
            drawChar5x5(1, 2, BITMAP_G, CRGB::Red);
            drawChar5x5(1, 9, BITMAP_A, CRGB::Red);
            drawChar5x5(1, 16, BITMAP_M, CRGB::Red);
            drawChar5x5(1, 23, BITMAP_E, CRGB::Red);
          } else { // Static "OVER"
            drawChar5x5(1, 2, BITMAP_O, CRGB::Red);
            drawChar5x5(1, 9, BITMAP_V, CRGB::Red);
            drawChar5x5(1, 16, BITMAP_E, CRGB::Red);
            drawChar5x5(1, 23, BITMAP_R, CRGB::Red);
          }
        } else if (twoPlayerGame) { // Evaluated 2P Mode / 2P Survival Mode
          if (wonGame) { // WIN
            drawChar5x5(1, 5, BITMAP_W, CRGB::Green);
            drawChar5x5(1, 13, BITMAP_I, CRGB::Green);
            drawChar5x5(1, 21, BITMAP_N, CRGB::Green);
          } else { // LOSE
            drawChar5x5(1, 3, BITMAP_L, CRGB::Red);
            drawChar5x5(1, 10, BITMAP_O, CRGB::Red);
            drawChar5x5(1, 17, BITMAP_S, CRGB::Red);
            drawChar5x5(1, 24, BITMAP_E, CRGB::Red);
          }
        } else { // 1P Arcade Mode
          if (wonGame) { // WIN
            drawChar5x5(1, 5, BITMAP_W, CRGB::Green);
            drawChar5x5(1, 13, BITMAP_I, CRGB::Green);
            drawChar5x5(1, 21, BITMAP_N, CRGB::Green);
          } else { // Continuous "GAME" -> "OVER" cycling
            uint8_t page = (textElapsed / 1000) % 2;
            if (page == 0) { // "GAME"
              drawChar5x5(1, 2, BITMAP_G, CRGB::Red);
              drawChar5x5(1, 9, BITMAP_A, CRGB::Red);
              drawChar5x5(1, 16, BITMAP_M, CRGB::Red);
              drawChar5x5(1, 23, BITMAP_E, CRGB::Red);
            } else { // "OVER"
              drawChar5x5(1, 2, BITMAP_O, CRGB::Red);
              drawChar5x5(1, 9, BITMAP_V, CRGB::Red);
              drawChar5x5(1, 16, BITMAP_E, CRGB::Red);
              drawChar5x5(1, 23, BITMAP_R, CRGB::Red);
            }
          }
        }
      }
    } else {
      // 1. Draw Field Stack
      for (int y = 0; y < SCREEN_H; y++) {
        for (int x = 0; x < SCREEN_W; x++) {
          uint8_t cell = getCell(y, x);
          if (cell != 0) {
            uint32_t col = pgm_read_dword(&(COLOR_PALETTE[cell]));
            leds[getPixelIndex(x, y)] = CRGB(col);
          }
        }
      }

      if (gameState == STATE_PLAYING_1P || gameState == STATE_PLAYING_2P) {
        // 2. Draw Ghost Block Landing Projection
        if (ghost_block) {
          int8_t ghostY = current.y;
          while (!checkCollision(current.x, ghostY + 1)) {
            ghostY++;
          }
          for (uint8_t i = 0; i < current.len; i++) {
            int8_t shapeX = (int8_t)pgm_read_byte(current.shapePtr + (i * 2) + 0);
            int8_t shapeY = (int8_t)pgm_read_byte(current.shapePtr + (i * 2) + 1);
            int xx = current.x + shapeX;
            int yy = ghostY + shapeY;
            if (xx >= 0 && xx < SCREEN_W && yy >= 0 && yy < SCREEN_H) {
              if (getCell(yy, xx) == 0) { // Only draw on empty grid space
                leds[getPixelIndex(xx, yy)] = ghost_block_color;
              }
            }
          }
        }

        // 3. Draw Active Falling Piece
        for (uint8_t i = 0; i < current.len; i++) {
          int8_t shapeX = (int8_t)pgm_read_byte(current.shapePtr + (i * 2) + 0);
          int8_t shapeY = (int8_t)pgm_read_byte(current.shapePtr + (i * 2) + 1);
          int xx = current.x + shapeX;
          int yy = current.y + shapeY;
          if (xx >= 0 && xx < SCREEN_W && yy >= 0 && yy < SCREEN_H) {
            uint32_t col = pgm_read_dword(&(COLOR_PALETTE[current.colorId]));
            leds[getPixelIndex(xx, yy)] = CRGB(col);
          }
        }
      }
    }

    uint8_t powerScaledBrightness = calculate_max_brightness_for_power_mW(
      leds, NUM_LEDS, LED_BRIGHTNESS, (uint32_t)MAX_POWER_VOLTS * MAX_POWER_MA
    );

    ledController->showLeds(powerScaledBrightness);
  }

  void update() {
    btnLeft.update(); btnRight.update(); btnUp.update(); btnDown.update(); btnDrop.update();

    uint16_t now = (uint16_t)millis();

    if (btnDrop.isDown()) {
      if (!isHoldingDrop) {
        isHoldingDrop = 1;
        dropHoldStartTime = now;
      } else if ((uint16_t)(now - dropHoldStartTime) >= QUIT_HOLD_MS) {
        isHoldingDrop = 0;
        btnDrop.clear();
        if (gameState == STATE_PLAYING_1P || gameState == STATE_PLAYING_2P) {
          sendForceQuitEvent();
        }
        if (gameState == STATE_PLAYING_2P) {
          triggerGameOver(false);
        } else {
          resetToMenu();
        }
        return;
      }
    } else {
      isHoldingDrop = 0;
    }

    if (gameState == STATE_MENU) {
      uint8_t maxMenuSelection = is2PEnabled ? 2 : 0;

      // Wrap-around scrolling menu navigation
      if (btnUp.wasPressed()) {
        if (menuSelection == 0) menuSelection = maxMenuSelection;
        else menuSelection--;
        renderMenuLCD();
      }
      if (btnDown.wasPressed()) {
        if (menuSelection >= maxMenuSelection) menuSelection = 0;
        else menuSelection++;
        renderMenuLCD();
      }
      if (btnDrop.wasPressed()) {
        if (menuSelection == 0) {
          start1PGame();
        } else if (is2PEnabled) {
          gameState = STATE_WAITING_2P;
          renderWaitingLCD();
        }
      }
      return;
    }

    if (gameState == STATE_WAITING_2P) {
      if (btnDrop.wasPressed()) resetToMenu();
      return;
    }

    if (gameState == STATE_GAMEOVER) {
      if ((uint16_t)(now - gameOverStartTime) < 3000) {
        return;
      }
      if (btnDrop.wasPressed() || btnUp.wasPressed()) {
        gameState = STATE_NAME_ENTRY;
        playerName[0] = 'A';
        for (uint8_t i = 1; i < 10; i++) playerName[i] = '*';
        nameCursor = 0;
        renderNameEntryLCD();
      }
      return;
    }

    if (gameState == STATE_NAME_ENTRY) {
      if (btnLeft.wasPressed() && nameCursor > 0) { nameCursor--; renderNameEntryLCD(); }
      if (btnRight.wasPressed() && nameCursor < 9) { nameCursor++; renderNameEntryLCD(); }

      if (btnDown.wasPressed()) {
        if (playerName[nameCursor] == 'Z') playerName[nameCursor] = ' ';
        else if (playerName[nameCursor] == ' ') playerName[nameCursor] = '*';
        else if (playerName[nameCursor] == '*') playerName[nameCursor] = 'A';
        else playerName[nameCursor]++;
        renderNameEntryLCD();
      }

      if (btnUp.wasPressed()) {
        if (playerName[nameCursor] == 'A') playerName[nameCursor] = '*';
        else if (playerName[nameCursor] == '*') playerName[nameCursor] = ' ';
        else if (playerName[nameCursor] == ' ') playerName[nameCursor] = 'Z';
        else playerName[nameCursor]--;
        renderNameEntryLCD();
      }

      if (btnDrop.wasPressed()) {
        gameState = STATE_SUBMIT_PING;
      }
      return;
    }

    if (gameState == STATE_SUBMIT_PING) {
      lcd.clear();
      lcd.setCursor(0, 0); lcd.print(F("CONNECTING... "));
      lcd.setCursor(0, 1); lcd.print(F("CHECKING HOST "));

      pongSeen = pongCount;
      Serial.println(F("PING"));
      submitTimer = now;
      gameState = STATE_SUBMIT_WAIT;
      return;
    }

    if (gameState == STATE_SUBMIT_WAIT) {
      if (pongCount != pongSeen) {
        uint8_t cleanLen = 10;
        while (cleanLen > 0 && playerName[cleanLen - 1] == '*') {
          cleanLen--;
        }

        Serial.print(F("{\"event\":\"SCORE_SUBMIT\",\"player\":"));
        Serial.print(playerNum);
        Serial.print(F(",\"game\":"));
        Serial.print(gameId);
        Serial.print(F(",\"mode\":\""));
        if (twoPlayerGame) Serial.print(F("2P")); else Serial.print(F("1P"));
        Serial.print(F("\",\"name\":\""));
        for (uint8_t i = 0; i < cleanLen; i++) Serial.print(playerName[i]);
        Serial.print(F("\",\"score\":"));
        Serial.print(score);
        Serial.print(F(",\"level\":"));
        Serial.print(speedLevel);
        Serial.print(F(",\"won\":"));
        Serial.print(wonGame ? 1 : 0);
        Serial.println(F("}"));

        lcd.clear();
        lcd.setCursor(0, 0); lcd.print(F(" ONLINE SUBMIT "));
        lcd.setCursor(0, 1); lcd.print(F(" SCORE SAVED! "));
        submitTimer = now;
        gameState = STATE_SUBMIT_RESULT;
        return;
      }

      if ((uint16_t)(now - submitTimer) >= 5000) {
        lcd.clear();
        lcd.setCursor(0, 0); lcd.print(F(" OFFLINE MODE "));
        lcd.setCursor(0, 1); lcd.print(F(" NO HOST FOUND "));
        submitTimer = now;
        gameState = STATE_SUBMIT_RESULT;
      }
      return;
    }

    if (gameState == STATE_SUBMIT_RESULT) {
      if ((uint16_t)(now - submitTimer) >= 1000) {
        resetToMenu();
      }
      return;
    }

    if (gameState == STATE_PLAYING_1P || gameState == STATE_PLAYING_2P) {
      if (checkCollision(current.x, current.y + 1)) {
        if (!isGrounded) {
          isGrounded = 1;
          lockStartTime = now;
        } else if ((uint16_t)(now - lockStartTime) >= LOCK_DELAY_MS) {
          placeBlock();
          return;
        }
      } else {
        isGrounded = 0;
      }

      // Movement Input Processing (Single tap only - hold does not move continuously)
      if (btnLeft.wasPressed()) {
        if (!checkCollision(current.x - 1, current.y)) {
          current.x--;
          if (isGrounded) lockStartTime = now;
        }
      } else if (btnRight.wasPressed()) {
        if (!checkCollision(current.x + 1, current.y)) {
          current.x++;
          if (isGrounded) lockStartTime = now;
        }
      }

      if (btnUp.wasPressed()) rotateBlock();
      if (btnDrop.wasPressed()) hardDrop();

      uint16_t currentDropInterval = btnDown.isDown() ? 40 : max(50, 500 - (speedLevel - 1) * 45);
      if ((uint16_t)(now - lastDrop) > currentDropInterval) {
        lastDrop = now;
        stepDown();
      }
    }
  }
};

// Player 1: LCD 0x27
TetrisGame player1(1, 0x27);

// Player 2: LCD 0x26
TetrisGame player2(2, 0x26);

Mode2P active2PMode = MODE_2P_SURVIVAL;
bool scoreMatchEvaluated = false;

uint16_t lastFrameTime = 0;
const uint8_t FRAME_INTERVAL = 33;

bool scanI2CDevice(uint8_t address) {
  Wire.beginTransmission(address);
  return (Wire.endTransmission() == 0);
}

void setup() {
  Wire.begin();
  Wire.setWireTimeout(3000, true);

  Serial.begin(115200);
  pinMode(BUZZER_PIN, OUTPUT);

  uint32_t seed = 0;
  for (uint8_t i = 0; i < 16; i++) {
    seed = (seed << 2) ^ analogRead(A6) ^ (analogRead(A7) << 1);
    delayMicroseconds(150);
  }
  randomSeed(seed ^ micros());

  p1Controller = &FastLED.addLeds<WS2812B, P1_LED_PIN, GRB>(leds, NUM_LEDS);
  p2Controller = &FastLED.addLeds<WS2812B, P2_LED_PIN, GRB>(leds, NUM_LEDS);

  FastLED.setMaxPowerInVoltsAndMilliamps(MAX_POWER_VOLTS, MAX_POWER_MA);
  FastLED.setBrightness(LED_BRIGHTNESS);
  FastLED.clear(true);

  if (!scanI2CDevice(0x26)) {
    is2PEnabled = false;
    Serial.println(F("[SYSTEM] P2 display (0x26) NOT detected. 2P mode disabled until next reboot."));
  } else {
    is2PEnabled = true;
    Serial.println(F("[SYSTEM] P2 display (0x26) detected. 2P mode ready."));
  }

  // Player 1: Left(5), Right(2), Up(6), Down(7), Drop(8)
  player1.setController(p1Controller);
  player1.setup(5, 2, 6, 7, 8);

  // Player 2: Left(A2), Right(A3), Up(9), Down(A0), Drop(A1)
  if (is2PEnabled) {
    player2.setController(p2Controller);
    player2.setup(A2, A3, 9, A0, A1);
  }
}

void loop() {
  pollHostSerial();

  if (player1.getState() == STATE_MENU) {
    random(7);
  }

  player1.update();

  if (is2PEnabled) {
    player2.update();

    static bool modeMismatchNotified = false;

    if (player1.getState() == STATE_WAITING_2P && player2.getState() == STATE_WAITING_2P) {
      if (player1.getMenuSelection() == player2.getMenuSelection()) {
        uint16_t id = ++gameCounter;
        active2PMode = (player1.getMenuSelection() == 2) ? MODE_2P_SCORE : MODE_2P_SURVIVAL;
        scoreMatchEvaluated = false;

        player1.start2PGame(id);
        player2.start2PGame(id);
        modeMismatchNotified = false;
      } else {
        if (!modeMismatchNotified) {
          player1.showModeMismatchLCD();
          player2.showModeMismatchLCD();
          modeMismatchNotified = true;
        }
      }
    } else {
      modeMismatchNotified = false;
    }

    if (active2PMode == MODE_2P_SURVIVAL) {
      if (player1.getState() == STATE_PLAYING_2P && player2.justDiedIn2P()) {
        player1.forceGameOver(true);
      }
      if (player2.getState() == STATE_PLAYING_2P && player1.justDiedIn2P()) {
        player2.forceGameOver(true);
      }
    }

    if (active2PMode == MODE_2P_SCORE) {
      bool p1Finished = (player1.getState() != STATE_PLAYING_2P && player1.getState() != STATE_WAITING_2P);
      bool p2Finished = (player2.getState() != STATE_PLAYING_2P && player2.getState() != STATE_WAITING_2P);

      if (p1Finished && p2Finished && !scoreMatchEvaluated) {
        scoreMatchEvaluated = true;
        if (player1.getScore() > player2.getScore()) {
          player1.setWonStatus(true);
          player2.setWonStatus(false);
        } else if (player2.getScore() > player1.getScore()) {
          player1.setWonStatus(false);
          player2.setWonStatus(true);
        } else {
          player1.setWonStatus(true);
          player2.setWonStatus(true);
        }
      }
    }
  }

  uint16_t now = (uint16_t)millis();
  if ((uint16_t)(now - lastFrameTime) >= FRAME_INTERVAL) {
    lastFrameTime = now;
    player1.drawMatrix();
    if (is2PEnabled) {
      player2.drawMatrix();
    }
  }

  bool activeGameplay = player1.isPlaying() || (is2PEnabled && player2.isPlaying());
  updateBGM(activeGameplay);
}
