#include "noven_counter.h"
 extern bool Sync_completed;
extern volatile uint64_t oldmillis;

APP_TIMER_DEF(msec_timer_id);
#define MS_INTERVAL     APP_TIMER_TICKS(1) 

extern bool tick;
static uint64_t msec_Counter;
static uint64_t currentmillis;

void msec_timers_init(void)
{

   ret_code_t err_code = app_timer_create(&msec_timer_id,
                                APP_TIMER_MODE_REPEATED,
                                Msec_tick_handler);
    APP_ERROR_CHECK(err_code);


}



 static void Msec_tick_handler()
{

 msec_Counter++;  
tick = true;
// this portion can be handled from main loop.but accuracy of toggling reduces
 //comment below lines to run the pin toggling task from main loop

// once Sync is completed, it will sync the led update counter to make the sync on master & slave as close as possible
   
 /*    if (Sync_completed)

     {  Sync_completed  =false;
        
         oldmillis= getCounter();
         bsp_board_led_off(3);
     }  */
            
        if (tick)  // code to toggle pin, every 10ms
      {
        
          currentmillis = getCounter();
        
        if ((currentmillis-oldmillis)==10)
        { oldmillis=currentmillis;
            
            bsp_board_led_invert(3);

        }
         
         tick = false;
      }
 

}





/**
*  Returns the current value of the millisecond counter
*
**/
uint64_t getCounter()
{

return msec_Counter;

}

/**
* Sets the value of the millisecond counter
*
*/
void setCounter(uint64_t newCounterValue)
{

msec_Counter = newCounterValue;

}

/**
* Starts the millisecond counter.
*/
void startCounter(){


  ret_code_t  err_code;
 
    // Start application timers.
    err_code = app_timer_start(msec_timer_id, MS_INTERVAL , NULL);
    APP_ERROR_CHECK(err_code);


}

/**
* Stops the millisecond counter
*/
void stopCounter()
{


 ret_code_t  err_code;
 err_code = app_timer_stop(msec_timer_id);
 APP_ERROR_CHECK(err_code);

}

