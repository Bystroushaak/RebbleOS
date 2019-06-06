/* asterix_flash.c
 * flash routines for QSPI flash on nRF52840
 * RebbleOS
 *
 * Author: Joshua Wise <joshua@joshuawise.com>
 */

#include <debug.h>
#include "rebbleos.h"
#include "nrfx_qspi.h"

#define BOARD_QSPI_SCK_PIN 19
#define BOARD_QSPI_CSN_PIN 17
#define BOARD_QSPI_IO0_PIN 20
#define BOARD_QSPI_IO1_PIN 21
#define BOARD_QSPI_IO2_PIN 22
#define BOARD_QSPI_IO3_PIN 23

#define QSPI_INSTR_JEDEC_ID 0x9F

void hw_flash_init() {
    nrfx_qspi_config_t config = NRFX_QSPI_DEFAULT_CONFIG;
    config.phy_if.sck_freq = NRF_QSPI_FREQ_32MDIV1;
    config.pins.sck_pin = BOARD_QSPI_SCK_PIN;
    config.pins.csn_pin = BOARD_QSPI_CSN_PIN;
    config.pins.io0_pin = BOARD_QSPI_IO0_PIN;
    config.pins.io1_pin = BOARD_QSPI_IO1_PIN;
    config.pins.io2_pin = BOARD_QSPI_IO2_PIN;
    config.pins.io3_pin = BOARD_QSPI_IO3_PIN;

    nrfx_err_t err;
    err = nrfx_qspi_init(&config, NULL, NULL);
    assert(err == NRFX_SUCCESS && "QSPI initialization failed");

    /* Read the JEDEC ID out of the flash */    
    nrf_qspi_cinstr_conf_t instr = NRFX_QSPI_DEFAULT_CINSTR(QSPI_INSTR_JEDEC_ID, 4);
    uint8_t buf[16];
    err = nrfx_qspi_cinstr_xfer(&instr, NULL, buf);
    assert(err == NRFX_SUCCESS && "QSPI JEDEC ID read failed");

    DRV_LOG("flash", APP_LOG_LEVEL_DEBUG, "QSPI: JEDEC ID %02x %02x %02x", buf[0], buf[1], buf[2]);
}

void hw_flash_read_bytes(uint32_t addr, uint8_t *buf, size_t len) {
    flash_operation_complete(0);
}
