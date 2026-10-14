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

// --- 時鐘待機模式物件 ---
static lv_obj_t* bg_clock = nullptr;
static lv_obj_t* clock_time_label = nullptr;
static lv_obj_t* clock_date_label = nullptr;
// 控制時鐘亮暗與 5 秒自動倒數
static bool clock_is_bright = false;  // 目前是否為高亮狀態
static int clock_dim_countdown = 0;   // 倒數計時器 (每 100ms 扣 1)
const int CLOCK_DIM_TIMEOUT_SEC = 5;  // 亮起後等待幾秒自動變暗 (可自行改成 3 或 5 秒)

#define EYE_BASE_W 22  // 眼睛基準寬度 (想放大改這裡即可，例如改 24)
#define EYE_BASE_H 30  // 眼睛基準高度 (想放大改這裡即可，例如改 24)
#define EYE_POS_L -30
#define EYE_POS_R 30
#define EYE_POS_H -6
#define MOUTH_BASE_W 28
#define MOUTH_BASE_H 8
#define MOUTH_POS_H 12

// ==========================================
//           螢幕解析度常數 (M5StickS3)
// ==========================================
#define LCD_W 128
#define LCD_H 128

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
static lv_obj_t* avatar_left_eye = nullptr;
static lv_obj_t* avatar_right_eye = nullptr;
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

static DeviceState last_avatar_state = (DeviceState)-1;
static int idle_to_clock_counter = 0;

volatile int current_system_mode = 0;  // 0: AI, 1: 遊戲選單, 2: 遊戲中
volatile int current_ui_mode = 0;
volatile int game_menu_selection = 0;  // 0: 2048, 1: Snake, 2: Exit
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
        } else if (touch_val > 0 && touch_val < baseline + 5000) {
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
        if (baseline > 0 && touch_val > baseline + 15000) {
            touch_cooldown = 30;  // 觸發後冷卻 3 秒，防止連觸

            // 5. 模式過濾：只允許在 AI 待機 或 時鐘模式 下喚醒！
            if (current_system_mode == 0 || current_system_mode == 3) {
                DeviceState state = Application::GetInstance().GetDeviceState();

                // 確保 AI 是安靜待機的狀態
                if (current_system_mode == 3 || state == kDeviceStateIdle) {
                    ESP_LOGI("Touch", "🌟 錫箔紙觸摸喚醒！數值飆升至: %lu",
                             (unsigned long)touch_val);

                    // 如果是在時鐘模式，瞬間切回 AI 臉蛋並點亮螢幕
                    if (current_system_mode == 3) {
                        auto backlight = Board::GetInstance().GetBacklight();
                        if (backlight)
                            backlight->SetBrightness(100);
                        current_system_mode = 0;
                        current_ui_mode = 0;
                        ui_mode_changed = true;
                        idle_to_clock_counter = 0;
                    }

                    // 【關鍵改變】：不直接喚醒麥克風，而是設定 1.5 秒倒數計時！
                    pending_touch_wake = 15;
                }
            }
        }
    }
}

// ==========================================
//           時鐘模式：天氣專屬變數
// ==========================================
static lv_obj_t* clock_weather_label = nullptr;
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

// --- 獨立的時間刷新函數 ---
static void update_clock_display() {
    if (!clock_time_label || !clock_date_label)
        return;

    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    char time_str[16];
    char date_str[32];
    strftime(time_str, sizeof(time_str), "%H:%M", &timeinfo);
    strftime(date_str, sizeof(date_str), "%m-%d %a", &timeinfo);  // 例如 09-23 Wed

    lv_label_set_text(clock_time_label, time_str);
    lv_label_set_text(clock_date_label, date_str);

    // 更新天氣標籤
    if (clock_weather_label) {
        if (weather_ready) {
            // lv_label_set_text_fmt(clock_weather_label, "%s | H:%d° L:%d°", weather_desc,
            // weather_temp_max, weather_temp_min);
            lv_label_set_text_fmt(clock_weather_label, "%s %d°\nH: %d° | L: %d°", weather_desc,
                                  weather_temp_cur, weather_temp_max, weather_temp_min);
        } else {
            lv_label_set_text(clock_weather_label, "Loading\nWeather...");
        }
    }
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
        lv_label_set_text(marksix_tip_label, "SHAKE TO DRAW (0/6)");
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
            lv_label_set_text_fmt(marksix_tip_label, "SHAKE TO DRAW (%d/6)", marksix_count);
        } else {
            lv_label_set_text(marksix_tip_label, "★ LUCKY 6 READY! ★");
        }
    }
    ESP_LOGI("MarkSix", "抽中第 %d 顆號碼球: %02d", marksix_count, num);
}

// ==========================================
//        太空侵略者 (Space Invaders) 系統變數
// ==========================================
#define SPACE_MAX_BULLETS 4
#define SPACE_ALIEN_ROWS 3
#define SPACE_ALIEN_COLS 4
#define SPACE_MAX_BOMBS 3

enum SpaceState { SPACE_STATE_START, SPACE_STATE_PLAYING, SPACE_STATE_GAMEOVER };

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
static SpaceAlien space_aliens[SPACE_ALIEN_ROWS][SPACE_ALIEN_COLS];

static int space_score = 0;
static int space_best_score = 0;
static SpaceState space_game_state = SPACE_STATE_START;
static int space_ship_x = 64;  // 飛船 X 座標 (置中)

// 外星人集團的移動狀態
static int alien_group_x = 0;
static int alien_group_y = 20;
static int alien_dir = 1;  // 1:向右, -1:向左

// ==========================================
//        打磚塊 (Breakout) 系統變數
// ==========================================
#define BK_ROWS 4
#define BK_COLS 5

enum BreakoutState { BK_STATE_START, BK_STATE_PLAYING, BK_STATE_GAMEOVER };

static lv_obj_t* bg_breakout = nullptr;
static lv_obj_t* bk_paddle = nullptr;
static lv_obj_t* bk_ball = nullptr;
static lv_obj_t* bk_bricks[BK_ROWS][BK_COLS];
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
static bool bk_bricks_active[BK_ROWS][BK_COLS];
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
    bk_paddle_w = 28 - (bk_level * 2);
    if (bk_paddle_w < 12)
        bk_paddle_w = 12;

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
    for (int r = 0; r < BK_ROWS; r++) {
        for (int c = 0; c < BK_COLS; c++) {
            bk_bricks_active[r][c] = true;
            if (bk_bricks[r][c])
                lv_obj_remove_flag(bk_bricks[r][c], LV_OBJ_FLAG_HIDDEN);
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
            lv_label_set_text_fmt(overlay_label, "GAME OVER\n\nSCORE: %d\n\n[CLICK]",
                                  current_score);
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
    if (strcmp(role, "user") == 0 && content != nullptr) {
        std::string text(content);

        // 1. 擲杯指令
        if (text.find("杯") != std::string::npos || text.find("筊") != std::string::npos) {
            ESP_LOGI("VoiceCmd", "語音指令：立即跳轉至 擲杯");
            current_system_mode = 2;
            current_ui_mode = 2;
            game_menu_selection = 0;  // 選擇 擲杯
            ui_mode_changed = true;
        }
        // 2. 六合彩指令
        else if (text.find("六合彩") != std::string::npos ||
                 text.find("六合") != std::string::npos) {
            ESP_LOGI("VoiceCmd", "語音指令：立即跳轉至 六合彩");
            current_system_mode = 2;
            current_ui_mode = 2;
            game_menu_selection = 1;  // 選擇 六合彩
            ui_mode_changed = true;
        }
        // 3. 太空侵略者指令
        else if (text.find("太空") != std::string::npos || text.find("飛機") != std::string::npos ||
                 text.find("飞机") != std::string::npos) {
            ESP_LOGI("VoiceCmd", "語音指令：立即跳轉至 太空侵略者");
            current_system_mode = 2;
            current_ui_mode = 2;
            game_menu_selection = 2;  // 選擇 Space
            ui_mode_changed = true;
        }
        // 4. 打磚塊指令
        else if (text.find("磚塊") != std::string::npos || text.find("砖块") != std::string::npos ||
                 text.find("breakout") != std::string::npos) {
            ESP_LOGI("VoiceCmd", "語音指令：立即跳轉至 打磚塊");
            current_system_mode = 2;
            current_ui_mode = 2;
            game_menu_selection = 5;  // 選擇 Breakout
            ui_mode_changed = true;
        }
        // 5. 2048 指令 (同時支援阿拉伯數字「2048」與中文「二零四八」)
        else if (text.find("2048") != std::string::npos ||
                 text.find("二零四八") != std::string::npos) {
            ESP_LOGI("VoiceCmd", "語音指令：立即跳轉至 2048");
            current_system_mode = 2;  // 進入遊戲模式
            current_ui_mode = 2;
            game_menu_selection = 3;  // 選擇 2048
            ui_mode_changed = true;   // 立刻重繪畫面！
        }
        // 6. 貪食蛇指令
        else if (text.find("蛇") != std::string::npos || text.find("snake") != std::string::npos ||
                 text.find("Snake") != std::string::npos) {
            ESP_LOGI("VoiceCmd", "語音指令：立即跳轉至 貪食蛇");
            current_system_mode = 2;
            current_ui_mode = 2;
            game_menu_selection = 4;  // 選擇 貪食蛇
            ui_mode_changed = true;
        }
        // 6. 時鐘模式指令 (支援繁簡「時間」、「時鐘」、「睇時間」、「幾點」)
        else if (text.find("時間模式") != std::string::npos ||
                 text.find("时间模式") != std::string::npos ||
                 text.find("時鐘模式") != std::string::npos ||
                 text.find("时钟模式") != std::string::npos) {
            ESP_LOGI("VoiceCmd", "語音指令：立即跳轉至 時鐘待機模式");
            current_system_mode = 3;
            current_ui_mode = 3;
            auto backlight = Board::GetInstance().GetBacklight();
            if (backlight)
                backlight->SetBrightness(5);  // 自動變暗
            ui_mode_changed = true;
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
    }
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

        // 1. 暴力破解法：先把「所有圖層」通通隱藏！
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
        if (game_menu_bg)
            lv_obj_add_flag(game_menu_bg, LV_OBJ_FLAG_HIDDEN);
        if (bg_2048)
            lv_obj_add_flag(bg_2048, LV_OBJ_FLAG_HIDDEN);
        if (bg_snake)
            lv_obj_add_flag(bg_snake, LV_OBJ_FLAG_HIDDEN);
        if (bg_bwa)
            lv_obj_add_flag(bg_bwa, LV_OBJ_FLAG_HIDDEN);
        if (bg_marksix)
            lv_obj_add_flag(bg_marksix, LV_OBJ_FLAG_HIDDEN);
        if (bg_space)
            lv_obj_add_flag(bg_space, LV_OBJ_FLAG_HIDDEN);
        if (bg_breakout)
            lv_obj_add_flag(bg_breakout, LV_OBJ_FLAG_HIDDEN);
        if (bg_clock)
            lv_obj_add_flag(bg_clock, LV_OBJ_FLAG_HIDDEN);

        // 2. 根據現在的模式，把對應的圖層打開
        if (current_ui_mode == 1) {  // --- 進入選單 ---
            if (!game_menu_bg) {     /* 建立選單的代碼... 這裡省略，保留你原本建立選單那幾行 */
                game_menu_bg = lv_obj_create(lv_scr_act());
                lv_obj_set_size(game_menu_bg, LCD_W, LCD_H);
                lv_obj_set_style_bg_color(game_menu_bg, lv_color_black(), 0);
                lv_obj_align(game_menu_bg, LV_ALIGN_CENTER, 0, 0);

                // 徹底去除邊框與圓角，讓黑色填滿整個螢幕！
                lv_obj_set_style_border_width(game_menu_bg, 0, 0);  // 邊框寬度設為 0
                lv_obj_set_style_radius(game_menu_bg, 0, 0);        // 取消圓角，變正方形
                lv_obj_set_style_pad_all(game_menu_bg, 0, 0);       // 內外邊距歸零

                lv_obj_t* title = lv_label_create(game_menu_bg);
                lv_label_set_text(title, "GAME MODE");
                lv_obj_set_style_text_color(title, lv_color_make(255, 230, 0), 0);
                lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 5);

                game_menu_label = lv_label_create(game_menu_bg);
                lv_obj_align(game_menu_label, LV_ALIGN_CENTER, 0, 0);
            }
            lv_obj_remove_flag(game_menu_bg, LV_OBJ_FLAG_HIDDEN);
            const char* menu_texts[] = {"Moon\nBlock", "Mark 6", "Space\nInvaders",
                                        "Breakout",    "2048",   "Snake"};
            lv_label_set_text(game_menu_label, menu_texts[game_menu_selection]);
            lv_obj_set_style_text_font(game_menu_label, &lv_font_montserrat_28, 0);
            lv_obj_set_style_text_line_space(game_menu_label, -4, 0);
            lv_obj_set_style_text_align(game_menu_label, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_color(game_menu_label, lv_color_white(), 0);
            // lv_obj_set_style_transform_zoom(game_menu_label, 192, 0);
            // lv_obj_set_style_transform_pivot_x(game_menu_label, 0, 0);
            // lv_obj_set_style_transform_pivot_y(game_menu_label, 0, 0);

            return;

        } else if (current_ui_mode == 2) {  // --- 進入遊戲 ---
            if (game_menu_selection == 0) {
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

                if (!imu_initialized)
                    imu_init();
                return;
            } else if (game_menu_selection == 1) {
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
                    lv_label_set_text(title, "★ MARK SIX ★");
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

                if (!imu_initialized)
                    imu_init();
                return;
            } else if (game_menu_selection == 2) {
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
                        lv_obj_set_size(space_bombs[i].obj, 5, 5);  // 炸彈大小
                        lv_obj_set_style_bg_color(space_bombs[i].obj, lv_color_hex(0xFF33FF),
                                                  0);  // 紫紅色，警示感強
                        lv_obj_set_style_radius(space_bombs[i].obj, LV_RADIUS_CIRCLE,
                                                0);  // 圓形炸彈
                        lv_obj_set_style_border_width(space_bombs[i].obj, 0, 0);
                        lv_obj_clear_flag(space_bombs[i].obj, LV_OBJ_FLAG_SCROLLABLE);
                        lv_obj_add_flag(space_bombs[i].obj, LV_OBJ_FLAG_HIDDEN);
                    }

                    // 4. 外星人物件池 (3x4 = 12隻)
                    for (int r = 0; r < SPACE_ALIEN_ROWS; r++) {
                        for (int c = 0; c < SPACE_ALIEN_COLS; c++) {
                            space_aliens[r][c].active = true;
                            space_aliens[r][c].obj = lv_obj_create(bg_space);
                            lv_obj_set_size(space_aliens[r][c].obj, 10, 10);
                            lv_obj_set_style_bg_color(space_aliens[r][c].obj,
                                                      lv_color_hex(0xFF3333), 0);  // 紅色
                            lv_obj_set_style_radius(space_aliens[r][c].obj, 2, 0);
                            lv_obj_set_style_border_width(space_aliens[r][c].obj, 0, 0);
                            lv_obj_clear_flag(space_aliens[r][c].obj, LV_OBJ_FLAG_SCROLLABLE);
                            // 初始排列
                            lv_obj_align(space_aliens[r][c].obj, LV_ALIGN_TOP_LEFT, 15 + c * 25,
                                         20 + r * 15);
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
                    lv_obj_set_style_text_align(space_overlay_label, LV_TEXT_ALIGN_CENTER, 0);
                    lv_obj_align(space_overlay_label, LV_ALIGN_CENTER, 0, 0);
                }

                lv_obj_remove_flag(bg_space, LV_OBJ_FLAG_HIDDEN);
                space_game_state = SPACE_STATE_START;
                lv_label_set_text_fmt(space_overlay_label, "SPACE\n\nBEST: %d\n\n[CLICK START]",
                                      space_best_score);
                lv_label_set_text_fmt(space_score_label, "S:%d  B:%d", space_score,
                                      space_best_score);
                lv_obj_remove_flag(space_overlay, LV_OBJ_FLAG_HIDDEN);

                if (!imu_initialized)
                    imu_init();
                return;
            } else if (game_menu_selection == 3) {
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

                    // 磚塊陣列 (4行 x 5列)
                    uint32_t row_colors[] = {0xFF3333, 0xFF9933, 0x33FF33,
                                             0x3333FF};  // 紅、橘、綠、藍
                    for (int r = 0; r < BK_ROWS; r++) {
                        for (int c = 0; c < BK_COLS; c++) {
                            bk_bricks[r][c] = lv_obj_create(bg_breakout);
                            lv_obj_set_size(bk_bricks[r][c], 23, 8);  // 寬 23，高 8
                            lv_obj_set_style_bg_color(bk_bricks[r][c], lv_color_hex(row_colors[r]),
                                                      0);
                            lv_obj_set_style_radius(bk_bricks[r][c], 1, 0);
                            lv_obj_set_style_border_width(bk_bricks[r][c], 0, 0);
                            lv_obj_clear_flag(bk_bricks[r][c], LV_OBJ_FLAG_SCROLLABLE);
                            // 排版計算：間距 2px，起點 X=2, Y=18
                            lv_obj_align(bk_bricks[r][c], LV_ALIGN_TOP_LEFT, 2 + c * 25,
                                         18 + r * 10);
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
                    lv_obj_set_style_text_align(bk_overlay_label, LV_TEXT_ALIGN_CENTER, 0);
                    lv_obj_align(bk_overlay_label, LV_ALIGN_CENTER, 0, 0);
                }

                lv_obj_remove_flag(bg_breakout, LV_OBJ_FLAG_HIDDEN);
                bk_game_state = BK_STATE_START;
                init_breakout_level(true);  // 初始化第一關
                lv_label_set_text_fmt(bk_overlay_label, "BREAKOUT\n\nBEST: %d\n\n[CLICK]",
                                      bk_best_score);
                lv_label_set_text_fmt(bk_score_label, "L:%d  S:%d", bk_level, bk_score);
                lv_obj_remove_flag(bk_overlay, LV_OBJ_FLAG_HIDDEN);

                if (!imu_initialized)
                    imu_init();
                return;
            } else if (game_menu_selection == 4) {
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
                    lv_obj_set_style_text_align(overlay_label, LV_TEXT_ALIGN_CENTER, 0);
                    lv_obj_align(overlay_label, LV_ALIGN_CENTER, 0, 0);
                }
                lv_obj_remove_flag(bg_2048, LV_OBJ_FLAG_HIDDEN);

                // 每次新切入 2048，先進入 START 畫面
                game_state_2048 = STATE_START_SCREEN;
                lv_label_set_text_fmt(overlay_label, "2048\n\nBEST: %d\n\n[CLICK START]",
                                      best_score);
                lv_obj_remove_flag(overlay_2048, LV_OBJ_FLAG_HIDDEN);

            } else if (game_menu_selection == 5) {
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
                    lv_obj_set_style_text_align(snake_overlay_label, LV_TEXT_ALIGN_CENTER, 0);
                    lv_obj_align(snake_overlay_label, LV_ALIGN_CENTER, 0, 0);
                }
                lv_obj_remove_flag(bg_snake, LV_OBJ_FLAG_HIDDEN);
                snake_game_state = SNAKE_STATE_START;
                lv_label_set_text_fmt(snake_overlay_label, "SNAKE\n\nBEST: %d\n\n[CLICK START]",
                                      snake_best_score);
                lv_obj_remove_flag(snake_overlay, LV_OBJ_FLAG_HIDDEN);
            }

            if (!imu_initialized)
                imu_init();
            return;
        } else if (current_ui_mode == 3) {
            // ==================== 【獨立時鐘模式】 ====================
            if (!bg_clock) {
                bg_clock = lv_obj_create(lv_scr_act());
                lv_obj_set_size(bg_clock, LCD_W, LCD_H);
                lv_obj_set_style_bg_color(bg_clock, lv_color_black(), 0);
                lv_obj_set_style_border_width(bg_clock, 0, 0);
                lv_obj_set_style_radius(bg_clock, 0, 0);
                lv_obj_set_style_pad_all(bg_clock, 0, 0);

                clock_time_label = lv_label_create(bg_clock);
                lv_obj_set_style_text_color(clock_time_label, lv_color_white(), 0);
                lv_obj_set_style_text_font(clock_time_label, &lv_font_montserrat_28, 0);
                lv_obj_align(clock_time_label, LV_ALIGN_TOP_LEFT, 10, 10);  // 靠左 6，靠上 10

                clock_date_label = lv_label_create(bg_clock);
                lv_obj_set_style_text_color(clock_date_label, lv_color_white(), 0);
                // lv_obj_set_style_text_color(clock_date_label, lv_color_hex(0xAAAAAA), 0);
                lv_obj_align(clock_date_label, LV_ALIGN_TOP_LEFT, 9, 46);  // 靠下對齊

                // 建立天氣標籤 (固定在正下方)
                clock_weather_label = lv_label_create(bg_clock);
                // lv_obj_set_style_text_color(clock_weather_label, lv_color_white(), 0);
                lv_obj_set_style_text_color(clock_weather_label, lv_color_make(255, 230, 0), 0);
                //  文字靠左
                lv_obj_set_style_text_align(clock_weather_label, LV_TEXT_ALIGN_LEFT, 0);
                lv_obj_set_style_text_line_space(clock_weather_label, -2, 0);
                lv_obj_align(clock_weather_label, LV_ALIGN_TOP_LEFT, 9, 69);  // 日期下方

                // 256是100%，192是75%
                // lv_obj_set_style_transform_zoom(clock_weather_label, 192, 0);
                // lv_obj_set_style_transform_pivot_x(clock_weather_label, 0, 0);
                // lv_obj_set_style_transform_pivot_y(clock_weather_label, 0, 0);
            }
            lv_obj_remove_flag(bg_clock, LV_OBJ_FLAG_HIDDEN);

            // 1. 立即更新時間
            update_clock_display();

            // =========================================================================
            // 檢查是否超過 30 秒，若是則立即拍醒背景工人去抓最新天氣！
            // =========================================================================
            int64_t now_us = esp_timer_get_time();
            if (weather_task_handle &&
                (last_weather_fetch_us == 0 || now_us - last_weather_fetch_us > 30000000LL)) {
                last_weather_fetch_us = now_us;
                xTaskNotifyGive(weather_task_handle);  // 拍醒工人！
                ESP_LOGI("Weather", "進入時鐘模式，觸發背景即時天氣更新！");
            }
            // =========================================================================

            // 調暗螢幕至 10%
            clock_is_bright = false;
            clock_dim_countdown = 0;
            auto backlight = Board::GetInstance().GetBacklight();
            if (backlight)
                backlight->SetBrightness(10);

            return;
        } else if (current_ui_mode == 0) {  // --- 退出遊戲，回到 AI ---
            // 恢復臉部顯示
            lv_obj_remove_flag(avatar_left_eye, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(avatar_right_eye, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(avatar_mouth, LV_OBJ_FLAG_HIDDEN);
            last_avatar_state = (DeviceState)-1;  // 強制刷新臉部
        }
    }

    // ==============================================
    // 貪食蛇自動前進引擎 (每 200ms 走一步)
    // ==============================================
    if (current_ui_mode == 2 && game_menu_selection == 5 &&
        snake_game_state == SNAKE_STATE_PLAYING) {
        static int snake_tick_counter = 0;
        if (++snake_tick_counter >= 2) {  // 2 * 100ms = 200ms 前進一格
            snake_tick_counter = 0;

            bool alive = snake_tick_step();  // 蛇向前爬一步
            update_snake_ui();               // 刷新畫面

            // 如果撞牆或咬到自己
            if (!alive) {
                snake_game_state = SNAKE_STATE_GAMEOVER;
                lv_label_set_text_fmt(snake_overlay_label, "GAME OVER\n\nSCORE: %d\n\n[CLICK]",
                                      snake_score);
                lv_obj_remove_flag(snake_overlay, LV_OBJ_FLAG_HIDDEN);
                ESP_LOGI("Snake", "Game Over! 最終得分: %d", snake_score);
            }
        }
    }

    // --- 時鐘模式：每 1 秒更新一次時間文字。支援點亮、手動/自動 3 秒變暗 ---
    if (current_ui_mode == 3) {
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
                    backlight->SetBrightness(5);
                clock_is_bright = false;
                clock_dim_countdown = 0;
                ESP_LOGI("Clock", "手動變暗！");
            }
            pending_game_action = 0;  // 清除訊號
        }

        // 2. 處理 5 秒自動變暗倒數
        if (clock_is_bright && clock_dim_countdown > 0) {
            clock_dim_countdown--;
            if (clock_dim_countdown == 0) {
                // 5 秒到了，沒有新動作 -> 自動降回 5%
                if (backlight)
                    backlight->SetBrightness(5);
                clock_is_bright = false;
                ESP_LOGI("Clock", "5 秒無操作，自動變暗");
            }
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
    if (current_ui_mode == 2 && imu_initialized) {
        static bool tilt_locked_2048 = false;
        uint8_t reg = BMI270_REG_DATA_X_LSB;  // 0x0C
        uint8_t data[6] = {0};

        esp_err_t res = i2c_master_transmit_receive(imu_dev_handle, &reg, 1, data, 6, -1);
        if (res == ESP_OK) {
            int16_t accel_x = (int16_t)((data[1] << 8) | data[0]);
            int16_t accel_y = (int16_t)((data[3] << 8) | data[2]);
            int16_t accel_z = (int16_t)((data[5] << 8) | data[4]);

            // 傾斜靈敏度閾值，傾斜靈敏度閾值，數值越小越靈敏
            const int16_t THRESHOLD_2048 = 2500;
            const int16_t THRESHOLD_SNAKE = 2000;

            // ----------------------------------------------------
            // 模式 A：擲杯 搖晃感應 (Shake Detection)
            // ----------------------------------------------------
            if (game_menu_selection == 0) {
                static int16_t prev_x = 0, prev_y = 0, prev_z = 0;
                int delta = abs(accel_x - prev_x) + abs(accel_y - prev_y) + abs(accel_z - prev_z);
                prev_x = accel_x;
                prev_y = accel_y;
                prev_z = accel_z;

                if (delta > 18000 && bwa_roll_anim_ticks == 0) {
                    bwa_roll_anim_ticks = 16;
                    ESP_LOGI("BwaPei", "偵測到搖晃！開始擲杯！");
                }
            }
            // ----------------------------------------------------
            // 模式 B：六合彩 搖晃抽波
            // ----------------------------------------------------
            else if (game_menu_selection == 1) {
                static int16_t prev_m6_x = 0, prev_m6_y = 0, prev_m6_z = 0;
                int delta =
                    abs(accel_x - prev_m6_x) + abs(accel_y - prev_m6_y) + abs(accel_z - prev_m6_z);
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
            }
            // ----------------------------------------------------
            // 模式 C：【新增】太空侵略者 (Space) 體感飛船控制
            // ----------------------------------------------------
            else if (game_menu_selection == 2 && space_game_state == SPACE_STATE_PLAYING) {
                // 【核心優化】降低死區，提升靈敏度！
                const int16_t DEADZONE = 600;  // 原本是 1500，現在只要稍微傾斜就能動！

                // 動態變速：如果傾斜角度超大 (>5000)，飛船速度加倍 (7px)，否則一般速度 (4px)
                int ship_speed = (abs(accel_y) > 5000) ? 12 : 6;

                if (accel_y < -DEADZONE) {
                    space_ship_x += ship_speed;  // 往右飛
                } else if (accel_y > DEADZONE) {
                    space_ship_x -= ship_speed;  // 往左飛
                }

                // 邊界防護
                if (space_ship_x < 0)
                    space_ship_x = 0;
                if (space_ship_x > LCD_W - 16)
                    space_ship_x = LCD_W - 16;

                if (space_ship) {
                    lv_obj_align(space_ship, LV_ALIGN_TOP_LEFT, space_ship_x, LCD_H * 0.9);
                }
            }
            // ----------------------------------------------------
            // 模式 D：【新增】打磚塊 (Breakout) 體感擋板控制
            // ----------------------------------------------------
            else if (game_menu_selection == 3 && bk_game_state == BK_STATE_PLAYING) {
                const int16_t DEADZONE = 600;
                // 【修改這裡】將速度大幅提升！
                // 輕微傾斜時：每次移動 6 像素 (原本 4)
                // 用力傾斜時：每次瞬間衝刺 12 像素 (原本 8)
                int pad_speed = (abs(accel_y) > 5000) ? 12 : 6;  // 動態感度

                if (accel_y < -DEADZONE)
                    bk_paddle_x += pad_speed;
                else if (accel_y > DEADZONE)
                    bk_paddle_x -= pad_speed;

                // 邊界防護 (螢幕寬 128 - 擋板寬)
                if (bk_paddle_x < 0)
                    bk_paddle_x = 0;
                if (bk_paddle_x > LCD_W - bk_paddle_w)
                    bk_paddle_x = LCD_W - bk_paddle_w;

                if (bk_paddle)
                    lv_obj_align(bk_paddle, LV_ALIGN_TOP_LEFT, bk_paddle_x, LCD_H * 0.9);
            }

            // ----------------------------------------------------
            // 模式 E：2048 體感 (需要鎖定放平)
            // ----------------------------------------------------
            else if (game_menu_selection == 4) {
                if (abs(accel_x) > THRESHOLD_2048 || abs(accel_y) > THRESHOLD_2048) {
                    if (!tilt_locked_2048) {
                        if (abs(accel_x) > abs(accel_y)) {
                            if (accel_x > 0)
                                pending_game_action = 3;  // 上
                            else
                                pending_game_action = 4;  // 下
                        } else {
                            if (accel_y < 0)
                                pending_game_action = 2;  // 右
                            else
                                pending_game_action = 1;  // 左
                        }
                        tilt_locked_2048 = true;
                    }
                } else {
                    tilt_locked_2048 = false;  // 放平回正
                }
            }
            // ----------------------------------------------------
            // 模式 F：貪食蛇體感 (即時方向盤，無須放平鎖定)
            // ----------------------------------------------------
            else if (game_menu_selection == 5 && snake_game_state == SNAKE_STATE_PLAYING) {
                if (abs(accel_x) > THRESHOLD_SNAKE || abs(accel_y) > THRESHOLD_SNAKE) {
                    // 採用與 2048 完全相同的經過校驗的軸向對應
                    if (abs(accel_x) > abs(accel_y)) {
                        if (accel_x > 0)
                            snake_set_direction(DIR_UP);  // 上
                        else
                            snake_set_direction(DIR_DOWN);  // 下
                    } else {
                        if (accel_y < 0)
                            snake_set_direction(DIR_RIGHT);  // 右
                        else
                            snake_set_direction(DIR_LEFT);  // 左
                    }
                }
            }
        }
    }

    // 安全地在 LVGL 執行緒裡處理遊戲動作 (方向 1~4，按鍵動作 5)
    if (current_ui_mode == 2 && pending_game_action != 0) {
        if (pending_game_action == 1)
            move_2048_left();
        else if (pending_game_action == 2)
            move_2048_right();
        else if (pending_game_action == 3)
            move_2048_up();
        else if (pending_game_action == 4)
            move_2048_down();
        else if (pending_game_action == 5) {
            if (game_menu_selection == 0) {
                // 短按螢幕也可以直接觸發擲杯！
                if (bwa_roll_anim_ticks == 0)
                    bwa_roll_anim_ticks = 8;
            } else if (game_menu_selection == 1) {
                // 不想搖晃時，短按一下螢幕也可以抽一顆 (或重置)
                draw_one_mark_six_ball();
            } else if (game_menu_selection == 2) {
                // ==================== Space 按鍵邏輯 ====================
                if (space_game_state == SPACE_STATE_START ||
                    space_game_state == SPACE_STATE_GAMEOVER) {
                    space_game_state = SPACE_STATE_PLAYING;
                    if (space_overlay)
                        lv_obj_add_flag(space_overlay, LV_OBJ_FLAG_HIDDEN);
                    space_score = 0;
                    space_ship_x = 64;
                    alien_group_x = 0;
                    alien_group_y = 20;
                    alien_dir = 1;

                    // 重置外星人與子彈
                    for (int r = 0; r < SPACE_ALIEN_ROWS; r++) {
                        for (int c = 0; c < SPACE_ALIEN_COLS; c++) {
                            space_aliens[r][c].active = true;
                            lv_obj_remove_flag(space_aliens[r][c].obj, LV_OBJ_FLAG_HIDDEN);
                            lv_obj_align(space_aliens[r][c].obj, LV_ALIGN_TOP_LEFT, 15 + c * 25,
                                         20 + r * 15);
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

                    lv_label_set_text_fmt(space_score_label, "S:%d  B:%d", space_score,
                                          space_best_score);
                    ESP_LOGI("Space", "太空侵略者 出發！");
                }
            } else if (game_menu_selection == 3) {
                // ==================== Breakout 按鍵邏輯 ====================
                if (bk_game_state == BK_STATE_START || bk_game_state == BK_STATE_GAMEOVER) {
                    bk_game_state = BK_STATE_PLAYING;
                    if (bk_overlay)
                        lv_obj_add_flag(bk_overlay, LV_OBJ_FLAG_HIDDEN);
                    init_breakout_level(true);  // 新遊戲
                    lv_label_set_text_fmt(bk_score_label, "L:%d  S:%d", bk_level, bk_score);
                    ESP_LOGI("Breakout", "打磚塊 出發！");
                } else if (bk_game_state == BK_STATE_PLAYING) {
                    // 如果球還沒發射，按一下螢幕發射！
                    if (!bk_ball_launched) {
                        bk_ball_launched = true;
                        ESP_LOGI("Breakout", "發射！");
                    }
                }
            } else if (game_menu_selection == 4) {
                // 2048 按鍵邏輯 (保留原樣)
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
                    ESP_LOGI("2048", "遊戲正式開始！");
                } else if (game_state_2048 == STATE_PLAYING) {
                    undo_2048();
                } else if (game_state_2048 == STATE_GAMEOVER) {
                    game_state_2048 = STATE_PLAYING;
                    if (overlay_2048)
                        lv_obj_add_flag(overlay_2048, LV_OBJ_FLAG_HIDDEN);
                    memset(board_2048, 0, sizeof(board_2048));
                    current_score = 0;
                    can_undo = false;
                    add_random_2048();
                    add_random_2048();
                    update_2048_ui();
                    ESP_LOGI("2048", "重新開局！");
                }
            } else if (game_menu_selection == 5) {
                // ==================== 貪食蛇 按鍵完整控制 ====================
                if (snake_game_state == SNAKE_STATE_START) {
                    // 1. 開始遊戲
                    snake_game_state = SNAKE_STATE_PLAYING;
                    if (snake_overlay)
                        lv_obj_add_flag(snake_overlay, LV_OBJ_FLAG_HIDDEN);
                    snake_init_game();
                    update_snake_ui();
                } else if (snake_game_state == SNAKE_STATE_PLAYING) {
                    // 2. 遊玩中短按：暫停遊戲
                    snake_game_state = SNAKE_STATE_PAUSE;
                    lv_label_set_text_fmt(snake_overlay_label,
                                          "PAUSED\n\nSCORE: %d\n\n[CLICK RESUME]", snake_score);
                    lv_obj_remove_flag(snake_overlay, LV_OBJ_FLAG_HIDDEN);
                } else if (snake_game_state == SNAKE_STATE_PAUSE) {
                    // 3. 暫停中短按：繼續遊玩
                    snake_game_state = SNAKE_STATE_PLAYING;
                    if (snake_overlay)
                        lv_obj_add_flag(snake_overlay, LV_OBJ_FLAG_HIDDEN);
                } else if (snake_game_state == SNAKE_STATE_GAMEOVER) {
                    // 4. 撞牆後短按：重新開局
                    snake_game_state = SNAKE_STATE_PLAYING;
                    if (snake_overlay)
                        lv_obj_add_flag(snake_overlay, LV_OBJ_FLAG_HIDDEN);
                    snake_init_game();
                    update_snake_ui();
                }
            }
        }
        pending_game_action = 0;  // 執行完畢，重設訊號
    }

    // --- 擲杯滾動動畫引擎 ---
    if (current_ui_mode == 2 && game_menu_selection == 0 && bwa_roll_anim_ticks > 0) {
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
    // 太空侵略者 (Space) 遊戲核心引擎
    // ==============================================
    if (current_ui_mode == 2 && game_menu_selection == 2 &&
        space_game_state == SPACE_STATE_PLAYING) {
        static int space_fire_tick = 0;
        static int space_alien_tick = 0;
        static int alien_speed = 10;  // 外星人移動速度 (數值越小越快)

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
            for (int r = 0; r < SPACE_ALIEN_ROWS; r++) {
                for (int c = 0; c < SPACE_ALIEN_COLS; c++) {
                    if (!space_aliens[r][c].active)
                        continue;

                    int ax = alien_group_x + 15 + c * 25;
                    int ay = alien_group_y + r * 15;

                    // 簡單的 AABB 碰撞算法 (子彈 4x8，外星人 10x10)
                    if (space_bullets[i].x < ax + 10 && space_bullets[i].x + 4 > ax &&
                        space_bullets[i].y < ay + 10 && space_bullets[i].y + 8 > ay) {
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
        // 2.5 外星人投擲炸彈 (360分之後開啟)
        // ------------------------------------------
        if (space_score >= 240) {
            // 每 100ms 有 5% 機率投彈 (平均每 2 秒一顆，頻率極低不刁難)
            if (esp_random() % 100 < 8) {
                for (int i = 0; i < SPACE_MAX_BOMBS; i++) {
                    if (!space_bombs[i].active) {
                        // 隨機挑選一隻存活的外星人投彈
                        int r = esp_random() % SPACE_ALIEN_ROWS;
                        int c = esp_random() % SPACE_ALIEN_COLS;
                        if (space_aliens[r][c].active) {
                            space_bombs[i].active = true;
                            space_bombs[i].x = alien_group_x + 15 + c * 25 + 2;  // 對齊外星人中心
                            space_bombs[i].y = alien_group_y + r * 15 + 10;
                            lv_obj_remove_flag(space_bombs[i].obj, LV_OBJ_FLAG_HIDDEN);
                            lv_obj_align(space_bombs[i].obj, LV_ALIGN_TOP_LEFT, space_bombs[i].x,
                                         space_bombs[i].y);
                            break;  // 每次只投一顆
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
            for (int r = 0; r < SPACE_ALIEN_ROWS; r++) {
                for (int c = 0; c < SPACE_ALIEN_COLS; c++) {
                    if (space_aliens[r][c].active) {
                        active_aliens++;
                        int ax = alien_group_x + 15 + c * 25;
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
                // 【全部清空】進入下一關！速度變快！
                alien_group_x = 0;
                alien_group_y = 20;
                if (alien_speed > 2)
                    alien_speed -= 2;
                for (int r = 0; r < SPACE_ALIEN_ROWS; r++) {
                    for (int c = 0; c < SPACE_ALIEN_COLS; c++) {
                        space_aliens[r][c].active = true;
                        lv_obj_remove_flag(space_aliens[r][c].obj, LV_OBJ_FLAG_HIDDEN);
                    }
                }
            } else {
                // 碰壁轉向與向下逼近
                if (alien_dir == 1 && max_x + 10 >= (LCD_W - 4)) {  // 124
                    alien_dir = -1;
                    alien_group_y += 8;
                } else if (alien_dir == -1 && min_x <= 4) {
                    alien_dir = 1;
                    alien_group_y += 8;
                } else {
                    alien_group_x += alien_dir * 4;
                }

                // 【Game Over 判定】外星人是否碰到底部？
                if (bottom_alien_y + 10 >= 110) {
                    game_over = true;
                } else {
                    // 更新所有存活外星人的畫面位置
                    for (int r = 0; r < SPACE_ALIEN_ROWS; r++) {
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
        // 4. Game Over 與最高分儲存
        // ------------------------------------------
        if (game_over) {
            space_game_state = SPACE_STATE_GAMEOVER;

            // 破紀錄寫入 NVS！
            if (space_score > space_best_score) {
                space_best_score = space_score;
                save_space_best_score(space_best_score);
            }

            lv_label_set_text_fmt(space_overlay_label, "GAME OVER\n\nSCORE: %d\n\n[CLICK]",
                                  space_score);
            lv_obj_remove_flag(space_overlay, LV_OBJ_FLAG_HIDDEN);
            ESP_LOGI("Space", "Game Over! 最終得分: %d", space_score);
        }
    }

    // ==============================================
    // 打磚塊 (Breakout) 遊戲核心引擎
    // ==============================================
    if (current_ui_mode == 2 && game_menu_selection == 3 && bk_game_state == BK_STATE_PLAYING) {
        if (!bk_ball_launched) {
            // 球還沒發射，黏在擋板上跟著移動！
            bk_ball_x = bk_paddle_x + (bk_paddle_w / 2) - 3;
            bk_ball_y = 110;
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
                bk_ball_dy = -abs(bk_ball_dy);
            }  // 頂部保留給計分板

            // 3. 掉落底部 (Game Over)
            if (bk_ball_y >= LCD_H) {
                bk_game_state = BK_STATE_GAMEOVER;
                if (bk_score > bk_best_score) {
                    bk_best_score = bk_score;
                    save_bk_best_score(bk_best_score);
                }
                lv_label_set_text_fmt(bk_overlay_label, "GAME OVER\n\nSCORE: %d\n\n[CLICK]",
                                      bk_score);
                lv_obj_remove_flag(bk_overlay, LV_OBJ_FLAG_HIDDEN);
            }

            // 4. 擋板碰撞反彈 (球寬高 6)
            if (bk_ball_y + 6 >= LCD_H * 0.9 && bk_ball_y < LCD_H * 0.9 + 4 && bk_ball_dy > 0) {
                if (bk_ball_x + 6 >= bk_paddle_x && bk_ball_x <= bk_paddle_x + bk_paddle_w) {
                    bk_ball_dy = -abs(bk_ball_dy);  // 強制向上反彈
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

            for (int r = 0; r < BK_ROWS; r++) {
                for (int c = 0; c < BK_COLS; c++) {
                    if (bk_bricks_active[r][c]) {
                        remaining_bricks++;
                        if (!hit_brick) {
                            int brx = 2 + c * 25;
                            int bry = 18 + r * 10;
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
        if (idle_to_clock_counter >= 6000) {  // 6000 * 100ms = 60 秒 = 5 分鐘
            idle_to_clock_counter = 0;
            ESP_LOGI("System", "1 分鐘無操作，自動進入時鐘待機模式");

            current_system_mode = 3;
            current_ui_mode = 3;
            ui_mode_changed = true;

            // 將背光降至 5% (極致省電)
            if (backlight)
                backlight->SetBrightness(5);
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
        lv_obj_set_style_radius(avatar_left_eye, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_size(avatar_right_eye, EYE_BASE_W, EYE_BASE_W);
        lv_obj_align(avatar_right_eye, LV_ALIGN_CENTER, EYE_POS_R, EYE_POS_H);
        lv_obj_set_style_radius(avatar_right_eye, LV_RADIUS_CIRCLE, 0);
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
            lv_obj_set_style_radius(avatar_left_eye, LV_RADIUS_CIRCLE, 0);
            lv_obj_align(avatar_right_eye, LV_ALIGN_CENTER, EYE_POS_R, EYE_POS_H);
            lv_obj_set_style_radius(avatar_right_eye, LV_RADIUS_CIRCLE, 0);
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
            backlight->SetBrightness(10);

        current_active_emotion = "neutral";  // 重置回普通待機
        set_eye_size(EYE_BASE_W, EYE_BASE_H);
        lv_obj_set_style_radius(avatar_left_eye, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_radius(avatar_right_eye, LV_RADIUS_CIRCLE, 0);

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

    avatar_theme_bg_color = lv_obj_get_style_bg_color(parent, LV_PART_MAIN);
    avatar_theme_color = lv_obj_get_style_text_color(parent, LV_PART_MAIN);

    // lv_obj_set_style_bg_color(parent, avatar_theme_bg_color, 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    // 設定眼睛和嘴巴的初始位置
    avatar_left_eye = lv_obj_create(parent);
    lv_obj_set_size(avatar_left_eye, EYE_BASE_W, EYE_BASE_H);
    lv_obj_set_style_radius(avatar_left_eye, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(avatar_left_eye, avatar_theme_color, 0);
    lv_obj_clear_flag(avatar_left_eye, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(avatar_left_eye, LV_ALIGN_CENTER, EYE_POS_L, EYE_POS_H);

    avatar_right_eye = lv_obj_create(parent);
    lv_obj_set_size(avatar_right_eye, EYE_BASE_W, EYE_BASE_H);
    lv_obj_set_style_radius(avatar_right_eye, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(avatar_right_eye, avatar_theme_color, 0);
    lv_obj_clear_flag(avatar_right_eye, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(avatar_right_eye, LV_ALIGN_CENTER, EYE_POS_R, EYE_POS_H);

    avatar_mouth = lv_obj_create(parent);
    lv_obj_set_size(avatar_mouth, MOUTH_BASE_W, MOUTH_BASE_H);
    lv_obj_set_style_radius(avatar_mouth, 8, 0);
    lv_obj_set_style_bg_color(avatar_mouth, avatar_theme_color, 0);
    lv_obj_clear_flag(avatar_mouth, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(avatar_mouth, LV_ALIGN_CENTER, 0, MOUTH_POS_H);

    // ==============================================
    // sad eye
    // ==============================================
    avatar_sad_eye = lv_obj_create(parent);
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
    avatar_surprised_eye = lv_obj_create(parent);
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
    avatar_angry_eye = lv_obj_create(parent);
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
    avatar_tears = lv_obj_create(parent);
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
    avatar_swallow = lv_obj_create(parent);
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
    avatar_confuse = lv_obj_create(parent);
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
    avatar_cool = lv_obj_create(parent);
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
    avatar_mouth_laugh = lv_obj_create(parent);
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
    avatar_silly_tongue = lv_obj_create(parent);
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
    avatar_sleepy_z = lv_obj_create(parent);
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
    avatar_mouth_happy = lv_obj_create(parent);
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
    avatar_embrassing = lv_obj_create(parent);
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
    lv_obj_set_style_bg_color(embrassing_left, blush_pink, 0);   // 【關鍵】實體顏色設定在子物件上
    lv_obj_set_style_bg_opa(embrassing_left, LV_OPA_COVER, 20);  // 100% 不透明
    lv_obj_set_style_border_width(embrassing_left, 0, 0);
    lv_obj_clear_flag(embrassing_left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(embrassing_left, LV_ALIGN_CENTER, EYE_POS_L - EYE_BASE_W + 8,
                 0);  // 左臉頰座標

    // 3. 右臉頰粉紅橢圓 (位於右眼下方偏右)
    lv_obj_t* embrassing_right = lv_obj_create(avatar_embrassing);
    lv_obj_set_size(embrassing_right, 16, 12);
    lv_obj_set_style_radius(embrassing_right, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(embrassing_right, blush_pink, 0);  // 【關鍵】實體顏色設定在子物件上
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
    avatar_sweat = lv_obj_create(parent);
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
    avatar_heart = lv_obj_create(parent);
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
    avatar_kissy = lv_obj_create(parent);
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
    avatar_anger = lv_obj_create(parent);
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
    lv_obj_align(bus_route_label, LV_ALIGN_TOP_MID, 0, 15);

    bus_t1_label = lv_label_create(bg_bus);
    lv_obj_set_style_text_color(bus_t1_label, lv_color_white(), 0);  // 白色時間
    lv_obj_align(bus_t1_label, LV_ALIGN_CENTER, 0, 5);

    bus_t2_label = lv_label_create(bg_bus);
    lv_obj_set_style_text_color(bus_t2_label, lv_color_hex(0xAAAAAA), 0);  // 灰色後班
    lv_obj_align(bus_t2_label, LV_ALIGN_BOTTOM_MID, 0, -20);

    lv_obj_add_flag(bg_bus, LV_OBJ_FLAG_HIDDEN);  // 預設隱藏

    // 啟動觸摸喚醒任務
    xTaskCreate(touch_wake_task, "touch_task", 4096, NULL, 5, NULL);

    // 建立背景任務
    xTaskCreate(bus_fetch_task, "bus_task", 4096, NULL, 5, &bus_task_handle);
}

static void reset_avatar_geometry() {
    if (avatar_left_eye) {
        lv_obj_set_size(avatar_left_eye, EYE_BASE_W, EYE_BASE_H);
        lv_obj_set_style_radius(avatar_left_eye, EYE_BASE_W / 2, 0);
        lv_obj_align(avatar_left_eye, LV_ALIGN_CENTER, EYE_POS_L, EYE_POS_H);  // 基準左眼
    }
    if (avatar_right_eye) {
        lv_obj_set_size(avatar_right_eye, EYE_BASE_W, EYE_BASE_H);
        lv_obj_set_style_radius(avatar_right_eye, EYE_BASE_W / 2, 0);
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

    lv_obj_remove_flag(avatar_mouth, LV_OBJ_FLAG_HIDDEN);

    if (emo_str == "neutral" || emo_str == "idle" || emo_str == "") {
        current_active_emotion = "neutral";
        last_avatar_state = (DeviceState)-1;
        return;
    }

    if (emo_str == "loving") {
        // 溫柔笑眼
        set_eye_size(EYE_BASE_W - 2, 8);
        lv_obj_align(avatar_left_eye, LV_ALIGN_CENTER, EYE_POS_L, EYE_POS_H - 6);
        lv_obj_align(avatar_right_eye, LV_ALIGN_CENTER, EYE_POS_R, EYE_POS_H - 6);
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
            // 讓眼淚容器從原本位置 (0) 往下掉 (15px)，製造眼淚落下的流動感
            lv_anim_set_values(&a, 0, 10);
            lv_anim_set_time(&a, 300);  // 掉落速度快 (300ms)
            // 不設 playback (不來回反彈)，直接無限重複掉落！
            lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
            lv_anim_start(&a);
        }
    } else if (emo_str == "confused") {
        // 無奈表情：左眼變扁、右眼縮小，嘴巴變成一條扁平橫線
        set_eye_size(EYE_BASE_W + 6, 10);
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
            lv_anim_set_repeat_count(&a, 1);  // 不做 playback，閉上後停在 10px
            lv_anim_start(&a);
        }
    } else if (emo_str == "shocked") {
        set_eye_size(EYE_BASE_W + 2, EYE_BASE_H + 2);
        lv_obj_set_size(avatar_mouth, 20, 30);
        if (avatar_sweat) {
            lv_obj_remove_flag(avatar_sweat, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (emo_str == "happy") {
        set_eye_size(EYE_BASE_W, 8);
        lv_obj_align(avatar_left_eye, LV_ALIGN_CENTER, EYE_POS_L, EYE_POS_H - 6);
        lv_obj_align(avatar_right_eye, LV_ALIGN_CENTER, EYE_POS_R, EYE_POS_H - 6);
        lv_obj_set_size(avatar_mouth, 3, 3);
        lv_obj_set_style_radius(avatar_mouth, 8, 0);
        if (avatar_embrassing) {
            lv_obj_remove_flag(avatar_embrassing, LV_OBJ_FLAG_HIDDEN);
        }
        if (avatar_mouth_happy) {
            lv_obj_remove_flag(avatar_mouth_happy, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (emo_str == "silly") {
        lv_obj_set_size(avatar_left_eye, EYE_BASE_W, EYE_BASE_H);
        lv_obj_set_size(avatar_right_eye, EYE_BASE_W, EYE_BASE_W);
        if (avatar_silly_tongue) {
            lv_obj_remove_flag(avatar_silly_tongue, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_set_size(avatar_mouth, MOUTH_BASE_W + 4, MOUTH_BASE_H);
        lv_obj_set_style_radius(avatar_mouth, 8, 0);
    } else if (emo_str == "laughing") {
        set_eye_size(EYE_BASE_W, 8);
        lv_obj_align(avatar_left_eye, LV_ALIGN_CENTER, EYE_POS_L, EYE_POS_H - 6);
        lv_obj_align(avatar_right_eye, LV_ALIGN_CENTER, EYE_POS_R, EYE_POS_H - 6);
        lv_obj_set_size(avatar_mouth, 1, 1);
        if (avatar_mouth_laugh) {
            lv_obj_remove_flag(avatar_mouth_laugh, LV_OBJ_FLAG_HIDDEN);
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, avatar_mouth_laugh);
            lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
            lv_anim_set_values(&a, MOUTH_POS_H, MOUTH_POS_H + 4);  // 上下震顫跳動 4px
            lv_anim_set_duration(&a, 150);
            lv_anim_set_playback_time(&a, 150);
            lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
            lv_anim_start(&a);
        }
    } else if (emo_str == "cool") {
        set_eye_size(EYE_BASE_W + 2, EYE_BASE_H);
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
    } else if (emo_str == "winking") {
        lv_obj_set_size(avatar_left_eye, EYE_BASE_W, EYE_BASE_H);
        lv_obj_set_size(avatar_mouth, MOUTH_BASE_W - 4, MOUTH_BASE_H);
        lv_obj_set_size(avatar_right_eye, EYE_BASE_W, EYE_BASE_H);

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
    } else if (emo_str == "kissy") {
        lv_obj_set_size(avatar_left_eye, EYE_BASE_W, EYE_BASE_H);
        lv_obj_set_size(avatar_right_eye, EYE_BASE_W, 10);
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

    boot_button.OnClick([]() {
        ESP_LOGI("Button", "短按觸發！當前系統模式: %d", current_system_mode);
        idle_to_clock_counter = 0;  // 只要按鍵，重置閒置計時

        if (current_system_mode == 1) {
            // 選單模式：切換選項
            game_menu_selection = (game_menu_selection + 1) % 6;
            ui_mode_changed = true;
        } else if (current_system_mode == 2) {
            // 遊戲中短按 (2048 悔棋 / Snake 暫停)
            pending_game_action = 5;
        } else if (current_system_mode == 3) {
            pending_game_action = 6;
        } else if (current_system_mode == 0) {
            // ==========================================
            // AI 模式：實體按鍵喚醒邏輯
            // ==========================================
            DeviceState state = Application::GetInstance().GetDeviceState();

            if (state == kDeviceStateIdle) {
                // 如果目前是安靜待機狀態，觸發 1.5 秒的隨機表情倒數！
                // (共用我們剛才寫好的 pending_touch_wake 邏輯)
                pending_touch_wake = 15;
            } else {
                // 如果 AI 正在說話或正在聆聽，代表你想「打斷」它
                // 這時候不要延遲，立刻瞬間觸發！
                if (ai_single_click_cb) {
                    ai_single_click_cb();
                }
            }
        }
    });

    // 2. 長按邏輯
    boot_button.OnLongPress([]() {
        // ESP_LOGI("Button", "長按觸發！當前系統模式: %d", current_system_mode);
        idle_to_clock_counter = 0;  // 只要按鍵，重置閒置計時

        if (current_system_mode == 1) {
            // ==========================================
            // 【核心修正】在選單中長按：一律進入當前選中的遊戲！
            // ==========================================
            current_system_mode = 2;  // 進入遊戲模式
            current_ui_mode = 2;
            ui_mode_changed = true;
        } else if (current_system_mode == 2) {
            // ==========================================
            // 在任何遊戲中長按：
            // 一律退出回到遊戲選單！
            // ==========================================
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

    boot_button.OnDoubleClick([]() {
        idle_to_clock_counter = 0;  // 只要按鍵，重置閒置計時

        if (current_system_mode == 0) {
            // AI 模式 -> 進入遊戲選單
            current_system_mode = 1;
            current_ui_mode = 1;
        } else if (current_system_mode == 1) {
            // 遊戲選單 -> 進入暗黑時鐘 (並降亮度至 10%)
            current_system_mode = 3;
            current_ui_mode = 3;  // 我們用 3 代表獨立時鐘模式
            auto backlight = Board::GetInstance().GetBacklight();
            if (backlight)
                backlight->SetBrightness(10);
        } else if (current_system_mode == 3 || current_system_mode == 2) {
            // 時鐘模式 (或遊戲中) 雙擊 -> 強制回血 100% 亮度，回到 AI！
            auto backlight = Board::GetInstance().GetBacklight();
            if (backlight)
                backlight->SetBrightness(100);

            current_system_mode = 0;
            current_ui_mode = 0;
        }
        ui_mode_changed = true;
    });
}