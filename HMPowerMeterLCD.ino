//- -----------------------------------------------------------------------------------------------------------------------
// HMPowerMeterLCD
//
// The following code is derived from the AskSinPP [1] sample HM-ES-TX-WM [2].
// 
// Author: H. Appelt, December 2022
// License: Creative Commons - http://creativecommons.org/licenses/by-nc-sa/3.0/de/
// 
// REFERENCES
// [1] AskSinPP https://github.com/pa-pa/AskSinPP and https://asksinpp.de/
// [2] HM-ES-TX-WM_CCU  https://github.com/jp112sdl/Beispiel_AskSinPP/tree/master/examples/HM-ES-TX-WM_CCU
// [3] EnableInterrupt https://github.com/GreyGnome/EnableInterrupt
// [4] Nokia 5110 LCD Library  https://github.com/platisd/nokia-5110-lcd-library/

#define PM_VERSION_MAJOR 1
#define PM_VERSION_MINOR 0
#define PM_VERSION_STR STRINGIZE(PM_VERSION_MAJOR) "." STRINGIZE(ASKSIN_PLUS_PLUS_MINOR)

// development
#define NDEBUG     // change to get debug output
#define SAVEPOWER  // change to use Idle instead Sleep to save power (for debug)
#define NDEBUGBAT  // change to transmit battery voltage as actualConsumption to the CCU (and may get a discharge diagram)

// #define USE_OTA_BOOTLOADER // define this to read the device id, serial and device type from bootloader section
#define EI_NOTEXTERNAL
#include <EnableInterrupt.h>
#include <AskSinPP.h>
#include <LowPower.h>
#include <Nokia_LCD.h>
#include <MultiChannelDevice.h>
using namespace as; // all library classes are placed in the namespace 'as'

// define device properties
const struct DeviceInfo PROGMEM devinfo = {
    {0x90,0x12,0x14},       // Device ID
    "HGAS901214",           // Device Serial
    {0x00,0xde},            // Device Model
    0x12,                   // Firmware Version
    as::DeviceType::PowerMeter, // Device Type
    {0x01,0x00}             // Info Bytes
};

// we use a Pro Mini and..
//  a single config button
#define CONFIG_BUTTON_PIN 8 // PB0
//  no LEDs (LED states are shown on the LCD display, for debug buildin LED is used)
//    #define LED_PIN A5
//  a CC1101 module with poweron switch
#define CC1101_CS_PIN   10
#define CC1101_MOSI_PIN 11
#define CC1101_MISO_PIN 12
#define CC1101_SCLK_PIN 13
#define CC1101_GD0_PIN   2
#define CC1101_PWR_PIN  17
//  a LCD display 84x48 pixel (eg. NOKIA 5110 or clones)
#define LCD_RST_PIN       3
#define LCD_CE_PIN        4
#define LCD_D0_PIN        5
#define LCD_DIN_PIN       6
#define LCD_CLK_PIN       7
#define LCD_BL_PIN        9     // backlight pin
#define LCD_BL_ONSTATE    HIGH  // backlight "on" pin state
#define LCD_CONTRAST      55    // typcal values are 40-60
#define BACKLIGHT_TIMEOUT 30    // sec
#define MENU_TIMEOUT      30    // sec
//  a LiFePo4 AA battery (w/o voltage regulator) and a TP5000 charging module (values in mV)
//    (!)check(!) that yout charger ends charge at 3,6V or you may destoy the CC1101 module and the battery
//    .. some TODO's (eg. check and implement temperature dependency for battery discharge curve)
//    .. test battery time - at the moment the battery is used for backup during power fail
#define BAT_CHARGE_PIN       A6   // Pin used to measure VDCin voltage to see if we are charging
#define BAT_ADC_INTERNAL_REF 1061 // measured ADC internal ref to calibrate ADC result (should be around 1100mV)
#define BAT_LOW              3130 // for CCU message
#define BAT_CRITICAL         3060 // critical level, go to sleep forever
#define BAT_CALC_FULL        3235 // values recorded at 18°C and CC1101 in on state
#define BAT_CALC_EMPTY       3100 
#define BAT_CALC_CURVE_D     {3235,3230,3225,3210,3200,3195,3185,3170,3150,3110,3090} // discharge 100% -> 0% (comment out to use linear calculation)
#define BAT_CALC_CURVE_C     {3410,3355,3345,3337,3300,3320,3310,3305,3290,3250,3210} // charge 100% -> 0% 
//  two counter inputs (reed or other contact to GND)
//  - two modes to detect a counter impuse
//    1: ISR trigger with sleep wakeup (original code)
//       requires a internal or external pullup resistor
//       disadvantage: some meter types can stay in the contact state "closed" for a long time and consuming power
//    2: Poll the input at a constant rate
//       the pullup resistor can be connected to pwrpin, only activated during a quit short read time
//       disadvantage: only for slow impulse rates, high rate would require a high poll rate consuming power
//  we assume that the generates one implus to increment the lowest digit.. so
//  - no mul/div operations are required to calculate the consumption, just dapat the format string
#define COUNTER1_MODE       2              // 0=none 1=Interrput 2=Poll
#define COUNTER1_PIN        14             // (A0=PIN14 A1=PIN15 A2=PIN16 on Pro Mini)
#define COUNTER1_PINMODE    INPUT          // INPUT_PULLUP or INPUT
#define COUNTER1_PWRPIN     16             // pin to power on measuring, 0xFF=none (poll mode only)
#define COUNTER1_POLL       600            // ms poll interval (poll mode only)
#define COUNTER1_DEBOUNCE   200            // ms
#define COUNTER1_INC        1              // counter increment for one pulse
#define COUNTER1_FORMAT     "######,## m3" // for display: each # is replaced by one digits of the countersum, other chars will be displayed they are
#define COUNTER1_FORMATSIGS 100            // for display: sigs for one pulse *100 (=100 means one pulse increments last digit in display format)
#define COUNTER1_SIGS       10             // for CCU: define to overwrite sigs from CCU config
#define COUNTER1_ICEMUL     1              // for CCU: define extra mutiplicator for counter value (ICE type only)
#define COUNTER2_MODE       1
#define COUNTER2_PIN        15
#define COUNTER2_PINMODE    INPUT_PULLUP
#define COUNTER2_PWRPIN     0xFF  
#define COUNTER2_POLL       600          
#define COUNTER2_DEBOUNCE   200          
#define COUNTER2_INC        1
#define COUNTER2_FORMAT     "######,# kWh"
#define COUNTER2_FORMATSIGS 750            // typical 3-phase meter has 75 rotations for one kWh
#define COUNTER2_SIGS       75
#define COUNTER2_ICEMUL     10000

// send the counter every 2,5 minutes
#define MSG_CYCLE seconds2ticks(2*60+30)
// after boot send counters imediate (so when testing we do not have to wait)
#define FIRST_MSG_CYCLE seconds2ticks(4)
// number of available peers per channel
#define PEERS_PER_CHANNEL 2

// NOKIA LCD display
//  bitmaps: each byte is a vertical line of 8 pixels (first byte is left, bit0 is top)
const uint8_t PMDisplaySymLedOff[9] PROGMEM= {
  B00011100, B00100010, B01000001, B01000001, B01000001, B01000001, B00100010, B00011100, B00000000, };
const uint8_t PMDisplaySymLed1On[9] PROGMEM= {
  B00011100, B00111110, B01111111, B01111111, B01111111, B01111111, B00111110, B00011100, B00000000, };
const uint8_t PMDisplaySymLed2On[9] PROGMEM= {
  B00011100, B00101010, B01010101, B01101011, B01010101, B01101011, B00101010, B00011100, B00000000, };
const uint8_t PMDisplaySymInput1Off[9] PROGMEM= {
  B00000000, B00000000, B01001000, B01000100, B00111110, B01000000, B01000000, B00000000, B00000000, };
const uint8_t PMDisplaySymInput1On[9] PROGMEM= {
  B01111110, B11111111, B10110111, B10111011, B11000001, B10111111, B10111111, B11111111, B01111110, };
const uint8_t PMDisplaySymInput2Off[9] PROGMEM= {
  B00000000, B00000000, B01000100, B01100010, B01010010, B01001010, B01000100, B00000000, B00000000, };
const uint8_t PMDisplaySymInput2On[9] PROGMEM= {
  B01111110, B11111111, B10111011, B10011101, B10101101, B10110101, B10111011, B11111111, B01111110, };
const uint8_t PMDisplaySymBat[15] PROGMEM= {
  B00000000, B00011100, B00100010, B00100010, B01111111, B01000001, B01000001, B01000001, 
  B01000001, B01000001, B01000001, B01000001, B01000001, B01000001, B01111111, };
const uint8_t PMDisplaySymBatH[8] PROGMEM= {
  B01001001, };
const uint8_t PMDisplaySymBatV[8] PROGMEM= {
  B01111111, };
const uint8_t PMDisplaySymBatCharge[6] PROGMEM= {
  B00011100, B00111111, B01111100, B00111111, B00011100, B00000000 };
const uint8_t PMDisplaySymBatNoCharge[6] PROGMEM= {
  B00000000, B00000000, B00000000, B00000000, B00000000, B00000000, };
const uint8_t PMDisplaySymMenuPosNone[6] PROGMEM= {
  B00000000, B00000000, B00000000, B00000000, B00000000, B00000000, };
const uint8_t PMDisplaySymMenuPosLeft[6] PROGMEM= {
  B01111111, B00111110, B00011100, B00011100, B00001000, B00000000, };
const uint8_t PMDisplaySymMenuPosTop[6] PROGMEM= {
  B00110000, B00111100, B00111110, B00111100, B00110000, B00000000, };
const uint8_t PMDisplaySymMenuPosEdit[6] PROGMEM= {
  B00110000, B00111000, B00011100, B01001110, B00000101, B01000010, };

//  display pages: 
//    the default (amin) page is drawn by code
//    other pages have a DISPLAY_MENU_DEF with text and menu marker definitions
enum { DISPLAY_PAGE_DEFAULT=0, DISPLAY_PAGE_WELCOME, DISPLAY_PAGE_CC1101ERROR, DISPLAY_PAGE_MENU1, DISPLAY_PAGE_RESETDEVICE,
       DISPLAY_PAGE_SETCOUNTER1, DISPLAY_PAGE_SETCOUNTER2 };
enum { DISPLAY_MENUDIR_NONE=0, DISPLAY_MENUDIR_LEFT, DISPLAY_MENUDIR_TOP };
enum { DISPLAY_MENU_END=0xFF, DISPLAY_MENU_NONE=0xFE, DISPLAY_MENU_WRAP=0xFD };
enum { DISPLAY_MENU_CANCEL,
       DISPLAY_MENU_SETCOUNTER1=10, DISPLAY_MENU_SETCOUNTER2, DISPLAY_MENU_SAVECOUNTERS, DISPLAY_MENU_STARTPAIRING, DISPLAY_MENU_RESETDEVICE,
       DISPLAY_MENU_SAVECOUNTER1=20, DISPLAY_MENU_SAVECOUNTER2,
       DISPLAY_MENU_RESETCONFIRM=30, 
       DISPLAY_MENU_COUNTERPOS_FIRST=100, DISPLAY_MENU_COUNTERPOS_LAST=120,
};
typedef struct { uint8_t menu; uint8_t textx; uint8_t texty; const char *text; uint8_t menux; uint8_t menuy; uint8_t menudir;  } DISPLAY_MENU_DEF;
static const DISPLAY_MENU_DEF _display_welcome_def[]={
  // menuitemid                textpos    text        markerpos    markertype
  { DISPLAY_MENU_NONE,          1, 2, "HMPowerMeter" },
  { DISPLAY_MENU_NONE,          5, 3, PM_VERSION_STR },
  { DISPLAY_MENU_END },
};
static const DISPLAY_MENU_DEF _display_cc1101error_def[]={
  // menuitemid                textpos    text        markerpos    markertype
  { DISPLAY_MENU_NONE,          4, 2, "CC1101" },
  { DISPLAY_MENU_NONE,          4, 3, "error" },
  { DISPLAY_MENU_END },
};
static const DISPLAY_MENU_DEF _display_menu1_def[]={
  // menuid/action             textpos    text        markerpos    markertype
  { DISPLAY_MENU_SETCOUNTER1,   1, 1, "set counter1", 0, 1, DISPLAY_MENUDIR_LEFT },
  { DISPLAY_MENU_SETCOUNTER2,   1, 2, "set counter2", 0, 2, DISPLAY_MENUDIR_LEFT },
  { DISPLAY_MENU_SAVECOUNTERS,  1, 3, "save counters", 0, 3, DISPLAY_MENUDIR_LEFT },
  { DISPLAY_MENU_STARTPAIRING,  1, 4, "start pairing", 0, 4, DISPLAY_MENUDIR_LEFT },
  { DISPLAY_MENU_RESETCONFIRM,  1, 5, "reset device" , 0, 5, DISPLAY_MENUDIR_LEFT },
  { DISPLAY_MENU_END },
};
static const DISPLAY_MENU_DEF _display_resetconfirm_def[]={
  { DISPLAY_MENU_NONE,          0, 2, "reset device?" },
  { DISPLAY_MENU_CANCEL,        1, 4, "cancel",        0, 4, DISPLAY_MENUDIR_LEFT },
  { DISPLAY_MENU_RESETDEVICE,   1, 5, "ok",            0, 5, DISPLAY_MENUDIR_LEFT },
  { DISPLAY_MENU_WRAP },
};
static const struct { uint8_t page; const DISPLAY_MENU_DEF *pagemenu; } _display_page2menu[]={
  { DISPLAY_PAGE_WELCOME, _display_welcome_def },
  { DISPLAY_PAGE_CC1101ERROR, _display_cc1101error_def },
  { DISPLAY_PAGE_MENU1, _display_menu1_def },
  { DISPLAY_PAGE_RESETDEVICE, _display_resetconfirm_def },
};

#define DISPLAY_WIDTH  84
#define DISPLAY_HEIGHT 48
#define CHAR_WIDTH      6 // including one right empty column as sep
#define CHAR_HEIGHT     8
#define DISPLAY_ROWS    (DISPLAY_HEIGHT/CHAR_HEIGHT) // 6
#define DISPLAY_COLS    (DISPLAY_WIDTH/CHAR_WIDTH) // 14
class PMDisplay : public Alarm,  protected Nokia_LCD {
protected:
  uint8_t backlightstate;
  uint8_t led1state, led2state;
  uint8_t input1state, input2state;
  uint8_t batterystate, batterycharge;
  uint64_t sum1, sum2;
  enum { FLAG_LED1=1, FLAG_LED2=2, FLAG_INPUT1=4, FLAG_INPUT2=8, FLAG_BAT=16, FLAG_SUM1=32, FLAG_SUM2=64, FLAG_BKGND=128, FLAG_MENUPOS=256, FLAG_EDITLINE=512 };
  uint16_t dirty;
  int8_t currentpage, currentmenupos;
  const DISPLAY_MENU_DEF *currentmenudef; uint8_t currentmenudefnonestatic;
  uint8_t menuedit;
  char countereditline[DISPLAY_COLS+1];
public:
  PMDisplay() : Alarm(0), Nokia_LCD(LCD_CLK_PIN,LCD_DIN_PIN,LCD_D0_PIN,LCD_CE_PIN,LCD_RST_PIN,LCD_BL_PIN) {
    backlightstate=0;
    led1state=0; led1state=0; 
    input1state=0; input2state=0;
    batterystate=0xFF;
    batterycharge=0;
    sum1=0; sum2=0;
    dirty=0xFF;
    currentpage=DISPLAY_PAGE_DEFAULT; currentmenupos=0;
    currentmenudef=0; currentmenudefnonestatic=0;
    menuedit=0;
  }
  void init( uint8_t input1init=0, uint8_t input2init=0 ) {
    input1state=input1init; input2state=input2init;
    begin(); // initialize the screen
    setContrast( LCD_CONTRAST );  // initialize contrast
    clear( false ); // clear the screen
    setpage( DISPLAY_PAGE_WELCOME, 3 ); update();
  }
  void initdone() {
    setpage( DISPLAY_PAGE_DEFAULT ); 
  }
  void setbacklight( uint8_t newstate ) {
    backlightstate=newstate; setBacklight(newstate);
  }
  void tooglebacklight() {
    backlightstate=(backlightstate?0:1); setBacklight(backlightstate);
  }
  uint8_t getbacklight() const {
    return backlightstate;
  }
  void setled( uint8_t lednr, uint8_t newstate ) {
    ATOMIC_BLOCK( ATOMIC_RESTORESTATE ) {
      uint8_t& state=(lednr==1?led1state:led2state);
      if ( state!=newstate ) { state=newstate; dirty|=(lednr==1?FLAG_LED1:FLAG_LED2); }
    }
  }
  uint8_t getled( uint8_t lednr, uint8_t removedirty=0 ) {
    volatile uint8_t ret;
    ATOMIC_BLOCK( ATOMIC_RESTORESTATE ) {
      ret=(lednr==1?led1state:led2state);
      if ( removedirty )  dirty&=(lednr==1?(~FLAG_LED1):(~FLAG_LED2));
    }
    return ret;
  }
  void setinput( uint8_t inputnr, uint8_t newstate ) {
    ATOMIC_BLOCK( ATOMIC_RESTORESTATE ) {
      uint8_t& state=(inputnr==1?input1state:input2state);
      if ( state!=newstate ) { state=newstate; dirty|=(inputnr==1?FLAG_INPUT1:FLAG_INPUT2); }
    }
  }
  void setbatterystate( uint8_t newstate ) {
    if ( batterystate!=newstate ) { batterystate=newstate; setdirty(FLAG_BAT); }
  }
  void setbatterycharge( uint8_t newstate ) {
    if ( batterycharge!=newstate ) { batterycharge=newstate; setdirty(FLAG_BAT); }
  }
  void setcounter( uint8_t channelnr, uint64_t newsum ) {
    uint64_t& sum=(channelnr==1?sum1:sum2);
    uint32_t sigs=(channelnr==1?COUNTER1_FORMATSIGS:COUNTER2_FORMATSIGS);
    sum=(newsum*100)/sigs;
    setdirty(channelnr==1?FLAG_SUM1:FLAG_SUM2);
  }
  void setpage( uint8_t newpage, uint8_t page_timeout_sec=0 ) {
    // DPRINT(F("setpage ")); DDECLN(newpage);
    uint16_t flags=0;
    if ( currentpage!=newpage )  { 
      currentpage=newpage; currentmenupos=-1; flags|=FLAG_BKGND; menuedit=0;
      setmenudef(0);
      // static page definitions
      for ( uint8_t i=0; i<sizeof(_display_page2menu)/sizeof(_display_page2menu[0]); i++ )
        if ( _display_page2menu[i].page==newpage )
          setmenudef( _display_page2menu[i].pagemenu );
      // dynamic page definitions
      if ( !currentmenudef && newpage==DISPLAY_PAGE_SETCOUNTER1 )
         createsetcountermenu( DISPLAY_MENU_SAVECOUNTER1, sum1, COUNTER1_FORMAT );
      if ( !currentmenudef && newpage==DISPLAY_PAGE_SETCOUNTER2 )
         createsetcountermenu( DISPLAY_MENU_SAVECOUNTER2, sum2, COUNTER2_FORMAT );
      nextmenupos(); // init first position
      if ( page_timeout_sec>0 ) {
        sysclock.cancel(*this); // set page timeout 
        tick=seconds2ticks(page_timeout_sec); sysclock.add(*this);
      }
    }
    setdirty( flags );
  }
  void setmenudef( const DISPLAY_MENU_DEF *pdef, uint8_t nonestatic=0 ) {
    if ( currentmenudef && currentmenudefnonestatic )   free(currentmenudef);
    currentmenudef=pdef; currentmenudefnonestatic=nonestatic;
  }
  uint8_t getpage() const {
    return currentpage;
  }
  uint8_t getmenu() const {
    if ( !currentmenudef )   return DISPLAY_MENU_END;
    if ( currentmenupos>=0 )   return currentmenudef[currentmenupos].menu;
    return DISPLAY_MENU_NONE;
  }
  bool nextmenupos() {
    // DPRINTLN(F("nextmenupos"));
    bool ret=false; uint16_t flags=0;
    while ( currentmenudef && currentmenupos>=-1 )
    {
      if ( currentmenupos>=0 && currentmenudef[currentmenupos].menu==DISPLAY_MENU_END )  return false;
      currentmenupos++; flags|=FLAG_MENUPOS; 
      uint8_t menu=currentmenudef[currentmenupos].menu;
      if ( menu==DISPLAY_MENU_END )  { ret=false; break; }
      else if ( menu==DISPLAY_MENU_WRAP )  currentmenupos=-1;
      else if ( menu!=DISPLAY_MENU_NONE )  { ret=true; break; }
    }
    setdirty( flags );
    return ret;
  }
  bool menuposedit() const { // current menu select is editable
    uint8_t menu=getmenu();
    return (menu>=DISPLAY_MENU_COUNTERPOS_FIRST && menu<=DISPLAY_MENU_COUNTERPOS_LAST);
  }
  uint8_t getmenuedit() const {
    return menuedit;
  }
  void setmenuedit( uint8_t newstate ) {
    if ( newstate!=menuedit )  { menuedit=newstate; setdirty(FLAG_MENUPOS); }
  }
  uint8_t tooglemenuedit() {
    setmenuedit( menuedit?0:1 );
    return menuedit;
  }
  bool nextmenuedit() {
    if ( !menuposedit() )   return false;
    const DISPLAY_MENU_DEF& m=currentmenudef[currentmenupos];    
    if ( m.menu>=DISPLAY_MENU_COUNTERPOS_FIRST && m.menu<=DISPLAY_MENU_COUNTERPOS_LAST ) {
      return incrementcounterdigit( m.menux );
    }
    return false;
  }
  bool getmenueditvalue( uint64_t& val, uint8_t updateval=1 ) {
    bool ret=true;
    if ( currentpage==DISPLAY_PAGE_SETCOUNTER1 ) {
      val=getcounterline( countereditline, COUNTER1_FORMAT );
      if ( updateval )  setcounter( 1, val );
    }
    else if ( currentpage==DISPLAY_PAGE_SETCOUNTER2 ) {
      val=getcounterline( countereditline, COUNTER2_FORMAT );
      if ( updateval )  setcounter( 2, val );
    } else {
      ret=false;
    }
    return ret;
  }
  void setdirty( uint16_t flags ) {
    ATOMIC_BLOCK( ATOMIC_RESTORESTATE ) { 
      dirty|=flags;
    }
  }
  bool update() {
    volatile uint16_t flags;  
    ATOMIC_BLOCK( ATOMIC_RESTORESTATE ) { 
      flags=dirty; dirty=0;
    }
    if ( flags&FLAG_LED1 )  drawled(1,getled(1,true));
    if ( flags&FLAG_LED2 )  drawled(2,getled(2,true));
    if ( flags&FLAG_INPUT1 )  drawinput( 1, input1state ); 
    if ( flags&FLAG_INPUT2 )  drawinput( 2, input2state );
    if ( flags&FLAG_BAT )  drawbattery( batterystate, batterycharge );
    if ( currentpage==DISPLAY_PAGE_DEFAULT ) {
        if ( flags&FLAG_BKGND )              clearrows();
        if ( flags&(FLAG_SUM1|FLAG_BKGND) )  printcounter( 2, sum1, COUNTER1_FORMAT );
        if ( flags&(FLAG_SUM2|FLAG_BKGND) )  printcounter( 4, sum2, COUNTER2_FORMAT );
    } else if ( currentmenudef ) {
        if ( flags&FLAG_BKGND ) { clearrows(); flags|=FLAG_MENUPOS|FLAG_EDITLINE|FLAG_SUM1|FLAG_SUM2; }
        const DISPLAY_MENU_DEF *mdef=currentmenudef;
        while ( mdef && mdef->menu!=DISPLAY_MENU_END && mdef->menu!=DISPLAY_MENU_WRAP ) {
          bool editlinedirty=(flags&FLAG_EDITLINE) && (mdef->text==countereditline);
          if ( flags&FLAG_BKGND )    printat( CHAR_WIDTH*mdef->textx, mdef->texty, mdef->text );
          if ( flags&FLAG_MENUPOS )  drawmenupos( CHAR_WIDTH*mdef->menux, mdef->menuy, mdef->menu, mdef->menudir );
          if ( editlinedirty )       printat( CHAR_WIDTH*mdef->textx, mdef->texty, mdef->text );
          mdef++;
        }
    }
    return dirty!=0;
  }
  virtual void trigger( AlarmClock& ) {
    setpage( DISPLAY_PAGE_DEFAULT );
  }
protected:
  void drawat( uint8_t x, uint8_t row, const unsigned char bitmap[], const unsigned int bitmap_size, const bool progmem=true ) {
    setCursor( x, row ); draw( bitmap, bitmap_size, progmem );
  }
  void printat( uint8_t x, uint8_t row, const char *str ) {
    if ( !str )  return;
    setCursor( x, row ); print( str );
  }
  void printat( uint8_t x, uint8_t row, const char c ) {
    setCursor( x, row ); print( c );
  }
  void drawled( uint8_t lednr, uint8_t state ) {
    uint8_t ledcx=sizeof(PMDisplaySymLedOff)/sizeof(PMDisplaySymLedOff[0]);
    setCursor( lednr==1?0:ledcx, 0 );
    if ( state==0 )                   draw(PMDisplaySymLedOff, ledcx, true);
    else if ( lednr==1 && state!=0 )  draw(PMDisplaySymLed1On, ledcx, true);
    else if ( lednr==2 && state!=0 )  draw(PMDisplaySymLed2On, ledcx, true);
  }
  void drawinput( uint8_t inputnr, uint8_t state ) {
    uint8_t inputx=19;
    uint8_t inputcx=sizeof(PMDisplaySymInput1Off)/sizeof(PMDisplaySymInput1Off[0]);
    #if COUNTER2_MODE<1
    inputx+=4; if ( inputnr>1 ) return;
    #endif
    setCursor( inputnr==1?inputx:inputx+inputcx, 0 );
    if ( inputnr==1 && state==0 )       draw(PMDisplaySymInput1On, inputcx, true);
    else if ( inputnr==1  )             draw(PMDisplaySymInput1Off, inputcx, true);
    else if ( inputnr==2 && state==0 )  draw(PMDisplaySymInput2On, inputcx, true);
    else if ( inputnr==2 )              draw(PMDisplaySymInput2Off, inputcx, true);
  }
  void drawbattery( uint8_t percent, uint8_t charge ) {
    uint8_t batcx=sizeof(PMDisplaySymBat)/sizeof(PMDisplaySymBat[0]);
    uint8_t batchgcx=sizeof(PMDisplaySymBatCharge)/sizeof(PMDisplaySymBatCharge[0]);
    uint8_t x=DISPLAY_WIDTH-batcx; char c[5]={ ' ', ' ', ' ', ' ', 0 };
    drawat( x, 0, PMDisplaySymBat, batcx, true );
    if ( percent!=0xFF ) {
      for ( uint8_t cx=0, p=0; p<=100 && p<percent; cx++, p+=10 )
        drawat( x+batcx-cx-2, 0, PMDisplaySymBatV, 1, true ); // fill battery symbol
      for ( int8_t i=2, p=percent; i>=0; i-- ) { // percent
        uint8_t d=(p%10); p/=10; 
        c[i]=d+'0'; if ( p==0 )   break;
      }
      c[3]='%';
    } else {
      for ( uint8_t i=0, cx=3; i<3; i++, cx+=2 )
        drawat( x+cx+4, 0, PMDisplaySymBatH, 1, true );
    }
    x-=4*CHAR_WIDTH; printat( x, 0, c );
    x-=1*CHAR_WIDTH; drawat( x, 0, charge?PMDisplaySymBatCharge:PMDisplaySymBatNoCharge, batchgcx, true );
  }
  void clearrow( uint8_t row ) {
    setCursor( 0, row );
    for ( uint8_t i=0; i<DISPLAY_COLS; i++ )   
       print((char)' ');
  }
  void clearrows() {
    for ( uint8_t i=1; i<DISPLAY_ROWS; i++ )   
      clearrow(i);
  }
  void printcounter( uint8_t row, uint64_t v, char *fmt=0 ) {
    char line[DISPLAY_COLS+1];
    formatcounterline( line, DISPLAY_COLS, fmt );
    setcounterline( line, DISPLAY_COLS, v );
    setCursor( 0, row ); print( line );
  }
  void formatcounterline( char *line, uint8_t len, char *fmt=0 ) {
    if ( fmt==0 )   fmt="########";
    uint8_t fmtlen=strlen(fmt); if ( fmtlen>len )   fmtlen=len;
    uint8_t fmtx=(DISPLAY_COLS-fmtlen)/2; // center
    for ( uint8_t i=0; i<len; i++ )   line[i]=' ';
    memcpy( line+fmtx, fmt, fmtlen*sizeof(char) );
    line[len]=0;
  }
  void setcounterline( char *line, uint8_t len, uint64_t v ) {
    while ( len>0 ) {
      len--; if ( line[len]=='#' )  { uint8_t d=(v%10); v/=10; line[len]=d+'0'; }
    }
  }
  uint64_t getcounterline( char *line, char *fmt=0 ) {
    char linefmt[DISPLAY_COLS+1]; formatcounterline( linefmt, DISPLAY_COLS, fmt );
    uint8_t i=0; uint64_t val=0;
    while ( line && line[i] && linefmt[i] ) {
      if ( linefmt[i]=='#' ) {
        val*=10; uint8_t c=line[i]; if ( c>='0' && c<='9' )  val+=c-'0';
      }
      i++;
    }
    return val;
  }
  bool incrementcounterdigit( uint8_t x ) {
    if ( x<0 || x>=DISPLAY_COLS )   return false;
    char c=countereditline[x]; if ( c<'0' || c>'9' )  return false;
    c++; if ( c>'9' )  c='0';
    countereditline[x]=c; dirty|=FLAG_EDITLINE;
    return true;
  }
  void createsetcountermenu( uint8_t menusave, uint64_t sum, const char *fmt=0 ) {
    uint8_t counterlinelen=sizeof(countereditline)/sizeof(countereditline[0])-1;
    formatcounterline( countereditline, counterlinelen, fmt );
    uint8_t digits=0; for ( uint8_t i=0; countereditline[i]; i++ )  digits+=(countereditline[i]=='#')?1:0;
    uint8_t menucnt=digits+2+1; uint16_t menusize=sizeof(DISPLAY_MENU_DEF)*menucnt;
    DISPLAY_MENU_DEF *pmenu=(DISPLAY_MENU_DEF*)malloc(menusize); if ( !pmenu )   return;
    memset( pmenu, 0, menusize );
    uint8_t digit=0, textrow=2, menu=DISPLAY_MENU_COUNTERPOS_FIRST; 
    uint8_t dpos=0; while ( countereditline[dpos] && countereditline[dpos]!='#' ) dpos++;
    for ( uint8_t ofs=0; ofs<menucnt; ofs++ ) {
      DISPLAY_MENU_DEF& m=pmenu[ofs];
      if ( digit<digits ) {
        m.menu=menu; menu++;
        m.textx=0; m.texty=textrow; m.text=(digit==0)?countereditline:0;
        m.menux=dpos; m.menuy=textrow+1; m.menudir=DISPLAY_MENUDIR_TOP;
        dpos++; while ( countereditline[dpos] && countereditline[dpos]!='#' ) dpos++;
        digit++;
      } else {
        switch (digit-digits) {
          case 0: 
            textrow+=2; m.menu=menusave; m.textx=1; m.texty=textrow; m.text="save";
            m.menux=0; m.menuy=textrow; m.menudir=DISPLAY_MENUDIR_LEFT;
            digit++; break;
          case 1: 
            textrow++; m.menu=DISPLAY_MENU_CANCEL; m.textx=1; m.texty=textrow; m.text="cancel";
            m.menux=0; m.menuy=textrow; m.menudir=DISPLAY_MENUDIR_LEFT;
            digit++; break;
          case 2: 
            m.menu=DISPLAY_MENU_WRAP;
            digit++; break;
        }
      }
    }
    setcounterline( countereditline, counterlinelen, sum );
    setmenudef( pmenu, 1 );
  }
  void drawmenupos( uint8_t x, uint8_t row, uint8_t menusel, uint8_t menudir ) {
    if ( menusel>=0x80 || menudir==DISPLAY_MENUDIR_NONE )  return;
    const unsigned int bitmap_size=sizeof(PMDisplaySymMenuPosNone)/sizeof(PMDisplaySymMenuPosNone[0]); // assuming on char size for all
    setCursor( x, row ); 
    if ( getmenu()!=menusel )                  draw( PMDisplaySymMenuPosNone, bitmap_size, true );
    else if ( menuedit )                       draw( PMDisplaySymMenuPosEdit, bitmap_size, true );
    else if ( menudir==DISPLAY_MENUDIR_LEFT )  draw( PMDisplaySymMenuPosLeft, bitmap_size, true );
    else if ( menudir==DISPLAY_MENUDIR_TOP )   draw( PMDisplaySymMenuPosTop, bitmap_size, true );
  }
};
PMDisplay display;

// custom battery sensor type
//  - manual trigger measurement
//  - values in mV
class PMBatterySensor : public InternalVCC {
  uint16_t m_Vcc, m_Low, m_Critical;
  uint16_t m_Vchg;
  uint8_t  m_Charging;
 public:
  PMBatterySensor() : m_Low(0), m_Critical(0), m_Vcc(0), m_Vchg(0), m_Charging(0) {
  }
  virtual ~PMBatterySensor() {
  }
  // BattSensor basics
  uint8_t current() const { return (m_Vcc+50)/100; }
  bool critical() const { return m_Vcc<m_Critical; }
  void critical(uint16_t value) { m_Critical=value; }
  bool low() const { return m_Vcc<m_Low; }
  void low (uint16_t value) { m_Low=value; }
  void resetCurrent() { m_Vcc=0; }
  void init(uint32_t period,AlarmClock& clock) { init(); }
  void init() {
    InternalVCC::init();
    measure();
  }
  void measure( uint8_t updatedisplay=0 ) {
    // measure battery voltage = internal Vcc
    InternalVCC::start();
    m_Vcc=InternalVCC::finish();
    #ifdef BAT_ADC_INTERNAL_REF
    m_Vcc=(((uint32_t)m_Vcc)*BAT_ADC_INTERNAL_REF)/1100;
    DPRINT(F("iVcc adjusted ")); DDECLN(m_Vcc);
    // measure charger voltage
    #endif
    #ifdef BAT_CHARGE_PIN
    analogReference(INTERNAL); m_Vchg=analogRead(BAT_CHARGE_PIN); 
    for ( uint8_t x=0; x<8; x++ ) { 
      uint16_t last=m_Vchg; delay(1); m_Vchg=analogRead(BAT_CHARGE_PIN); 
      if ( m_Vchg<=last )   break;
    }
    m_Vchg=(m_Vchg*(10+1))/1; // assume 1:11 divider eg. 1k/10k
    #ifdef BAT_ADC_INTERNAL_REF
    m_Vchg=(((uint32_t)m_Vchg)*BAT_ADC_INTERNAL_REF)/1100;
    #endif
    m_Charging=(m_Vchg>4000);
    DPRINT(F("Vchg ")); DDECLN(m_Vchg);
    #endif
    if ( updatedisplay ) {
      uint32_t vcc=m_Vcc; uint8_t percent=0xFF;
      #ifdef BAT_CALC_CURVE_D
      // calc by discharge curve, BAT_CALC_CURVE_D array has 11 elements (100% to 0%) with vcc in mV
      static const uint16_t curve_d[]=BAT_CALC_CURVE_D; static const uint16_t curve_c[]=BAT_CALC_CURVE_C;  
      uint16_t *curve=(m_Charging?curve_c:curve_d);
      uint8_t curvesize=(m_Charging?(sizeof(curve_c)/sizeof(curve_c[0])):(sizeof(curve_d)/sizeof(curve_d[0])));
      int8_t curvestep=100/(curvesize-1);
      if ( vcc>=curve[0] )   percent=100;
      for ( int8_t i=0, p1=100, p2=100-curvestep; i<curvesize-1 && percent==0xFF; i++, p1-=curvestep, p2-=curvestep ) {
        p1=(p1>=0?p1:0);  p2=(p2>=0?p2:0); 
        if ( vcc>=curve[i+1] )
           percent=((vcc-curve[i+1])*(p2-p1))/(curve[i]-curve[i+1])+p2; // linear between curve points
      }
      if ( percent==0xFF )   percent=0;
      #elif defined(BAT_CALC_EMPTY) && defined(BAT_CALC_FULL);
      // calc linear
      if ( vcc<BAT_CALC_EMPTY )  vcc=BAT_CALC_EMPTY;
      if ( vcc>BAT_CALC_FULL )  vcc=BAT_CALC_FULL;
      percent=((vcc-BAT_CALC_EMPTY)*100)/(BAT_CALC_FULL-BAT_CALC_EMPTY);
      #endif
      display.setbatterystate( percent );
      #ifdef BAT_CHARGE_PIN
      display.setbatterycharge( m_Charging );
      #endif
    }
  }
  void setIdle () {}
  void unsetIdle () {}
  // for backward compatibility
  uint16_t voltageHighRes() { return m_Vcc; }
  uint8_t voltage() { return current(); }
  // asksin METER for compatibility
  PMBatterySensor& meter() { return *this; }
  PMBatterySensor& sensor() { return *this; }
  uint16_t value () const { return m_Vcc; }
};

// custom LED type
// - uses LCD display instead of LEDs
#ifndef LED_PIN
class PMLCDLedPins {
public:
  static void setOutput( __attribute__ ((unused)) uint8_t lednr ) {
    }
  static void setHigh( uint8_t lednr ) {
    display.setled( lednr, 1 );
    #ifndef NDEBUG
    #ifndef LED_BUILTIN
    #define LED_BUILTIN 13 // pro mini board
    #endif
    if ( lednr==1 )   digitalWrite(LED_BUILTIN, HIGH);
    #endif
  }
  static void setLow( uint8_t lednr ) {
    display.setled( lednr, 0 );
    #ifndef NDEBUG
    if ( lednr==1 )   digitalWrite(LED_BUILTIN, LOW);
    #endif
  }
};
class PMLedType : public DualStatusLed<1,2,PMLCDLedPins,PMLCDLedPins> {
  typedef DualStatusLed<1,2,PMLCDLedPins,PMLCDLedPins> LedTypeBase;
  uint8_t errorcnt;
public:
  PMLedType() : LedTypeBase(), errorcnt(0) {
  }
  void set(Mode stat) {
    if ( stat==LedStates::failure && errorcnt<0xFF )  errorcnt++; // catch errors (eg. init CC1101, see AskSinPP.h)
    LedTypeBase::set( stat ); 
  }
  uint8_t geterrorcnt( uint8_t reseterrors=1 ) {
    uint8_t ret=errorcnt; if ( reseterrors )  errorcnt=0;
    return ret;
  }
};
#else
typedef StatusLed<LED_PIN> PMLedType;
#endif

// custom storage for counter values
// - keeps content during reset
// - backup in eeprom for power fail
#define PERSIST_MEM_BACKUP_CYCLE seconds2ticks(24*60*60) // backup data at least every day
#define PERSIST_MEM_MAGIC 0x1A3B5C7D
typedef struct {
  uint16_t hdr_chksum;
  uint32_t hdr_magic;
  uint32_t cnt1;
  uint32_t cnt2;
  uint64_t sum1;
  uint64_t sum2;
  uint8_t  state1;
  uint8_t  state2;
  uint8_t  boot1;
  uint8_t  boot2;
} PERSIST_MEM_TYPE;
volatile PERSIST_MEM_TYPE _persist_mem __attribute__ ((section (".noinit")));

class PersistMemType : public Alarm {
  volatile PERSIST_MEM_TYPE& m;
  uint32_t userstorage_start;
  uint8_t userstorage_dirty;
  enum { INIT_STAT_MEM_PERSIT=1, INIT_STAT_MEM_RESTORED, INIT_STAT_MEM_RESET };
  uint8_t init_stat;
public:
  PersistMemType( volatile PERSIST_MEM_TYPE& mref ) : Alarm(PERSIST_MEM_BACKUP_CYCLE), m(mref), userstorage_start(0), init_stat(0) {
  }
  void reset() {
    m.hdr_chksum=0; m.hdr_magic=PERSIST_MEM_MAGIC; 
    m.cnt1=0; m.cnt2=0; m.sum1=0; m.sum2=0; m.state1=0xFF; m.state2=0xFF; m.boot1=1; m.boot2=1; 
    validate();
    userstorage_dirty=1; save();
  }
  void init( uint32_t us_start ) {
    userstorage_start=us_start; userstorage_dirty=1; init_stat=INIT_STAT_MEM_PERSIT;
    if ( !isvalid() ) {
      UserStorage us(userstorage_start);
      us.getData( 0, (uint8_t*)&m, sizeof(m) );
      userstorage_dirty=0; init_stat=INIT_STAT_MEM_RESTORED;
    }
    if ( !isvalid() ) {
      reset(); init_stat=INIT_STAT_MEM_RESET;
    }
    save();
    #ifndef NDEBUG
    dumpSize();
    // DPRINT(F("PersitMem sum1 ")); DHEX((uint32_t)(m.sum1>>32)); DHEXLN((uint32_t)m.sum1);
    // DPRINT(F("PersitMem sum2 ")); DHEX((uint32_t)(m.sum2>>32)); DHEXLN((uint32_t)m.sum2);
    #endif
  }
  void save() {
    if ( userstorage_dirty && isvalid() ) {
      UserStorage us(userstorage_start);
      us.setData( 0, (uint8_t*)&m, sizeof(m) );
      userstorage_dirty=0;
    }
  }
  uint32_t nextcount(uint8_t channelnr, uint16_t inc=1 ) {
    volatile uint32_t ret;
    ATOMIC_BLOCK( ATOMIC_RESTORESTATE ) {
      volatile uint32_t& cnt=(channelnr==1?m.cnt1:m.cnt2);
      cnt+=inc; ret=cnt; validate(); userstorage_dirty=1;
    }
    return ret;
  }
  uint64_t getsum(uint8_t channelnr, uint8_t updated=1) {
    volatile uint64_t ret;
    ATOMIC_BLOCK( ATOMIC_RESTORESTATE ) {
      ret=(channelnr==1?m.sum1:m.sum2);
      if ( updated )   ret+=(channelnr==1?m.cnt1:m.cnt2);
    }
    return ret;
  }
  void setsum( uint8_t channelnr, uint64_t newsum, uint8_t updated=1, uint8_t runsave=1 ) {
    ATOMIC_BLOCK( ATOMIC_RESTORESTATE )
    {
      volatile uint32_t& cnt=(channelnr==1?m.cnt1:m.cnt2);
      volatile uint64_t& sum=(channelnr==1?m.sum1:m.sum2);
      sum=newsum; if ( updated )  cnt=0; 
      validate(); userstorage_dirty=1;
    }    
    if ( runsave )  save();
  }
  void updatesum(uint8_t channelnr, uint32_t *pcntout, uint64_t *psumout) {
    volatile uint64_t ret;
    ATOMIC_BLOCK( ATOMIC_RESTORESTATE )
    {
      volatile uint32_t& cnt=(channelnr==1?m.cnt1:m.cnt2);
      volatile uint64_t& sum=(channelnr==1?m.sum1:m.sum2);
      if ( pcntout )  *pcntout=cnt;
      sum+=cnt; cnt=0; 
      if ( psumout )  *psumout=sum; 
      validate(); userstorage_dirty=1;
    }  
  }
  uint8_t getstate(uint8_t channelnr) const {
    volatile uint8_t ret;
    ATOMIC_BLOCK( ATOMIC_RESTORESTATE ) {
      ret=(channelnr==1?m.state1:m.state2);
    }
    return ret;
  }
  void setstate(uint8_t channelnr, uint8_t newstate) {
    ATOMIC_BLOCK( ATOMIC_RESTORESTATE ) {
      volatile uint8_t& s=(channelnr==1?m.state1:m.state2);
      if ( s!=newstate ) {
        s=newstate; validate(); userstorage_dirty=1;
      }
    }
  }
  bool resetboot(uint8_t channelnr) {
    bool ret;
    ATOMIC_BLOCK( ATOMIC_RESTORESTATE ) {
      volatile uint8_t& b=(channelnr==1?m.boot1:m.boot2);
      ret=b; if ( b ) { b=0; validate(); userstorage_dirty=1; }
    }
    return ret;
  }
  virtual void trigger(AlarmClock& clock) {
    tick=PERSIST_MEM_BACKUP_CYCLE; clock.add(*this);
    save();
  }
  protected:
  uint16_t checksum() {
    uint16_t cs=0x35; uint8_t *p=(uint8_t *)&m; uint8_t s=sizeof(m);
    p+=sizeof(m.hdr_chksum); s-=sizeof(m.hdr_chksum);
    while (s>0)  { cs^=*p; ++p; --s; }
    return cs;
  }
  uint8_t isvalid() {
    return (PERSIST_MEM_MAGIC==m.hdr_magic && checksum()==m.hdr_chksum);
  }
  void validate() {
    m.hdr_chksum=checksum();
  }
  void dumpSize () {
    #ifndef NDEBUG
    DPRINT(F("PersitMem Space: ")); DDEC(userstorage_start); DPRINT(F(" - ")); DDEC(userstorage_start+sizeof(m));
    if ( m.boot1 || m.boot2 )  DPRINT(F(" boot"));
    if ( init_stat==INIT_STAT_MEM_PERSIT )  DPRINT(F(" mem_persit"));
    if ( init_stat==INIT_STAT_MEM_RESTORED )  DPRINT(F(" mem_restored"));
    if ( init_stat==INIT_STAT_MEM_RESET )  DPRINT(F(" mem_reset"));
    DPRINTLN(F(""));
    #endif
  }
};
volatile PersistMemType PersistMem(_persist_mem);
PersistMemType *_gPersistMem=&PersistMem;

// custom menu button 
// the one and olny button is used to set/control display modes
// - page: default display
//   short: toogle display backlight
//   long: show menue page
// - page: menu page
//   short: next menu item
//   long: exec menu item
// - page: counter edit 
//   short: next digit/menu item
//   long: toogle digit edit mode/exec menu item
//   in digit edit mode: short=increment digit, long=leave digit edit mode
 enum { MENUTIMEOUT_DEFAULTPAGE=1, MENUTIMEOUT_BACKLIGHT };
class PMMenuButtonTimeout : public Alarm {
  uint16_t timeout;
  uint8_t action;
public:
  PMMenuButtonTimeout( uint16_t settime_sec, uint8_t setaction ) : Alarm(0), timeout(settime_sec), action(setaction) {
  }
  virtual ~PMMenuButtonTimeout() { 
  }
  void set() {
    if ( tick )  {
      sysclock.cancel(*this); tick=0; 
    }
    if ( (action==MENUTIMEOUT_DEFAULTPAGE && display.getpage()!=DISPLAY_PAGE_DEFAULT) ||
         (action==MENUTIMEOUT_BACKLIGHT && display.getbacklight()) ) {
      tick=seconds2ticks(timeout); 
      if ( tick )  sysclock.add(*this);
    }
  }
  virtual void trigger( AlarmClock& clock ) {
    tick=0;
    if ( action==MENUTIMEOUT_DEFAULTPAGE )  
      display.setpage(DISPLAY_PAGE_DEFAULT);
    else if ( action==MENUTIMEOUT_BACKLIGHT )  
      display.setbacklight(0);
  }
};

template <class DEVTYPE,uint8_t OFFSTATE=HIGH,uint8_t ONSTATE=LOW,WiringPinMode MODE=INPUT_PULLUP>
class PMMenuButton : public StateButton<OFFSTATE,ONSTATE,MODE> {
protected:
  DEVTYPE& device;
  PMMenuButtonTimeout menutimeout, backlighttimeout;
  typedef StateButton<OFFSTATE,ONSTATE,MODE> ButtonBaseType;
public:
  PMMenuButton(DEVTYPE& dev,uint8_t longpresstime=2) : 
      device(dev), 
      menutimeout(MENU_TIMEOUT,MENUTIMEOUT_DEFAULTPAGE),
      backlighttimeout(BACKLIGHT_TIMEOUT,MENUTIMEOUT_BACKLIGHT) {
    this->setLongPressTime(seconds2ticks(longpresstime));
  }
  virtual ~PMMenuButton() {
  }
  virtual void state( uint8_t s ) {
    ButtonBaseType::state(s);
    if ( s==ButtonBaseType::released )
      shortpressed();
    else if ( s==ButtonBaseType::longpressed )
      longpressed();
  }
protected:
  void shortpressed() {
    uint8_t curpage=display.getpage();
    if ( curpage==DISPLAY_PAGE_DEFAULT ) {
      display.tooglebacklight();
    } else if ( display.getmenuedit() ) {
      display.nextmenuedit();
    } else {
      bool b=display.nextmenupos();
      if ( b )  menutimeout.set();
      else      display.setpage(DISPLAY_PAGE_DEFAULT);
    }
    backlighttimeout.set();
  }
  void longpressed() {
    uint8_t curpage=display.getpage(); uint8_t nextpage=0xFF; uint8_t showack=0;
    if ( curpage==DISPLAY_PAGE_DEFAULT ) {
      display.setpage( DISPLAY_PAGE_MENU1 );
    } else { // action!
      uint8_t menu=display.getmenu(); uint64_t val=0;
      switch ( menu ) {
        case DISPLAY_MENU_CANCEL:        nextpage=DISPLAY_PAGE_DEFAULT; break;
        case DISPLAY_MENU_SETCOUNTER1:   nextpage=DISPLAY_PAGE_SETCOUNTER1; break;
        case DISPLAY_MENU_SETCOUNTER2:   nextpage=DISPLAY_PAGE_SETCOUNTER2; break;
        case DISPLAY_MENU_SAVECOUNTERS:  _gPersistMem->save(); nextpage=DISPLAY_PAGE_DEFAULT; showack=true; break;
        case DISPLAY_MENU_STARTPAIRING:  device.startPairing(); nextpage=DISPLAY_PAGE_DEFAULT; break;
        case DISPLAY_MENU_RESETDEVICE:   _gPersistMem->reset(); device.reset(); break;
        case DISPLAY_MENU_RESETCONFIRM:  nextpage=DISPLAY_PAGE_RESETDEVICE; break;
        case DISPLAY_MENU_SAVECOUNTER1:  display.getmenueditvalue(val); savecounter(1,val); nextpage=DISPLAY_PAGE_DEFAULT; break;
        case DISPLAY_MENU_SAVECOUNTER2:  display.getmenueditvalue(val); savecounter(2,val); nextpage=DISPLAY_PAGE_DEFAULT; break;
        default:                         if ( display.menuposedit() ) if ( !display.tooglemenuedit() )  display.nextmenupos(); 
                                         break;
      }
    }
    if ( nextpage!=0xFF )  display.setpage(nextpage);
    if ( showack )   device.led().ledOn( millis2ticks(300) );
    stayawake(); menutimeout.set(); backlighttimeout.set();
  }
  void savecounter( uint8_t cntnr, uint64_t val ) {
    _gPersistMem->setsum(cntnr,val); // store persist
    device.channel(cntnr).firstinitclock(sysclock); // transmit
  }
  void stayawake() {
    device.activity().stayAwake( millis2ticks(500) );
  }
};



// asksinpp configuration
typedef AvrSPI<CC1101_CS_PIN,CC1101_MOSI_PIN,CC1101_MISO_PIN,CC1101_SCLK_PIN> SPIType;
typedef Radio<SPIType,CC1101_GD0_PIN,CC1101_PWR_PIN> RadioType;
typedef AskSin<PMLedType,PMBatterySensor,RadioType> HalType;

class MeterList0Data : public List0Data {
  uint8_t LocalResetDisbale : 1;   // 0x18 - 24
  uint8_t Baudrate          : 8;   // 0x23 - 35
  uint8_t SerialFormat      : 8;   // 0x24 - 36
  uint8_t MeterPowerMode    : 8;   // 0x25 - 37
  uint8_t MeterProtocolMode : 8;   // 0x26 - 38
  uint8_t SamplesPerCycle   : 8;   // 0x27 - 39

public:
  static uint8_t getOffset(uint8_t reg) {
    switch (reg) {
      case 0x18: return sizeof(List0Data) + 0;
      case 0x23: return sizeof(List0Data) + 1;
      case 0x24: return sizeof(List0Data) + 2;
      case 0x25: return sizeof(List0Data) + 3;
      case 0x26: return sizeof(List0Data) + 4;
      case 0x27: return sizeof(List0Data) + 5;
      default:   break;
    }
    return List0Data::getOffset(reg);
  }

  static uint8_t getRegister(uint8_t offset) {
    switch (offset) {
      case sizeof(List0Data) + 0:  return 0x18;
      case sizeof(List0Data) + 1:  return 0x23;
      case sizeof(List0Data) + 2:  return 0x24;
      case sizeof(List0Data) + 3:  return 0x25;
      case sizeof(List0Data) + 4:  return 0x26;
      case sizeof(List0Data) + 5:  return 0x27;
      default: break;
    }
    return List0Data::getRegister(offset);
  }
};

class MeterList0 : public ChannelList<MeterList0Data> {
public:
 MeterList0(uint16_t a) : ChannelList(a) {}

  operator List0& () const { return *(List0*)this; }

  // from List0
  HMID masterid () { return ((List0*)this)->masterid(); }
  void masterid (const HMID& mid) { ((List0*)this)->masterid(mid); }
  bool aesActive() const { return ((List0*)this)->aesActive(); }

  bool localResetDisable () const { return isBitSet(sizeof(List0Data) + 0,0x01); }
  bool localResetDisable (bool value) const { return setBit(sizeof(List0Data) + 0,0x01,value); }
  uint8_t baudrate () const { return getByte(sizeof(List0Data) + 1); }
  bool baudrate (uint8_t value) const { return setByte(sizeof(List0Data) + 1,value); }
  uint8_t serialFormat () const { return getByte(sizeof(List0Data) + 2); }
  bool serialFormat (uint8_t value) const { return setByte(sizeof(List0Data) + 2,value); }
  uint8_t powerMode () const { return getByte(sizeof(List0Data) + 3); }
  bool powerMode (uint8_t value) const { return setByte(sizeof(List0Data) + 3,value); }
  uint8_t protocolMode () const { return getByte(sizeof(List0Data) + 4); }
  bool protocolMode (uint8_t value) const { return setByte(sizeof(List0Data) + 4,value); }
  uint8_t samplesPerCycle () const { return getByte(sizeof(List0Data) + 5); }
  bool samplesPerCycle (uint8_t value) const { return setByte(sizeof(List0Data) + 5,value); }

  uint8_t transmitDevTryMax () const { return 6; }
  uint8_t ledMode () const { return 1; }

  void defaults () {
    ((List0*)this)->defaults();
  }
};

class MeterList1Data {
public:
  uint8_t  AesActive          :1;   // 0x08, s:0, e:1
  uint8_t  MeterType          :8;   // 0x95
  uint8_t  MeterSensibilityIR :8;   // 0x9c
  uint32_t TxThresholdPower   :24;  // 0x7C - 0x7E
  uint8_t  PowerString[16];         // 0x36 - 0x46 : 06 - 21
  uint8_t  EnergyCounterString[16]; // 0x47 - 0x57 : 22 - 37
  uint16_t MeterConstantIR    :16;  // 0x96 - 0x97 : 38 - 39
  uint16_t MeterConstantGas   :16;  // 0x98 - 0x99 : 40 - 41
  uint16_t MeterConstantLed   :16;  // 0x9a - 0x9b : 42 - 43

  static uint8_t getOffset(uint8_t reg) {
    switch (reg) {
      case 0x08: return 0;
      case 0x95: return 1;
      case 0x9c: return 2;
      case 0x7c: return 3;
      case 0x7d: return 4;
      case 0x7e: return 5;
      default: break;
    }
    if( reg >= 0x36 && reg <= 0x57 ) {
      return reg - 0x36 + 6;
    }
    if( reg >= 0x96 && reg <= 0x9b ) {
      return reg - 0x96 + 38;
    }
    return 0xff;
  }

  static uint8_t getRegister(uint8_t offset) {
    switch (offset) {
      case 0:  return 0x08;
      case 1:  return 0x95;
      case 2:  return 0x9c;
      case 3:  return 0x7c;
      case 4:  return 0x7d;
      case 5:  return 0x7e;
      default: break;
    }
    if( offset >= 6 && offset <= 37 ) {
      return offset - 6 + 0x36;
    }
    if( offset >= 38 && offset <= 43 ) {
      return offset - 38 + 0x96;
    }
    return 0xff;
  }
};

class MeterList1 : public ChannelList<MeterList1Data> {
public:
  MeterList1(uint16_t a) : ChannelList(a) {}

  bool aesActive () const { return isBitSet(0,0x01); }
  bool aesActive (bool s) const { return setBit(0,0x01,s); }
  uint8_t meterType () const { return getByte(1); }
  bool meterType (uint8_t value) const { return setByte(1,value); }
  uint8_t meterSensibilty () const { return getByte(2); }
  bool meterSensibilty (uint8_t value) const { return setByte(2,value); }
  uint32_t thresholdPower () const { return ((uint32_t)getByte(3)<<16) + ((uint16_t)getByte(4)<<8) + getByte(5); }
  bool thresholdPower (uint32_t value) const { return setByte(3,(value>>16)&0xff) && setByte(4,(value>>8)&0xff) && setByte(5,value&0xff); }

  uint16_t constantIR () const { return ((uint16_t)getByte(38)<<8) + getByte(39); }
  bool constantIR (uint16_t value) const { return setByte(38,(value>>8)&0xff) && setByte(39,value&0xff); }
  uint16_t constantGas () const { return ((uint16_t)getByte(40)<<8) + getByte(41); }
  bool constantGas (uint16_t value) const { return setByte(40,(value>>8)&0xff) && setByte(41,value&0xff); }
  uint16_t constantLed () const { return ((uint16_t)getByte(42)<<8) + getByte(43); }
  bool constantLed (uint16_t value) const { return setByte(42,(value>>8)&0xff) && setByte(43,value&0xff); }

  void defaults () {
    aesActive(false);
    meterType(0xff);
    meterSensibilty(0);
    thresholdPower(100*100);
    constantIR(100);
    constantGas(10);
    constantLed(10000);
  }
};

class GasPowerEventMsg : public Message {
public:
  void init(uint8_t msgcnt,bool boot,const uint64_t& counter,const uint32_t& power) {
    uint8_t cnt1 = (counter >> 24) & 0x7f;
    if( boot == true ) {
      cnt1 |= 0x80;
    }
    Message::init(0x10,msgcnt,0x54,BIDI|WKMEUP,cnt1,(counter >> 16) & 0xff);
    pload[0] = (counter >> 8) & 0xff;
    pload[1] = counter & 0xff;
    pload[2] = (power >> 16) & 0xff;
    pload[3] = (power >> 8) & 0xff;
    pload[4] = power & 0xff;
  }
};

class GasPowerEventCycleMsg : public GasPowerEventMsg {
public:
  void init(uint8_t msgcnt,bool boot,const uint64_t& counter,const uint32_t& power) {
    GasPowerEventMsg::init(msgcnt,boot,counter,power);
    typ = 0x53;
  }
};

class PowerEventMsg : public Message {
public:
  void init(uint8_t msgcnt,bool boot,const uint64_t& counter,const uint32_t& power) {
    uint8_t cnt1 = (counter >> 16) & 0x7f;
    if( boot == true ) {
      cnt1 |= 0x80;
    }
    Message::init(0x0f,msgcnt,0x5f,BIDI|WKMEUP,cnt1,(counter >> 8) & 0xff);
    pload[0] = counter & 0xff;
    pload[1] = (power >> 16) & 0xff;
    pload[2] = (power >> 8) & 0xff;
    pload[3] = power & 0xff;
  }
};

class PowerEventCycleMsg : public PowerEventMsg {
public:
  void init(uint8_t msgcnt,bool boot,const uint64_t& counter,const uint32_t& power) {
    PowerEventMsg::init(msgcnt,boot,counter,power);
    typ = 0x5e;
  }
};

class IECEventMsg : public Message {
public:
  void init(uint8_t msgcnt,uint8_t channel,const uint64_t& counter,const uint32_t& power,bool lowbat) {
    uint8_t cnt1 = channel & 0x3f;
    if( lowbat == true ) {
      cnt1 |= 0x40;
    }
    Message::init(0x15,msgcnt,0x61,BIDI|WKMEUP,cnt1,0x00);
    pload[0] = (counter >> 32) & 0xff;
    pload[1] = (counter >> 24) & 0xff;
    pload[2] = (counter >> 16) & 0xff;
    pload[3] = (counter >>  8) & 0xff;
    pload[4] = counter & 0xff;
    pload[5] = 0x00; //
    pload[6] = (power >> 24) & 0xff;
    pload[7] = (power >> 16) & 0xff;
    pload[8] = (power >>  8) & 0xff;
    pload[9] = power & 0xff;
  }
};

class IECEventCycleMsg : public IECEventMsg {
public:
  void init(uint8_t msgcnt,uint8_t channel,const uint64_t& counter,const uint32_t& power,bool lowbat) {
    IECEventMsg::init(msgcnt,channel,counter,power,lowbat);
    typ = 0x60;
  }
};

class MeterChannel : public Channel<HalType,MeterList1,EmptyList,List4,PEERS_PER_CHANNEL,MeterList0>, public Alarm {
  const uint32_t    maxVal = 838860700;
  // uint64_t          counterSum;
  // volatile uint32_t counter; // declare as volatile because of usage withing interrupt
  Message           msg;
  uint8_t           msgcnt;
  // bool              boot;
private:
public:
  MeterChannel () : Channel(), Alarm(0), /*counterSum(0), counter(0),*/ msgcnt(0) /*, boot(true)*/ {}
  virtual ~MeterChannel () {}

  void firstinit () {
    Channel<HalType,MeterList1,EmptyList,List4,PEERS_PER_CHANNEL,MeterList0>::firstinit();
    getList1().meterType(number()==1 ? 1 : 8);  // Channel 1 default Gas / Channel 2 default IEC
  }

  void firstinitclock(AlarmClock& clock) {
    if ( tick )  sysclock.cancel(*this);
    tick=FIRST_MSG_CYCLE; sysclock.add(*this);
  }

  uint8_t status () const {
    return 0;
  }

  uint8_t flags () const {
    return device().battery().low() ? 0x80 : 0x00;
  }

  void next () {
    // only count rotations/flashes and calculate real value when sending, to prevent inaccuracy
    uint16_t inc=(number()==1?COUNTER1_INC:COUNTER2_INC);
    uint32_t counter=PersistMem.nextcount( number(), inc );
    display.setcounter( number(), PersistMem.getsum(number()) );

    // device().led().ledOn(millis2ticks(300));
    #ifndef NDEBUG
      DPRINT(F("cnt")); DDEC(number()); DPRINT(F(" ")); DHEXLN(counter);
    #endif
  }

  virtual void trigger (AlarmClock& clock) {
    tick = MSG_CYCLE;
    clock.add(*this);

    uint32_t consumptionSum;
    uint32_t actualConsumption=0;

    MeterList1 l1 = getList1();
    uint8_t metertype = l1.meterType(); // cache metertype to reduce eeprom access
    if( metertype == 0 ) {
      return;
    }

    // copy value, to be consistent during calculation (counter may change when an interrupt is triggered)
    /*
    uint32_t c;
    ATOMIC_BLOCK( ATOMIC_RESTORESTATE )
    {
      c = counter;
      counter = 0;
    }
    counterSum += c;
    */
    uint64_t counterSum; uint32_t c; bool boot;
    PersistMem.updatesum( number(), &c, &counterSum );
    boot=PersistMem.resetboot( number() );

    
    uint16_t sigs = 1;
    switch( metertype ) {
      case 1: sigs = l1.constantGas(); break;
      case 2: sigs = l1.constantIR(); break;
      case 4: sigs = l1.constantLed(); break;
      default: break;
    }
    #if defined(COUNTER1_SIGS) && (COUNTER1_SIGS>0)
    if ( number()==1 )   sigs=COUNTER1_SIGS;
    #endif
    #if defined(COUNTER2_SIGS) && (COUNTER2_SIGS>0)
    if ( number()==2 )   sigs=COUNTER2_SIGS;
    #endif

    switch( metertype ) {
    case 1:
      consumptionSum = counterSum * sigs;

      actualConsumption = (c * sigs * 10) / (MSG_CYCLE / seconds2ticks(60));
      //!! for development: transmit battery voltage as actualConsumption to log a discharge diagram
      #ifndef NDEBUGBAT
      actualConsumption=((uint32_t)device().battery().value())*1000;      
      #endif

      // TODO handle overflow
      
      ((GasPowerEventCycleMsg&)msg).init(msgcnt++,boot,consumptionSum,actualConsumption);
      break;
    case 2: 
    case 4: 
      // calculate sum
      consumptionSum = (10000 * counterSum / sigs);
      // TODO handle overflow
      
      // calculate consumption whithin the last MSG_CYCLE period
      actualConsumption = (60 * 100000 * c) / (sigs * (MSG_CYCLE / seconds2ticks(60)));
      ((PowerEventCycleMsg&)msg).init(msgcnt++,boot,consumptionSum,actualConsumption);
      // DHEX(consumptionSum); DHEX(consumptionSum); 
      break;
    case 8: {
      uint32_t mul=(number()==1?COUNTER1_ICEMUL:COUNTER2_ICEMUL);
      uint64_t counterSumCCU=(counterSum*mul)/sigs; 
      actualConsumption=( (60*60*c)/(sigs * (MSG_CYCLE/seconds2ticks(1))) )*mul;
      ((IECEventCycleMsg&)msg).init(msgcnt++,number(),counterSumCCU,actualConsumption,device().battery().low());
    } break;
    default:
      DPRINTLN("Unknown meter type");
      return;
      break;
    }

    #ifndef NDEBUG
    char *msgtype=0;
    if ( metertype==1 )   msgtype="GasPowerMsg";
    if ( metertype==2 || metertype==4 )   msgtype="PowerMsg";
    if ( metertype==8 )   msgtype="IECMsg";
    if ( msgtype ) { 
      DPRINT(msgtype); 
      DPRINT(" cnt "); uint32_t cs=counterSum; DHEX(cs);
      DPRINT(" psum "); DHEX(consumptionSum); 
      DPRINT(" pact "); DHEX(actualConsumption); 
      DPRINT(" sigs "); DHEX(sigs); 
      DPRINTLN(""); }
    #endif

    device().sendPeerEvent(msg,*this);

    device().battery().measure(1);
    
    // boot = false;
  }
};

// asksinpp hal and device
typedef MultiChannelDevice<HalType,MeterChannel,2,MeterList0> MeterType;
HalType hal;
MeterType sdev(devinfo,0x20);
PMMenuButton<MeterType> cfgBtn(sdev);

// counter input handling by interrupt
template <uint8_t pin, uint8_t pinmode, void (*isr)(), uint16_t millis>
class ISRWrapper : public Alarm {
  uint8_t curstate;
public:
  ISRWrapper () : Alarm(0), curstate(HIGH) {
    pinMode(pin,pinmode);
  }
  virtual ~ISRWrapper () {
  }
  void init( AlarmClock& clock ) {
    attach();
  }
  bool checkstate () {
    uint8_t oldstate=curstate;
    curstate=digitalRead(pin);
    return curstate != oldstate;
  }
  uint8_t state () const {
    return curstate;
  }
  void attach() {
    if( digitalPinToInterrupt(pin)==NOT_AN_INTERRUPT )   enableInterrupt(pin,isr,CHANGE);
    else                                                 attachInterrupt(digitalPinToInterrupt(pin),isr,CHANGE);
  }
  void detach () {
    if( digitalPinToInterrupt(pin)==NOT_AN_INTERRUPT )   disableInterrupt(pin);
    else                                                 detachInterrupt(digitalPinToInterrupt(pin));
  }
  void debounce () {
    detach();
    tick=millis2ticks(millis);
    sysclock.add(*this);
  }
  virtual void trigger (__attribute__ ((unused)) AlarmClock& clock) {
    checkstate();
    attach();
  }
};

#if COUNTER1_MODE==1
void counter1ISR(); 
ISRWrapper<COUNTER1_PIN,COUNTER1_PINMODE,counter1ISR,COUNTER1_DEBOUNCE> c1Handler;
void counter1ISR () {
  c1Handler.debounce();
  if( c1Handler.checkstate() ) {
    display.setinput( 1, c1Handler.state() );
    if( c1Handler.state()==LOW ) 
      sdev.channel(1).next();
  }
}
#endif

#if COUNTER2_MODE==1
void counter2ISR();
ISRWrapper<COUNTER2_PIN,COUNTER2_PINMODE,counter2ISR,COUNTER2_DEBOUNCE> c2Handler;
void counter2ISR () {
  c2Handler.debounce();
  if( c2Handler.checkstate() ) {
    display.setinput( 2, c2Handler.state() );
    if( c2Handler.state()==LOW )
      sdev.channel(2).next();
  }
}
#endif

// counter input handling by polling
#define UNKNOWN_STATE 0xFF
template <uint8_t channelnr, uint8_t pin, uint8_t pinmode, uint8_t pwrpin, uint16_t pollmillies, uint16_t debouncemillis>
class InputPollHandler : public Alarm {
  uint8_t curstate;
public:
  InputPollHandler() {
  }
  void init( AlarmClock& clock ) {
    curstate=_gPersistMem->getstate(channelnr);
    pinMode( pin, pinmode );
    tick=millis2ticks(pollmillies); clock.add(*this);
  }
  uint8_t read() {
    if ( pwrpin!=0xFF ) {
      pinMode( pwrpin, OUTPUT );
      digitalWrite( pwrpin, HIGH );
      delayMicroseconds( 30 );
    }
    uint8_t state=digitalRead( pin );
    if ( pwrpin!=0xFF ) {
      pinMode( pwrpin, INPUT );
    }
    return state;
  }
  void pinchange() {
    display.setinput( channelnr, curstate );
    if ( curstate==LOW ) 
       sdev.channel(channelnr).next();
  }
  virtual void trigger (AlarmClock& clock) {
    uint8_t state=read(); uint32_t next=pollmillies;
    if ( curstate==UNKNOWN_STATE )  
      curstate=state;
    else if ( state!=curstate ) {
      curstate=state; _gPersistMem->setstate( channelnr, state );
      if ( next<debouncemillis )  next=debouncemillis;
      pinchange();
    }
    tick=millis2ticks(next); clock.add(*this);
  }
};

#if COUNTER1_MODE==2
InputPollHandler<1,COUNTER1_PIN,COUNTER1_PINMODE,COUNTER1_PWRPIN,COUNTER1_POLL,COUNTER1_DEBOUNCE> c1Handler;
#endif

#if COUNTER2_MODE==2
InputPollHandler<2,COUNTER2_PIN,COUNTER2_PINMODE,COUNTER2_PWRPIN,COUNTER2_POLL,COUNTER2_DEBOUNCE> c2Handler;
#endif


void setup () {
  DINIT(57600,ASKSIN_PLUS_PLUS_IDENTIFIER);
/*   -> measure internal ADC ref (AREF = ATMega328P pin 20) with a multimeter
        (should be around 1100mV) to calibrate battery voltage measurement 
     -> set BAT_ADC_INTERNAL_REF and check debug output
  DPRINTLN("please measure adc ref");
  analogReference(INTERNAL); analogRead(0); delay( 8000 );
  DPRINTLN("done"); Idle<>::waitSerial();
*/
  // setup user storage
  PersistMem.init( sdev.getUserStorage().getAddress() );
  // display init
  display.init( PersistMem.getstate(1), PersistMem.getstate(2) );
  // device init
  sdev.init(hal);
  // a little hack - check for recorded init errors by checking the led mode
  //  the only error state used by AskSinPP.h is a CC1101 init error
  if ( sdev.led().geterrorcnt() ) {
    // try to reset CC1101
    sdev.radio().setIdle(); 
    uint8_t pins[4]={CC1101_CS_PIN,CC1101_MOSI_PIN,CC1101_MISO_PIN,CC1101_SCLK_PIN};
    for ( uint8_t u=0; u<4; u++ )   pinMode( pins[u], INPUT );
    digitalWrite( CC1101_PWR_PIN, HIGH );
    display.setpage(DISPLAY_PAGE_CC1101ERROR); display.update();
    delay(3000); digitalWrite( CC1101_PWR_PIN, LOW ); delay(500);
    bool ok=sdev.radio().init();
    if ( ok )  sdev.init(hal);
    else       hal.activity.sleepForever(hal); // do not continue - sending message will hang!
  }
  // setup config button
  buttonISR(cfgBtn,CONFIG_BUTTON_PIN);
  // measure battery every 1h
  hal.battery.init(seconds2ticks(60UL*60),sysclock);
  // set battery parameters
  hal.battery.low( BAT_LOW );
  hal.battery.critical( BAT_CRITICAL );
  // setup counters
  #if COUNTER1_MODE>0
  c1Handler.init(sysclock);
  sdev.channel(1).firstinitclock(sysclock);
  display.setcounter( 1, PersistMem.getsum(1) );
  #endif
  #if COUNTER2_MODE>0
  c2Handler.init(sysclock);
  sdev.channel(2).firstinitclock(sysclock);
  display.setcounter( 2, PersistMem.getsum(2) );
  #endif
  sdev.initDone();
}

void loop() {
  bool worked = hal.runready();
  bool poll = sdev.pollRadio();
  bool displayupdate = display.update();
  if( worked==false && poll==false && displayupdate==false ) {
    if ( hal.battery.critical() ) {
      // if battery is critical: end up all here before getting a bubbling idiot
      PersistMem.save();
      hal.activity.sleepForever(hal);
    }
    #ifdef SAVEPOWER
    hal.activity.savePower< Sleep<> >(hal);
    #else
    hal.activity.savePower< Idle<> >(hal);
    #endif
  }
}
