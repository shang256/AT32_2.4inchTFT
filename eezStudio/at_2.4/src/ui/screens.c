#include <string.h>

#include "screens.h"
#include "images.h"
#include "fonts.h"
#include "actions.h"
#include "vars.h"
#include "styles.h"
#include "ui.h"

#include <string.h>

objects_t objects;

screen_main_state_t screen_main_state;

//
// Event handlers
//

lv_obj_t *tick_value_change_obj;

//
// Screens
//

void create_screen_main() {
//    screen_main_state_t *state = &screen_main_state;
//    (void)state;
//    lv_obj_t *obj = lv_obj_create(0);
//    objects.main = obj;
//    lv_obj_set_pos(obj, 0, 0);
//    lv_obj_set_size(obj, 240, 240);
//    {
//        lv_obj_t *parent_obj = obj;
//        {
//            lv_obj_t *obj = lv_label_create(parent_obj);
//            lv_obj_set_pos(obj, 10, 10);
//            lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
//            lv_label_set_text_static(obj, "Hello, world!");
//        }
//        {
//            lv_obj_t *obj = lv_arc_create(parent_obj);
//            lv_obj_set_pos(obj, 49, 103);
//            lv_obj_set_size(obj, 108, 120);
//            lv_arc_set_value(obj, 25);
//        }
//        {
//            lv_obj_t *obj = lv_img_create(parent_obj);
//            lv_obj_set_pos(obj, 40, 151);
//            lv_obj_set_size(obj, 190, 72);
//            lv_img_set_src(obj, &img_success);
//            lv_img_set_size_mode(obj, LV_IMG_SIZE_MODE_REAL);
//            lv_obj_set_style_bg_img_src(obj, &img_success, LV_PART_MAIN | LV_STATE_DEFAULT);
//        }
//        {
//            lv_obj_t *obj = lv_meter_create(parent_obj);
//            lv_obj_set_pos(obj, 71, 2);
//            lv_obj_set_size(obj, 163, 155);
//            {
//                lv_meter_scale_t *scale = lv_meter_add_scale(obj);
//                state->scale = scale;
//                lv_meter_set_scale_ticks(obj, scale, 41, 1, 5, lv_color_hex(0xa0a0a0));
//                lv_meter_set_scale_major_ticks(obj, scale, 8, 3, 10, lv_color_hex(0x000000), 10);
//                lv_meter_set_scale_range(obj, scale, 0, 100, 300, 120);
//                {
//                    lv_meter_indicator_t *indicator = lv_meter_add_needle_line(obj, scale, 3, lv_color_hex(0x0000ff), -28);
//                    state->indicator = indicator;
//                    lv_meter_set_indicator_value(obj, indicator, 30);
//                }
//            }
//        }
//    }
//    
//    tick_screen_main();
}

void tick_screen_main() {
    screen_main_state_t *state = &screen_main_state;
    (void)state;
}

typedef void (*tick_screen_func_t)();
tick_screen_func_t tick_screen_funcs[] = {
    tick_screen_main,
};
void tick_screen(int screen_index) {
    if (screen_index >= 0 && screen_index < 1) {
        tick_screen_funcs[screen_index]();
    }
}
void tick_screen_by_id(enum ScreensEnum screenId) {
    tick_screen(screenId - 1);
}

//
// Fonts
//

ext_font_desc_t fonts[] = {
#if LV_FONT_MONTSERRAT_8
    { "MONTSERRAT_8", &lv_font_montserrat_8 },
#endif
#if LV_FONT_MONTSERRAT_10
    { "MONTSERRAT_10", &lv_font_montserrat_10 },
#endif
#if LV_FONT_MONTSERRAT_12
    { "MONTSERRAT_12", &lv_font_montserrat_12 },
#endif
#if LV_FONT_MONTSERRAT_14
    { "MONTSERRAT_14", &lv_font_montserrat_14 },
#endif
#if LV_FONT_MONTSERRAT_16
    { "MONTSERRAT_16", &lv_font_montserrat_16 },
#endif
#if LV_FONT_MONTSERRAT_18
    { "MONTSERRAT_18", &lv_font_montserrat_18 },
#endif
#if LV_FONT_MONTSERRAT_20
    { "MONTSERRAT_20", &lv_font_montserrat_20 },
#endif
#if LV_FONT_MONTSERRAT_22
    { "MONTSERRAT_22", &lv_font_montserrat_22 },
#endif
#if LV_FONT_MONTSERRAT_24
    { "MONTSERRAT_24", &lv_font_montserrat_24 },
#endif
#if LV_FONT_MONTSERRAT_26
    { "MONTSERRAT_26", &lv_font_montserrat_26 },
#endif
#if LV_FONT_MONTSERRAT_28
    { "MONTSERRAT_28", &lv_font_montserrat_28 },
#endif
#if LV_FONT_MONTSERRAT_30
    { "MONTSERRAT_30", &lv_font_montserrat_30 },
#endif
#if LV_FONT_MONTSERRAT_32
    { "MONTSERRAT_32", &lv_font_montserrat_32 },
#endif
#if LV_FONT_MONTSERRAT_34
    { "MONTSERRAT_34", &lv_font_montserrat_34 },
#endif
#if LV_FONT_MONTSERRAT_36
    { "MONTSERRAT_36", &lv_font_montserrat_36 },
#endif
#if LV_FONT_MONTSERRAT_38
    { "MONTSERRAT_38", &lv_font_montserrat_38 },
#endif
#if LV_FONT_MONTSERRAT_40
    { "MONTSERRAT_40", &lv_font_montserrat_40 },
#endif
#if LV_FONT_MONTSERRAT_42
    { "MONTSERRAT_42", &lv_font_montserrat_42 },
#endif
#if LV_FONT_MONTSERRAT_44
    { "MONTSERRAT_44", &lv_font_montserrat_44 },
#endif
#if LV_FONT_MONTSERRAT_46
    { "MONTSERRAT_46", &lv_font_montserrat_46 },
#endif
#if LV_FONT_MONTSERRAT_48
    { "MONTSERRAT_48", &lv_font_montserrat_48 },
#endif
};

//
// Color themes
//

uint32_t active_theme_index = 0;

//
//
//

void create_screens() {

// Set default LVGL theme
    lv_disp_t *dispp = lv_disp_get_default();
    lv_theme_t *theme = lv_theme_default_init(dispp, lv_palette_main(LV_PALETTE_BLUE), lv_palette_main(LV_PALETTE_RED), false, LV_FONT_DEFAULT);
    lv_disp_set_theme(dispp, theme);
    
    // Initialize screens
    // Create screens
    create_screen_main();
}