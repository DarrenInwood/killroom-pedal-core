#pragma once
#include <cstdint>

// The hardware seam. Every .cpp in this library talks to silicon exclusively
// through the declarations below; no CMSIS header, register, or pin number ever
// appears on the library side of this line. The consuming product implements
// them twice: once over its own GPIO/SPI/clock drivers for the hardware build,
// and once as fakes for its native tests. That is the whole porting contract —
// a pedal on a different MCU family reimplements this file's functions and the
// library compiles unchanged.
//
// The `spi`, `systick` and `watchdog` namespaces carry the same signatures the
// products already use for their own drivers, so the hardware implementation is
// the product's existing driver and no adapter exists.

namespace spi {
    // Clock out len bytes, discarding what comes back.
    void write(const uint8_t* data, uint16_t len);

    // Full-duplex: clock out tx[i] (or 0xFF if tx == nullptr) and store the
    // received byte in rx[i] (if rx != nullptr).
    void transfer(const uint8_t* tx, uint8_t* rx, uint16_t len);
}

namespace systick {
    uint32_t now_ms();
    void     delay_ms(uint32_t ms);
}

namespace watchdog {
    // Called inside any bounded busy-wait the library runs (the EEPROM's
    // write-in-progress poll), so a slow part cannot feed a watchdog reset.
    void kick();
}

namespace pedal_core::hal {
    // Pin plumbing. `select`/`on`/`in_reset` are logical states; the product
    // maps each to its electrical level and port/pin, so an active-low chip
    // select or an inverted enable is the product's business.
    //
    // The *_pins_init() calls configure the lines as outputs and leave every
    // device deselected and unpowered; the library calls them once, at the top
    // of the owning driver's init().
    void eeprom_pins_init();
    void eeprom_cs(bool select);

    void display_pins_init();
    void display_cs(bool select);
    void display_dc_data(bool data);    // true = pixel data, false = command
    void display_reset(bool in_reset);
    void display_power(bool on);        // panel supply / boost enable

    // Footswitches: raw pressed state for the library's debouncer. `idx` is
    // 0-based in the product's own switch order; polarity and pull-ups are the
    // product's business. footswitch_pins_init() configures the inputs and is
    // called once from footswitch::init() — a product whose panel bring-up
    // already configured them implements it as a forward or a no-op.
    void footswitch_pins_init();
    bool fs_pressed(uint8_t idx);
}
