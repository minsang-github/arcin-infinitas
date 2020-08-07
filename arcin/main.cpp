#include <rcc/rcc.h>
#include <gpio/gpio.h>
#include <interrupt/interrupt.h>
#include <timer/timer.h>
#include <os/time.h>
#include <usb/usb.h>
#include <usb/descriptor.h>

#include "report_desc.h"
#include "usb_strings.h"
#include "configloader.h"
#include "config.h"

//
// Pin out on arcin board
//

#define ARCIN_BUTTON_KEY_1        ((uint16_t)(1 << 0))
#define ARCIN_BUTTON_KEY_2        ((uint16_t)(1 << 1))
#define ARCIN_BUTTON_KEY_3        ((uint16_t)(1 << 2))
#define ARCIN_BUTTON_KEY_4        ((uint16_t)(1 << 3))
#define ARCIN_BUTTON_KEY_5        ((uint16_t)(1 << 4))
#define ARCIN_BUTTON_KEY_6        ((uint16_t)(1 << 5))
#define ARCIN_BUTTON_KEY_7        ((uint16_t)(1 << 6))

#define ARCIN_BUTTON_KEY_ALL_MAIN ((uint16_t)(0x7F))

#define ARCIN_BUTTON_EXTRA_8      ((uint16_t)(1 << 7))
#define ARCIN_BUTTON_EXTRA_9      ((uint16_t)(1 << 8))

#define ARCIN_BUTTON_START        ((uint16_t)(1 << 9))
#define ARCIN_BUTTON_SEL          ((uint16_t)(1 << 10))

//
// Remapped values for Windows
//

#define JOY_BUTTON_13             ((uint16_t)(1 << 12))
#define JOY_BUTTON_14             ((uint16_t)(1 << 13))

#define INFINITAS_BUTTON_E1       ((uint16_t)(1 << 8))
#define INFINITAS_BUTTON_E2       ((uint16_t)(1 << 9))
#define INFINITAS_BUTTON_E3       ((uint16_t)(1 << 10))
#define INFINITAS_BUTTON_E4       ((uint16_t)(1 << 11))

static uint32_t& reset_reason = *(uint32_t*)0x10000000;

static bool do_reset_bootloader;
static bool do_reset;

void reset() {
    SCB.AIRCR = (0x5fa << 16) | (1 << 2); // SYSRESETREQ
}

void reset_bootloader() {
    reset_reason = 0xb007;
    reset();
}

Configloader configloader(0x801f800);

config_t config;

/* 
 // origial hardware ID for arcin - expected by firmware flash
 // and the settings tool
auto dev_desc = device_desc(0x200, 0, 0, 0, 64, 0x1d50, 0x6080, 0x110, 1, 2, 3, 1);
*/

// Hardware ID Infinitas controller: 0x1ccf, 0x8048
// The game detects this and automatically uses its own (internal) key config
// overridng any user settings in the launcher
auto dev_desc = device_desc(0x200, 0, 0, 0, 64, 0x1CCF, 0x8048, 0x110, 1, 2, 3, 1);

auto conf_desc = configuration_desc(1, 1, 0, 0xc0, 0,
    // HID interface.
    interface_desc(0, 0, 1, 0x03, 0x00, 0x00, 0,
        hid_desc(0x111, 0, 1, 0x22, sizeof(report_desc)),
        endpoint_desc(0x81, 0x03, 16, 1)
    )
);

desc_t dev_desc_p = {sizeof(dev_desc), (void*)&dev_desc};
desc_t conf_desc_p = {sizeof(conf_desc), (void*)&conf_desc};
desc_t report_desc_p = {sizeof(report_desc), (void*)&report_desc};

static Pin usb_dm = GPIOA[11];
static Pin usb_dp = GPIOA[12];
static Pin usb_pu = GPIOA[15];

static PinArray button_inputs = GPIOB.array(0, 10);
static PinArray button_leds = GPIOC.array(0, 10);

static Pin qe1a = GPIOA[0];
static Pin qe1b = GPIOA[1];
static Pin qe2a = GPIOA[6];
static Pin qe2b = GPIOA[7];

static Pin led1 = GPIOA[8];
static Pin led2 = GPIOA[9];

USB_f1 usb(USB, dev_desc_p, conf_desc_p);

uint32_t last_led_time;

class HID_arcin : public USB_HID {
    private:
        bool set_feature_bootloader(bootloader_report_t* report) {
            switch(report->func) {
                case 0:
                    return true;
                
                case 0x10: // Reset to bootloader
                    do_reset_bootloader = true;
                    return true;
                
                case 0x20: // Reset to runtime
                    do_reset = true;
                    return true;
                
                default:
                    return false;
            }
        }
        
        bool set_feature_config(config_report_t* report) {
            if(report->segment != 0) {
                return false;
            }
            
            configloader.write(report->size, report->data);
            
            return true;
        }
        
        bool get_feature_config() {
            config_report_t report = {0xc0, 0, sizeof(config)};
            
            memcpy(report.data, &config, sizeof(config));
            
            usb.write(0, (uint32_t*)&report, sizeof(report));
            
            return true;
        }
    
    public:
        HID_arcin(USB_generic& usbd, desc_t rdesc) : USB_HID(usbd, rdesc, 0, 1, 64) {}
    
    protected:
        virtual bool set_output_report(uint32_t* buf, uint32_t len) {
            if(len != sizeof(output_report_t)) {
                return false;
            }
            
            output_report_t* report = (output_report_t*)buf;
            
            last_led_time = Time::time();
            button_leds.set(report->leds);
            return true;
        }
        
        virtual bool set_feature_report(uint32_t* buf, uint32_t len) {
            switch(*buf & 0xff) {
                case 0xb0:
                    if(len != sizeof(bootloader_report_t)) {
                        return false;
                    }
                    
                    return set_feature_bootloader((bootloader_report_t*)buf);
                
                case 0xc0:
                    if(len != sizeof(config_report_t)) {
                        return false;
                    }
                    
                    return set_feature_config((config_report_t*)buf);
                
                default:
                    return false;
            }
        }
        
        virtual bool get_feature_report(uint8_t report_id) {
            switch(report_id) {
                case 0xc0:
                    return get_feature_config();
                
                default:
                    return false;
            }
        }
};

uint16_t ARCIN_DEBUG;

uint32_t first_e2_rising_edge_time;

#define LAST_E2_STATUS_SIZE 3

// [0] is most recent, [1] is one before that, and so on
bool last_e2_status[LAST_E2_STATUS_SIZE];
uint32_t e2_rising_edge_count;

// Window that begins on the first falling edge of E2. At the end of the window
// the number of taps is calculated and the resulting button combination will
// begin to assert.
// i.e., any multi-taps must be done within this window in order to count
#define MULTITAP_DETECTION_WINDOW_MS           400
#define MULTITAP_RESULT_HOLD_MS   100

uint16_t multitap_active_frames;
uint16_t multitap_buttons_to_assert;

// For LR2 mode digital turntable
#define DIGITAL_TT_HOLD_DURATION_MS 100

uint8_t last_x = 0;
int16_t state_x = 0;

void e2_update(bool pressed) {
    // rising edge (detect off-off-on-on sequence)
    if (pressed && last_e2_status[0] &&
        !last_e2_status[1] && !last_e2_status[2]) {

        // start counting on the first falling edge
        if (first_e2_rising_edge_time == 0) {
            first_e2_rising_edge_time = Time::time();
        }

        e2_rising_edge_count += 1;
    }

    for (int i = (LAST_E2_STATUS_SIZE - 1); i >= 1; i -= 1) {
        last_e2_status[i] = last_e2_status[i-1];
    }

    last_e2_status[0] = pressed;
}

uint16_t get_multitap_output(uint32_t tap_count) {
    uint16_t button;
    switch (tap_count) {
    case 1:
        button = INFINITAS_BUTTON_E2;
        break;

    case 2:
        button = INFINITAS_BUTTON_E3;
        break;

    case 3:
        button = (INFINITAS_BUTTON_E2 | INFINITAS_BUTTON_E3);
        break;

    case 0:
    default:
        button = 0;
        break;
    }

    return button;
}

bool is_multitap_window_closed() {
    uint32_t diff;

    if (first_e2_rising_edge_time == 0) {
        return false;
    }

    diff = Time::time() - first_e2_rising_edge_time;
    return diff > MULTITAP_DETECTION_WINDOW_MS;
}

uint16_t remap_buttons(uint16_t buttons) {
    uint16_t remapped;

    // Grab the first 7 buttons (keys)
    remapped = buttons & ARCIN_BUTTON_KEY_ALL_MAIN;

    // Remap start button
    if (buttons & ARCIN_BUTTON_START) {
        switch(config.effector_mode) {
        case START_E1_SEL_E2:
        default:
            remapped |= INFINITAS_BUTTON_E1;
            break;

        case START_E2_SEL_E1:
            remapped |= INFINITAS_BUTTON_E2;
            break;

        case START_E3_SEL_E4:
            remapped |= INFINITAS_BUTTON_E3;
            break;

        case START_E4_SEL_E3:
            remapped |= INFINITAS_BUTTON_E4;
            break;
        }
    }

    // Remap select button
    if (buttons & ARCIN_BUTTON_SEL) {
        switch(config.effector_mode) {
        case START_E1_SEL_E2:
        default:
            remapped |= INFINITAS_BUTTON_E2;
            break;

        case START_E2_SEL_E1:
            remapped |= INFINITAS_BUTTON_E1;
            break;

        case START_E3_SEL_E4:
            remapped |= INFINITAS_BUTTON_E4;
            break;

        case START_E4_SEL_E3:
            remapped |= INFINITAS_BUTTON_E3;
            break;
        }
    }

    // Button 8 is normally E3, unless flipped
    if (buttons & ARCIN_BUTTON_EXTRA_8) {
        if (config.flags & ARCIN_CONFIG_FLAG_SWAP_8_9) {
            remapped |= INFINITAS_BUTTON_E4;
        } else {
            remapped |= INFINITAS_BUTTON_E3;
        }
    }

    // Button 9 is normally E4, unless flipped
    if (buttons & ARCIN_BUTTON_EXTRA_9) {
        if (config.flags & ARCIN_CONFIG_FLAG_SWAP_8_9) {
            remapped |= INFINITAS_BUTTON_E3;
        } else {
            remapped |= INFINITAS_BUTTON_E4;
        }
    }

    return remapped;
}

#define DEBOUNCE_TIME_MS 5

uint16_t debounce_state = 0;
uint16_t debounce_history[DEBOUNCE_TIME_MS] = { 0 };
uint32_t debounce_sample_time = 0;
int debounce_index = 0;

/* 
 * Perform debounce processing. The buttons input is sampled at most once per ms
 * (when update is true); buttons is then set to the last stable state for each
 * bit (i.e., the last state maintained for DEBOUNCE_TIME_MS consequetive samples
 *
 * We use update to sync to the USB polls; this helps avoid additional latency when
 * debounce samples just after the USB poll.
 */
uint16_t debounce(uint16_t buttons) {
    if (Time::time() == debounce_sample_time) {
        return debounce_state;
    }

    debounce_sample_time = Time::time();
    debounce_history[debounce_index] = buttons;
    debounce_index = (debounce_index + 1) % DEBOUNCE_TIME_MS;

    uint16_t has_ones = 0, has_zeroes = 0;
    for (int i = 0; i < DEBOUNCE_TIME_MS; i++) {
        has_ones |= debounce_history[i];
        has_zeroes |= ~debounce_history[i];
    }

    uint16_t stable = has_ones ^ has_zeroes;
    debounce_state = (debounce_state & ~stable) | (has_ones & stable);
    return debounce_state;
}

class analog_button {
public:
    // config

    // Number of ticks we need to advance before recognizing an input
    uint32_t deadzone;
    // How long to sustain the input before clearing it (if opposite direction is input, we'll release immediately)
    uint32_t sustain_ms;
    // Always provide a zero-input for one poll before reversing?
    bool clear;

    const volatile uint32_t &counter;

    // State: Center of deadzone
    uint32_t center;
    // times to: reset to zero, reset center to counter
    uint32_t t_timeout;

    int8_t state; // -1, 0, 1
    int8_t last_delta;
public:
    analog_button(volatile uint32_t &counter, uint32_t deadzone, uint32_t sustain_ms, bool clear)
        : deadzone(deadzone), sustain_ms(sustain_ms), clear(clear), counter(counter)
    {
        center = counter;
        t_timeout = 0;
        state = 0;
    }

    int8_t poll() {
        uint8_t observed = counter;
        int8_t delta = observed - center;
        last_delta = delta;

        uint8_t direction = 0;
        if (delta >= (int32_t)deadzone) {
            direction = 1;
        } else if (delta <= -(int32_t)deadzone) {
            direction = -1;
        }

        if (direction != 0) {
            center = observed;
            t_timeout = Time::time() + sustain_ms;
        } else if (t_timeout != 0 && Time::time() >= t_timeout) {
            state = 0;
            center = observed;
            t_timeout = 0;
        }

        if (direction == -state && clear) {
            state = direction;
            return 0;
        } else if (direction != 0) {
            state = direction;
        }

        return state;
    }
};

HID_arcin usb_hid(usb, report_desc_p);

USB_strings usb_strings(usb, config.label);

int main() {
    rcc_init();
    
    // Initialize system timer.
    STK.LOAD = 72000000 / 8 / 1000; // 1000 Hz.
    STK.CTRL = 0x03;
    
    // Load config.
    configloader.read(sizeof(config), &config);

    RCC.enable(RCC.GPIOA);
    RCC.enable(RCC.GPIOB);
    RCC.enable(RCC.GPIOC);
    
    usb_dm.set_mode(Pin::AF);
    usb_dm.set_af(14);
    usb_dp.set_mode(Pin::AF);
    usb_dp.set_af(14);
    
    RCC.enable(RCC.USB);
    
    usb.init();
    
    usb_pu.set_mode(Pin::Output);
    usb_pu.on();
    
    button_inputs.set_mode(Pin::Input);
    button_inputs.set_pull(Pin::PullUp);
    
    button_leds.set_mode(Pin::Output);
    
    led1.set_mode(Pin::Output);
    led1.on();
    
    led2.set_mode(Pin::Output);
    led2.on();
    
    RCC.enable(RCC.TIM2);
    RCC.enable(RCC.TIM3);
    
    if(!(config.flags & ARCIN_CONFIG_FLAG_INVERT_QE1)) {
        TIM2.CCER = 1 << 1;
    }
    
    TIM2.CCMR1 = (1 << 8) | (1 << 0);
    TIM2.SMCR = 3;
    TIM2.CR1 = 1;
    
    if(config.qe1_sens < 0) {
        TIM2.ARR = 256 * -config.qe1_sens - 1;
    } else {
        TIM2.ARR = 256 - 1;
    }
    
    TIM3.CCMR1 = (1 << 8) | (1 << 0);
    TIM3.SMCR = 3;
    TIM3.CR1 = 1;
    
    if(config.qe2_sens < 0) {
        TIM3.ARR = 256 * -config.qe2_sens - 1;
    } else {
        TIM3.ARR = 256 - 1;
    }
    
    qe1a.set_af(1);
    qe1b.set_af(1);
    qe1a.set_mode(Pin::AF);
    qe1b.set_mode(Pin::AF);
    
    qe2a.set_af(2);
    qe2b.set_af(2);
    qe2a.set_mode(Pin::AF);
    qe2b.set_mode(Pin::AF);    

    analog_button tt1(TIM2.CNT, 4, 100, true);

    while(1) {
        usb.process();

        uint32_t now = Time::time();
        uint16_t buttons = button_inputs.get() ^ 0x7ff;
        
        if(do_reset_bootloader) {
            Time::sleep(10);
            reset_bootloader();
        }
        
        if(do_reset) {
            Time::sleep(10);
            reset();
        }
        
        if(now - last_led_time > 1000) {
            button_leds.set(buttons);
        }

        if (ARCIN_DEBUG > 0) {
            button_leds.set(ARCIN_DEBUG);
        }
        
        if(usb.ep_ready(1)) {
            uint32_t qe1_count = TIM2.CNT;
            uint16_t remapped = remap_buttons(buttons);

            // Digital turntable for LR2.
            if (config.flags & ARCIN_CONFIG_FLAG_DIGITAL_TT_ENABLE) {
                switch (tt1.poll()) {
                case -1:
                    remapped |= JOY_BUTTON_13;
                    break;
                case 1:
                    remapped |= JOY_BUTTON_14;
                    break;
                default:
                    break;
                }
            }

            // Apply debounce...
            uint16_t debounce_mask = 0;
            // If multi-tap on E2 is enabled, debounce E2 beforehand
            if (config.flags & ARCIN_CONFIG_FLAG_SEL_MULTI_TAP) {
                debounce_mask |= INFINITAS_BUTTON_E2;
            }

            if (debounce_mask != 0) {
                remapped = (remapped & ~debounce_mask) |
                           (debounce(remapped) & debounce_mask);
            }

            // Multi-tap processing of E2. Must be done after debounce.
            if (config.flags & ARCIN_CONFIG_FLAG_SEL_MULTI_TAP) {
                if (multitap_active_frames == 0) {
                    // Make a note of its current state
                    e2_update((remapped & INFINITAS_BUTTON_E2) != 0);
                } else if ((remapped & INFINITAS_BUTTON_E2) == 0) {
                    // if the button is no longer being held, count down
                    multitap_active_frames -= 1;
                }

                // Always clear E2 since it should not be asserted directly
                remapped &= ~(INFINITAS_BUTTON_E2);

                if (multitap_active_frames > 0) {                    
                    remapped |= multitap_buttons_to_assert;

                } else if (is_multitap_window_closed()) {
                    multitap_active_frames = MULTITAP_RESULT_HOLD_MS;
                    first_e2_rising_edge_time = 0;
                    multitap_buttons_to_assert =
                        get_multitap_output(e2_rising_edge_count);

                    e2_rising_edge_count = 0;
                }
            }

            // Finally - adjust turntable sensitivity before reporting it
            if(config.qe1_sens < 0) {
                qe1_count /= -config.qe1_sens;
            } else if(config.qe1_sens > 0) {
                qe1_count *= config.qe1_sens;
            }

            input_report_t report;
            report.report_id = 1;
            report.buttons = remapped;
            if (config.flags & ARCIN_CONFIG_FLAG_DIGITAL_TT_ENABLE) {
                report.axis_x = uint8_t(127);
            } else {
                report.axis_x = uint8_t(qe1_count);
            }
            report.axis_y = 127;
            usb.write(1, (uint32_t*)&report, sizeof(report));
        }
    }
}
