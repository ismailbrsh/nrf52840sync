
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#ifndef __NOVEN_TIMER_SYNC_H__
#define __NOVEN_TIMER_SYNC_H__

#include <stdbool.h>
#include <stdint.h>

#include "nrf.h"

#define TS_SOC_OBSERVER_PRIO 0

#define TIME_SYNC_TIMER_MAX_VAL (40000)
#define TIME_SYNC_RTC_MAX_VAL   (0xFFFFFF)

/**@brief Data handler type. */
typedef void (*ts_evt_handler_t)(uint32_t time);

typedef struct
{
    uint8_t          rf_chn;          /** RF Channel [0-80] */
    uint8_t          rf_addr[5];      /** 5-byte RF address */
    uint8_t          ppi_chns[4];     /** PPI channels */
    uint8_t          ppi_chg;        /** PPI Channel Group */
    NRF_EGU_Type   * egu;
    IRQn_Type        egu_irq_type;
} ts_params_t;

/**@brief Initialize time sync library
 * 
 * @param[in] p_params Parameters
 *
 * @retval NRF_SUCCESS if successful 
 */
uint32_t ts_init(const ts_params_t * p_params);

/**@brief Enable time sync library. This will enable reception of sync packets.
 *
 * @retval NRF_SUCCESS if successful 
 */
uint32_t ts_enable(void);


/**@brief Start sync packet transmission (become timing master).
 *
 * @note @ref ts_enable() must be called prior to calling this function
 * @note Expect some jitter depending on BLE activity.
 *
 * 
 *
 * @retval NRF_SUCCESS if successful 
 */
uint32_t ts_tx_start();

/**@brief Stop sync packet transmission (become timing slave again).
 *
 * @retval NRF_SUCCESS if successful 
 */
uint32_t ts_tx_stop(void);



#endif /* __TIME_SYNC_H__ */

void startslavesync(uint64_t millis);