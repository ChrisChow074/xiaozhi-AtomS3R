#include "my_custom_app.h"
#include <driver/i2c_master.h>
#include <esp_log.h>
#include <stdlib.h>
// 提供 abs() 絕對值函數
#include <cJSON.h>
#include <sdkconfig.h>
#include <string.h>
#include <time.h>
#include "application.h"
#include "bmi270_api.h"
#include "board.h"
#include "driver/gpio.h"
#undef CONFIG_TOUCH_SUPPRESS_DEPRECATE_WARN
#define CONFIG_TOUCH_SUPPRESS_DEPRECATE_WARN 1
#include "driver/touch_pad.h"
#include "esp_http_client.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

// static const char* TAG = "MyCustomApp";

// ==========================================
// 天氣模式專屬變數
// ==========================================
LV_FONT_DECLARE(micro5_65);

// 1. 時間模式變數
static lv_obj_t* bg_time_mode = nullptr;
static lv_obj_t* time_mode_time_label = nullptr;
static lv_obj_t* time_mode_date_label = nullptr;

// 2. 天氣模式變數
static lv_obj_t* bg_weather_mode = nullptr;
static lv_obj_t* weather_icon_container = nullptr;
static lv_obj_t* weather_icon_label = nullptr;
static lv_obj_t* weather_current_label = nullptr;
static lv_obj_t* weather_hilo_label = nullptr;

static char last_drawn_weather[32] = "";  // 紀錄上次畫的天氣，避免每秒重複繪製導致閃爍

// 控制時鐘亮暗與 5 秒自動倒數
static bool clock_is_bright = false;  // 目前是否為高亮狀態
static int clock_dim_countdown = 0;   // 倒數計時器 (每 100ms 扣 1)
const int CLOCK_DIM_TIMEOUT_SEC = 5;  // 亮起後等待幾秒自動變暗 (可自行改成 3 或 5 秒)

#define EYE_BASE_W 22  // 眼睛基準寬度 (想放大改這裡即可，例如改 24)
#define EYE_BASE_H 28  // 眼睛基準高度 (想放大改這裡即可，例如改 24)
#define EYE_POS_L -30
#define EYE_POS_R 30
#define EYE_POS_H -8
#define EYE_CORNER 10  // (LV_RADIUS_CIRCLE)
#define MOUTH_BASE_W 26
#define MOUTH_BASE_H 8
#define MOUTH_POS_H 12

#define BRIGHTNESS_DIM 5

// ==========================================
//           螢幕解析度常數 (M5StickS3)
// ==========================================
#define LCD_W 128
#define LCD_H 128

// ==========================================
//           老虎機 (Lucky Slot) 系統變數
// ==========================================
LV_IMG_DECLARE(slot_36);
#define SLOT_WIDTH 36

enum SlotState {
    SLOT_STATE_IDLE,
    SLOT_STATE_SPINNING,
    SLOT_STATE_STOP_1,
    SLOT_STATE_STOP_2,
    SLOT_STATE_GAMEOVER
};
static SlotState slot_game_state = SLOT_STATE_IDLE;

static lv_obj_t* bg_slot = nullptr;
static lv_obj_t* slot_col[3] = {nullptr};
static lv_obj_t* slot_reel[3] = {nullptr};  // <--- 新增
static lv_obj_t* slot_img1[3] = {nullptr};  // <--- 新增
static lv_obj_t* slot_img2[3] = {nullptr};  // <--- 新增
static lv_obj_t* slot_score_label = nullptr;
static lv_obj_t* slot_status_label = nullptr;

static int slot_results[3] = {0};  // 紀錄三排的最終圖案索引 (0~7)
static int slot_score = 100;       // 玩家初始金幣

// ==========================================
//           IMU (BMI270) 全新驅動設定
// ==========================================
#define BMI270_SENSOR_ADDR 0x68
#define BMI270_REG_CHIP_ID 0x00
#define BMI270_REG_PWR_CONF 0x7C
#define BMI270_REG_PWR_CTRL 0x7D
#define BMI270_REG_ACC_CONF 0x40
#define BMI270_REG_ACC_RANGE 0x41
#define BMI270_REG_DATA_X_LSB 0x0C

// 新版 API 專用的設備代碼 (Handle)
static i2c_master_dev_handle_t imu_dev_handle = nullptr;
static bool imu_initialized = false;
static bool device_added = false;
static i2c_master_bus_handle_t shared_i2c_bus = nullptr;  // 存放借來的總線

// 接收鑰匙的函數
void CustomApp_ProvideI2CBus(i2c_master_bus_handle_t bus_handle) { shared_i2c_bus = bus_handle; }

// --- UI 與狀態變數 ---
static std::string current_active_emotion = "neutral";
static lv_obj_t* face_container = nullptr;
static lv_obj_t* avatar_left_eye = nullptr;
static lv_obj_t* avatar_right_eye = nullptr;
static lv_obj_t* left_eye_highlight = nullptr;
static lv_obj_t* right_eye_highlight = nullptr;
static void set_eye_size(int width, int height) {
    if (avatar_left_eye)
        lv_obj_set_size(avatar_left_eye, width, height);
    if (avatar_right_eye)
        lv_obj_set_size(avatar_right_eye, width, height);
}

static lv_obj_t* avatar_mouth = nullptr;
static lv_obj_t* avatar_heart = nullptr;
static lv_obj_t* avatar_kissy = nullptr;
static lv_obj_t* avatar_sweat = nullptr;
static lv_obj_t* avatar_anger = nullptr;
static lv_obj_t* avatar_embrassing = nullptr;
static lv_obj_t* avatar_sad_eye = nullptr;
static lv_obj_t* avatar_surprised_eye = nullptr;
static lv_obj_t* avatar_angry_eye = nullptr;
static lv_obj_t* avatar_mouth_laugh = nullptr;
static lv_obj_t* avatar_silly_tongue = nullptr;
static lv_obj_t* avatar_mouth_happy = nullptr;
static lv_obj_t* avatar_cool = nullptr;
static lv_obj_t* avatar_sleepy_z = nullptr;
static lv_obj_t* avatar_tears = nullptr;
static lv_obj_t* avatar_swallow = nullptr;
static lv_obj_t* avatar_confuse = nullptr;

static lv_color_t avatar_theme_color;
static lv_color_t avatar_theme_bg_color;

static lv_obj_t* game_menu_bg = nullptr;
static lv_obj_t* game_menu_label = nullptr;
static lv_obj_t* menu_title_label = nullptr;  // <--- 用來動態改變標題

static DeviceState last_avatar_state = (DeviceState)-1;
static int idle_to_clock_counter = 0;
static bool is_tarot_mode = false;

volatile int current_system_mode = 0;  // 0: AI, 1: 遊戲選單, 2: 遊戲中
volatile int current_ui_mode = 0;
volatile bool ui_mode_changed = false;
volatile int pending_game_action = 0;
volatile int pending_touch_wake = 0;  // 觸摸後的表情展示倒數

// 儲存原本 AI 模式下的按鍵動作
static std::function<void()> ai_single_click_cb = nullptr;
static std::function<void()> ai_long_press_cb = nullptr;

static void imu_init() {
    if (imu_initialized)
        return;
    if (!shared_i2c_bus) {
        ESP_LOGE("IMU", "未提供 I2C Bus Handle!");
        return;
    }

    // 1. 掛載設備 (AtomS3R 內部頻率建議 100kHz~400kHz)
    if (!device_added) {
        i2c_device_config_t dev_config = {};
        dev_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        dev_config.device_address = BMI270_SENSOR_ADDR;  // 0x68
        dev_config.scl_speed_hz = 100000;
        esp_err_t err = i2c_master_bus_add_device(shared_i2c_bus, &dev_config, &imu_dev_handle);
        if (err != ESP_OK) {
            ESP_LOGE("IMU", "無法掛載 BMI270: %s", esp_err_to_name(err));
            return;
        }
        device_added = true;
    }

    // 2. 軟復位 (Soft Reset)
    uint8_t reset_cmd[2] = {0x7E, 0xB6};
    i2c_master_transmit(imu_dev_handle, reset_cmd, 2, -1);
    vTaskDelay(pdMS_TO_TICKS(50));  // 重啟後至少等待 50ms (Bosch 規定至少 2ms，給予充裕時間)

    // 3. 關閉進階省電模式 (PWR_CONF: 0x7C -> 0x00)
    uint8_t pwr_conf[2] = {BMI270_REG_PWR_CONF, 0x00};
    i2c_master_transmit(imu_dev_handle, pwr_conf, 2, -1);
    vTaskDelay(pdMS_TO_TICKS(5));

    // 4. 開始準備傳輸韌體
    ESP_LOGI("IMU", "開始載入 BMI270 Firmware...");
    uint8_t init_ctrl_0[2] = {0x59, 0x00};
    i2c_master_transmit(imu_dev_handle, init_ctrl_0, 2, -1);

    // 5. 分塊寫入 Firmware (按照 Bosch 官方標準：每次最多 32/64 bytes)
    // 注意：每次傳入 INIT_DATA (0x5E) 時，晶片內部指標會自動遞增！
    // 官方驅動只在傳輸開始時設定一次起始地址 (0x0000)，或者每 chunk 更新。
    const int CHUNK_SIZE = 64;
    uint8_t tx_buf[CHUNK_SIZE + 1];
    tx_buf[0] = 0x5E;  // INIT_DATA 暫存器

    for (int i = 0; i < BMI270_CONFIG_FILE_SIZE; i += CHUNK_SIZE) {
        uint16_t word_addr = i / 2;
        uint8_t set_addr[3] = {
            0x5B,
            (uint8_t)(word_addr & 0x0F),  // INIT_ADDR_0 (LSB 4-bits)
            (uint8_t)((word_addr >> 4) &
                      0xFF)  // INIT_ADDR_1 (MSB 8-bits)  <-- 【關鍵修正！】Bosch 的 Word
                             // 位址不是單純 >> 8，而是 4-bit / 8-bit 分割！
        };

        if (i2c_master_transmit(imu_dev_handle, set_addr, 3, -1) != ESP_OK) {
            ESP_LOGE("IMU", "設定記憶體位址失敗 (Offset: %d)", i);
            return;
        }

        int send_len =
            (BMI270_CONFIG_FILE_SIZE - i) < CHUNK_SIZE ? (BMI270_CONFIG_FILE_SIZE - i) : CHUNK_SIZE;
        memcpy(&tx_buf[1], &bmi270_config_file[i], send_len);

        if (i2c_master_transmit(imu_dev_handle, tx_buf, send_len + 1, -1) != ESP_OK) {
            ESP_LOGE("IMU", "Firmware 寫入失敗 (Offset: %d)", i);
            return;
        }
    }

    // 6. 結束 Firmware 寫入
    uint8_t init_ctrl_1[2] = {0x59, 0x01};
    i2c_master_transmit(imu_dev_handle, init_ctrl_1, 2, -1);

    ESP_LOGI("IMU", "等待 DSP 驗證 Firmware...");
    vTaskDelay(pdMS_TO_TICKS(150));

    // 7. 檢查初始化狀態 (0x21)
    uint8_t status_reg = 0x21;
    uint8_t status_val = 0;
    i2c_master_transmit_receive(imu_dev_handle, &status_reg, 1, &status_val, 1, -1);

    if ((status_val & 0x0F) != 0x01) {
        ESP_LOGE("IMU", "Firmware 初始化失敗！狀態值: 0x%02X", status_val);
        // 如果狀態值是 0x02，代表微碼校驗失敗
        return;
    }
    ESP_LOGI("IMU", "★ Firmware 載入成功！(Status: 0x%02X)", status_val);

    // 8. 啟用加速度計、陀螺儀與溫度感測 (PWR_CTRL = 0x0E: aux_en, gyr_en, acc_en)
    // 如果只想開 Accel，設為 0x04 即可
    uint8_t pwr_ctrl[2] = {BMI270_REG_PWR_CTRL, 0x0E};
    i2c_master_transmit(imu_dev_handle, pwr_ctrl, 2, -1);
    vTaskDelay(pdMS_TO_TICKS(10));

    // 9. 配置加速度計參數 (ACC_CONF 0x40: 0xA8 代表 100Hz ODR, 濾波性能優先)
    uint8_t acc_conf[2] = {BMI270_REG_ACC_CONF, 0xA8};
    i2c_master_transmit(imu_dev_handle, acc_conf, 2, -1);
    vTaskDelay(pdMS_TO_TICKS(2));

    // 10. 配置加速度計量程 (ACC_RANGE 0x41: 0x01 代表 ±4g, 0x00 代表 ±2g)
    uint8_t acc_range[2] = {BMI270_REG_ACC_RANGE, 0x01};  // ±4g
    i2c_master_transmit(imu_dev_handle, acc_range, 2, -1);
    vTaskDelay(pdMS_TO_TICKS(2));

    imu_initialized = true;
    ESP_LOGI("IMU", "★ BMI270 準備就緒！");
}

// ==========================================
//           巴士動態顯示面板變數
// ==========================================
static lv_obj_t* bg_bus = nullptr;
static lv_obj_t* bus_route_label = nullptr;
static lv_obj_t* bus_t1_label = nullptr;
static lv_obj_t* bus_t2_label = nullptr;
static TaskHandle_t bus_task_handle = nullptr;

static char bus_route_str[16] = "";
static char bus_t1_str[32] = "";
static char bus_t2_str[32] = "";
static int bus_show_countdown = 0;  // 顯示倒數計時器

// ==========================================
// IMU 獨立背景初始化任務 (防卡死畫面)
// ==========================================
static void imu_init_task(void* pvParameters) {
    // 1. 在背景耐心等待，直到系統把 I2C Bus 交接過來
    while (shared_i2c_bus == nullptr) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // 2. 拿到 I2C Bus 後，開始執行初始化 (這裡面的 Delay 就不會卡到畫面了)
    ESP_LOGI("IMU", "背景任務開始初始化 BMI270...");
    imu_init();

    // 3. 執行完畢，任務自殺釋放記憶體
    vTaskDelete(NULL);
}

// 背景任務：被喚醒後去抓取巴士時間
static void bus_fetch_task(void* pvParameters) {
    char url[128];
    snprintf(url, sizeof(url), "http://chowcfu.duckdns.org:8001/api/bus");

    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  // 沉睡直到被語音觸發

        // =========================================================
        // 【核心升級】每 1.5 秒問一次伺服器，最多問 15 次 (共等 22 秒)
        // 保證 100% 同步伺服器的計算時間，資料一算完立刻彈出！
        // =========================================================
        for (int attempt = 0; attempt < 15; attempt++) {
            vTaskDelay(pdMS_TO_TICKS(1500));

            esp_http_client_config_t config = {};
            config.url = url;
            config.timeout_ms = 3000;
            esp_http_client_handle_t client = esp_http_client_init(&config);
            bool success_fetched = false;
            bool stop_polling = false;

            if (client) {
                if (esp_http_client_open(client, 0) == ESP_OK) {
                    esp_http_client_fetch_headers(client);
                    char* buffer = (char*)malloc(1024);
                    if (buffer) {
                        int read_len = esp_http_client_read(client, buffer, 1023);
                        if (read_len > 0) {
                            buffer[read_len] = '\0';
                            cJSON* json = cJSON_Parse(buffer);
                            if (json) {
                                cJSON* status = cJSON_GetObjectItem(json, "status");
                                if (status && status->valuestring) {
                                    if (strcmp(status->valuestring, "ok") == 0) {
                                        strncpy(bus_route_str,
                                                cJSON_GetObjectItem(json, "route")->valuestring,
                                                15);
                                        strncpy(bus_t1_str,
                                                cJSON_GetObjectItem(json, "t1")->valuestring, 31);
                                        strncpy(bus_t2_str,
                                                cJSON_GetObjectItem(json, "t2")->valuestring, 31);

                                        bus_show_countdown = 50;  // 觸發面板顯示 5 秒鐘
                                        ESP_LOGI("Bus", "成功抓取巴士資訊: %s", bus_route_str);
                                        success_fetched = true;
                                    } else if (strcmp(status->valuestring, "loading") == 0) {
                                        ESP_LOGI("Bus", "伺服器計算中，繼續等待...");
                                    } else {
                                        // Empty 或 Error 代表沒車或失敗，停止死等
                                        stop_polling = true;
                                    }
                                }
                                cJSON_Delete(json);
                            }
                        }
                        free(buffer);
                    }
                }
                esp_http_client_cleanup(client);
            }

            // 如果成功抓到資料，或者確認沒資料，就跳出重試迴圈
            if (success_fetched || stop_polling) {
                break;
            }
        }
    }
}

// ==========================================
// G1 錫箔紙觸摸喚醒任務
// ==========================================
static void touch_wake_task(void* pvParameters) {
    // 1. 初始化 Touch 模組 (G1 對應 TOUCH_PAD_NUM1)
    touch_pad_init();
    touch_pad_config(TOUCH_PAD_NUM1);
    touch_pad_fsm_start();  // 啟動硬體背景測量

    uint32_t baseline = 0;
    int touch_cooldown = 0;
    int log_timer = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(100));  // 每 100ms 讀取一次

        uint32_t touch_val = 0;
        touch_pad_read_raw_data(TOUCH_PAD_NUM1, &touch_val);

        // 2. 智慧基準值 (Baseline) 自動校準
        if (baseline == 0 && touch_val > 0) {
            baseline = touch_val;
        } else if (touch_val > 0 && touch_val < baseline + 2000) {
            baseline = (baseline * 19 + touch_val) / 20;
        }

        if (++log_timer >= 20) {
            log_timer = 0;
        }

        if (touch_cooldown > 0) {
            touch_cooldown--;
            continue;
        }

        // 4. 觸摸判定！
        if (baseline > 0 && touch_val > baseline + 6000) {
            touch_cooldown = 30;  // 觸發後冷卻 3 秒，防止連觸

            // 👇 【修復關鍵】：加入 current_system_mode == 6，允許在天氣模式下觸摸喚醒！
            if (current_system_mode == 0 || current_system_mode == 3 || current_system_mode == 6) {
                DeviceState state = Application::GetInstance().GetDeviceState();

                // 確保 AI 是安靜待機的狀態，或是正處於時鐘/天氣模式
                if (current_system_mode == 3 || current_system_mode == 6 ||
                    state == kDeviceStateIdle) {
                    ESP_LOGI("Touch", "🌟 錫箔紙觸摸喚醒！數值飆升至: %lu",
                             (unsigned long)touch_val);

                    // 如果是在時鐘或天氣模式，瞬間切回 AI 臉蛋並點亮螢幕
                    if (current_system_mode == 3 || current_system_mode == 6) {
                        auto backlight = Board::GetInstance().GetBacklight();
                        if (backlight)
                            backlight->SetBrightness(100);
                        current_system_mode = 0;
                        current_ui_mode = 0;
                        ui_mode_changed = true;
                        idle_to_clock_counter = 0;
                    }

                    // 設定 1.5 秒倒數計時！
                    pending_touch_wake = 15;
                }
            }
        }
    }
}

// ==========================================
//           時鐘模式：天氣專屬變數
// ==========================================
static int weather_temp_cur = 0;  // 當前即時溫度
static int weather_temp_max = 0;
static int weather_temp_min = 0;
static char weather_desc[32] = "Updating...";
static bool weather_ready = false;

// 背景任務控制碼 ＋ 30秒防刷冷卻時間戳
static TaskHandle_t weather_task_handle = nullptr;
static int64_t last_weather_fetch_us = 0;

// 【重要】填入你申請到的 OpenWeatherMap API Key！
#define OWM_API_KEY "b5aef42ed255665f9024ea34161bcaa7"

// ==========================================
// 天氣更新背景任務
// ==========================================
static void weather_update_task(void* pvParameters) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));

    char url[128];
    // 【修改為你 iMac 的內網 IP 與 Port】
    snprintf(url, sizeof(url), "http://chowcfu.duckdns.org:8001/api/weather/simple");

    while (1) {
        ESP_LOGI("Weather", "▶ 工人起床！開始執行網路請求...");

        esp_http_client_config_t config = {};
        config.url = url;
        config.timeout_ms = 8000;

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (client) {
            esp_err_t err = esp_http_client_open(client, 0);
            if (err == ESP_OK) {
                esp_http_client_fetch_headers(client);
                char* buffer = (char*)malloc(1024);
                if (buffer) {
                    int read_len = esp_http_client_read(client, buffer, 1023);
                    if (read_len > 0) {
                        buffer[read_len] = '\0';
                        cJSON* json = cJSON_Parse(buffer);
                        if (json) {
                            cJSON* status = cJSON_GetObjectItem(json, "status");
                            if (status && status->valuestring &&
                                strcmp(status->valuestring, "ok") == 0) {
                                weather_temp_cur = cJSON_GetObjectItem(json, "t_cur")->valueint;
                                weather_temp_max = cJSON_GetObjectItem(json, "t_max")->valueint;
                                weather_temp_min = cJSON_GetObjectItem(json, "t_min")->valueint;
                                strncpy(weather_desc,
                                        cJSON_GetObjectItem(json, "desc")->valuestring, 31);
                                weather_ready = true;
                                ESP_LOGI("Weather", "✅ GPS 天氣同步成功: %s %d度", weather_desc,
                                         weather_temp_cur);
                            } else {
                                ESP_LOGW("Weather", "❌ 伺服器回傳錯誤狀態！");
                            }
                            cJSON_Delete(json);
                        } else {
                            ESP_LOGW("Weather", "❌ JSON 解析失敗！");
                        }
                    } else {
                        ESP_LOGW("Weather", "❌ 讀取不到數據，可能伺服器沒開");
                    }
                    free(buffer);
                }
            } else {
                ESP_LOGE("Weather", "❌ 無法連上伺服器，HTTP 錯誤碼: %s", esp_err_to_name(err));
            }
            esp_http_client_cleanup(client);
        }

        ESP_LOGI("Weather", "⏸️ 任務進入淺眠待命，等待下一次喚醒或 30 分鐘...");

        // 【核心關鍵！】一定要是 ulTaskNotifyTake，絕對不能是 vTaskDelay！
        // 這樣它才能被按鍵瞬間拍醒！
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1800000));
    }
}

LV_FONT_DECLARE(weather_70);

// 幾何圖形天氣繪製引擎
static void update_weather_icon_ui(const char* desc) {
    if (!weather_icon_container)
        return;
    lv_obj_clean(weather_icon_container);

    // 1. 取得當前時間，判斷是否為白天 (07:00 ~ 18:59)
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    bool is_daytime = (timeinfo.tm_hour >= 7 && timeinfo.tm_hour < 19);

    // 【修改】：避免反覆 lv_obj_clean 產生記憶體碎片，改為重複利用 label
    if (!weather_icon_label) {
        weather_icon_label = lv_label_create(weather_icon_container);
        lv_obj_set_style_text_font(weather_icon_label, &weather_70, 0);
        lv_obj_align(weather_icon_label, LV_ALIGN_CENTER, 0, 0);
    }

    // 根據後端傳來的分類，對應到圖片中的英文字母 (採用線條風格 A-Z)
    if (strstr(desc, "PartlyCloudy")) {
        if (is_daytime) {
            lv_label_set_text(weather_icon_label, "A");
            lv_obj_set_style_text_color(weather_icon_label, lv_color_hex(0xFF9C00), 0);
        } else {
            lv_label_set_text(weather_icon_label, "a");
            lv_obj_set_style_text_color(weather_icon_label, lv_color_hex(0xCCCCCC), 0);
        }
    } else if (strstr(desc, "Sunny") || strstr(desc, "Clear")) {
        if (is_daytime) {
            lv_label_set_text(weather_icon_label, "1");
            lv_obj_set_style_text_color(weather_icon_label, lv_color_hex(0xFF9C00), 0);
        } else {
            lv_label_set_text(weather_icon_label, "6");
            lv_obj_set_style_text_color(weather_icon_label, lv_color_white(), 0);
        }
    } else if (strstr(desc, "Overcast")) {
        lv_label_set_text(weather_icon_label, "3");
        lv_obj_set_style_text_color(weather_icon_label, lv_color_hex(0xAAAAAA), 0);
    } else if (strstr(desc, "Cloudy") || strstr(desc, "Clouds")) {
        lv_label_set_text(weather_icon_label, "3");
        lv_obj_set_style_text_color(weather_icon_label, lv_color_hex(0xAAAAAA), 0);
    } else if (strstr(desc, "HeavyRain")) {
        if (is_daytime) {
            lv_label_set_text(weather_icon_label, "O");
            lv_obj_set_style_text_color(weather_icon_label, lv_color_hex(0x0088FF), 0);
        } else {
            lv_label_set_text(weather_icon_label, "o");
            lv_obj_set_style_text_color(weather_icon_label, lv_color_hex(0x00AAFF), 0);
        }
    } else if (strstr(desc, "Rain") || strstr(desc, "Drizzle")) {
        if (is_daytime) {
            lv_label_set_text(weather_icon_label, "M");
            lv_obj_set_style_text_color(weather_icon_label, lv_color_hex(0x0088FF), 0);
        } else {
            lv_label_set_text(weather_icon_label, "m");
            lv_obj_set_style_text_color(weather_icon_label, lv_color_hex(0x00AAFF), 0);
        }
    } else if (strstr(desc, "Storm") || strstr(desc, "Thunder")) {
        if (is_daytime) {
            lv_label_set_text(weather_icon_label, "Q");
            lv_obj_set_style_text_color(weather_icon_label, lv_color_black(), 0);
        } else {
            lv_label_set_text(weather_icon_label, "q");
            lv_obj_set_style_text_color(weather_icon_label, lv_color_white(), 0);
        }
    } else if (strstr(desc, "Snow")) {
        if (is_daytime) {
            lv_label_set_text(weather_icon_label, "H");
            lv_obj_set_style_text_color(weather_icon_label, lv_color_hex(0x44bbff), 0);
        } else {
            lv_label_set_text(weather_icon_label, "i");
            lv_obj_set_style_text_color(weather_icon_label, lv_color_hex(0xc0e0ff), 0);
        }
    } else if (strstr(desc, "Fog") || strstr(desc, "Mist") || strstr(desc, "Haze")) {
        if (is_daytime) {
            lv_label_set_text(weather_icon_label, "B");
            lv_obj_set_style_text_color(weather_icon_label, lv_color_hex(0xA0B0C0), 0);
        } else {
            lv_label_set_text(weather_icon_label, "c");
            lv_obj_set_style_text_color(weather_icon_label, lv_color_hex(0xBBBBBB), 0);
        }
    } else if (strstr(desc, "Wind")) {
        if (is_daytime) {
            lv_label_set_text(weather_icon_label, "D");
            lv_obj_set_style_text_color(weather_icon_label, lv_color_hex(0xCCCCCC), 0);
        } else {
            lv_label_set_text(weather_icon_label, "e");
            lv_obj_set_style_text_color(weather_icon_label, lv_color_hex(0xCCCCCC), 0);
        }
    } else {
        lv_label_set_text(weather_icon_label, "*");
        lv_obj_set_style_text_color(weather_icon_label, lv_color_hex(0xCCCCCC), 0);
    }
}

// --- 獨立的時// --- 獨立的時間刷新函數 ---
static void update_clock_display() {
    // 獲取當前時間
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    // ==========================================
    // 🎨 日夜主題自動切換邏輯
    // 白天：07:00 ~ 18:59 | 晚上：19:00 ~ 06:59
    // ==========================================
    bool is_daytime = (timeinfo.tm_hour >= 7 && timeinfo.tm_hour < 19);

    if (bg_time_mode) {
        // 設定背景色
        lv_color_t bg_color = is_daytime ? lv_color_white() : lv_color_black();
        lv_obj_set_style_bg_color(bg_time_mode, bg_color, 0);

        // 設定字體顏色 (白天黑字，晚上白字)
        lv_color_t text_primary = is_daytime ? lv_color_black() : lv_color_white();
        // 日期顏色 (白天深藍色，晚上黃色，增添設計感)
        lv_color_t text_accent = is_daytime ? lv_color_hex(0x555555) : lv_color_hex(0xCCCCCC);
        if (time_mode_time_label)
            lv_obj_set_style_text_color(time_mode_time_label, text_primary, 0);
        if (time_mode_date_label)
            lv_obj_set_style_text_color(time_mode_date_label, text_accent, 0);
    }

    char time_str[16];
    char date_str[32];
    // strftime(time_str, sizeof(time_str), "%H:%M", &timeinfo); //24小時制
    // strftime(time_str, sizeof(time_str), "%I:%M%P", &timeinfo);    // 12小時制有AM/PM
    strftime(time_str, sizeof(time_str), "%I:%M", &timeinfo);      // 12小時制有AM/PM
    strftime(date_str, sizeof(date_str), "%m-%d\n%A", &timeinfo);  // 例如 09-23 Wed
    int len = strlen(time_str);
    if (len > 0 && (time_str[len - 1] == 'm' || time_str[len - 1] == 'M')) {
        time_str[len - 1] = '\0';  // 將 'm' 替換 fluid 為字串結束符
    }

    if (time_mode_time_label) {
        lv_label_set_text(time_mode_time_label, time_str);
    }
    if (time_mode_date_label) {
        lv_label_set_text(time_mode_date_label, date_str);
    }

    if (bg_weather_mode) {
        // 設定背景色
        lv_color_t bg_color = is_daytime ? lv_color_white() : lv_color_black();
        lv_obj_set_style_bg_color(bg_weather_mode, bg_color, 0);
        lv_color_t weather_text_primary = is_daytime ? lv_color_black() : lv_color_white();

        if (weather_current_label)
            lv_obj_set_style_text_color(weather_current_label, weather_text_primary, 0);
        if (weather_hilo_label)
            lv_obj_set_style_text_color(weather_hilo_label, weather_text_primary, 0);
    }

    if (weather_current_label && !lv_obj_has_flag(bg_weather_mode, LV_OBJ_FLAG_HIDDEN)) {
        /*
        // ==========================================
        // ⬇️ 測試專用代碼開始 (測試完記得刪除或註解掉)
        // ==========================================
        const char* test_weathers[] = {"Sunny", "PartlyCloudy", "Overcast", "Cloudy", "HeavyRain",
                                       "Rain",  "Storm",        "Snow",     "Fog",    "Wind"};
        // 根據目前的秒數，每 2 秒切換一個陣列索引 (0~9)
        int test_idx = (timeinfo.tm_sec / 2) % 10;

        strncpy(weather_desc, test_weathers[test_idx], sizeof(weather_desc) - 1);
        weather_ready = true;  // 強制讓系統以為資料準備好了
        // ==========================================
        // ⬆️ 測試專用代碼結束
        // ==========================================
        */

        if (weather_ready) {
            // 檢查天氣狀態有沒有變，有變才重新畫幾何圖形 (防閃爍)
            if (strcmp(last_drawn_weather, weather_desc) != 0) {
                strncpy(last_drawn_weather, weather_desc, sizeof(last_drawn_weather) - 1);
                update_weather_icon_ui(weather_desc);
            }

            // 右上：氣溫與狀態換行
            lv_label_set_text_fmt(weather_current_label, "%s\n%d°C", weather_desc,
                                  weather_temp_cur);

            // 右下：最高低溫
            lv_label_set_text_fmt(weather_hilo_label, "H:%d°C\nL:%d°C", weather_temp_max,
                                  weather_temp_min);
        } else {
            // 如果還在載入中
            if (strcmp(last_drawn_weather, "Loading") != 0) {
                strcpy(last_drawn_weather, "Loading");
                update_weather_icon_ui("Unknown");  // 會畫出問號
            }
            lv_label_set_text(weather_current_label, "Loading\nWeather...");
            lv_label_set_text(weather_hilo_label, "--");
        }
    }
}

// ==========================================
// 系統清單對照表 (未來修改順序，只要改這裡就好！)
// ==========================================
enum AppIndex {
    APP_BWA = 0,    // 擲杯
    APP_MARK6,      // 六合彩
    APP_LEVEL,      // 新增的水平儀
    APP_STOPWATCH,  // 碼表
    APP_TIMER,      // 新增的倒數計時器
    APP_MAX         // 自動計算總數
};

enum GameIndex {
    GAME_SPACE = 0,  // 太空侵略者
    GAME_SLOT,       // 老虎機
    GAME_TICTACTOE,  // 過三關
    GAME_BREAKOUT,   // 打磚塊
    GAME_2048,       // 2048
    GAME_SNAKE,      // 貪食蛇
    GAME_MAX         // 自動計算總數
};

static int app_menu_selection = APP_BWA;
static int game_menu_selection = GAME_SPACE;

// ==========================================
// 水平儀 (Spirit Level) 系統變數
// ==========================================
static lv_obj_t* bg_level = nullptr;
static lv_obj_t* level_target_circle = nullptr;
static lv_obj_t* inside_circle = nullptr;
static lv_obj_t* level_bubble = nullptr;
static lv_obj_t* level_info_label = nullptr;

// ==========================================
// 倒數計時器 (Timer) 系統變數
// ==========================================
enum TimerState { TIMER_IDLE, TIMER_RUNNING, TIMER_PAUSED, TIMER_ALARM };
static TimerState timer_state = TIMER_IDLE;
static int64_t timer_total_us = 0;     // 總設定微秒數
static int64_t timer_remain_us = 0;    // 剩餘微秒數
static int64_t timer_end_time_us = 0;  // 系統預計結束時間
static lv_obj_t* bg_timer = nullptr;
static lv_obj_t* timer_time_label = nullptr;
static lv_obj_t* timer_ms_label = nullptr;  // 新增：毫秒標籤
static lv_obj_t* timer_status_label = nullptr;

// ==========================================
// 碼表 (Stop Watch) 系統變數
// ==========================================
enum StopwatchState { SW_IDLE, SW_RUNNING, SW_PAUSED };
static StopwatchState sw_state = SW_IDLE;
static int64_t sw_start_time_us = 0;
static int64_t sw_accumulated_us = 0;
static lv_obj_t* bg_stopwatch = nullptr;
static lv_obj_t* sw_time_label = nullptr;
static lv_obj_t* sw_ms_label = nullptr;  // 毫秒標籤
static lv_obj_t* sw_status_label = nullptr;

// ==========================================
//        過三關 (Tic-Tac-Toe) 系統變數
// ==========================================
enum TicTacToeState { TTT_STATE_START, TTT_STATE_PLAYING, TTT_STATE_PAUSE, TTT_STATE_GAMEOVER };

static lv_obj_t* bg_tictactoe = nullptr;
static lv_obj_t* ttt_cells[3][3] = {nullptr};
static lv_obj_t* ttt_labels[3][3] = {nullptr};
static lv_obj_t* ttt_overlay = nullptr;
static lv_obj_t* ttt_overlay_label = nullptr;
static lv_obj_t* ttt_toast_bg = nullptr;
static lv_obj_t* ttt_toast_label = nullptr;
static int ttt_toast_timer = 0;  // Toast 顯示倒數
static int pending_ttt_r = -1;   // 安全跨執行緒下棋 R
static int pending_ttt_c = -1;   // 安全跨執行緒下棋 C

static int ttt_board[3][3] = {0};  // 0:空, 1:玩家(O), 2:電腦(X)
static TicTacToeState ttt_game_state = TTT_STATE_START;
static bool ttt_user_turn = true;   // 紀錄現在是誰的回合
static int ttt_ai_delay_ticks = 0;  // 讓電腦假裝思考一下的計時器

// 1. 輔助函數：解析單個中文數字 (0-9)
static int get_cn_digit(const std::string& str, size_t pos) {
    if (pos + 3 > str.length())
        return -1;  // 避免 UTF-8 越界
    std::string sub = str.substr(pos, 3);
    if (sub == "一")
        return 1;
    if (sub == "二" || sub == "兩" || sub == "两")
        return 2;
    if (sub == "三")
        return 3;
    if (sub == "四")
        return 4;
    if (sub == "五")
        return 5;
    if (sub == "六")
        return 6;
    if (sub == "七")
        return 7;
    if (sub == "八")
        return 8;
    if (sub == "九")
        return 9;
    if (sub == "零")
        return 0;
    return -1;
}

// 2. 核心函數：解析「單一區段」內的數字
static int parse_single_number(const std::string& str) {
    // A. 阿拉伯數字 (抓取字串中「最後出現」的一組連續數字)
    int current_num = 0;
    int final_num = 0;
    bool has_num = false;
    for (size_t i = 0; i < str.length(); i++) {
        char c = str[i];
        if (c >= '0' && c <= '9') {
            current_num = current_num * 10 + (c - '0');
            has_num = true;
        } else {
            if (has_num) {
                final_num = current_num;
                current_num = 0;
                has_num = false;
            }
        }
    }
    if (has_num)
        final_num = current_num;
    if (final_num > 0)
        return final_num;

    // B. 廣東話「廿」、「卅」
    size_t pos_20 = str.rfind("廿");
    if (pos_20 != std::string::npos) {
        int units = 0;
        int single = get_cn_digit(str, pos_20 + 3);
        if (single > 0)
            units = single;
        return 20 + units;
    }

    size_t pos_30 = str.rfind("卅");
    if (pos_30 != std::string::npos) {
        int units = 0;
        int single = get_cn_digit(str, pos_30 + 3);
        if (single > 0)
            units = single;
        return 30 + units;
    }

    // C. 處理「十」字結構 (如：十五、三十、十)
    size_t pos_10 = str.rfind("十");
    if (pos_10 != std::string::npos) {
        int tens = 1;
        if (pos_10 >= 3) {
            int pre = get_cn_digit(str, pos_10 - 3);
            if (pre > 0)
                tens = pre;
        }
        int units = 0;
        int post = get_cn_digit(str, pos_10 + 3);
        if (post > 0)
            units = post;
        return tens * 10 + units;
    }

    // D. 純個位數 (如：兩、五)
    int last_digit = 0;
    for (size_t i = 0; i + 2 < str.length(); i++) {
        int d = get_cn_digit(str, i);
        if (d > 0)
            last_digit = d;
    }
    return last_digit;
}

// 3. 終極語音時間解析器 (支援「X分Y秒」拆解！)
static int parse_timer_seconds(const std::string& str) {
    // 優先處理特例：「半分」、「分半」
    if (str.find("半分") != std::string::npos)
        return 30;
    if (str.find("分半") != std::string::npos) {
        size_t pos = str.find("分半");
        int m = parse_single_number(str.substr(0, pos));
        if (m == 0)
            m = 1;
        return m * 60 + 30;
    }

    // 尋找「分」與「秒」的位置
    size_t pos_min = str.find("分");
    size_t pos_sec = str.find("秒");

    // ==========================================
    // 情況 A: 同時有分和秒 (例如 "1分15秒", "兩分鐘三十秒")
    // ==========================================
    if (pos_min != std::string::npos && pos_sec != std::string::npos && pos_min < pos_sec) {
        // 切割前半段：算出分鐘
        int m = parse_single_number(str.substr(0, pos_min));
        int s = 0;
        // 切割後半段：算出秒數 (避免越界)
        if (pos_sec > pos_min + 3) {
            s = parse_single_number(str.substr(pos_min + 3, pos_sec - pos_min - 3));
        }
        if (m == 0)
            m = 1;  // 容錯：如果沒聽清楚幾分，預設當作 1 分
        return m * 60 + s;
    }

    // ==========================================
    // 情況 B: 只有分
    // ==========================================
    if (pos_min != std::string::npos) {
        int m = parse_single_number(str.substr(0, pos_min));
        if (m == 0)
            m = 1;  // 沒講數字預設為 1 分鐘
        return m * 60;
    }

    // ==========================================
    // 情況 C: 只有秒
    // ==========================================
    if (pos_sec != std::string::npos) {
        int s = parse_single_number(str.substr(0, pos_sec));
        if (s == 0)
            s = 30;  // 單講秒但沒講數字，預設 30 秒
        return s;
    }

    // ==========================================
    // 情況 D: 都沒有單位
    // ==========================================
    if (str.find("半") != std::string::npos) {
        return 30;  // 單獨講 "半" (當作半分鐘)
    }

    int n = parse_single_number(str);
    if (n > 0)
        return n * 60;  // 只講數字不講單位，預設當作「分鐘」

    // 終極防呆
    return 3 * 60;  // 聽不懂一律 3 分鐘
}

// ==========================================
//        香港六合彩 (Mark Six) 系統變數
// ==========================================
static lv_obj_t* bg_marksix = nullptr;
static lv_obj_t* marksix_balls[6] = {nullptr};
static lv_obj_t* marksix_labels[6] = {nullptr};
static lv_obj_t* marksix_tip_label = nullptr;

static int marksix_drawn_numbers[6] = {0};
static int marksix_count = 0;
static int marksix_shake_cooldown = 0;

// 香港六合彩官方紅、藍、綠波規則判斷
static lv_color_t get_mark_six_color(int num) {
    // 紅色波 (17個)
    static const int red_balls[] = {1, 2, 7, 8, 12, 13, 18, 19, 23, 24, 29, 30, 34, 35, 40, 45, 46};
    // 藍色波 (16個)
    static const int blue_balls[] = {3, 4, 9, 10, 14, 15, 20, 25, 26, 31, 36, 37, 41, 42, 47, 48};
    // 綠色波 (16個)
    // static const int green_balls[] = {5, 6, 11, 16, 17, 21, 22, 27, 28, 32, 33, 38, 39, 43, 44,
    // 49};

    for (int r : red_balls) {
        if (num == r)
            return lv_color_hex(0xE53935);  // 正紅色
    }
    for (int b : blue_balls) {
        if (num == b)
            return lv_color_hex(0x1E88E5);  // 寶藍色
    }
    return lv_color_hex(0x43A047);  // 草綠色
}

// 重置 6 顆號碼球為空白待抽狀態
static void reset_mark_six_ui() {
    marksix_count = 0;
    memset(marksix_drawn_numbers, 0, sizeof(marksix_drawn_numbers));
    for (int i = 0; i < 6; i++) {
        if (marksix_balls[i]) {
            lv_obj_set_style_border_color(marksix_balls[i], lv_color_hex(0x444444), 0);
            lv_obj_set_style_border_width(marksix_balls[i], 2, 0);
            lv_obj_set_style_bg_color(marksix_balls[i], lv_color_hex(0x1A1A1A), 0);  // 暗灰底
        }
        if (marksix_labels[i]) {
            lv_label_set_text(marksix_labels[i], "?");
            lv_obj_set_style_text_color(marksix_labels[i], lv_color_hex(0x666666), 0);
        }
    }
    if (marksix_tip_label) {
        lv_label_set_text(marksix_tip_label, "Lucky Draw (0/6)");
    }
}

// 抽出一顆新號碼球 (不重複 1~49)
static void draw_one_mark_six_ball() {
    if (marksix_count >= 6) {
        // 抽滿 6 顆後，再按/再搖一次就清空重開一局！
        reset_mark_six_ui();
        return;
    }

    // 隨機抽一顆不重複的號碼
    int num = 0;
    bool duplicate;
    do {
        duplicate = false;
        num = (esp_random() % 49) + 1;
        for (int i = 0; i < marksix_count; i++) {
            if (marksix_drawn_numbers[i] == num) {
                duplicate = true;
                break;
            }
        }
    } while (duplicate);

    marksix_drawn_numbers[marksix_count] = num;
    int idx = marksix_count;
    marksix_count++;

    // 換上六合彩官方代表色 (粗邊框 + 純白球心 + 黑色粗字)
    lv_color_t ball_color = get_mark_six_color(num);
    lv_obj_set_style_border_color(marksix_balls[idx], ball_color, 0);
    lv_obj_set_style_border_width(marksix_balls[idx], 4, 0);             // 粗顏色線
    lv_obj_set_style_bg_color(marksix_balls[idx], lv_color_white(), 0);  // 中間純白
    lv_label_set_text_fmt(marksix_labels[idx], "%02d", num);
    lv_obj_set_style_text_color(marksix_labels[idx], lv_color_black(), 0);  // 黑色數字

    // 更新底部提示
    if (marksix_tip_label) {
        if (marksix_count < 6) {
            lv_label_set_text_fmt(marksix_tip_label, "Lucky Draw (%d/6)", marksix_count);
        } else {
            lv_label_set_text(marksix_tip_label, "LUCKY 6 READY!");
        }
    }
    ESP_LOGI("MarkSix", "抽中第 %d 顆號碼球: %02d", marksix_count, num);
}

// ==========================================
//        太空侵略者 (Space Invaders) 系統變數
// ==========================================
#define SPACE_MAX_BULLETS 4
#define SPACE_MAX_ALIEN_ROWS 6
#define SPACE_ALIEN_COLS 4
// 【外星人起始 Y 座標】距離頂部
#define SPACE_ALIEN_START_Y 20
#define SPACE_ALIEN_W 12  // 敵人寬度（原本是 10，建議可改 12 或 14）
#define SPACE_MAX_BOMBS 8
#define SPACE_ALIEN_H 8               // 敵人高度（原本是 10，建議可改 10 或 12）
#define SPACE_DESCENT_INTERVAL_SEC 8  // 【可在這調節】：每 6 秒自動向下逼近一次
#define SPACE_DESCENT_STEP_Y 12       // 【可在這調節】：每次向下移一層的距離 (像素)

static int space_descent_timer = 0;  // 下移計時器 (每 100ms 累加 1)

enum SpaceState { SPACE_STATE_START, SPACE_STATE_PLAYING, SPACE_STATE_PAUSE, SPACE_STATE_GAMEOVER };

struct SpaceBullet {
    bool active;
    int x;
    int y;
    lv_obj_t* obj;
};

struct SpaceBomb {
    bool active;
    int x;
    int y;
    lv_obj_t* obj;
};

struct SpaceAlien {
    bool active;
    lv_obj_t* obj;
};

static lv_obj_t* bg_space = nullptr;
static lv_obj_t* space_ship = nullptr;
static lv_obj_t* space_score_label = nullptr;
static lv_obj_t* space_overlay = nullptr;
static lv_obj_t* space_overlay_label = nullptr;

static SpaceBullet space_bullets[SPACE_MAX_BULLETS];
static SpaceBomb space_bombs[SPACE_MAX_BOMBS];
static SpaceAlien space_aliens[SPACE_MAX_ALIEN_ROWS][SPACE_ALIEN_COLS];  // 大小擴展至 6x4

static int space_score = 0;
static int space_best_score = 0;
static int space_level = 1;        // 【新增】：當前關卡
static int space_active_rows = 3;  // 【新增】：當前實際敵人層數 (預設 3)
static SpaceState space_game_state = SPACE_STATE_START;
static int space_ship_x = 64;  // 飛船 X 座標 (置中)

// 外星人集團的移動狀態
static int alien_group_x = 0;
static int alien_group_y = SPACE_ALIEN_START_Y;
static int alien_dir = 1;  // 1:向右, -1:向左
static int alien_speed = 10;

// ==========================================
//        打磚塊 (Breakout) 系統變數
// ==========================================
#define BK_MAX_ROWS 6
#define BK_COLS 5
// ==========================================
// 【調節位置】：磚塊起始位置常數
// ==========================================
#define BK_BRICK_START_X 8   // 磚塊起始 X (置中)
#define BK_BRICK_START_Y 18  // 【在這裡調節磚塊與分數的距離！】(原為 32 或 18)
#define BK_BRICK_W 23        // 磚塊寬度
#define BK_BRICK_H 8         // 磚塊高度
#define BK_BRICK_STEP_Y 10   // 垂直間距 (高 8 + 間隙 2)

enum BreakoutState { BK_STATE_START, BK_STATE_PLAYING, BK_STATE_PAUSE, BK_STATE_GAMEOVER };

static int bk_active_rows = 4;  // 當前關卡的實際磚塊行數
static lv_obj_t* bk_bricks[BK_MAX_ROWS][BK_COLS];
static bool bk_bricks_active[BK_MAX_ROWS][BK_COLS];

static lv_obj_t* bg_breakout = nullptr;
static lv_obj_t* bk_paddle = nullptr;
static lv_obj_t* bk_ball = nullptr;
static lv_obj_t* bk_score_label = nullptr;
static lv_obj_t* bk_overlay = nullptr;
static lv_obj_t* bk_overlay_label = nullptr;

static int bk_score = 0;
static int bk_best_score = 0;
static int bk_level = 1;
static BreakoutState bk_game_state = BK_STATE_START;

// 物理與座標變數
static int bk_paddle_x = 50;
static int bk_paddle_w = 28;  // 擋板寬度 (隨關卡變短)
static int bk_ball_x = 62;
static int bk_ball_y = 100;
static int bk_ball_dx = 4;   // X 軸速度
static int bk_ball_dy = -4;  // Y 軸速度 (隨關卡變快)
static bool bk_ball_launched = false;

// 載入與儲存最高分
static void load_bk_best_score() {
    nvs_handle_t nvs_h;
    if (nvs_open("storage", NVS_READONLY, &nvs_h) == ESP_OK) {
        int32_t val = 0;
        if (nvs_get_i32(nvs_h, "bk_best", &val) == ESP_OK)
            bk_best_score = val;
        nvs_close(nvs_h);
    }
}
static void save_bk_best_score(int score) {
    nvs_handle_t nvs_h;
    if (nvs_open("storage", NVS_READWRITE, &nvs_h) == ESP_OK) {
        nvs_set_i32(nvs_h, "bk_best", score);
        nvs_commit(nvs_h);
        nvs_close(nvs_h);
    }
}

// 初始化/重置一個新關卡
static void init_breakout_level(bool is_new_game) {
    if (is_new_game) {
        bk_score = 0;
        bk_level = 1;
    }
    bk_active_rows = 4 + (bk_level - 1) / 3;
    if (bk_active_rows > BK_MAX_ROWS) {
        bk_active_rows = BK_MAX_ROWS;
    }

    bk_paddle_w = 28 - (bk_level * 2);
    if (bk_paddle_w < 14)
        bk_paddle_w = 14;

    int speed = 4 + (bk_level / 2);
    if (speed > 8)
        speed = 8;

    bk_paddle_x = (LCD_W - bk_paddle_w) / 2;

    // 【修改這裡】重置發射狀態，並把球黏在擋板正中央！
    bk_ball_launched = false;
    bk_ball_x = bk_paddle_x + (bk_paddle_w / 2) - 3;  // 減 3 是因為球寬 6
    bk_ball_y = LCD_H * 0.9 - 6;                      // 擋板 Y 是 116，球高 6，所以 116 - 6 = 110

    bk_ball_dx = (esp_random() % 2 == 0) ? speed : -speed;
    bk_ball_dy = -speed;

    if (bk_paddle) {
        lv_obj_set_width(bk_paddle, bk_paddle_w);
        lv_obj_align(bk_paddle, LV_ALIGN_TOP_LEFT, bk_paddle_x, LCD_H * 0.9);
    }
    if (bk_ball)
        lv_obj_align(bk_ball, LV_ALIGN_TOP_LEFT, bk_ball_x, bk_ball_y);

    // 重置所有磚塊
    for (int r = 0; r < BK_MAX_ROWS; r++) {
        for (int c = 0; c < BK_COLS; c++) {
            if (r < bk_active_rows) {
                bk_bricks_active[r][c] = true;
                if (bk_bricks[r][c])
                    lv_obj_remove_flag(bk_bricks[r][c], LV_OBJ_FLAG_HIDDEN);
            } else {
                bk_bricks_active[r][c] = false;
                if (bk_bricks[r][c])
                    lv_obj_add_flag(bk_bricks[r][c], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

// ==========================================
//           擲杯 (Bwa Pei) 系統變數
// ==========================================
static lv_obj_t* bg_bwa = nullptr;
static lv_obj_t* bwa_1_bg = nullptr;
static lv_obj_t* bwa_2_bg = nullptr;
static lv_obj_t* bwa_result_label = nullptr;
static lv_obj_t* bwa_desc_label = nullptr;
static int bwa_roll_anim_ticks = 0;  // 滾動動畫倒數
static bool is_initial_state = true;

// 設定筊杯狀態 (val: 0=平/淺木色, 1=凸/鮮紅色)
static void set_bwa_face(int val1, int val2, bool is_rolling) {
    if (!bwa_1_bg || !bwa_2_bg)
        return;

    // 定義真實筊杯的顏色
    lv_color_t color_flat = lv_color_hex(0xFFBB99);   // 淺木色 (平)
    lv_color_t color_curve = lv_color_hex(0xFF4444);  // 鮮紅色 (凸)
    lv_obj_set_style_bg_color(bwa_1_bg, val1 == 0 ? color_flat : color_curve, 0);
    lv_obj_set_style_bg_color(bwa_2_bg, val2 == 0 ? color_flat : color_curve, 0);

    // 如果正在搖晃中
    if (is_rolling) {
        is_initial_state = false;
        lv_label_set_text(bwa_result_label, "搖動中...");
        lv_label_set_text(bwa_desc_label, "誠心祈求");
        lv_obj_set_style_text_color(bwa_result_label, lv_color_white(), 0);
        lv_obj_set_style_text_color(bwa_desc_label, lv_color_white(), 0);
        return;
    }

    if (is_initial_state) {
        lv_label_set_text(bwa_result_label, "誠心祈求");
        lv_label_set_text(bwa_desc_label, "心中想著問題");
        lv_obj_set_style_text_color(bwa_result_label, lv_color_white(), 0);
        lv_obj_set_style_text_color(bwa_desc_label, lv_color_white(), 0);
        return;
    }

    // ==========================================
    // 結算與文字顯示 (隨機抽選解讀句子)
    // ==========================================
    if ((val1 == 0 && val2 == 1) || (val1 == 1 && val2 == 0)) {
        // 一平一凸：聖杯
        lv_label_set_text(bwa_result_label, "【 聖 杯 】");
        lv_obj_set_style_text_color(bwa_result_label, lv_color_make(255, 200, 0), 0);

        const char* descs[] = {"神明允許", "行事順利", "神明同意", "大吉大利"};
        lv_label_set_text(bwa_desc_label, descs[esp_random() % 4]);
        lv_obj_set_style_text_color(bwa_desc_label, lv_color_white(), 0);
    } else if (val1 == 0 && val2 == 0) {
        // 兩平：笑杯
        lv_label_set_text(bwa_result_label, "【 笑 杯 】");
        lv_obj_set_style_text_color(bwa_result_label, lv_color_white(), 0);

        const char* descs[] = {"神明一笑", "狀況不明", "考慮中..."};
        lv_label_set_text(bwa_desc_label, descs[esp_random() % 3]);
        lv_obj_set_style_text_color(bwa_desc_label, lv_color_white(), 0);
    } else {
        // 兩凸：陰杯
        lv_label_set_text(bwa_result_label, "【 陰 杯 】");
        lv_obj_set_style_text_color(bwa_result_label, lv_color_hex(0xFF7702), 0);  // 紅色否定

        const char* descs[] = {"神明反對", "不宜行事", "神明否定", "時機未到"};
        lv_label_set_text(bwa_desc_label, descs[esp_random() % 4]);
        lv_obj_set_style_text_line_space(bwa_desc_label, -4, 0);
        lv_obj_set_style_text_color(bwa_desc_label, lv_color_white(), 0);
    }
}

// ==========================================
//           2048 遊戲引擎與變數
// ==========================================
static lv_obj_t* bg_2048 = nullptr;
static lv_obj_t* tiles_2048[3][3];
static lv_obj_t* labels_2048[3][3];
static lv_obj_t* score_label = nullptr;
static lv_obj_t* overlay_2048 = nullptr;
static lv_obj_t* overlay_label = nullptr;

static int board_2048[3][3] = {0};
static int board_backup[3][3] = {0};  // 悔棋備份
static int current_score = 0;
static int score_backup = 0;
static int best_score = 0;
static bool can_undo = false;

// 2048 內部狀態機
enum Game2048State { STATE_START_SCREEN, STATE_PLAYING, STATE_GAMEOVER };
static Game2048State game_state_2048 = STATE_START_SCREEN;

// ==========================================
//           貪食蛇 (Snake) 系統變數與規格
// ==========================================
#define SNAKE_GRID_W 16   // 地圖寬度 (格)
#define SNAKE_GRID_H 14   // 地圖高度 (格)
#define SNAKE_MAX_LEN 64  // 蛇身最大長度

enum SnakeDir { DIR_UP, DIR_DOWN, DIR_LEFT, DIR_RIGHT };

enum SnakeState { SNAKE_STATE_START, SNAKE_STATE_PLAYING, SNAKE_STATE_PAUSE, SNAKE_STATE_GAMEOVER };

struct GridPoint {
    int8_t x;
    int8_t y;
};

static GridPoint snake_body[SNAKE_MAX_LEN];
static int snake_len = 3;
static SnakeDir snake_dir = DIR_RIGHT;
static SnakeDir next_snake_dir = DIR_RIGHT;  // 緩衝下一個轉向，防止瞬間反向自殺
static GridPoint food_pos = {8, 7};
static int snake_score = 0;
static int snake_best_score = 0;
static SnakeState snake_game_state = SNAKE_STATE_START;

// UI 相關物件 (第二階段會用到)
static lv_obj_t* bg_snake = nullptr;
static lv_obj_t* snake_score_label = nullptr;
static lv_obj_t* snake_overlay = nullptr;
static lv_obj_t* snake_overlay_label = nullptr;
static lv_obj_t* food_obj = nullptr;
static lv_obj_t* snake_body_objs[SNAKE_MAX_LEN];

// --- NVS 讀寫函數 ---
static void load_best_score() {
    nvs_handle_t nvs_h;
    if (nvs_open("storage", NVS_READONLY, &nvs_h) == ESP_OK) {
        int32_t val = 0;
        if (nvs_get_i32(nvs_h, "2048_best", &val) == ESP_OK) {
            best_score = val;
        }
        nvs_close(nvs_h);
    }
}

static void save_best_score(int score) {
    nvs_handle_t nvs_h;
    if (nvs_open("storage", NVS_READWRITE, &nvs_h) == ESP_OK) {
        nvs_set_i32(nvs_h, "2048_best", score);
        nvs_commit(nvs_h);
        nvs_close(nvs_h);
    }
}

static void load_snake_best_score() {
    nvs_handle_t nvs_h;
    if (nvs_open("storage", NVS_READONLY, &nvs_h) == ESP_OK) {
        int32_t val = 0;
        if (nvs_get_i32(nvs_h, "snake_best", &val) == ESP_OK) {
            snake_best_score = val;
        }
        nvs_close(nvs_h);
    }
}

static void save_snake_best_score(int score) {
    nvs_handle_t nvs_h;
    if (nvs_open("storage", NVS_READWRITE, &nvs_h) == ESP_OK) {
        nvs_set_i32(nvs_h, "snake_best", score);
        nvs_commit(nvs_h);
        nvs_close(nvs_h);
    }
}

static void load_space_best_score() {
    nvs_handle_t nvs_h;
    if (nvs_open("storage", NVS_READONLY, &nvs_h) == ESP_OK) {
        int32_t val = 0;
        if (nvs_get_i32(nvs_h, "space_best", &val) == ESP_OK) {
            space_best_score = val;
        }
        nvs_close(nvs_h);
    }
}

static void save_space_best_score(int score) {
    nvs_handle_t nvs_h;
    if (nvs_open("storage", NVS_READWRITE, &nvs_h) == ESP_OK) {
        nvs_set_i32(nvs_h, "space_best", score);
        nvs_commit(nvs_h);
        nvs_close(nvs_h);
    }
}

// 檢查過三關輸贏：回傳 1(玩家贏), 2(電腦贏), 3(平手), 0(繼續)
static int ttt_check_winner() {
    // 檢查橫排與直排
    for (int i = 0; i < 3; i++) {
        if (ttt_board[i][0] != 0 && ttt_board[i][0] == ttt_board[i][1] &&
            ttt_board[i][1] == ttt_board[i][2])
            return ttt_board[i][0];
        if (ttt_board[0][i] != 0 && ttt_board[0][i] == ttt_board[1][i] &&
            ttt_board[1][i] == ttt_board[2][i])
            return ttt_board[0][i];
    }
    // 檢查對角線
    if (ttt_board[0][0] != 0 && ttt_board[0][0] == ttt_board[1][1] &&
        ttt_board[1][1] == ttt_board[2][2])
        return ttt_board[0][0];
    if (ttt_board[0][2] != 0 && ttt_board[0][2] == ttt_board[1][1] &&
        ttt_board[1][1] == ttt_board[2][0])
        return ttt_board[0][2];

    // 檢查是否平手 (盤面全滿)
    bool full = true;
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            if (ttt_board[r][c] == 0)
                full = false;
        }
    }
    return full ? 3 : 0;
}

// 輔助函數：電腦 AI 尋找是否有必勝或必須防守的格子
static bool ttt_find_winning_move(int player, int& out_r, int& out_c) {
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            if (ttt_board[r][c] == 0) {
                ttt_board[r][c] = player;            // 假裝下一子
                if (ttt_check_winner() == player) {  // 發現這步會贏！
                    ttt_board[r][c] = 0;             // 復原
                    out_r = r;
                    out_c = c;
                    return true;
                }
                ttt_board[r][c] = 0;  // 復原
            }
        }
    }
    return false;
}

// 本地端電腦 AI 下棋邏輯
static void ttt_ai_make_move() {
    int r = -1, c = -1;

    // 1. 電腦能贏就直接贏
    if (ttt_find_winning_move(2, r, c)) {
    }
    // 2. 玩家快贏了，立刻防守堵住他
    else if (ttt_find_winning_move(1, r, c)) {
    }
    // 3. 佔領最有利的中間
    else if (ttt_board[1][1] == 0) {
        r = 1;
        c = 1;
    }
    // 4. 隨機下在空位
    else {
        int empty_count = 0;
        int empty_cells[9][2];
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 3; j++) {
                if (ttt_board[i][j] == 0) {
                    empty_cells[empty_count][0] = i;
                    empty_cells[empty_count][1] = j;
                    empty_count++;
                }
            }
        }
        if (empty_count > 0) {
            int idx = esp_random() % empty_count;
            r = empty_cells[idx][0];
            c = empty_cells[idx][1];
        }
    }

    if (r != -1 && c != -1) {
        ttt_board[r][c] = 2;   // 電腦下 X
        ttt_user_turn = true;  // 換玩家
    }
}

// 隨機生成食物 (確保不會生在蛇的身體裡面)
static void snake_spawn_food() {
    srand(esp_random());  // 用 ESP32 硬體雜訊洗牌隨機種子

    bool on_snake;
    do {
        on_snake = false;
        food_pos.x = esp_random() % SNAKE_GRID_W;
        food_pos.y = esp_random() % SNAKE_GRID_H;
        for (int i = 0; i < snake_len; i++) {
            if (snake_body[i].x == food_pos.x && snake_body[i].y == food_pos.y) {
                on_snake = true;
                break;
            }
        }
    } while (on_snake);
}

// 初始化/重置遊戲
static void snake_init_game() {
    snake_len = 3;
    snake_dir = DIR_RIGHT;
    next_snake_dir = DIR_RIGHT;
    snake_score = 0;

    // 初始位置設定在中間偏左
    snake_body[0] = {4, 7};  // 蛇頭
    snake_body[1] = {3, 7};
    snake_body[2] = {2, 7};  // 蛇尾

    snake_spawn_food();
}

// 改變前進方向 (防自殺邏輯：往右走時不能直接轉左)
static void snake_set_direction(SnakeDir new_dir) {
    if (snake_game_state != SNAKE_STATE_PLAYING)
        return;
    if (new_dir == DIR_UP && snake_dir != DIR_DOWN)
        next_snake_dir = DIR_UP;
    else if (new_dir == DIR_DOWN && snake_dir != DIR_UP)
        next_snake_dir = DIR_DOWN;
    else if (new_dir == DIR_LEFT && snake_dir != DIR_RIGHT)
        next_snake_dir = DIR_LEFT;
    else if (new_dir == DIR_RIGHT && snake_dir != DIR_LEFT)
        next_snake_dir = DIR_RIGHT;
}

// 蛇往前走一步的核心迴圈 (由定時器定期呼叫)
// 回傳 true 代表遊戲繼續，回傳 false 代表撞牆或自撞 (Game Over)
static bool snake_tick_step() {
    if (snake_game_state != SNAKE_STATE_PLAYING)
        return true;

    snake_dir = next_snake_dir;  // 套用緩衝的方向

    // 計算新蛇頭的座標
    GridPoint new_head = snake_body[0];
    if (snake_dir == DIR_UP)
        new_head.y--;
    else if (snake_dir == DIR_DOWN)
        new_head.y++;
    else if (snake_dir == DIR_LEFT)
        new_head.x--;
    else if (snake_dir == DIR_RIGHT)
        new_head.x++;

    // 1. 撞牆判定
    if (new_head.x < 0 || new_head.x >= SNAKE_GRID_W || new_head.y < 0 ||
        new_head.y >= SNAKE_GRID_H) {
        return false;  // 撞牆 Game Over
    }

    // 2. 自撞判定 (檢查是否咬到自己的身體)
    for (int i = 0; i < snake_len - 1; i++) {
        if (snake_body[i].x == new_head.x && snake_body[i].y == new_head.y) {
            return false;  // 咬到自己 Game Over
        }
    }

    // 3. 吃食物判定
    bool ate_food = (new_head.x == food_pos.x && new_head.y == food_pos.y);

    // 身體向前挪動一格
    int move_len = ate_food ? snake_len : (snake_len - 1);
    if (ate_food && snake_len < SNAKE_MAX_LEN) {
        snake_len++;
    }

    for (int i = move_len; i > 0; i--) {
        snake_body[i] = snake_body[i - 1];
    }
    snake_body[0] = new_head;  // 更新蛇頭

    if (ate_food) {
        snake_score += 10;  // 吃一顆 +10 分
        if (snake_score > snake_best_score) {
            snake_best_score = snake_score;
            save_snake_best_score(snake_best_score);
        }
        snake_spawn_food();
    }

    return true;
}

// 刷新貪食蛇的所有畫面 (蛇身、食物、分數)
static void update_snake_ui() {
    if (!bg_snake)
        return;

    // 1. 更新計分列
    if (snake_score_label) {
        lv_label_set_text_fmt(snake_score_label, "S:%d  B:%d", snake_score, snake_best_score);
    }

    // 2. 更新紅色食物位置 (每個格子 8x8，Y 軸避開頂部 16px 狀態列)
    if (food_obj) {
        lv_obj_set_pos(food_obj, food_pos.x * 8, 16 + food_pos.y * 8);
    }

    // 3. 更新蛇身每一節的位置與顏色
    for (int i = 0; i < SNAKE_MAX_LEN; i++) {
        if (i < snake_len) {
            lv_obj_set_pos(snake_body_objs[i], snake_body[i].x * 8, 16 + snake_body[i].y * 8);

            // 蛇頭用亮綠色，蛇身用深綠色
            if (i == 0) {
                lv_obj_set_style_bg_color(snake_body_objs[i], lv_color_hex(0x00FF66),
                                          0);  // 亮綠蛇頭
            } else {
                lv_obj_set_style_bg_color(snake_body_objs[i], lv_color_hex(0x009933),
                                          0);  // 深綠身體
            }
            lv_obj_remove_flag(snake_body_objs[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            // 超過長度的部分隱藏
            lv_obj_add_flag(snake_body_objs[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// 隨機產生 2 或 4 的方塊
static void add_random_2048() {
    srand(esp_random());  // 用 ESP32 硬體雜訊洗牌隨機種子

    int empty[9][2];
    int count = 0;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            if (board_2048[i][j] == 0) {
                empty[count][0] = i;
                empty[count][1] = j;
                count++;
            }
        }
    }
    if (count > 0) {
        int idx = esp_random() % count;
        board_2048[empty[idx][0]][empty[idx][1]] = (esp_random() % 10 == 0) ? 4 : 2;
    }
}

// 檢查是否還有合法移動 (判斷 Game Over)
static bool check_2048_game_over() {
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            if (board_2048[i][j] == 0)
                return false;
            if (i < 2 && board_2048[i][j] == board_2048[i + 1][j])
                return false;
            if (j < 2 && board_2048[i][j] == board_2048[i][j + 1])
                return false;
        }
    }
    return true;  // 盤面全滿且無法再合併
}

static void update_2048_ui() {
    // 1. 更新頂部分數列
    if (score_label) {
        lv_label_set_text_fmt(score_label, "S:%d  B:%d", current_score, best_score);
    }

    // 2. 更新方塊顏色與數字
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            int val = board_2048[i][j];
            if (val == 0) {
                lv_label_set_text(labels_2048[i][j], "");
                lv_obj_set_style_bg_color(tiles_2048[i][j], lv_color_hex(0x333333), 0);
            } else {
                lv_label_set_text_fmt(labels_2048[i][j], "%d", val);
                uint32_t color = 0xEEEEEE;
                if (val == 2)
                    // color = 0xEEE4DA;
                    color = 0xFAF6ED;
                else if (val == 4)
                    color = 0xF0DDBB;
                else if (val == 8)
                    color = 0xF2B179;
                else if (val == 16)
                    color = 0xF59563;
                else if (val == 32)
                    color = 0xF67C5F;
                else if (val >= 64)
                    color = 0xF65E3B;

                lv_obj_set_style_bg_color(tiles_2048[i][j], lv_color_hex(color), 0);
                lv_obj_set_style_text_color(
                    labels_2048[i][j], val <= 4 ? lv_color_hex(0x776E65) : lv_color_hex(0xFFFFFF),
                    0);
            }
        }
    }

    // 3. 檢查 Game Over
    if (check_2048_game_over()) {
        game_state_2048 = STATE_GAMEOVER;
        if (overlay_2048 && overlay_label) {
            lv_label_set_text_fmt(overlay_label, "GAME\nOVER\nScore:\n%d", current_score);
            lv_obj_remove_flag(overlay_2048, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// 悔棋 (Undo) 功能
static void undo_2048() {
    if (!can_undo || game_state_2048 != STATE_PLAYING)
        return;
    memcpy(board_2048, board_backup, sizeof(board_2048));
    current_score = score_backup;
    can_undo = false;
    update_2048_ui();
    ESP_LOGI("2048", "★ 悔棋成功！回退到上一步");
}

#define BACKUP_BEFORE_MOVE()                            \
    int prev_board[3][3];                               \
    memcpy(prev_board, board_2048, sizeof(board_2048)); \
    int prev_score = current_score;

#define FINISH_MOVE(moved)                                      \
    if (moved) {                                                \
        memcpy(board_backup, prev_board, sizeof(board_backup)); \
        score_backup = prev_score;                              \
        can_undo = true;                                        \
        if (current_score > best_score) {                       \
            best_score = current_score;                         \
            save_best_score(best_score);                        \
        }                                                       \
        add_random_2048();                                      \
        update_2048_ui();                                       \
    }

static void move_2048_left() {
    if (game_state_2048 != STATE_PLAYING)
        return;
    BACKUP_BEFORE_MOVE();
    bool moved = false;
    for (int i = 0; i < 3; i++) {
        int write_idx = 0;
        for (int j = 0; j < 3; j++) {
            if (board_2048[i][j] != 0) {
                if (write_idx != j) {
                    board_2048[i][write_idx] = board_2048[i][j];
                    board_2048[i][j] = 0;
                    moved = true;
                }
                write_idx++;
            }
        }
        for (int j = 0; j < 2; j++) {  // 改為 2
            if (board_2048[i][j] != 0 && board_2048[i][j] == board_2048[i][j + 1]) {
                board_2048[i][j] *= 2;
                current_score += board_2048[i][j];
                board_2048[i][j + 1] = 0;
                moved = true;
                for (int k = j + 1; k < 2; k++)
                    board_2048[i][k] = board_2048[i][k + 1];
                board_2048[i][2] = 0;  // 最右格補零 (索引2)
            }
        }
    }
    FINISH_MOVE(moved);
}

// 3x3 向右滑
static void move_2048_right() {
    if (game_state_2048 != STATE_PLAYING)
        return;
    BACKUP_BEFORE_MOVE();
    bool moved = false;
    for (int i = 0; i < 3; i++) {
        int write_idx = 2;  // 起始為 2
        for (int j = 2; j >= 0; j--) {
            if (board_2048[i][j] != 0) {
                if (write_idx != j) {
                    board_2048[i][write_idx] = board_2048[i][j];
                    board_2048[i][j] = 0;
                    moved = true;
                }
                write_idx--;
            }
        }
        for (int j = 2; j > 0; j--) {
            if (board_2048[i][j] != 0 && board_2048[i][j] == board_2048[i][j - 1]) {
                board_2048[i][j] *= 2;
                current_score += board_2048[i][j];
                board_2048[i][j - 1] = 0;
                moved = true;
                for (int k = j - 1; k > 0; k--)
                    board_2048[i][k] = board_2048[i][k - 1];
                board_2048[i][0] = 0;
            }
        }
    }
    FINISH_MOVE(moved);
}

// 3x3 向上滑
static void move_2048_up() {
    if (game_state_2048 != STATE_PLAYING)
        return;
    BACKUP_BEFORE_MOVE();
    bool moved = false;
    for (int j = 0; j < 3; j++) {
        int write_idx = 0;
        for (int i = 0; i < 3; i++) {
            if (board_2048[i][j] != 0) {
                if (write_idx != i) {
                    board_2048[write_idx][j] = board_2048[i][j];
                    board_2048[i][j] = 0;
                    moved = true;
                }
                write_idx++;
            }
        }
        for (int i = 0; i < 2; i++) {
            if (board_2048[i][j] != 0 && board_2048[i][j] == board_2048[i + 1][j]) {
                board_2048[i][j] *= 2;
                current_score += board_2048[i][j];
                board_2048[i + 1][j] = 0;
                moved = true;
                for (int k = i + 1; k < 2; k++)
                    board_2048[k][j] = board_2048[k + 1][j];
                board_2048[2][j] = 0;  // 最底格補零 (索引2)
            }
        }
    }
    FINISH_MOVE(moved);
}

// 3x3 向下滑
static void move_2048_down() {
    if (game_state_2048 != STATE_PLAYING)
        return;
    BACKUP_BEFORE_MOVE();
    bool moved = false;
    for (int j = 0; j < 3; j++) {
        int write_idx = 2;  // 起始為 2
        for (int i = 2; i >= 0; i--) {
            if (board_2048[i][j] != 0) {
                if (write_idx != i) {
                    board_2048[write_idx][j] = board_2048[i][j];
                    board_2048[i][j] = 0;
                    moved = true;
                }
                write_idx--;
            }
        }
        for (int i = 2; i > 0; i--) {
            if (board_2048[i][j] != 0 && board_2048[i][j] == board_2048[i - 1][j]) {
                board_2048[i][j] *= 2;
                current_score += board_2048[i][j];
                board_2048[i - 1][j] = 0;
                moved = true;
                for (int k = i - 1; k > 0; k--)
                    board_2048[k][j] = board_2048[k - 1][j];
                board_2048[0][j] = 0;
            }
        }
    }
    FINISH_MOVE(moved);
}

// --- 眨眼動畫 ---
static void start_blink_animation() {
    if (!avatar_left_eye || !avatar_right_eye)
        return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_height);
    lv_anim_set_values(&a, EYE_BASE_H, 2);
    lv_anim_set_duration(&a, 100);
    lv_anim_set_playback_time(&a, 100);
    lv_anim_set_repeat_delay(&a, 3000);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_var(&a, avatar_left_eye);
    lv_anim_start(&a);
    lv_anim_set_var(&a, avatar_right_eye);
    lv_anim_start(&a);
}

static void anim_eye_snore_cb(void* var, int32_t v) {
    if (!avatar_left_eye || !avatar_right_eye)
        return;

    // --- 1. 計算高度：從 EYE_BASE_H (睜眼) 縮減到 2 (閉合) ---
    int32_t cur_h = EYE_BASE_H - (EYE_BASE_H - 6) * v / 100;
    lv_obj_set_height(avatar_left_eye, cur_h);
    lv_obj_set_height(avatar_right_eye, cur_h);

    // --- 2. 計算 Y 軸位移：微微向上提 (向上為負數) ---
    int32_t base_y = EYE_POS_H;  // 這是眼睛平常的 Y 軸基準位置 (請依你的設定微調)
    int32_t move_up_max = 10;    // 最大上移量 (4px 在 128x128 螢幕上最自然)
    int32_t cur_y = base_y - (move_up_max * v / 100);

    // --- 3. 重新對齊左右眼 (保持 X 軸間距，更新 Y 軸高度) ---
    // 假設左眼 X 是 -24，右眼 X 是 +24 (請依你的眼睛間距調整)
    lv_obj_align(avatar_left_eye, LV_ALIGN_CENTER, EYE_POS_L, cur_y);
    lv_obj_align(avatar_right_eye, LV_ALIGN_CENTER, EYE_POS_R, cur_y);
}

static void start_eye_snore_animation() {
    if (!avatar_left_eye || !avatar_right_eye)
        return;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_exec_cb(&a, anim_eye_snore_cb);  // 綁定剛剛的回呼函式
    lv_anim_set_values(&a, 0, 100);              // 0% -> 100%

    // 與嘴巴的打呼節奏完全同步
    lv_anim_set_duration(&a, 600);            // 100ms: 快速閉眼 + 向上提
    lv_anim_set_playback_duration(&a, 2400);  // 1200ms: 隨吐氣緩緩下沉復位
    lv_anim_set_repeat_delay(&a, 12000);      // 10秒後進行下一次打呼
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);

    lv_anim_set_var(&a, avatar_left_eye);  // var 填任一物件即可
    lv_anim_start(&a);
}

static void anim_mouth_snore_cb(void* var, int32_t v) {
    lv_obj_t* obj = (lv_obj_t*)var;

    // ==========================================
    // 1. 定義【合上口】時的尺寸 (v = 0 時的狀態)
    // ==========================================
    int32_t close_w = MOUTH_BASE_W;  // <-- 你設定的合上寬度 38！
    int32_t close_h = 8;             // 合上時的高度 (例如扁扁的一條線 6px)

    // ==========================================
    // 2. 定義【張開打呼】時的尺寸 (v = 100 時的狀態)
    // ==========================================
    // 打呼張開通常會縮成圓形 'O' 嘴，或者擴張：
    int32_t open_w = 16;  // 張嘴時的寬度 (如果是 'O' 形嘴可設小一點如 22~24；若大嘴可設更大)
    int32_t open_h = 32;  // 張嘴時的高度 (25px)

    // ==========================================
    // 3. 計算當前尺寸 (從 close 平滑過渡到 open)
    // ==========================================
    int32_t cur_w = close_w + (open_w - close_w) * v / 100;
    int32_t cur_h = close_h + (open_h - close_h) * v / 100;

    int32_t base_y = MOUTH_POS_H;  // 這是口部平常的 Y 軸基準位置 (請依你的設定微調)
    int32_t move_down_max = 4;     // 最大上移量 (4px 在 128x128 螢幕上最自然)
    int32_t cur_y = base_y - (move_down_max * v / 100);

    lv_obj_set_size(obj, cur_w, cur_h);
    lv_obj_align(obj, LV_ALIGN_CENTER, 0, cur_y);  // 保持居中
}

static void start_mouth_animation() {
    if (!avatar_mouth)
        return;

    // 先清除可能正在運行的嘴巴動畫
    lv_anim_del(avatar_mouth, NULL);

    // 【關鍵】立刻重設為合口尺寸 (寬 38，高 8)，避免切換表情時尺寸不對
    lv_obj_set_size(avatar_mouth, MOUTH_BASE_W, MOUTH_BASE_H);
    lv_obj_align(avatar_mouth, LV_ALIGN_CENTER, 0, MOUTH_POS_H);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_exec_cb(&a, anim_mouth_snore_cb);
    lv_anim_set_values(&a, 0, 100);           // 0 (合口38) -> 100 (張口)
    lv_anim_set_duration(&a, 600);            // 200ms 快速張嘴吸氣
    lv_anim_set_playback_duration(&a, 2400);  // 1200ms 慢慢吐氣合嘴，回到寬度 38
    lv_anim_set_repeat_delay(&a, 12000);      // 維持合嘴狀態 10 秒
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_var(&a, avatar_mouth);
    lv_anim_start(&a);
}

void CustomApp_OnChatMessage(const char* role, const char* content) {
    // 全域靜音攔截器 (讓 AI 在遊戲中徹底閉嘴)
    if (strcmp(role, "assistant") == 0) {
        // 如果處於遊戲選單 (1) 或 遊戲中 (2)，強制截斷 AI 的語音輸出
        if (current_system_mode == 1 || current_system_mode == 2) {
            Application::GetInstance().Schedule(
                []() { Application::GetInstance().AbortSpeaking(kAbortReasonNone); });
            return;  // 直接返回，完全不處理 AI 的文字
        }
    }

    if (strcmp(role, "user") == 0 && content != nullptr) {
        std::string text(content);

        // 🌟 新增：塔羅模式防休眠攔截
        if (text.find("抽牌") != std::string::npos || text.find("占卜") != std::string::npos ||
            text.find("易經") != std::string::npos || text.find("塔羅") != std::string::npos) {
            if (text.find("停止") != std::string::npos || text.find("結束") != std::string::npos ||
                text.find("退出") != std::string::npos) {
                is_tarot_mode = false;
                ESP_LOGI("VoiceCmd", "退出塔羅模式，恢復 3 分鐘待機");
            } else {
                is_tarot_mode = true;
                ESP_LOGI("VoiceCmd", "進入塔羅模式，延長待機至 15 分鐘");
            }
        }

        // 1. 擲杯指令
        if (text.find("杯") != std::string::npos || text.find("筊") != std::string::npos) {
            ESP_LOGI("VoiceCmd", "語音指令：立即跳轉至 擲杯");
            current_system_mode = 5;
            current_ui_mode = 5;
            app_menu_selection = APP_BWA;  // 選擇 擲杯
            ui_mode_changed = true;
            Application::GetInstance().Schedule([]() {
                auto& app = Application::GetInstance();
                app.AbortSpeaking(kAbortReasonNone);
                app.StopListening();
                app.SetDeviceState(kDeviceStateIdle);
            });
        }
        // 2. 六合彩指令
        else if (text.find("六合彩") != std::string::npos ||
                 text.find("六合") != std::string::npos) {
            ESP_LOGI("VoiceCmd", "語音指令：立即跳轉至 六合彩");
            current_system_mode = 5;
            current_ui_mode = 5;
            app_menu_selection = APP_MARK6;  // 選擇 六合彩
            ui_mode_changed = true;
            Application::GetInstance().Schedule([]() {
                auto& app = Application::GetInstance();
                app.AbortSpeaking(kAbortReasonNone);
                app.StopListening();
                app.SetDeviceState(kDeviceStateIdle);
            });
        }
        // 3. 太空侵略者指令
        else if (text.find("太空") != std::string::npos || text.find("飛機") != std::string::npos ||
                 text.find("飞机") != std::string::npos) {
            ESP_LOGI("VoiceCmd", "語音指令：立即跳轉至 太空侵略者");
            current_system_mode = 2;
            current_ui_mode = 2;
            game_menu_selection = GAME_SPACE;  // 選擇 Space
            ui_mode_changed = true;
            Application::GetInstance().Schedule([]() {
                auto& app = Application::GetInstance();
                app.AbortSpeaking(kAbortReasonNone);
                app.StopListening();
                app.SetDeviceState(kDeviceStateIdle);
            });
        }
        // 老虎機
        else if (text.find("老虎機") != std::string::npos ||
                 text.find("slot") != std::string::npos) {
            ESP_LOGI("VoiceCmd", "語音指令：立即跳轉至 老虎機");
            current_system_mode = 2;
            current_ui_mode = 2;
            game_menu_selection = GAME_SLOT;  // 選擇 Breakout
            ui_mode_changed = true;
            Application::GetInstance().Schedule([]() {
                auto& app = Application::GetInstance();
                app.AbortSpeaking(kAbortReasonNone);
                app.StopListening();
                app.SetDeviceState(kDeviceStateIdle);
            });
        }
        // 4. 打磚塊指令
        else if (text.find("磚塊") != std::string::npos || text.find("砖块") != std::string::npos ||
                 text.find("breakout") != std::string::npos) {
            ESP_LOGI("VoiceCmd", "語音指令：立即跳轉至 打磚塊");
            current_system_mode = 2;
            current_ui_mode = 2;
            game_menu_selection = GAME_BREAKOUT;  // 選擇 Breakout
            ui_mode_changed = true;
            Application::GetInstance().Schedule([]() {
                auto& app = Application::GetInstance();
                app.AbortSpeaking(kAbortReasonNone);
                app.StopListening();
                app.SetDeviceState(kDeviceStateIdle);
            });
        }
        // 5. 2048 指令 (同時支援阿拉伯數字「2048」與中文「二零四八」)
        else if (text.find("2048") != std::string::npos ||
                 text.find("二零四八") != std::string::npos) {
            ESP_LOGI("VoiceCmd", "語音指令：立即跳轉至 2048");
            current_system_mode = 2;  // 進入遊戲模式
            current_ui_mode = 2;
            game_menu_selection = GAME_2048;  // 選擇 2048
            ui_mode_changed = true;           // 立刻重繪畫面！
            Application::GetInstance().Schedule([]() {
                auto& app = Application::GetInstance();
                app.AbortSpeaking(kAbortReasonNone);
                app.StopListening();
                app.SetDeviceState(kDeviceStateIdle);
            });
        }
        // 6. 貪食蛇指令
        else if (text.find("蛇") != std::string::npos || text.find("snake") != std::string::npos ||
                 text.find("Snake") != std::string::npos) {
            ESP_LOGI("VoiceCmd", "語音指令：立即跳轉至 貪食蛇");
            current_system_mode = 2;
            current_ui_mode = 2;
            game_menu_selection = GAME_SNAKE;  // 選擇 貪食蛇
            ui_mode_changed = true;
            Application::GetInstance().Schedule([]() {
                auto& app = Application::GetInstance();
                app.AbortSpeaking(kAbortReasonNone);
                app.StopListening();
                app.SetDeviceState(kDeviceStateIdle);
            });
        }
        // ========================================================
        // 6.5 語音進入過三關指令
        // ========================================================
        else if (text.find("過三關") != std::string::npos ||
                 text.find("過參關") != std::string::npos ||
                 text.find("個參關") != std::string::npos ||
                 text.find("個三關") != std::string::npos ||
                 text.find("打井") != std::string::npos) {
            ESP_LOGI("VoiceCmd", "語音指令：立即跳轉至 過三關");
            current_system_mode = 2;
            current_ui_mode = 2;
            game_menu_selection = GAME_TICTACTOE;
            ui_mode_changed = true;

            // 安全打斷，不破壞狀態機
            Application::GetInstance().Schedule(
                []() { Application::GetInstance().AbortSpeaking(kAbortReasonNone); });
        }
        // 6. 時間模式指令
        else if (text.find("時間模式") != std::string::npos ||
                 text.find("时间模式") != std::string::npos ||
                 text.find("時鐘模式") != std::string::npos ||
                 text.find("睇時間") != std::string::npos ||
                 text.find("几点") != std::string::npos) {
            ESP_LOGI("VoiceCmd", "語音指令：跳轉至 橫向時間模式");
            current_system_mode = 3;
            current_ui_mode = 3;
            auto backlight = Board::GetInstance().GetBacklight();
            if (backlight)
                backlight->SetBrightness(100);
            ui_mode_changed = true;
        }
        // 7. 天氣模式指令
        else if (text.find("天氣模式") != std::string::npos ||
                 text.find("天气模式") != std::string::npos ||
                 text.find("睇天氣") != std::string::npos) {
            ESP_LOGI("VoiceCmd", "語音指令：跳轉至 橫向天氣模式");
            current_system_mode = 6;  // 新增模式 6 給天氣
            current_ui_mode = 6;
            auto backlight = Board::GetInstance().GetBacklight();
            if (backlight)
                backlight->SetBrightness(100);
            ui_mode_changed = true;

            // 立即喚醒背景去抓天氣
            int64_t now_us = esp_timer_get_time();
            if (weather_task_handle &&
                (last_weather_fetch_us == 0 || now_us - last_weather_fetch_us > 10000000LL)) {
                last_weather_fetch_us = now_us;
                xTaskNotifyGive(weather_task_handle);
            }
            Application::GetInstance().Schedule([]() {
                auto& app = Application::GetInstance();
                app.AbortSpeaking(kAbortReasonNone);
                app.StopListening();
                app.SetDeviceState(kDeviceStateIdle);
            });
        }
        // 6. 判斷巴士查詢指令 (喚醒背景工人去抓資料)
        else if (text.find("巴士") != std::string::npos || text.find("號") != std::string::npos ||
                 text.find("号") != std::string::npos || text.find("線") != std::string::npos) {
            ESP_LOGI("VoiceCmd", "語音指令：偵測到查詢巴士，準備顯示面板！");
            bus_show_countdown = 0;
            if (bg_bus)
                lv_obj_add_flag(bg_bus, LV_OBJ_FLAG_HIDDEN);

            if (bus_task_handle) {
                xTaskNotifyGive(bus_task_handle);  // 拍醒工人
            }
        }
        // 7. 計時器指令
        else if (text.find("倒數") != std::string::npos) {
            ESP_LOGI("VoiceCmd", "語音指令：啟動計時器");
            int total_sec = parse_timer_seconds(text);
            timer_total_us = (int64_t)total_sec * 1000000LL;  // 轉換為微秒
            timer_remain_us = timer_total_us;
            timer_state = TIMER_IDLE;

            current_system_mode = 5;
            current_ui_mode = 5;
            app_menu_selection = APP_TIMER;
            ui_mode_changed = true;

            Application::GetInstance().Schedule([]() {
                auto& app = Application::GetInstance();
                app.AbortSpeaking(kAbortReasonNone);
                app.StopListening();
                app.SetDeviceState(kDeviceStateIdle);
            });
        }
        // 8. 碼表指令
        else if (text.find("計時") != std::string::npos) {
            ESP_LOGI("VoiceCmd", "語音指令：啟動碼表");
            sw_state = SW_IDLE;
            sw_accumulated_us = 0;

            current_system_mode = 5;
            current_ui_mode = 5;
            app_menu_selection = APP_STOPWATCH;
            ui_mode_changed = true;

            Application::GetInstance().Schedule([]() {
                auto& app = Application::GetInstance();
                app.AbortSpeaking(kAbortReasonNone);
                app.StopListening();
                app.SetDeviceState(kDeviceStateIdle);
            });
        }
        // 水平儀指令
        else if (text.find("水平儀") != std::string::npos ||
                 text.find("水平仪") != std::string::npos ||
                 text.find("水平移") != std::string::npos ||
                 text.find("量水平") != std::string::npos ||
                 text.find("梁水平") != std::string::npos ||
                 text.find("涼水平") != std::string::npos ||
                 text.find("度水平") != std::string::npos) {
            ESP_LOGI("VoiceCmd", "語音指令：啟動水平儀");

            current_system_mode = 5;  // 進入 App 執行模式
            current_ui_mode = 5;
            app_menu_selection = APP_LEVEL;  // 指定啟動 水平儀
            ui_mode_changed = true;

            // 立即掐斷語音並暫停 AI
            Application::GetInstance().Schedule([]() {
                auto& app = Application::GetInstance();
                app.AbortSpeaking(kAbortReasonNone);
                app.StopListening();
                app.SetDeviceState(kDeviceStateIdle);
            });
        }
        // ========================================================
        // 過三關遊戲中：攔截座標語音 (無縫下棋)
        // ========================================================
        if (current_ui_mode == 2 && game_menu_selection == GAME_TICTACTOE &&
            (ttt_user_turn || ttt_game_state == TTT_STATE_START ||
             ttt_game_state == TTT_STATE_GAMEOVER)) {
            int move_r = -1, move_c = -1;

            // 判斷玩家說的方位 (增加容錯關鍵字)
            if (text.find("左上") != std::string::npos || text.find("咗上") != std::string::npos) {
                move_r = 0;
                move_c = 0;
            } else if (text.find("中上") != std::string::npos ||
                       text.find("鍾上") != std::string::npos ||
                       text.find("綜上") != std::string::npos ||
                       text.find("中償") != std::string::npos ||
                       text.find("鍾償") != std::string::npos ||
                       text.find("綜償") != std::string::npos ||
                       text.find("上中") != std::string::npos ||
                       text.find("正上") != std::string::npos ||
                       text.find("正償") != std::string::npos) {
                move_r = 0;
                move_c = 1;
            } else if (text.find("右上") != std::string::npos ||
                       text.find("由上") != std::string::npos ||
                       text.find("又上") != std::string::npos ||
                       text.find("右償") != std::string::npos ||
                       text.find("由償") != std::string::npos ||
                       text.find("又償") != std::string::npos) {
                move_r = 0;
                move_c = 2;
            } else if (text.find("左中") != std::string::npos ||
                       text.find("中左") != std::string::npos ||
                       text.find("正左") != std::string::npos ||
                       text.find("咗中") != std::string::npos ||
                       text.find("左鍾") != std::string::npos ||
                       text.find("咗鍾") != std::string::npos ||
                       text.find("鍾左") != std::string::npos ||
                       text.find("鍾咗") != std::string::npos) {
                move_r = 1;
                move_c = 0;
            } else if (text.find("中間") != std::string::npos ||
                       text.find("鍾間") != std::string::npos ||
                       text.find("綜間") != std::string::npos ||
                       text.find("正中") != std::string::npos ||
                       text.find("正鍾") != std::string::npos ||
                       text.find("正綜") != std::string::npos) {
                move_r = 1;
                move_c = 1;
            } else if (text.find("右中") != std::string::npos ||
                       text.find("右鐘") != std::string::npos ||
                       text.find("右綜") != std::string::npos ||
                       text.find("又中") != std::string::npos ||
                       text.find("又綜") != std::string::npos ||
                       text.find("又鍾") != std::string::npos ||
                       text.find("由中") != std::string::npos ||
                       text.find("由綜") != std::string::npos ||
                       text.find("由鍾") != std::string::npos ||
                       text.find("中右") != std::string::npos ||
                       text.find("鐘右") != std::string::npos ||
                       text.find("綜右") != std::string::npos) {
                move_r = 1;
                move_c = 2;
            } else if (text.find("左下") != std::string::npos ||
                       text.find("咗下") != std::string::npos) {
                move_r = 2;
                move_c = 0;
            } else if (text.find("中下") != std::string::npos ||
                       text.find("綜下") != std::string::npos ||
                       text.find("鍾下") != std::string::npos ||
                       text.find("中吓") != std::string::npos ||
                       text.find("綜吓") != std::string::npos ||
                       text.find("鍾吓") != std::string::npos) {
                move_r = 2;
                move_c = 1;
            } else if (text.find("右下") != std::string::npos ||
                       text.find("由下") != std::string::npos ||
                       text.find("又下") != std::string::npos ||
                       text.find("右吓") != std::string::npos ||
                       text.find("由吓") != std::string::npos ||
                       text.find("又吓") != std::string::npos) {
                move_r = 2;
                move_c = 2;
            }

            // 如果有辨識到方位
            if (move_r != -1 && move_c != -1) {
                ESP_LOGI("TicTacToe", "收到語音方位: [%d][%d]", move_r, move_c);

                // 【核心修復】：絕對不要在這裡呼叫 LVGL，只把座標交給 UI 執行緒去處理
                pending_ttt_r = move_r;
                pending_ttt_c = move_c;

                Application::GetInstance().Schedule(
                    []() { Application::GetInstance().AbortSpeaking(kAbortReasonNone); });
            }
        }
    }
}

// 啟動指定列的滾動動畫
static void slot_start_spinning(int col) {
    if (!slot_reel[col])
        return;
    lv_anim_del(slot_reel[col], nullptr);  // 先清除舊動畫

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, slot_reel[col]);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);

    // 8個圖案 * 36px寬 = 288 (無縫銜接)
    lv_anim_set_values(&a, 0, -288);
    lv_anim_set_time(&a, 400 + col * 80);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

// 停止指定列，並產生清脆的「卡榫回彈」視覺效果
static void slot_stop_reel(int col) {
    if (!slot_reel[col])
        return;
    lv_anim_del(slot_reel[col], nullptr);

    int result = esp_random() % 8;
    slot_results[col] = result;
    int target_y = -(result * 36);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, slot_reel[col]);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_values(&a, target_y - 30, target_y);
    lv_anim_set_time(&a, 300);
    lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
    lv_anim_start(&a);
}

// --- 定時器：控制畫面刷新與狀態 ---
static void custom_app_timer_cb(lv_timer_t* timer) {
    // ==============================================
    // 【最高優先級】如果正在顯示巴士資訊，蓋住一切畫面 8 秒鐘！
    // ==============================================
    if (bus_show_countdown > 0) {
        lv_label_set_text(bus_route_label, bus_route_str);
        lv_label_set_text(bus_t1_label, bus_t1_str);
        lv_label_set_text(bus_t2_label, bus_t2_str);
        lv_obj_remove_flag(bg_bus, LV_OBJ_FLAG_HIDDEN);

        bus_show_countdown--;
        if (bus_show_countdown == 0) {
            // 8 秒結束，隱藏面板，露出底下的大笑臉！
            lv_obj_add_flag(bg_bus, LV_OBJ_FLAG_HIDDEN);
        }
        return;  // 顯示期間停止更新其他畫面
    }

    if (!avatar_left_eye || !avatar_right_eye || !avatar_mouth)
        return;

    // 處理模式切換的 UI 重繪
    if (ui_mode_changed) {
        ui_mode_changed = false;

        // 1. 保留臉部物件單純隱藏 (因為對話切換頻繁，保留在記憶體)
        if (avatar_left_eye)
            lv_obj_add_flag(avatar_left_eye, LV_OBJ_FLAG_HIDDEN);
        if (avatar_right_eye)
            lv_obj_add_flag(avatar_right_eye, LV_OBJ_FLAG_HIDDEN);
        if (avatar_mouth)
            lv_obj_add_flag(avatar_mouth, LV_OBJ_FLAG_HIDDEN);
        if (avatar_heart)
            lv_obj_add_flag(avatar_heart, LV_OBJ_FLAG_HIDDEN);
        if (avatar_kissy)
            lv_obj_add_flag(avatar_kissy, LV_OBJ_FLAG_HIDDEN);
        if (avatar_sweat)
            lv_obj_add_flag(avatar_sweat, LV_OBJ_FLAG_HIDDEN);
        if (avatar_anger)
            lv_obj_add_flag(avatar_anger, LV_OBJ_FLAG_HIDDEN);
        if (avatar_embrassing)
            lv_obj_add_flag(avatar_embrassing, LV_OBJ_FLAG_HIDDEN);
        if (avatar_sad_eye)
            lv_obj_add_flag(avatar_sad_eye, LV_OBJ_FLAG_HIDDEN);
        if (avatar_surprised_eye)
            lv_obj_add_flag(avatar_surprised_eye, LV_OBJ_FLAG_HIDDEN);
        if (avatar_angry_eye)
            lv_obj_add_flag(avatar_angry_eye, LV_OBJ_FLAG_HIDDEN);
        if (avatar_mouth_laugh)
            lv_obj_add_flag(avatar_mouth_laugh, LV_OBJ_FLAG_HIDDEN);
        if (avatar_silly_tongue)
            lv_obj_add_flag(avatar_silly_tongue, LV_OBJ_FLAG_HIDDEN);
        if (avatar_mouth_happy)
            lv_obj_add_flag(avatar_mouth_happy, LV_OBJ_FLAG_HIDDEN);
        if (avatar_cool)
            lv_obj_add_flag(avatar_cool, LV_OBJ_FLAG_HIDDEN);
        if (avatar_tears)
            lv_obj_add_flag(avatar_tears, LV_OBJ_FLAG_HIDDEN);
        if (avatar_swallow)
            lv_obj_add_flag(avatar_swallow, LV_OBJ_FLAG_HIDDEN);
        if (avatar_confuse)
            lv_obj_add_flag(avatar_confuse, LV_OBJ_FLAG_HIDDEN);
        if (avatar_sleepy_z)
            lv_obj_add_flag(avatar_sleepy_z, LV_OBJ_FLAG_HIDDEN);

        // 選單畫面隱藏即可，佔用極小
        if (game_menu_bg)
            lv_obj_add_flag(game_menu_bg, LV_OBJ_FLAG_HIDDEN);

        // =========================================================
        // 2. 🚀 記憶體深度清理機制：銷毀非當前遊戲/應用的 UI 以防 Hang機
        // =========================================================
        if (current_ui_mode != 2) {
            if (bg_space) {
                lv_obj_del(bg_space);
                bg_space = nullptr;
                space_ship = nullptr;
                space_score_label = nullptr;
                space_overlay = nullptr;
                space_overlay_label = nullptr;
            }
            if (bg_slot) {
                lv_obj_del(bg_slot);
                bg_slot = nullptr;
                slot_score_label = nullptr;
                slot_status_label = nullptr;
                for (int i = 0; i < 3; i++) {
                    slot_col[i] = nullptr;
                    slot_reel[i] = nullptr;
                    slot_img1[i] = nullptr;
                    slot_img2[i] = nullptr;
                }
                slot_game_state = SLOT_STATE_IDLE;
            }
            if (bg_breakout) {
                lv_obj_del(bg_breakout);
                bg_breakout = nullptr;
                bk_paddle = nullptr;
                bk_ball = nullptr;
                bk_score_label = nullptr;
                bk_overlay = nullptr;
                bk_overlay_label = nullptr;
            }
            if (bg_2048) {
                lv_obj_del(bg_2048);
                bg_2048 = nullptr;
                score_label = nullptr;
                overlay_2048 = nullptr;
                overlay_label = nullptr;
            }
            if (bg_snake) {
                lv_obj_del(bg_snake);
                bg_snake = nullptr;
                snake_score_label = nullptr;
                food_obj = nullptr;
                snake_overlay = nullptr;
                snake_overlay_label = nullptr;
            }
            if (bg_tictactoe) {
                lv_obj_del(bg_tictactoe);
                bg_tictactoe = nullptr;
                ttt_overlay = nullptr;
                ttt_overlay_label = nullptr;
                ttt_toast_bg = nullptr;
                ttt_toast_label = nullptr;
            }
        } else {
            if (game_menu_selection != GAME_SPACE && bg_space) {
                lv_obj_del(bg_space);
                bg_space = nullptr;
                space_ship = nullptr;
                space_score_label = nullptr;
                space_overlay = nullptr;
                space_overlay_label = nullptr;
            }
            if (game_menu_selection != GAME_SLOT && bg_slot) {
                lv_obj_del(bg_slot);
                bg_slot = nullptr;
                slot_score_label = nullptr;
                slot_status_label = nullptr;
                for (int i = 0; i < 3; i++) {
                    slot_col[i] = nullptr;
                    slot_reel[i] = nullptr;
                    slot_img1[i] = nullptr;
                    slot_img2[i] = nullptr;
                }
                slot_game_state = SLOT_STATE_IDLE;
            }
            if (game_menu_selection != GAME_BREAKOUT && bg_breakout) {
                lv_obj_del(bg_breakout);
                bg_breakout = nullptr;
                bk_paddle = nullptr;
                bk_ball = nullptr;
                bk_score_label = nullptr;
                bk_overlay = nullptr;
                bk_overlay_label = nullptr;
            }
            if (game_menu_selection != GAME_2048 && bg_2048) {
                lv_obj_del(bg_2048);
                bg_2048 = nullptr;
                score_label = nullptr;
                overlay_2048 = nullptr;
                overlay_label = nullptr;
            }
            if (game_menu_selection != GAME_SNAKE && bg_snake) {
                lv_obj_del(bg_snake);
                bg_snake = nullptr;
                snake_score_label = nullptr;
                food_obj = nullptr;
                snake_overlay = nullptr;
                snake_overlay_label = nullptr;
            }
            if (game_menu_selection != GAME_TICTACTOE && bg_tictactoe) {
                lv_obj_del(bg_tictactoe);
                bg_tictactoe = nullptr;
                ttt_overlay = nullptr;
                ttt_overlay_label = nullptr;
                ttt_toast_bg = nullptr;
                ttt_toast_label = nullptr;
            }
        }

        if (current_ui_mode != 5) {
            if (bg_bwa) {
                lv_obj_del(bg_bwa);
                bg_bwa = nullptr;
                bwa_1_bg = nullptr;
                bwa_2_bg = nullptr;
                bwa_result_label = nullptr;
                bwa_desc_label = nullptr;
            }
            if (bg_marksix) {
                lv_obj_del(bg_marksix);
                bg_marksix = nullptr;
                marksix_tip_label = nullptr;
                for (int i = 0; i < 6; i++) {
                    marksix_balls[i] = nullptr;
                    marksix_labels[i] = nullptr;
                }
            }
            if (bg_timer) {
                lv_obj_del(bg_timer);
                bg_timer = nullptr;
                timer_time_label = nullptr;
                timer_ms_label = nullptr;
                timer_status_label = nullptr;
            }
            if (bg_stopwatch) {
                lv_obj_del(bg_stopwatch);
                bg_stopwatch = nullptr;
                sw_time_label = nullptr;
                sw_ms_label = nullptr;
                sw_status_label = nullptr;
            }
            if (bg_level) {
                lv_obj_del(bg_level);
                bg_level = nullptr;
                level_target_circle = nullptr;
                inside_circle = nullptr;
                level_bubble = nullptr;
                level_info_label = nullptr;
            }
        } else {
            if (app_menu_selection != APP_BWA && bg_bwa) {
                lv_obj_del(bg_bwa);
                bg_bwa = nullptr;
                bwa_1_bg = nullptr;
                bwa_2_bg = nullptr;
                bwa_result_label = nullptr;
                bwa_desc_label = nullptr;
            }
            if (app_menu_selection != APP_MARK6 && bg_marksix) {
                lv_obj_del(bg_marksix);
                bg_marksix = nullptr;
                marksix_tip_label = nullptr;
                for (int i = 0; i < 6; i++) {
                    marksix_balls[i] = nullptr;
                    marksix_labels[i] = nullptr;
                }
            }
            if (app_menu_selection != APP_TIMER && bg_timer) {
                lv_obj_del(bg_timer);
                bg_timer = nullptr;
                timer_time_label = nullptr;
                timer_ms_label = nullptr;
                timer_status_label = nullptr;
            }
            if (app_menu_selection != APP_STOPWATCH && bg_stopwatch) {
                lv_obj_del(bg_stopwatch);
                bg_stopwatch = nullptr;
                sw_time_label = nullptr;
                sw_ms_label = nullptr;
                sw_status_label = nullptr;
            }
            if (app_menu_selection != APP_LEVEL && bg_level) {
                lv_obj_del(bg_level);
                bg_level = nullptr;
                level_target_circle = nullptr;
                inside_circle = nullptr;
                level_bubble = nullptr;
                level_info_label = nullptr;
            }
        }

        if (current_ui_mode != 3 && bg_time_mode) {
            lv_obj_del(bg_time_mode);
            bg_time_mode = nullptr;
            time_mode_time_label = nullptr;
            time_mode_date_label = nullptr;
        }
        if (current_ui_mode != 6 && bg_weather_mode) {
            lv_obj_del(bg_weather_mode);
            bg_weather_mode = nullptr;
            weather_icon_container = nullptr;
            weather_icon_label = nullptr;
            weather_current_label = nullptr;
            weather_hilo_label = nullptr;
        }

        // 防呆：如果退出老虎機，務必清除背景無限運作的滾動動畫，避免浪費 CPU！
        if (current_ui_mode != 2 || game_menu_selection != GAME_SLOT) {
            for (int i = 0; i < 3; i++) {
                // 【修正】：動畫現在是綁在 slot_reel 上，所以要刪除 slot_reel 的動畫
                if (slot_reel[i]) {
                    lv_anim_del(slot_reel[i], nullptr);
                }
            }
            slot_game_state = SLOT_STATE_IDLE;
        }
        // 2. 根據現在的模式，打開對應的選單
        if (current_ui_mode == 1 || current_ui_mode == 4) {  // 1 game mode, 4 app mode
            if (!game_menu_bg) {
                game_menu_bg = lv_obj_create(lv_scr_act());
                lv_obj_set_size(game_menu_bg, LCD_W, LCD_H);
                lv_obj_set_style_bg_color(game_menu_bg, lv_color_black(), 0);
                lv_obj_align(game_menu_bg, LV_ALIGN_CENTER, 0, 0);
                lv_obj_set_style_border_width(game_menu_bg, 0, 0);
                lv_obj_set_style_radius(game_menu_bg, 0, 0);
                lv_obj_set_style_pad_all(game_menu_bg, 0, 0);

                menu_title_label = lv_label_create(game_menu_bg);
                lv_obj_set_style_text_color(menu_title_label, lv_color_make(255, 230, 0), 0);
                lv_obj_align(menu_title_label, LV_ALIGN_TOP_MID, 0, 5);

                game_menu_label = lv_label_create(game_menu_bg);
                lv_obj_align(game_menu_label, LV_ALIGN_CENTER, 0, 0);
            }
            lv_obj_remove_flag(game_menu_bg, LV_OBJ_FLAG_HIDDEN);

            // 【核心分離邏輯】：未來新增 App 或 Game，只要在這邊的陣列加字即可！
            if (current_ui_mode == 4) {
                lv_obj_set_style_bg_color(game_menu_bg, lv_color_hex(0x000ec7), 0);
                lv_label_set_text(menu_title_label, "APP MODE");
                const char* app_texts[] = {"Moon\nBlock", "Mark 6", "Spirit\nLevel", "Stop\nWatch",
                                           "Timer"};
                lv_label_set_text(game_menu_label, app_texts[app_menu_selection]);
            } else {
                lv_obj_set_style_bg_color(game_menu_bg, lv_color_hex(0x57007b), 0);
                lv_label_set_text(menu_title_label, "GAME MODE");
                const char* game_texts[] = {"Space\nInvaders", "Lucky 7", "Tic Tac\nToe",
                                            "Breakout",        "2048",    "Snake"};
                lv_label_set_text(game_menu_label, game_texts[game_menu_selection]);
            }

            lv_obj_set_style_text_font(game_menu_label, &lv_font_montserrat_28, 0);
            lv_obj_set_style_text_line_space(game_menu_label, -4, 0);
            lv_obj_set_style_text_align(game_menu_label, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_color(game_menu_label, lv_color_white(), 0);

            return;
        } else if (current_ui_mode == 5) {  // --- 進入 APP 模式 ---
            if (app_menu_selection == APP_BWA) {
                // ==================== 擲杯 啟動邏輯 ====================
                if (!bg_bwa) {
                    bg_bwa = lv_obj_create(lv_scr_act());
                    lv_obj_set_size(bg_bwa, LCD_W, LCD_H);
                    lv_obj_set_style_bg_color(bg_bwa, lv_color_black(), 0);
                    lv_obj_set_style_border_width(bg_bwa, 0, 0);
                    lv_obj_set_style_radius(bg_bwa, 0, 0);
                    lv_obj_set_style_pad_all(bg_bwa, 0, 0);

                    // 1. 左邊的筊杯 (左邊半圓形)
                    bwa_1_bg = lv_obj_create(bg_bwa);
                    lv_obj_set_size(bwa_1_bg, 60, 60);
                    lv_obj_align(bwa_1_bg, LV_ALIGN_CENTER, -36, -28);
                    lv_obj_set_style_radius(bwa_1_bg, 28, 0);  // 大圓角變橢圓
                    lv_obj_set_style_border_width(bwa_1_bg, 0, 0);
                    lv_obj_clear_flag(bwa_1_bg, LV_OBJ_FLAG_SCROLLABLE);

                    // 用黑色方塊切掉右半邊，形成完美的 ( 形狀
                    lv_obj_t* cut_1 = lv_obj_create(bwa_1_bg);
                    lv_obj_set_size(cut_1, 40, 60);
                    lv_obj_align(cut_1, LV_ALIGN_RIGHT_MID, -12, 0);
                    lv_obj_set_style_bg_color(cut_1, lv_color_black(), 0);
                    lv_obj_set_style_border_width(cut_1, 0, 0);
                    lv_obj_set_style_radius(cut_1, 0, 0);
                    lv_obj_clear_flag(cut_1, LV_OBJ_FLAG_SCROLLABLE);

                    // 2. 右邊的筊杯 (右邊半圓形)
                    bwa_2_bg = lv_obj_create(bg_bwa);
                    lv_obj_set_size(bwa_2_bg, 60, 60);
                    lv_obj_align(bwa_2_bg, LV_ALIGN_CENTER, 36, -28);
                    lv_obj_set_style_radius(bwa_2_bg, 28, 0);
                    lv_obj_set_style_border_width(bwa_2_bg, 0, 0);
                    lv_obj_clear_flag(bwa_2_bg, LV_OBJ_FLAG_SCROLLABLE);

                    // 用黑色方塊切掉左半邊，形成完美的 ) 形狀
                    lv_obj_t* cut_2 = lv_obj_create(bwa_2_bg);
                    lv_obj_set_size(cut_2, 40, 60);
                    lv_obj_align(cut_2, LV_ALIGN_LEFT_MID, 12, 0);
                    lv_obj_set_style_bg_color(cut_2, lv_color_black(), 0);
                    lv_obj_set_style_border_width(cut_2, 0, 0);
                    lv_obj_set_style_radius(cut_2, 0, 0);
                    lv_obj_clear_flag(cut_2, LV_OBJ_FLAG_SCROLLABLE);

                    // 3. 結果文字標籤
                    bwa_result_label = lv_label_create(bg_bwa);
                    // 若有大字體可開啟，無則用默認
                    // lv_obj_set_style_text_font(bwa_result_label, &lv_font_montserrat_28, 0);
                    lv_obj_align(bwa_result_label, LV_ALIGN_BOTTOM_MID, 0, -30);

                    bwa_desc_label = lv_label_create(bg_bwa);
                    lv_obj_set_style_text_color(bwa_desc_label, lv_color_hex(0xAAAAAA), 0);
                    lv_obj_align(bwa_desc_label, LV_ALIGN_BOTTOM_MID, 0, -10);
                }
                lv_obj_remove_flag(bg_bwa, LV_OBJ_FLAG_HIDDEN);

                // 每次進入先給一個隨機預設值
                // set_bwa_face(esp_random() % 2, esp_random() % 2, false);
                set_bwa_face(1, 1, false);
                // if (!imu_initialized)
                //     imu_init();
                return;

            } else if (app_menu_selection == APP_MARK6) {
                // ==================== 六合彩 啟動邏輯 ====================
                if (!bg_marksix) {
                    bg_marksix = lv_obj_create(lv_scr_act());
                    lv_obj_set_size(bg_marksix, LCD_W, LCD_H);
                    lv_obj_set_style_bg_color(bg_marksix, lv_color_black(), 0);  // 純黑底色
                    lv_obj_set_style_border_width(bg_marksix, 0, 0);
                    lv_obj_set_style_radius(bg_marksix, 0, 0);
                    lv_obj_set_style_pad_all(bg_marksix, 0, 0);

                    // 1. 金黃色標題
                    lv_obj_t* title = lv_label_create(bg_marksix);
                    lv_label_set_text(title, "★ MARK 6 ★");
                    lv_obj_set_style_text_color(title, lv_color_make(255, 200, 0), 0);
                    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

                    // 2. 建立 6 顆號碼球 (上排 3 顆，下排 3 顆)
                    const int ball_x[6] = {23, 64, 105, 23, 64, 105};
                    const int ball_y[6] = {42, 42, 42, 80, 80, 80};

                    for (int i = 0; i < 6; i++) {
                        marksix_balls[i] = lv_obj_create(bg_marksix);
                        lv_obj_set_size(marksix_balls[i], 30, 30);  // 直徑 30 像素
                        lv_obj_set_style_radius(marksix_balls[i], LV_RADIUS_CIRCLE, 0);  // 正圓
                        lv_obj_set_style_bg_opa(marksix_balls[i], LV_OPA_COVER, 0);
                        lv_obj_clear_flag(marksix_balls[i], LV_OBJ_FLAG_SCROLLABLE);
                        lv_obj_align(marksix_balls[i], LV_ALIGN_CENTER, ball_x[i] - 64,
                                     ball_y[i] - 64);

                        marksix_labels[i] = lv_label_create(marksix_balls[i]);
                        lv_obj_align(marksix_labels[i], LV_ALIGN_CENTER, 0, 2);
                    }

                    // 3. 底部進度狀態列
                    marksix_tip_label = lv_label_create(bg_marksix);
                    lv_obj_set_style_text_color(marksix_tip_label, lv_color_hex(0xAAAAAA), 0);
                    lv_obj_align(marksix_tip_label, LV_ALIGN_BOTTOM_MID, 0, -4);
                }
                lv_obj_remove_flag(bg_marksix, LV_OBJ_FLAG_HIDDEN);

                // 每次進入遊戲，重置盤面為 6 個空白待抽球
                reset_mark_six_ui();
                // if (!imu_initialized)
                //     imu_init();
                return;

            } else if (app_menu_selection == APP_TIMER) {
                // ==================== 計時器 UI 啟動邏輯 ====================
                if (!bg_timer) {
                    bg_timer = lv_obj_create(lv_scr_act());
                    lv_obj_set_size(bg_timer, LCD_W, LCD_H);
                    lv_obj_set_style_bg_color(bg_timer, lv_color_black(), 0);
                    lv_obj_set_style_border_width(bg_timer, 0, 0);
                    lv_obj_set_style_radius(bg_timer, 0, 0);
                    lv_obj_set_style_pad_all(bg_timer, 0, 0);

                    lv_obj_t* title = lv_label_create(bg_timer);
                    lv_label_set_text(title, "TIMER");
                    lv_obj_set_style_text_color(title, lv_color_make(0, 255, 255), 0);
                    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 5);

                    // 上行：分與秒
                    timer_time_label = lv_label_create(bg_timer);
                    lv_obj_set_style_text_font(timer_time_label, &lv_font_montserrat_28, 0);
                    lv_obj_set_style_text_color(timer_time_label, lv_color_white(), 0);
                    lv_obj_align(timer_time_label, LV_ALIGN_CENTER, 0, -15);  // 稍微往上移

                    // 下行：毫秒 (較小字體或同字體放下面)
                    timer_ms_label = lv_label_create(bg_timer);
                    lv_obj_set_style_text_font(timer_ms_label, &lv_font_montserrat_28, 0);
                    lv_obj_set_style_text_color(timer_ms_label, lv_color_hex(0xAAAAAA),
                                                0);                        // 淺灰色凸顯層次
                    lv_obj_align(timer_ms_label, LV_ALIGN_CENTER, 0, 16);  // 放在秒數下方

                    // 狀態提示
                    timer_status_label = lv_label_create(bg_timer);
                    lv_obj_align(timer_status_label, LV_ALIGN_BOTTOM_MID, 0, -4);
                }
                lv_obj_remove_flag(bg_timer, LV_OBJ_FLAG_HIDDEN);

                // UI 更新將由核心引擎即時處理，這裡只做初始化狀態
                if (timer_state == TIMER_IDLE) {
                    lv_label_set_text(timer_status_label, "Press to Start");
                    lv_obj_set_style_text_color(timer_status_label, lv_color_hex(0xAAAAAA), 0);
                }
                return;
            } else if (app_menu_selection == APP_STOPWATCH) {
                // ==================== 碼表 UI 啟動邏輯 ====================
                if (!bg_stopwatch) {
                    bg_stopwatch = lv_obj_create(lv_scr_act());
                    lv_obj_set_size(bg_stopwatch, LCD_W, LCD_H);
                    lv_obj_set_style_bg_color(bg_stopwatch, lv_color_black(), 0);
                    lv_obj_set_style_border_width(bg_stopwatch, 0, 0);
                    lv_obj_set_style_radius(bg_stopwatch, 0, 0);
                    lv_obj_set_style_pad_all(bg_stopwatch, 0, 0);

                    lv_obj_t* title = lv_label_create(bg_stopwatch);
                    lv_label_set_text(title, "STOPWATCH");
                    lv_obj_set_style_text_color(title, lv_color_make(255, 100, 100),
                                                0);  // 橘紅色標題
                    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 5);

                    sw_time_label = lv_label_create(bg_stopwatch);
                    lv_obj_set_style_text_font(sw_time_label, &lv_font_montserrat_28, 0);
                    lv_obj_set_style_text_color(sw_time_label, lv_color_white(), 0);
                    lv_obj_align(sw_time_label, LV_ALIGN_CENTER, 0, -15);

                    sw_ms_label = lv_label_create(bg_stopwatch);
                    lv_obj_set_style_text_font(sw_ms_label, &lv_font_montserrat_28, 0);
                    lv_obj_set_style_text_color(sw_ms_label, lv_color_hex(0xAAAAAA), 0);
                    lv_obj_align(sw_ms_label, LV_ALIGN_CENTER, 0, 16);

                    sw_status_label = lv_label_create(bg_stopwatch);
                    lv_obj_align(sw_status_label, LV_ALIGN_BOTTOM_MID, 0, -4);
                }
                lv_obj_remove_flag(bg_stopwatch, LV_OBJ_FLAG_HIDDEN);
                sw_state = SW_IDLE;
                sw_accumulated_us = 0;

                if (sw_state == SW_IDLE) {
                    lv_label_set_text(sw_status_label, "Press to Start");
                    lv_obj_set_style_text_color(sw_status_label, lv_color_hex(0xAAAAAA), 0);
                }
                return;
            } else if (app_menu_selection == APP_LEVEL) {
                // ==================== 水平儀 UI 啟動邏輯 ====================
                if (!bg_level) {
                    bg_level = lv_obj_create(lv_scr_act());
                    lv_obj_set_size(bg_level, LCD_W, LCD_H);
                    lv_obj_set_style_bg_color(bg_level, lv_color_black(), 0);
                    lv_obj_set_style_border_width(bg_level, 0, 0);
                    lv_obj_set_style_radius(bg_level, 0, 0);
                    lv_obj_set_style_pad_all(bg_level, 0, 0);

                    // 標題
                    lv_obj_t* title = lv_label_create(bg_level);
                    lv_label_set_text(title, "LEVEL");
                    lv_obj_set_style_text_color(title, lv_color_make(0, 255, 0), 0);
                    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 5);

                    // 外圈圓形準星
                    level_target_circle = lv_obj_create(bg_level);
                    lv_obj_set_size(level_target_circle, 70, 70);
                    lv_obj_set_style_radius(level_target_circle, LV_RADIUS_CIRCLE, 0);
                    lv_obj_set_style_bg_opa(level_target_circle, LV_OPA_TRANSP, 0);
                    lv_obj_set_style_border_color(level_target_circle, lv_color_white(), 0);
                    lv_obj_set_style_border_width(level_target_circle, 2, 0);
                    lv_obj_align(level_target_circle, LV_ALIGN_CENTER, 0, 0);
                    lv_obj_clear_flag(level_target_circle, LV_OBJ_FLAG_SCROLLABLE);

                    // 中間圓形準星
                    inside_circle = lv_obj_create(bg_level);
                    lv_obj_set_size(inside_circle, 35, 35);
                    lv_obj_set_style_radius(inside_circle, LV_RADIUS_CIRCLE, 0);
                    lv_obj_set_style_bg_opa(inside_circle, LV_OPA_TRANSP, 0);
                    lv_obj_set_style_border_color(inside_circle, lv_color_hex(0x555555), 0);
                    lv_obj_set_style_border_width(inside_circle, 2, 0);
                    lv_obj_align(inside_circle, LV_ALIGN_CENTER, 0, 0);
                    lv_obj_clear_flag(inside_circle, LV_OBJ_FLAG_SCROLLABLE);

                    // 十字準心線
                    lv_obj_t* h_line = lv_obj_create(level_target_circle);
                    lv_obj_set_size(h_line, 70, 2);
                    lv_obj_set_style_bg_color(h_line, lv_color_hex(0x555555), 0);
                    lv_obj_set_style_border_width(h_line, 0, 0);
                    lv_obj_align(h_line, LV_ALIGN_CENTER, 0, 0);

                    lv_obj_t* v_line = lv_obj_create(level_target_circle);
                    lv_obj_set_size(v_line, 2, 70);
                    lv_obj_set_style_bg_color(v_line, lv_color_hex(0x555555), 0);
                    lv_obj_set_style_border_width(v_line, 0, 0);
                    lv_obj_align(v_line, LV_ALIGN_CENTER, 0, 0);

                    // 移動的水平氣泡 (紅/黃/綠)
                    level_bubble = lv_obj_create(bg_level);
                    lv_obj_set_size(level_bubble, 20, 20);
                    lv_obj_set_style_radius(level_bubble, LV_RADIUS_CIRCLE, 0);
                    lv_obj_set_style_bg_color(level_bubble, lv_color_hex(0xFF0000),
                                              0);  // 預設紅色
                    lv_obj_set_style_border_width(level_bubble, 0, 0);
                    lv_obj_align(level_bubble, LV_ALIGN_CENTER, 0, 0);
                    lv_obj_clear_flag(level_bubble, LV_OBJ_FLAG_SCROLLABLE);

                    // 數據文字
                    level_info_label = lv_label_create(bg_level);
                    // lv_obj_set_style_text_font(level_info_label, &lv_font_montserrat_28, 0);
                    lv_obj_align(level_info_label, LV_ALIGN_BOTTOM_MID, 0, 0);
                }
                lv_obj_remove_flag(bg_level, LV_OBJ_FLAG_HIDDEN);
                // if (!imu_initialized)
                //     imu_init();
                return;
            }
        } else if (current_ui_mode == 2) {  // --- 進入遊戲 ---
            if (game_menu_selection == GAME_SPACE) {
                // ==================== 太空侵略者 啟動邏輯 ====================
                if (!bg_space) {
                    load_space_best_score();

                    bg_space = lv_obj_create(lv_scr_act());
                    lv_obj_set_size(bg_space, LCD_W, LCD_H);
                    lv_obj_set_style_bg_color(bg_space, lv_color_black(), 0);
                    lv_obj_set_style_border_width(bg_space, 0, 0);
                    lv_obj_set_style_radius(bg_space, 0, 0);
                    lv_obj_set_style_pad_all(bg_space, 0, 0);

                    // 1. 頂部計分板
                    space_score_label = lv_label_create(bg_space);
                    lv_obj_set_style_text_color(space_score_label, lv_color_white(), 0);
                    lv_obj_align(space_score_label, LV_ALIGN_TOP_MID, 0, 2);

                    // 2. 玩家飛船 (青色小方塊)
                    space_ship = lv_obj_create(bg_space);
                    lv_obj_set_size(space_ship, 16, 8);
                    lv_obj_set_style_bg_color(space_ship, lv_color_hex(0x00FFFF), 0);  // 青色
                    lv_obj_set_style_radius(space_ship, 2, 0);
                    lv_obj_set_style_border_width(space_ship, 0, 0);
                    lv_obj_clear_flag(space_ship, LV_OBJ_FLAG_SCROLLABLE);
                    space_ship_x = (LCD_W - 16) / 2;  // 56
                    lv_obj_align(space_ship, LV_ALIGN_TOP_LEFT, space_ship_x, LCD_H * 0.9);

                    // 3. 子彈物件池 (預設隱藏)
                    for (int i = 0; i < SPACE_MAX_BULLETS; i++) {
                        space_bullets[i].active = false;
                        space_bullets[i].obj = lv_obj_create(bg_space);
                        lv_obj_set_size(space_bullets[i].obj, 4, 8);  // 子彈大小
                        lv_obj_set_style_bg_color(space_bullets[i].obj, lv_color_hex(0xFFFF00),
                                                  0);  // 黃色
                        lv_obj_set_style_radius(space_bullets[i].obj, 2, 0);
                        lv_obj_set_style_border_width(space_bullets[i].obj, 0, 0);
                        lv_obj_clear_flag(space_bullets[i].obj, LV_OBJ_FLAG_SCROLLABLE);
                        lv_obj_add_flag(space_bullets[i].obj, LV_OBJ_FLAG_HIDDEN);
                    }

                    // 炸彈物件池 (紫色小圓點)
                    for (int i = 0; i < SPACE_MAX_BOMBS; i++) {
                        space_bombs[i].active = false;
                        space_bombs[i].obj = lv_obj_create(bg_space);
                        lv_obj_set_size(space_bombs[i].obj, 5, 5);
                        lv_obj_set_style_bg_color(space_bombs[i].obj, lv_color_hex(0xFF33FF), 0);
                        lv_obj_set_style_radius(space_bombs[i].obj, LV_RADIUS_CIRCLE, 0);
                        lv_obj_set_style_border_width(space_bombs[i].obj, 0, 0);
                        lv_obj_clear_flag(space_bombs[i].obj, LV_OBJ_FLAG_SCROLLABLE);
                        lv_obj_add_flag(space_bombs[i].obj, LV_OBJ_FLAG_HIDDEN);
                    }

                    // 4. 外星人物件池 (3x4 = 12隻)
                    uint32_t alien_colors[SPACE_MAX_ALIEN_ROWS] = {
                        0xFF3333,  // 第1排：鮮紅
                        0xFF7700,  // 第2排：橙橘
                        0xFFDD00,  // 第3排：亮黃
                        0x33FF33,  // 第4排：青綠
                        0x00FFFF,  // 第5排：青藍
                        0xFF33FF   // 第6排：紫紅
                    };
                    for (int r = 0; r < SPACE_MAX_ALIEN_ROWS; r++) {
                        for (int c = 0; c < SPACE_ALIEN_COLS; c++) {
                            space_aliens[r][c].active = true;
                            space_aliens[r][c].obj = lv_obj_create(bg_space);
                            lv_obj_set_size(space_aliens[r][c].obj, SPACE_ALIEN_W, SPACE_ALIEN_H);
                            lv_obj_set_style_bg_color(space_aliens[r][c].obj,
                                                      lv_color_hex(alien_colors[r]), 0);
                            lv_obj_set_style_radius(space_aliens[r][c].obj, 2, 0);
                            lv_obj_set_style_border_width(space_aliens[r][c].obj, 0, 0);
                            lv_obj_clear_flag(space_aliens[r][c].obj, LV_OBJ_FLAG_SCROLLABLE);
                            // 初始排列
                            lv_obj_align(space_aliens[r][c].obj, LV_ALIGN_TOP_LEFT, 15 + c * 25,
                                         SPACE_ALIEN_START_Y + r * 15);
                            lv_obj_add_flag(space_aliens[r][c].obj,
                                            LV_OBJ_FLAG_HIDDEN);  // 開局前隱藏
                        }
                    }

                    // 5. START / GAME OVER 遮罩
                    space_overlay = lv_obj_create(bg_space);
                    lv_obj_set_size(space_overlay, LCD_W, LCD_H);
                    lv_obj_set_style_bg_color(space_overlay, lv_color_black(), 0);
                    lv_obj_set_style_bg_opa(space_overlay, LV_OPA_80, 0);
                    lv_obj_align(space_overlay, LV_ALIGN_CENTER, 0, 0);

                    space_overlay_label = lv_label_create(space_overlay);
                    lv_obj_set_style_text_color(space_overlay_label, lv_color_make(255, 200, 0), 0);
                    lv_obj_set_style_text_font(space_overlay_label, &lv_font_montserrat_28, 0);
                    lv_obj_set_style_text_align(space_overlay_label, LV_TEXT_ALIGN_CENTER, 0);
                    lv_obj_align(space_overlay_label, LV_ALIGN_CENTER, 0, 0);
                }

                lv_obj_remove_flag(bg_space, LV_OBJ_FLAG_HIDDEN);
                space_game_state = SPACE_STATE_START;
                lv_label_set_text_fmt(space_overlay_label, "Best\nScore:\n%d", space_best_score);
                lv_label_set_text_fmt(space_score_label, "S:%d  B:%d", space_score,
                                      space_best_score);
                lv_obj_remove_flag(space_overlay, LV_OBJ_FLAG_HIDDEN);

            }
            // ===============================================
            // UI 建立區塊 (修復白底與無縫輪播)
            // ===============================================
            else if (game_menu_selection == GAME_SLOT) {
                if (!bg_slot) {
                    bg_slot = lv_obj_create(lv_scr_act());
                    lv_obj_set_size(bg_slot, LCD_W, LCD_H);
                    lv_obj_set_style_bg_color(bg_slot, lv_color_black(), 0);
                    lv_obj_set_style_border_width(bg_slot, 0, 0);
                    lv_obj_set_style_radius(bg_slot, 0, 0);
                    lv_obj_set_style_pad_all(bg_slot, 0, 0);

                    slot_score_label = lv_label_create(bg_slot);
                    lv_label_set_text_fmt(slot_score_label, "C: %d", slot_score);
                    lv_obj_set_style_text_color(slot_score_label, lv_color_make(255, 200, 0), 0);
                    lv_obj_set_style_text_font(slot_score_label, &lv_font_montserrat_24, 0);
                    lv_obj_align(slot_score_label, LV_ALIGN_TOP_MID, 0, 5);

                    lv_obj_t* frame = lv_obj_create(bg_slot);
                    lv_obj_set_size(frame, 10 + (SLOT_WIDTH * 3), SLOT_WIDTH + 4);
                    lv_obj_set_style_bg_color(frame, lv_color_make(220, 30, 30), 0);
                    lv_obj_set_style_border_width(frame, 0, 0);
                    lv_obj_set_style_pad_all(frame, 2, 0);
                    lv_obj_align(frame, LV_ALIGN_CENTER, 0, 0);

                    for (int i = 0; i < 3; i++) {
                        slot_col[i] = lv_obj_create(frame);
                        lv_obj_set_size(slot_col[i], SLOT_WIDTH, SLOT_WIDTH);
                        lv_obj_set_style_bg_color(slot_col[i], lv_color_white(), 0);
                        lv_obj_set_style_bg_opa(slot_col[i], LV_OPA_COVER, 0);
                        lv_obj_set_style_border_width(slot_col[i], 0, 0);
                        lv_obj_set_style_pad_all(slot_col[i], 0, 0);
                        lv_obj_clear_flag(slot_col[i], LV_OBJ_FLAG_SCROLLABLE);
                        lv_obj_set_style_clip_corner(slot_col[i], true, 0);
                        lv_obj_align(slot_col[i], LV_ALIGN_LEFT_MID, i * (SLOT_WIDTH + 2), 0);

                        // 無縫輪播容器
                        slot_reel[i] = lv_obj_create(slot_col[i]);
                        lv_obj_set_size(slot_reel[i], SLOT_WIDTH, 600);
                        lv_obj_set_style_bg_opa(slot_reel[i], LV_OPA_TRANSP, 0);
                        lv_obj_set_style_border_width(slot_reel[i], 0, 0);
                        lv_obj_set_style_pad_all(slot_reel[i], 0, 0);
                        lv_obj_clear_flag(slot_reel[i], LV_OBJ_FLAG_SCROLLABLE);

                        slot_img1[i] = lv_img_create(slot_reel[i]);
                        lv_img_set_src(slot_img1[i], &slot_36);
                        lv_obj_set_y(slot_img1[i], 0);

                        slot_img2[i] = lv_img_create(slot_reel[i]);
                        lv_img_set_src(slot_img2[i], &slot_36);
                        lv_obj_set_y(slot_img2[i], 288);  // 8格 * 36px
                    }

                    slot_status_label = lv_label_create(bg_slot);
                    lv_label_set_text(slot_status_label, "Press to Spin!");
                    lv_obj_set_style_text_color(slot_status_label, lv_color_white(), 0);
                    lv_obj_align(slot_status_label, LV_ALIGN_BOTTOM_MID, 0, -10);
                }
                lv_obj_remove_flag(bg_slot, LV_OBJ_FLAG_HIDDEN);
                // 進入遊戲時防呆
                if (slot_score <= 0 && slot_game_state != SLOT_STATE_GAMEOVER) {
                    slot_score = 100;
                }
                lv_label_set_text_fmt(slot_score_label, "C: %d", slot_score);
            } else if (game_menu_selection == GAME_BREAKOUT) {
                // ==================== 打磚塊 啟動邏輯 ====================
                if (!bg_breakout) {
                    load_bk_best_score();
                    bg_breakout = lv_obj_create(lv_scr_act());
                    lv_obj_set_size(bg_breakout, LCD_W, LCD_H);
                    lv_obj_set_style_bg_color(bg_breakout, lv_color_black(), 0);
                    lv_obj_set_style_border_width(bg_breakout, 0, 0);
                    lv_obj_set_style_radius(bg_breakout, 0, 0);
                    lv_obj_set_style_pad_all(bg_breakout, 0, 0);

                    // 計分板
                    bk_score_label = lv_label_create(bg_breakout);
                    lv_obj_set_style_text_color(bk_score_label, lv_color_white(), 0);
                    lv_obj_align(bk_score_label, LV_ALIGN_TOP_MID, 0, 2);

                    // 擋板 (藍色)
                    bk_paddle = lv_obj_create(bg_breakout);
                    lv_obj_set_size(bk_paddle, 28, 4);
                    lv_obj_set_style_bg_color(bk_paddle, lv_color_hex(0x00A8FF), 0);
                    lv_obj_set_style_radius(bk_paddle, 2, 0);
                    lv_obj_set_style_border_width(bk_paddle, 0, 0);
                    lv_obj_clear_flag(bk_paddle, LV_OBJ_FLAG_SCROLLABLE);

                    // 彈力球 (黃色)
                    bk_ball = lv_obj_create(bg_breakout);
                    lv_obj_set_size(bk_ball, 6, 6);
                    lv_obj_set_style_bg_color(bk_ball, lv_color_hex(0xFFDD00), 0);
                    lv_obj_set_style_radius(bk_ball, LV_RADIUS_CIRCLE, 0);
                    lv_obj_set_style_border_width(bk_ball, 0, 0);
                    lv_obj_clear_flag(bk_ball, LV_OBJ_FLAG_SCROLLABLE);

                    // 2. 建立 6 層磚塊 (顏色：紅、橘、黃、綠、藍、紫)
                    uint32_t row_colors[BK_MAX_ROWS] = {0xFF3333, 0xFF7700, 0xFFCC00,
                                                        0x33CC33, 0x00A8FF, 0x9933FF};
                    for (int r = 0; r < BK_MAX_ROWS; r++) {
                        for (int c = 0; c < BK_COLS; c++) {
                            bk_bricks[r][c] = lv_obj_create(bg_breakout);
                            lv_obj_set_size(bk_bricks[r][c], 23, 8);  // 寬 23，高 8
                            lv_obj_set_style_bg_color(bk_bricks[r][c], lv_color_hex(row_colors[r]),
                                                      0);
                            lv_obj_set_style_radius(bk_bricks[r][c], 1, 0);
                            lv_obj_set_style_border_width(bk_bricks[r][c], 0, 0);
                            lv_obj_clear_flag(bk_bricks[r][c], LV_OBJ_FLAG_SCROLLABLE);
                            // 排版計算：間距 2px，起點 X=2, Y=18
                            lv_obj_align(bk_bricks[r][c], LV_ALIGN_TOP_LEFT,
                                         BK_BRICK_START_X + c * BK_BRICK_W,
                                         BK_BRICK_START_Y + r * BK_BRICK_STEP_Y);
                        }
                    }

                    // START / GAMEOVER 遮罩
                    bk_overlay = lv_obj_create(bg_breakout);
                    lv_obj_set_size(bk_overlay, LCD_W, LCD_H);
                    lv_obj_set_style_bg_color(bk_overlay, lv_color_black(), 0);
                    lv_obj_set_style_bg_opa(bk_overlay, LV_OPA_80, 0);
                    lv_obj_align(bk_overlay, LV_ALIGN_CENTER, 0, 0);

                    bk_overlay_label = lv_label_create(bk_overlay);
                    lv_obj_set_style_text_color(bk_overlay_label, lv_color_make(255, 200, 0), 0);
                    lv_obj_set_style_text_font(bk_overlay_label, &lv_font_montserrat_28, 0);
                    lv_obj_set_style_text_align(bk_overlay_label, LV_TEXT_ALIGN_CENTER, 0);
                    lv_obj_align(bk_overlay_label, LV_ALIGN_CENTER, 0, 0);
                }

                lv_obj_remove_flag(bg_breakout, LV_OBJ_FLAG_HIDDEN);
                bk_game_state = BK_STATE_START;
                init_breakout_level(true);  // 初始化第一關
                lv_label_set_text_fmt(bk_overlay_label, "Best\nScore:\n%d", bk_best_score);
                lv_label_set_text_fmt(bk_score_label, "L:%d  S:%d", bk_level, bk_score);
                lv_obj_remove_flag(bk_overlay, LV_OBJ_FLAG_HIDDEN);

            } else if (game_menu_selection == GAME_2048) {
                // ==================== 2048 啟動邏輯 (保留原樣) ====================
                if (!bg_2048) {
                    load_best_score();
                    bg_2048 = lv_obj_create(lv_scr_act());
                    lv_obj_set_size(bg_2048, LCD_W, LCD_H);
                    lv_obj_set_style_bg_color(bg_2048, lv_color_hex(0xBBADA0), 0);
                    lv_obj_set_style_pad_all(bg_2048, 0, 0);
                    lv_obj_set_style_border_width(bg_2048, 0, 0);

                    // 1. 頂部狀態列 (高度 16px)
                    score_label = lv_label_create(bg_2048);
                    lv_obj_set_style_text_color(score_label, lv_color_white(), 0);
                    lv_obj_align(score_label, LV_ALIGN_TOP_MID, 0, 2);

                    // 2. 3x3 格子 (整體往下移 18px，讓出空間給狀態列)
                    for (int i = 0; i < 3; i++) {
                        for (int j = 0; j < 3; j++) {
                            tiles_2048[i][j] = lv_obj_create(bg_2048);
                            // 方塊放大到 34x34 像素！
                            lv_obj_set_size(tiles_2048[i][j], 34, 34);
                            // 完美置中公式 (邊距 4px，水平起始 8px，垂直起始 20px)
                            lv_obj_align(tiles_2048[i][j], LV_ALIGN_TOP_LEFT, j * 38 + 8,
                                         i * 35 + 20);
                            lv_obj_set_style_radius(tiles_2048[i][j], 4, 0);
                            lv_obj_set_style_border_width(tiles_2048[i][j], 0, 0);
                            lv_obj_clear_flag(tiles_2048[i][j], LV_OBJ_FLAG_SCROLLABLE);

                            labels_2048[i][j] = lv_label_create(tiles_2048[i][j]);
                            lv_obj_align(labels_2048[i][j], LV_ALIGN_CENTER, 0, 0);
                        }
                    }

                    // 3. 全螢幕遮罩層 (用來顯示 START 和 GAME OVER)
                    overlay_2048 = lv_obj_create(bg_2048);
                    lv_obj_set_size(overlay_2048, LCD_W, LCD_H);
                    lv_obj_set_style_bg_color(overlay_2048, lv_color_black(), 0);
                    lv_obj_set_style_bg_opa(overlay_2048, LV_OPA_80, 0);
                    lv_obj_align(overlay_2048, LV_ALIGN_CENTER, 0, 0);

                    overlay_label = lv_label_create(overlay_2048);
                    lv_obj_set_style_text_color(overlay_label, lv_color_make(255, 200, 0), 0);
                    lv_obj_set_style_text_font(overlay_label, &lv_font_montserrat_28, 0);
                    lv_obj_set_style_text_align(overlay_label, LV_TEXT_ALIGN_CENTER, 0);
                    lv_obj_align(overlay_label, LV_ALIGN_CENTER, 0, 0);
                }
                lv_obj_remove_flag(bg_2048, LV_OBJ_FLAG_HIDDEN);

                // 每次新切入 2048，先進入 START 畫面
                game_state_2048 = STATE_START_SCREEN;
                lv_label_set_text_fmt(overlay_label, "Best\nScore:\n%d", best_score);
                lv_obj_remove_flag(overlay_2048, LV_OBJ_FLAG_HIDDEN);

            } else if (game_menu_selection == GAME_SNAKE) {
                // ==================== 貪食蛇 啟動邏輯 ====================
                if (!bg_snake) {
                    load_snake_best_score();

                    bg_snake = lv_obj_create(lv_scr_act());
                    lv_obj_set_size(bg_snake, LCD_W, LCD_H);
                    lv_obj_set_style_bg_color(bg_snake, lv_color_black(), 0);  // 經典黑底
                    lv_obj_set_style_pad_all(bg_snake, 0, 0);
                    lv_obj_set_style_border_width(bg_snake, 0, 0);

                    // 1. 頂部狀態列
                    snake_score_label = lv_label_create(bg_snake);
                    lv_obj_set_style_text_color(snake_score_label, lv_color_hex(0x00FF66), 0);
                    lv_obj_align(snake_score_label, LV_ALIGN_TOP_MID, 0, 2);

                    // 2. 紅色食物 (圓形)
                    food_obj = lv_obj_create(bg_snake);
                    lv_obj_set_size(food_obj, 8, 8);
                    lv_obj_set_style_bg_color(food_obj, lv_color_hex(0xFF3333), 0);
                    lv_obj_set_style_radius(food_obj, LV_RADIUS_CIRCLE, 0);
                    lv_obj_set_style_border_width(food_obj, 0, 0);
                    lv_obj_clear_flag(food_obj, LV_OBJ_FLAG_SCROLLABLE);

                    // 3. 蛇身物件池 (64 節，每節 8x8)
                    for (int i = 0; i < SNAKE_MAX_LEN; i++) {
                        snake_body_objs[i] = lv_obj_create(bg_snake);
                        lv_obj_set_size(snake_body_objs[i], 8, 8);
                        lv_obj_set_style_radius(snake_body_objs[i], 2, 0);
                        lv_obj_set_style_border_width(snake_body_objs[i], 0, 0);
                        lv_obj_clear_flag(snake_body_objs[i], LV_OBJ_FLAG_SCROLLABLE);
                        lv_obj_add_flag(snake_body_objs[i], LV_OBJ_FLAG_HIDDEN);
                    }

                    // 4. 遮罩層 (START & GAME OVER)
                    snake_overlay = lv_obj_create(bg_snake);
                    lv_obj_set_size(snake_overlay, LCD_W, LCD_H);
                    lv_obj_set_style_bg_color(snake_overlay, lv_color_black(), 0);
                    lv_obj_set_style_bg_opa(snake_overlay, LV_OPA_80, 0);
                    lv_obj_align(snake_overlay, LV_ALIGN_CENTER, 0, 0);

                    snake_overlay_label = lv_label_create(snake_overlay);
                    lv_obj_set_style_text_color(snake_overlay_label, lv_color_make(255, 200, 0), 0);
                    lv_obj_set_style_text_font(snake_overlay_label, &lv_font_montserrat_28, 0);
                    lv_obj_set_style_text_align(snake_overlay_label, LV_TEXT_ALIGN_CENTER, 0);
                    lv_obj_align(snake_overlay_label, LV_ALIGN_CENTER, 0, 0);
                }
                lv_obj_remove_flag(bg_snake, LV_OBJ_FLAG_HIDDEN);
                snake_game_state = SNAKE_STATE_START;
                lv_label_set_text_fmt(snake_overlay_label, "Best\nScore:\n%d", snake_best_score);
                lv_obj_remove_flag(snake_overlay, LV_OBJ_FLAG_HIDDEN);
            } else if (game_menu_selection == GAME_TICTACTOE) {
                // ==================== 過三關 啟動邏輯 ====================
                if (!bg_tictactoe) {
                    bg_tictactoe = lv_obj_create(lv_scr_act());
                    lv_obj_set_size(bg_tictactoe, LCD_W, LCD_H);
                    lv_obj_set_style_bg_color(bg_tictactoe, lv_color_black(), 0);
                    lv_obj_set_style_pad_all(bg_tictactoe, 0, 0);
                    lv_obj_set_style_border_width(bg_tictactoe, 0, 0);
                    lv_obj_set_style_radius(bg_tictactoe, 0, 0);

                    // 2. 建立 3x3 棋盤
                    for (int r = 0; r < 3; r++) {
                        for (int c = 0; c < 3; c++) {
                            ttt_cells[r][c] = lv_obj_create(bg_tictactoe);
                            lv_obj_set_size(ttt_cells[r][c], 34, 34);  // 格子大小
                            lv_obj_align(ttt_cells[r][c], LV_ALIGN_TOP_LEFT, 8 + c * 38,
                                         8 + r * 38);  // 置中計算
                            lv_obj_set_style_bg_color(ttt_cells[r][c], lv_color_hex(0x222222), 0);
                            // lv_obj_set_style_border_color(ttt_cells[r][c],
                            // lv_color_hex(0x555555), 0);
                            lv_obj_set_style_border_width(ttt_cells[r][c], 0, 0);
                            lv_obj_clear_flag(ttt_cells[r][c], LV_OBJ_FLAG_SCROLLABLE);

                            ttt_labels[r][c] = lv_label_create(ttt_cells[r][c]);
                            lv_obj_set_style_text_font(ttt_labels[r][c], &lv_font_montserrat_28, 0);
                            lv_obj_align(ttt_labels[r][c], LV_ALIGN_CENTER, 0, 0);
                        }
                    }

                    // 2. 建立 Toast 彈出提示框 (覆蓋在畫面上方)
                    ttt_toast_bg = lv_obj_create(bg_tictactoe);
                    lv_obj_set_size(ttt_toast_bg, 110, 50);
                    lv_obj_set_style_bg_color(ttt_toast_bg, lv_color_hex(0x000000), 0);
                    lv_obj_set_style_bg_opa(ttt_toast_bg, LV_OPA_90, 0);
                    lv_obj_set_style_border_color(ttt_toast_bg, lv_color_white(), 0);
                    lv_obj_set_style_border_width(ttt_toast_bg, 2, 0);
                    lv_obj_align(ttt_toast_bg, LV_ALIGN_CENTER, 0, 0);
                    lv_obj_add_flag(ttt_toast_bg, LV_OBJ_FLAG_HIDDEN);  // 預設隱藏

                    ttt_toast_label = lv_label_create(ttt_toast_bg);
                    lv_obj_set_style_text_font(ttt_toast_label, &lv_font_montserrat_16, 0);
                    lv_obj_set_style_text_align(ttt_toast_label, LV_TEXT_ALIGN_CENTER, 0);
                    lv_obj_align(ttt_toast_label, LV_ALIGN_CENTER, 0, 0);

                    // 3. 遊戲開始/結束 遮罩層
                    ttt_overlay = lv_obj_create(bg_tictactoe);
                    lv_obj_set_size(ttt_overlay, LCD_W, LCD_H);
                    lv_obj_set_style_bg_color(ttt_overlay, lv_color_black(), 0);
                    lv_obj_set_style_bg_opa(ttt_overlay, LV_OPA_80, 0);
                    lv_obj_align(ttt_overlay, LV_ALIGN_CENTER, 0, 0);

                    ttt_overlay_label = lv_label_create(ttt_overlay);
                    lv_obj_set_style_text_color(ttt_overlay_label, lv_color_make(255, 200, 0), 0);
                    lv_obj_set_style_text_font(ttt_overlay_label, &lv_font_montserrat_24, 0);
                    lv_obj_set_style_text_align(ttt_overlay_label, LV_TEXT_ALIGN_CENTER, 0);
                    lv_obj_align(ttt_overlay_label, LV_ALIGN_CENTER, 0, 0);
                }

                lv_obj_remove_flag(bg_tictactoe, LV_OBJ_FLAG_HIDDEN);
                ttt_game_state = TTT_STATE_START;
                lv_label_set_text(ttt_overlay_label, "Press to\nStart");
                lv_obj_remove_flag(ttt_overlay, LV_OBJ_FLAG_HIDDEN);
            }

            // if (!imu_initialized)
            //     imu_init();
            return;
        } else if (current_ui_mode == 3) {
            // ==================== 【時間模式】 ====================
            if (!bg_time_mode) {
                bg_time_mode = lv_obj_create(lv_scr_act());
                lv_obj_set_size(bg_time_mode, LCD_W, LCD_H);
                lv_obj_set_style_border_width(bg_time_mode, 0, 0);
                lv_obj_set_style_pad_all(bg_time_mode, 0, 0);
                lv_obj_clear_flag(bg_time_mode, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_align(bg_time_mode, LV_ALIGN_TOP_LEFT, 0, 0);
                lv_obj_set_style_radius(bg_time_mode, 0, 0);

                // 時間
                time_mode_time_label = lv_label_create(bg_time_mode);
                lv_obj_set_style_text_font(time_mode_time_label, &micro5_65, 0);
                lv_obj_align(time_mode_time_label, LV_ALIGN_LEFT_MID, 9, -22);  // 靠左 10，向下 10

                // 日期與星期 (右上角)
                time_mode_date_label = lv_label_create(bg_time_mode);
                lv_obj_set_style_text_font(time_mode_date_label, &lv_font_montserrat_24, 0);
                lv_obj_align(time_mode_date_label, LV_ALIGN_BOTTOM_LEFT, 9, -10);
            }
            lv_obj_remove_flag(bg_time_mode, LV_OBJ_FLAG_HIDDEN);

            update_clock_display();                            // 刷新資訊
            clock_is_bright = true;                            // 👈 原本是 false，改為 true
            clock_dim_countdown = CLOCK_DIM_TIMEOUT_SEC * 10;  // 👈 原本是 0，改為開始倒數
            auto backlight = Board::GetInstance().GetBacklight();
            if (backlight)
                backlight->SetBrightness(100);  // 👈 原本是 BRIGHTNESS_DIM，改為 100
            return;
        } else if (current_ui_mode == 6) {
            // ==================== 【天氣模式】 ====================
            if (!bg_weather_mode) {
                bg_weather_mode = lv_obj_create(lv_scr_act());
                lv_obj_set_size(bg_weather_mode, LCD_W, LCD_H);
                lv_obj_set_style_border_width(bg_weather_mode, 0, 0);
                lv_obj_set_style_pad_all(bg_weather_mode, 0, 0);
                lv_obj_clear_flag(bg_weather_mode, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_align(bg_weather_mode, LV_ALIGN_TOP_LEFT, 0, 0);
                lv_obj_set_style_radius(bg_weather_mode, 0, 0);

                // 圖示「畫布容器」
                weather_icon_container = lv_obj_create(bg_weather_mode);
                lv_obj_set_size(weather_icon_container, 70, 70);
                lv_obj_set_style_bg_opa(weather_icon_container, LV_OPA_TRANSP, 0);  // 透明底
                lv_obj_set_style_border_width(weather_icon_container, 0, 0);
                lv_obj_clear_flag(weather_icon_container, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_align(weather_icon_container, LV_ALIGN_TOP_LEFT, 2, 4);

                // 氣溫與狀態 (兩行)
                weather_current_label = lv_label_create(bg_weather_mode);
                lv_obj_set_style_text_font(weather_current_label, &lv_font_montserrat_24, 0);
                lv_obj_set_style_text_line_space(weather_current_label, 2, 0);
                lv_obj_align(weather_current_label, LV_ALIGN_TOP_LEFT, 10, 58);

                // 最高與最低溫
                weather_hilo_label = lv_label_create(bg_weather_mode);
                lv_obj_align(weather_hilo_label, LV_ALIGN_TOP_MID, 25, 12);
                lv_obj_set_style_text_line_space(weather_hilo_label, -4, 0);
            }
            lv_obj_remove_flag(bg_weather_mode, LV_OBJ_FLAG_HIDDEN);

            // 強制重繪一次圖標
            memset(last_drawn_weather, 0, sizeof(last_drawn_weather));
            update_clock_display();

            clock_is_bright = true;                            // 👈 原本是 false，改為 true
            clock_dim_countdown = CLOCK_DIM_TIMEOUT_SEC * 10;  // 👈 原本是 0，改為開始倒數
            auto backlight = Board::GetInstance().GetBacklight();
            if (backlight)
                backlight->SetBrightness(100);  // 👈 原本是 BRIGHTNESS_DIM，改為 100
            return;
        } else if (current_ui_mode == 0) {  // --- 退出遊戲，回到 AI ---
            // 恢復臉部顯示
            lv_obj_remove_flag(avatar_left_eye, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(avatar_right_eye, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(avatar_mouth, LV_OBJ_FLAG_HIDDEN);
            last_avatar_state = (DeviceState)-1;  // 強制刷新臉部
            clock_is_bright = true;
            clock_dim_countdown = CLOCK_DIM_TIMEOUT_SEC * 10;
            auto backlight = Board::GetInstance().GetBacklight();
            if (backlight)
                backlight->SetBrightness(100);
        }
    }

    // ==============================================
    // 過三關 (Tic-Tac-Toe) 遊戲核心引擎與 AI
    // ==============================================
    if (current_ui_mode == 2 && game_menu_selection == GAME_TICTACTOE) {
        // 1. 安全接收背景語音傳來的玩家下棋動作
        if (pending_ttt_r != -1 && pending_ttt_c != -1) {
            // 如果還在 Start 畫面，自動開始新遊戲
            if (ttt_game_state == TTT_STATE_START || ttt_game_state == TTT_STATE_GAMEOVER) {
                ttt_game_state = TTT_STATE_PLAYING;
                memset(ttt_board, 0, sizeof(ttt_board));
                ttt_user_turn = (esp_random() % 2 == 0);  // 🎲 改為隨機先手
                ttt_ai_delay_ticks = 0;
                if (ttt_overlay)
                    lv_obj_add_flag(ttt_overlay, LV_OBJ_FLAG_HIDDEN);

                if (ttt_toast_bg) {
                    lv_obj_remove_flag(ttt_toast_bg, LV_OBJ_FLAG_HIDDEN);
                    ttt_toast_timer = 15;
                    if (ttt_user_turn) {
                        lv_label_set_text(ttt_toast_label, "Your Turn");
                        lv_obj_set_style_text_color(ttt_toast_label, lv_color_hex(0x00FF66), 0);
                        if (ai_single_click_cb) {
                            ai_single_click_cb();
                        }
                    } else {
                        lv_label_set_text(ttt_toast_label, "thinking...");
                        lv_obj_set_style_text_color(ttt_toast_label, lv_color_hex(0xFF3333), 0);
                    }
                }
            }

            // 落子判斷
            if (ttt_user_turn) {
                if (ttt_board[pending_ttt_r][pending_ttt_c] == 0) {
                    ttt_board[pending_ttt_r][pending_ttt_c] = 1;
                    ttt_user_turn = false;
                    ttt_ai_delay_ticks = 0;

                    if (ttt_toast_bg) {
                        lv_label_set_text(ttt_toast_label, "thinking...");
                        lv_obj_set_style_text_color(ttt_toast_label, lv_color_hex(0xFF3333), 0);
                        lv_obj_remove_flag(ttt_toast_bg, LV_OBJ_FLAG_HIDDEN);
                        ttt_toast_timer = 15;
                    }
                } else {
                    // ⚠️ 位置已經被佔用的提示
                    if (ttt_toast_bg) {
                        lv_label_set_text(ttt_toast_label, "Taken!");
                        lv_obj_set_style_text_color(ttt_toast_label, lv_color_hex(0xFF3333), 0);
                        lv_obj_remove_flag(ttt_toast_bg, LV_OBJ_FLAG_HIDDEN);
                        ttt_toast_timer = 15;
                    }
                }
            }
            pending_ttt_r = -1;  // 處理完畢歸零
            pending_ttt_c = -1;
        }

        if (ttt_game_state == TTT_STATE_PLAYING) {
            // 檢查輸贏
            int winner = ttt_check_winner();
            if (winner != 0) {
                ttt_game_state = TTT_STATE_GAMEOVER;
                lv_obj_remove_flag(ttt_overlay, LV_OBJ_FLAG_HIDDEN);
                if (winner == 1)
                    lv_label_set_text(ttt_overlay_label, "You WIN!");
                else if (winner == 2)
                    lv_label_set_text(ttt_overlay_label, "AI WINS!");
                else
                    lv_label_set_text(ttt_overlay_label, "DRAW!");
            } else if (!ttt_user_turn) {
                // 電腦回合：思考 0.8 秒
                ttt_ai_delay_ticks++;
                if (ttt_ai_delay_ticks > 8) {
                    ttt_ai_make_move();
                    ttt_ai_delay_ticks = 0;

                    // 電腦下完，彈出換玩家的 Toast
                    if (ttt_toast_bg) {
                        lv_label_set_text(ttt_toast_label, "Your Turn");
                        lv_obj_set_style_text_color(ttt_toast_label, lv_color_hex(0x00FF66), 0);
                        lv_obj_remove_flag(ttt_toast_bg, LV_OBJ_FLAG_HIDDEN);
                        ttt_toast_timer = 15;
                    }
                    if (ai_single_click_cb) {
                        ai_single_click_cb();
                    }
                }
            }
        }

        // 處理 Toast 自動消失
        if (ttt_toast_timer > 0) {
            ttt_toast_timer--;
            if (ttt_toast_timer == 0 && ttt_toast_bg) {
                lv_obj_add_flag(ttt_toast_bg, LV_OBJ_FLAG_HIDDEN);
            }
        }

        // 刷新 3x3 棋盤 UI
        if (bg_tictactoe) {
            for (int r = 0; r < 3; r++) {
                for (int c = 0; c < 3; c++) {
                    if (ttt_board[r][c] == 1) {
                        lv_label_set_text(ttt_labels[r][c], "O");
                        lv_obj_set_style_text_color(ttt_labels[r][c], lv_color_hex(0x00FF66), 0);
                    } else if (ttt_board[r][c] == 2) {
                        lv_label_set_text(ttt_labels[r][c], "X");
                        lv_obj_set_style_text_color(ttt_labels[r][c], lv_color_hex(0xFF3333), 0);
                    } else {
                        lv_label_set_text(ttt_labels[r][c], "");
                    }
                }
            }
        }
    }

    // ==============================================
    // 貪食蛇自動前進引擎 (每 200ms 走一步)
    // ==============================================
    if (current_ui_mode == 2 && game_menu_selection == GAME_SNAKE &&
        snake_game_state == SNAKE_STATE_PLAYING) {
        static int snake_tick_counter = 0;
        if (++snake_tick_counter >= 2) {  // 2 * 100ms = 200ms 前進一格
            snake_tick_counter = 0;

            bool alive = snake_tick_step();  // 蛇向前爬一步
            update_snake_ui();               // 刷新畫面

            // 如果撞牆或咬到自己
            if (!alive) {
                snake_game_state = SNAKE_STATE_GAMEOVER;
                lv_label_set_text_fmt(snake_overlay_label, "GAME\nOVER\nScore:\n%d", snake_score);
                lv_obj_remove_flag(snake_overlay, LV_OBJ_FLAG_HIDDEN);
                ESP_LOGI("Snake", "Game Over! 最終得分: %d", snake_score);
            }
        }
    }

    // ==============================================
    // 通用螢幕背光 5 秒自動變暗倒數 (適用於 AI待機、時間、天氣)
    // ==============================================
    if (current_ui_mode == 3 || current_ui_mode == 6 ||
        (current_ui_mode == 0 && Application::GetInstance().GetDeviceState() == kDeviceStateIdle)) {
        if (clock_is_bright && clock_dim_countdown > 0) {
            clock_dim_countdown--;
            if (clock_dim_countdown == 0) {
                auto backlight = Board::GetInstance().GetBacklight();
                if (backlight)
                    backlight->SetBrightness(BRIGHTNESS_DIM);
                clock_is_bright = false;
                ESP_LOGI("System", "5秒無操作，螢幕自動變暗");
            }
        }
    }

    // --- 時鐘模式：每 1 秒更新一次時間文字。支援點亮、手動/自動 3 秒變暗 ---
    if (current_ui_mode == 3 || current_ui_mode == 6) {
        auto backlight = Board::GetInstance().GetBacklight();

        // 1. 處理按鍵訊號 (短按點亮 / 熄滅)
        if (pending_game_action == 6) {
            if (!clock_is_bright) {
                // 當前是暗的 -> 點亮螢幕至 100%！
                if (backlight)
                    backlight->SetBrightness(100);
                clock_is_bright = true;
                clock_dim_countdown = CLOCK_DIM_TIMEOUT_SEC * 10;  // 設定 5 秒倒數 (30 * 100ms)
                ESP_LOGI("Clock", "螢幕點亮！");

                // 點亮螢幕的瞬間，順便叫背景工人去抓最新天氣！
                int64_t now_us = esp_timer_get_time();
                // 加上 5 秒防刷保護，避免你連續快速按螢幕時狂發 API 請求
                if (weather_task_handle &&
                    (last_weather_fetch_us == 0 || now_us - last_weather_fetch_us > 30000000LL)) {
                    last_weather_fetch_us = now_us;
                    xTaskNotifyGive(weather_task_handle);  // 發出訊號，拍醒工人！
                    ESP_LOGI("Weather", "手動點亮時鐘，觸發背景即時天氣更新！ %lld",
                             last_weather_fetch_us);
                }
            } else {
                // 當前已經是亮的，手動再按一次 -> 立刻變暗 (不用等3秒)
                if (backlight)
                    backlight->SetBrightness(BRIGHTNESS_DIM);
                clock_is_bright = false;
                clock_dim_countdown = 0;
                ESP_LOGI("Clock", "手動變暗！");
            }
            pending_game_action = 0;  // 清除訊號
        }

        // 3. 每 1 秒刷新一次時間文字
        static int clock_timer_cnt = 0;
        if (++clock_timer_cnt >= 10) {  // 10 * 100ms = 1秒
            clock_timer_cnt = 0;
            update_clock_display();
        }

        return;  // 時鐘模式結束，不執行下面的遊戲與臉部邏輯
    }

    // ==============================================
    // 【體感控制核心】BMI270 讀取與雙遊戲方向分流
    // ==============================================
    if ((current_ui_mode == 0 || current_ui_mode == 2 || current_ui_mode == 5) && imu_initialized) {
        static bool tilt_locked_2048 = false;
        uint8_t reg = BMI270_REG_DATA_X_LSB;
        uint8_t data[6] = {0};

        esp_err_t res = i2c_master_transmit_receive(imu_dev_handle, &reg, 1, data, 6, 10);
        if (res == ESP_OK) {
            // 1. 先讀取晶片原始的數據
            int16_t raw_x = (int16_t)((data[1] << 8) | data[0]);
            int16_t raw_y = (int16_t)((data[3] << 8) | data[2]);
            int16_t accel_z = (int16_t)((data[5] << 8) | data[4]);

            // 2. 【核心修正】校正 StickS3 的物理安裝方向與螢幕的對應關係
            // 讓 accel_x 專門負責「左右傾斜」(右傾為正)
            // 讓 accel_y 專門負責「前後傾斜」(前傾/螢幕朝下為正)
            int16_t accel_x = -raw_y;
            int16_t accel_y = raw_x;

            // ==============================================
            // 【新增】：AI 模式 (0) 專屬的臉部重力微移邏輯
            // ==============================================
            if (current_ui_mode == 0 && face_container != nullptr) {
                int max_offset = 12;  // 表情最大可以移動的像素，你可以自由改成 8 或 10

                // 換算比例：4096 大約是垂直 90 度 (1g)
                int offset_x = (accel_x * max_offset) / 4096;
                int offset_y = (accel_y * max_offset) / 4096;

                // 限制不出界
                if (offset_x > max_offset)
                    offset_x = max_offset;
                if (offset_x < -max_offset)
                    offset_x = -max_offset;
                if (offset_y > max_offset)
                    offset_y = max_offset;
                if (offset_y < -max_offset)
                    offset_y = -max_offset;

                // 即時移動整個臉部的容器！
                // (注意：StickS3 在螢幕正放時，x 或 y 軸的感應器方向可能需要加上負號反轉)
                lv_obj_align(face_container, LV_ALIGN_CENTER, -offset_x, offset_y);
            }

            const int16_t THRESHOLD_2048 = 2500;
            const int16_t THRESHOLD_SNAKE = 2000;

            // --- APP 模式 ---
            if (current_ui_mode == 5) {
                if (app_menu_selection == APP_BWA) {
                    static int16_t prev_x = 0, prev_y = 0, prev_z = 0;
                    int delta =
                        abs(accel_x - prev_x) + abs(accel_y - prev_y) + abs(accel_z - prev_z);
                    prev_x = accel_x;
                    prev_y = accel_y;
                    prev_z = accel_z;

                    if (delta > 18000 && bwa_roll_anim_ticks == 0) {
                        bwa_roll_anim_ticks = 16;
                        ESP_LOGI("BwaPei", "偵測到搖晃！開始擲杯！");
                    }
                } else if (app_menu_selection == APP_MARK6) {
                    static int16_t prev_m6_x = 0, prev_m6_y = 0, prev_m6_z = 0;
                    int delta = abs(accel_x - prev_m6_x) + abs(accel_y - prev_m6_y) +
                                abs(accel_z - prev_m6_z);
                    prev_m6_x = accel_x;
                    prev_m6_y = accel_y;
                    prev_m6_z = accel_z;

                    if (marksix_shake_cooldown > 0) {
                        marksix_shake_cooldown--;
                    }

                    // 偵測到用力搖晃，且冷卻時間結束
                    if (delta > 18000 && marksix_shake_cooldown == 0) {
                        marksix_shake_cooldown = 6;  // 600ms 防重複抖動
                        draw_one_mark_six_ball();
                        ESP_LOGI("MarkSix", "偵測到搖晃！抽出號碼球！");
                    }
                } else if (app_menu_selection == APP_LEVEL) {
                    // BMI270 量程我們設為 ±4g，1g 約等於 8192。
                    // 用 accel_x 和 accel_y 來計算傾斜角度對應的像素點
                    int max_radius = 45;  // 氣泡最大移動範圍(像素)

                    // 靈敏度係數 (4096 約等於 30 度角就碰到邊緣)
                    // 備註：依據 Stick/Atom 的晶片貼片方向，x/y 可能需要加負號互換。
                    // 以下為 Stick S3 預設直立正面的通用映射：
                    int pixel_x = (accel_x * max_radius) / 4096;
                    int pixel_y = (accel_y * max_radius) / 4096;

                    // 限制氣泡不出界
                    if (pixel_x > max_radius)
                        pixel_x = max_radius;
                    if (pixel_x < -max_radius)
                        pixel_x = -max_radius;
                    if (pixel_y > max_radius)
                        pixel_y = max_radius;
                    if (pixel_y < -max_radius)
                        pixel_y = -max_radius;

                    if (level_bubble) {
                        // StickS3 的物理特性通常是：螢幕朝上平放時，氣泡應歸中
                        // 反轉方向使得實體操作就像真的氣泡一樣
                        lv_obj_align(level_bubble, LV_ALIGN_CENTER, -pixel_x, pixel_y);

                        // 計算總偏差值
                        int deviation = abs(pixel_x) + abs(pixel_y);

                        // 變色與文字狀態
                        if (deviation < 3) {
                            lv_obj_set_style_bg_color(level_bubble, lv_color_hex(0x00FF66),
                                                      0);  // 完美水平：亮綠色
                            lv_label_set_text(level_info_label, "FLAT");
                            lv_obj_set_style_text_color(level_info_label, lv_color_hex(0x00FF66),
                                                        0);
                        } else if (deviation < 15) {
                            lv_obj_set_style_bg_color(level_bubble, lv_color_hex(0xFFDD00),
                                                      0);  // 接近：黃色
                            lv_label_set_text_fmt(level_info_label, "X:%d  Y:%d", -pixel_x,
                                                  pixel_y);
                            lv_obj_set_style_text_color(level_info_label, lv_color_white(), 0);
                        } else {
                            lv_obj_set_style_bg_color(level_bubble, lv_color_hex(0xFF3333),
                                                      0);  // 歪斜：紅色
                            lv_label_set_text_fmt(level_info_label, "X:%d  Y:%d", -pixel_x,
                                                  pixel_y);
                            lv_obj_set_style_text_color(level_info_label, lv_color_white(), 0);
                        }
                    }
                }
            }
            // --- GAME 模式 ---
            else if (current_ui_mode == 2) {
                if (game_menu_selection == GAME_SPACE && space_game_state == SPACE_STATE_PLAYING) {
                    const int16_t DEADZONE = 600;
                    int ship_speed = (abs(accel_y) > 5000) ? 12 : 6;
                    if (accel_x > DEADZONE)
                        space_ship_x += ship_speed;  // 右傾往右移
                    else if (accel_x < -DEADZONE)
                        space_ship_x -= ship_speed;  // 左傾往左移

                    if (space_ship_x < 0)
                        space_ship_x = 0;
                    if (space_ship_x > LCD_W - 16)
                        space_ship_x = LCD_W - 16;
                    if (space_ship)
                        lv_obj_align(space_ship, LV_ALIGN_TOP_LEFT, space_ship_x, LCD_H * 0.9);
                } else if (game_menu_selection == GAME_BREAKOUT &&
                           bk_game_state == BK_STATE_PLAYING) {
                    const int16_t DEADZONE = 600;
                    int pad_speed = (abs(accel_x) > 5000) ? 12 : 6;
                    if (accel_x > DEADZONE)
                        bk_paddle_x += pad_speed;
                    else if (accel_x < -DEADZONE)
                        bk_paddle_x -= pad_speed;

                    if (bk_paddle_x < 0)
                        bk_paddle_x = 0;
                    if (bk_paddle_x > LCD_W - bk_paddle_w)
                        bk_paddle_x = LCD_W - bk_paddle_w;
                    if (bk_paddle)
                        lv_obj_align(bk_paddle, LV_ALIGN_TOP_LEFT, bk_paddle_x, LCD_H * 0.9);
                } else if (game_menu_selection == GAME_2048) {
                    if (abs(accel_x) > THRESHOLD_2048 || abs(accel_y) > THRESHOLD_2048) {
                        if (!tilt_locked_2048) {
                            // 直覺判斷：X軸變化大就是左右，Y軸變化大就是上下
                            if (abs(accel_x) > abs(accel_y)) {
                                if (accel_x > 0)
                                    pending_game_action = 2;  // 右傾 -> 向右滑
                                else
                                    pending_game_action = 1;  // 左傾 -> 向左滑
                            } else {
                                if (accel_y > 0)
                                    pending_game_action = 3;  // 前傾 -> 向上滑
                                else
                                    pending_game_action = 4;  // 後傾 -> 向下滑
                            }
                            tilt_locked_2048 = true;
                        }
                    } else
                        tilt_locked_2048 = false;
                } else if (game_menu_selection == GAME_SNAKE &&
                           snake_game_state == SNAKE_STATE_PLAYING) {
                    if (abs(accel_x) > THRESHOLD_SNAKE || abs(accel_y) > THRESHOLD_SNAKE) {
                        if (abs(accel_x) > abs(accel_y)) {
                            if (accel_x > 0)
                                snake_set_direction(DIR_RIGHT);
                            else
                                snake_set_direction(DIR_LEFT);
                        } else {
                            if (accel_y > 0)
                                snake_set_direction(DIR_UP);
                            else
                                snake_set_direction(DIR_DOWN);
                        }
                    }
                }
            }
        }
    }

    // 安全地在 LVGL 執行緒裡處理遊戲動作 (方向 1~4，按鍵動作 5)
    if ((current_ui_mode == 2 || current_ui_mode == 5) && pending_game_action != 0) {
        if (pending_game_action == 1)
            move_2048_left();
        else if (pending_game_action == 2)
            move_2048_right();
        else if (pending_game_action == 3)
            move_2048_up();
        else if (pending_game_action == 4)
            move_2048_down();
        else if (pending_game_action == 5) {
            // ==================== APP 模式的短按動作 ====================
            if (current_ui_mode == 5) {
                if (app_menu_selection == APP_BWA) {
                    if (bwa_roll_anim_ticks == 0)
                        bwa_roll_anim_ticks = 8;
                } else if (app_menu_selection == APP_MARK6) {
                    draw_one_mark_six_ball();
                } else if (app_menu_selection == APP_TIMER) {
                    if (timer_state == TIMER_IDLE || timer_state == TIMER_PAUSED) {
                        timer_state = TIMER_RUNNING;
                        // 設定系統絕對結束時間 = 現在時間 + 剩餘微秒
                        timer_end_time_us = esp_timer_get_time() + timer_remain_us;
                        lv_label_set_text(timer_status_label, "Running...");
                        lv_obj_set_style_text_color(timer_status_label, lv_color_hex(0x00FF66), 0);
                    } else if (timer_state == TIMER_RUNNING) {
                        timer_state = TIMER_PAUSED;
                        // 記錄暫停時還剩多少微秒
                        timer_remain_us = timer_end_time_us - esp_timer_get_time();
                        lv_label_set_text(timer_status_label, "Paused");
                        lv_obj_set_style_text_color(timer_status_label, lv_color_make(255, 200, 0),
                                                    0);
                    } else if (timer_state == TIMER_ALARM) {
                        timer_remain_us = timer_total_us;  // 重置
                        timer_state = TIMER_IDLE;
                        lv_obj_set_style_bg_color(bg_timer, lv_color_black(), 0);
                        lv_label_set_text(timer_status_label, "Press to Start");
                        lv_obj_set_style_text_color(timer_status_label, lv_color_hex(0xAAAAAA), 0);
                        lv_obj_set_style_text_color(timer_time_label, lv_color_white(), 0);
                        lv_obj_set_style_text_color(timer_ms_label, lv_color_hex(0xAAAAAA), 0);
                    }
                } else if (app_menu_selection == APP_STOPWATCH) {
                    if (sw_state == SW_IDLE || sw_state == SW_PAUSED) {
                        sw_state = SW_RUNNING;
                        sw_start_time_us = esp_timer_get_time() - sw_accumulated_us;
                        lv_label_set_text(sw_status_label, "Running...");
                        lv_obj_set_style_text_color(sw_status_label, lv_color_hex(0x00FF66), 0);
                    } else if (sw_state == SW_RUNNING) {
                        sw_state = SW_PAUSED;
                        sw_accumulated_us = esp_timer_get_time() - sw_start_time_us;
                        lv_label_set_text(sw_status_label, "Paused");
                        lv_obj_set_style_text_color(sw_status_label, lv_color_make(255, 200, 0), 0);
                    }
                    // 若要重置碼表，只要長按或側鍵退出回選單再進入即可自動歸零。
                }
            }
            // ==================== GAME 模式的短按動作 ====================
            else if (current_ui_mode == 2) {
                if (game_menu_selection == GAME_SPACE) {
                    if (space_game_state == SPACE_STATE_START ||
                        space_game_state == SPACE_STATE_GAMEOVER) {
                        space_game_state = SPACE_STATE_PLAYING;
                        if (space_overlay)
                            lv_obj_add_flag(space_overlay, LV_OBJ_FLAG_HIDDEN);
                        space_score = 0;
                        space_level = 1;
                        space_active_rows = 3;
                        space_descent_timer = 0;
                        alien_speed = 10;
                        space_ship_x = (135 - 16) / 2;
                        alien_group_x = 0;
                        alien_group_y = SPACE_ALIEN_START_Y;
                        alien_dir = 1;

                        for (int r = 0; r < SPACE_MAX_ALIEN_ROWS; r++) {
                            for (int c = 0; c < SPACE_ALIEN_COLS; c++) {
                                if (r < space_active_rows) {
                                    space_aliens[r][c].active = true;
                                    lv_obj_remove_flag(space_aliens[r][c].obj, LV_OBJ_FLAG_HIDDEN);
                                    lv_obj_align(space_aliens[r][c].obj, LV_ALIGN_TOP_LEFT,
                                                 15 + c * 25, SPACE_ALIEN_START_Y + r * 15);
                                } else {
                                    space_aliens[r][c].active = false;
                                    lv_obj_add_flag(space_aliens[r][c].obj, LV_OBJ_FLAG_HIDDEN);
                                }
                            }
                        }
                        for (int i = 0; i < SPACE_MAX_BULLETS; i++) {
                            space_bullets[i].active = false;
                            lv_obj_add_flag(space_bullets[i].obj, LV_OBJ_FLAG_HIDDEN);
                        }
                        for (int i = 0; i < SPACE_MAX_BOMBS; i++) {
                            space_bombs[i].active = false;
                            lv_obj_add_flag(space_bombs[i].obj, LV_OBJ_FLAG_HIDDEN);
                        }
                        lv_label_set_text_fmt(space_score_label, "L:%d S:%d B:%d", space_level,
                                              space_score, space_best_score);
                    } else if (space_game_state == SPACE_STATE_PLAYING) {
                        space_game_state = SPACE_STATE_PAUSE;
                        if (space_overlay && space_overlay_label) {
                            lv_label_set_text_fmt(space_overlay_label, "Paused\nScore:\n%d",
                                                  space_score);
                            lv_obj_remove_flag(space_overlay, LV_OBJ_FLAG_HIDDEN);
                        }
                    } else if (space_game_state == SPACE_STATE_PAUSE) {
                        space_game_state = SPACE_STATE_PLAYING;
                        if (space_overlay)
                            lv_obj_add_flag(space_overlay, LV_OBJ_FLAG_HIDDEN);
                    }
                }
                // ===============================================
                // 按鍵控制區塊 (加入 Game Over 判斷)
                // ===============================================
                else if (game_menu_selection == GAME_SLOT) {
                    if (slot_game_state == SLOT_STATE_GAMEOVER) {
                        slot_score = 100;
                        lv_label_set_text_fmt(slot_score_label, "C: %d", slot_score);
                        lv_label_set_text(slot_status_label, "Press to Spin!");
                        slot_game_state = SLOT_STATE_IDLE;
                    } else if (slot_game_state == SLOT_STATE_IDLE) {
                        if (slot_score >= 10) {
                            slot_score -= 10;
                            lv_label_set_text_fmt(slot_score_label, "C: %d", slot_score);
                            lv_label_set_text(slot_status_label, "Spinning...");

                            slot_start_spinning(0);
                            slot_start_spinning(1);
                            slot_start_spinning(2);
                            slot_game_state = SLOT_STATE_SPINNING;
                        } else {
                            // 防呆，沒錢了強制進入 GameOver
                            slot_game_state = SLOT_STATE_GAMEOVER;
                            lv_label_set_text(slot_status_label, "GAME OVER");
                        }
                    } else if (slot_game_state == SLOT_STATE_SPINNING) {
                        slot_stop_reel(0);  // 按一下停第一排
                        slot_game_state = SLOT_STATE_STOP_1;
                    } else if (slot_game_state == SLOT_STATE_STOP_1) {
                        slot_stop_reel(1);  // 按第二下停第二排
                        slot_game_state = SLOT_STATE_STOP_2;
                    } else if (slot_game_state == SLOT_STATE_STOP_2) {
                        slot_stop_reel(2);  // 按第三下停第三排，並結算

                        // =========================
                        // 💰 中獎結算邏輯
                        // =========================
                        int r0 = slot_results[0];
                        int r1 = slot_results[1];
                        int r2 = slot_results[2];

                        // 三星連線大獎！
                        if (r0 == r1 && r1 == r2) {
                            int win_amount = 50;

                            // 第 5 個是 7 (索引 4)
                            if (r0 == 4) {
                                win_amount = 500;
                                lv_label_set_text(slot_status_label, "SUPER 777! +500");
                            }
                            // 第 8 個是 BAR (索引 7)
                            else if (r0 == 7) {
                                win_amount = 200;
                                lv_label_set_text(slot_status_label, "BAR BAR BAR! +200");
                            } else {
                                lv_label_set_text_fmt(slot_status_label, "JACKPOT! +%d",
                                                      win_amount);
                            }
                            slot_score += win_amount;

                        }
                        // 兩個一樣，小獎安慰
                        else if (r0 == r1 || r1 == r2 || r0 == r2) {
                            slot_score += 15;
                            lv_label_set_text(slot_status_label, "Small Win! +15");
                        }
                        // 沒中獎
                        else {
                            lv_label_set_text(slot_status_label, "Try Again!");
                        }

                        lv_label_set_text_fmt(slot_score_label, "C: %d", slot_score);

                        // 【修復 3】：檢查結算後是否破產
                        if (slot_score <= 0) {
                            slot_game_state = SLOT_STATE_GAMEOVER;
                            lv_label_set_text_fmt(slot_score_label, "BANKRUPT!");
                            lv_label_set_text(slot_status_label, "GAME OVER");
                            lv_obj_set_style_text_color(slot_status_label, lv_color_hex(0xFF3333),
                                                        0);  // 變紅字警告
                        } else {
                            slot_game_state = SLOT_STATE_IDLE;
                            lv_obj_set_style_text_color(slot_status_label, lv_color_white(),
                                                        0);  // 恢復白字
                        }
                    }
                } else if (game_menu_selection == GAME_TICTACTOE) {
                    if (ttt_game_state == TTT_STATE_START || ttt_game_state == TTT_STATE_GAMEOVER) {
                        ttt_game_state = TTT_STATE_PLAYING;
                        if (ttt_overlay)
                            lv_obj_add_flag(ttt_overlay, LV_OBJ_FLAG_HIDDEN);

                        // 清空盤面
                        memset(ttt_board, 0, sizeof(ttt_board));

                        // 隨機決定誰先手 (50% 機率)
                        ttt_user_turn = (esp_random() % 2 == 0);
                        ttt_ai_delay_ticks = 0;
                        if (ttt_toast_bg) {
                            lv_obj_remove_flag(ttt_toast_bg, LV_OBJ_FLAG_HIDDEN);
                            ttt_toast_timer = 15;
                            if (ttt_user_turn) {
                                lv_label_set_text(ttt_toast_label, "Your Turn");
                                lv_obj_set_style_text_color(ttt_toast_label, lv_color_hex(0x00FF66),
                                                            0);
                                if (ai_single_click_cb) {
                                    ai_single_click_cb();
                                }

                            } else {
                                lv_label_set_text(ttt_toast_label, "thinking...");
                                lv_obj_set_style_text_color(ttt_toast_label, lv_color_hex(0xFF3333),
                                                            0);
                            }
                        }
                    }
                    /*
                    else if (ttt_game_state == TTT_STATE_PLAYING) {
                        ttt_game_state = TTT_STATE_PAUSE;
                        lv_label_set_text(ttt_overlay_label, "Paused");
                        lv_obj_remove_flag(ttt_overlay, LV_OBJ_FLAG_HIDDEN);
                    } else if (ttt_game_state == TTT_STATE_PAUSE) {
                        ttt_game_state = TTT_STATE_PLAYING;
                        if (ttt_overlay)
                            lv_obj_add_flag(ttt_overlay, LV_OBJ_FLAG_HIDDEN);
                    }
                    */
                } else if (game_menu_selection == GAME_BREAKOUT) {
                    if (bk_game_state == BK_STATE_START || bk_game_state == BK_STATE_GAMEOVER) {
                        bk_game_state = BK_STATE_PLAYING;
                        if (bk_overlay)
                            lv_obj_add_flag(bk_overlay, LV_OBJ_FLAG_HIDDEN);
                        init_breakout_level(true);
                        lv_label_set_text_fmt(bk_score_label, "L:%d  S:%d", bk_level, bk_score);
                    } else if (bk_game_state == BK_STATE_PLAYING) {
                        if (!bk_ball_launched)
                            bk_ball_launched = true;
                        else {
                            bk_game_state = BK_STATE_PAUSE;
                            if (bk_overlay && bk_overlay_label) {
                                lv_label_set_text_fmt(bk_overlay_label, "Paused\nScore:\n%d",
                                                      bk_score);
                                lv_obj_remove_flag(bk_overlay, LV_OBJ_FLAG_HIDDEN);
                            }
                        }
                    } else if (bk_game_state == BK_STATE_PAUSE) {
                        bk_game_state = BK_STATE_PLAYING;
                        if (bk_overlay)
                            lv_obj_add_flag(bk_overlay, LV_OBJ_FLAG_HIDDEN);
                    }
                } else if (game_menu_selection == GAME_2048) {
                    if (game_state_2048 == STATE_START_SCREEN) {
                        game_state_2048 = STATE_PLAYING;
                        if (overlay_2048)
                            lv_obj_add_flag(overlay_2048, LV_OBJ_FLAG_HIDDEN);
                        memset(board_2048, 0, sizeof(board_2048));
                        current_score = 0;
                        can_undo = false;
                        add_random_2048();
                        add_random_2048();
                        update_2048_ui();
                    } else if (game_state_2048 == STATE_PLAYING)
                        undo_2048();
                    else if (game_state_2048 == STATE_GAMEOVER) {
                        game_state_2048 = STATE_PLAYING;
                        if (overlay_2048)
                            lv_obj_add_flag(overlay_2048, LV_OBJ_FLAG_HIDDEN);
                        memset(board_2048, 0, sizeof(board_2048));
                        current_score = 0;
                        can_undo = false;
                        add_random_2048();
                        add_random_2048();
                        update_2048_ui();
                    }
                } else if (game_menu_selection == GAME_SNAKE) {
                    if (snake_game_state == SNAKE_STATE_START) {
                        snake_game_state = SNAKE_STATE_PLAYING;
                        if (snake_overlay)
                            lv_obj_add_flag(snake_overlay, LV_OBJ_FLAG_HIDDEN);
                        snake_init_game();
                        update_snake_ui();
                    } else if (snake_game_state == SNAKE_STATE_PLAYING) {
                        snake_game_state = SNAKE_STATE_PAUSE;
                        lv_label_set_text_fmt(snake_overlay_label, "Paused\nScore:\n%d",
                                              snake_score);
                        lv_obj_remove_flag(snake_overlay, LV_OBJ_FLAG_HIDDEN);
                    } else if (snake_game_state == SNAKE_STATE_PAUSE) {
                        snake_game_state = SNAKE_STATE_PLAYING;
                        if (snake_overlay)
                            lv_obj_add_flag(snake_overlay, LV_OBJ_FLAG_HIDDEN);
                    } else if (snake_game_state == SNAKE_STATE_GAMEOVER) {
                        snake_game_state = SNAKE_STATE_PLAYING;
                        if (snake_overlay)
                            lv_obj_add_flag(snake_overlay, LV_OBJ_FLAG_HIDDEN);
                        snake_init_game();
                        update_snake_ui();
                    }
                }
            }
        }
        pending_game_action = 0;  // 執行完畢，重設訊號
    }

    // --- 擲杯滾動動畫引擎 ---
    if (current_ui_mode == 5 && app_menu_selection == APP_BWA && bwa_roll_anim_ticks > 0) {
        bwa_roll_anim_ticks--;

        // 滾動期間不斷變換顏色 (木色/紅色閃爍)
        set_bwa_face(esp_random() % 2, esp_random() % 2, true);

        // 筊杯輕微晃動 (Jitter)，產生逼真的撞擊手感
        if (bwa_1_bg && bwa_2_bg) {
            int shake_1_x = -36 + (esp_random() % 9) - 4;
            int shake_1_y = -28 + (esp_random() % 9) - 4;
            int shake_2_x = 36 + (esp_random() % 9) - 4;
            int shake_2_y = -28 + (esp_random() % 9) - 4;
            lv_obj_align(bwa_1_bg, LV_ALIGN_CENTER, shake_1_x, shake_1_y);
            lv_obj_align(bwa_2_bg, LV_ALIGN_CENTER, shake_2_x, shake_2_y);
        }

        if (bwa_roll_anim_ticks == 0) {
            // 滾動結束，筊杯回正並顯示最終結果
            if (bwa_1_bg)
                lv_obj_align(bwa_1_bg, LV_ALIGN_CENTER, -36, -28);
            if (bwa_2_bg)
                lv_obj_align(bwa_2_bg, LV_ALIGN_CENTER, 36, -28);
            set_bwa_face(esp_random() % 2, esp_random() % 2, false);
        }
    }

    // ==============================================
    // 計時器 (Timer) 與 碼表 (Stopwatch) 高精度引擎
    // ==============================================
    if (current_ui_mode == 5) {
        if (app_menu_selection == APP_TIMER || app_menu_selection == APP_STOPWATCH) {
            if (Application::GetInstance().GetDeviceState() == kDeviceStateListening) {
                Application::GetInstance().SetDeviceState(kDeviceStateIdle);
            }
        }

        if (app_menu_selection == APP_TIMER) {
            int64_t current_remain = timer_remain_us;
            if (timer_state == TIMER_RUNNING) {
                current_remain = timer_end_time_us - esp_timer_get_time();
                if (current_remain <= 0) {
                    current_remain = 0;
                    timer_state = TIMER_ALARM;

                    // 改變狀態標籤為白字，以免被紅色背景吃掉
                    lv_label_set_text(timer_status_label, "TIME'S UP!");
                    lv_obj_set_style_text_color(timer_status_label, lv_color_white(), 0);
                    ESP_LOGI("Timer", "計時時間到！觸發警報");
                }
            }
            if (timer_state != TIMER_ALARM) {
                int total_s = current_remain / 1000000;
                int m = total_s / 60;
                int s = total_s % 60;
                // 取得 00~99 的毫秒百分位
                int centi_sec = (current_remain % 1000000) / 10000;

                lv_label_set_text_fmt(timer_time_label, "%02d:%02d", m, s);
                lv_label_set_text_fmt(timer_ms_label, "%02d", centi_sec);
            } else {
                // 紅白閃爍警告
                static bool blink = false;
                blink = !blink;
                lv_obj_set_style_bg_color(bg_timer,
                                          blink ? lv_color_hex(0xFF0000) : lv_color_black(), 0);

                // 確保文字一律保持白色，在紅黑背景上都能看清楚
                lv_obj_set_style_text_color(timer_time_label, lv_color_white(), 0);
                lv_obj_set_style_text_color(timer_ms_label, lv_color_white(), 0);

                lv_label_set_text(timer_time_label, "00:00");
                lv_label_set_text(timer_ms_label, "00");
            }
        } else if (app_menu_selection == APP_STOPWATCH) {
            int64_t current_elapsed = sw_accumulated_us;
            if (sw_state == SW_RUNNING) {
                current_elapsed = esp_timer_get_time() - sw_start_time_us;
            }

            int total_s = current_elapsed / 1000000;
            int m = total_s / 60;
            int s = total_s % 60;
            int centi_sec = (current_elapsed % 1000000) / 10000;  // 00~99

            lv_label_set_text_fmt(sw_time_label, "%02d:%02d", m, s);
            lv_label_set_text_fmt(sw_ms_label, "%02d", centi_sec);
        }
    }

    // ==============================================
    // 太空侵略者 (Space) 遊戲核心引擎
    // ==============================================
    if (current_ui_mode == 2 && game_menu_selection == GAME_SPACE &&
        space_game_state == SPACE_STATE_PLAYING) {
        static int space_fire_tick = 0;
        static int space_alien_tick = 0;

        bool game_over = false;
        int active_aliens = 0;
        int bottom_alien_y = 0;

        // ------------------------------------------
        // 1. 自動發射與子彈飛行邏輯
        // ------------------------------------------
        space_fire_tick++;
        if (space_fire_tick >= 5) {  // 每 500ms 自動發射一顆子彈
            space_fire_tick = 0;
            for (int i = 0; i < SPACE_MAX_BULLETS; i++) {
                if (!space_bullets[i].active) {
                    space_bullets[i].active = true;
                    space_bullets[i].x = space_ship_x + 6;  // 從飛船正中央射出
                    space_bullets[i].y = 110;
                    lv_obj_remove_flag(space_bullets[i].obj, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_align(space_bullets[i].obj, LV_ALIGN_TOP_LEFT, space_bullets[i].x,
                                 space_bullets[i].y);
                    break;
                }
            }
        }

        // 子彈向上飛行
        for (int i = 0; i < SPACE_MAX_BULLETS; i++) {
            if (space_bullets[i].active) {
                space_bullets[i].y -= 8;  // 子彈速度
                if (space_bullets[i].y < 0) {
                    space_bullets[i].active = false;  // 飛出螢幕外，回收子彈
                    lv_obj_add_flag(space_bullets[i].obj, LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_align(space_bullets[i].obj, LV_ALIGN_TOP_LEFT, space_bullets[i].x,
                                 space_bullets[i].y);
                }
            }
        }

        // ------------------------------------------
        // 2. 碰撞偵測 (Hitbox: 子彈 vs 外星人)
        // ------------------------------------------
        for (int i = 0; i < SPACE_MAX_BULLETS; i++) {
            if (!space_bullets[i].active)
                continue;
            for (int r = 0; r < space_active_rows; r++)  // 改為 space_active_rows
            {
                for (int c = 0; c < SPACE_ALIEN_COLS; c++) {
                    if (!space_aliens[r][c].active)
                        continue;

                    int ax = alien_group_x + 15 + c * 25;
                    int ay = alien_group_y + r * 15;

                    // 簡單的 AABB 碰撞算法 (子彈 4x8，外星人 10x10)
                    if (space_bullets[i].x < ax + SPACE_ALIEN_W && space_bullets[i].x + 4 > ax &&
                        space_bullets[i].y < ay + SPACE_ALIEN_H && space_bullets[i].y + 8 > ay) {
                        // 擊殺成功！
                        space_aliens[r][c].active = false;
                        lv_obj_add_flag(space_aliens[r][c].obj, LV_OBJ_FLAG_HIDDEN);
                        space_bullets[i].active = false;
                        lv_obj_add_flag(space_bullets[i].obj, LV_OBJ_FLAG_HIDDEN);

                        space_score += 10;  // 得分！
                        lv_label_set_text_fmt(space_score_label, "S:%d  B:%d", space_score,
                                              space_best_score);
                        break;
                    }
                }
            }
        }

        // ------------------------------------------
        // 2.5 【全新平衡飛彈系統】：循序漸進的難度
        // ------------------------------------------
        int max_allowed_bombs = 0;
        int drop_chance = 0;

        if (space_level == 1) {
            // 第 1 關：極度溫和，最多 1 顆，機率極低
            max_allowed_bombs = 1;
            drop_chance = 1;
        } else if (space_level == 2) {
            // 第 2 關：微幅提升，最多 2 顆
            max_allowed_bombs = 2;
            drop_chance = 3;
        } else {
            // 第 3 關起：難度開始爆發
            max_allowed_bombs = space_level;
            if (max_allowed_bombs > SPACE_MAX_BOMBS)
                max_allowed_bombs = SPACE_MAX_BOMBS;  // 上限 8 顆
            drop_chance = space_level * 2;
            if (drop_chance > 15)
                drop_chance = 15;  // 機率最高鎖定在 15% (否則會變彈幕地獄)
        }

        // 【新手保護期】：第一關如果分數還沒超過 50
        // 分，絕對不掉任何飛彈，讓玩家安心適應體感操作！
        if (space_level == 1 && space_score < 50) {
            max_allowed_bombs = 0;
        }

        // 接著計算目前畫面上已經有幾顆飛彈...
        int active_bombs = 0;
        for (int i = 0; i < SPACE_MAX_BOMBS; i++) {
            if (space_bombs[i].active)
                active_bombs++;
        }

        // 如果畫面上的飛彈還沒達到當前關卡上限，嘗試投彈
        if (active_bombs < max_allowed_bombs) {
            if (esp_random() % 100 < drop_chance) {
                for (int i = 0; i < SPACE_MAX_BOMBS; i++) {
                    if (!space_bombs[i].active) {
                        // 隨機選取存活外星人發射飛彈
                        int r = esp_random() % space_active_rows;
                        int c = esp_random() % SPACE_ALIEN_COLS;
                        if (space_aliens[r][c].active) {
                            space_bombs[i].active = true;
                            space_bombs[i].x =
                                alien_group_x + 15 + c * 25 + (SPACE_ALIEN_W / 2 - 2);
                            space_bombs[i].y = alien_group_y + r * 15 + SPACE_ALIEN_H;
                            lv_obj_remove_flag(space_bombs[i].obj, LV_OBJ_FLAG_HIDDEN);
                            lv_obj_align(space_bombs[i].obj, LV_ALIGN_TOP_LEFT, space_bombs[i].x,
                                         space_bombs[i].y);
                            break;
                        }
                    }
                }
            }
        }

        // 炸彈下落與玩家碰撞判定
        for (int i = 0; i < SPACE_MAX_BOMBS; i++) {
            if (space_bombs[i].active) {
                space_bombs[i].y += 5;  // 炸彈下落速度 (比子彈的 8 慢，讓你有時間躲)
                if (space_bombs[i].y > LCD_H) {
                    space_bombs[i].active = false;  // 掉出螢幕，回收炸彈
                    lv_obj_add_flag(space_bombs[i].obj, LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_align(space_bombs[i].obj, LV_ALIGN_TOP_LEFT, space_bombs[i].x,
                                 space_bombs[i].y);

                    // 碰撞判定：炸彈 (5x5) vs 飛船 (16x8, Y=116)
                    if (space_bombs[i].x < space_ship_x + 16 &&
                        space_bombs[i].x + 5 > space_ship_x && space_bombs[i].y < LCD_H * 0.9 + 8 &&
                        space_bombs[i].y + 5 > LCD_H * 0.9) {
                        game_over = true;  // 被炸彈擊中，Game Over！
                        ESP_LOGI("Space", "💥 被外星人炸彈擊毀！");
                    }
                }
            }
        }

        // ------------------------------------------
        // 3. 外星人軍團巡邏與逼近邏輯
        // ------------------------------------------
        space_alien_tick++;
        if (space_alien_tick >= alien_speed) {
            space_alien_tick = 0;

            int max_x = 0;
            int min_x = 128;

            // 計算外星人陣型的真實邊界
            for (int r = 0; r < space_active_rows; r++)  // ★確保這裡是 space_active_rows
            {
                for (int c = 0; c < SPACE_ALIEN_COLS; c++) {
                    if (space_aliens[r][c].active) {
                        active_aliens++;
                        int ax = alien_group_x + 15 + c * 25;

                        // ★ 檢查1：這裡必須是 alien_group_y！
                        int ay = alien_group_y + r * 15;

                        if (ax > max_x)
                            max_x = ax;
                        if (ax < min_x)
                            min_x = ax;
                        if (ay > bottom_alien_y)
                            bottom_alien_y = ay;
                    }
                }
            }

            if (active_aliens == 0) {
                // ==========================================
                // 【全部清空】進入下一關！
                // ==========================================
                space_level++;
                space_descent_timer = 0;  // 過關重置下移計時器

                // 【核心算法】：每過 3 關增加 1 排，最多 6 排！
                // Level 1~3: 3排 | Level 4~6: 4排 | Level 7~9: 5排 | Level 10+: 6排
                space_active_rows = 3 + (space_level - 1) / 3;
                if (space_active_rows > SPACE_MAX_ALIEN_ROWS) {
                    space_active_rows = SPACE_MAX_ALIEN_ROWS;
                }

                alien_group_x = 0;
                alien_group_y = SPACE_ALIEN_START_Y;  // 重置回 50

                // 每次過關適度加速
                if (alien_speed > 3) {
                    alien_speed -= 1;
                }

                // 重新佈陣敵人
                for (int r = 0; r < SPACE_MAX_ALIEN_ROWS; r++) {
                    for (int c = 0; c < SPACE_ALIEN_COLS; c++) {
                        if (r < space_active_rows) {
                            space_aliens[r][c].active = true;
                            lv_obj_remove_flag(space_aliens[r][c].obj, LV_OBJ_FLAG_HIDDEN);
                            lv_obj_align(space_aliens[r][c].obj, LV_ALIGN_TOP_LEFT, 15 + c * 25,
                                         SPACE_ALIEN_START_Y + r * 15);
                        } else {
                            space_aliens[r][c].active = false;
                            lv_obj_add_flag(space_aliens[r][c].obj, LV_OBJ_FLAG_HIDDEN);
                        }
                    }
                }

                lv_label_set_text_fmt(space_score_label, "L:%d S:%d B:%d", space_level, space_score,
                                      space_best_score);
                ESP_LOGI("Space", "★ 通關！進入第 %d 關，敵人增加至 %d 排！", space_level,
                         space_active_rows);

            } else {
                // 碰壁轉向與向下逼近
                if (alien_dir == 1 && max_x + SPACE_ALIEN_W >= (LCD_W - 4)) {  // 124
                    alien_dir = -1;
                    alien_group_y += 8;
                } else if (alien_dir == -1 && min_x <= 4) {
                    alien_dir = 1;
                    alien_group_y += 8;
                } else {
                    alien_group_x += alien_dir * 4;
                }

                // 【Game Over 判定】外星人是否碰到底部？
                if (bottom_alien_y + SPACE_ALIEN_H >= 110) {
                    game_over = true;
                } else {
                    // 更新所有存活外星人的畫面位置
                    for (int r = 0; r < space_active_rows; r++) {
                        for (int c = 0; c < SPACE_ALIEN_COLS; c++) {
                            if (space_aliens[r][c].active) {
                                lv_obj_align(space_aliens[r][c].obj, LV_ALIGN_TOP_LEFT,
                                             alien_group_x + 15 + c * 25, alien_group_y + r * 15);
                            }
                        }
                    }
                }
            }
        }

        // ------------------------------------------
        // 【修復】：定時強制向下逼近一層（即時下移刷新）
        // ------------------------------------------
        // ★ 測試提醒：如果你想在第 1 關馬上看到效果，可以暫時把 >= 3 改成 >= 1 測試！
        if (space_level >= 3) {
            space_descent_timer++;
            // 時間到了 (每 6 秒一次)
            if (space_descent_timer >= (SPACE_DESCENT_INTERVAL_SEC * 10)) {
                space_descent_timer = 0;
                alien_group_y += SPACE_DESCENT_STEP_Y;  // 向下跨一整層 (15px)

                // 【關鍵修復】：時間一到「立刻」強制更新所有存活外星人的座標！
                for (int r = 0; r < space_active_rows; r++) {
                    for (int c = 0; c < SPACE_ALIEN_COLS; c++) {
                        if (space_aliens[r][c].active) {
                            lv_obj_align(space_aliens[r][c].obj, LV_ALIGN_TOP_LEFT,
                                         alien_group_x + 15 + c * 25, alien_group_y + r * 15);
                        }
                    }
                }
                ESP_LOGI("Space", "⚠️ 警報！第 %d 關敵人強制下推一層！當前 Y: %d", space_level,
                         alien_group_y);
            }
        }

        // ------------------------------------------
        // 4. Game Over 與最高分儲存
        // ------------------------------------------
        if (game_over) {
            space_game_state = SPACE_STATE_GAMEOVER;

            // 破紀錄寫入 NVS！
            if (space_score > space_best_score) {
                space_best_score = space_score;
                save_space_best_score(space_best_score);
            }

            lv_label_set_text_fmt(space_overlay_label, "GAME\nOVER\nScore:\n%d", space_score);
            lv_obj_remove_flag(space_overlay, LV_OBJ_FLAG_HIDDEN);
            ESP_LOGI("Space", "Game Over! 最終得分: %d", space_score);
        }
    }

    // ==============================================
    // 打磚塊 (Breakout) 遊戲核心引擎
    // ==============================================
    if (current_ui_mode == 2 && game_menu_selection == GAME_BREAKOUT &&
        bk_game_state == BK_STATE_PLAYING) {
        if (!bk_ball_launched) {
            // 球還沒發射，黏在擋板上跟著移動！
            bk_ball_x = bk_paddle_x + (bk_paddle_w / 2) - 3;
            bk_ball_y = LCD_H * 0.9 - 6;
        } else {
            // 1. 移動球體
            bk_ball_x += bk_ball_dx;
            bk_ball_y += bk_ball_dy;

            // 2. 邊界反彈 (球寬高改為 6)
            if (bk_ball_x <= 0) {
                bk_ball_x = 0;
                bk_ball_dx = -bk_ball_dx;
            }
            if (bk_ball_x >= LCD_W - 6) {
                bk_ball_x = LCD_W - 6;
                bk_ball_dx = -bk_ball_dx;
            }  // 128 - 6
            if (bk_ball_y <= 16) {
                bk_ball_y = 16;
                bk_ball_dy = abs(bk_ball_dy);
            }  // 頂部保留給計分板

            // ==========================================
            // 【商業級防卡死保護】：防止球在頂部或橫向死循環
            // ==========================================
            static int anti_stuck_counter = 0;
            anti_stuck_counter++;

            // 如果球超過 8 秒 (80 次 tick) 都沒有回到擋板
            if (anti_stuck_counter > 80) {
                // 若球正往上飛或在頂部盤旋，強制讓它轉為向下
                if (bk_ball_dy < 0) {
                    bk_ball_dy = abs(bk_ball_dy);
                }
                anti_stuck_counter = 0;
                ESP_LOGI("Breakout", "觸發防卡死看門狗，引導球向下！");
            }

            // 3. 掉落底部 (Game Over)
            if (bk_ball_y >= LCD_H) {
                bk_game_state = BK_STATE_GAMEOVER;
                if (bk_score > bk_best_score) {
                    bk_best_score = bk_score;
                    save_bk_best_score(bk_best_score);
                }
                lv_label_set_text_fmt(bk_overlay_label, "GAME\nOVER\nScore:\n%d", bk_score);
                lv_obj_remove_flag(bk_overlay, LV_OBJ_FLAG_HIDDEN);
            }

            // 4. 擋板碰撞反彈 (球寬高 6)
            if (bk_ball_y + 6 >= LCD_H * 0.9 && bk_ball_y < LCD_H * 0.9 + 4 && bk_ball_dy > 0) {
                if (bk_ball_x + 6 >= bk_paddle_x && bk_ball_x <= bk_paddle_x + bk_paddle_w) {
                    bk_ball_dy = -abs(bk_ball_dy);  // 向上反彈
                    anti_stuck_counter = 0;         // ★ 成功接球，看門狗計時重置！
                    // 根據擊中擋板的位置，改變球的橫向軌跡 (增加遊戲性)
                    int hit_pos = (bk_ball_x + 3) - (bk_paddle_x + (bk_paddle_w / 2));
                    if (hit_pos < -3)
                        bk_ball_dx = -abs(bk_ball_dx);  // 擊中左側往左彈
                    if (hit_pos > 3)
                        bk_ball_dx = abs(bk_ball_dx);  // 擊中右側往右彈
                }
            }

            // 5. 磚塊碰撞偵測
            int remaining_bricks = 0;
            bool hit_brick = false;

            for (int r = 0; r < bk_active_rows; r++) {
                for (int c = 0; c < BK_COLS; c++) {
                    if (bk_bricks_active[r][c]) {
                        remaining_bricks++;
                        if (!hit_brick) {
                            int brx = BK_BRICK_START_X + c * BK_BRICK_W;
                            int bry = BK_BRICK_START_Y + r * BK_BRICK_STEP_Y;
                            // AABB 碰撞 (球寬高 6)
                            if (bk_ball_x < brx + 23 && bk_ball_x + 6 > brx &&
                                bk_ball_y < bry + 8 && bk_ball_y + 6 > bry) {
                                bk_bricks_active[r][c] = false;
                                lv_obj_add_flag(bk_bricks[r][c], LV_OBJ_FLAG_HIDDEN);
                                bk_ball_dy = -bk_ball_dy;  // 反彈
                                hit_brick = true;
                                remaining_bricks--;

                                bk_score += 10;
                                lv_label_set_text_fmt(bk_score_label, "L:%d  S:%d", bk_level,
                                                      bk_score);
                            }
                        }
                    }
                }
            }

            // 6. 過關判定
            if (remaining_bricks == 0) {
                bk_level++;
                init_breakout_level(false);  // 進入下一關，保留分數，難度提升！
                lv_label_set_text_fmt(bk_score_label, "L:%d  S:%d", bk_level, bk_score);
            }
        }

        // 更新球的位置 (不論發射與否都要更新)
        if (bk_ball)
            lv_obj_align(bk_ball, LV_ALIGN_TOP_LEFT, bk_ball_x, bk_ball_y);
    }

    if (current_ui_mode != 0)
        return;  // 如果在遊戲模式，不更新臉部表情

    // =======================================================
    // 觸摸喚醒的隨機表情展示與延遲啟動邏輯
    // =======================================================
    if (pending_touch_wake > 0) {
        if (pending_touch_wake == 15) {
            // 剛摸到的瞬間：隨機選取 6 種表情之一
            const char* random_emotions[] = {"happy",
                                             "loving",
                                             "silly",
                                             "angry",
                                             "thinking"
                                             "cool",
                                             "sleepy",
                                             "confused",
                                             "surprised"};
            int total_emotions = sizeof(random_emotions) / sizeof(random_emotions[0]);
            int rand_idx = esp_random() % total_emotions;
            CustomApp_SetEmotion(random_emotions[rand_idx]);
            ESP_LOGI("Touch", "隨機展示表情: %s (idx: %d/%d)", random_emotions[rand_idx], rand_idx,
                     total_emotions);
            auto backlight = Board::GetInstance().GetBacklight();
            if (backlight)
                backlight->SetBrightness(100);
        }

        pending_touch_wake = pending_touch_wake - 1;

        if (pending_touch_wake == 0) {
            // 1.5 秒展示結束，真正觸發 AI 麥克風聆聽！
            if (ai_single_click_cb) {
                ai_single_click_cb();
            }
        }
        // 直接返回，在倒數的這 1.5 秒內不執行下方的常規臉部更新，確保表情不會被洗掉
        return;
    }

    DeviceState current_state = Application::GetInstance().GetDeviceState();
    auto backlight = Board::GetInstance().GetBacklight();

    // 1. 自動進入時間模式，閒置計時器：只要處於待機 (Idle)，每次 tick (100ms) 都要累加
    if (current_state == kDeviceStateIdle) {
        idle_to_clock_counter++;
        // 🌟 核心修改：判斷目前是不是塔羅模式
        // 如果是，設定為 9000 (15分鐘)；如果不是，設定為 1800 (3分鐘)
        int timeout_limit = is_tarot_mode ? 9000 : 1800;

        if (idle_to_clock_counter >= timeout_limit) {
            idle_to_clock_counter = 0;
            if (is_tarot_mode) {
                ESP_LOGI("System", "15 分鐘無操作，自動進入時鐘待機模式");
                is_tarot_mode = false;  // 超時後自動解除塔羅模式鎖定
            } else {
                ESP_LOGI("System", "3 分鐘無操作，自動進入時鐘待機模式");
            }

            current_system_mode = 3;
            current_ui_mode = 3;
            ui_mode_changed = true;

            // 將背光降至 2% (極致省電)
            if (backlight)
                backlight->SetBrightness(100);
            return;  // 立即跳轉，不執行後續
        }
    } else {
        // 如果不是 Idle (正在講話或聆聽)，將閒置計時歸零
        idle_to_clock_counter = 0;
    }

    // 2. 只有狀態發生「變化」時，才重新建立/切換表情動畫
    if (current_state == last_avatar_state)
        return;

    last_avatar_state = current_state;

    // 清除舊動畫
    lv_anim_del(avatar_mouth, NULL);
    lv_anim_del(avatar_left_eye, NULL);
    lv_anim_del(avatar_right_eye, NULL);

    if (current_state == kDeviceStateListening) {
        lv_obj_set_size(avatar_left_eye, EYE_BASE_W, EYE_BASE_H);
        lv_obj_align(avatar_left_eye, LV_ALIGN_CENTER, EYE_POS_L, EYE_POS_H);
        lv_obj_set_style_radius(avatar_left_eye, EYE_CORNER, 0);
        lv_obj_set_size(avatar_right_eye, EYE_BASE_W, EYE_BASE_W);
        lv_obj_align(avatar_right_eye, LV_ALIGN_CENTER, EYE_POS_R, EYE_POS_H);
        lv_obj_set_style_radius(avatar_right_eye, EYE_CORNER, 0);
        lv_obj_set_size(avatar_mouth, MOUTH_BASE_W - 4, MOUTH_BASE_H);
        lv_obj_align(avatar_mouth, LV_ALIGN_CENTER, 0, MOUTH_POS_H);
        lv_obj_set_style_radius(avatar_mouth, 8, 0);

        if (backlight)
            backlight->SetBrightness(100);
    } else if (current_state == kDeviceStateSpeaking) {
        if (backlight)
            backlight->SetBrightness(100);

        if (current_active_emotion != "laughing" || current_active_emotion != "happy") {
            if (avatar_mouth_laugh)
                lv_obj_add_flag(avatar_mouth_laugh, LV_OBJ_FLAG_HIDDEN);
            if (avatar_mouth_happy)
                lv_obj_add_flag(avatar_mouth_happy, LV_OBJ_FLAG_HIDDEN);
            set_eye_size(EYE_BASE_W, EYE_BASE_H);
            lv_obj_align(avatar_left_eye, LV_ALIGN_CENTER, EYE_POS_L, EYE_POS_H);
            lv_obj_set_style_radius(avatar_left_eye, EYE_CORNER, 0);
            lv_obj_align(avatar_right_eye, LV_ALIGN_CENTER, EYE_POS_R, EYE_POS_H);
            lv_obj_set_style_radius(avatar_right_eye, EYE_CORNER, 0);
            lv_obj_remove_flag(avatar_mouth, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_size(avatar_mouth, MOUTH_BASE_W - 4, MOUTH_BASE_H);
            lv_obj_set_style_radius(avatar_mouth, 8, 0);
            lv_obj_align(avatar_mouth, LV_ALIGN_CENTER, 0, MOUTH_POS_H);

            // 啟動一字嘴開合
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, avatar_mouth);
            lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_height);
            lv_anim_set_values(&a, 8, 28);
            lv_anim_set_duration(&a, 150);
            lv_anim_set_playback_time(&a, 150);
            lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
            lv_anim_start(&a);

            start_blink_animation();
        } else {
            lv_obj_add_flag(avatar_mouth, LV_OBJ_FLAG_HIDDEN);
        }

    } else {  // 進入待機狀態 (首次切換進來時執行一次)
        if (backlight)
            backlight->SetBrightness(100);

        clock_is_bright = true;
        clock_dim_countdown = CLOCK_DIM_TIMEOUT_SEC * 10;

        current_active_emotion = "neutral";  // 重置回普通待機
        set_eye_size(EYE_BASE_W, EYE_BASE_H);
        lv_obj_set_style_radius(avatar_left_eye, EYE_CORNER, 0);
        lv_obj_set_style_radius(avatar_right_eye, EYE_CORNER, 0);

        lv_obj_remove_flag(avatar_mouth, LV_OBJ_FLAG_HIDDEN);
        if (avatar_sad_eye)
            lv_obj_add_flag(avatar_sad_eye, LV_OBJ_FLAG_HIDDEN);
        if (avatar_surprised_eye)
            lv_obj_add_flag(avatar_surprised_eye, LV_OBJ_FLAG_HIDDEN);
        if (avatar_angry_eye)
            lv_obj_add_flag(avatar_angry_eye, LV_OBJ_FLAG_HIDDEN);
        if (avatar_anger)
            lv_obj_add_flag(avatar_anger, LV_OBJ_FLAG_HIDDEN);
        if (avatar_embrassing)
            lv_obj_add_flag(avatar_embrassing, LV_OBJ_FLAG_HIDDEN);
        if (avatar_heart)
            lv_obj_add_flag(avatar_heart, LV_OBJ_FLAG_HIDDEN);
        if (avatar_kissy)
            lv_obj_add_flag(avatar_kissy, LV_OBJ_FLAG_HIDDEN);
        if (avatar_sweat)
            lv_obj_add_flag(avatar_sweat, LV_OBJ_FLAG_HIDDEN);
        if (avatar_mouth_laugh)
            lv_obj_add_flag(avatar_mouth_laugh, LV_OBJ_FLAG_HIDDEN);
        if (avatar_silly_tongue)
            lv_obj_add_flag(avatar_silly_tongue, LV_OBJ_FLAG_HIDDEN);
        if (avatar_mouth_happy)
            lv_obj_add_flag(avatar_mouth_happy, LV_OBJ_FLAG_HIDDEN);
        if (avatar_cool)
            lv_obj_add_flag(avatar_cool, LV_OBJ_FLAG_HIDDEN);
        if (avatar_tears)
            lv_obj_add_flag(avatar_tears, LV_OBJ_FLAG_HIDDEN);
        if (avatar_swallow)
            lv_obj_add_flag(avatar_swallow, LV_OBJ_FLAG_HIDDEN);
        if (avatar_confuse)
            lv_obj_add_flag(avatar_confuse, LV_OBJ_FLAG_HIDDEN);
        if (avatar_sleepy_z)
            lv_obj_add_flag(avatar_sleepy_z, LV_OBJ_FLAG_HIDDEN);

        lv_obj_set_size(avatar_mouth, MOUTH_BASE_W, MOUTH_BASE_H);
        lv_obj_align(avatar_mouth, LV_ALIGN_CENTER, 0, MOUTH_POS_H);
        lv_obj_set_style_radius(avatar_mouth, 8, 0);

        start_eye_snore_animation();
        start_mouth_animation();
    }
}

// --- 鉤子 1：初始化 UI ---
void CustomApp_InitUI(lv_obj_t* parent) {
    srand(esp_random());
    // 設定系統時區為香港時間 (UTC+8)
    setenv("TZ", "HKT-8", 1);
    tzset();

    avatar_theme_bg_color = lv_obj_get_style_bg_color(parent, LV_PART_MAIN);  // 白色
    // avatar_theme_color = lv_obj_get_style_text_color(parent, LV_PART_MAIN); //黑色
    lv_obj_set_style_text_color(parent, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    avatar_theme_color = lv_color_make(255, 100, 0);

    // ==============================================
    // 【新增】建立一個臉部總容器，用來做重力跟隨移動
    // ==============================================
    face_container = lv_obj_create(parent);
    lv_obj_set_size(face_container, LCD_W, LCD_H);
    lv_obj_set_style_bg_opa(face_container, LV_OPA_TRANSP, 0);  // 容器必須透明
    lv_obj_set_style_border_width(face_container, 0, 0);
    lv_obj_set_style_pad_all(face_container, 0, 0);
    lv_obj_clear_flag(face_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(face_container, LV_ALIGN_CENTER, 0, 0);

    // 設定眼睛和嘴巴的初始位置
    avatar_left_eye = lv_obj_create(face_container);
    lv_obj_set_size(avatar_left_eye, EYE_BASE_W, EYE_BASE_H);
    lv_obj_set_style_radius(avatar_left_eye, EYE_CORNER, 0);
    lv_obj_set_style_bg_color(avatar_left_eye, avatar_theme_color, 0);
    lv_obj_set_style_pad_all(avatar_left_eye, 0, 0);
    lv_obj_set_style_border_width(avatar_left_eye, 0, 0);
    lv_obj_clear_flag(avatar_left_eye, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(avatar_left_eye, LV_ALIGN_CENTER, EYE_POS_L, EYE_POS_H);

    left_eye_highlight = lv_obj_create(avatar_left_eye);
    // 【修改 1】使用百分比：寬度佔眼睛 35%，高度佔 40% (不管眼睛怎麼縮放，高光自動跟著縮放！)
    lv_obj_set_size(left_eye_highlight, lv_pct(40), lv_pct(40));
    lv_obj_set_style_bg_color(left_eye_highlight, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(left_eye_highlight, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(left_eye_highlight, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(left_eye_highlight, 0, 0);
    // 【修改 2】對齊右上角，稍微往內縮 (X: -2, Y: 2) 會更有神
    lv_obj_align(left_eye_highlight, LV_ALIGN_TOP_RIGHT, -2, 4);
    lv_obj_clear_flag(left_eye_highlight, LV_OBJ_FLAG_SCROLLABLE);

    avatar_right_eye = lv_obj_create(face_container);
    lv_obj_set_size(avatar_right_eye, EYE_BASE_W, EYE_BASE_H);
    lv_obj_set_style_radius(avatar_right_eye, EYE_CORNER, 0);
    lv_obj_set_style_bg_color(avatar_right_eye, avatar_theme_color, 0);
    lv_obj_set_style_pad_all(avatar_right_eye, 0, 0);
    lv_obj_set_style_border_width(avatar_right_eye, 0, 0);
    lv_obj_clear_flag(avatar_right_eye, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(avatar_right_eye, LV_ALIGN_CENTER, EYE_POS_R, EYE_POS_H);

    right_eye_highlight = lv_obj_create(avatar_right_eye);
    // 同樣設定為百分比
    lv_obj_set_size(right_eye_highlight, lv_pct(40), lv_pct(40));
    lv_obj_set_style_bg_color(right_eye_highlight, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(right_eye_highlight, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(right_eye_highlight, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(right_eye_highlight, 0, 0);
    // 右眼也是對齊右上角 (高光的反光方向通常要一致，才像同一個光源)
    lv_obj_align(right_eye_highlight, LV_ALIGN_TOP_RIGHT, -2, 4);
    lv_obj_clear_flag(right_eye_highlight, LV_OBJ_FLAG_SCROLLABLE);

    avatar_mouth = lv_obj_create(face_container);
    lv_obj_set_size(avatar_mouth, MOUTH_BASE_W, MOUTH_BASE_H);
    lv_obj_set_style_radius(avatar_mouth, 8, 0);
    lv_obj_set_style_bg_color(avatar_mouth, avatar_theme_color, 0);
    lv_obj_clear_flag(avatar_mouth, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(avatar_mouth, LV_ALIGN_CENTER, 0, MOUTH_POS_H);

    // ==============================================
    // sad eye
    // ==============================================
    avatar_sad_eye = lv_obj_create(face_container);
    lv_obj_set_size(avatar_sad_eye, LCD_W, LCD_H);
    lv_obj_set_style_bg_opa(avatar_sad_eye, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(avatar_sad_eye, 0, 0);
    lv_obj_set_style_pad_all(avatar_sad_eye, 0, 0);
    lv_obj_clear_flag(avatar_sad_eye, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(avatar_sad_eye, LV_ALIGN_CENTER, 0, 0);

    // ---------------- 左眼遮罩 ----------------
    lv_obj_t* sad_left = lv_obj_create(avatar_sad_eye);
    lv_obj_set_size(sad_left, 36, 20);
    lv_obj_set_style_bg_color(sad_left, avatar_theme_bg_color, 0);
    lv_obj_set_style_bg_opa(sad_left, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(sad_left, 0, 0);
    lv_obj_set_style_border_width(sad_left, 0, 0);

    // 將旋轉支點設定在方塊正中心 (寬36的一半=18, 高20的一半=10)
    lv_obj_set_style_transform_pivot_x(sad_left, 18, 0);
    lv_obj_set_style_transform_pivot_y(sad_left, 10, 0);

    lv_obj_set_style_transform_angle(sad_left, -150, 0);  // 逆時針 15 度
    lv_obj_clear_flag(sad_left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(sad_left, LV_ALIGN_CENTER, EYE_POS_L, EYE_POS_H - 20);  // 完美對稱座標

    // ---------------- 右眼遮罩 ----------------
    lv_obj_t* sad_right = lv_obj_create(avatar_sad_eye);
    lv_obj_set_size(sad_right, 36, 20);
    lv_obj_set_style_bg_color(sad_right, avatar_theme_bg_color, 0);
    lv_obj_set_style_bg_opa(sad_right, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(sad_right, 0, 0);
    lv_obj_set_style_border_width(sad_right, 0, 0);

    // 將旋轉支點設定在方塊正中心
    lv_obj_set_style_transform_pivot_x(sad_right, 18, 0);
    lv_obj_set_style_transform_pivot_y(sad_right, 10, 0);

    lv_obj_set_style_transform_angle(sad_right, 150, 0);  // 順時針 15 度
    lv_obj_clear_flag(sad_right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(sad_right, LV_ALIGN_CENTER, EYE_POS_R, EYE_POS_H - 20);

    lv_obj_add_flag(avatar_sad_eye, LV_OBJ_FLAG_HIDDEN);

    // ==============================================
    // surprised eye
    // ==============================================
    avatar_surprised_eye = lv_obj_create(face_container);
    lv_obj_set_size(avatar_surprised_eye, LCD_W, 12);
    lv_obj_set_style_bg_opa(avatar_surprised_eye, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(avatar_surprised_eye, 0, 0);
    lv_obj_set_style_pad_all(avatar_surprised_eye, 0, 0);
    lv_obj_clear_flag(avatar_surprised_eye, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(avatar_surprised_eye, LV_ALIGN_CENTER, 0, EYE_POS_H - EYE_BASE_H + 4);

    // ---------------- 左眼遮罩 ----------------
    lv_obj_t* surprised_left = lv_obj_create(avatar_surprised_eye);
    lv_obj_set_size(surprised_left, 14, 6);
    lv_obj_set_style_radius(surprised_left, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(surprised_left, avatar_theme_color, 0);
    lv_obj_set_style_bg_opa(surprised_left, LV_OPA_COVER, 60);
    lv_obj_set_style_radius(surprised_left, 0, 0);
    lv_obj_set_style_border_width(surprised_left, 0, 0);

    // 將旋轉支點設定在方塊正中心 (寬30的一半=15, 高20的一半=10)
    lv_obj_set_style_transform_pivot_x(surprised_left, 7, 0);
    lv_obj_set_style_transform_pivot_y(surprised_left, 3, 0);

    lv_obj_set_style_transform_angle(surprised_left, -60, 0);  // 逆時針 8 度
    lv_obj_clear_flag(surprised_left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(surprised_left, LV_ALIGN_CENTER, EYE_POS_L, 0);  // 完美對稱座標

    // ---------------- 右眼遮罩 ----------------
    lv_obj_t* surprised_right = lv_obj_create(avatar_surprised_eye);
    lv_obj_set_size(surprised_right, 14, 6);
    lv_obj_set_style_radius(surprised_right, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(surprised_right, avatar_theme_color, 0);
    lv_obj_set_style_bg_opa(surprised_right, LV_OPA_COVER, 60);
    lv_obj_set_style_radius(surprised_right, 0, 0);
    lv_obj_set_style_border_width(surprised_right, 0, 0);

    // 將旋轉支點設定在方塊正中心
    lv_obj_set_style_transform_pivot_x(surprised_right, 7, 0);
    lv_obj_set_style_transform_pivot_y(surprised_right, 3, 0);

    lv_obj_set_style_transform_angle(surprised_right, 60, 0);  // 順時針 8 度
    lv_obj_clear_flag(surprised_right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(surprised_right, LV_ALIGN_CENTER, EYE_POS_R, 0);

    lv_obj_add_flag(avatar_surprised_eye, LV_OBJ_FLAG_HIDDEN);

    // ==============================================
    // angry eye
    // ==============================================
    avatar_angry_eye = lv_obj_create(face_container);
    lv_obj_set_size(avatar_angry_eye, LCD_W, LCD_H);
    lv_obj_set_style_bg_opa(avatar_angry_eye, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(avatar_angry_eye, 0, 0);
    lv_obj_set_style_pad_all(avatar_angry_eye, 0, 0);
    lv_obj_clear_flag(avatar_angry_eye, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(avatar_angry_eye, LV_ALIGN_CENTER, 0, 0);

    // ---------------- 左眼遮罩 ----------------
    lv_obj_t* angry_left = lv_obj_create(avatar_angry_eye);
    lv_obj_set_size(angry_left, 36, 20);
    lv_obj_set_style_bg_color(angry_left, avatar_theme_bg_color, 0);
    lv_obj_set_style_bg_opa(angry_left, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(angry_left, 0, 0);
    lv_obj_set_style_border_width(angry_left, 0, 0);

    // 將旋轉支點設定在方塊正中心 (寬30的一半=15, 高20的一半=10)
    lv_obj_set_style_transform_pivot_x(angry_left, 18, 0);
    lv_obj_set_style_transform_pivot_y(angry_left, 10, 0);

    lv_obj_set_style_transform_angle(angry_left, 150, 0);  // 逆時針 15 度
    lv_obj_clear_flag(angry_left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(angry_left, LV_ALIGN_CENTER, EYE_POS_L, EYE_POS_H - 20);

    // ---------------- 右眼遮罩 ----------------
    lv_obj_t* angry_right = lv_obj_create(avatar_angry_eye);
    lv_obj_set_size(angry_right, 36, 20);
    lv_obj_set_style_bg_color(angry_right, avatar_theme_bg_color, 0);
    lv_obj_set_style_bg_opa(angry_right, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(angry_right, 0, 0);
    lv_obj_set_style_border_width(angry_right, 0, 0);

    // 將旋轉支點設定在方塊正中心
    lv_obj_set_style_transform_pivot_x(angry_right, 18, 0);
    lv_obj_set_style_transform_pivot_y(angry_right, 10, 0);

    lv_obj_set_style_transform_angle(angry_right, -150, 0);  // 順時針 15 度
    lv_obj_clear_flag(angry_right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(angry_right, LV_ALIGN_CENTER, EYE_POS_R, EYE_POS_H - 20);
    lv_obj_add_flag(avatar_angry_eye, LV_OBJ_FLAG_HIDDEN);

    // ==============================================
    // 建立「大哭 (Crying)」的藍色眼淚 (😭)
    // ==============================================
    // 1. 建立透明母圖層
    avatar_tears = lv_obj_create(face_container);
    lv_obj_set_size(avatar_tears, LCD_W, LCD_H);
    lv_obj_set_style_bg_opa(avatar_tears, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(avatar_tears, 0, 0);
    lv_obj_set_style_pad_all(avatar_tears, 0, 0);
    lv_obj_clear_flag(avatar_tears, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(avatar_tears, LV_ALIGN_CENTER, 0, 0);

    lv_color_t tear_blue = lv_color_hex(0x33AAFF);  // 水藍色

    // 2. 左眼淚水 (位於左眼下方)
    lv_obj_t* tear_left = lv_obj_create(avatar_tears);
    lv_obj_set_size(tear_left, 8, 18);  // 寬8，高18的長條淚柱
    lv_obj_set_style_bg_color(tear_left, tear_blue, 0);
    lv_obj_set_style_bg_opa(tear_left, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(tear_left, 4, 0);  // 圓潤水滴感
    lv_obj_set_style_border_width(tear_left, 0, 0);
    lv_obj_clear_flag(tear_left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(tear_left, LV_ALIGN_CENTER, EYE_POS_L, 6);  // 對齊左眼下方

    // 3. 右眼淚水 (位於右眼下方)
    lv_obj_t* tear_right = lv_obj_create(avatar_tears);
    lv_obj_set_size(tear_right, 8, 18);
    lv_obj_set_style_bg_color(tear_right, tear_blue, 0);
    lv_obj_set_style_bg_opa(tear_right, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(tear_right, 4, 0);
    lv_obj_set_style_border_width(tear_right, 0, 0);
    lv_obj_clear_flag(tear_right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(tear_right, LV_ALIGN_CENTER, EYE_POS_R, 6);  // 對齊右眼下方

    lv_obj_add_flag(avatar_tears, LV_OBJ_FLAG_HIDDEN);  // 平常隱藏

    // ==============================================
    // 建立「口水 (swallow)」
    // ==============================================
    // 1. 建立透明母圖層
    avatar_swallow = lv_obj_create(face_container);
    lv_obj_set_size(avatar_swallow, LCD_W, LCD_H);
    lv_obj_set_style_bg_opa(avatar_swallow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(avatar_swallow, 0, 0);
    lv_obj_set_style_pad_all(avatar_swallow, 0, 0);
    lv_obj_clear_flag(avatar_swallow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(avatar_swallow, LV_ALIGN_CENTER, 0, 0);

    lv_color_t swallow_blue = lv_color_hex(0x33AAFF);  // 水藍色

    // 2. 左眼淚水 (位於左眼下方)
    lv_obj_t* swallow_left = lv_obj_create(avatar_swallow);
    lv_obj_set_size(swallow_left, 8, 16);  // 寬8，高18的長條淚柱
    lv_obj_set_style_bg_color(swallow_left, swallow_blue, 0);
    lv_obj_set_style_bg_opa(swallow_left, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(swallow_left, 4, 0);  // 圓潤水滴感
    lv_obj_set_style_border_width(swallow_left, 0, 0);
    lv_obj_clear_flag(swallow_left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(swallow_left, LV_ALIGN_CENTER, 6, MOUTH_POS_H + 3);

    lv_obj_add_flag(avatar_swallow, LV_OBJ_FLAG_HIDDEN);  // 平常隱藏

    // ==============================================
    // 建立Confuse眼睛
    // ==============================================
    avatar_confuse = lv_obj_create(face_container);
    lv_obj_set_size(avatar_confuse, LCD_W, 10);
    lv_obj_set_style_bg_opa(avatar_confuse, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(avatar_confuse, 0, 0);
    lv_obj_set_style_pad_all(avatar_confuse, 0, 0);
    lv_obj_clear_flag(avatar_confuse, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(avatar_confuse, LV_ALIGN_CENTER, 0, EYE_POS_H + 1);

    lv_obj_t* confuse_eye_l = lv_obj_create(avatar_confuse);
    lv_obj_set_size(confuse_eye_l, 12, 16);
    lv_obj_set_style_bg_color(confuse_eye_l, avatar_theme_color, 0);
    lv_obj_set_style_border_width(confuse_eye_l, 0, 0);  // 修正：確保邊框為 0
    lv_obj_set_style_radius(confuse_eye_l, LV_RADIUS_CIRCLE, 0);
    lv_obj_align(confuse_eye_l, LV_ALIGN_BOTTOM_MID, EYE_POS_L + 6, 0);
    lv_obj_clear_flag(confuse_eye_l, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* confuse_eye_r = lv_obj_create(avatar_confuse);
    lv_obj_set_size(confuse_eye_r, 12, 16);
    lv_obj_set_style_bg_color(confuse_eye_r, avatar_theme_color, 0);
    lv_obj_set_style_border_width(confuse_eye_r, 0, 0);  // 修正：確保邊框為 0
    lv_obj_set_style_radius(confuse_eye_r, LV_RADIUS_CIRCLE, 0);
    lv_obj_align(confuse_eye_r, LV_ALIGN_BOTTOM_MID, EYE_POS_R + 6, 0);
    lv_obj_clear_flag(confuse_eye_r, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_add_flag(avatar_confuse, LV_OBJ_FLAG_HIDDEN);  // 平常隱藏

    // ==============================================
    // 建立cool的表情
    // ==============================================
    avatar_cool = lv_obj_create(face_container);
    lv_obj_set_size(avatar_cool, LCD_W, LCD_H);
    lv_obj_set_style_bg_opa(avatar_cool, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(avatar_cool, 0, 0);
    lv_obj_set_style_pad_all(avatar_cool, 0, 0);
    lv_obj_clear_flag(avatar_cool, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(avatar_cool, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t* cool_left = lv_obj_create(avatar_cool);
    lv_obj_set_size(cool_left, 35, 20);
    lv_obj_set_style_bg_color(cool_left, avatar_theme_bg_color, 0);
    lv_obj_set_style_bg_opa(cool_left, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(cool_left, 0, 0);
    lv_obj_set_style_border_width(cool_left, 0, 0);

    // 將旋轉支點設定在方塊正中心 (寬30的一半=15, 高20的一半=10)
    lv_obj_set_style_transform_pivot_x(cool_left, 15, 0);
    lv_obj_set_style_transform_pivot_y(cool_left, 10, 0);

    lv_obj_set_style_transform_angle(cool_left, 20, 0);  // 逆時針 2 度
    lv_obj_clear_flag(cool_left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(cool_left, LV_ALIGN_CENTER, EYE_POS_L, EYE_POS_H - 20);

    // ================= 左眼反光點 (Highlight) =================
    lv_obj_t* cool_l_highlight = lv_obj_create(avatar_cool);
    lv_obj_set_size(cool_l_highlight, 6, 8);
    lv_obj_set_style_bg_color(cool_l_highlight, avatar_theme_bg_color, 0);
    lv_obj_set_style_border_width(cool_l_highlight, 0, 0);
    lv_obj_set_style_pad_all(cool_l_highlight, 0, 0);
    lv_obj_set_style_radius(cool_l_highlight, 3, 0);
    // 總容器寬 88，左鏡片寬 42，所以從最左邊往右移 14px，剛好是左鏡片的左上側
    lv_obj_align(cool_l_highlight, LV_ALIGN_CENTER, EYE_POS_L + 2, EYE_POS_H - 4);
    lv_obj_clear_flag(cool_l_highlight, LV_OBJ_FLAG_SCROLLABLE);

    // ---------------- 右眼遮罩 ----------------
    lv_obj_t* cool_right = lv_obj_create(avatar_cool);
    lv_obj_set_size(cool_right, 35, 20);
    lv_obj_set_style_bg_color(cool_right, avatar_theme_bg_color, 0);
    lv_obj_set_style_bg_opa(cool_right, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(cool_right, 0, 0);
    lv_obj_set_style_border_width(cool_right, 0, 0);

    // 將旋轉支點設定在方塊正中心
    lv_obj_set_style_transform_pivot_x(cool_right, 15, 0);
    lv_obj_set_style_transform_pivot_y(cool_right, 10, 0);

    lv_obj_set_style_transform_angle(cool_right, -20, 0);  // 順時針 2 度
    lv_obj_clear_flag(cool_right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(cool_right, LV_ALIGN_CENTER, EYE_POS_R, EYE_POS_H - 20);

    // ================= 右眼反光點 (Highlight) =================
    lv_obj_t* cool_r_highlight = lv_obj_create(avatar_cool);
    lv_obj_set_size(cool_r_highlight, 6, 8);
    lv_obj_set_style_bg_color(cool_r_highlight, avatar_theme_bg_color, 0);
    lv_obj_set_style_border_width(cool_r_highlight, 0, 0);
    lv_obj_set_style_pad_all(cool_r_highlight, 0, 0);
    lv_obj_set_style_radius(cool_r_highlight, 3, 0);
    lv_obj_align(cool_r_highlight, LV_ALIGN_CENTER, EYE_POS_R + 2, EYE_POS_H - 4);
    lv_obj_clear_flag(cool_r_highlight, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_add_flag(avatar_cool, LV_OBJ_FLAG_HIDDEN);

    // ==============================================
    // 建立開口笑嘴巴 (laughing)
    // ==============================================
    // 1. 父容器 (高度只有 30px，超出部分會被自動裁切)
    avatar_mouth_laugh = lv_obj_create(face_container);
    lv_obj_set_size(avatar_mouth_laugh, 28, 30);
    lv_obj_set_style_bg_opa(avatar_mouth_laugh, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(avatar_mouth_laugh, 0, 0);
    lv_obj_set_style_pad_all(avatar_mouth_laugh, 0, 0);
    lv_obj_clear_flag(avatar_mouth_laugh, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(avatar_mouth_laugh, LV_ALIGN_CENTER, 0, MOUTH_POS_H);  // 嘴巴位置

    // 2. 下半部：圓弧碗底 (提供圓潤飽滿的下巴弧度)
    lv_obj_t* mouth_bottom_curve = lv_obj_create(avatar_mouth_laugh);
    lv_obj_set_size(mouth_bottom_curve, 28, 28);
    lv_obj_set_style_radius(mouth_bottom_curve, LV_RADIUS_CIRCLE, 0);      // 底部全圓弧
    lv_obj_set_style_bg_color(mouth_bottom_curve, avatar_theme_color, 0);  // 同色融合
    lv_obj_set_style_border_width(mouth_bottom_curve, 0, 0);
    lv_obj_clear_flag(mouth_bottom_curve, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(mouth_bottom_curve, LV_ALIGN_BOTTOM_MID, 0, 0);  // 貼齊底部

    // 3. 上半部：純直角長方形 (寬 28，高 8) -> 【這就是你要的方形上半部！】
    // 它會蓋住下半部的圓弧頂角，形成筆直的兩側與 90 度平整開口！
    lv_obj_t* mouth_top_square = lv_obj_create(avatar_mouth_laugh);
    lv_obj_set_size(mouth_top_square, 28, 16);
    lv_obj_set_style_radius(mouth_top_square, 0, 0);                     // 【關鍵】純直角，無圓角！
    lv_obj_set_style_bg_color(mouth_top_square, avatar_theme_color, 0);  // 同色融合
    lv_obj_set_style_border_width(mouth_top_square, 0, 0);
    lv_obj_clear_flag(mouth_top_square, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(mouth_top_square, LV_ALIGN_TOP_MID, 0, 0);  // 貼齊頂部

    lv_obj_add_flag(avatar_mouth_laugh, LV_OBJ_FLAG_HIDDEN);

    // ==============================================
    // 建立silly舌頭
    // ==============================================
    avatar_silly_tongue = lv_obj_create(face_container);
    lv_obj_set_size(avatar_silly_tongue, 16, 18);
    lv_obj_set_style_bg_opa(avatar_silly_tongue, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(avatar_silly_tongue, 0, 0);
    lv_obj_set_style_pad_all(avatar_silly_tongue, 0, 0);
    lv_obj_clear_flag(avatar_silly_tongue, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(avatar_silly_tongue, LV_ALIGN_CENTER, 0, MOUTH_POS_H + 10);

    // 2. 下半部：圓弧碗底 (提供圓潤飽滿的下巴弧度)
    lv_obj_t* tongue_curve = lv_obj_create(avatar_silly_tongue);
    lv_obj_set_size(tongue_curve, 16, 36);
    lv_obj_set_style_radius(tongue_curve, 8, 0);                         // 底部全圓弧
    lv_obj_set_style_bg_color(tongue_curve, lv_color_hex(0xFF9999), 0);  // 同色融合
    lv_obj_set_style_border_width(tongue_curve, 0, 0);
    lv_obj_clear_flag(tongue_curve, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(tongue_curve, LV_ALIGN_BOTTOM_MID, 0, 0);  // 貼齊底部

    lv_obj_add_flag(avatar_silly_tongue, LV_OBJ_FLAG_HIDDEN);

    // ==============================================
    // 建立 Zzz 容器 (放在螢幕左上方)
    // ==============================================
    avatar_sleepy_z = lv_obj_create(face_container);
    lv_obj_set_size(avatar_sleepy_z, 22, 22);
    lv_obj_set_style_bg_opa(avatar_sleepy_z, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(avatar_sleepy_z, 0, 0);
    lv_obj_set_style_pad_all(avatar_sleepy_z, 0, 0);
    lv_obj_clear_flag(avatar_sleepy_z, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(avatar_sleepy_z, LV_ALIGN_CENTER, EYE_POS_R + (EYE_BASE_W - 6),
                 EYE_POS_H - (EYE_BASE_H - 10));

    // 大 Z (靠右上一點)
    lv_obj_t* label_z_big = lv_label_create(avatar_sleepy_z);
    lv_label_set_text(label_z_big, "Z");
    lv_obj_set_style_text_color(label_z_big, avatar_theme_color, 0);
    lv_obj_set_style_text_opa(label_z_big, LV_OPA_100, 0);
    lv_obj_align(label_z_big, LV_ALIGN_BOTTOM_LEFT, 0, 4);

    // 小 z (靠左下一點，稍微半透明)
    lv_obj_t* label_z_small = lv_label_create(avatar_sleepy_z);
    lv_label_set_text(label_z_small, "z");
    lv_obj_set_style_text_color(label_z_small, avatar_theme_color, 0);
    lv_obj_set_style_text_opa(label_z_small, LV_OPA_100, 0);
    lv_obj_align(label_z_small, LV_ALIGN_TOP_RIGHT, 0, -4);

    // 預設隱藏
    lv_obj_add_flag(avatar_sleepy_z, LV_OBJ_FLAG_HIDDEN);

    // ==============================================
    // 建立開口笑嘴巴 (半圓) happy
    // ==============================================
    // 1. 外層父容器：寬度設為半徑的 2 倍，高度設為半徑 (等於只露出下半部)
    avatar_mouth_happy = lv_obj_create(face_container);
    lv_obj_set_size(avatar_mouth_happy, 24, 19);                    // 寬 38，高 19 (剛好半圓比例)
    lv_obj_set_style_bg_opa(avatar_mouth_happy, LV_OPA_TRANSP, 0);  // 容器本身透明
    lv_obj_set_style_border_width(avatar_mouth_happy, 0, 0);
    lv_obj_set_style_pad_all(avatar_mouth_happy, 0, 0);
    lv_obj_clear_flag(avatar_mouth_happy, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(avatar_mouth_happy, LV_ALIGN_CENTER, 0, MOUTH_POS_H);  // 嘴巴位置

    // 2. 內部正圓球：放一個 30x30 的正圓，靠頂部對齊，下半部剛好露出來
    lv_obj_t* circle = lv_obj_create(avatar_mouth_happy);
    lv_obj_set_size(circle, 24, 38);                       // 完整正圓尺寸
    lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);  // 設為全圓
    lv_obj_set_style_bg_color(circle, avatar_theme_color, 0);
    lv_obj_set_style_border_width(circle, 0, 0);
    lv_obj_clear_flag(circle, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(circle, LV_ALIGN_BOTTOM_MID, 0, 0);  // 頂部對齊，超出容器下方的會被裁切

    lv_obj_add_flag(avatar_mouth_happy, LV_OBJ_FLAG_HIDDEN);

    // ==============================================
    // embrassing face
    // ==============================================
    avatar_embrassing = lv_obj_create(face_container);
    lv_obj_set_size(avatar_embrassing, LCD_W, 12);
    lv_obj_set_style_bg_opa(avatar_embrassing, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(avatar_embrassing, 0, 0);
    lv_obj_set_style_pad_all(avatar_embrassing, 0, 0);
    lv_obj_clear_flag(avatar_embrassing, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(avatar_embrassing, LV_ALIGN_CENTER, 0, 0);

    lv_color_t blush_pink = lv_color_hex(0xFFAAAA);  // 柔和粉紅色 (若想深一點可用 0xFF7799)

    // 2. 左臉頰粉紅橢圓 (位於左眼下方偏左)
    lv_obj_t* embrassing_left = lv_obj_create(avatar_embrassing);
    lv_obj_set_size(embrassing_left, 16, 12);
    lv_obj_set_style_radius(embrassing_left, LV_RADIUS_CIRCLE, 0);  // 橢圓形
    lv_obj_set_style_bg_color(embrassing_left, blush_pink,
                              0);                                // 【關鍵】實體顏色設定在子物件上
    lv_obj_set_style_bg_opa(embrassing_left, LV_OPA_COVER, 20);  // 100% 不透明
    lv_obj_set_style_border_width(embrassing_left, 0, 0);
    lv_obj_clear_flag(embrassing_left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(embrassing_left, LV_ALIGN_CENTER, EYE_POS_L - EYE_BASE_W + 8,
                 0);  // 左臉頰座標

    // 3. 右臉頰粉紅橢圓 (位於右眼下方偏右)
    lv_obj_t* embrassing_right = lv_obj_create(avatar_embrassing);
    lv_obj_set_size(embrassing_right, 16, 12);
    lv_obj_set_style_radius(embrassing_right, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(embrassing_right, blush_pink,
                              0);  // 【關鍵】實體顏色設定在子物件上
    lv_obj_set_style_bg_opa(embrassing_right, LV_OPA_COVER, 20);
    lv_obj_set_style_border_width(embrassing_right, 0, 0);
    lv_obj_clear_flag(embrassing_right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(embrassing_right, LV_ALIGN_CENTER, EYE_POS_R + EYE_BASE_W - 6,
                 0);  // 右臉頰座標

    // 平常預設隱藏
    lv_obj_add_flag(avatar_embrassing, LV_OBJ_FLAG_HIDDEN);

    // ==============================================
    // 建立左額頭汗線 (|||)
    // ==============================================
    avatar_sweat = lv_obj_create(face_container);
    lv_obj_set_size(avatar_sweat, 20, 20);
    lv_obj_set_style_bg_opa(avatar_sweat, LV_OPA_TRANSP, 0);  // 透明背景容器
    lv_obj_set_style_border_width(avatar_sweat, 0, 0);
    lv_obj_set_style_pad_all(avatar_sweat, 0, 0);
    lv_obj_clear_flag(avatar_sweat, LV_OBJ_FLAG_SCROLLABLE);
    // lv_obj_align(avatar_sweat, LV_ALIGN_TOP_RIGHT, -10, 12);  // 位於左眼上方偏左
    lv_obj_align(avatar_sweat, LV_ALIGN_CENTER, EYE_POS_R + 16,
                 EYE_POS_H - EYE_BASE_H + 8);  // 位於左眼上方偏左

    // 畫 3 條垂直細線
    for (int i = 0; i < 3; i++) {
        lv_obj_t* line = lv_obj_create(avatar_sweat);
        lv_obj_set_size(line, 2, 14 + (i) * 3);  // 長短交錯更有動漫感
        lv_obj_set_pos(line, i * 6, 0);
        lv_obj_set_style_bg_color(line, avatar_theme_color, 0);
        lv_obj_set_style_border_width(line, 0, 0);
        lv_obj_set_style_radius(line, 1, 0);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_obj_add_flag(avatar_sweat, LV_OBJ_FLAG_HIDDEN);  // 平常隱藏

    // ==============================================
    // 建立右上角向量愛心 (♥)
    // ==============================================
    avatar_heart = lv_obj_create(face_container);
    lv_obj_set_size(avatar_heart, 22, 22);
    lv_obj_set_style_bg_opa(avatar_heart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(avatar_heart, 0, 0);
    lv_obj_set_style_pad_all(avatar_heart, 0, 0);
    lv_obj_clear_flag(avatar_heart, LV_OBJ_FLAG_SCROLLABLE);
    // lv_obj_align(avatar_heart, LV_ALIGN_TOP_RIGHT, -6, 10);  // 位於右眼上方偏右
    lv_obj_align(avatar_heart, LV_ALIGN_CENTER, EYE_POS_R + 16,
                 EYE_POS_H - EYE_BASE_H + 6);  // 位於右眼上方偏右

    // 浪漫粉紅/桃紅色
    lv_color_t heart_color = lv_color_hex(0xFF3366);

    // 愛心下半部 (旋轉 45 度的正方形，形成下尖角)
    lv_obj_t* heart_bottom = lv_obj_create(avatar_heart);
    lv_obj_set_size(heart_bottom, 11, 11);
    lv_obj_set_style_bg_color(heart_bottom, heart_color, 0);
    lv_obj_set_style_border_width(heart_bottom, 0, 0);
    lv_obj_set_style_radius(heart_bottom, 1, 0);
    lv_obj_set_style_transform_angle(heart_bottom, 450, 0);  // 旋轉 45 度
    lv_obj_align(heart_bottom, LV_ALIGN_CENTER, 4, -1);
    lv_obj_clear_flag(heart_bottom, LV_OBJ_FLAG_SCROLLABLE);

    // 愛心左圓弧
    lv_obj_t* heart_left = lv_obj_create(avatar_heart);
    lv_obj_set_size(heart_left, 10, 10);
    lv_obj_set_style_bg_color(heart_left, heart_color, 0);
    lv_obj_set_style_radius(heart_left, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(heart_left, 0, 0);
    lv_obj_align(heart_left, LV_ALIGN_CENTER, -4, -3);
    lv_obj_clear_flag(heart_left, LV_OBJ_FLAG_SCROLLABLE);

    // 愛心右圓弧
    lv_obj_t* heart_right = lv_obj_create(avatar_heart);
    lv_obj_set_size(heart_right, 10, 10);
    lv_obj_set_style_bg_color(heart_right, heart_color, 0);
    lv_obj_set_style_radius(heart_right, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(heart_right, 0, 0);
    lv_obj_align(heart_right, LV_ALIGN_CENTER, 4, -3);
    lv_obj_clear_flag(heart_right, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_add_flag(avatar_heart, LV_OBJ_FLAG_HIDDEN);  // 平常隱藏

    // ==============================================
    // 建立右邊咀愛心 (♥)
    // ==============================================
    avatar_kissy = lv_obj_create(face_container);
    lv_obj_set_size(avatar_kissy, 22, 22);
    lv_obj_set_style_bg_opa(avatar_kissy, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(avatar_kissy, 0, 0);
    lv_obj_set_style_pad_all(avatar_kissy, 0, 0);
    lv_obj_clear_flag(avatar_kissy, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(avatar_kissy, LV_ALIGN_CENTER, 18, MOUTH_POS_H);

    // 浪漫粉紅/桃紅色
    lv_color_t kissy_color = lv_color_hex(0xFF3366);

    // 愛心下半部 (旋轉 45 度的正方形，形成下尖角)
    lv_obj_t* kissy_bottom = lv_obj_create(avatar_kissy);
    lv_obj_set_size(kissy_bottom, 11, 11);
    lv_obj_set_style_bg_color(kissy_bottom, kissy_color, 0);
    lv_obj_set_style_border_width(kissy_bottom, 0, 0);
    lv_obj_set_style_radius(kissy_bottom, 1, 0);
    lv_obj_set_style_transform_angle(kissy_bottom, 450, 0);  // 旋轉 45 度
    lv_obj_align(kissy_bottom, LV_ALIGN_CENTER, 4, -1);
    lv_obj_clear_flag(kissy_bottom, LV_OBJ_FLAG_SCROLLABLE);

    // 愛心左圓弧
    lv_obj_t* kissy_left = lv_obj_create(avatar_kissy);
    lv_obj_set_size(kissy_left, 10, 10);
    lv_obj_set_style_bg_color(kissy_left, kissy_color, 0);
    lv_obj_set_style_radius(kissy_left, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(kissy_left, 0, 0);
    lv_obj_align(kissy_left, LV_ALIGN_CENTER, -4, -3);
    lv_obj_clear_flag(kissy_left, LV_OBJ_FLAG_SCROLLABLE);

    // 愛心右圓弧
    lv_obj_t* kissy_right = lv_obj_create(avatar_kissy);
    lv_obj_set_size(kissy_right, 10, 10);
    lv_obj_set_style_bg_color(kissy_right, kissy_color, 0);
    lv_obj_set_style_radius(kissy_right, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(kissy_right, 0, 0);
    lv_obj_align(kissy_right, LV_ALIGN_CENTER, 4, -3);
    lv_obj_clear_flag(kissy_right, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_add_flag(avatar_kissy, LV_OBJ_FLAG_HIDDEN);  // 平常隱藏

    // ==========================================
    // 【第 2 題】建立日系動漫「💢」生氣青筋符號
    // ==========================================
    avatar_anger = lv_obj_create(face_container);
    lv_obj_set_size(avatar_anger, 22, 22);
    lv_obj_set_style_bg_opa(avatar_anger, LV_OPA_TRANSP, 0);  // 透明容器
    lv_obj_set_style_border_width(avatar_anger, 0, 0);
    lv_obj_set_style_pad_all(avatar_anger, 0, 0);
    lv_obj_set_style_transform_angle(avatar_anger, 200, 0);  // 旋轉
    lv_obj_clear_flag(avatar_anger, LV_OBJ_FLAG_SCROLLABLE);
    // lv_obj_align(avatar_anger, LV_ALIGN_TOP_RIGHT, -4, -6);  // 位於右額頭
    lv_obj_align(avatar_anger, LV_ALIGN_CENTER, EYE_POS_R + 16, EYE_POS_H - EYE_BASE_H);

    lv_color_t anger_red = lv_color_hex(0xFF2222);  // 鮮紅怒氣色

    // 定義 4 個角的邊框方向：上左(⌜)、上右(⌝)、下左(⌞)、下右(⌟)
    const lv_border_side_t sides[4] = {
        (lv_border_side_t)(LV_BORDER_SIDE_BOTTOM | LV_BORDER_SIDE_RIGHT),
        (lv_border_side_t)(LV_BORDER_SIDE_BOTTOM | LV_BORDER_SIDE_LEFT),
        (lv_border_side_t)(LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_RIGHT),
        (lv_border_side_t)(LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_LEFT)};
    const lv_coord_t pos_x[4] = {2, 12, 2, 12};
    const lv_coord_t pos_y[4] = {2, 2, 12, 12};

    for (int i = 0; i < 4; i++) {
        lv_obj_t* corner = lv_obj_create(avatar_anger);
        lv_obj_set_size(corner, 8, 8);
        lv_obj_set_pos(corner, pos_x[i], pos_y[i]);
        lv_obj_set_style_bg_opa(corner, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(corner, anger_red, 0);
        lv_obj_set_style_border_width(corner, 3, 0);  // 3px 粗細
        lv_obj_set_style_border_side(corner, sides[i], 0);
        lv_obj_set_style_radius(corner, 0, 0);
        lv_obj_clear_flag(corner, LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_obj_add_flag(avatar_anger, LV_OBJ_FLAG_HIDDEN);  // 平常隱藏

    // 啟動定時器
    lv_timer_create(custom_app_timer_cb, 100, NULL);

    // 啟動天氣抓取背景任務
    xTaskCreate(weather_update_task, "weather_task", 8192, NULL, 5, &weather_task_handle);

    // ==============================================
    // 初始化巴士顯示面板 (全黑底，黃字+白字)
    // ==============================================
    bg_bus = lv_obj_create(parent);
    lv_obj_set_size(bg_bus, LCD_W, LCD_H);
    lv_obj_set_style_bg_color(bg_bus, lv_color_black(), 0);
    lv_obj_set_style_border_width(bg_bus, 0, 0);
    lv_obj_set_style_radius(bg_bus, 0, 0);
    lv_obj_set_style_pad_all(bg_bus, 0, 0);
    lv_obj_clear_flag(bg_bus, LV_OBJ_FLAG_SCROLLABLE);

    bus_route_label = lv_label_create(bg_bus);
    lv_obj_set_style_text_color(bus_route_label, lv_color_make(255, 200, 0), 0);  // 黃色路線
    lv_obj_set_style_text_font(bus_route_label, &lv_font_montserrat_28, 0);
    lv_obj_align(bus_route_label, LV_ALIGN_CENTER, 0, -35);

    bus_t1_label = lv_label_create(bg_bus);
    lv_obj_set_style_text_color(bus_t1_label, lv_color_white(), 0);  // 白色時間
    lv_obj_align(bus_t1_label, LV_ALIGN_CENTER, 0, 0);

    bus_t2_label = lv_label_create(bg_bus);
    lv_obj_set_style_text_color(bus_t2_label, lv_color_hex(0xAAAAAA), 0);  // 灰色後班
    lv_obj_align(bus_t2_label, LV_ALIGN_CENTER, 0, 25);

    lv_obj_add_flag(bg_bus, LV_OBJ_FLAG_HIDDEN);  // 預設隱藏

    // 啟動觸摸喚醒任務
    xTaskCreate(touch_wake_task, "touch_task", 4096, NULL, 5, NULL);

    // 建立背景任務
    xTaskCreate(bus_fetch_task, "bus_task", 4096, NULL, 5, &bus_task_handle);
    // 👇 加入這一行，啟動 IMU 背景初始化
    xTaskCreate(imu_init_task, "imu_init_task", 4096, NULL, 5, NULL);
}

static void reset_avatar_geometry() {
    if (avatar_left_eye) {
        lv_obj_set_size(avatar_left_eye, EYE_BASE_W, EYE_BASE_H);
        lv_obj_set_style_radius(avatar_left_eye, EYE_CORNER, 0);
        lv_obj_align(avatar_left_eye, LV_ALIGN_CENTER, EYE_POS_L, EYE_POS_H);  // 基準左眼
    }
    if (avatar_right_eye) {
        lv_obj_set_size(avatar_right_eye, EYE_BASE_W, EYE_BASE_H);
        lv_obj_set_style_radius(avatar_right_eye, EYE_CORNER, 0);
        lv_obj_align(avatar_right_eye, LV_ALIGN_CENTER, EYE_POS_R, EYE_POS_H);  // 基準右眼
    }
    if (avatar_mouth) {
        lv_obj_set_size(avatar_mouth, MOUTH_BASE_W, MOUTH_BASE_H);
        lv_obj_set_style_radius(avatar_mouth, 8, 0);
        lv_obj_align(avatar_mouth, LV_ALIGN_CENTER, 0, MOUTH_POS_H);  // 基準嘴巴 (置中)
    }
}

// --- 鉤子 2：設置特殊表情 ---
void CustomApp_SetEmotion(const char* emotion) {
    if (emotion == nullptr)
        return;

    if (!avatar_left_eye)
        return;
    std::string emo_str(emotion);
    current_active_emotion = emo_str;  // 【關鍵】記錄當前表情
    lv_obj_set_style_radius(avatar_left_eye, EYE_CORNER, 0);
    lv_obj_set_style_radius(avatar_right_eye, EYE_CORNER, 0);

    // 1. 每次變換表情前，停止所有可能在跑的動畫
    lv_anim_del(avatar_mouth, NULL);
    lv_anim_del(avatar_left_eye, NULL);
    lv_anim_del(avatar_right_eye, NULL);
    if (avatar_heart)
        lv_anim_del(avatar_heart, NULL);
    if (avatar_anger)
        lv_anim_del(avatar_anger, NULL);
    if (avatar_tears)
        lv_anim_del(avatar_tears, NULL);
    if (avatar_swallow)
        lv_anim_del(avatar_swallow, NULL);

    // 2. 立刻強制拉回基準座標與大小，消除上一個表情或被打斷動畫的殘留位移！
    reset_avatar_geometry();

    if (avatar_heart)
        lv_obj_add_flag(avatar_heart, LV_OBJ_FLAG_HIDDEN);
    if (avatar_kissy)
        lv_obj_add_flag(avatar_kissy, LV_OBJ_FLAG_HIDDEN);
    if (avatar_sweat)
        lv_obj_add_flag(avatar_sweat, LV_OBJ_FLAG_HIDDEN);
    if (avatar_anger)
        lv_obj_add_flag(avatar_anger, LV_OBJ_FLAG_HIDDEN);
    if (avatar_embrassing)
        lv_obj_add_flag(avatar_embrassing, LV_OBJ_FLAG_HIDDEN);
    if (avatar_sad_eye)
        lv_obj_add_flag(avatar_sad_eye, LV_OBJ_FLAG_HIDDEN);
    if (avatar_surprised_eye)
        lv_obj_add_flag(avatar_surprised_eye, LV_OBJ_FLAG_HIDDEN);
    if (avatar_angry_eye)
        lv_obj_add_flag(avatar_angry_eye, LV_OBJ_FLAG_HIDDEN);
    if (avatar_mouth_laugh)
        lv_obj_add_flag(avatar_mouth_laugh, LV_OBJ_FLAG_HIDDEN);
    if (avatar_silly_tongue)
        lv_obj_add_flag(avatar_silly_tongue, LV_OBJ_FLAG_HIDDEN);
    if (avatar_mouth_happy)
        lv_obj_add_flag(avatar_mouth_happy, LV_OBJ_FLAG_HIDDEN);
    if (avatar_cool)
        lv_obj_add_flag(avatar_cool, LV_OBJ_FLAG_HIDDEN);
    if (avatar_tears)
        lv_obj_add_flag(avatar_tears, LV_OBJ_FLAG_HIDDEN);
    if (avatar_swallow)
        lv_obj_add_flag(avatar_swallow, LV_OBJ_FLAG_HIDDEN);
    if (avatar_confuse)
        lv_obj_add_flag(avatar_confuse, LV_OBJ_FLAG_HIDDEN);
    if (avatar_sleepy_z)
        lv_obj_add_flag(avatar_sleepy_z, LV_OBJ_FLAG_HIDDEN);
    if (left_eye_highlight)
        lv_obj_remove_flag(left_eye_highlight, LV_OBJ_FLAG_HIDDEN);
    if (right_eye_highlight)
        lv_obj_remove_flag(right_eye_highlight, LV_OBJ_FLAG_HIDDEN);

    lv_obj_remove_flag(avatar_mouth, LV_OBJ_FLAG_HIDDEN);

    if (emo_str == "neutral" || emo_str == "idle" || emo_str == "") {
        current_active_emotion = "neutral";
        last_avatar_state = (DeviceState)-1;
        return;
    }

    if (emo_str == "loving") {
        // 溫柔笑眼
        set_eye_size(EYE_BASE_W, 8);
        lv_obj_align(avatar_left_eye, LV_ALIGN_CENTER, EYE_POS_L, EYE_POS_H - 6);
        lv_obj_align(avatar_right_eye, LV_ALIGN_CENTER, EYE_POS_R, EYE_POS_H - 6);
        if (left_eye_highlight)
            lv_obj_add_flag(left_eye_highlight, LV_OBJ_FLAG_HIDDEN);
        if (right_eye_highlight)
            lv_obj_add_flag(right_eye_highlight, LV_OBJ_FLAG_HIDDEN);

        lv_obj_set_size(avatar_mouth, 25, 12);
        lv_obj_set_style_radius(avatar_mouth, 8, 0);

        // 顯示愛心
        if (avatar_heart) {
            lv_obj_remove_flag(avatar_heart, LV_OBJ_FLAG_HIDDEN);

            // 加入愛心上下輕微飄動動畫
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, avatar_heart);
            lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
            lv_anim_set_values(
                &a, EYE_POS_H - EYE_BASE_H + 6,
                EYE_POS_H - EYE_BASE_H);  // 上下移動位置，第一個是原本位置，第二是移動位置
            lv_anim_set_duration(&a, 600);
            lv_anim_set_playback_time(&a, 600);
            lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
            lv_anim_start(&a);
        }
        if (avatar_embrassing) {
            lv_obj_remove_flag(avatar_embrassing, LV_OBJ_FLAG_HIDDEN);
        }
        if (avatar_mouth_happy) {
            lv_obj_remove_flag(avatar_mouth_happy, LV_OBJ_FLAG_HIDDEN);
        }

    } else if (emo_str == "crying") {
        set_eye_size(EYE_BASE_W, 10);  // 眼睛瞇起來哭
        // lv_obj_set_size(avatar_mouth, 20, 20);        // 嘴巴張大哭
        lv_obj_set_style_radius(avatar_mouth, 8, 0);  // 嘴巴帶點圓角
        if (left_eye_highlight)
            lv_obj_add_flag(left_eye_highlight, LV_OBJ_FLAG_HIDDEN);
        if (right_eye_highlight)
            lv_obj_add_flag(right_eye_highlight, LV_OBJ_FLAG_HIDDEN);

        if (avatar_mouth) {
            lv_obj_set_size(avatar_mouth, 20, 20);
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_exec_cb(&a, anim_mouth_snore_cb);
            lv_anim_set_values(&a, 20, 60);          // 0 (合口38) -> 100 (張口)
            lv_anim_set_duration(&a, 300);           // 200ms 快速張嘴吸氣
            lv_anim_set_playback_duration(&a, 600);  // 1200ms 慢慢吐氣合嘴，回到寬度 38
            lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
            lv_anim_set_var(&a, avatar_mouth);
            lv_anim_start(&a);
        }

        // 啟動瀑布眼淚動畫！
        if (avatar_tears) {
            lv_obj_remove_flag(avatar_tears, LV_OBJ_FLAG_HIDDEN);

            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, avatar_tears);
            lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
            // 讓眼淚容器從原本位置 (0) 往下掉 (15px)，製造眼淚落下的流動感
            lv_anim_set_values(&a, 0, 15);
            lv_anim_set_time(&a, 300);  // 掉落速度快 (300ms)
            // 不設 playback (不來回反彈)，直接無限重複掉落！
            lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
            lv_anim_start(&a);
        }
    } else if (emo_str == "delicious") {
        set_eye_size(EYE_BASE_W, 10);  // 眼睛瞇起來哭
        lv_obj_align(avatar_left_eye, LV_ALIGN_CENTER, EYE_POS_L, EYE_POS_H - 6);
        lv_obj_align(avatar_right_eye, LV_ALIGN_CENTER, EYE_POS_R, EYE_POS_H - 6);
        if (left_eye_highlight)
            lv_obj_add_flag(left_eye_highlight, LV_OBJ_FLAG_HIDDEN);
        if (right_eye_highlight)
            lv_obj_add_flag(right_eye_highlight, LV_OBJ_FLAG_HIDDEN);

        lv_obj_set_size(avatar_mouth, MOUTH_BASE_W, 20);  // 嘴巴張大哭
        lv_obj_set_style_radius(avatar_mouth, 8, 0);      // 嘴巴帶點圓角

        if (avatar_embrassing) {
            lv_obj_remove_flag(avatar_embrassing, LV_OBJ_FLAG_HIDDEN);
        }

        // 啟動口水動畫！
        if (avatar_swallow) {
            lv_obj_remove_flag(avatar_swallow, LV_OBJ_FLAG_HIDDEN);

            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, avatar_swallow);
            lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
            // 讓眼淚容器從原本位置 (0) 往下掉 (10px)，製造眼淚落下的流動感
            lv_anim_set_values(&a, 0, 10);
            lv_anim_set_time(&a, 300);  // 掉落速度快 (300ms)
            // 不設 playback (不來回反彈)，直接無限重複掉落！
            lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
            lv_anim_start(&a);
        }
    } else if (emo_str == "confused") {
        // 無奈表情：左眼變扁、右眼縮小，嘴巴變成一條扁平橫線
        set_eye_size(EYE_BASE_W + 6, 8);
        lv_obj_align(avatar_left_eye, LV_ALIGN_CENTER, EYE_POS_L, EYE_POS_H - 1);
        lv_obj_align(avatar_right_eye, LV_ALIGN_CENTER, EYE_POS_R, EYE_POS_H - 1);
        lv_obj_set_size(avatar_mouth, MOUTH_BASE_W + 6, MOUTH_BASE_H);  // 扁嘴
        if (avatar_confuse) {
            lv_obj_remove_flag(avatar_confuse, LV_OBJ_FLAG_HIDDEN);
        }
        if (avatar_sweat) {
            lv_obj_remove_flag(avatar_sweat, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (emo_str == "angry") {
        // 憤怒表情：瞇眼、嘴角緊抿
        set_eye_size(EYE_BASE_W, EYE_BASE_H);
        lv_obj_set_size(avatar_mouth, 26, 32);

        // 彈出 💢 青筋符號並加上微幅震顫/跳動動畫！
        if (avatar_anger) {
            lv_obj_remove_flag(avatar_anger, LV_OBJ_FLAG_HIDDEN);

            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, avatar_anger);
            lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
            lv_anim_set_values(&a, EYE_POS_H - EYE_BASE_H,
                               EYE_POS_H - EYE_BASE_H - 4);  // 上下震顫跳動 4px
            lv_anim_set_duration(&a, 150);
            lv_anim_set_playback_time(&a, 150);
            lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
            lv_anim_start(&a);
        }
        if (avatar_angry_eye) {
            lv_obj_remove_flag(avatar_angry_eye, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (emo_str == "embarrassed") {
        set_eye_size(EYE_BASE_W, 12);
        lv_obj_align(avatar_left_eye, LV_ALIGN_CENTER, EYE_POS_L, EYE_POS_H - 6);
        lv_obj_align(avatar_right_eye, LV_ALIGN_CENTER, EYE_POS_R, EYE_POS_H - 6);
        lv_obj_set_size(avatar_mouth, 16, 6);
        if (avatar_embrassing) {
            lv_obj_remove_flag(avatar_embrassing, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (emo_str == "thinking") {
        lv_obj_set_size(avatar_right_eye, EYE_BASE_W, EYE_BASE_H);
        lv_obj_set_size(avatar_mouth, MOUTH_BASE_W, MOUTH_BASE_H);
        lv_obj_set_size(avatar_left_eye, EYE_BASE_W, EYE_BASE_H);
        lv_anim_del(avatar_left_eye, NULL);

        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, avatar_left_eye);
        lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_height);
        lv_anim_set_values(&a, EYE_BASE_H, EYE_BASE_W);
        lv_anim_set_duration(&a, 240);
        lv_anim_set_path_cb(&a, lv_anim_path_bounce);
        lv_anim_set_repeat_count(&a, 1);  // 不做 playback，閉上後停在 10px
        lv_anim_start(&a);

        lv_anim_t b;
        lv_anim_init(&b);
        lv_anim_set_var(&b, avatar_mouth);
        lv_anim_set_exec_cb(&b, (lv_anim_exec_xcb_t)lv_obj_set_width);
        lv_anim_set_values(&b, MOUTH_BASE_W, MOUTH_BASE_W + 6);  // 從睜眼縮小到 10px 瞇瞇眼
        lv_anim_set_duration(&b, 240);
        lv_anim_set_path_cb(&b, lv_anim_path_bounce);
        lv_anim_set_repeat_count(&b, 1);  // 不做 playback，閉上後停在 10px
        lv_anim_start(&b);
    } else if (emo_str == "surprised") {
        set_eye_size(EYE_BASE_W, EYE_BASE_H - 2);
        lv_obj_set_size(avatar_mouth, 20, 20);
        if (avatar_surprised_eye) {
            lv_obj_remove_flag(avatar_surprised_eye, LV_OBJ_FLAG_HIDDEN);
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, avatar_surprised_eye);
            lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
            lv_anim_set_values(&a, EYE_POS_H - EYE_BASE_H + 4,
                               EYE_POS_H - EYE_BASE_H - 2);  // 向上6px
            lv_anim_set_duration(&a, 400);
            lv_anim_set_repeat_count(&a, 1);
            lv_anim_start(&a);
        }
    } else if (emo_str == "shocked") {
        set_eye_size(EYE_BASE_W + 2, EYE_BASE_H + 2);
        lv_obj_set_size(avatar_mouth, 20, 30);
        if (avatar_sweat) {
            lv_obj_remove_flag(avatar_sweat, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (emo_str == "silly") {
        lv_obj_set_size(avatar_left_eye, EYE_BASE_W, EYE_BASE_H);
        lv_obj_set_size(avatar_right_eye, EYE_BASE_W, EYE_BASE_W);
        if (avatar_silly_tongue) {
            lv_obj_remove_flag(avatar_silly_tongue, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_set_size(avatar_mouth, MOUTH_BASE_W + 4, MOUTH_BASE_H);
        lv_obj_set_style_radius(avatar_mouth, 8, 0);
    } else if (emo_str == "happy") {
        set_eye_size(EYE_BASE_W, 8);
        lv_obj_align(avatar_left_eye, LV_ALIGN_CENTER, EYE_POS_L, EYE_POS_H - 6);
        lv_obj_align(avatar_right_eye, LV_ALIGN_CENTER, EYE_POS_R, EYE_POS_H - 6);
        if (left_eye_highlight)
            lv_obj_add_flag(left_eye_highlight, LV_OBJ_FLAG_HIDDEN);
        if (right_eye_highlight)
            lv_obj_add_flag(right_eye_highlight, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(avatar_mouth, 1, 1);

        if (avatar_embrassing) {
            lv_obj_remove_flag(avatar_embrassing, LV_OBJ_FLAG_HIDDEN);
        }
        if (avatar_mouth_happy) {
            lv_obj_remove_flag(avatar_mouth_happy, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (emo_str == "laughing") {
        set_eye_size(EYE_BASE_W, 8);
        lv_obj_align(avatar_left_eye, LV_ALIGN_CENTER, EYE_POS_L, EYE_POS_H - 6);
        lv_obj_align(avatar_right_eye, LV_ALIGN_CENTER, EYE_POS_R, EYE_POS_H - 6);
        if (left_eye_highlight)
            lv_obj_add_flag(left_eye_highlight, LV_OBJ_FLAG_HIDDEN);
        if (right_eye_highlight)
            lv_obj_add_flag(right_eye_highlight, LV_OBJ_FLAG_HIDDEN);

        lv_obj_set_size(avatar_mouth, 1, 1);
        if (avatar_mouth_laugh) {
            lv_obj_remove_flag(avatar_mouth_laugh, LV_OBJ_FLAG_HIDDEN);
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, avatar_mouth_laugh);
            lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
            lv_anim_set_values(&a, 24, 20);  // 上下震顫跳動 4px
            lv_anim_set_duration(&a, 150);
            lv_anim_set_playback_time(&a, 150);
            lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
            lv_anim_start(&a);
        }
    } else if (emo_str == "cool") {
        set_eye_size(EYE_BASE_W, EYE_BASE_H);
        lv_obj_set_size(avatar_mouth, 3, 3);
        if (avatar_mouth_happy) {
            lv_obj_remove_flag(avatar_mouth_happy, LV_OBJ_FLAG_HIDDEN);
        }
        if (avatar_cool) {
            lv_obj_remove_flag(avatar_cool, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (emo_str == "sad") {
        set_eye_size(EYE_BASE_W, EYE_BASE_H);
        lv_obj_set_size(avatar_mouth, 20, MOUTH_BASE_H);
        if (avatar_sad_eye) {
            lv_obj_remove_flag(avatar_sad_eye, LV_OBJ_FLAG_HIDDEN);
        }
        if (left_eye_highlight)
            lv_obj_add_flag(left_eye_highlight, LV_OBJ_FLAG_HIDDEN);
        if (right_eye_highlight)
            lv_obj_add_flag(right_eye_highlight, LV_OBJ_FLAG_HIDDEN);
    } else if (emo_str == "winking") {
        lv_obj_set_size(avatar_left_eye, EYE_BASE_W, EYE_BASE_H);
        lv_obj_set_size(avatar_mouth, MOUTH_BASE_W - 4, MOUTH_BASE_H);
        lv_obj_set_size(avatar_right_eye, EYE_BASE_W, EYE_BASE_H);
        if (right_eye_highlight)
            lv_obj_add_flag(right_eye_highlight, LV_OBJ_FLAG_HIDDEN);

        // 執行單次閉眼動畫（不回彈，定格在 10px）
        lv_anim_del(avatar_right_eye, NULL);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, avatar_right_eye);
        lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_height);
        lv_anim_set_values(&a, EYE_BASE_H, 10);  // 從睜眼縮小到 10px 瞇瞇眼
        lv_anim_set_duration(&a, 120);           // 150ms 快速閉合
        lv_anim_set_path_cb(&a, lv_anim_path_bounce);
        lv_anim_set_repeat_count(&a, 1);  // 不做 playback，閉上後停在 10px
        lv_anim_start(&a);
    } else if (emo_str == "relaxed") {
        set_eye_size(EYE_BASE_W, 8);
        lv_obj_set_size(avatar_mouth, 20, MOUTH_BASE_H);
        if (avatar_mouth_happy) {
            lv_obj_remove_flag(avatar_mouth_happy, LV_OBJ_FLAG_HIDDEN);
        }
        if (left_eye_highlight)
            lv_obj_add_flag(left_eye_highlight, LV_OBJ_FLAG_HIDDEN);
        if (right_eye_highlight)
            lv_obj_add_flag(right_eye_highlight, LV_OBJ_FLAG_HIDDEN);
    } else if (emo_str == "kissy") {
        lv_obj_set_size(avatar_left_eye, EYE_BASE_W, EYE_BASE_H);
        lv_obj_set_size(avatar_right_eye, EYE_BASE_W, 10);
        if (right_eye_highlight)
            lv_obj_add_flag(right_eye_highlight, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(avatar_mouth, 12, 12);
        lv_obj_align(avatar_mouth, LV_ALIGN_CENTER, 2, MOUTH_POS_H);
        if (avatar_kissy) {
            lv_obj_set_style_radius(avatar_mouth, LV_RADIUS_CIRCLE, 0);
            lv_obj_remove_flag(avatar_kissy, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (emo_str == "sleepy") {
        set_eye_size(EYE_BASE_W, EYE_BASE_H / 2);
        lv_obj_set_size(avatar_mouth, 18, 20);
        if (avatar_sleepy_z) {
            lv_obj_remove_flag(avatar_sleepy_z, LV_OBJ_FLAG_HIDDEN);
        }
        if (left_eye_highlight)
            lv_obj_add_flag(left_eye_highlight, LV_OBJ_FLAG_HIDDEN);
        if (right_eye_highlight)
            lv_obj_add_flag(right_eye_highlight, LV_OBJ_FLAG_HIDDEN);

        // 啟動口水動畫！
        if (avatar_swallow) {
            lv_obj_remove_flag(avatar_swallow, LV_OBJ_FLAG_HIDDEN);

            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, avatar_swallow);
            lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
            // 讓眼淚容器從原本位置 (0) 往下掉 (15px)，製造眼淚落下的流動感
            lv_anim_set_values(&a, 0, 10);
            lv_anim_set_time(&a, 1200);  // 掉落速度快 (300ms)
            // 不設 playback (不來回反彈)，直接無限重複掉落！
            lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
            lv_anim_start(&a);
        }
    }
}

// --- 鉤子 3：劫持按鍵 ---
void CustomApp_HijackButtons(Button& boot_button, std::function<void()> on_ai_single_click,
                             std::function<void()> on_ai_long_press) {
    ai_single_click_cb = on_ai_single_click;
    ai_long_press_cb = on_ai_long_press;

    // 1. 短按邏輯
    boot_button.OnClick([]() {
        ESP_LOGI("Button", "短按觸發！當前系統模式: %d", current_system_mode);
        idle_to_clock_counter = 0;  // 只要按鍵，重置閒置計時

        if (current_system_mode == 4) {
            // App 選單輪捲
            app_menu_selection = (app_menu_selection + 1) % APP_MAX;
            ui_mode_changed = true;
        } else if (current_system_mode == 1) {
            // Game 選單輪捲
            game_menu_selection = (game_menu_selection + 1) % GAME_MAX;
            ui_mode_changed = true;
        } else if (current_system_mode == 2 || current_system_mode == 5) {
            // 【修正】確保在過三關中按鈕可以喚醒麥克風
            if (current_system_mode == 2 && game_menu_selection == GAME_TICTACTOE) {
                if (ttt_game_state == TTT_STATE_START || ttt_game_state == TTT_STATE_GAMEOVER) {
                    pending_game_action = 5;
                } else {
                    // 已開始：短按喚醒麥克風
                    ESP_LOGI("TicTacToe", "按下主按鍵，啟動麥克風聽取方位...");
                    if (ai_single_click_cb)
                        ai_single_click_cb();
                }
            } else {
                // 其他遊戲保留暫停/開始功能
                pending_game_action = 5;
            }
        } else if (current_system_mode == 3 || current_system_mode == 6) {
            pending_game_action = 6;
        } else if (current_system_mode == 0) {
            DeviceState state = Application::GetInstance().GetDeviceState();
            if (state == kDeviceStateIdle) {
                pending_touch_wake = 15;
            } else {
                if (ai_single_click_cb)
                    ai_single_click_cb();
            }
        }
    });

    // 2. 長按邏輯
    boot_button.OnLongPress([]() {
        idle_to_clock_counter = 0;

        if (current_system_mode == 4) {
            // 【修正】在 App 選單長按：進入 App 執行模式 (5)
            current_system_mode = 5;
            current_ui_mode = 5;
            ui_mode_changed = true;
        } else if (current_system_mode == 1) {
            // 【修正】在 Game 選單長按：進入 Game 執行模式 (2)
            current_system_mode = 2;
            current_ui_mode = 2;
            ui_mode_changed = true;
        } else if (current_system_mode == 5) {
            // 【修正】在 App 執行中長按：退回 App 選單 (4)
            current_system_mode = 4;
            current_ui_mode = 4;
            ui_mode_changed = true;
        } else if (current_system_mode == 2) {
            // 【修正】在 Game 執行中長按：退回 Game 選單 (1)
            current_system_mode = 1;
            current_ui_mode = 1;
            ui_mode_changed = true;
        } else if (current_system_mode == 0) {
            // 在 AI 模式下長按：進入配網模式
            if (ai_long_press_cb) {
                ai_long_press_cb();
            }
        }
    });

    // 3. 雙擊切換模式邏輯：AI(0) -> Apps(4) -> Games(1) -> Time(3) -> Weather(6)-> AI(0)
    boot_button.OnDoubleClick([]() {
        idle_to_clock_counter = 0;

        if (current_system_mode == 0) {
            // AI 模式 -> 進入 Apps 選單
            current_system_mode = 4;
            current_ui_mode = 4;
            app_menu_selection = APP_BWA;
            auto backlight = Board::GetInstance().GetBacklight();
            if (backlight)
                backlight->SetBrightness(100);
        } else if (current_system_mode == 4) {
            // Apps 選單 -> 進入 Games 選單
            current_system_mode = 1;
            current_ui_mode = 1;
            game_menu_selection = GAME_SPACE;
        } else if (current_system_mode == 1) {
            // Games 選單 -> 進入暗黑時鐘 (3)
            current_system_mode = 3;
            current_ui_mode = 3;
        } else if (current_system_mode == 3) {
            // 時間(3) 雙擊 -> 進入天氣模式(6)
            current_system_mode = 6;
            current_ui_mode = 6;
        } else if (current_system_mode == 6 || current_system_mode == 2 ||
                   current_system_mode == 5) {
            // 【新增修改】：從天氣(6) 或其他遊戲App雙擊 -> 回血 100% 亮度，回到 AI (0)
            auto backlight = Board::GetInstance().GetBacklight();
            if (backlight)
                backlight->SetBrightness(100);
            current_system_mode = 0;
            current_ui_mode = 0;
            Application::GetInstance().Schedule([]() {
                auto& app = Application::GetInstance();
                app.AbortSpeaking(kAbortReasonNone);
                app.StopListening();
                app.SetDeviceState(kDeviceStateIdle);
            });
        }
        ui_mode_changed = true;
    });
}
