
#include "noven_timersync.h"
#include "noven_counter.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "app_error.h"
#include "app_util_platform.h"
#include "nrf.h"
#include "nrf_atomic.h"
#include "nrf_balloc.h"
#include "nrf_error.h"
#include "nrf_soc.h"
#include "nrf_sdh_soc.h"
#include "nrf_sdm.h"

#define NRF_LOG_MODULE_NAME time_sync
#define NRF_LOG_LEVEL 4
#include "nrf_log.h"
NRF_LOG_MODULE_REGISTER();

static void ts_on_sys_evt(uint32_t sys_evt, void * p_context);

NRF_SDH_SOC_OBSERVER(timesync_soc_obs,     \
                     TS_SOC_OBSERVER_PRIO, \
                     ts_on_sys_evt, 0);

#define TS_LEN_US                            (1000UL)
#define TX_LEN_EXTENSION_US                  (1000UL)
#define TS_SAFETY_MARGIN_US                  (500UL)   /**< The timeslot activity should be finished with this much to spare. */
#define TS_EXTEND_MARGIN_US                  (700UL)   /**< The timeslot activity should request an extension this long before end of timeslot. */


#define MAIN_DEBUG                           0x12345678UL

static void ppi_radio_rx_disable(void);
static void ppi_radio_rx_configure(void);
static void ppi_radio_tx_configure(void);



typedef PACKED_STRUCT
{
   uint64_t millis_counter ;
} sync_pkt_t;

NRF_BALLOC_DEF(m_sync_pkt_pool, sizeof(sync_pkt_t), 10);

static volatile bool     m_timeslot_session_open;
static volatile uint32_t m_blocked_cancelled_count;
static uint32_t          m_total_timeslot_length = 0;
static uint32_t          m_timeslot_distance = 40;
static ts_params_t       m_params;

static volatile bool m_send_sync_pkt = false;
static volatile bool m_timer_update_in_progress = false;


static volatile uint32_t m_master_counter = 0;
static volatile uint32_t m_rcv_count      = 0;

static volatile sync_pkt_t * mp_curr_adj_pkt;



uint8_t sync_counter=0;
uint64_t currentmilliscounter;
extern bool Sync_completed;
 

static volatile enum
{
    RADIO_STATE_IDLE, /* Default state */
    RADIO_STATE_RX,   /* Waiting for packets */
    RADIO_STATE_TX    /* Trying to transmit packet */
} m_radio_state = RADIO_STATE_IDLE;

//static bool sync_timer_offset_compensate(sync_pkt_t * p_pkt);
static void timeslot_begin_handler(void);
static void timeslot_end_handler(void);

/**< This will be used when requesting the first timeslot or any time a timeslot is blocked or cancelled. */
static nrf_radio_request_t m_timeslot_req_earliest = {
        NRF_RADIO_REQ_TYPE_EARLIEST,
        .params.earliest = {
            NRF_RADIO_HFCLK_CFG_XTAL_GUARANTEED,
            NRF_RADIO_PRIORITY_NORMAL,
            TS_LEN_US,
            NRF_RADIO_EARLIEST_TIMEOUT_MAX_US
        }};

/**< This will be used at the end of each timeslot to request the next timeslot. */
static nrf_radio_request_t m_timeslot_req_normal = {
        NRF_RADIO_REQ_TYPE_NORMAL,
        .params.normal = {
            NRF_RADIO_HFCLK_CFG_XTAL_GUARANTEED,
            NRF_RADIO_PRIORITY_NORMAL,
            0,
            TS_LEN_US
        }};

/**< This will be used at the end of each timeslot to request the next timeslot. */
static nrf_radio_signal_callback_return_param_t m_rsc_return_sched_next_normal = {
        NRF_RADIO_SIGNAL_CALLBACK_ACTION_REQUEST_AND_END,
        .params.request = {
                (nrf_radio_request_t*) &m_timeslot_req_normal
        }};

/**< This will be used at the end of each timeslot to request the next timeslot. */
static nrf_radio_signal_callback_return_param_t m_rsc_return_sched_next_earliest = {
        NRF_RADIO_SIGNAL_CALLBACK_ACTION_REQUEST_AND_END,
        .params.request = {
                (nrf_radio_request_t*) &m_timeslot_req_earliest
        }};

/**< This will be used at the end of each timeslot to request an extension of the timeslot. */
static nrf_radio_signal_callback_return_param_t m_rsc_extend = {
        NRF_RADIO_SIGNAL_CALLBACK_ACTION_EXTEND,
        .params.extend = {TX_LEN_EXTENSION_US}
        };

/**< This will be used at the end of each timeslot to request the next timeslot. */
static nrf_radio_signal_callback_return_param_t m_rsc_return_no_action = {
        NRF_RADIO_SIGNAL_CALLBACK_ACTION_NONE,
        .params.request = {NULL}
        };

void RADIO_IRQHandler(void)
{
    if (NRF_RADIO->EVENTS_END != 0)
    {
        NRF_RADIO->EVENTS_END = 0;
        (void)NRF_RADIO->EVENTS_END;

        if (m_radio_state == RADIO_STATE_RX &&
           (NRF_RADIO->CRCSTATUS & RADIO_CRCSTATUS_CRCSTATUS_Msk) == (RADIO_CRCSTATUS_CRCSTATUS_CRCOk << RADIO_CRCSTATUS_CRCSTATUS_Pos))
        {
            sync_pkt_t * p_pkt;
            bool         adjustment_procedure_started;

            p_pkt = (sync_pkt_t *) NRF_RADIO->PACKETPTR;
///////////////////////////on received updating the local counter. you can diable the below 2 lines to reduce time difference after debugging.
             currentmilliscounter = getCounter();
             NRF_LOG_INFO("timestamp: M 0x%d, S 0x%d \r\n",p_pkt->millis_counter,currentmilliscounter);
////////////////////////////////////////////////////////////////////////////////////////////////////////
             startslavesync(p_pkt->millis_counter);
             Sync_completed = true;
            
      }

        NRF_RADIO->TASKS_START = 1;
    }
}

/**@brief   Function for handling timeslot events.
 */
static nrf_radio_signal_callback_return_param_t * radio_callback (uint8_t signal_type)
{
    // NOTE: This callback runs at lower-stack priority (the highest priority possible).
    switch (signal_type) {
    case NRF_RADIO_CALLBACK_SIGNAL_TYPE_START:
        // TIMER0 is pre-configured for 1Mhz.
        NRF_TIMER0->TASKS_STOP          = 1;
        NRF_TIMER0->TASKS_CLEAR         = 1;
        NRF_TIMER0->MODE                = (TIMER_MODE_MODE_Timer << TIMER_MODE_MODE_Pos);
        NRF_TIMER0->EVENTS_COMPARE[0]   = 0;
        NRF_TIMER0->EVENTS_COMPARE[1]   = 0;

        if (m_send_sync_pkt)
        {
            NRF_TIMER0->INTENSET  = (TIMER_INTENSET_COMPARE0_Set << TIMER_INTENSET_COMPARE0_Pos);
        }
        else
        {
            NRF_TIMER0->INTENSET = (TIMER_INTENSET_COMPARE0_Set << TIMER_INTENSET_COMPARE0_Pos) |
                                   (TIMER_INTENSET_COMPARE1_Set << TIMER_INTENSET_COMPARE1_Pos);
        }
        NRF_TIMER0->CC[0]               = (TS_LEN_US - TS_SAFETY_MARGIN_US);
        NRF_TIMER0->CC[1]               = (TS_LEN_US - TS_EXTEND_MARGIN_US);
        NRF_TIMER0->BITMODE             = (TIMER_BITMODE_BITMODE_24Bit << TIMER_BITMODE_BITMODE_Pos);
        NRF_TIMER0->TASKS_START         = 1;


        NRF_RADIO->POWER                = (RADIO_POWER_POWER_Enabled << RADIO_POWER_POWER_Pos);

        NVIC_EnableIRQ(TIMER0_IRQn);

        m_total_timeslot_length = 0;

        timeslot_begin_handler();

        break;

    case NRF_RADIO_CALLBACK_SIGNAL_TYPE_TIMER0:
        if (NRF_TIMER0->EVENTS_COMPARE[0] &&
           (NRF_TIMER0->INTENSET & (TIMER_INTENSET_COMPARE0_Enabled << TIMER_INTENCLR_COMPARE0_Pos)))
        {
            NRF_TIMER0->TASKS_STOP  = 1;
            NRF_TIMER0->EVENTS_COMPARE[0] = 0;
            (void)NRF_TIMER0->EVENTS_COMPARE[0];

            // This is the "timeslot is about to end" timeout

            timeslot_end_handler();

            // Schedule next timeslot
            if (m_send_sync_pkt)
            {
                m_timeslot_req_normal.params.normal.distance_us = m_total_timeslot_length + m_timeslot_distance;
                return (nrf_radio_signal_callback_return_param_t*) &m_rsc_return_sched_next_normal;
            }
            else
            {
                return (nrf_radio_signal_callback_return_param_t*) &m_rsc_return_sched_next_earliest;
            }
        }

        if (NRF_TIMER0->EVENTS_COMPARE[1] &&
           (NRF_TIMER0->INTENSET & (TIMER_INTENSET_COMPARE1_Enabled << TIMER_INTENCLR_COMPARE1_Pos)))
        {
            NRF_TIMER0->EVENTS_COMPARE[1] = 0;
            (void)NRF_TIMER0->EVENTS_COMPARE[1];

            // This is the "try to extend timeslot" timeout

            if (m_total_timeslot_length < (128000000UL - 5000UL - TX_LEN_EXTENSION_US) && !m_send_sync_pkt)
            {
                // Request timeslot extension if total length does not exceed 128 seconds
                return (nrf_radio_signal_callback_return_param_t*) &m_rsc_extend;
            }
            else if (!m_send_sync_pkt)
            {
                // Don't do anything. Timeslot will end and new one requested upon the next timer0 compare.
            }
        }



    case NRF_RADIO_CALLBACK_SIGNAL_TYPE_RADIO:
        RADIO_IRQHandler();
        break;

    case NRF_RADIO_CALLBACK_SIGNAL_TYPE_EXTEND_FAILED:
        // Don't do anything. Our timer will expire before timeslot ends
        return (nrf_radio_signal_callback_return_param_t*) &m_rsc_return_no_action;

    case NRF_RADIO_CALLBACK_SIGNAL_TYPE_EXTEND_SUCCEEDED:
        // Extension succeeded: update timer
        NRF_TIMER0->TASKS_STOP          = 1;
        NRF_TIMER0->EVENTS_COMPARE[0]   = 0;
        NRF_TIMER0->EVENTS_COMPARE[1]   = 0;
        NRF_TIMER0->CC[0]               += (TX_LEN_EXTENSION_US - 25);
        NRF_TIMER0->CC[1]               += (TX_LEN_EXTENSION_US - 25);
        NRF_TIMER0->TASKS_START         = 1;

        // Keep track of total length
        m_total_timeslot_length += TX_LEN_EXTENSION_US;
        break;

    default:
        app_error_handler(MAIN_DEBUG, __LINE__, (const uint8_t*)__FILE__);
        break;
    };

    // Fall-through return: return with no action request
    return (nrf_radio_signal_callback_return_param_t*) &m_rsc_return_no_action;
}

static void update_radio_parameters(sync_pkt_t * p_pkt)
{
    // TX power
    NRF_RADIO->TXPOWER  = RADIO_TXPOWER_TXPOWER_0dBm   << RADIO_TXPOWER_TXPOWER_Pos;

    // RF bitrate
    NRF_RADIO->MODE     = RADIO_MODE_MODE_Nrf_2Mbit       << RADIO_MODE_MODE_Pos;

    // Fast startup mode
    NRF_RADIO->MODECNF0 = RADIO_MODECNF0_RU_Fast << RADIO_MODECNF0_RU_Pos;

    // CRC configuration
    NRF_RADIO->CRCCNF  = RADIO_CRCCNF_LEN_Three << RADIO_CRCCNF_LEN_Pos;
    NRF_RADIO->CRCINIT = 0xFFFFFFUL;      // Initial value
    NRF_RADIO->CRCPOLY = 0x11021UL;     // CRC poly: x^16+x^12^x^5+1

    // Packet format
    NRF_RADIO->PCNF0 = (0 << RADIO_PCNF0_S0LEN_Pos) | (0 << RADIO_PCNF0_LFLEN_Pos) | (0 << RADIO_PCNF0_S1LEN_Pos);
    NRF_RADIO->PCNF1 = (RADIO_PCNF1_WHITEEN_Disabled     << RADIO_PCNF1_WHITEEN_Pos) |
                       (RADIO_PCNF1_ENDIAN_Big           << RADIO_PCNF1_ENDIAN_Pos)  |
                       (4                                << RADIO_PCNF1_BALEN_Pos)   |
                       (sizeof(sync_pkt_t)               << RADIO_PCNF1_STATLEN_Pos) |
                       (sizeof(sync_pkt_t)               << RADIO_PCNF1_MAXLEN_Pos);
    NRF_RADIO->PACKETPTR = (uint32_t) p_pkt;

    // Radio address config
    NRF_RADIO->PREFIX0 = m_params.rf_addr[0];
    NRF_RADIO->BASE0   = (m_params.rf_addr[1] << 24 | m_params.rf_addr[2] << 16 | m_params.rf_addr[3] << 8 | m_params.rf_addr[4]);

    NRF_RADIO->TXADDRESS   = 0;
    NRF_RADIO->RXADDRESSES = (1 << 0);

    NRF_RADIO->FREQUENCY = m_params.rf_chn;
    NRF_RADIO->TXPOWER   = RADIO_TXPOWER_TXPOWER_Pos4dBm << RADIO_TXPOWER_TXPOWER_Pos;

    NRF_RADIO->EVENTS_END = 0;

    NRF_RADIO->INTENCLR = 0xFFFFFFFF;
    NRF_RADIO->INTENSET = RADIO_INTENSET_END_Msk;

    NVIC_EnableIRQ(RADIO_IRQn);
}

/**@brief IRQHandler used for execution context management.
  *        Any available handler can be used as we're not using the associated hardware.
  *        This handler is used to stop and disable UESB
  */
void timeslot_end_handler(void)
{
    NRF_RADIO->TASKS_DISABLE = 1;
    NRF_RADIO->INTENCLR      = 0xFFFFFFFF;

    ppi_radio_rx_disable();

    nrf_balloc_free(&m_sync_pkt_pool, (void*) NRF_RADIO->PACKETPTR);

    m_total_timeslot_length = 0;
    m_radio_state           = RADIO_STATE_IDLE;
}

/**@brief IRQHandler used for execution context management.
  *        Any available handler can be used as we're not using the associated hardware.
  *        This handler is used to initiate UESB RX/TX
  */
void timeslot_begin_handler(void)
{
    sync_pkt_t * p_pkt;
ret_code_t err_code;
    if (!m_send_sync_pkt)
    {
        if (m_radio_state    != RADIO_STATE_RX ||
            NRF_RADIO->STATE != (RADIO_STATE_STATE_Rx << RADIO_STATE_STATE_Pos))
        {
            p_pkt = nrf_balloc_alloc(&m_sync_pkt_pool);
            APP_ERROR_CHECK_BOOL(p_pkt != 0);

            update_radio_parameters(p_pkt);

            NRF_RADIO->SHORTS     = RADIO_SHORTS_READY_START_Msk;
            NRF_RADIO->TASKS_RXEN = 1;

            ppi_radio_rx_configure();

            m_radio_state = RADIO_STATE_RX;
        }

        return;
    }

    if (m_radio_state == RADIO_STATE_RX)
    {
        NRF_RADIO->EVENTS_DISABLED = 0;
        NRF_RADIO->TASKS_DISABLE = 1;
        while (NRF_RADIO->EVENTS_DISABLED == 0)
        {
            __NOP();
        }
    }

    p_pkt = nrf_balloc_alloc(&m_sync_pkt_pool);
    APP_ERROR_CHECK_BOOL(p_pkt != 0);

    ppi_radio_tx_configure();
    update_radio_parameters(p_pkt);

   
    NRF_RADIO->SHORTS     = RADIO_SHORTS_END_DISABLE_Msk | RADIO_SHORTS_READY_START_Msk;
    NRF_RADIO->TASKS_TXEN = 1;



    while (NRF_RADIO->EVENTS_READY == 0)
    {
        // PPI is used to trigger sync timer capture when radio is ready
        // Radio will automatically start transmitting once ready, so the captured timer value must be copied into radio packet buffer ASAP
        __NOP();
    }

 ////////////////CDEEC- transmission of Sync packet/////////////////////////////////// 
    p_pkt->millis_counter =  getCounter(); 

    m_radio_state = RADIO_STATE_TX;
    sync_counter++;

      
         if (sync_counter<=1)
        {  
     
           startslavesync(p_pkt->millis_counter);
    
     
          }
          else{
           sync_counter=0;
           err_code = ts_tx_stop();
           Sync_completed = true;
          }


 ///////////////////////////////////////////////////////////////////////////////////////


}

/**@brief Function for handling the Application's system events.
 *
 * @param[in]   sys_evt   system event.
 */
void ts_on_sys_evt(uint32_t sys_evt, void * p_context)
{
    switch(sys_evt)
    {
        case NRF_EVT_FLASH_OPERATION_SUCCESS:
        case NRF_EVT_FLASH_OPERATION_ERROR:
            break;
        case NRF_EVT_RADIO_BLOCKED:
        case NRF_EVT_RADIO_CANCELED:
        {
            // Blocked events are rescheduled with normal priority. They could also
            // be rescheduled with high priority if necessary.
            uint32_t err_code = sd_radio_request((nrf_radio_request_t*) &m_timeslot_req_earliest);
            APP_ERROR_CHECK(err_code);

            m_blocked_cancelled_count++;

            break;
        }
        case NRF_EVT_RADIO_SIGNAL_CALLBACK_INVALID_RETURN:
            NRF_LOG_ERROR("NRF_EVT_RADIO_SIGNAL_CALLBACK_INVALID_RETURN\r\n");
            app_error_handler(MAIN_DEBUG, __LINE__, (const uint8_t*)__FILE__);
            break;
        case NRF_EVT_RADIO_SESSION_CLOSED:
            {
                m_timeslot_session_open = false;

                NRF_LOG_INFO("NRF_EVT_RADIO_SESSION_CLOSED\r\n");
            }

            break;
        case NRF_EVT_RADIO_SESSION_IDLE:
        {
            NRF_LOG_INFO("NRF_EVT_RADIO_SESSION_IDLE\r\n");

            uint32_t err_code = sd_radio_session_close();
            APP_ERROR_CHECK(err_code);
            break;
        }
        default:
            // No implementation needed.
            NRF_LOG_INFO("Event: 0x%08x\r\n", sys_evt);
            break;
    }
}


static void ppi_radio_rx_configure(void)
{
    uint32_t chn;

    chn = m_params.ppi_chns[2];

    NRF_PPI->CH[chn].EEP   = (uint32_t) &NRF_RADIO->EVENTS_ADDRESS;
    NRF_PPI->CHENSET       = (1 << chn);
}

static void ppi_radio_tx_configure(void)
{
    uint32_t chn;

    chn = m_params.ppi_chns[0];

    NRF_PPI->CH[chn].EEP   = (uint32_t) &NRF_RADIO->EVENTS_READY;
    NRF_PPI->CHENSET       = (1 << chn);
}

static void ppi_radio_rx_disable(void)
{
    uint32_t chn;

    chn = m_params.ppi_chns[2];

    NRF_PPI->CHENCLR = (1 << chn);
}


uint32_t ts_init(const ts_params_t * p_params)
{
    memcpy(&m_params, p_params, sizeof(ts_params_t));

    if (m_params.egu                == 0)
    {
        
        return NRF_ERROR_INVALID_PARAM;
    }

    if (m_params.egu != NRF_EGU3)
    {
        
        return NRF_ERROR_INVALID_PARAM;
    }

    

    APP_ERROR_CHECK(nrf_balloc_init(&m_sync_pkt_pool));

    return NRF_SUCCESS;
}

uint32_t ts_enable(void)
{
    uint32_t err_code;

    if (m_timeslot_session_open)
    {
        return NRF_ERROR_INVALID_STATE;
    }

    err_code = sd_clock_hfclk_request();
    if (err_code != NRF_SUCCESS)
    {
        return err_code;
    }

    err_code |= sd_power_mode_set(NRF_POWER_MODE_CONSTLAT);
    if (err_code != NRF_SUCCESS)
    {
        return err_code;
    }

    err_code = sd_radio_session_open(radio_callback);
    if (err_code != NRF_SUCCESS)
    {
        return err_code;
    }

    err_code = sd_radio_request(&m_timeslot_req_earliest);
    if (err_code != NRF_SUCCESS)
    {
        return err_code;
    }



    NVIC_ClearPendingIRQ(m_params.egu_irq_type);
    NVIC_SetPriority(m_params.egu_irq_type, 7);
    NVIC_EnableIRQ(m_params.egu_irq_type);

    m_params.egu->INTENCLR = 0xFFFFFFFF;
    m_params.egu->INTENSET = EGU_INTENSET_TRIGGERED0_Msk;

    m_blocked_cancelled_count  = 0;
    m_send_sync_pkt            = false;
    m_radio_state              = RADIO_STATE_IDLE;


    m_timeslot_session_open    = true;

    return NRF_SUCCESS;
}


uint32_t ts_tx_start()
{
   

    m_send_sync_pkt = true;
       return NRF_SUCCESS;
}

uint32_t ts_tx_stop()
{
    m_send_sync_pkt = false;

    return NRF_SUCCESS;
}


void startslavesync(uint64_t millis)
{
 stopCounter();
 //bsp_board_led_off(2);
 //oldmillis=millis;
 setCounter(millis);
 startCounter();

}
