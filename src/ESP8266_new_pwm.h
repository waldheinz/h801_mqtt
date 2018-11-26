#pragma once

#include <Arduino.h>

/**
 * period: initial period (base unit 1us OR 200ns)
 * duty: array of initial duty values, may be NULL, may be freed after pwm_init
 * pwm_channel_num: number of channels to use
 * pin_info_list: array of pin_info
 */
void ICACHE_FLASH_ATTR
pwm_init(uint32_t period, uint32_t *duty, uint32_t pwm_channel_num, uint32_t (*pin_info_list)[3]);

void ICACHE_FLASH_ATTR
pwm_start(void);

void ICACHE_FLASH_ATTR
pwm_set_duty(uint32_t duty, uint8_t channel);
