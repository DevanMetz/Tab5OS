#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "capture_data.h"

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
    assert(capture_data_detect("unix_time,elapsed_us,gpio,millivolts,sample_rate_hz,offset_mv,scale_permille") ==
           CAPTURE_FORMAT_SCOPE);
    assert(capture_data_detect("unix_time,address,register,value,status,speed_khz") == CAPTURE_FORMAT_I2C);
    assert(capture_data_detect("not a capture") == CAPTURE_FORMAT_NONE);

    capture_point_t rows[4];
    capture_data_t data;
    FILE *file = input("unix_time,elapsed_us,gpio,millivolts,sample_rate_hz,offset_mv,scale_permille\n"
                       "100,0,16,1200,5000,0,1000\n"
                       "100,200,16,1300,5000,0,1000\n"
                       "bad,line\n"
                       "100,400,16,5000,5000,0,1000\n");
    assert(capture_data_read(file, rows, 4, &data));
    assert(data.format == CAPTURE_FORMAT_SCOPE && data.count == 2 && data.ignored == 2);
    assert(!data.truncated);
    assert(rows[0].time == 0 && rows[0].value == 1200 && rows[0].valid);
    assert(rows[1].time == 200 && rows[1].value == 1300 && rows[1].valid);
    fclose(file);

    file = input("unix_time,address,register,value,status,speed_khz\r\n"
                 "100,0x76,0xD0,0x60,ESP_OK,100\r\n"
                 "101,0x76,0xD0,,ESP_ERR_TIMEOUT,100\r\n"
                 "102,0x76,0xD0,0x61,ESP_OK,100\r\n");
    assert(capture_data_read(file, rows, 2, &data));
    assert(data.format == CAPTURE_FORMAT_I2C && data.count == 2 && data.truncated);
    assert(rows[0].time == 100 && rows[0].value == 0x60 && rows[0].valid);
    assert(rows[1].time == 101 && !rows[1].valid);
    fclose(file);

    file = input("unix_time,address,register,value,status,speed_khz\n"
                 "103,0x76,0xD0,0x60,ESP_OK,100");
    assert(capture_data_read(file, rows, 4, &data));
    assert(data.count == 1 && rows[0].time == 103 && rows[0].value == 0x60);
    fclose(file);

    file = input("name,value\n1,2\n");
    assert(!capture_data_read(file, rows, 4, &data));
    fclose(file);
    puts("capture data tests passed");
    return 0;
}
