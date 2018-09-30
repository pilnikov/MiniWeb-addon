#include "mp3.h"


MP3::MP3() {}

MP3::~MP3() {}

void     MP3::stop_mp3client(){}
void     MP3::setVolume(uint8_t vol){}                    // Set the player volume.Level from 0-21, higher is louder.
uint8_t  MP3::getVolume(){}                               // Get the current volume setting, higher is louder.
bool     MP3::connecttohost(String host){}
bool     MP3::connecttoSD(String sdfile){}
bool     MP3::connecttospeech(String speech, String lang){}
void     MP3::loop(){}
 
