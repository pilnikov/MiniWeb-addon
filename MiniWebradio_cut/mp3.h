
#ifndef _mp3
#define _mp3

#include "Arduino.h"
#include "SPI.h"
#include "SD.h"
#include "FS.h"

class MP3
{
  public:
    MP3();                                              // Constructor
    ~MP3();

    void     stop_mp3client();
    void    loop();
    bool     connecttohost(String host);
    bool	   connecttoSD(String sdfile);
    bool     connecttospeech(String speech, String lang);
} ;

#endif
