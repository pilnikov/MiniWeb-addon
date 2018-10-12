//******************************************************************************************
//*  MiniWebRadio -- Webradio receiver for ESP32, 2.8 color display and VS1053 MP3 module.    *
//******************************************************************************************
//
// Preparations:
//
// 1)  change the partitionstable (defaut.csv in folder esp32/tools/partitions/)
//     MiniWebRadio need 1,5MByte flash and 200KByte nvs
//
//   # Name,     Type,   SubType,   Offset,   Size,     Flags
//     phy_init, data,   phy,       0x9000,   0x7000,
//     factory,  app,    factory,   0x10000,  0x300000,
//     nvs,      data,   nvs,       0x310000, 0x32000,
//     spiffs,   data,   spiffs,    0x342000, 0xB0000,
//     eeprom,   data,   0x99,      0x3F2000, 0xD000,
//
//     or copy the default.csv from the repository to replace the original one
//
//     Arduino IDE only: set treshold in boards.txt from [boardname].upload.maximum_size=1310720 to 3145728
//
// 2)  set the Timezone mentioned below, examples are in rtime.cpp
//
// 3)  extract the zip file to SD Card
//
// 4)  set WiFi credentials below, more credentials can be set in networks.csv (SD Card)
//
// 5)  change GPIOs if nessessary, e.g ESP32 Pico V4: GPIO16 and 17 are connected to FLASH
//
// 6)  add libraries from my repositories to this project: vs1053_ext, IR and tft
//     TFT controller can be ILI9341 or HX8347D
//
//
//
//  Display 320x240
//  +-------------------------------------------+ _yHeader=0
//  | Header                                    |       _hHeader=20px
//  +-------------------------------------------+ _yName=20
//  |                                           |
//  | Logo                   StationName        |       _hName=100px
//  |                                           |
//  +-------------------------------------------+ _yTitle=120
//  |                                           |
//  |              StreamTitle                  |       _hTitle=100px
//  |                                           |
//  +-------------------------------------------+ _yFooter=220
//  | Footer                                    |       _hFooter=20px
//  +-------------------------------------------+ 240
//                                             320

#include <Arduino.h>

// system libraries
#include <Preferences.h>
#include <SPI.h>
#include <SD.h>
#include <FS.h>

#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiManager.h>

#include <TimeLib.h>
#include <ArduinoOTA.h>


// own libraries
#include "IR.h"             // see my repository at github "ESP32-IR-Remote-Control"
#include "tft.h"            // see my repository at github "ESP32-TFT-Library-ILI9431-HX8347D"
#include "mp3.h"            // 

// Digital I/O used
#define TFT_CS        22
#define TFT_DC        21
#define TFT_BL        27  // 33 (pico V4) 17 def 27 ttgo
#define TP_IRQ        39
#define TP_CS         26  // 32 (pico V4) 16 def 26 ttgo
#define SD_CS          5
#define IR_PIN        34
#define SPI_MOSI      23
#define SPI_MISO      19
#define SPI_SCK       18


//global variables
char     _chbuf[1024];
String   _station = "",   _stationname = "",   _stationURL = "",      _homepage = "";
String   _title = "",     _info = "",          _alarmtime = "";
String   _time_s = "",    _hour = "",          _bitrate = "",         _mp3Name[10];
String   _pressBtn[5],  _releaseBtn[5],    _myIP = "0.0.0.0",     _lastconnectedhost = "";
int8_t   _mp3Index = 0;         // pointer _mp3Name[]
uint8_t  _releaseNr = 0;
uint8_t  _timefile = 0;         // speak the time
uint8_t  _commercial_dur = 0;   // duration of advertising
uint16_t _sleeptime = 0;        // time in min until MiniWebRadio goes to sleep
uint32_t _millis = 0;
uint32_t _alarmdays = 0;
boolean  f_1sec = false;        // flag is set every second
boolean  f_1min = false;        // flag is set every minute
boolean  f_SD_okay = false;     // true if SD card in place and readable
boolean  f_mute = false;
boolean  f_rtc = false;         // true if time from ntp is received
boolean  f_mp3eof = false;      // set at the end of mp3 file
boolean  f_alarm = false;       // set if alarmday and alarmtime is equal localtime
boolean  f_timespeech = false;  // if true activate timespeech
boolean  f_has_ST = false;      // has StreamTitle?
boolean  f_sleeping = false;    // true if sleepmode
boolean  semaphore = false;

// display layout
const uint16_t _yHeader = 0;                    // yPos Header
const uint16_t _hHeader = 20;                   // height Header
const uint16_t _yName  = _yHeader + _hHeader;   // yPos StationName
const uint16_t _hName  = 100;                   // height Stationname
const uint16_t _yTitle = _yName + _hName;       // yPos StreamTitle
const uint16_t _hTitle = 100;                   // height StreamTitle
const uint16_t _yFooter = _yTitle + _hTitle;    // yPos Footer
const uint16_t _hFooter = 20;                   // height Footer
const uint16_t _yVolBar = _yTitle + 30;         // yPos VolumeBar
const uint16_t _hVolBar = 5;                    // height VolumeBar
const uint16_t _wLogo = 96;                     // width Logo
const uint16_t _hLogo = 96;                     // height Logo
const uint16_t _yLogo = _yName + (_hName - _hLogo) / 2; // yPos Logos
const uint16_t _wBtn = 64;                      // width Button
const uint16_t _hBtn = 64;                      // height Button
const uint16_t _yBtn = _yVolBar + _hVolBar + 10; // yPos Buttons

enum status {RADIO = 0, RADIOico = 1, RADIOmenue = 2, CLOCK = 3, CLOCKico = 4, BRIGHTNESS = 5, MP3PLAYER = 6,
             MP3PLAYERico = 7, ALARM = 8, SLEEP = 9
            };
status _state = RADIO;          //statemaschine

unsigned long   telnet_time  = millis();

//----------------------------------------------------- Karadio specific data


#define BUFLEN  180

char line[BUFLEN]; // receive buffer
char station[BUFLEN]; //received station
char title[BUFLEN]; // received title
char nameset[BUFLEN]; // the local name of the station
char _sta_num[4];    // the local number of the station
char genre[BUFLEN]; // the genre of the station
int16_t volume;
uint8_t _index = 0;
bool askDraw = false, syncTime = false, itAskTime = false;


//------------------------------------------------ Wifi
const char* radio_addr = "192.168.43.42";

#define DBG_OUT_PORT Serial

//objects
TFT tft(0);                     // parameter:  (0)ILI9341, (1)HX8347D
hw_timer_t* timer = NULL;       // instance of the timer
Preferences pref;
IR ir(IR_PIN);                  // do not change the objectname, it must be "ir"
TP tp(TP_CS, TP_IRQ);
MP3 mp3;

//**************************************************************************************************
//                                D E F A U L T S E T T I N G S                                    *
//**************************************************************************************************

void defaultsettings() {
  String cy = "", str = "", str_s = "", str_u = "", info = "";
  char tkey[12];
  uint16_t i = 0, j = 0, cnt = 0;
  log_i("set default");
  //
  pref.clear();
  //
  pref.putString("MiniWebRadio", "default");
  pref.putUInt("brightness", 100); // 100% display backlight
  pref.putUInt("alarm_weekday", 0); // for alarmclock
  pref.putString("alarm_time", "00:00");
  pref.putUInt("ringvolume", 21);
  //
  pref.putUInt("volume", 12); // 0...21
  pref.putUInt("mute",   0); // no mute
  //
  pref.putUInt("preset", 1);
  pref.putUInt("sleeptime", 1);
  //
  // StationList
  File file = SD.open("/presets.txt");
  if (file) {                                     // try to read from SD
    while (file.available()) {
      str = file.readStringUntil('\n');       // read the line
      if (str[0] == '*' ) continue;           // ignore this, goto next line
      if (str[0] == '\n') continue;           // empty line
      i = 1;  while (str[i] == '\t') i++;     // seek first entry, skip tabs
      j = i;  while (str[j] >= 32) j++;       // end of first entry?
      cy =  str.substring(i, j);              // is country
      cy.trim();
      if (cy.length() == 0) {};               // empty country?
      i = j;  while (str[i] == '\t') i++;     // seek next entry, skip tabs
      j = i;  while (str[j] >= 32) j++;       // end of next entry?
      str_s = str.substring(i, j);
      i = j;  while (str[i] == '\t') i++;     // seek next entry, skip tabs
      j = i;  while (str[j] >= 32) j++;       // end of next entry?
      str_u = str.substring(i, j);
      i = j;  while (str[i] == '\t') i++;     // seek last entry, skip tabs
      j = i;  while (str[j] >= 32) j++;       // end of last entry?
      info = str.substring(i, j);
      if (str_s.length() == 0) continue;
      if (str_u.length() == 0) continue;
      str_s.trim(); str_u.trim();
      cnt++;
      str = str_s; // station
      str += "#";
      str += str_u; // url
      log_i("cy=%s  str_s=%s  str_u=%s  info=%s", cy.c_str(), str_s.c_str(), str_u.c_str(), info.c_str());
      sprintf(tkey, "preset_%03d", cnt);
      pref.putString(tkey, str);
      if (info.length() > 4) { // is reasonable? then save additional info
        info.trim();
        sprintf(tkey, "info_%03d", cnt);
        pref.putString(tkey, info);
      }

    }
    file.close();
    pref.putUInt("maxstations", cnt);
    log_i("maxstations: %i", cnt);
  }
  else {                                      // file not available
    log_i("SD/presets.txt not found, use default stream URls");
    String s[12], u[12];
    s[  1] = "030-berlinfm";          u[  1] = "030-berlinfm.stream.laut.fm/030-berlinfm"; //D
    s[  2] = "104.6 RTL";             u[  2] = "rtlberlin.hoerradar.de/rtlberlin-live-mp3-128"; //D
    s[  3] = "105.5 Spreeradio";      u[  3] = "stream.spreeradio.de/spree-live/mp3-128/vtuner/"; //D
    s[  4] = "Бобина";                u[  4] = "ic5.101.ru:8000/p429088"; //D
    s[  5] = "1Live, WDR Koeln";      u[  5] = "www.wdr.de/wdrlive/media/einslive.m3u"; //D
    s[  6] = "88vier";                u[  6] = "ice.rosebud-media.de:8000/88vier"; //D
    s[  7] = "93,6 JAM FM";           u[  7] = "stream.jam.fm/jamfm-live/mp3-128/vtuner/"; //D
    s[  8] = "94.3 RS2";              u[  8] = "stream.rs2.de/rs2/mp3-128/internetradio"; //D
    s[  9] = "94.3 RS2 Partymix";     u[  9] = "stream.rs2.de/rs2-relax/mp3-128/internetradio"; //D
    s[ 10] = "95.5 Charivari";        u[ 10] = "rs5.stream24.net:80/stream"; //D
    s[ 11] = "98.8 KISS FM";          u[ 11] = "stream.kissfm.de/kissfm/mp3-128/internetradio"; //D
    for (i = 0; i < 12; i++) {
      sprintf(tkey, "preset_%03d", i);
      str = s[i] + String("#") + u[i];
      pref.putString(tkey, str);
    }
    pref.putUInt("maxstations", 11);
  }
}
boolean ST_rep() { // if station has no streamtitle: replace streamtitle, seek in info
  uint16_t preset = pref.getUInt("preset");
  sprintf(_chbuf, "info_%03d", preset);
  String str = pref.getString(_chbuf, "Station provides no Streamtitle"); // found probably replacement information
  if (str.length() > 5) {
    showTitle(str);
    return true;
  }
  return false;
}
//**************************************************************************************************
//                                T F T   B R I G H T N E S S                                      *
//**************************************************************************************************
void setTFTbrightness(uint8_t duty) { //duty 0...100 (min...max)
  ledcAttachPin(TFT_BL, 1);        //Configure variable led, TFT_BL pin to channel 1
  ledcSetup(1, 12000, 8);          // 12 kHz PWM and 8 bit resolution
  ledcWrite(1, duty);
}
inline uint32_t getTFTbrightness() {
  return ledcRead(1);
}
inline uint8_t downBrightness() {
  uint8_t br; br = pref.getUInt("brightness");
  if (br > 5) {
    br -= 5;
    pref.putUInt("brightness", br);
    setTFTbrightness(br);
  } return br;
}
inline uint8_t upBrightness() {
  uint8_t br; br = pref.getUInt("brightness");
  if (br < 100) {
    br += 5;
    pref.putUInt("brightness", br);
    setTFTbrightness(br);
  } return br;
}
inline uint8_t getBrightness() {
  return pref.getUInt("brightness");
}
//**************************************************************************************************
//                                       A S C I I                                                 *
//**************************************************************************************************
char ASCIIfromUTF8(char ch) { // if no ascii char available return blank
  uint8_t ascii;
  char tab[96] = {
    96, 173, 155, 156, 32, 157, 32, 32, 32, 32, 166, 174, 170, 32, 32, 32, 248, 241, 253, 32,
    32, 230, 32, 250, 32, 32, 167, 175, 172, 171, 32, 168, 32, 32, 32, 32, 142, 143, 146, 128,
    32, 144, 32, 32, 32, 32, 32, 32, 32, 165, 32, 32, 32, 32, 153, 32, 32, 32, 32, 32,
    154, 32, 32, 225, 133, 160, 131, 32, 132, 134, 145, 135, 138, 130, 136, 137, 141, 161, 140, 139,
    32, 164, 149, 162, 147, 32, 148, 246, 32, 151, 163, 150, 129, 32, 32, 152
  };
  ascii = ch;
  if (ch < 128) return ascii;
  if (ch < 160) return 32;
  ascii -= 160;
  ascii = tab[ascii];
  return ascii;
}
uint16_t UTF8fromASCII(char ch) {
  uint16_t uni;
  uint16_t tab[128] = {
    199, 252, 233, 226, 228, 224, 229, 231, 234, 235, 232, 239, 238, 236, 196, 197,
    201, 230, 198, 244, 246, 242, 251, 249, 255, 214, 220, 162, 163, 165, 8359, 402,
    225, 237, 243, 250, 241, 209, 170, 186, 191, 8976, 172, 189, 188, 161, 171, 187,
    9617, 9618, 9619, 9474, 9508, 9569, 9570, 9558, 9557, 9571, 9553, 9559, 9565, 9564, 9563, 9488,
    9492, 9524, 9516, 9500, 9472, 9532, 9566, 9567, 9562, 9556, 9577, 9574, 9568, 9552, 9580, 9575,
    9576, 9572, 9573, 9561, 9560, 9554, 9555, 9579, 9578, 9496, 9484, 9608, 9604, 9612, 9616, 9600,
    945, 223, 915, 960, 931, 963, 181, 964, 934, 920, 937, 948, 8734, 966, 949, 8745,
    8801, 177, 8805, 8804, 8992, 8993, 247, 8776, 176, 8729, 183, 8730, 8319, 178, 9632, 160
  };
  uni = ch;
  if (ch < 128)return uni;
  uni -= 128;
  uni = tab[uni];
  return uni;
}
const char* ASCIItoUTF8(const char* str) {
  uint16_t i = 0, j = 0, uni = 0;
  while ((str[i] != 0) && (j < 1020)) {
    uni = UTF8fromASCII(str[i]);
    switch (uni) {
      case   0 ... 127: {
          _chbuf[j] = str[i];
          i++;
          j++;
          break;
        }
      case 160 ... 191: {
          _chbuf[j] = 0xC2;
          _chbuf[j + 1] = uni;
          j += 2;
          i++;
          break;
        }
      case 192 ... 255: {
          _chbuf[j] = 0xC3;
          _chbuf[j + 1] = uni - 64;
          j += 2;
          i++;
          break;
        }
      default: {
          _chbuf[j] = ' ';  // ignore all other
          i++;
          j++;
        }
    }
  }
  _chbuf[j] = 0;
  return _chbuf;
}
const char* UTF8toASCII(const char* str) {
  uint16_t i = 0, j = 0;
  while ((str[i] != 0) && (j < 1020)) {
    _chbuf[j] = str[i];
    if (str[i] == 0xC2) { // compute unicode from utf8
      i++;
      if ((str[i] > 159) && (str[i] < 192)) _chbuf[j] = ASCIIfromUTF8(str[i]);
      else _chbuf[j] = 32;
    }
    else if (str[i] == 0xC3) {
      i++;
      if ((str[i] > 127) && (str[i] < 192)) _chbuf[j] = ASCIIfromUTF8(str[i] + 64);
      else _chbuf[j] = 32;
    }
    i++; j++;
  }
  _chbuf[j] = 0;
  return (_chbuf);
}
//**************************************************************************************************
//                                        T I M E R                                                *
//**************************************************************************************************
void IRAM_ATTR timer1sec() {
  static uint8_t sec = 0;
  f_1sec = true;
  sec++;
  if (sec == 60) {
    sec = 0;
    f_1min = true;
  }

}
void IRAM_ATTR timer5() {                              // called every 5ms
  static uint8_t  count1sec = 0;
  count1sec++;
  if (count1sec == 200) {
    count1sec = 0; timer1sec();                    // 1 second passed?
  }
}
void startTimer() {
  timer = timerBegin(0, 80, true); // timer_id = 0; divider=80; countUp = true;
  timerAttachInterrupt(timer, &timer5, true); // edge = true
  timerAlarmWrite(timer, 5000, true); //5 ms
  timerAlarmEnable(timer);
  delay(1000);
}
//**************************************************************************************************
//                                       D I S P L A Y                                             *
//**************************************************************************************************
inline void clearHeader() {
  tft.fillRect(0, _yHeader, tft.width(), _hHeader, TFT_BLACK); // y   0...19
}
inline void clearStation() {
  tft.fillRect(0, _yName,   tft.width(), _hName,   TFT_BLACK); // y  20...119
}
inline void clearTitle()  {
  tft.fillRect(0, _yTitle,  tft.width(), _hTitle,  TFT_BLACK); // y 120...219
}
inline void clearFooter() {
  tft.fillRect(0, _yFooter, tft.width(), _hFooter, TFT_BLACK); // y 220...239
}
inline void clearDisplay() {
  tft.fillScreen(TFT_BLACK); // y   0...239
}
inline uint16_t txtlen(String str) {
  uint16_t len = 0;
  for (int i = 0; i < str.length(); i++) if (str[i] <= 0xC2) len++;
  return len;
}

void display_info(const char *str, int ypos, int height, uint16_t color, uint16_t indent) {
  tft.fillRect(0, ypos, tft.width(), height, TFT_BLACK);  // Clear the space for new info
  tft.setTextColor(color);                                // Set the requested color
  tft.setCursor(indent, ypos);                            // Prepare to show the info
  tft.print(str);                                         // Show the string
}
void showTitle(String str) {
  str.trim();  // remove all leading or trailing whitespaces
  str.replace(" | ", "\n");   // some stations use pipe as \n or
  str.replace("| ", "\n");    // or
  str.replace("|", "\n");
  if (_state != RADIO) return;
  if (txtlen(str) >  4) f_has_ST = true; else f_has_ST = false;
  tft.setFont(Times_New_Roman43x35);
  if (txtlen(str) > 30) tft.setFont(Times_New_Roman38x31);
  if (txtlen(str) > 50) tft.setFont(Times_New_Roman34x27);
  if (txtlen(str) > 70) tft.setFont(Times_New_Roman27x21);
  if (txtlen(str) > 130) tft.setFont(Times_New_Roman21x17);
  //    for(int i=0;i<str.length(); i++) log_i("str[%i]=%i", i, str[i]);  // see what You get
  display_info(str.c_str(), _yTitle, _hTitle, TFT_CYAN, 0);
}
void showStation() {
  String str1 = "", str2 = "", str3 = "";
  int16_t idx = 0;
  if (_stationname == "") str1 = _station;
  else str1 = _stationname;
  str2 = str1;
  str3 = str1; // now str1, str2 and str3 contains either _station or _stationname
  idx = str1.indexOf('|');
  if (idx > 0) {
    str2 = str1.substring(0, idx); // before pipe
    str2.trim();
    str3 = str1.substring(idx + 1); // after pipe
    str3.trim();
  }

  tft.setFont(Times_New_Roman43x35);
  if (txtlen(str2) > 30) tft.setFont(Times_New_Roman38x31);
  if (txtlen(str2) > 50) tft.setFont(Times_New_Roman34x27);
  if (txtlen(str2) > 80) tft.setFont(Times_New_Roman27x21);
  display_info(str2.c_str(), _yName, _hName, TFT_YELLOW, _wLogo + 14); // Show station name

  showTitle("");
  showFooter();

  //    str3.toLowerCase();
  //    str3.replace(",",".");
  str2 = "/logo/" + String(UTF8toASCII(str3.c_str())) + ".bmp";
  if (f_SD_okay) {
    if (tft.drawBmpFile(SD, str2.c_str(), 0, _yLogo) == false) { // filename mostly given from _stationname exist?
      str2 = "/logo/" + String(UTF8toASCII(_station.c_str())) + ".bmp";
      if (tft.drawBmpFile(SD, str2.c_str(), 0, _yLogo) == false) { // filename given from station exist?
        tft.drawBmpFile(SD, "/logo/unknown.bmp", 1, 22); // if no draw unknown
      }
    }
  }
}
void showHeadlineVolume(uint8_t vol) {
  if (_state == ALARM || _state == BRIGHTNESS) return;
  sprintf(_chbuf, "Vol %02d", vol);
  tft.fillRect(180, _yHeader, 69, _hHeader, TFT_BLACK);
  tft.setCursor(180, _yHeader);
  tft.setFont(Times_New_Roman27x21);
  tft.setTextColor(TFT_DEEPSKYBLUE);
  tft.print(_chbuf);
}
void showHeadlineItem(const char* hl) {
  tft.setFont(Times_New_Roman27x21);
  display_info(hl, _yHeader, _hHeader, TFT_WHITE, 0);
  if (_state != SLEEP) showHeadlineVolume(volume);
}
void showHeadlineTime() {
  if (_state == CLOCK || _state == CLOCKico || _state == BRIGHTNESS || _state == ALARM || _state == SLEEP) return;
  tft.setFont(Times_New_Roman27x21);
  tft.setTextColor(TFT_GREENYELLOW);
  tft.fillRect(250, _yHeader, 89, _hHeader, TFT_BLACK);
  if (!f_rtc) return; // has rtc the correct time? no -> return
  tft.setCursor(250, 0);
  tft.print(gettime_s());
}
void showFooter() { // bitrate stationnumber, IPaddress
  if (_state != RADIO) return;
  clearFooter();
  if (_bitrate.length() == 0) _bitrate = "   "; // if bitrate is unknown
  tft.setFont(Times_New_Roman21x17);
  tft.setTextColor(TFT_GREENYELLOW);
  tft.setCursor(0, _yFooter);
  tft.print("BR:");
  tft.setTextColor(TFT_LAVENDER);
  tft.print(_bitrate.c_str());
  tft.setCursor(60, _yFooter);
  tft.setTextColor(TFT_GREENYELLOW);
  tft.print("STA:");
  tft.setTextColor(TFT_LAVENDER);
  tft.print(_sta_num);
  tft.setCursor(130, _yFooter);
  tft.setTextColor(TFT_GREENYELLOW);
  tft.print("myIP:");
  tft.setTextColor(TFT_LAVENDER);
  tft.print(_myIP.c_str());
  tft.setCursor(280, _yFooter);
  tft.setTextColor(TFT_GREENYELLOW);
  tft.print("S:");
  if (_sleeptime == 0 ) tft.setTextColor(TFT_LAVENDER); else tft.setTextColor(TFT_ORANGE);
  tft.printf("%03d", _sleeptime);
}
void updateSleepTime() {
  if (_sleeptime > 0) {
    _sleeptime--;
    if (_sleeptime == 0) {
      setTFTbrightness(0);    // backlight off
      //      mp3.setVolume(0);       // silence ///////////////////////////////////////////////////////////
      f_sleeping = true;      // MiniWebRadio is in sleepmode now
    }
  }
  if (_state == RADIO) showFooter();
}

//**************************************************************************************************
//                                       L I S T M P 3 F I L E                                     *
//**************************************************************************************************
String listmp3file(const char * dirname = "/mp3files", uint8_t levels = 2, fs::FS &fs = SD) {
  static String SD_outbuf = "";          // Output buffer for cmdclient
  String filename;                       // Copy of filename for lowercase test
  uint8_t index = 0;
  if (!f_SD_okay) return "";             // See if known card
  File root = fs.open(dirname);
  if (!root) {
    log_e("Failed to open directory");
    return "";
  }
  if (!root.isDirectory()) {
    log_e("Not a directory");
    return "";
  }
  SD_outbuf = "";
  File file = root.openNextFile();
  while (file) {
    if (file.isDirectory()) {
      if (levels) {
        listmp3file(file.name(), levels - 1, fs);
      }
    } else {
      //log_i("FILE: %s, SIZE: %i",file.name(), file.size());
      filename = file.name();
      filename.substring(filename.length() - 4).toLowerCase();
      filename = filename.substring(1, filename.length()); // remove first '/'
      if (filename.endsWith(".mp3")) {
        filename += "\n";
        if (index < 10) {
          _mp3Name[index] = filename;  //store the first 10 Names
          index++;
        }
        SD_outbuf += ASCIItoUTF8(filename.c_str());
      }
    }
    file = root.openNextFile();
  }
  if (SD_outbuf == "") SD_outbuf + "\n"; //nothing found
  return SD_outbuf;
}
//**************************************************************************************************
//                                           S E T U P                                             *
//**************************************************************************************************
void setup()
{
  DBG_OUT_PORT.begin(115200); // For debug

  DBG_OUT_PORT.println("Starting.....");

  // first set all components inactive
  pinMode(SD_CS, OUTPUT);      digitalWrite(SD_CS, HIGH);
  pinMode(TFT_CS, OUTPUT);     digitalWrite(TFT_CS, HIGH);
  pinMode(TP_CS, OUTPUT);      digitalWrite(TP_CS, HIGH);
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);

  SD.end();       // to recognize SD after reset correctly
  DBG_OUT_PORT.println("setup      : Init SD card");
  SD.begin(SD_CS);
  delay(100); // wait while SD is ready
  tft.begin(TFT_CS, TFT_DC, SPI_MOSI, SPI_MISO, SPI_SCK, TFT_BL);    // Init TFT interface
  SD.begin(SD_CS, SPI, 16000000);  // faster speed, after tft.begin()
  ir.begin();  // Init InfraredDecoder
  tft.setRotation(3); // Use landscape format
  tp.setRotation(3);
  pref.begin("MiniWebRadio", false);
  setTFTbrightness(pref.getUInt("brightness"));
  f_SD_okay = (SD.cardType() != CARD_NONE); // See if known card
  if (!f_SD_okay) DBG_OUT_PORT.println("setup      : SD card not found");
  else DBG_OUT_PORT.println("setup      : found SD card");
  if (pref.getString("MiniWebRadio") != "default") defaultsettings(); // first init
  if (f_SD_okay) tft.drawBmpFile(SD, "/MiniWebRadio.bmp", 0, 0); //Welcomescreen

  _alarmdays = pref.getUInt("alarm_weekday");
  _alarmtime = pref.getString("alarm_time");
  setTFTbrightness(pref.getUInt("brightness"));
  delay(100);

  tft.fillScreen(TFT_BLACK); // Clear screen
  showHeadlineItem("** KaRadio addon **");


  //mp3.connecttohost(readhostfrompref(0)); //last used station
  //mp3.printDetails();


  //------------------------------------------------------  Определяем консоль

  DBG_OUT_PORT.setDebugOutput(true);

  //-------------------------------------------------------- Запускаем WiFi

  start_wifi();

  DBG_OUT_PORT.println("WiFi started");

  //------------------------------------------------------ Подключаем OTA
  OTA_init();

  //------------------------------------------------------ Запускаем парсер
  telnet_time = millis() + 31000;
  radio_snd("cli.info", true);
  info();
  migrate();

  showHeadlineVolume(volume);
  showStation(); // if station gives no icy-name display _stationname
  showTitle(_title);
  startTimer();

}
//**************************************************************************************************
//                                      S E T T I N G S                                            *
//**************************************************************************************************
void getsettings(int8_t config = 0) { //config=0 update index.html, config=1 update config.html
  String val = "", content = "", s = "", u = "";
  int i, j, ix, s_len = 0;
  char tkey[12];
  char nr[8];
  for (i = 1; i < (pref.getUInt("maxstations") + 1); i++) { // max presets
    sprintf(tkey, "preset_%03d", i);
    sprintf(nr, "%03d - ", i);
    content = pref.getString(tkey);
    ix = content.indexOf("#");
    if (ix > 0) {
      u = content.substring(ix, content.length());
      s = content.substring(0, ix);
      j = 0; s_len = 0;
      while (s[j] != 0) {
        if ((s[j] < 0xC2) || (s[j] > 0xD4)) s_len++;  //count UTF-8 chars, compute len
        j++;
      }
    }
    else {
      s = " ";
      u = " ";
    }
    if (config == 0) {
      content = String(nr) + s;
      val = String(tkey) + String("=") + String(content) + String("\n");
    }
    if (config == 1) {
      for (j = s_len; j < 39; j++) s += String(" ");
      content = s + u;
      val = String(nr) + (String(content) + String("\n"));
    }
  }
}

//**************************************************************************************************
inline uint8_t downvolume() {
  uint8_t vol; vol = pref.getUInt("volume");
  if (vol > 0) {
    vol--;
    pref.putUInt("volume", vol);
    //    if (f_mute == false) mp3.setVolume(vol);/////////////////////////////////////////////////////////
  }
  showHeadlineVolume(vol); return vol;
}
inline uint8_t upvolume() {
  uint8_t vol; vol = pref.getUInt("volume");
  if (vol < 21) {
    vol++;
    pref.putUInt("volume", vol);
    //    if (f_mute == false) mp3.setVolume(vol); ///////////////////////////////////////////////////////////
  }
  showHeadlineVolume(vol); return vol;
}
inline uint8_t getvolume() {
  return pref.getUInt("volume");
}
inline void mute() {
  if (f_mute == false) {
    f_mute = true;
    //    mp3.setVolume(0);  /////////////////////////////////////////////////////////////////////////////////////
    showHeadlineVolume(0);
  }
  else {
    f_mute = false;
    //    mp3.setVolume(volume); ////////////////////////////////////////////////////////////////////////////////
    showHeadlineVolume(volume);
  }
  pref.putUInt("mute", f_mute);
}
inline void showVolumeBar() {
  uint16_t vol = tft.width() * pref.getUInt("volume") / 21;
  tft.fillRect(0, _yVolBar, vol, _hVolBar, TFT_RED);
  tft.fillRect(vol + 1, _yVolBar, tft.width() - vol + 1, _hVolBar, TFT_GREEN);
}
inline void showBrightnessBar() {
  uint16_t br = tft.width() * pref.getUInt("brightness") / 100;
  tft.fillRect(0, 140, br, 5, TFT_RED); tft.fillRect(br + 1, 140, tft.width() - br + 1, 5, TFT_GREEN);
}
inline String StationsItems() {
  return (String(pref.getUInt("preset")) + " " + _stationURL + " " + _stationname);
}
//**************************************************************************************************
//                                M E N U E / B U T T O N S                                        *
//**************************************************************************************************
void changeState(int state) {
  if (!f_SD_okay) return;
  _state = static_cast<status>(state);
  switch (_state) {
    case RADIO: {
        showFooter(); showHeadlineItem("** KaRadio addon **");
        showStation(); showTitle(_title);
        break;
      }
    case RADIOico: {
        _pressBtn[0] = "/btn/Button_Mute_Red.bmp";           _releaseBtn[0] = "/btn/Button_Mute_Green.bmp";
        _pressBtn[1] = "/btn/Button_Volume_Down_Yellow.bmp"; _releaseBtn[1] = "/btn/Button_Volume_Down_Blue.bmp";
        _pressBtn[2] = "/btn/Button_Volume_Up_Yellow.bmp";   _releaseBtn[2] = "/btn/Button_Volume_Up_Blue.bmp";
        _pressBtn[3] = "/btn/Button_Previous_Yellow.bmp";    _releaseBtn[3] = "/btn/Button_Previous_Green.bmp";
        _pressBtn[4] = "/btn/Button_Next_Yellow.bmp";        _releaseBtn[4] = "/btn/Button_Next_Green.bmp";
        clearTitle(); clearFooter(); showVolumeBar();
        break;
      }
    case RADIOmenue: {
        _pressBtn[0] = "/btn/MP3_Yellow.bmp";                _releaseBtn[0] = "/btn/MP3_Green.bmp";
        _pressBtn[1] = "/btn/Clock_Yellow.bmp";              _releaseBtn[1] = "/btn/Clock_Green.bmp";
        _pressBtn[2] = "/btn/Radio_Yellow.bmp";              _releaseBtn[2] = "/btn/Radio_Green.bmp";
        _pressBtn[3] = "/btn/Button_Sleep_Yellow.bmp";       _releaseBtn[3] = "/btn/Button_Sleep_Green.bmp";
        _pressBtn[4] = "/btn/Bulb_Yellow.bmp";               _releaseBtn[4] = "/btn/Bulb_Green.bmp";
        clearTitle(); clearFooter();
        break;
      }
    case CLOCKico: {
        _pressBtn[0] = "/btn/MP3_Yellow.bmp";                _releaseBtn[0] = "/btn/MP3_Green.bmp";
        _pressBtn[1] = "/btn/Bell_Yellow.bmp";               _releaseBtn[1] = "/btn/Bell_Green.bmp";
        _pressBtn[2] = "/btn/Radio_Yellow.bmp";              _releaseBtn[2] = "/btn/Radio_Green.bmp";
        _pressBtn[3] = "/btn/Black.bmp";                     _releaseBtn[3] = "/btn/Black.bmp";
        _pressBtn[4] = "/btn/Black.bmp";                     _releaseBtn[4] = "/btn/Black.bmp";
        break;
      }
    case BRIGHTNESS: {
        _pressBtn[0] = "/btn/Button_Left_Yellow.bmp";        _releaseBtn[0] = "/btn/Button_Left_Blue.bmp";
        _pressBtn[1] = "/btn/Button_Right_Yellow.bmp";       _releaseBtn[1] = "/btn/Button_Right_Blue.bmp";
        _pressBtn[2] = "/btn/Button_Ready_Yellow.bmp";       _releaseBtn[2] = "/btn/Button_Ready_Blue.bmp";
        _pressBtn[3] = "/btn/Black.bmp";                     _releaseBtn[3] = "/btn/Black.bmp";
        _pressBtn[4] = "/btn/Black.bmp";                     _releaseBtn[4] = "/btn/Black.bmp";
        break;
      }
    case MP3PLAYER: {
        _pressBtn[0] = "/btn/Radio_Yellow.bmp";              _releaseBtn[0] = "/btn/Radio_Green.bmp";
        _pressBtn[1] = "/btn/Button_Left_Yellow.bmp";        _releaseBtn[1] = "/btn/Button_Left_Blue.bmp";
        _pressBtn[2] = "/btn/Button_Right_Yellow.bmp";       _releaseBtn[2] = "/btn/Button_Right_Blue.bmp";
        _pressBtn[3] = "/btn/Button_Ready_Yellow.bmp";       _releaseBtn[3] = "/btn/Button_Ready_Blue.bmp";
        _pressBtn[4] = "/btn/Black.bmp";                     _releaseBtn[4] = "/btn/Black.bmp";
        break;
      }
    case MP3PLAYERico: {
        _pressBtn[0] = "/btn/Button_Mute_Red.bmp";           _releaseBtn[0] = "/btn/Button_Mute_Green.bmp";
        _pressBtn[1] = "/btn/Button_Volume_Down_Yellow.bmp"; _releaseBtn[1] = "/btn/Button_Volume_Down_Blue.bmp";
        _pressBtn[2] = "/btn/Button_Volume_Up_Yellow.bmp";   _releaseBtn[2] = "/btn/Button_Volume_Up_Blue.bmp";
        _pressBtn[3] = "/btn/MP3_Yellow.bmp";                _releaseBtn[3] = "/btn/MP3_Green.bmp";
        _pressBtn[4] = "/btn/Radio_Yellow.bmp";              _releaseBtn[4] = "/btn/Radio_Green.bmp";
        break;
      }
    case ALARM: {
        _pressBtn[0] = "/btn/Button_Left_Yellow.bmp";        _releaseBtn[0] = "/btn/Button_Left_Blue.bmp";
        _pressBtn[1] = "/btn/Button_Right_Yellow.bmp";       _releaseBtn[1] = "/btn/Button_Right_Blue.bmp";
        _pressBtn[2] = "/btn/Button_Up_Yellow.bmp";          _releaseBtn[2] = "/btn/Button_Up_Blue.bmp";
        _pressBtn[3] = "/btn/Button_Down_Yellow.bmp";        _releaseBtn[3] = "/btn/Button_Down_Blue.bmp";
        _pressBtn[4] = "/btn/Button_Ready_Yellow.bmp";       _releaseBtn[4] = "/btn/Button_Ready_Blue.bmp";
        break;
      }
    case SLEEP: {
        _pressBtn[0] = "/btn/Button_Up_Yellow.bmp";          _releaseBtn[0] = "/btn/Button_Up_Blue.bmp";
        _pressBtn[1] = "/btn/Button_Down_Yellow.bmp";        _releaseBtn[1] = "/btn/Button_Down_Blue.bmp";
        _pressBtn[2] = "/btn/Button_Ready_Yellow.bmp";       _releaseBtn[2] = "/btn/Button_Ready_Blue.bmp";
        _pressBtn[3] = "/btn/Black.bmp";                     _releaseBtn[3] = "/btn/Black.bmp";
        _pressBtn[4] = "/btn/Button_Cancel_Yellow.bmp";      _releaseBtn[4] = "/btn/Button_Cancel_Blue.bmp";
        break;
      }
    case CLOCK: {
        break;
      }

  }
  if (_state != RADIO && _state != CLOCK) { // RADIO and CLOCK have no Buttons
    int j = 0;
    if (_state == RADIOico || _state == MP3PLAYERico) { // show correct mute button
      if (f_mute == false) {
        tft.drawBmpFile(SD, _releaseBtn[0].c_str(), 0, _yBtn);
        mp3.loop();
      }
      else {
        tft.drawBmpFile(SD, _pressBtn[0].c_str(), 0, _yBtn);
        mp3.loop();
      }
      j = 1;
    }
    for (int i = j; i < 5; i++) {
      tft.drawBmpFile(SD, _releaseBtn[i].c_str(), i * _wBtn, _yBtn);
      mp3.loop();
    }
  }
}
void changeBtn_pressed(uint8_t btnNr) {
  if (_state != RADIO && _state != CLOCK) tft.drawBmpFile(SD, _pressBtn[btnNr].c_str(), btnNr * _wBtn , _yBtn);
}
void changeBtn_released(uint8_t btnNr) {
  if (_state != RADIO && _state != CLOCK) tft.drawBmpFile(SD, _releaseBtn[btnNr].c_str(), btnNr * _wBtn , _yBtn);
}
void display_weekdays(uint8_t ad, boolean showall = false) {
  uint8_t i = 0;
  String str = "";
  static uint8_t d, old_d;
  d = ad; //alarmday
  for (i = 0; i < 7; i++) {
    if ((d & (1 << i)) == (old_d & (1 << i)) && !showall) continue; //icon is alread displayed
    str = "/day/" + String(i);
    if (d & (1 << i))  str += "_rt.bmp"; // l<<i instead pow(2,i)
    else            str += "_gn.bmp";
    if (f_SD_okay) tft.drawBmpFile(SD, str.c_str(), 5 + i * 44, 0);
    mp3.loop();
  }
  old_d = ad;
}

void display_alarmtime(int8_t xy = 0, int8_t ud = 0, boolean showall = false) {
  uint8_t i = 0, j[4] = {5, 77, 173, 245}, k[4] = {0, 1, 3, 4}, ch = 0;
  String str = "";
  static int8_t pos = 0, oldpos = 0;;
  static String oldt = "";
  if (ud == 1) {
    ch = _alarmtime[k[pos]]; ch++;
    if (pos == 0) {
      if (_alarmtime[1] > 51) {
        if (ch == 50) ch = 48;  //hour 0...1
        _alarmtime[k[pos]] = ch;
      }
      else {
        if (ch == 51) ch = 48;  //hour 0...2
        _alarmtime[k[pos]] = ch;
      }
    }
    if (pos == 1) {
      if (_alarmtime[0] == '2') {
        if (ch == 52) ch = 48;  //hour*10 0...3
        _alarmtime[k[pos]] = ch;
      }
      else {
        if (ch == 58) ch = 48;  //hour*10 0...9
        _alarmtime[k[pos]] = ch;
      }
    }
    if (pos == 2) {
      if (ch == 54) ch = 48;  //min 0...5
      _alarmtime[k[pos]] = ch;
    }
    if (pos == 3) {
      if (ch == 58) ch = 48;  //min*10 0...9
      _alarmtime[k[pos]] = ch;
    }
  }
  if (ud == -1) {
    ch = _alarmtime[k[pos]]; ch--;
    if (pos == 0) {
      if (_alarmtime[1] > 51) {
        if (ch == 47) ch = 49;  //hour 1...0
        _alarmtime[k[pos]] = ch;
      }
      else {
        if (ch == 47) ch = 50;  //hour 2...0
        _alarmtime[k[pos]] = ch;
      }
    }
    if (pos == 1) {
      if (_alarmtime[0] == '2') {
        if (ch == 47) ch = 51;  //hour*10 0...3
        _alarmtime[k[pos]] = ch;
      }
      else {
        if (ch == 47) ch = 57;  //hour*10 9...0
        _alarmtime[k[pos]] = ch;
      }
    }
    if (pos == 2) {
      if (ch == 47) ch = 53;  //min 5...0
      _alarmtime[k[pos]] = ch;
    }
    if (pos == 3) {
      if (ch == 47) ch = 57;  //min*10 9...0
      _alarmtime[k[pos]] = ch;
    }
  }

  if (xy == 1) pos++; if (pos == 4) pos = 0; //pos only 0...3
  if (xy == -1)pos--; if (pos == -1)pos = 3;

  if (showall == true) {
    oldt = "";
    if (f_SD_okay) {
      tft.drawBmpFile(SD, "/digits/ert.bmp", 149, 45);
      mp3.loop();
    }
  }
  String at = _alarmtime;
  //log_i("at=%s",_alarmtime.c_str());
  if (pos != oldpos) {
    str = "/digits/" + String(at.charAt(k[pos]))   + "or.bmp";
    if (f_SD_okay) {
      tft.drawBmpFile(SD, str.c_str(), j[pos],    45);
      mp3.loop();
    }
    str = "/digits/" + String(at.charAt(k[oldpos])) + "rt.bmp";
    if (f_SD_okay) {
      tft.drawBmpFile(SD, str.c_str(), j[oldpos], 45);
      mp3.loop();
    }
  }
  for (i = 0; i < 4; i++) {
    if (at[k[i]] != oldt[k[i]]) {
      str = "/digits/" + String(at.charAt(k[i]));
      if (i == pos) str += "or.bmp"; //show orange number
      else       str += "rt.bmp"; //show red numbers
      if (f_SD_okay) {
        tft.drawBmpFile(SD, str.c_str(), j[i], 45);
        mp3.loop();
      }
    }
  }
  oldt = at; oldpos = pos;
}

void display_time(boolean showall = false) { //show current time on the TFT Display
  static String t, oldt = "";
  static boolean k = false;
  uint8_t i = 0;
  uint16_t j = 0;

  if (showall == true) oldt = "";
  if ((_state == CLOCK) || (_state == CLOCKico))
  {
    t = gettime_s();
    DBG_OUT_PORT.printf("t ...", t);
    for (i = 0; i < 5; i++)
    {
      if (t[i] == ':')
      {
        if (k == false)
        {
          k = true;
          t[i] = 'd';
        } else {
          t[i] = 'e';
          k = false;
        }
      }
      if (t[i] != oldt[i])
      {
        sprintf(_chbuf, "/digits/%cgn.bmp", t[i]);
        DBG_OUT_PORT.printf("Cbuf ...", _chbuf);
        DBG_OUT_PORT.printf("Sd ok ...", f_SD_okay);
        if (f_SD_okay) tft.drawBmpFile(SD, _chbuf, 5 + j, 45);
      }
      if ((t[i] == 'd') || (t[i] == 'e'))j += 24; else j += 72;
    }
    oldt = t;
  }
}

void display_sleeptime(int8_t ud = 0, boolean ready = false) { // set sleeptimer
  uint8_t p = 0, ypos[4] = {5, 54, 71, 120};
  String   m[] = {"0:00", "0:05", "0:10", "0:15", "0:30", "0:45", "1:00", "2:00", "3:00", "4:00", "5:00", "6:00"};
  uint16_t n[] = {    0,      5,     10,     15,     30,     45,     60,    120,    180,    240,    300,    360 };
  String str = "", color = "rt";
  p = pref.getUInt("sleeptime");
  if (ready == true) {
    _sleeptime = n[p];
    return;
  }
  if (ud == 1) {
    if (p < 11) p++;
    pref.putUInt("sleeptime", p);
  }
  if (ud == -1) {
    if (p > 0) p--;
    pref.putUInt("sleeptime", p);
  }
  if (p == 0) color = "gn";

  String st = m[p];
  str = "/digits/" + String(st.charAt(0)) + "s" + color + ".bmp";
  if (f_SD_okay) {
    tft.drawBmpFile(SD, str.c_str(), ypos[0],    48);
    mp3.loop();
  }
  str = "/digits/ds" + color + ".bmp"; // colon
  if (f_SD_okay) {
    tft.drawBmpFile(SD, str.c_str(), ypos[1],    48);
    mp3.loop();
  }
  str = "/digits/" + String(st.charAt(2)) + "s" + color + ".bmp";
  if (f_SD_okay) {
    tft.drawBmpFile(SD, str.c_str(), ypos[2],    48);
    mp3.loop();
  }
  str = "/digits/" + String(st.charAt(3)) + "s" + color + ".bmp";
  if (f_SD_okay) {
    tft.drawBmpFile(SD, str.c_str(), ypos[3],    48);
    mp3.loop();
  }
}
//**************************************************************************************************
//                                           L O O P                                               *
//**************************************************************************************************
void loop() {
  static uint8_t sec = 0;
  ir.loop();
  tp.loop();
  if (f_1sec == true) {
    if (f_rtc == true) { // true -> rtc has the current time
      int8_t h = 0;
      char tkey[20];
      _time_s = gettime_s();
      if ((f_mute == false) && (!f_sleeping)) {
        if (_time_s.endsWith("59:51")) { // speech the time 9 sec before a new hour is arrived
          _hour = _time_s.substring(0, 2); // extract the hour
          h = _hour.toInt();
          h++;
          if (h == 24) h = 0;
          sprintf ( tkey, "/voice_time/%03d.mp3", h);
          //DBG_OUT_PORT.println(tkey);
          _timefile = 3;
          mp3.connecttoSD(tkey);
        }
      }
      showHeadlineTime();
    }
    display_time();
    if ((f_has_ST == false) && (_state == RADIO)) sec++; else sec = 0; // Streamtitle==""?
    if (sec > 6) {
      sec = 0;
      ST_rep();
    }
    if (_commercial_dur > 0) {
      _commercial_dur--;
      if ((_commercial_dur == 2) && (_state == RADIO))showTitle(""); // end of commercial? clear streamtitle
    }
    f_1sec = false;
  }
  if (f_1min == true) {
    updateSleepTime();

    radio_snd("cli.info", true);
    info();
    migrate();

    showHeadlineVolume(volume);
    showStation(); // if station gives no icy-name display _stationname
    showTitle(_title);


    f_1min = false;
  }
  /* need append
    if (_alarmtime == rtc.gettime_xs()) { //is alarmtime
     if ((_alarmdays >> rtc.getweekday()) & 1) { //is alarmday
       if (!semaphore) {
         f_alarm = true;  //set alarmflag
         f_mute = false;
         semaphore = true;
       }
     }
    }
    else
  */  semaphore = false;

  if (_millis + 5000 < millis()) { //5sec no touch?
    if (_state == RADIOico)  {
      _state = RADIO;
      showTitle(_title);
      showFooter();
    }
    if (_state == RADIOmenue) {
      _state = RADIO;
      showTitle(_title);
      showFooter();
    }
    if (_state == CLOCKico)  {
      display_info("", 160, 79, TFT_BLACK, 0);
      _state = CLOCK;
    }
  }

  if (f_alarm) {
    // log_i("Alarm");
    f_alarm = false;
    mp3.connecttoSD("/ring/alarm_clock.mp3");
    //    mp3.setVolume(21); ///////////////////////////////////////////////////////////////////////////////////////////////
    setTFTbrightness(pref.getUInt("brightness"));
  }

  if (f_mp3eof) {
    if (_timefile > 0) {
      if (_timefile == 1) {
        mp3.connecttoSD("/voice_time/080.mp3");  // stroke
        _timefile--;
      }
      if (_timefile == 2) {
        mp3.connecttoSD("/voice_time/200.mp3");  // precisely
        _timefile--;
      }
      if (_timefile == 3) {
        mp3.connecttoSD("/voice_time/O'clock.mp3");
        _timefile--;
      }
    }
    else {
      changeState(RADIO);
      mp3.connecttohost(_lastconnectedhost);
      //      mp3.setVolume(pref.getUInt("volume")); //////////////////////////////////////////////////////////////////////////
    }
    f_mp3eof = false;
  }
  ArduinoOTA.handle();
}

//**************************************************************************************************
//                                            E V E N T S                                          *
//**************************************************************************************************
void RTIME_info(const char *info) {
  DBG_OUT_PORT.print("rtime_info : ");
  DBG_OUT_PORT.println(info);
}

//Events from tft library
void tft_info(const char *info) {
  DBG_OUT_PORT.print("tft_info   : ");
  DBG_OUT_PORT.print(info);
}
void ir_number(const char* num) {
  if (_state == RADIO) {
    tft.setTextSize(6);
    display_info(num, _yName, _hName + _hTitle, TFT_YELLOW, 100); //state RADIO
  }
}
void ir_key(const char* key) {
  switch (key[0]) {
    case 'k':   if (_state == SLEEP) {
        display_sleeptime(0, true);  //OK
        changeState(RADIO);
      }
      break;
    case 'r':   upvolume(); if ((_state == RADIOico) || (_state == MP3PLAYERico)) showVolumeBar(); // right
      break;
    case 'l':   downvolume(); if ((_state == RADIOico) || (_state == MP3PLAYERico)) showVolumeBar(); // left
      break;
    case 'u':   //if (_state == RADIO) mp3.connecttohost(readnexthostfrompref(true)); // up //////////////////////////////
      if (_state == SLEEP) display_sleeptime(1);
      break;
    case 'd':   //if (_state == RADIO) mp3.connecttohost(readnexthostfrompref(false)); // down ///////////////////////////
      if (_state == SLEEP) display_sleeptime(-1);
      break;
    case '#':   if (_state == SLEEP) changeState(RADIO); // #
      else mute();
      break;
    case '*':   if (_state == RADIO) {
        tft.fillScreen(TFT_BLACK); changeState(SLEEP); showHeadlineItem("* Einschlafautomatik *");
        tft.drawBmpFile(SD, "/Night_Gown.bmp", 198, 25); display_sleeptime();
      }  // *
      break;
    default:    break;
  }
}
// Event from TouchPad
void tp_pressed(uint16_t x, uint16_t y) {
  DBG_OUT_PORT.print("state in tp pressed = "); DBG_OUT_PORT.println(_state);
  //  DBG_OUT_PORT.print("touch y = "); DBG_OUT_PORT.println(y);

  uint8_t yPos = 255, y1Pos = 255, d = 0;
  if (f_sleeping == true) return; // sleepmode, awake in tp_released()
  _millis = millis();
  if (y < 167) {
    if (_state == RADIOico) changeState(RADIOmenue);
    if (_state == RADIO) changeState(RADIOico);
    if (_state == CLOCK) changeState(CLOCKico);
    if (_state == BRIGHTNESS) {}
    if (y < 40) {
      switch (x) { //weekdays
        case   0 ...  48: y1Pos = 0; break; //So
        case  49 ...  92: y1Pos = 1; break; //Mon
        case  93 ... 136: y1Pos = 2; break; //Tue
        case 137 ... 180: y1Pos = 3; break; //We
        case 181 ... 224: y1Pos = 4; break; //Th
        case 225 ... 268: y1Pos = 5; break; //Fri
        case 269 ... 319: y1Pos = 6; break;
      }//Sat
    }
  }
  else {
    switch (x) { // icons
      case   0 ...  63: yPos = 0; break;
      case  64 ... 127: yPos = 1; break;
      case 128 ... 191: yPos = 2; break;
      case 192 ... 255: yPos = 3; break;
      case 256 ... 319: yPos = 4; break;
    }
    changeBtn_pressed(yPos);
  }
  if (_state == RADIOico) {
    if (yPos == 0) {
      mute();
      if (f_mute == false) changeBtn_released(yPos);
    }
    if (yPos == 1) {
      _releaseNr = 1;  // Vol-
      downvolume();
      showVolumeBar();
    }
    if (yPos == 2) {
      _releaseNr = 2;  // Vol+
      upvolume();
      showVolumeBar();
    }
    if (yPos == 3) {
      _releaseNr = 3;
      //mp3.connecttohost(readnexthostfrompref(false)); /////////////////
    }
    if (yPos == 4) {
      _releaseNr = 4;
      //mp3.connecttohost(readnexthostfrompref(true)); ////////////////////
    }
  }
  if (_state == RADIOmenue) {
    if (yPos == 0) {
      _releaseNr = 5;  // MP3
      mp3.stop_mp3client();
      listmp3file();
    }
    if (yPos == 1) {
      _releaseNr = 6; // Clock
    }
    if (yPos == 2) {
      _releaseNr = 7; // Radio
    }
    if (yPos == 3) {
      _releaseNr = 8; // Sleep
    }
    if (yPos == 4) {
      _releaseNr = 16; // Brightness
    }
  }
  if (_state == CLOCKico) {
    if (yPos == 0) {
      _releaseNr = 5;  // MP3
      listmp3file();
    }
    if (yPos == 1) {
      _releaseNr = 9; // Bell
    }
    if (yPos == 2) {
      _releaseNr = 7; // Radio
    }
  }
  if (_state == ALARM) {
    if (yPos == 0) {
      _releaseNr = 11; // left
    }
    if (yPos == 1) {
      _releaseNr = 12; // right
    }
    if (yPos == 2) {
      _releaseNr = 13; // up
    }
    if (yPos == 3) {
      _releaseNr = 14; // down
    }
    if (yPos == 4) {
      _releaseNr = 15; // ready (return to CLOCK)
    }

    if (y1Pos < 7) {
      d = (1 << y1Pos);
      if ((_alarmdays & d))_alarmdays -= d; else _alarmdays += d; display_weekdays(_alarmdays);
    }
  }
  if (_state == BRIGHTNESS) {
    if (yPos == 0) {
      _releaseNr = 17; // left
    }
    if (yPos == 1) {
      _releaseNr = 18; // right
    }
    if (yPos == 2) {
      _releaseNr = 7; // ready (return to RADIO)
    }
  }
  if (_state == MP3PLAYER) {
    if (yPos == 0) {
      _releaseNr = 10; // Radio
    }
    if (yPos == 1) {
      _releaseNr = 21; // left
    }
    if (yPos == 2) {
      _releaseNr = 22; // right
    }
    if (yPos == 3) {
      _releaseNr = 23; // ready
    }
  }
  if (_state == MP3PLAYERico) {
    if (yPos == 0) {
      mute();
      if (f_mute == false) changeBtn_released(yPos);
    }
    if (yPos == 1) {
      _releaseNr = 1;  // Vol-
      downvolume();
      showVolumeBar();
    }
    if (yPos == 2) {
      _releaseNr = 2;  // Vol+
      upvolume();
      showVolumeBar();
    }
    if (yPos == 3) {
      _releaseNr = 26; // MP3
    }
    if (yPos == 4) {
      _releaseNr = 10;  // Radio
      _title = "";
      changeState(RADIO);
      mp3.connecttohost(_lastconnectedhost);
    }
  }
  if (_state == SLEEP) {
    if (yPos == 0) {
      _releaseNr = 19; // sleeptime up
    }
    if (yPos == 1) {
      _releaseNr = 20; // sleeptime down
    }
    if (yPos == 2) {
      _releaseNr = 7;  // ready, return to RADIO
      display_sleeptime(0, true);
    }
    if (yPos == 4) {
      _releaseNr = 7; // return to RADIO without saving sleeptime
    }
  }
}

void tp_released() {
  static String str = "";
  if (f_sleeping == true)
  { //awake
    setTFTbrightness(pref.getUInt("brightness"));   // restore brightness
    //    mp3.setVolume(pref.getUInt("volume"));          // restore volume ///////////////////////////////////////////////////////
    f_sleeping = false;
    return;
  }

  switch (_releaseNr) {
    case  1: changeBtn_released(1); break; // Vol-
    case  2: changeBtn_released(2); break; // Vol+
    case  3: changeBtn_released(3); break; // nextstation
    case  4: changeBtn_released(4); break; // previousstation
    case  5: tft.fillScreen(TFT_BLACK);
      changeState(MP3PLAYER); showHeadlineItem("* MP3Player *");
      tft.setTextSize(4); str = _mp3Name[_mp3Index];
      str = str.substring(str.lastIndexOf("/") + 1, str.length() - 5); //only filename, get rid of foldername(s) and suffix
      display_info(ASCIItoUTF8(str.c_str()), _yName, _hName, TFT_CYAN, 5); break; //MP3
    case  6: tft.fillScreen(TFT_BLACK); changeState(CLOCK);
      showHeadlineItem("** Clock **"); display_time(true); break;//Clock
    case  7: changeState(RADIO); break;
    case  8: tft.fillScreen(TFT_BLACK); changeState(SLEEP); showHeadlineItem("* SleepTimer *");
      tft.drawBmpFile(SD, "/Night_Gown.bmp", 198, 25); display_sleeptime(); break;
    case  9: changeState(ALARM); showHeadlineItem("");
      display_weekdays(_alarmdays, true);
      display_alarmtime(0, 0, true); break;
    case 10: _title = ""; changeState(RADIO); mp3.connecttohost(_lastconnectedhost); break;
    case 11: display_alarmtime(-1);    changeBtn_released(0);  break;
    case 12: display_alarmtime(+1);    changeBtn_released(1);  break;
    case 13: display_alarmtime(0, +1); changeBtn_released(2);  break; // alarmtime up
    case 14: display_alarmtime(0, -1); changeBtn_released(3);  break; // alarmtime down
    case 15: pref.putUInt("alarm_weekday", _alarmdays); // ready
      pref.putString("alarm_time", _alarmtime);
      tft.fillScreen(TFT_BLACK); changeState(CLOCK);
      showHeadlineItem("** Clock **");
      display_time(true); break;//Clock
    case 16: tft.fillScreen(TFT_BLACK); changeState(BRIGHTNESS); showHeadlineItem("** Brightness **");
      showBrightnessBar(); mp3.loop();
      tft.drawBmpFile(SD, "/Brightness.bmp", 0, 21); break;
    case 17: changeBtn_released(0); downBrightness(); showBrightnessBar(); break;
    case 18: changeBtn_released(1); upBrightness(); showBrightnessBar(); break;
    case 19: display_sleeptime(1);  changeBtn_released(0); break;
    case 20: display_sleeptime(-1); changeBtn_released(1); break;
    case 21: changeBtn_released(1); _mp3Index--; if (_mp3Index == -1) _mp3Index = 9;
      str = _mp3Name[_mp3Index];
      while (str.length() == 0) {
        _mp3Index--;
        str = _mp3Name[_mp3Index];
        if (_mp3Index == 0) break;
      }
      str = str.substring(str.lastIndexOf("/") + 1, str.length() - 5); //only filename, get rid of foldername(s) and suffix
      tft.setTextSize(4);
      display_info(ASCIItoUTF8(str.c_str()), _yName, _hName, TFT_CYAN, 5);
      break; // left file--
    case 22: changeBtn_released(2); _mp3Index++; if (_mp3Index > 9) _mp3Index = 0;
      str = _mp3Name[_mp3Index];
      if (str.length() == 0) {
        _mp3Index = 0;
        str = _mp3Name[_mp3Index];
      }
      str = str.substring(str.lastIndexOf("/") + 1, str.length() - 5); //only filename, get rid of foldername(s) and suffix
      tft.setTextSize(4);
      display_info(ASCIItoUTF8(str.c_str()), _yName, _hName, TFT_CYAN, 5);
      break; // right file++
    case 23: changeState(MP3PLAYERico); showVolumeBar();
      mp3.connecttoSD("/" + _mp3Name[_mp3Index]); break; // play mp3file
    case 26: clearTitle(); clearFooter(); changeState(MP3PLAYER); break;
  }
  _releaseNr = 0;
}


//---------------------------------------Client
void radio_snd (String cmd, bool rcv)
{

  if (millis() - telnet_time > 30000)
  {
    DBG_OUT_PORT.println("\n Start communication over telnet");
    String out = "\n No connect with Radio";
    WiFiClient client;
    const int port = 23;

    if (!client.connect(radio_addr, port))
    {
      client.stop();
      out = "\n connection failed";
    }
    else
    {
      client.print(cmd + "\r\n");
      if (rcv)
      {
        //DBG_OUT_PORT.println("\n Start rcv");
        char tmp = client.read();
        unsigned long st_time = millis();
        while (tmp != '\r' && millis() - st_time < 2000 )
        {
          tmp = client.read();
          switch (tmp)
          {
            case '\n' :
              if (_index == 0) break;

            case '\r' :
              line[_index] = 0; // end of string
              _index = 0;
              parse_k(line);
              break;
            case 0xFF :
              break;

            default : // put the received char in line

              if (tmp < 0xFF ) line[_index++] = tmp;
              if (_index > BUFLEN - 1) // next line
              {
                DBG_OUT_PORT.println(F("overflow"));
                line[_index] = 0;
                parse_k(line);
                _index = 0;
              }
          }
          delay(2);
        }
      }
      //DBG_OUT_PORT.println("\n End rcv");
      client.stop();
      out = "\n Sussecs end communication over telnet";
    }
    DBG_OUT_PORT.println(out);
    telnet_time = millis();
  }
}

////////////////////////////////////////
// parse the karadio received line and do the job
void parse_k(char* line)
{
  //DBG_OUT_PORT.println("\n Start parsing");
  char* ici;
  removeUtf8((byte*)line);

  //DBG_OUT_PORT.printf("\n Inline %s\n", line);

  ////// Meta title
  if ((ici = strstr(line, "META#: ")) != NULL)
  {
    strcpy(title, ici + 7);
    //DBG_OUT_PORT.printf("\n Title ...%s\n", title);
    askDraw = true;
  }
  else
    ////// ICY5 Bitrate
    if ((ici = strstr(line, "ICY5#: ")) != NULL)
    {
      _bitrate = atoi(ici + 7);
      //DBG_OUT_PORT.printf("\n Bitrate ...%s\n", _bitrate);
    }
    else
      ////// ICY4 Description
      if ((ici = strstr(line, "ICY4#: ")) != NULL)
      {
        strcpy(genre, ici + 7);
        //DBG_OUT_PORT.printf("\n Genree ...%s\n", genre);
      }
      else
        ////// ICY0 station name
        if ((ici = strstr(line, "ICY0#: ")) != NULL)
        {
          if (strlen(ici + 7) == 0) strcpy (station, nameset);
          else strcpy(station, ici + 7);
          //DBG_OUT_PORT.printf("\n Station name ...%s\n", station);
        }
        else
          ////// STOPPED
          if ((ici = strstr(line, "STOPPED")) != NULL)
          {
            strcpy(title, "STOPPED");
          }
          else
            //////Nameset
            if ((ici = strstr(line, "NAMESET#: ")) != NULL)
            {
              strcpy(nameset, ici + 9);
              //DBG_OUT_PORT.printf("\n Nameset ...%s\n", nameset);
              strncat(_sta_num, nameset, 4);
              //DBG_OUT_PORT.printf("\n Station number ...%s\n", _sta_num);
            }
            else
              //////Playing
              if ((ici = strstr(line, "PLAYING#")) != NULL)
              {
                if (strcmp(title, "STOPPED") == 0)
                {
                  //;
                }
              }
              else
                //////Volume
                if ((ici = strstr(line, "VOL#:")) != NULL)
                {
                  volume = atoi(ici + 6);
                  askDraw = true;;
                  //DBG_OUT_PORT.printf("\n Volume ...%03d\n", volume);
                }
                else
                  //////Date Time  ##SYS.DATE#: 2017-04-12T21:07:59+01:00
                  if ((ici = strstr(line, "SYS.DATE#:")) != NULL)
                  {
                    if (*(ici + 11) != '2') //// invalid date. try again later
                    {
                      askDraw = true;
                      return;
                    }
                    char lstr[30];
                    strcpy(lstr, ici + 11);

                    tmElements_t dt;
                    breakTime(now(), dt); //Записываем в структуру dt (содержащую элементы час минута секунда год) текущее время в контроллере (в дурине)
                    int year, month, day, hour, minute, second; //объявляем переменные под год месяц день недели и.т.д
                    sscanf(lstr, "%04d-%02d-%02dT%02d:%02d:%02d", &(year), &(month), &(day), &(hour), &(minute), &(second)); //переносим (разбираем) строчку с датой на отдельные кусочки (день месяц год и.т.д)
                    dt.Year = year - 1970; dt.Month = month; dt.Day = day; //заменяем кусочки структуры dt значениями из нашей принятой и разобранной строки с датой и временем
                    dt.Hour = hour; dt.Minute = minute; dt.Second = second;
                    setTime(makeTime(dt)); //записываем в timestamp(штамп/оттиск времени в формате UNIX time (количество секунд с 1970 года) значение времени сформированное в структуре dt
                    syncTime = true;
                    //DBG_OUT_PORT.printf("\n Current time is %02d:%02d:%02d  %02d-%02d-%04d\n", hour, minute, second, day, month, year);
                  }
  //DBG_OUT_PORT.println("\n End parsing");
}

////////////////////////////////////////
void removeUtf8(byte *characters)
{
  int index = 0;
  while (characters[index])
  {
    if ((characters[index] >= 0xc2) && (characters[index] <= 0xc3)) // only 0 to FF ascii char
    {
      //      DBG_OUT_PORT.println((characters[index]));
      characters[index + 1] = ((characters[index] << 6) & 0xFF) | (characters[index + 1] & 0x3F);
      int sind = index + 1;
      while (characters[sind]) {
        characters[sind - 1] = characters[sind];
        sind++;
      }
      characters[sind - 1] = 0;

    }
    index++;
  }
}

////////////////////////////////////////
void askTime()
{
  if (itAskTime) // time to ntp. Don't do that in interrupt.
  {
    radio_snd("sys.date", true);
    itAskTime = false;
  }
}

void info()
{
  //DBG_OUT_PORT.printf("\nNameset ...%s\n", nameset);
  DBG_OUT_PORT.printf("\nStation number ...%s\n", _sta_num);
  DBG_OUT_PORT.printf("Station name ...%s\n", station);
  DBG_OUT_PORT.printf("Genree ...%s\n", genre);
  DBG_OUT_PORT.printf("Title ...%s\n", title);
  DBG_OUT_PORT.printf("Volume ...%03d\n", volume);

  tmElements_t dt;
  breakTime(now(), dt); //Current time (in MC) -> dt
  DBG_OUT_PORT.printf("Current time is %02d:%02d:%02d  %02d-%02d-%04d\n", dt.Hour, dt.Minute, dt.Second, dt.Day, dt.Month, dt.Year + 1970);
}

const char* gettime_s() { // hh:mm:ss
  tmElements_t dt;
  breakTime(now(), dt); //Current time (in MC) -> dt
  String s = String(dt.Hour) + ':' + String(dt.Minute) + ':' + String(dt.Second);
  char const *pchar = s.c_str();  //use char const* as target type
  DBG_OUT_PORT.printf("Current time is \n", pchar);
  return pchar;
}


void migrate()
{
  _title = title;
  _stationname = station;
  _station = nameset;
  _myIP = WiFi.localIP().toString();
  f_rtc = syncTime;
  //  _SSID = WiFi.SSID();
}
//////////////////////////////////////////// WiFi
//-------------------------------------------------------------- Callback
/*
  void configModeCallback(WiFiManager *myWiFiManager)
  {
  DBG_OUT_PORT.print("\n Entered config mode, IP is...");
  DBG_OUT_PORT.println(WiFi.localIP());

  DBG_OUT_PORT.print("\n Connected to...");
  DBG_OUT_PORT.println(WiFi.SSID());
  }
*/

//-------------------------------------------------------------- Start_wifi
void start_wifi()
{
  WiFiManager wm;
  // wm.resetSettings();
  //wm.setConfigPortalBlocking(false);
  //  wm.setAPCallback(configModeCallback);
  wm.setConfigPortalTimeout(60);
  wm.autoConnect("Addon", "12345678");
}

//------------------------------------------------------------- OTA
void OTA_init()
{
  ArduinoOTA
  .onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH)
      type = "sketch";
    else // U_SPIFFS
      type = "filesystem";

    // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
    DBG_OUT_PORT.println("Start updating " + type);
  })
  .onEnd([]() {
    DBG_OUT_PORT.println("\nEnd");
  })
  .onProgress([](unsigned int progress, unsigned int total) {
    DBG_OUT_PORT.printf("Progress: %u%%\r", (progress / (total / 100)));
  })
  .onError([](ota_error_t error) {
    DBG_OUT_PORT.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) DBG_OUT_PORT.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) DBG_OUT_PORT.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) DBG_OUT_PORT.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) DBG_OUT_PORT.println("Receive Failed");
    else if (error == OTA_END_ERROR) DBG_OUT_PORT.println("End Failed");
  });

  ArduinoOTA.begin();

  DBG_OUT_PORT.print("OTA ready with IP address: ");
  DBG_OUT_PORT.println(WiFi.localIP());
}
