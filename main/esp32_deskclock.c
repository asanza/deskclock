#include <stdio.h>
#include <epd_driver.h>

void app_main(void)
{
    epd_init();
    epd_poweron();
    epd_clear();
    while(1);
}
