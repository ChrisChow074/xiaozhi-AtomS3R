#pragma once

#include <driver/i2c_master.h>  // 【新增這行】
#include <functional>
#include "button.h"
#include "lvgl.h"

// 這是我們對外開放的三個「鉤子 (Hooks)」
void CustomApp_InitUI(lv_obj_t* parent);
void CustomApp_SetEmotion(const char* emotion);
void CustomApp_HijackButtons(Button& boot_button, std::function<void()> on_ai_single_click,
                             std::function<void()> on_ai_long_press);
void CustomApp_ProvideI2CBus(i2c_master_bus_handle_t bus_handle);
// 接收語音文字指令的鉤子
void CustomApp_OnChatMessage(const char* role, const char* content);
