/* Test phases are sequential. Retain the exact original open-bus image
 * length while sharing storage with the later 32-MiB packet boundary image. */
_Alignas(4) static union {
    uint8_t open_bus[0x1000000+32];
    uint8_t packet[GBN_ROM_MAX_SIZE];
} shared_test_rom;
#define open_bus_large_rom shared_test_rom.open_bus
#define packet_large_rom shared_test_rom.packet
