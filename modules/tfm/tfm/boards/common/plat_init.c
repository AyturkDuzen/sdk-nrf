/*
 * Copyright (c) 2019-2020, Arm Limited. All rights reserved.
 * Copyright (c) 2021 Nordic Semiconductor ASA.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "tfm_hal_platform.h"
#include "cmsis.h"
#include "uart_stdout.h"
#include "tfm_spm_log.h"
#include "hw_unique_key.h"
#include <nrf_cc3xx_platform.h>

enum tfm_hal_status_t tfm_hal_platform_init(void)
{
	__enable_irq();

	/* Only if UART1 is used by TF-M do we initialize it. */
#ifdef SECURE_UART1
	stdio_init();
#endif

#if defined(TFM_PARTITION_CRYPTO) && \
    !defined(TFM_CRYPTO_KEY_DERIVATION_MODULE_DISABLED) && \
    !defined(PLATFORM_DUMMY_CRYPTO_KEYS)
	/* Initialize the nrf_cc3xx runtime */
	int result = nrf_cc3xx_platform_init();
	if (result != NRF_CC3XX_PLATFORM_SUCCESS) {
		return TFM_HAL_ERROR_BAD_STATE;
	}

	if (!hw_unique_key_are_any_written()) {
		SPMLOG_INFMSG("Writing random Hardware Unique Keys to the KMU.\r\n");
		hw_unique_key_write_random();
		SPMLOG_INFMSG("Success\r\n");
	}
#endif

	return TFM_HAL_SUCCESS;
}
