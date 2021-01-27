// SPDX-License-Identifier: GPL-3.0-only

#include <board/battery.h>
#include <board/board.h>
#include <board/dgpu.h>
#include <board/espi.h>
#include <board/gctrl.h>
#include <board/gpio.h>
#include <board/peci.h>
#include <board/power.h>
#include <common/debug.h>
#include <common/macro.h>

extern uint8_t main_cycle;

bool have_dgpu = false;

volatile uint8_t __xdata __at(0x1900) ADCSTS;
volatile uint8_t __xdata __at(0x1901) ADCCFG;
volatile uint8_t __xdata __at(0x1941) VCH7CTL;
volatile uint8_t __xdata __at(0x1942) VCH7DATM;
volatile uint8_t __xdata __at(0x1943) VCH7DATL;

static void adc_init(void) {
    // Perform ADC accuracy initialization
    ADCSTS |= BIT(3);
    ADCSTS &= ~BIT(3);
}

static void board_detect(void) {
    DEBUG("have_dgpu before %d\n", have_dgpu);

    // Set GPI7 to alt mode
    GPCRI7 = GPIO_ALT;

    // Clear channel 7 data valid
    VCH7CTL |= BIT(7);

    // Enable channel 7
    VCH7CTL |= BIT(4);

    // Enable ADC
    ADCCFG |= BIT(0);

    // Wait for channel 7 data valid
    while (!(VCH7CTL & BIT(7))) {}

    // Read channel 7 data
    uint8_t low = VCH7DATL;
    uint8_t high = VCH7DATM;

    DEBUG("VCH7 0x%02X%02X\n", high, low);

    switch (high) {
        case 0x00:
            // NVIDIA 1650 variant
            have_dgpu = true;
            break;
        case 0x01:
        case 0x02:
            // No NVIDIA variant
            have_dgpu = false;
            break;
        case 0x03:
            // NVIDIA 1650 Ti variant
            have_dgpu = true;
            break;
    }

    // Disable ADC
    ADCCFG &= ~BIT(0);

    // Disable channel 7
    VCH7CTL &= ~BIT(4);

    // Clear channel 7 data valid
    VCH7CTL |= BIT(7);

    // Set GPI7 to input
    GPCRI7 = GPIO_IN;

    DEBUG("have_dgpu after %d\n", have_dgpu);
}

void board_init(void) {
    espi_init();

    // Make sure charger is in off state, also enables PSYS
    battery_charger_disable();

    // Initialize ADC, run only once before board_detect
    adc_init();

    // Detect board features
    board_detect();

    // Allow CPU to boot
    gpio_set(&SB_KBCRST_N, true);
    // Allow backlight to be turned on
    gpio_set(&BKL_EN, true);
    // Enable camera
    gpio_set(&CCD_EN, true);
    // Enable wireless
    gpio_set(&BT_EN, true);
    gpio_set(&WLAN_EN, true);
    gpio_set(&WLAN_PWR_EN, true);
    // Enable right USB port
    gpio_set(&USB_PWR_EN_N, false);
    // Assert SMI# and SWI#
    gpio_set(&SMI_N, true);
    gpio_set(&SWI_N, true);

    // Enable POST codes
    SPCTRL1 |= 0xC8;
}

#if HAVE_DGPU
// Set PL4 using PECI
static int set_power_limit(uint8_t watts) {
    return peci_wr_pkg_config(
        60, // index
        0, // param
        ((uint32_t)watts) * 8
    );
}

void board_on_ac(bool ac) {
    uint8_t power_limit = ac ? POWER_LIMIT_AC : POWER_LIMIT_DC;
    // Retry, timeout errors happen occasionally
    for (int i = 0; i < 16; i++) {
        int res = set_power_limit(power_limit);
        DEBUG("set_power_limit %d = %d\n", power_limit, res);
        if (res == 0x40) {
            break;
        } else if (res < 0) {
            ERROR("set_power_limit failed: 0x%02X\n", -res);
        } else {
            ERROR("set_power_limit unknown response: 0x%02X\n", res);
        }
    }

    //XXX just for testing
    board_detect();
}
#else // HAVE_DGPU
void board_on_ac(bool ac) { /* Fix unused variable */ ac = ac; }
#endif // HAVE_DGPU

void board_event(void) {
#if HAVE_DGPU
    static bool last_power_limit_ac = true;
    // We don't use power_state because the latency needs to be low
    if (gpio_get(&CPU_C10_GATE_N)) {
        bool ac = !gpio_get(&ACIN_N);
        if (last_power_limit_ac != ac) {
            board_on_ac(ac);
            last_power_limit_ac = ac;
        }
    } else {
        last_power_limit_ac = true;
    }
#endif // HAVE_DGPU

    espi_event();

    // Read POST codes
    while (P80H81HS & 1) {
        uint8_t p80h = P80HD;
        uint8_t p81h = P81HD;
        P80H81HS |= 1;

        DEBUG("POST %02X%02X\n", p81h, p80h);
    }
}
