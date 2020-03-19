#include "bsp_btn_ble.h"
#include "app_timer.h"

void msec_timers_init(void);   //call this funtion from main to initialise the timer

static void Msec_tick_handler();


//uint64_t currentmillis,oldmillis;
/**
*  Returns the current value of the millisecond counter
*
**/
uint64_t getCounter();

/**
* Sets the value of the millisecond counter
*
*/
void setCounter(uint64_t newCounterValue);

/**
* Starts the millisecond counter.
*/
void startCounter();

/**
* Stops the millisecond counter
*/
void stopCounter();

