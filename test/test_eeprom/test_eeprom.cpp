// Host-native unit tests for the SPI EEPROM driver (drivers/eeprom.cpp).
//
// eeprom.cpp talks to the SPI bus + the hal CS line, so — like the other driver
// tests — we compile it into this TU via #include behind a recording SPI stub and
// no-op hal pin stubs. The stub also lets read() return programmable bytes
// (the status register's WIP bit and read-back data). This pins the host-portable
// logic: the READ/WRITE command framing (opcode + 16-bit big-endian address), the
// WREN-before-write sequence, and the 64-byte page-boundary splitting.

#include <unity.h>
#include <cstdint>
#include <vector>

#include "pedal_core_config.hpp"
#include <pedal_core/hal.hpp>

// --- Recording SPI stub ----------------------------------------------------
// Captures every byte written; serves a scripted sequence to transfer()'s rx so
// the status-register WIP poll terminates and reads return known data.
namespace spi {
    static std::vector<uint8_t> g_tx;     // all bytes clocked out (write + transfer-tx)
    static std::vector<uint8_t> g_rx_feed; // bytes handed back by transfer(), in order
    static size_t g_rx_pos = 0;

    void init() {}
    void write(const uint8_t* d, uint16_t n) { for (uint16_t i = 0; i < n; ++i) g_tx.push_back(d[i]); }
    void transfer(const uint8_t* tx, uint8_t* rx, uint16_t n) {
        for (uint16_t i = 0; i < n; ++i) {
            g_tx.push_back(tx ? tx[i] : 0xFFu);
            const uint8_t v = (g_rx_pos < g_rx_feed.size()) ? g_rx_feed[g_rx_pos++] : 0x00u;
            if (rx) rx[i] = v;
        }
    }
}

// The CS line and pin init are the product's; here they are no-ops.
namespace pedal_core::hal {
    void eeprom_pins_init() {}
    void eeprom_cs(bool) {}
}

#include "../../src/eeprom.cpp"

void setUp(void) {
    spi::g_tx.clear();
    // Feed the boot health probe (eeprom::init): its write's WIP poll reads an idle status
    // (0x00), then its read-back must return the 0xA5 sentinel so the part is marked healthy
    // and read()/write() aren't gated off. Reset the feed afterwards for the test body.
    spi::g_rx_feed = {0x00, 0xA5};
    spi::g_rx_pos = 0;
    eeprom::init();
    TEST_ASSERT_TRUE(eeprom::healthy());
    spi::g_tx.clear();   // drop init/probe traffic
    spi::g_rx_feed.clear();
    spi::g_rx_pos = 0;
}
void tearDown(void) {}

// read() frames a 0x03 READ opcode + big-endian address, then clocks the data in.
void test_read_command_framing(void) {
    spi::g_rx_feed = {0xAA, 0xBB, 0xCC};
    uint8_t buf[3] = {0};
    TEST_ASSERT_TRUE(eeprom::read(0x1234, buf, 3));
    // First three TX bytes are the command header.
    TEST_ASSERT_EQUAL_UINT8(0x03, spi::g_tx[0]);   // READ
    TEST_ASSERT_EQUAL_UINT8(0x12, spi::g_tx[1]);   // addr hi
    TEST_ASSERT_EQUAL_UINT8(0x34, spi::g_tx[2]);   // addr lo
    // Data clocked back in.
    TEST_ASSERT_EQUAL_UINT8(0xAA, buf[0]);
    TEST_ASSERT_EQUAL_UINT8(0xBB, buf[1]);
    TEST_ASSERT_EQUAL_UINT8(0xCC, buf[2]);
}

// write() sends WREN (0x06), then a 0x02 WRITE opcode + address + data; the WIP
// poll (status reg fed 0x00 = idle) terminates immediately.
void test_write_command_framing(void) {
    const uint8_t data[2] = {0xDE, 0xAD};
    TEST_ASSERT_TRUE(eeprom::write(0x0040, data, 2));
    TEST_ASSERT_EQUAL_UINT8(0x06, spi::g_tx[0]);   // WREN
    TEST_ASSERT_EQUAL_UINT8(0x02, spi::g_tx[1]);   // WRITE
    TEST_ASSERT_EQUAL_UINT8(0x00, spi::g_tx[2]);   // addr hi
    TEST_ASSERT_EQUAL_UINT8(0x40, spi::g_tx[3]);   // addr lo
    TEST_ASSERT_EQUAL_UINT8(0xDE, spi::g_tx[4]);
    TEST_ASSERT_EQUAL_UINT8(0xAD, spi::g_tx[5]);
    TEST_ASSERT_EQUAL_UINT8(0x05, spi::g_tx[6]);   // RDSR (WIP poll)
}

// A write straddling a 64-byte page boundary is split into two WREN+WRITE cycles,
// the second starting at the next page origin.
void test_write_splits_on_page_boundary(void) {
    uint8_t data[4] = {1, 2, 3, 4};
    TEST_ASSERT_TRUE(eeprom::write(62, data, 4));   // 62,63 | 64,65 → two pages
    // First cycle: WREN, WRITE, addr 0x00,0x3E, 2 data bytes, then RDSR.
    TEST_ASSERT_EQUAL_UINT8(0x06, spi::g_tx[0]);
    TEST_ASSERT_EQUAL_UINT8(0x02, spi::g_tx[1]);
    TEST_ASSERT_EQUAL_UINT8(0x00, spi::g_tx[2]);
    TEST_ASSERT_EQUAL_UINT8(62u,  spi::g_tx[3]);
    TEST_ASSERT_EQUAL_UINT8(1u,   spi::g_tx[4]);
    TEST_ASSERT_EQUAL_UINT8(2u,   spi::g_tx[5]);
    TEST_ASSERT_EQUAL_UINT8(0x05, spi::g_tx[6]);    // first WIP poll (RDSR + a dummy byte follow)
    // Second cycle starts with another WREN + WRITE at address 64.
    TEST_ASSERT_EQUAL_UINT8(0x06, spi::g_tx[8]);
    TEST_ASSERT_EQUAL_UINT8(0x02, spi::g_tx[9]);
    TEST_ASSERT_EQUAL_UINT8(0x00, spi::g_tx[10]);
    TEST_ASSERT_EQUAL_UINT8(64u,  spi::g_tx[11]);
    TEST_ASSERT_EQUAL_UINT8(3u,   spi::g_tx[12]);
    TEST_ASSERT_EQUAL_UINT8(4u,   spi::g_tx[13]);
}

// A probe whose read-back doesn't match the sentinel (a dead/absent part, or a blank one
// reading 0xFF) marks the device unhealthy. read()/write() then serve the volatile RAM
// mirror: it comes up blank (all 0xFF, like a fresh device), a write round-trips through
// it, and not one byte reaches the bus.
void test_failed_probe_falls_back_to_ram(void) {
    spi::g_tx.clear();
    spi::g_rx_feed = {0x00, 0x00};   // WIP idle, but read-back != 0xA5 sentinel
    spi::g_rx_pos = 0;
    eeprom::init();
    TEST_ASSERT_FALSE(eeprom::healthy());

    spi::g_tx.clear();
    uint8_t buf[2] = {0};
    TEST_ASSERT_TRUE(eeprom::read(0x0010, buf, 2));
    TEST_ASSERT_EQUAL_UINT8(0xFFu, buf[0]);        // mirror presents as a blank device
    TEST_ASSERT_EQUAL_UINT8(0xFFu, buf[1]);

    const uint8_t data[2] = {0xDE, 0xAD};
    TEST_ASSERT_TRUE(eeprom::write(0x0010, data, 2));
    TEST_ASSERT_TRUE(eeprom::read(0x0010, buf, 2));
    TEST_ASSERT_EQUAL_UINT8(0xDEu, buf[0]);        // ...and reads back what was stored
    TEST_ASSERT_EQUAL_UINT8(0xADu, buf[1]);

    TEST_ASSERT_TRUE(spi::g_tx.empty());           // never touched the bus
}

// The mirror covers the stored map only (presets, global config, last-slot ring). An access
// past its end — which on a healthy part would reach the probe byte at the top of the
// device — fails rather than running off the array.
void test_ram_fallback_rejects_out_of_range(void) {
    spi::g_rx_feed = {0x00, 0x00};   // fail the probe → RAM mirror
    spi::g_rx_pos = 0;
    eeprom::init();
    TEST_ASSERT_FALSE(eeprom::healthy());

    uint8_t buf[2] = {0};
    TEST_ASSERT_FALSE(eeprom::read(EEPROM_STORE_SIZE, buf, 1));
    TEST_ASSERT_FALSE(eeprom::write(EEPROM_STORE_SIZE, buf, 1));
    TEST_ASSERT_FALSE(eeprom::read((uint16_t)(EEPROM_STORE_SIZE - 1u), buf, 2));  // straddles the end
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_read_command_framing);
    RUN_TEST(test_write_command_framing);
    RUN_TEST(test_write_splits_on_page_boundary);
    RUN_TEST(test_failed_probe_falls_back_to_ram);
    RUN_TEST(test_ram_fallback_rejects_out_of_range);
    return UNITY_END();
}
