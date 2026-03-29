#include <Arduino.h>         
#include <SPI.h>             // thats for SdFat
#include <Wire.h>            // for I2C. just in case.
#include "SdFat.h"           // sd card stuff. i hope ya have one spare
#include <GyverOLED.h>       // Alex Gyver OLED library. lightweigh, reliable, fast enough

//      YM2608 pin setup
#define YM_A0   PB4  // YM2608 A0, for selecting register banks (port 0 or port 1)
#define YM_A1   PB5  // YM2608 A1, 0 - writing register data, 1 - write data
#define YM_CS   PB8  // YM2608 chip select pin, ACTIVE LOW
#define YM_WR   PB9  // YM2608 strobe pulse for writing data. ACTIVE LOW
#define YM_IC   PB10 // YM2608 initial clear, used as hardware reset. ACTIVE LOW

#define YM_RD   PB11 // YM2608 read *from* synth. basically, unused here, because i pulled it to VCC. ACTIVE LOW
// note: data bus is on PA0-PA7. these pins are in the same port, makes it easier to set them all with minimal latency. hardcoded a little below
// note: PA8 is a 8 MHz clock for YM2608. (mco)

//      software SPI setup (matches SPI2)
#define SD_CS_PIN   PB12 // sd chip select pin
#define SD_SCK_PIN  PB13 // clock 
#define SD_MISO_PIN PB14 // master in slave out
#define SD_MOSI_PIN PB15 // master out slave in
//                            !!!!!  ATTENTION  !!!!!
//                            DO THINGS BELOW! I SWEAR TO GOD
// \Documents\Arduino\libraries\SdFat\src\SdFatConfig.h:
// line 96 -  #define ENABLE_ARDUINO_SERIAL 0            will free some memory, allowing to disable Serial
// line 169 - #define SPI_DRIVER_SELECT 2                will enable software SPI. i have no idea why SPI2 hangs the chip.

// button setup
#define BTN_LEFT  PA9  // backwards
#define BTN_OK    PA10 // OK/back to menu button
#define BTN_RIGHT PA11 // forward

//      some globals and classes
// software spi class
SoftSpiDriver<SD_MISO_PIN, SD_MOSI_PIN, SD_SCK_PIN> mySoftSpi;
// setting up SdFat to use our software spi on 4 mhz clock
#define SD_CONFIG SdSpiConfig(SD_CS_PIN, DEDICATED_SPI, SD_SCK_MHZ(4), &mySoftSpi)

SdFat SD;       // main object for working with fat32
FsFile vgmFile; // file object with our track data

// OLED init
// using no buffer mode, saves 1K of RAM and reduces chances of lags
GyverOLED<SSD1306_128x64, OLED_NO_BUFFER> oled;

//      global variables
enum State { STATE_BROWSER, STATE_PLAY }; // two states - menu (list of tracks) and player
State currentState = STATE_BROWSER;       // main menu during bootup

bool isS98 = false;               // if true - S98 file is playing, if false - VGM is playing
uint32_t data_offset = 0x40;      // data offset where music data begins
uint32_t loop_offset = 0;         // offset where to jump when track ends
uint32_t s98_tick_us = 10000;     // single s98 tick length in microseconds

uint32_t current_samples = 0;     // sample counter to calculate track length in seconds (funnily enough, 44100 = 1s. nothing to do with *actual* discretization frequency of YM2608 (55.5K))
uint32_t total_samples = 0;       // VGM file length in samples
uint32_t current_us = 0;          // us counter for calculating the length of S98 file
uint32_t lastDrawTime = 0;        // last screen update timer

uint32_t track_sync_timer = 0;    // absolute timer, used to send data in YM2608 as precisely as i can

bool isPaused = false;            // pause flag. fun fact! it hangs notes, because YM2608 does not recieve a "note off" command
bool action_next = false;         // next track check
bool action_prev = false;         // prev track check
bool action_menu = false;         // middle button check

uint16_t totalFiles = 0;          // supported file counter in root dir
uint16_t fileIndex = 0;           // cursor index
uint16_t displayTopIdx = 0;       // for scrolling - shows the first file *displayed on screen*

char currentFilename[32] = "";       // track filename buffer
char metaTitle[21] = "Unknown Title"; // metadata track name buffer
char metaComposer[21] = "Unknown";    // metadata composer name buffer

//      some hardware stuff for YM2608

// setting up a clock signal for YM2608
void setupMCO() {
  // making a PA8 as push-pull, 50mhz
  GPIOA->CRH = (GPIOA->CRH & 0xFFFFFFF0) | 0x0000000B; 
  // enabling MCO, HSI, using external clock source (this shiny 8mhz cristal, in case of STM32F103. no, not the 32768hz one, thats realtime clock you illy goober)
  RCC->CFGR = (RCC->CFGR & ~(0x07 << 24)) | (0x05 << 24); 
}

//      pain kicks in
// sending data in YM2608 while trying to keep latencies from datasheet
void writeYM(uint8_t port, uint8_t reg, uint8_t data) {
  // stage 1 - choosing registers
  digitalWrite(YM_A1, port); // choosing port: 1 - FM1-FM3 + SSG, 0 - FM4-FM6 + ADPCM + RSS. YM2608 is YM2203(OPN) compatible, remember that?
  digitalWrite(YM_A0, LOW);  // "im gonna send you the register number address rn"
  
  // grabbing our port A, zeroing lower 8 bits, and writing register address
  GPIOA->ODR = (GPIOA->ODR & 0xFFFFFF00) | reg;
  delayMicroseconds(2); // setup time for volatges to settle

  digitalWrite(YM_CS, LOW);  // enbling the chip, making CS 0
  digitalWrite(YM_WR, LOW);  // making it WRIGHT data
  delayMicroseconds(2);      // giving some time to settle again
  digitalWrite(YM_WR, HIGH); // disbling data writing
  digitalWrite(YM_CS, HIGH); // disabling chip
  delayMicroseconds(4);      // approx. 17 clock cycles to apply the address

  // stage 2 - writing data in that register
  digitalWrite(YM_A0, HIGH); // "im sending the DATA to that address"
  GPIOA->ODR = (GPIOA->ODR & 0xFFFFFF00) | data; // writing port A data to send to YM2608
  delayMicroseconds(2); // settle time

  digitalWrite(YM_CS, LOW);  // enabling the chip
  digitalWrite(YM_WR, LOW);  // WRITING DATA
  delayMicroseconds(2);      // girls are preparing... please wait warmly
  digitalWrite(YM_WR, HIGH); // lowering the WRITE signal
  digitalWrite(YM_CS, HIGH); // releasing the chip in the wild
  delayMicroseconds(12);     // letting it proccess the audio data for approx. 83 clock cycles, as instructed in the datasheet
}

//      hardware reset
void resetYM() {
  digitalWrite(YM_IC, LOW);  // setting initial clear as 0, enabling it
  delay(20);                 // holding for a bit
  digitalWrite(YM_IC, HIGH); // and getting it back to 1, disabling
  delay(20);                 // giving it ome time to start
}

//      some button stuff
bool btn_l_last = HIGH, btn_ok_last = HIGH, btn_r_last = HIGH; // savestates for buttons, pulled up to HIGH interanally

//      checking the buttons, debounce included! (it sucks, millis() = UEEUGHH)
void checkButtons() {
  bool l = digitalRead(BTN_LEFT);  // reading LEFT button
  bool ok = digitalRead(BTN_OK);   // reading MIDDLE (aka OK/MENU, however ya call it m8) button
  bool r = digitalRead(BTN_RIGHT); // reading RIGHT button

  // if left btn got LOW and was HIGH                                                                                                             high does not mean "stoned" you doofus
  if (l == LOW && btn_l_last == HIGH) { action_prev = true; delay(50); } // falgging it and debouncing
  
  // middle button short press
  if (ok == LOW && btn_ok_last == HIGH) { 
    if (currentState == STATE_BROWSER) action_menu = true; // if in menu - plays selected file
    else isPaused = !isPaused;                             // if in player - play/pause
    delay(50); 
  }
  
  // long middle button press
  if (ok == LOW && currentState == STATE_PLAY) {
    uint32_t holdTime = millis(); // hold timer start
    while(digitalRead(BTN_OK) == LOW) { // while it pressed:
      if (millis() - holdTime > 700) {  // and .7s passed
        action_menu = true;             // going back to menu
        break;                          // exiting the cycle!
      }
    }
  }

  // right button press
  if (r == LOW && btn_r_last == HIGH) { action_next = true; delay(50); }

  // saving states for next function call
  btn_l_last = l; btn_ok_last = ok; btn_r_last = r;
}

//      pain 2: electric boogaloo
//      synchronization
// this changes delay() function, except it does not block anything
void syncWaitUs(uint32_t us) {
  // infinite cycle, while system micros() timer and our target timer are less than our required delay
  while ((micros() - track_sync_timer) < us) {
    checkButtons(); // as we are waiting, we need to check how buttons are doing
    // if a button is pressed, breaking the pause
    if (action_next || action_prev || action_menu) return;
  }
  // adding that pause to our absolute timer
  track_sync_timer += us;
}

//      file and meta reading procedures

// reads two bytes to shove them in a single 32bit number
uint32_t read32() {
  uint32_t val = 0;
  vgmFile.read(&val, 4);
  return val;
}

// reading GD3 tags. these are used in VGMs
void readGD3String(char* buf, int maxLen) {
  int i = 0;
  while (true) {
    // GD3s are saven in UTF-16
    uint16_t c = vgmFile.read() | (vgmFile.read() << 8);
    if (c == 0x0000) { buf[i] = '\0'; break; } // two 00 bytes - end of line
    // reading only ASCII symbols
    if (i < maxLen - 1) { buf[i] = (char)(c & 0xFF); i++; }
  }
}

// S98 tag reader
void parseS98Tags(uint32_t tag_offset) {
  // clearing buffers
  strcpy(metaTitle, "Unknown");
  strcpy(metaComposer, "Unknown");
  if (tag_offset == 0) return; // if offset is 0, there's no tags, exiting
  
  vgmFile.seek(tag_offset); // jumping straight to tag block
  char buf[5];
  vgmFile.read(buf, 5); // reading our 5 byte marker closer to the end, [S98]
  if (strncmp(buf, "[S98]", 5) != 0) return; // if its not "[S98]", tags might be corrupted, exiting

  char line[64]; // one line buffer, 64 symbols
  while (vgmFile.available()) {
    int i = 0;
    while (vgmFile.available() && i < 63) { // reading 63 of them
      char c = vgmFile.read(); // taking 1 symbol
      if (c == '\n' || c == '\r') { // if we got line feed
        if (i == 0) continue; // skiping empty symbols
        else break;           // if the string isnt empty, exiting
      }
      line[i++] = c; // saving a symbol to a buffer
    }
    line[i] = '\0'; // closing a string with a zero byte. its not me, its C
    if (i == 0) break; // if we didnt read anything, then its end of file

    // if our string starts with "title="
    if (strncmp(line, "title=", 6) == 0) {
      strncpy(metaTitle, line + 6, 20); metaTitle[20] = '\0'; // copying everything in title buffer
    } 
    // same for composer
    else if (strncmp(line, "artist=", 7) == 0 || strncmp(line, "composer=", 9) == 0) {
      int offset = (line[0] == 'a') ? 7 : 9; // calculating the offset
      strncpy(metaComposer, line + offset, 20); metaComposer[20] = '\0'; // and copying in metaComposer
    }
  }
}

// counting the musical tracks here. once per boot
void scanFilesCount() {
  FsFile dir; dir.open("/"); // looking up what do we have in root
  FsFile file;
  totalFiles = 0; // zeroing the counter
  while (file.openNext(&dir, O_READ)) { // looking for files
    if (!file.isHidden() && !file.isDir()) { // ignoring folders and hidden stuff
      char name[32]; file.getName(name, sizeof(name)); // reading file name...
      // ...and exension. if .vgm or .s98, adding 1 to a counter
      if (strstr(name, ".vgm") || strstr(name, ".VGM") || strstr(name, ".s98") || strstr(name, ".S98")) totalFiles++;
    }
    file.close(); // closing file, picking up next one
  }
  dir.close(); // closing the folder
}
//      draw calls for OLED

// drawing main menu with our list of files
void drawBrowser() {
  oled.clear(); // screen clear
  oled.home();  // carriage goes in top left
  oled.print(F("== SELECT TRACK ==")); // header
  
  FsFile dir; dir.open("/"); // reading root
  FsFile file;
  uint16_t count = 0;

  // stage one - scrolling and looking for the first file thats being drawn
  while (file.openNext(&dir, O_READ)) {
    if (!file.isHidden() && !file.isDir()) {
      char name[32]; file.getName(name, sizeof(name));
      if (strstr(name, ".vgm") || strstr(name, ".VGM") || strstr(name, ".s98") || strstr(name, ".S98")) {
        if (count == displayTopIdx) break; // found it, closing the cycle (the file is STILL BEING OPEN)
        count++;
      }
    }
    file.close(); // closing the file, looking for the next one
  }

  // stage 2 - drawing 6 rows of files
  for (int i = 0; i < 6; i++) {
    if (!file.isOpen()) break; // if no more files - exiting
    char name[32]; file.getName(name, sizeof(name)); // grabbing file name
    
    oled.setCursor(0, i + 1); // carriage goes to rows 1 and 6
    
    // drawing a simple cursor next to selected file
    if (displayTopIdx + i == fileIndex) oled.print(F(">"));
    else oled.print(F(" ")); // or else drawing a space
    
    oled.print(name); // printing a file name
    file.close(); // closing the file

    // small cycle to see a next file for the list
    while (file.openNext(&dir, O_READ)) {
      if (!file.isHidden() && !file.isDir()) {
        char nextName[32]; file.getName(nextName, sizeof(nextName));
        if (strstr(nextName, ".vgm") || strstr(nextName, ".VGM") || strstr(nextName, ".s98") || strstr(nextName, ".S98")) break; 
      }
      file.close();
    }
  }
  dir.close(); // closing directory
}

// drawing time! every second
void drawPlayer() {
  if (millis() - lastDrawTime < 1000) return; // if 1s didnt pass yet - exiting
  lastDrawTime = millis(); // refreshing draw call timer

  oled.setCursor(0, 6); // moving our carriage to a row 6

  if (!isS98) { // Если играем VGM
    uint32_t curSec = current_samples / 44100; // counting seconds that passed
    uint32_t totSec = total_samples / 44100;   // calculating track length
    
    // drawing time (mm:ss)
    oled.print(curSec / 60); oled.print(F(":")); 
    if ((curSec % 60) < 10) oled.print(F("0")); // sneakily adding a 0, if <10 seconds were played
    oled.print(curSec % 60);
    
    oled.print(F(" / "));
    
    // track length
    oled.print(totSec / 60); oled.print(F(":"));
    if ((totSec % 60) < 10) oled.print(F("0")); 
    oled.print(totSec % 60);
  } else { // if S98 is being played...
    uint32_t curSec = current_us / 1000000; // calculating from microseconds
    
    // i already forgot wht was i doing here, i think .s98 doesnt have track length stored in it. but at least we can count current time. im sowwy
    oled.print(curSec / 60); oled.print(F(":")); 
    if ((curSec % 60) < 10) oled.print(F("0")); 
    oled.print(curSec % 60);
    oled.print(F(" / --:--"));
  }
  
  oled.print(F("       ")); // drawing spaces to erase some stuff that could appear
}

// ==========================================
// ФУНКЦИЯ ЗАПУСКА ТРЕКА
// ==========================================

void playSelectedFile() {
  vgmFile.close(); // closing previous file, just in case
  
  // looking for selected file
  FsFile dir; dir.open("/"); FsFile file; uint16_t c=0;
  while(file.openNext(&dir, O_READ)){
    if(!file.isHidden() && !file.isDir()){
      char name[32]; file.getName(name, sizeof(name));
      if(strstr(name, ".vgm")||strstr(name, ".VGM")||strstr(name, ".s98")||strstr(name, ".S98")){
        if(c==fileIndex){ strcpy(currentFilename, name); file.close(); break; } // Нашли! Сохраняем имя
        c++;
      }
    }
    file.close();
  }
  dir.close();

  vgmFile.open(currentFilename, O_READ); // opening the file

  // resetting tags and timers
  strcpy(metaTitle, "Unknown");
  strcpy(metaComposer, "Unknown");
  current_samples = 0;
  total_samples = 0;
  current_us = 0;

  // header parser
  uint8_t magic[3]; vgmFile.read(magic, 3); // reading first three bytes
  
  if (magic[0] == 'S' && magic[1] == '9' && magic[2] == '8') {
    // if we have .s98
    isS98 = true;
    vgmFile.seek(0x04); // 0x04 stores timer settings
    uint32_t num = read32(); uint32_t den = read32(); // reading numerator and denominator
    // turning 1 tick into microseconds
    s98_tick_us = ((num == 0 ? 10 : num) * 1000000ULL) / (den == 0 ? 1000 : den);
    
    // s98 v3 offsets
    vgmFile.seek(0x10); 
    uint32_t tag_offset = read32(); // 0x10 - tags offset
    data_offset = read32();         // 0x14 - note offset
    loop_offset = read32();         // 0x18 - loop offset
    
    parseS98Tags(tag_offset); // tag reding action

  } else {
    // vgm logic
    isS98 = false;
    vgmFile.seek(0x18); total_samples = read32(); // 0x18 - track length
    vgmFile.seek(0x1C); uint32_t rel_loop = read32(); loop_offset = (rel_loop == 0) ? 0 : 0x1C + rel_loop; // finding the loop
    vgmFile.seek(0x34); uint32_t rel_offset = read32(); data_offset = (rel_offset == 0) ? 0x40 : 0x34 + rel_offset; // finding notes
    
    // reading GD3 data
    vgmFile.seek(0x14); // 0x14 - GD3 tag offset
    uint32_t gd3_off = read32();
    if (gd3_off != 0) { // if it exists...
      vgmFile.seek(gd3_off + 0x14); // jumping there
      uint8_t tag[4]; vgmFile.read(tag, 4); // reading "Gd3 "
      if (tag[0] == 'G' && tag[1] == 'd' && tag[2] == '3') {
        vgmFile.seek(vgmFile.position() + 8); // skipping tag version
        readGD3String(metaTitle, 20);      // reading Track Name (English)
        readGD3String(currentFilename, 1); // skipping Track Name (Japanese)
        readGD3String(currentFilename, 1); // skipping Game Name (English)
        readGD3String(currentFilename, 1); // skipping Game Name (Japanese)
        readGD3String(currentFilename, 1); // skipping System Name (English)
        readGD3String(currentFilename, 1); // skipping System Name (Japanese)
        readGD3String(metaComposer, 20);   // reading Composer (English)
      }
    }
  }

  // BOOT SEQUENCE! yippee!
  resetYM(); // resetting the YM2608
  writeYM(0, 0x29, 0x80); // enabling OPNA mode
  vgmFile.seek(data_offset); // setting our "readind head" to where the notes start
  
  currentState = STATE_PLAY; // switching to player mode

  // UI draw call
  oled.clear();
  oled.home(); oled.print(F("=== PLAYING ==="));
  oled.setCursor(0, 1); oled.print(currentFilename);
  oled.setCursor(0, 3); oled.print(F("Tr: ")); oled.print(metaTitle);
  oled.setCursor(0, 4); oled.print(F("By: ")); oled.print(metaComposer);

  // sync reset, goes HERE, or else it screws up the tempo. like, really bad
  track_sync_timer = micros(); 
}

//      setup. duh

void setup() {
  // settig up our buttons and pulling em up
  pinMode(BTN_LEFT, INPUT_PULLUP);
  pinMode(BTN_OK, INPUT_PULLUP);
  pinMode(BTN_RIGHT, INPUT_PULLUP);

  // setting up data bus pins
  for (int i = 0; i <= 7; i++) pinMode(PA0 + i, OUTPUT);
  
  // ... output pins...
  pinMode(YM_A0, OUTPUT); pinMode(YM_A1, OUTPUT);
  pinMode(YM_CS, OUTPUT); pinMode(YM_WR, OUTPUT);
  pinMode(YM_IC, OUTPUT); pinMode(YM_RD, OUTPUT);
  
  // writing initial logic levels
  digitalWrite(YM_RD, HIGH); digitalWrite(YM_CS, HIGH);
  digitalWrite(YM_WR, HIGH); digitalWrite(YM_A0, HIGH); digitalWrite(YM_A1, LOW);

  // OLED init
  Wire.begin();
  Wire.setClock(1000000); // overclocking the bus to a whole megaherz, lmao (it reduces lags too!)
  oled.init();           // library init
  oled.clear();          // clearing screen
  oled.setScale(1);      // using small font
  oled.home();
  oled.print(F("Minimal OPNA player")); // "boot logo" type thingy 
  oled.setCursor(0, 2);
  oled.print(F("Booting..."));

  setupMCO(); // startion our 8MHz master clock
  resetYM();  // and resetting the synth

  // mounting SD card
  if (!SD.begin(SD_CONFIG)) { 
    oled.setCursor(0, 4); oled.print(F("SD Mount Fail!")); 
    while(1); // if we dont have it or failed, staying here until reset
  }
  
  scanFilesCount(); // counting siles...
  if (totalFiles == 0) { 
    oled.setCursor(0, 4); oled.print(F("No files found!")); 
    while(1); // if we found whopping 0 files, staying here until reset
  }

  drawBrowser(); // drawing menu!
}

//      main loop. duh

void loop() {
  checkButtons(); // checking how our buttons are doing

//      branch 1 - main meny, aka file list
  if (currentState == STATE_BROWSER) {
    if (action_prev) { // on LEFT press...
      if (fileIndex > 0) fileIndex--; // moving cursor up
      if (fileIndex < displayTopIdx) displayTopIdx = fileIndex; // scrolling up, if needed
      drawBrowser(); // redrawing
    }
    if (action_next) { // on RIGHT press...
      if (fileIndex < totalFiles - 1) fileIndex++; // moving cursor down
      if (fileIndex >= displayTopIdx + 6) displayTopIdx = fileIndex - 5; // scrolling down if needed
      drawBrowser(); // redrawing
    }
    if (action_menu) playSelectedFile(); // on OK press - playing selected file
    
    // resetting button flags
    action_next = action_prev = action_menu = false;
    return; // going back to beginning of loop()
  }

//          branch 2 - player loop
  if (currentState == STATE_PLAY) {
    
    // if you are *holding* MIDDLE button...
    if (action_menu) { 
      resetYM(); // muting
      currentState = STATE_BROWSER; // returning to menu
      drawBrowser(); // and drawing it
      action_next = action_prev = action_menu = false; 
      return;
    }
    
    // if you press RIGHT while playing, play next file
    if (action_next) { fileIndex = (fileIndex + 1) % totalFiles; playSelectedFile(); action_next = false; return; }
    // if you press LEFT while playing, play previous file
    if (action_prev) { fileIndex = (fileIndex > 0) ? fileIndex - 1 : totalFiles - 1; playSelectedFile(); action_prev = false; return; }

    // if we are paused
    if (isPaused) {
      drawPlayer(); // updating the time
      delay(100); //i wanted to make it bling but later, i have other priorities
      // and syncing time so it wouldnt RUSH forwards as if it missed a train
      track_sync_timer = micros(); 
      return;
    }
    drawPlayer(); // drawcall

    // reading commands from file!
    int c = vgmFile.read();
    if (c < 0) { // if file is finished...
      if (loop_offset != 0) vgmFile.seek(loop_offset); // looking for a loop point.
      else { resetYM(); writeYM(0, 0x29, 0x80); vgmFile.seek(data_offset); current_samples = 0; current_us = 0; } // if theres none - jumping to beginning of file
      track_sync_timer = micros(); // resetting timer
      return; 
    }
    
    uint8_t cmd = c; // command to execute by YM2608 

//          s98 parser
    if (isS98) { 
      switch(cmd) {
        case 0x00: writeYM(0, vgmFile.read(), vgmFile.read()); break; // 0x00 = writing in port 0. aka OPN compatibility, FM1-FM3, SSG, and timers
        case 0x01: writeYM(1, vgmFile.read(), vgmFile.read()); break; // 0x01 = writing in port 1, aka FM4-FM6, ADPCM and RSS
        
        case 0xFF: // 1 tick pause
          current_us += s98_tick_us; // incrementing the counter
          syncWaitUs(s98_tick_us);   // waiting for one tick
          break;
          
        case 0xFE: // looooong pause with N ticks, pretty tricky
          {
            uint32_t n = 0; uint8_t shift = 0, b;
            do { b = vgmFile.read(); n |= (b & 0x7F) << shift; shift += 7; } while (b & 0x80); // unpacking pause length
            uint32_t delay_time = (n + 2) * s98_tick_us; // formula from S98 specs
            current_us += delay_time; 
            syncWaitUs(delay_time); // waiting for calculated amount of time
          } break;
          
        case 0xFD: // marker of end of track / loop
          if (loop_offset != 0) vgmFile.seek(loop_offset);
          else { resetYM(); writeYM(0, 0x29, 0x80); vgmFile.seek(data_offset); current_us = 0;}
          track_sync_timer = micros(); // preventing loss of tempo
          break;
          
        default: 
          if (cmd < 0x7F) { vgmFile.read(); vgmFile.read(); } // skipping any other chip aside YM2203 and YM2608
          break;
      }
    } 

//          VGm parser

    else { 
      switch (cmd) {
        // writing in port 0(0x51=OPN, 0x54/0x56=OPNA)
        case 0x51: case 0x54: case 0x56: writeYM(0, vgmFile.read(), vgmFile.read()); break;
        // writing in port 1 (0x55/0x57=OPNA)
        case 0x55: case 0x57: writeYM(1, vgmFile.read(), vgmFile.read()); break;
        
        // 0x61 - waiting for some amount of samples
        case 0x61: { 
            uint16_t s; vgmFile.read(&s, 2); // reading 2 bytes of lengh
            current_samples += s;            // adding to a timer
            syncWaitUs(( (uint32_t)s * 10000 ) / 441);              // 1 saple @ 44.1кГц ≈ 22.6 us (evading any floating point operations to preserve precision, syncWaitUs will compesate. syncWaitUs <3)
          } break;
          
        // 0x62: waiting EXACTLY 1/60th of a second (NTSC)
        case 0x62: current_samples += 735; syncWaitUs(16667); break;
        
        // 0x63: waiting EXACLTY 1/50th of a second (PAL)
        case 0x63: current_samples += 882; syncWaitUs(20000); break;
        
        // 0x66: end of file / loop
        case 0x66: 
          if (loop_offset != 0) vgmFile.seek(loop_offset);
          else { resetYM(); writeYM(0, 0x29, 0x80); vgmFile.seek(data_offset); current_samples = 0; }
          track_sync_timer = micros(); // loss of tempo prevention hotline
          break;
          
        // 0x67 - ADPCM samples
        case 0x67: { 
            vgmFile.read(); // skipping 0x66 marker
            vgmFile.read(); // skipping memory type
            uint32_t sz = read32(); // reading sample block file
            vgmFile.seek(vgmFile.position() + sz); // skipping all of that, I DONT HAVE A DRAM CHIP UUUUWAAAAAAAHHHHHHHHHHH
          } break;
          
        // skipping unknown commands
        default:
          if (cmd >= 0x70 && cmd <= 0x7F) { // 0x70-0x7F: short pause (last 4 bits + 1) samples
            uint8_t w = (cmd & 0x0F) + 1; current_samples += w; syncWaitUs(( (uint32_t)w * 10000 ) / 441); 
          }
          // if we dont know a command, doing nothing
          else if (cmd >= 0x30 && cmd <= 0x3F) vgmFile.read(); // 1 byte command
          else if (cmd >= 0x40 && cmd <= 0x4F) { vgmFile.read(); vgmFile.read(); } // 2 byte command
          else if (cmd >= 0x50 && cmd <= 0x5F) { vgmFile.read(); vgmFile.read(); } 
          else if (cmd >= 0xA0 && cmd <= 0xBF) { vgmFile.read(); vgmFile.read(); }
          else if (cmd >= 0xC0 && cmd <= 0xDF) { vgmFile.read(); vgmFile.read(); vgmFile.read(); } // 3 byte command
          else if (cmd >= 0xE0 && cmd <= 0xFF) { read32(); } // 4 byte command
          break;
        } //                    and we are gaming. or idk
      }
    }
  }
