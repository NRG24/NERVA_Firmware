/* Syntax-check stub: the nrfx HAL calls wdt.c makes on an inherited watchdog. */
#ifndef ZSTUB_HAL_NRF_WDT_H_
#define ZSTUB_HAL_NRF_WDT_H_
#include <stdbool.h>
#include <stdint.h>
typedef struct { uint32_t RUNSTATUS; uint32_t RREN; uint32_t RR[8]; } NRF_WDT_Type;
extern NRF_WDT_Type *NRF_WDT;
#define NRF_WDT_CHANNEL_NUMBER	8
typedef enum { NRF_WDT_RR0 = 0 } nrf_wdt_rr_register_t;
bool nrf_wdt_started_check(NRF_WDT_Type const *p_reg);
bool nrf_wdt_reload_request_enable_check(NRF_WDT_Type const *p_reg,
					 nrf_wdt_rr_register_t rr_register);
void nrf_wdt_reload_request_set(NRF_WDT_Type *p_reg,
				nrf_wdt_rr_register_t rr_register);
#endif
