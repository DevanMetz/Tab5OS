#include "ipv4_data.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    const struct { const char *text; uint32_t value; } examples[] = {
        {"0.0.0.0", 0}, {"255.255.255.255", UINT32_MAX},
        {"1.2.3.4", UINT32_C(0x01020304)}, {"127.0.0.1", UINT32_C(0x7f000001)},
        {"192.168.1.100", UINT32_C(0xc0a80164)}, {"224.0.0.1", UINT32_C(0xe0000001)},
        {"255.0.0.1", UINT32_C(0xff000001)}
    };
    for (size_t i = 0; i < sizeof(examples) / sizeof(examples[0]); i++) {
        uint32_t address;
        assert(ipv4_parse(examples[i].text, &address) && address == examples[i].value);
        struct { char text[IPV4_TEXT_SIZE]; char guard[4]; } formatted;
        memset(&formatted, '!', sizeof(formatted));
        ipv4_format(address, formatted.text);
        assert(!strcmp(formatted.text, examples[i].text));
        assert(!memcmp(formatted.guard, "!!!!", 4));
    }
    const char *const invalid[] = {
        NULL, "", ".", "...", "1", "127.1", "1.2.3", "1.2.3.", ".1.2.3", "1..2.3",
        "1.2.3.4.", "1.2.3.4.5", "01.2.3.4", "1.02.3.4", "1.2.03.4", "1.2.3.04",
        "00.0.0.0", "256.0.0.1", "1.256.0.1", "1.2.256.1", "1.2.3.256", "9999.2.3.4",
        "2130706433", "0x7f000001", "0x7f.0.0.1", "0177.0.0.1", "127.0.0.01",
        " 1.2.3.4", "1.2.3.4 ", "1.2.3.4\t", "1.2.3.4\n", "1.2.3.4\r", "1.2.3.4\f",
        "1.2.3.4\v", "1.2.\n3.4", "1.2.3.-4", "+1.2.3.4", "1e1.2.3.4", "1,2,3,4",
        "example.com", "[1.2.3.4]", "1.2.3.4/24", "1.2.3.4:123", "::1", "1.2.3.4junk",
        "255.255.255.2551", "\xc2\xb9.2.3.4", "1.2.3.4\xc2\xa0"
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        uint32_t address = UINT32_MAX;
        assert(!ipv4_parse(invalid[i], &address) && address == 0);
    }
    assert(!ipv4_parse("1.2.3.4", NULL));
    ipv4_format(0, NULL);
    /* Exercise every possible octet in every position, including its decimal
     * width transitions and the numeric byte-order contract used by sockets. */
    unsigned checked = 0;
    for (unsigned position = 0; position < 4; position++) {
        for (unsigned octet = 0; octet <= 255; octet++) {
            unsigned parts[] = {1, 2, 3, 4}; parts[position] = octet;
            char text[IPV4_TEXT_SIZE];
            snprintf(text, sizeof(text), "%u.%u.%u.%u", parts[0], parts[1], parts[2], parts[3]);
            unsigned shift = (3 - position) * 8;
            uint32_t expected = (UINT32_C(0x01020304) & ~(UINT32_C(255) << shift)) | (uint32_t)octet << shift;
            uint32_t address;
            assert(ipv4_parse(text, &address) && address == expected);
            char formatted[IPV4_TEXT_SIZE]; ipv4_format(address, formatted);
            assert(!strcmp(text, formatted));
            checked++;
        }
    }
    printf("Strict IPv4 valid/invalid/output checks and %u octet-position cases passed\n", checked);
    return 0;
}
