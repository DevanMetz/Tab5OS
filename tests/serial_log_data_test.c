#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "serial_log_data.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static FILE *input(const char *text)
{
    FILE *file = tmpfile();
    assert(file);
    assert(fputs(text, file) >= 0);
    rewind(file);
    return file;
}

int main(void)
{
    assert(serial_log_detect("unix_time,direction,data_hex"));
    assert(serial_log_detect("\xef\xbb\xbfunix_time,direction,data_hex"));
    assert(!serial_log_detect("unix_time,direction,value"));

    serial_log_row_t rows[3];
    serial_log_data_t data;
    FILE *file = input("unix_time,direction,data_hex\r\n"
                       "100,TX,48 65 6C 6C 6F 0D 0A\r\n"
                       "101,RX,00 ff 7F\r\n"
                       "102,RX,0G\r\n"
                       "103,BAD,01\r\n"
                       "104,TX,41");
    assert(serial_log_read(file, rows, 3, &data));
    assert(data.count == 3 && data.ignored == 2 && !data.truncated);
    assert(rows[0].unix_time == 100 && rows[0].tx && rows[0].length == 7);
    assert(rows[0].bytes[0] == 'H' && rows[0].bytes[6] == '\n');
    assert(rows[1].unix_time == 101 && !rows[1].tx && rows[1].length == 3);
    assert(rows[1].bytes[0] == 0 && rows[1].bytes[1] == 0xff);
    assert(rows[2].unix_time == 104 && rows[2].bytes[0] == 'A');
    fclose(file);

    file = input("unix_time,direction,data_hex\n100,TX,01\n101,RX,02\n");
    assert(serial_log_read(file, rows, 1, &data));
    assert(data.count == 1 && data.truncated);
    fclose(file);

    file = tmpfile();
    assert(file);
    assert(fputs("unix_time,direction,data_hex\n100,TX,", file) >= 0);
    for (int i = 0; i < 256; i++) assert(fprintf(file, "%s%02X", i ? " " : "", i) > 0);
    assert(fputs("\n101,RX,", file) >= 0);
    for (int i = 0; i < 257; i++) assert(fprintf(file, "%s%02X", i ? " " : "", i & 255) > 0);
    assert(fputc('\n', file) != EOF);
    rewind(file);
    assert(serial_log_read(file, rows, 3, &data));
    assert(data.count == 1 && data.ignored == 1 && rows[0].length == 256);
    fclose(file);

    file = input("not a serial log\n100,TX,01\n");
    assert(!serial_log_read(file, rows, 3, &data));
    fclose(file);

    puts("serial log data tests passed");
    return 0;
}
