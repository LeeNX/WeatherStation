/*****************************************
 accurite 5n1 weather station decoder
  
  for arduino and 433 MHz OOK RX module
  Note: use superhet (with xtal) rx board
  the regen rx boards are too noisy
 Jens Jensen, (c)2015
*****************************************/
#include <EEPROM.h>
#include <PGMWrap.h>
#include <RingBuf.h>

// pulse timings
// SYNC
#define SYNC_HI      675
#define SYNC_LO      575

// HIGH == 1
#define LONG_HI      450
#define LONG_LO      375

// SHORT == 0
#define SHORT_HI     250
#define SHORT_LO     175

#define RESETTIME    10000

// other settables
#define LED          13
#define PIN           2  // data pin from 433 RX module
#define MAXBITS      65  // max framesize

#define DEBUG         1  // uncomment to enable debugging
#define DEBUGPIN     A0  // pin for triggering logic analyzer
#define METRIC_UNITS  0  // select display of metric or imperial units

// sync states
#define RESET     0   // no sync yet
#define INSYNC    1   // sync pulses detected 
#define SYNCDONE  2   // complete sync header received 

volatile unsigned int    pulsecnt = 0; 
volatile unsigned long   risets = 0;     // track rising edge time
volatile unsigned int    syncpulses = 0; // track sync pulses
volatile byte            state = RESET;  
 byte            buf[8] = {0,0,0,0,0,0,0,0};  // processing message frame buffer
 byte            recBuf[8] = {0,0,0,0,0,0,0,0}; // Receive msg frame buffer

RingBuf *rngBuf = RingBuf_new(sizeof(buf),5);

unsigned int   raincounter = 0;
unsigned int   EEMEM raincounter_persist;    // persist raincounter in eeprom
#define  MARKER  0x5AA5
unsigned int   EEMEM eeprom_marker = MARKER; // indicate if we have written to eeprom or not before

// wind directions:
// { "NW", "WSW", "WNW", "W", "NNW", "SW", "N", "SSW",
//   "ENE", "SE", "E", "ESE", "NE", "SSE", "NNE", "S" };
const float winddirections[] = { 315.0, 247.5, 292.5, 270.0, 
                                 337.5, 225.0, 0.0, 202.5,
                                 67.5, 135.0, 90.0, 112.5,
                                 45.0, 157.5, 22.5, 180.0 };
const char channelID[] = {'C','D','B','A'};
const int uploadDevice[2]  PROGMEM = {0x1D,0x20}; //Device ID and Channel of device you want to upload
// wx message types
#define  MT_WS_WD_RF  49    // wind speed, wind direction, rainfall
#define  MT_WS_T_RH   56    // wind speed, temp, RH


//WundergroundStuff

//const String WUnderURL PROGMEM = "https://rtupdate.wunderground.com/weatherstation/updateweatherstation.php?action=updateraw&dateutc=now&realtime=1&rtfreq=15";
//const String WunderOpts[] PROGMEM = {"&ID=","&PASSWORD=","&winddir=","&windspeedmph=","&windgustmph=","&humidity=","&tempf=","&rainin=","&baromin=","&dewptf="}; //First 2 are Station ID and Key
//String WunderVal[9];



void setup() {
  // put your setup code here, to run once:
  Serial.begin(9600); 
  Serial.println(F("Starting Acurite5n1 433 WX Decoder v0.2 ..."));
  pinMode(PIN, INPUT);
  raincounter = getRaincounterEEPROM();
  #ifdef DEBUG
    // setup a pin for triggering logic analyzer for debugging pulse train
    pinMode(DEBUGPIN, OUTPUT);
    digitalWrite(DEBUGPIN, HIGH);
  #endif
  attachInterrupt(0, My_ISR, CHANGE);

}

int freeRam () {
  extern int __heap_start, *__brkval; 
  int v; 
  return (int) &v - (__brkval == 0 ? (int) &__heap_start : (int) __brkval); 
}

void loop() {


  
  if (!rngBuf->isEmpty(rngBuf))
  {
    rngBuf->pull(rngBuf, &buf);
   
    if (acurite_crc(buf, sizeof(buf))) {
      // passes crc, good message 
      #ifdef DEBUG
        print_CRC(F("5n1 CRC"));
      #endif
      
      digitalWrite(LED, HIGH);     
      //identifyDevice();
      float windspeedkph = getWindSpeed(buf[3], buf[4]);
      //WunderVal[3] = String(convKphMph(windspeedkph),1);  //WindSpeed
      Serial.print("windspeed: ");
      if (METRIC_UNITS) {
        Serial.print(windspeedkph, 1);
        Serial.print(" km/h, ");
      } else {
        Serial.print(convKphMph(windspeedkph),1);
        Serial.print(" mph, ");
      }       

      int msgtype = (buf[2] & 0x3F);
      if (msgtype == MT_WS_WD_RF) {
        // wind speed, wind direction, rainfall
        float rainfall = 0.00;
        unsigned int curraincounter = getRainfallCounter(buf[5], buf[6]);
        updateRaincounterEEPROM(curraincounter);
        if (raincounter > 0) {
          // track rainfall difference after first run
          rainfall = (curraincounter - raincounter) * 0.01;
        } else {
          // capture starting counter
          raincounter = curraincounter; 
        }

        float winddir = getWindDirection(buf[4]);
        //WunderVal[2] = String(winddir,0);  //WindDirection
        Serial.print("wind direction: ");
        Serial.print(winddir, 1);
        Serial.print(", rain gauge: ");
        if (METRIC_UNITS) {
          Serial.print(convInMm(rainfall), 1);
          Serial.print(" mm");
        } else {
          Serial.print(rainfall, 2);
          Serial.print(" inches");
        }
       // WunderVal[7] = String(rainfall,2);  //RainFall
      } else if (msgtype == MT_WS_T_RH) {
        TempAndHumid(false);
      } else {
        #ifdef DEBUG
          print_CRC(F("Unknown MsgType"));
        #endif
      }
      // time
      unsigned int timesincestart = millis()/60/1000;
      Serial.print(", mins since start: ");
      Serial.print(timesincestart);     
      Serial.println();
      
    } 
    else if (acurite_crc(buf, sizeof(buf)-1)) {
      #ifdef DEBUG
        print_CRC(F("Small CRC"));
      #endif
     identifyDevice();
     TempAndHumid(true);
       } 
   else  {
      // failed CRC
      #ifdef DEBUG
        print_CRC(F("CRC BAD"));
      #endif    
    }

    digitalWrite(LED, LOW);
  }

  //delay(100);
}

void print_CRC(String status){
  int i;
  for (i=0; i<8; i++) {
    Serial.print(buf[i],HEX);
    Serial.print(" ");
  }
  Serial.println(status);
}

bool isUploadDevice()
{
  int deviceID[2];
  deviceID[0] = buf[0] & 63;
  deviceID[1] = buf[1];
  if (memcmp_P(deviceID , uploadDevice,2) == 0)
    return true;
  else
    return false;
  
}

void identifyDevice()
{
  Serial.print("Device ID: ");
  Serial.print(buf[0] & 63,HEX);
  Serial.print(buf[1],HEX);
  Serial.print(" Channel: ");
  Serial.print(channelID[buf[0]>>6]);
  Serial.print(" ");
}

bool acurite_crc(volatile byte row[], int cols) {
      // sum of first n-1 bytes modulo 256 should equal nth byte
      cols -= 1; // last byte is CRC
        int sum = 0;
      for (int i = 0; i < cols; i++) {
        sum += row[i];
      }    
      if (sum != 0 && sum % 256 == row[cols]) {
        return true;
      } else {
        return false;
      }
}

bool wundergrnd_send()
{
//  String request = WUnderURL;
//  for (int i=0; i < sizeof(WunderOpts); i++)
//  {
//    request += WunderOpts[i];
//    request += WunderVal[i];
//  }
//
//  Serial.print(request);
  
}

void TempAndHumid(bool smallDevice)
{
  // Tempature and Humidity
  float tempf = getTempF(buf[4], buf[5]);
  bool batteryok = ((buf[2] & 0x40) >> 6);
  int humidity;
  
  if (smallDevice) {
    humidity = getHumidity(buf[3]);
  } else {
    humidity = getHumidity(buf[6]);
  }
   
  Serial.print("temp: ");
  if (METRIC_UNITS) {
    Serial.print(convFC(tempf), 1);
    Serial.print(" C, ");
  } else {
    Serial.print(tempf, 1);
    Serial.print(" F, ");
  }
 // WunderVal[6] = String(tempf,1);  //Tempature
  Serial.print("humidity: ");
  Serial.print(humidity);
  //WunderVal[5] = String(humidity,0);  //Humidity
  Serial.print(" %RH, battery: ");
  if (batteryok) {
    Serial.print("OK");
  } else {
    Serial.print("LOW");
  }
  return;
}

float getTempF(byte hibyte, byte lobyte) {
  // range -40 to 158 F
  int highbits = (hibyte & 0x0F) << 7;
  int lowbits = lobyte & 0x7F;
  int rawtemp = highbits | lowbits;
  float temp = (rawtemp - 400) / 10.0;
  return temp;
}

float getWindSpeed(byte hibyte, byte lobyte) {
  // range: 0 to 159 kph
  int highbits = (hibyte & 0x7F) << 3;
  int lowbits = (lobyte & 0x7F) >> 4;
  float speed = highbits | lowbits;
  // speed in m/s formula according to empirical data
  if (speed > 0) {
    speed = speed * 0.23 + 0.28;
  }
  float kph = speed * 60 * 60 / 1000;
  return kph;
}

float getWindDirection(byte b) {
  // 16 compass points, ccw from (NNW) to 15 (N), 
        // { "NW", "WSW", "WNW", "W", "NNW", "SW", "N", "SSW",
        //   "ENE", "SE", "E", "ESE", "NE", "SSE", "NNE", "S" };
  int direction = b & 0x0F;
  return winddirections[direction];
}

int getHumidity(byte b) {
  // range: 1 to 99 %RH
  int humidity = b & 0x7F;
  return humidity;
}

int getRainfallCounter(byte hibyte, byte lobyte) {
  // range: 0 to 99.99 in, 0.01 increment rolling counter
  int raincounter = ((hibyte & 0x7f) << 7) | (lobyte & 0x7F);
  return raincounter;
}

float convKphMph(float kph) {
  return kph * 0.62137;
}

float convFC(float f) {
  return (f-32) / 1.8;
}

float convInMm(float in) {
  return in * 25.4;
}

unsigned int getRaincounterEEPROM() {
  unsigned int oldraincounter = 0;
  unsigned int marker = eeprom_read_word(&eeprom_marker);
  #ifdef DEBUG 
    Serial.print("marker: ");
    Serial.print(marker, HEX);
  #endif
  if (marker == MARKER) {
    // we have written before, use old value
    oldraincounter = eeprom_read_word(&raincounter_persist);
    #ifdef DEBUG
      Serial.print(", raincounter_persist raw value: ");
      Serial.println(raincounter, HEX);
    #endif 
  } 
  return oldraincounter;
}

void updateRaincounterEEPROM(unsigned int raincounter) {
  eeprom_update_word(&raincounter_persist, raincounter);
  eeprom_update_word(&eeprom_marker, MARKER); // indicate first write
  #ifdef DEBUG
    Serial.print("updateraincountereeprom: ");
    Serial.print(eeprom_read_word(&raincounter_persist), HEX);
    Serial.print(", eeprommarker: ");
    Serial.print(eeprom_read_word(&eeprom_marker), HEX);
    Serial.println();
  #endif
}

void My_ISR()
{
  // decode the pulses
  unsigned long timestamp = micros();
  
  if (digitalRead(PIN) == HIGH) {
    // going high, start timing
    if (timestamp - risets > RESETTIME) {
      // detect reset condition
      state=RESET;
      syncpulses=0;
      pulsecnt=0;
    }
    risets = timestamp;
    return;
  }

  // going low
  unsigned long duration = timestamp - risets;

  if (state == RESET || state == INSYNC) {
    // looking for sync pulses
    if ((SYNC_LO) < duration && duration < (SYNC_HI))  {
      // start counting sync pulses
      state=INSYNC;
      syncpulses++;
      if (syncpulses > 3) {
        // found complete sync header
        state = SYNCDONE;
        syncpulses = 0;
        pulsecnt=0;
        
        #ifdef DEBUG
          // quick debug to trigger logic analyzer at sync
          digitalWrite(DEBUGPIN, LOW);
        #endif
      }
      return; 
      
    } else { 
      // not interested, reset  
      syncpulses=0;
      pulsecnt=0;
      state=RESET;
      #ifdef DEBUG
        digitalWrite(DEBUGPIN, HIGH); //return trigger
      #endif
      return; 
    }
  } else {
    
    // SYNCDONE, now look for message 
    // detect if finished here
    if ( pulsecnt > MAXBITS ) {
      noInterrupts();
      state = RESET;
      pulsecnt = 0;
      if (!rngBuf->isFull(rngBuf)) {
        rngBuf->add(rngBuf, &recBuf);
      }
      interrupts();
      return;
    }
    // stuff buffer with message
    
    byte bytepos = pulsecnt / 8;
    byte bitpos = 7 - (pulsecnt % 8); // reverse bitorder
    if ( LONG_LO < duration && duration < LONG_HI) {
      bitSet(buf[bytepos], bitpos);
      pulsecnt++;
    }
    else if ( SHORT_LO < duration && duration < SHORT_HI) {
    
      bitClear(buf[bytepos], bitpos);
      pulsecnt++;
    }
  
  }
}
void TestISR()
{
  int i;
  char readbuf[16];
  Serial.println("Awaiting input");
  Serial.setTimeout(60000);
  Serial.readBytes(readbuf,16);

  for (i=0;i<16;i+=2)
    {
    buf[i/2]=x2i(&readbuf[i]);
    }
  
  Serial.print("Value : ");
  
  for (i=0;i<8;i++)
  {
   Serial.print(buf[i],HEX);
   Serial.print(" ");
  }

}

int x2i(char *s) 
{
 int i;
 int x = 0;
 for(i=0;i<2;i++) {
   char c = *s;
   Serial.print(c);
   if (c >= '0' && c <= '9') {
      x *= 16;
      x += c - '0'; 
   }
   else if (c >= 'A' && c <= 'F') {
      x *= 16;
      x += (c - 'A') + 10; 
   }
   else if (c >= 'a' && c <= 'f') {
      x *= 16;
      x += (c - 'a') + 10;   
   }
   else break;
   s++;
 }
  Serial.println();
 return x;
}
