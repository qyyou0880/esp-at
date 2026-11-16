# 有源 32768 Hz RTC 时钟配置核查

2026-11-15，根据用户要求核查。基线为 qyyou0880/esp-at 的
`ffa61b2c73c2d3df6ac69fdffe1dd89985038249`，不是其它芯片的默认配置。

原提交 `07ea56ad` 加入 `CONFIG_RTC_CLK_SRC_EXT_OSC=y` 及
`CONFIG_RTC_CLK_SRC_CUSTOM=y`；后续 `ffa61b2c` 正确删除了 CUSTOM。
固定 IDF 的 C6 Kconfig 只有 INT_RC / EXT_CRYS / EXT_OSC 三选一，没有 CUSTOM。

## 上次构建确实选中了有源输入

已通过 GitHub 连接工具下载上次成功工作流 `40582921474` 的正常 C6 产物
`12770503893`（esp32c6-4MB-at），读取其中实际生成的 sdkconfig：

```text
# CONFIG_RTC_CLK_SRC_INT_RC is not set
# CONFIG_RTC_CLK_SRC_EXT_CRYS is not set
CONFIG_RTC_CLK_SRC_EXT_OSC=y
CONFIG_RTC_CLK_CAL_CYCLES=1024
CONFIG_ESP_SYSTEM_RTC_EXT_OSC=y
CONFIG_BT_LE_LP_CLK_SRC_DEFAULT=y
```

因此那次固件的软件选择是正确的。源码普通 defaults 当时同时包含 EXT_CRYS=y 和
末尾 EXT_OSC=y，最终结果虽为 EXT_OSC，仍应清理互斥项。静默版还保留 EXT_CRYS，
没有同步有源选项。本轮两份 defaults 都改为单一 EXT_OSC，保留原校准周期 1024。

## 引脚和实际启用路径

固定 IDF 提交 `4dbfecac7e3b196d3f6826b3db2fdd5a23708e0d` 中：

* `components/esp_hw_support/port/esp32c6/Kconfig.rtc`：EXT_OSC 选择
  ESP_SYSTEM_RTC_EXT_OSC；1024 在允许的校准范围内。
* `components/soc/esp32c6/include/soc/clk_tree_defs.h`：
  `SOC_EXT_OSC_SLOW_GPIO_NUM=0`，即 GPIO0 / XTAL_32K_P。
* `components/esp_system/port/soc/esp32c6/clk.c`：选择 OSC_SLOW 后调用
  `rtc_clk_32k_enable_external()`，并用 OSC_SLOW 路径校准；启动失败会回退内部 RC。
* `components/esp_hw_support/port/esp32c6/rtc_clk.c`：使能外部输入及引脚保持；
  它不是给无源晶体使用的 CRYSTAL 模式。
* ESP-AT `at_workaround.c` 的无源晶体检查仅在 EXT_CRYS 下编译；正确选择
  EXT_OSC 时不会进入那段关闭无源振荡器的 workaround。

新增 `AT+RTCCLK?` 可查看运行中的时钟源和标称频率；它不能测量外部频率。
LINK 功能明确禁止占用 GPIO0/1。

## 仍需实机确认

GPIO0 上需要实际存在稳定的 32768 Hz 输入且公共地正确；振荡器的 OE/供电在
目标休眠期间也需保持。电平和幅度应符合 C6 数据手册，不应输入 5V。
本轮没有原理图、示波器或实机启动证据，因此不能证明硬件实际选用或时钟精度。

官方依据：
[RTC 时钟说明](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/api-reference/system/system_time.html)、
[固定 IDF Kconfig](https://github.com/espressif/esp-idf/blob/4dbfecac7e3b196d3f6826b3db2fdd5a23708e0d/components/esp_hw_support/port/esp32c6/Kconfig.rtc)、
[固定 IDF 时钟初始化](https://github.com/espressif/esp-idf/blob/4dbfecac7e3b196d3f6826b3db2fdd5a23708e0d/components/esp_system/port/soc/esp32c6/clk.c)。
