#include <pedal_core/bypass.hpp>
#include <pedal_core/hal.hpp>

static bool     s_active      = false;
static uint8_t  s_flash_count = 0;
static bool     s_flash_on    = false;
static uint32_t s_flash_ms    = 0;
static constexpr uint32_t FLASH_PERIOD_MS = 100;

static void sync_relay()
{
    // Monostable relay: engaged puts the effect in the signal path;
    // disengaged is true bypass. Drive polarity is the product's hal.
    pedal_core::hal::bypass_relay(s_active);
}

static void led_write(bool on)
{
    pedal_core::hal::panel_led(0u, on);
}

static void sync_led()
{
    led_write(s_active);
}

void bypass::init()
{
    pedal_core::hal::panel_led_pins_init();
    pedal_core::hal::bypass_pins_init();

    // Power on bypassed: relay disengaged, effect out of the signal path.
    s_active = false;
    sync_relay();
    sync_led();
}

void bypass::set_active(bool active)
{
    s_active = active;
    sync_relay();
    if (s_flash_count == 0) sync_led();
}

void bypass::toggle()
{
    set_active(!s_active);
}

bool bypass::is_active()
{
    return s_active;
}

void bypass::flash(uint8_t count)
{
    s_flash_count = count * 2;  // on + off per flash
    s_flash_on    = s_active;   // match current LED state so first toggle is immediately visible
    s_flash_ms    = systick::now_ms();
}

void bypass::update()
{
    if (s_flash_count == 0) return;

    const uint32_t now = systick::now_ms();
    if ((now - s_flash_ms) >= FLASH_PERIOD_MS) {
        s_flash_ms = now;
        s_flash_on = !s_flash_on;
        --s_flash_count;

        if (s_flash_count == 0) {
            sync_led();  // relay already follows s_active; just restore LED
        } else {
            led_write(s_flash_on);
        }
    }
}
