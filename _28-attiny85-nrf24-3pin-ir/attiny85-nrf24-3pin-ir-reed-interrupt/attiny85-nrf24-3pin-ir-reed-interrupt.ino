// 5pin
/* 24l01    85
   1  gnd   4
   2  vcc   8
   3  ce    1
   4  csn   3
   5  sck   7
   6  mosi  6
   7  miso  5
*/

// 3pin
/* 24l01    85
   1  gnd   4
   2  vcc   8
   3  ce    x
   4  csn   x
   5  sck   7
   6  mosi  6
   7  miso  5
*/
#include <LowPower.h>
#include <TimeLib.h>
#include <avr/wdt.h>
#include <avr/interrupt.h>
#include <avr/sleep.h>
#include <avr/pgmspace.h>
//#include <avr/power.h>
#include <nRF24L01.h>
#include <RF24.h>

#define adc_disable() (ADCSRA &= ~(1<<ADEN)) // disable ADC
#define adc_enable()  (ADCSRA |=  (1<<ADEN)) // re-enable ADC

// http://www.gammon.com.au/forum/?id=12769
#if defined(__AVR_ATtiny85__)
#define watchdogRegister WDTCR
#else
#define watchdogRegister WDTCSR
#endif

#define _3PIN
//#define _5PIN

#ifdef _3PIN
// 3pin
#define CE_PIN 3
#define CSN_PIN 3
#else
// 5pin
#define CE_PIN 5
#define CSN_PIN 4
#endif

#define DEVICE_ID 65
#define CHANNEL 100

const uint64_t pipes[1] = { 0xFFFFFFFFFFLL };

struct {
  uint32_t _salt;
  uint16_t volt;
  int16_t data1;
  int16_t data2;
  uint8_t devid;
} payload;

struct {
  uint32_t timestamp;
} time_ackpayload;

struct {
  uint32_t timestamp;
} time_reqpayload;

RF24 radio(CE_PIN, CSN_PIN);

#define IRENPIN 4 // p3 / A2
#define DATA1PIN 3 // p2 // A3

int16_t doorStatus;
int16_t rollStatus;

const int min_hour   = 8;
const int min_minute = 0;

void setup() {
  delay(100);
  time_reqpayload.timestamp = 0;
  time_ackpayload.timestamp = 0;

  adc_disable();
  unsigned long startmilis = millis();

  pinMode(DATA1PIN, INPUT);
  pinMode(IRENPIN, OUTPUT);
  digitalWrite(IRENPIN, LOW);

  radio.begin();
  radio.setChannel(CHANNEL);
  radio.setPALevel(RF24_PA_LOW);
  radio.setDataRate(RF24_1MBPS);
  radio.setRetries(15, 15);
  radio.setAutoAck(true);
  radio.enableAckPayload();
  radio.enableDynamicPayloads();
  radio.openWritingPipe(pipes[0]);
  radio.powerDown();

  unsigned long stopmilis = millis();
  payload.data2 = ( stopmilis - startmilis ) * 10 ;

  while (digitalRead(DATA1PIN) == 1 ) {
    LowPower.powerDown(SLEEP_1S, ADC_OFF, BOD_OFF);
  }
  
  payload.data1 = rollStatus = digitalRead(DATA1PIN) * 10 ;
  payload._salt = 0;
  payload.volt  = readVcc();
  payload.devid = DEVICE_ID;

  // get time
  setSyncProvider( requestSync);
  radio.powerUp();
  getNrfTime();
  radio.write(&payload , sizeof(payload));
  radio.powerDown();
}

void loop() {
  pinint_sleep();
  goToSleep ();
  doorStatus = digitalRead(DATA1PIN);
  if (doorStatus == 1 ) {
    return;
  } else {
    radio.powerUp();
    getNrfTime();
    radio.powerDown();

    uint32_t alarmtime = numberOfSecondsSinceEpoch(year(), month(), day(), min_hour, min_minute, 0);
    if (alarmtime >= (now() + 8)) {
      int16_t timediff = ( alarmtime - now() ) / 8;
      timedsleep(timediff);
    }

    unsigned long startmilis = millis();
    payload._salt++;
    payload.volt = readVcc();

    digitalWrite(IRENPIN, HIGH);
    delay(1);
    payload.data1 = digitalRead(DATA1PIN) * 10;
    digitalWrite(IRENPIN, LOW);

    if ( rollStatus != payload.data1 ) {
      radio.powerUp();
      radio.write(&payload , sizeof(payload));
      radio.powerDown();
      rollStatus = payload.data1;
    }
    
    unsigned long stopmilis = millis();
    payload.data2 = ( stopmilis - startmilis ) * 10 ;
  }
}

void goToSleep () {
  set_sleep_mode (SLEEP_MODE_PWR_DOWN);
  noInterrupts ();       // timed sequence coming up
  // pat the dog
  wdt_reset();

  // clear various "reset" flags
  MCUSR = 0;
  // allow changes, disable reset, clear existing interrupt
  watchdogRegister = bit (WDCE) | bit (WDE) | bit (WDIF);
  // set interrupt mode and an interval (WDE must be changed from 1 to 0 here)
  watchdogRegister = bit (WDIE) | bit (WDP2) | bit (WDP1) | bit (WDP0);    // set WDIE, and 2 seconds delay

  sleep_enable ();       // ready to sleep
  interrupts ();         // interrupts are required now
  sleep_cpu ();          // sleep
  sleep_disable ();      // precaution
}  // end of goToSleep

/*
  void goToSleep () {
  LowPower.powerDown(SLEEP_2S, ADC_OFF, BOD_OFF);
  }
*/

ISR(PCINT0_vect) {
  cli();
  PCMSK &= ~_BV(PCINT3);
  sleep_disable();
  sei();
}

void pinint_sleep() {
  PCMSK |= _BV(PCINT3);
  GIMSK |= _BV(PCIE);
  set_sleep_mode(SLEEP_MODE_PWR_DOWN);
  sleep_enable();
  sei();
  sleep_cpu();
}

void timedsleep(int16_t n) {
  for (int i = 0; i < n; i++) {
    LowPower.powerDown(SLEEP_8S, ADC_OFF, BOD_OFF);
  }
}

int readVcc() {
  adc_enable();
  ADMUX = _BV(MUX3) | _BV(MUX2);

  delay(2);
  ADCSRA |= _BV(ADSC); // Start conversion
  while (bit_is_set(ADCSRA, ADSC)); // measuring

  uint8_t low  = ADCL; // must read ADCL first - it then locks ADCH
  uint8_t high = ADCH; // unlocks both

  long result = (high << 8) | low;

  //result = 1126400L / result; // Calculate Vcc (in mV);
  result = 1074835L / result;

  //Disable ADC
  adc_disable();

  return (int)result; // Vcc in millivolts
}


void getNrfTime() {
  uint32_t beginWait = millis();
  while (millis() - beginWait < 500) {
    time_reqpayload.timestamp = now();
    radio.write(&time_reqpayload , sizeof(time_reqpayload));
    if (radio.isAckPayloadAvailable()) {
      uint8_t len = radio.getDynamicPayloadSize();
      if ( len == sizeof(time_ackpayload)) {
        radio.read(&time_ackpayload, sizeof(time_ackpayload));
      }
    }

    time_reqpayload.timestamp = now();
    radio.write(&time_reqpayload , sizeof(time_reqpayload));
    if (radio.isAckPayloadAvailable()) {
      uint8_t len = radio.getDynamicPayloadSize();
      if ( len == sizeof(time_ackpayload)) {
        radio.read(&time_ackpayload, sizeof(time_ackpayload));
      }
    }

    time_reqpayload.timestamp = now();
    radio.write(&time_reqpayload , sizeof(time_reqpayload));
    if (radio.isAckPayloadAvailable()) {
      uint8_t len = radio.getDynamicPayloadSize();
      if ( len == sizeof(time_ackpayload)) {
        radio.read(&time_ackpayload, sizeof(time_ackpayload));
        setTime((unsigned long)time_ackpayload.timestamp);
        return;
      }
    }
  }
}

time_t requestSync() {
  return 0;
}

long DateToMjd (uint16_t y, uint8_t m, uint8_t d) {
  return
    367 * y
    - 7 * (y + (m + 9) / 12) / 4
    - 3 * ((y + (m - 9) / 7) / 100 + 1) / 4
    + 275 * m / 9
    + d
    + 1721028
    - 2400000;
}

static unsigned long numberOfSecondsSinceEpoch(uint16_t y, uint8_t m, uint8_t d, uint8_t h, uint8_t mm, uint8_t s) {
  long Days;
  Days = DateToMjd(y, m, d) - DateToMjd(1970, 1, 1);
  return (uint16_t)Days * 86400 + h * 3600L + mm * 60L + s;
}
// end
