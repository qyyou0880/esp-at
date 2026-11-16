/* SPDX-License-Identifier: Apache-2.0 */
/* Included by link_gpio.c: one mutex owns LINK and BOOT/NRST pins. */
typedef struct { int32_t boot, reset, enabled; } bootrst_config_t;
static bootrst_config_t s_bootrst = {-1,-1,0};

static void bootrst_release(void)
{
    if (!s_bootrst.enabled) return;
    (void)gpio_set_level(s_bootrst.reset,1); /* open drain: high impedance */
    (void)gpio_set_level(s_bootrst.boot,0);
}

static bool bootrst_save(const bootrst_config_t *cfg)
{
    nvs_handle_t h;
    if (nvs_open("BOOTRST",NVS_READWRITE,&h)!=ESP_OK) return false;
    esp_err_t err=nvs_set_blob(h,"pins",cfg,sizeof(*cfg));
    if (err==ESP_OK) err=nvs_commit(h);
    nvs_close(h);
    return err==ESP_OK;
}

static bool bootrst_configure_locked(int boot,int reset,bool enabled,bool persist)
{
    if (s_powerdown || boot==reset || !link_gpio_pin_allowed(boot) ||
        !link_gpio_pin_allowed(reset)) return false;
    if (s_bootrst.enabled && (boot!=s_bootrst.boot || reset!=s_bootrst.reset)) return false;
    ESP_LOGI(TAG,"BOOTRST pins %d/%d enable=%d persist=%d",boot,reset,enabled,persist);
    if (enabled && !s_bootrst.enabled) {
        /* Never turn a push-pull LINK output into NRST while it is high. */
        if (reset==s_state.gpio) return false;
        if (gpio_set_level(boot,0)!=ESP_OK || !link_gpio_prepare_pin(boot,false)) return false;
        if (gpio_set_direction(reset,GPIO_MODE_INPUT)!=ESP_OK ||
            gpio_set_level(reset,1)!=ESP_OK) return false;
        gpio_config_t cfg={.pin_bit_mask=1ULL<<reset,.mode=GPIO_MODE_OUTPUT_OD,
            .pull_up_en=GPIO_PULLUP_DISABLE,.pull_down_en=GPIO_PULLDOWN_DISABLE,
            .intr_type=GPIO_INTR_DISABLE};
        if (gpio_config(&cfg)!=ESP_OK) return false;
        s_owned_pins|=1ULL<<reset;
        if (gpio_sleep_sel_dis(reset)!=ESP_OK) return false;
    }
    ESP_LOGI(TAG,"BOOTRST GPIO prepared");
    bootrst_config_t next={boot,reset,enabled};
    if (persist && !bootrst_save(&next)) return false;
    if (enabled) {
        if (!link_gpio_state_configure(&s_state,-1) || gpio_set_level(boot,0)!=ESP_OK ||
            gpio_set_level(reset,1)!=ESP_OK) {
            if (persist) (void)bootrst_save(&s_bootrst);
            return false;
        }
    } else {
        bootrst_release();
        if (s_bootrst.reset>=0) (void)gpio_set_direction(s_bootrst.reset,GPIO_MODE_INPUT);
        if (s_bootrst.boot>=0) (void)gpio_set_direction(s_bootrst.boot,GPIO_MODE_INPUT);
    }
    s_bootrst=next;
    ESP_LOGI(TAG,"BOOTRST applied");
    return true;
}

bool esp_at_bootrst_configure(int boot,int reset,bool enabled)
{
    if (!s_lock) return false;
    xSemaphoreTake(s_lock,portMAX_DELAY);
    bool ok;
    if (s_bootrst.enabled && enabled && s_bootrst.boot==boot && s_bootrst.reset==reset)
        ok=!s_powerdown; /* Repeated configuration must not change live BOOT. */
    else ok=bootrst_configure_locked(boot,reset,enabled,true);
    xSemaphoreGive(s_lock);
    return ok;
}

void esp_at_bootrst_get(int *boot,int *reset,bool *enabled)
{
    *boot=-1;*reset=-1;*enabled=false;
    if (!s_lock) return;
    xSemaphoreTake(s_lock,portMAX_DELAY);
    *boot=s_bootrst.boot;*reset=s_bootrst.reset;*enabled=s_bootrst.enabled;
    xSemaphoreGive(s_lock);
}

bool esp_at_bootrst_mode(bool bootloader)
{
    if (!s_lock) return false;
    xSemaphoreTake(s_lock,portMAX_DELAY);
    bool ok=s_bootrst.enabled && !s_powerdown;
    if (ok) ok=gpio_set_level(s_bootrst.boot,bootloader?1:0)==ESP_OK;
    if (ok) {
        vTaskDelay(pdMS_TO_TICKS(10)); /* BOOT setup before NRST assertion */
        ok=gpio_set_level(s_bootrst.reset,0)==ESP_OK;
        if (ok) vTaskDelay(pdMS_TO_TICKS(50));
        if (gpio_set_level(s_bootrst.reset,1)!=ESP_OK) ok=false;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (!ok) bootrst_release();
    xSemaphoreGive(s_lock);
    return ok;
}

static void bootrst_load(void)
{
    nvs_handle_t h;
    if (nvs_open("BOOTRST",NVS_READONLY,&h)!=ESP_OK) return;
    bootrst_config_t cfg;size_t len=sizeof(cfg);
    esp_err_t err=nvs_get_blob(h,"pins",&cfg,&len);nvs_close(h);
    if (err!=ESP_OK || len!=sizeof(cfg) || (cfg.enabled!=0 && cfg.enabled!=1)) return;
    if (!bootrst_configure_locked(cfg.boot,cfg.reset,cfg.enabled,false))
        ESP_LOGE(TAG,"BOOTRST configuration unavailable; automatic reset disabled");
}

static uint8_t bootrst_setup(uint8_t n)
{
    int32_t b,r,e;
    if (n!=3 || esp_at_get_para_as_digit(0,&b)!=ESP_AT_PARA_PARSE_RESULT_OK ||
        esp_at_get_para_as_digit(1,&r)!=ESP_AT_PARA_PARSE_RESULT_OK ||
        esp_at_get_para_as_digit(2,&e)!=ESP_AT_PARA_PARSE_RESULT_OK || (e!=0 && e!=1))
        return ESP_AT_RESULT_CODE_ERROR;
    return esp_at_bootrst_configure(b,r,e)?ESP_AT_RESULT_CODE_OK:ESP_AT_RESULT_CODE_ERROR;
}
static uint8_t bootrst_query(uint8_t *name)
{
    int b,r;bool e;char reply[64];esp_at_bootrst_get(&b,&r,&e);
    snprintf(reply,sizeof(reply),"%s:%d,%d,%d\r\n",name,b,r,e?1:0);
    esp_at_port_write_data((uint8_t *)reply,strlen(reply));
    return ESP_AT_RESULT_CODE_OK;
}
static uint8_t bootmode_setup(uint8_t n)
{
    int32_t mode;
    if (n!=1 || esp_at_get_para_as_digit(0,&mode)!=ESP_AT_PARA_PARSE_RESULT_OK ||
        (mode!=0 && mode!=1)) return ESP_AT_RESULT_CODE_ERROR;
    return esp_at_bootrst_mode(mode!=0)?ESP_AT_RESULT_CODE_OK:ESP_AT_RESULT_CODE_ERROR;
}
