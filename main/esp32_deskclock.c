#include <stdio.h>
#include <epd_driver.h>
#include <firasans.h>

void app_main(void)
{
    uint8_t *fbuf;
    int32_t x=0, y=50;
    fbuf = (uint8_t *)calloc(sizeof(uint8_t), EPD_WIDTH * EPD_HEIGHT / 2);

    FontProperties props = {
        .fg_color = 15,
        .bg_color = 0,
        .fallback_glyph = 0,
        .flags = 0
    };


    epd_init();
    epd_poweron();
    epd_clear();
    
    writeln((GFXfont *)&FiraSans, "➸ RTC is probe failed!  😂 \n", &x, &y, NULL);
    
    // epd_draw_grayscale_image(epd_full_screen(), fbuf);
    
    while(1);
}
