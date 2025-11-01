#include <stdio.h>
#include <epd_driver.h>

void app_main(void)
{
    Rect_t area = {
        .x = 230,
        .y = 0,
        .width = 20,
        .height = 20,
    };
    epd_poweron();
    epd_clear_area(area);
}
