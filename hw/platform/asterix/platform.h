#ifndef _PLATFORM_H
#define _PLATFORM_H

#include "nrf52840.h"

#define DISPLAY_ROWS 168
#define DISPLAY_COLS 144

/* Asterix has 16MB of flash. */

/* Bootloader private area: 1MB */
#define REGION_BOOTLOADER_START 0x0
#define REGION_BOOTLOADER_SIZE  0x100000

/* Reflash staging area: 1MB ROM, 1MB resources */
#define REGION_STAGING_START 0x100000
#define REGION_STAGING_SIZE  0x200000

/* System resources: 1MB */
#define REGION_RES_START 0x300000
#define REGION_RES_SIZE  0x100000

/* The rest of the filesystem: 12MB */
#define REGION_FS_START         0x400000
#define REGION_FS_PAGE_SIZE     0x1000
#define REGION_FS_N_PAGES       ((0xFE0000 - REGION_FS_START) / REGION_FS_PAGE_SIZE)

/* The size of the page that holds an apps header table. This is the amount before actual app content e.g
 0x0000  Resource table header
 0x1000  Resource data start
 */
#define APP_RES_START           0x1000

/* XXX: issue pebble-dev/RebbleOS#43 */
#define RES_START           0x200C

#define CCRAM

typedef enum hw_button {
    HW_BUTTON_BACK = 0,
    HW_BUTTON_UP,
    HW_BUTTON_SELECT,
    HW_BUTTON_DOWN,
    HW_BUTTON_MAX
} hw_button_t;

typedef void (*hw_button_isr_t)(hw_button_t id);

void hw_button_init(void);
int hw_button_pressed(hw_button_t button_id);
void hw_button_set_isr(hw_button_isr_t isr);

#define WATCHDOG_RESET_MS 500

static inline uint8_t is_interrupt_set(void)
{
    return ((volatile int)(SCB->ICSR & SCB_ICSR_VECTACTIVE_Msk)) != 0 ;
}

#endif
