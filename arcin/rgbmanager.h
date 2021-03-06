#ifndef RGBMANAGER_DEFINES_H
#define RGBMANAGER_DEFINES_H

#include <stdint.h>
#include <os/time.h>
#include "fastled_shim.h"
#include "FastLED.h"
#include "ws2812b.h"
#include "color.h"
#include "color_palettes.h"
#include "rgb_pacifica.h"
#include "rgb_pride2015.h"

extern uint32_t debug_value;

WS2812B ws2812b_global;

// duration of each frame, in milliseconds
//
// https://github.com/FastLED/FastLED/wiki/Interrupt-problems
// Each pixel takes 30 microseconds.
//  60 LEDs = 1800 us = 1.8ms
// 180 LEDs = 5400 us = 5.4ms
// So 20ms is more than enough to handle the worst case.

#define RGB_MANAGER_FRAME_MS 20

extern bool global_led_enable;

// Here, "0" is off, "1" refers to primary color, "2" is secondary, "3" is tertiary
typedef enum _WS2812B_Mode {
    // static   - all LEDs on 1
    // animated - this is the "breathe" effect (cycles between 0 and 1)
    // tt       - same as static (with default fade in/out only)
    WS2812B_MODE_SINGLE_COLOR,

    // static   - all LEDs on 1
    // animated - reverse sawtooth (flash all with 2, fade out to 1, repeat)
    // tt       - tt movement turns all LED to 2, fade out to 1
    WS2812B_MODE_TWO_COLOR_FADE,

    // static   - all LEDs on random color
    // animated - reverse sawtooth (flash all with random color, fade out to 0, repeat)
    // tt       - tt movement turns all LED to a new random color
    WS2812B_MODE_RANDOM_HUE,

    // static   - each LED takes 1/2/3
    // animated - each LED cycles through 1/2/3
    // tt       - controls animation speed and direction
    WS2812B_MODE_TRICOLOR,

    // static   - dots using 1/2/3
    // animated - same as static but rotates
    // tt       - controls animation speed and direction
    // mult     - controls the number of dots
    WS2812B_MODE_DOTS,
    WS2812B_MODE_DIVISIONS,

    // static   - all LEDs have the same color (somewhere on the hue spectrum)
    // animated - all LEDs cycle through hue spectrum
    // tt       - controls animation speed and direction
    WS2812B_MODE_STATIC_RAINBOW,

    // static   - each LED represents hue value on the palette
    // animated - static, but rotates through
    // tt       - controls animation speed and direction
    // mult     - controls the wave length
    WS2812B_MODE_RAINBOW_WAVE,

    WS2812B_MODE_PRIDE,
    WS2812B_MODE_PACIFICA,
} WS2812B_Mode;

void crgb_from_colorrgb(ColorRgb color, CRGB& crgb) {
    crgb = CRGB(color.Red, color.Green, color.Blue);
}

// This routine exists so that we can scale w.r.t. the RPM, which is dependent on the number of
// LEDs in a circle.
uint8_t pick_led_number(uint8_t num_leds, fract16 fract) {
    uint16_t val = lerp16by16(0, num_leds, fract);

    // it's possible for lerp16by16 to return exactly the max value; clamp it
    if (num_leds <= val) {
        val = 0;
    }

    return val;
}

// given multiplicity, num_leds, and dot1, calculate dot2 and dot3
void get_divisions(uint8_t multiplicity, uint8_t num_leds, uint8_t dot1, uint8_t& dot2, uint8_t& dot3) {
    dot2 = UINT8_MAX;
    dot3 = UINT8_MAX;
    if (2 == multiplicity) {
        dot2 = (dot1 + (num_leds / 2)) % num_leds;
    } else if (3 <= multiplicity) {
        dot2 = (dot1 + (num_leds / 3)) % num_leds;
        dot3 = (dot2 + (num_leds / 3)) % num_leds;
    }
}

class RGBManager {

    CRGB leds[WS2812B_MAX_LEDS];
    uint8_t num_leds;

    uint32_t last_hid_report = 0;
    uint32_t last_outdated_hid_check = 0;

    // reacting to tt movement (stationary / moving)
    // any movement instantly increases it to -127 or +127
    // no movement - slowly reaches 0 over time
    int8_t tt_activity = 0;
    uint32_t last_tt_activity_time = 0;
    uint16_t tt_fade_out_time = 0;
    int8_t previous_tt = 0;

    // user-defined color mode
    WS2812B_Mode rgb_mode = WS2812B_MODE_SINGLE_COLOR;
    rgb_config_flags flags = {0};
    uint8_t multiplicity = 0;

    // user-defined modifiers
    uint8_t default_darkness = 0;
    uint8_t idle_brightness = 0;
    accum88 idle_animation_speed = 0;

    // ranges are [-100, 100]
    // divide by 10 to get actual multiplier (100 => 10x) from UI   
    int16_t tt_animation_speed_10x = 0;

    // user-defined colors
    CRGB rgb_off = CRGB(0, 0, 0);
    CRGB rgb_primary;
    CRGB rgb_secondary;
    CRGB rgb_tertiary;

    // for random hue (from palette)
    uint8_t current_random8;
    bool ready_for_new_hue = false;
    uint8_t previous_value;

    // shift values that modify colors, ranges from [0, UINT16_MAX]
    uint16_t shift_value = 0;
    uint32_t tt_time_travel_base_ms = 0;

    // for palette-based RGB modes
    CRGBPalette256 current_palette;

    private:    
        void update_static(CRGB& rgb) {
            fill_solid(leds, num_leds, rgb);
            show();
        }

        uint8_t calculate_brightness() {
            uint16_t brightness;
            if (flags.ReactToTt) {
                // start out with max brightness..
                brightness = UINT8_MAX;
                // and decrease with TT activity
                brightness -= scale8(
                    UINT8_MAX - idle_brightness,
                    quadwave8(127 + abs(tt_activity)));

            } else {
                // full brightness
                brightness = UINT8_MAX;
            }
            
            // finally, scale everything down by overall brightness override
            return scale8(brightness, UINT8_MAX - default_darkness);
        }

        void show() {
            FastLED.setBrightness(calculate_brightness());
            show_without_dimming();
        }

        void show_without_dimming() {
            FastLED.show();
        }

        void set_off() {
            fill_solid(leds, num_leds, CRGB::Black);
            show_without_dimming();
        }

        accum88 calculate_adjusted_speed(WS2812B_Mode rgb_mode, uint8_t raw_value) {
            // temporarily add precision
            uint32_t raw_value_1k = raw_value * 1000;
            accum88 adjusted;
            switch(rgb_mode) {
                case WS2812B_MODE_SINGLE_COLOR:
                case WS2812B_MODE_TWO_COLOR_FADE:
                    // BPM. Must match UI calculation
                    raw_value_1k = raw_value_1k * raw_value / UINT8_MAX;
                    break;

                case WS2812B_MODE_DOTS:
                case WS2812B_MODE_DIVISIONS:
                case WS2812B_MODE_RAINBOW_WAVE:
                case WS2812B_MODE_TRICOLOR:
                    // RPM. Must match UI calculation
                    raw_value_1k = raw_value_1k / 2;
                    break;

                case WS2812B_MODE_STATIC_RAINBOW:                
                    // it's way too distracting otherwise
                    raw_value_1k = raw_value_1k / 8;
                    break;

                case WS2812B_MODE_RANDOM_HUE:
                default:
                    break;
            }

            adjusted = 0;
            adjusted |= (raw_value_1k / 1000) << 8;
            adjusted |= ((raw_value_1k % 1000) * 255 / 1000) & 0xFF;

            // 0 bpm is OK, but don't let it fall between 0-1 bpm since the library will convert
            // accum88 into uint8
            if (adjusted < 256) {
                adjusted = 0;
            }

            return adjusted;
        }

        void set_palette(WS2812B_Mode rgb_mode, WS2812B_Palette palette) {
            fill_from_palette(
                current_palette,
                palette,
                bool(rgb_mode == WS2812B_MODE_RAINBOW_WAVE));
        }
        
        void set_mode(WS2812B_Mode rgb_mode, WS2812B_Palette palette, uint8_t multiplicity) {
            this->rgb_mode = rgb_mode;
            this->multiplicity = max(1, multiplicity);

            // seed random
            random16_add_entropy(serial_num() >> 16);
            random16_add_entropy(serial_num());
            current_random8 = random8();

            // pre-initialize color palette
            switch(rgb_mode) {
                case WS2812B_MODE_TWO_COLOR_FADE:
                    current_palette = CRGBPalette256(rgb_primary, rgb_secondary);
                    break;

                case WS2812B_MODE_RANDOM_HUE:
                case WS2812B_MODE_STATIC_RAINBOW:
                case WS2812B_MODE_RAINBOW_WAVE:                
                    set_palette(rgb_mode, palette);
                    break;

                default:
                    break;
            }
        }
        
        void update_turntable_activity(uint32_t now, int8_t tt) {
            // Detect TT activity; framerate dependent, of course.
            switch (tt) {
                case 1:
                    tt_activity = 127;
                    last_tt_activity_time = now;
                    break;

                case -1:
                    tt_activity = -127;
                    last_tt_activity_time = now;
                    break;

                case 0:
                default:
                    if (last_tt_activity_time == 0) {
                        tt_activity = 0;
                    } else if (tt_activity != 0) {
                        uint16_t time_since_last_tt = now - last_tt_activity_time;
                        if (time_since_last_tt < tt_fade_out_time) {
                            uint16_t delta = tt_fade_out_time - time_since_last_tt;
                            int16_t temp = tt_activity;
                            if (temp > 0) {
                                temp = 
                                    ((int16_t)127) * delta / tt_fade_out_time;
                            } else {
                                temp = 
                                    ((int16_t)-127) * delta / tt_fade_out_time;
                            }

                            tt_activity = temp;

                        } else {
                            tt_activity = 0;
                        }
                    }
                    break;
            }

            // while turntable animation is active, pause idle animation by "stopping"
            // time progression. We always *increment* here to cancel out the effect of the
            // wall-clock.
            if (tt_activity != 0) {
                tt_time_travel_base_ms += scale8(RGB_MANAGER_FRAME_MS, quadwave8(abs(tt_activity)));
            }
        }

        int16_t calculate_shift(int8_t tt_multiplier) {
            const int16_t tt_animation = tt_animation_speed_10x * tt_multiplier;
            if (tt_activity == 0 || tt_animation == 0) {
                // TT movement has no effect
                return 0;
            }

            return tt_animation * tt_activity / 127;            
        }

        void update_shift(int8_t tt_multiplier) {
            shift_value += calculate_shift(tt_multiplier);
        }

        void next_random8() {
            // pick one that is not too similar to the previous one
            current_random8 = current_random8 + random8(30, UINT8_MAX-30);
        }

        CRGB& get_user_color(uint8_t color) {
            switch (color) {
                case 0:
                default:
                    return rgb_off;

                case 1:
                    return rgb_primary;

                case 2:
                    return rgb_secondary;

                case 3:
                    return rgb_tertiary;
            }
        }

    public:
        void init(rgb_config* config) {
            // parse flags
            this->flags = config->Flags;
            this->tt_fade_out_time = 0;
            if (config->Flags.FadeOutFast) {
                this->tt_fade_out_time += 400;
            }
            if (config->Flags.FadeOutSlow) {
                this->tt_fade_out_time += 800;
            }

            crgb_from_colorrgb(config->RgbPrimary, this->rgb_primary);
            crgb_from_colorrgb(config->RgbSecondary, this->rgb_secondary);
            crgb_from_colorrgb(config->RgbTertiary, this->rgb_tertiary);

            this->default_darkness = config->Darkness;
            this->idle_brightness = config->IdleBrightness;

            this->idle_animation_speed =
                calculate_adjusted_speed((WS2812B_Mode)config->Mode, config->IdleAnimationSpeed);
            
            this->tt_animation_speed_10x = config->TtAnimationSpeed;

            set_mode(
                (WS2812B_Mode)config->Mode,
                (WS2812B_Palette)config->ColorPalette,
                config->Multiplicity);

            this->num_leds = config->NumberOfLeds;
            ws2812b_global.init(config->NumberOfLeds, config->Flags.FlipDirection);
            FastLED.addLeds<ArcinController>(leds, num_leds);
            FastLED.setCorrection(TypicalLEDStrip);
            // we can't afford to call into FastLED too often, so disable temporal dithering
            FastLED.setDither(DISABLE_DITHER);
            set_off();
        }

        void update_from_hid(ColorRgb color) {
            if (!global_led_enable || !flags.EnableHidControl) {
                return;
            }
            last_hid_report = Time::time();

            CRGB crgb;
            crgb_from_colorrgb(color, crgb);
            this->update_static(crgb);
        }

        // tt +1 is clockwise, -1 is counter-clockwise
        void update_colors(int8_t tt) {
            // prevent frequent updates - use 20ms as the framerate. This framerate will have
            // downstream effects on the various color algorithms below.
            uint32_t now = Time::time();
            if ((now - last_outdated_hid_check) < RGB_MANAGER_FRAME_MS) {
                return;
            }
            last_outdated_hid_check = now;

            // if there was a HID report recently, don't take over control
            if ((last_hid_report != 0) && ((now - last_hid_report) < 5000)) {
                return;
            }
            if (!global_led_enable) {
                this->set_off();
                return;
            }

            if (flags.ReactToTt){
                update_turntable_activity(now, tt);
            }

            switch(rgb_mode) {
                case WS2812B_MODE_STATIC_RAINBOW:
                {
                    // +20 seems good
                    update_shift(20);

                    uint8_t index = beat8(idle_animation_speed, tt_time_travel_base_ms);

                    // +20 seems good
                    index += (shift_value >> 8);

                    CRGB color = ColorFromPalette(current_palette, index);
                    this->update_static(color);
                }
                break;

                case WS2812B_MODE_RAINBOW_WAVE:
                {
                    // -60 seems good
                    update_shift(-60);

                    uint8_t step = 255 / (num_leds * (multiplicity + 1) / 2);

                    // we actually want to go "backwards" so that each color seem to be rotating clockwise.
                    uint8_t start_index =
                        UINT8_MAX - beat8(idle_animation_speed, tt_time_travel_base_ms);

                    start_index += (shift_value >> 8);

                    fill_palette(
                        leds,
                        num_leds,
                        start_index,
                        step,
                        current_palette,
                        UINT8_MAX,
                        LINEARBLEND
                        );
                    show();
                }
                break;

                case WS2812B_MODE_TRICOLOR:
                {
                    // +60 seems good
                    update_shift(60);

                    const uint16_t beat = beat16(idle_animation_speed, tt_time_travel_base_ms) + shift_value;
                    ws2812b_global.set_right_shift(pick_led_number(num_leds, beat));
                    uint8_t color_index = 0;
                    for (uint8_t led = 0; led < num_leds; led++) {
                        leds[led] = get_user_color(color_index + 1);
                        color_index = (color_index + 1) % 3;
                    }
                    this->show();
                }
                break;

                case WS2812B_MODE_TWO_COLOR_FADE:
                {
                    uint8_t progress;
                    if (this->flags.ReactToTt) {
                        // normally, all LEDs are primary color
                        // any turntable activity instantly changes to secondary color, then it
                        //     graudally fades back to the primary color
                        progress = quadwave8(abs(tt_activity));
                    } else {
                        // reverse sawtooth (spike and ease out) + smoothing
                        progress = UINT8_MAX - ease8InOutQuad(beat8(idle_animation_speed));
                    }

                    CRGB rgb = ColorFromPalette(current_palette, progress);
                    this->update_static(rgb);
                }
                break;

                case WS2812B_MODE_RANDOM_HUE:
                {
                    if (this->flags.ReactToTt) {
                        if ((this->previous_tt == 0) && (124 <= abs(tt_activity))) {
                            // TT triggered, time to pick a new hue value
                            next_random8();
                        }

                        CRGB rgb = ColorFromPalette(current_palette, current_random8);
                        this->update_static(rgb);

                    } else {
                        // sawtooth (drop to 0 and ease up) + smoothing
                        uint8_t darkness = ease8InOutQuad(beat8(idle_animation_speed));

                        // detect spikes
                        if (darkness < previous_value) {
                            next_random8();
                        }
                        previous_value = darkness;

                        CRGB rgb = ColorFromPalette(current_palette, current_random8);
                        rgb.fadeToBlackBy(darkness);
                        this->update_static(rgb);
                    }
                }
                break;

                case WS2812B_MODE_DOTS:
                case WS2812B_MODE_DIVISIONS:
                {
                    // +80 seems good.
                    update_shift(80);

                    uint8_t dot1 = 0;
                    uint8_t dot2;
                    uint8_t dot3;
                    get_divisions(multiplicity, num_leds, dot1, dot2, dot3);

                    const uint16_t beat = beat16(idle_animation_speed, tt_time_travel_base_ms) + shift_value;
                    ws2812b_global.set_right_shift(pick_led_number(num_leds, beat));

                    CRGB current_color;
                    uint8_t current_division = 1;
                    for (uint8_t led = 0; led < num_leds; led++) {
                        switch (rgb_mode) {
                            case WS2812B_MODE_DIVISIONS:
                                if (led == dot2) {
                                    current_division = 2;
                                } else if (led == dot3) {
                                    current_division = 3;
                                }
                                current_color = get_user_color(current_division);
                                break;
                            
                            case WS2812B_MODE_DOTS:
                            default:
                                if (led == dot1) {
                                    current_color = get_user_color(1);
                                } else if (led == dot2) {
                                    current_color = get_user_color(2);
                                } else if (led == dot3) {
                                    current_color = get_user_color(3);
                                } else {
                                    current_color = get_user_color(0);
                                }
                                break;
                        }

                        leds[led] = current_color;
                    }
                    this->show();
                }
                break;

                case WS2812B_MODE_PRIDE:
                {
                    animation_pride_2015(leds, num_leds);
                    this->show();
                }
                break;

                case WS2812B_MODE_PACIFICA:
                {
                    animation_pacifica(leds, num_leds);
                    this->show();
                }
                break;

                case WS2812B_MODE_SINGLE_COLOR:
                default:
                {
                    if (this->idle_animation_speed == 0 || this->flags.ReactToTt) {
                        // just use a solid color, and let the turntable dimming logic take care of
                        // fade in/out
                        this->update_static(rgb_primary);
                    } else {
                        uint8_t brightness = beatsin8(idle_animation_speed, 20);
                        CRGB rgb = rgb_primary;
                        rgb.fadeToBlackBy(UINT8_MAX - brightness);
                        this->update_static(rgb);
                    }
                }
                break;
            }

            if (flags.ReactToTt){
                this->previous_tt = tt;
            }
        }

        void irq() {
            ws2812b_global.irq();
        }
};

#endif
