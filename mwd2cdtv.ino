/*
 * Microsoft wireless desktop to CDTV keyboard/mouse adapter
 * Using an atmega328 based Arduino and an NRF24l01+ radio
 * 
 * This uses ideas and borrows code from: 
 * https://github.com/hkzlab/AVR-Experiments/tree/master/samples/m128-ir-cdtv
 * https://github.com/hkzlab/AVR-Experiments/tree/master/libs/amiga_keyb
 * https://github.com/samyk/keysweeper
 * https://github.com/insecurityofthings/uC_mousejack
 * Respective projects license applies.
 * 
 * It 'emulates' the dongle for older Microsoft wireless desktop
 * keyboard/mouse combos. I use an MWD800, it might work or be adaptable
 * to other models.
 * Note that you *need* to know the address of the keyboard and mouse
 * as well as the channel(s) that they use (pick one). Sniffing those
 * is beyond the scope of this project, I used uC_mousejack successfully
 * and decided to use the same pinout for the nrf to make it simple to
 * change sketch once it has been extracted.
 * 
 *  Timer 1 used for timing mouse pulses
 *  INT1 (Pin3) used for detecting keyboard ack
 *  Pinchange interrupt 1 used for joystick
 *   
 *  Pins used
 *  
 *  
 *  3   (PD3, INT1)     KB_DATA  
 *  4   (PD4)           KB_CLK
 *  
 *  5   (PD5)           nRF24L01 CE
 *  6   (PD6)           nRF24L01 CSN
 *  11  (PB3, MOSI)     nRF24L01 MOSI
 *  12  (PB4, MISO)     nRF24L01 MISO
 *  13  (PB5, SCK)      nRF24L01 SCK
 *  
 *  7   (PD7)           CDTV PRDT
 *  
 *  A0  (PC0)           JOYSTICK UP
 *  A1  (PC1)           JOYSTICK DOWN
 *  A2  (PC2)           JOYSTICK LEFT
 *  A3  (PC3)           JOYSTICK RIGHT
 *  A4  (PC4)           JOYSTICK BUTTON B
 *  A5  (PC5)           JOYSTICK BUTTON A
 *  
 */ 

#include <Arduino.h>
#include <stdint.h>
#include "amiga_keyb.h"
#include "nRF24L01.h"
#include "RF24.h"
#include "hid2amiga.h"
#include "cdtv.h"

#define CE 5
#define CSN 6
#define PAY_SIZE 16

// CHANGE THESE TO MATCH SPECIFIC MWD
#define KBD_ADDR    0xDEADBEEFCDLL
#define MOUSE_ADDR  0xDEADBEEF6DLL
#define CHANNEL     50 // For my MWD one of: 29,33,50,54,70,74,80,84

RF24 radio(CE, CSN);

uint8_t writeRegister(uint8_t reg, uint8_t value)
{
  uint8_t status;

  digitalWrite(CSN, LOW);
  status = SPI.transfer( W_REGISTER | ( REGISTER_MASK & reg ) );
  SPI.transfer(value);
  digitalWrite(CSN, HIGH);
  return status;
}

void process_payload(uint8_t *payload, uint8_t payload_size){
  static uint8_t kbmeta = 0;
  static uint8_t prevkbhids[6] = {0};
  static int8_t prevkbhidslastindex = -1;
  static uint8_t capslockstatus = 0;

  if(payload[0] == 0xA){ // device_type = kbd
    ms_crypt(KBD_ADDR, payload, payload_size); // Decrypt payload
    if(payload[1] == 0x78) { // packet_type = key_event

      // Check & send changed meta keys
      {
        uint8_t meta = payload[7] & ~(KEY_MOD_RCTRL); // Mask out RCTRL as that's missing on Amiga
        uint8_t metadiff = kbmeta ^ meta;
        uint8_t i,j;

        for(i=0; i<8; i++){
          if((metadiff >> i) & 0x1){
            uint8_t cmd = mod2amiga[i];
            cmd |=  ((kbmeta << (7-i)) & 0x80); // Make or break?
            amikbd_kSendCommand(cmd);
          }
        }

        kbmeta = meta;
      }


      /*
      Sigh... Ok... So this bit got a bit messy to handle.
      I believe this is a USB keyboard HID thing. The keyboard sends
      Up to 6 keycodes of the keys that are currently pressed. They
      are always in the order as they are pressed, with the most recently
      pressed key first.
      The Amiga expects make and break codes. So, we need to keep
      the previous packet received and compare it to the current to work out
      which key(s) have been pressed or released.
      */
    
      // Check and send keys
      {
        uint8_t *currkbhids=&payload[9];
        int8_t currkbhidslastindex=5;
        int8_t p, c;

        /* First find the index of the last actual key in new packet */
        while(currkbhids[currkbhidslastindex]==0 && currkbhidslastindex >= 0){
            currkbhidslastindex--;
        }

        p=prevkbhidslastindex; // o = index to previous key buffer
        c=currkbhidslastindex; // i = index to current key buffer

        // Loop over the the two buffers BACKWARDS and try to match up keys
        while(p>=0 || c>=0) {
            if(p<0){
                // prev is out of bounds -> any remaining keys in curr are new
                uint8_t cmd = hid2amiga[currkbhids[c]];
                if(currkbhids[c] == 0x39){
                  // Special handling for caps lock
                  cmd |= capslockstatus;
                  capslockstatus ^= 0x80; 
                }
                amikbd_kSendCommand(cmd);
                c--;
            } else if(c>=0 && currkbhids[c] == prevkbhids[p]){
                // Key found in both prev and curr -> still pressed
                // ignore for now
                p--;
                c--;
            } else {
                // Key only found in p -> send break
                uint8_t cmd = 0x80 | hid2amiga[prevkbhids[p]];
                if(prevkbhids[p] == 0x39){
                  // Ignore caps lock release events
                  cmd = 0xFF; // Nop
                }
                amikbd_kSendCommand(cmd);
                p--;
            }
        }
        
        for(c=0; c <= currkbhidslastindex; c++){
          prevkbhids[c] = currkbhids[c];
        }
        prevkbhidslastindex=currkbhidslastindex;
      }
      
    }
  } else if(payload[0] == 0x8) { // device_type = mouse
    static int8_t x,y;
    static uint8_t buttons;
    if(payload[1] == 0x90) { // packet_type = mouse_event
//      x = payload[10] << 8 | payload[9];   // 16 bit counters...
//      y = payload[12] << 8 | payload[11];  // but 8 bits are enough
      x = payload[9];
      y = payload[11];
      buttons = payload[8] & 0x3;
      mouse_set_state(buttons, x, y);
    } else if(payload[1] == 0x38) { // packet_type = repeat
      mouse_set_state(buttons, 0, 0); // Cannot repeat x & y
    }
  }
    
}

// decrypt those keyboard packets!
void ms_crypt(const uint64_t address, uint8_t *payload, uint8_t payload_size)
{
  for (uint8_t i = 4; i < payload_size; i++)
    payload[i] ^= (address >> (((i - 4) % 5) * 8)) & 0xFF;
}

// calculate microsoft wireless keyboard checksum
uint8_t ms_checksum(uint8_t *payload, uint8_t payload_size)
{
  uint8_t cs = 0;
  for (uint8_t i = 0; i < payload_size - 1; i++){
    cs ^= payload[i];
  }
  cs = ~cs;
  return cs;
}

void radio_setup(){
  radio.begin();
  // the order of the following is VERY IMPORTANT
  radio.setAutoAck(true); // enable when no dongle?
  writeRegister(RF_SETUP, 0x09); // Disable PA, 2M rate, LNA enabled
  radio.enableDynamicPayloads(); 
  radio.setChannel(50); // Change this to your preferred channel
  // RF24 doesn't ever fully set this -- only certain bits of it
  writeRegister(EN_RXADDR, 0x00); // Disable all data pipes
  writeRegister(SETUP_AW, 0x03); // Reset addr size to 5 bytes
  radio.openReadingPipe(0, KBD_ADDR);
  radio.openReadingPipe(1, MOUSE_ADDR);
  radio.startListening();
}

void radio_update() {

  if (radio.available()) {
    uint8_t buf[PAY_SIZE];
    uint8_t payload_size;
    payload_size = radio.getDynamicPayloadSize();
    radio.read(&buf, payload_size);
    process_payload(buf, payload_size);
  }
}


void setup() {
  Serial.begin(115200);

  radio_setup();
  cdtv_init();
  amikbd_setup();
}

void loop() {
  radio_update();
  amikbd_update();
}
