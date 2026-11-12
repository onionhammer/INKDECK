# Power, PMIC, RTC and the wake lifecycle (vendor reference)

Source: factory app `ESP-IDF/08_ESP32-S3_e-Paper-3.97` at commit `9b12d40`. Every line of code below is
quoted from it (trimmed with `// ...`). Anything marked **(inferred)** was not stated by the source or
datasheets and is our own deduction.

1. [Upstream file map](#1-upstream-file-map)
2. [PMIC (TG28 / AXP2101 via XPowersLib)](#2-pmic-tg28--axp2101-via-xpowerslib)
3. [RTC (PCF85063A)](#3-rtc-pcf85063a)
4. [The power lifecycle](#4-the-power-lifecycle)
5. [Wake-scheduling policy per page](#5-wake-scheduling-policy-per-page)
6. [Battery and charging figures](#6-battery-and-charging-figures)
7. [Porting notes and pitfalls](#7-porting-notes-and-pitfalls)
8. [Licensing](#8-licensing)

## 1. Upstream file map

Base: `https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/`

| What | Link |
| --- | --- |
| PMIC I2C callbacks, `axp_init()` | [axp_prot.cpp#L16-L122](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/axpPower/axp_prot.cpp#L16-L122) |
| Status logger task (never started), battery %, rail helpers | [axp_prot.cpp#L126-L272](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/axpPower/axp_prot.cpp#L126-L272) |
| `axp_pwr_off()`, `get_usb_connected()`, `getstat()` | [axp_prot.cpp#L275-L301](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/axpPower/axp_prot.cpp#L275-L301) |
| Rail enum `DC1..ALDO4` | [axp_prot.h#L6-L12](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/axpPower/axp_prot.h#L6-L12) |
| XPowersLib compile defines | [axpPower/CMakeLists.txt#L11](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/axpPower/CMakeLists.txt#L11) |
| RTC register defines, `RTC_INT`/`PWR_OUT` pins, `Time_data` | [pcf85063_bsp.h#L5-L39](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/pcf85063_bsp/pcf85063_bsp.h#L5-L39) |
| NVS `mode` helpers, GPIO init, `PCF85063_init()` | [pcf85063_bsp.c#L17-L117](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/pcf85063_bsp/pcf85063_bsp.c#L17-L117) |
| Time set/get, alarm enable/disable/flag | [pcf85063_bsp.c#L119-L265](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/pcf85063_bsp/pcf85063_bsp.c#L119-L265) |
| Shared I2C bus + `i2c_read_reg`/`i2c_write_reg` | [i2c_bsp.c#L19-L139](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/i2c_bsp/i2c_bsp.c#L19-L139) |
| Panel rail ALDO3 on in `EPD_Init`, off in `EPD_Sleep` | [epaper_port.c#L182-L192](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/epaper_port/epaper_port.c#L182-L192), [#L692-L702](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/epaper_port/epaper_port.c#L692-L702) |
| BOOT double-click = code 22, BOOT long-press = code 23 | [button_bsp.c#L179-L183](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/button_bsp/button_bsp.c#L179-L183), [#L292-L296](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/button_bsp/button_bsp.c#L292-L296) |
| Timeouts, status-bar battery, `user_Task` | [main.cc#L60-L63](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/main.cc#L60-L63), [#L150-L157](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/main.cc#L150-L157), [#L285-L418](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/main.cc#L285-L418) |
| `app_main`: boot-reason/mode logic and dispatch | [main.cc#L438-L572](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/main.cc#L438-L572) |
| Clock page shutdown, clock mode (1) | [page_clock.cc#L408-L422](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/page_clock/page_clock.cc#L408-L422), [#L454-L506](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/page_clock/page_clock.cc#L454-L506) |
| Calendar wake calc, page shutdown, calendar mode (2) | [page_clock.cc#L511-L543](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/page_clock/page_clock.cc#L511-L543), [#L646-L658](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/page_clock/page_clock.cc#L646-L658), [#L677-L715](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/page_clock/page_clock.cc#L677-L715) |
| Clock face persisted to SD / NVS across power-off | [page_clock.cc#L218-L233](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/page_clock/page_clock.cc#L218-L233), [#L1085-L1143](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/page_clock/page_clock.cc#L1085-L1143) |
| Weather wake calc, page shutdown, weather mode (3) | [page_weather.cc#L601-L697](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/page_weather/page_weather.cc#L601-L697), [#L800-L869](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/page_weather/page_weather.cc#L800-L869) |
| Alarm list (6 slots), NVS, idle shutdown, `alarm_task` | [page_alarm.cc#L130-L213](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/page_alarm/page_alarm.cc#L130-L213), [#L446-L493](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/page_alarm/page_alarm.cc#L446-L493) |

The vendored XPowersLib is v0.2.7 (`components/axpPower/src/XPowersLib_Version.h`).

## 2. PMIC (TG28 / AXP2101 via XPowersLib)

**Class and bus hookup.** `XPowersPMU` is a typedef for `XPowersAXP2101`, selected by
`add_compile_definitions(XPOWERS_CHIP_AXP2101 CONFIG_XPOWERS_ESP_IDF_NEW_API)`. The library doesn't own
I2C. Waveshare uses the callback overload `begin(addr, read, write)` with
`typedef int (*iic_fptr_t)(uint8_t devAddr, uint8_t regAddr, uint8_t *data, uint8_t len)` (0 = ok,
-1 = error). The callbacks wrap `i2c_bsp.c`: IDF `i2c_master`, port 0, SDA 41, SCL 42, 400 kHz,
internal pull-ups, one pre-added device handle per address (PMIC `0x34`).

```cpp
static XPowersPMU axp2101;
static int AXP2101_SLAVE_Read(uint8_t devAddr, uint8_t regAddr, uint8_t *data, uint8_t len)
{
    esp_err_t ret = i2c_read_reg(devAddr, regAddr, data, len);   // ok -> 0, else log + -1
    // ...
}
// AXP2101_SLAVE_Write(): same shape around i2c_write_reg()

void axp_init(void)
{
    if (axp2101.begin(AXP2101_SLAVE_ADDRESS, AXP2101_SLAVE_Read, AXP2101_SLAVE_Write)) {
    // ... logs "Init PMU SUCCESS!" / "Init PMU FAILED!" and carries on either way
    axp2101.setVbusVoltageLimit(XPOWERS_AXP2101_VBUS_VOL_LIM_4V36);
    axp2101.setVbusCurrentLimit(XPOWERS_AXP2101_VBUS_CUR_LIM_1500MA);
    // ...
    axp2101.setSysPowerDownVoltage(2600);
    // ...
    axp2101.setDC1Voltage(3300);
    axp2101.setALDO1Voltage(3300);
    axp2101.setALDO2Voltage(3300);
    axp2101.setALDO3Voltage(3300);
    /* Press the button to turn off the time setting */
    axp2101.setPowerKeyPressOffTime(XPOWERS_POWEROFF_4S);
    // ...
    axp2101.setPowerKeyPressOnTime(XPOWERS_POWERON_1S);
    // ...
    axp2101.enableTemperatureMeasure();
    axp2101.enableBattDetection();
    axp2101.enableVbusVoltageMeasure();
    axp2101.enableBattVoltageMeasure();
    axp2101.enableSystemVoltageMeasure();
    /* Disable the CHGLED function */
    axp2101.setChargingLedMode(XPOWERS_CHG_LED_OFF);
    /* Charge the VBAT */
    axp2101.setPrechargeCurr(XPOWERS_AXP2101_PRECHARGE_50MA); // Precharging current
    axp2101.setChargerConstantCurr(XPOWERS_AXP2101_CHG_CUR_200MA);//Constant current charging current ...
    axp2101.setChargerTerminationCurr(XPOWERS_AXP2101_CHG_ITERM_25MA);//Charging termination current
    axp2101.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V2);
    // ... #if 0: PMU watchdog (IRQ to pin, 4 s) -- compiled out
    axp2101.setButtonBatteryChargeVoltage(3000);  // Set the charging termination voltage of the RTC battery to 3
    axp2101.enableButtonBatteryCharge();          // Start RTC charging
    axp2101.setLowBatWarnThreshold(10);           // Output is interrupted when the rate is below 10%
    axp2101.setLowBatShutdownThreshold(5);        // Turn off the battery when it drops below 5%
    axp2101.fuelGaugeControl(true, true);         // Enable battery learning and save the data to ROM 
}
```

Register meanings: XPowersLib's implementation, checked against `docs/datasheets/TG28-PMIC.pdf`.

| Call | Register | Effect |
| --- | --- | --- |
| `begin()` → `initImpl()` | `0x03` chip ID, `0x30[1]` | Checks the AXP2101 chip ID and **clears the TS-pin ADC enable** ("Disable NTC temperature detection by default"). Init continues even if `begin()` fails. |
| `setVbusVoltageLimit(4V36)` | `0x15[3:0]` | VINDPM 4.36 V. Charge current backs off when VBUS sags to this level. |
| `setVbusCurrentLimit(1500MA)` | `0x16[2:0]` | Input current limit 1.5 A |
| `setSysPowerDownVoltage(2600)` | `0x24[2:0]` | VOFF 2.6 V. The PMIC hard-powers-off when VSYS drops below it. 2.6 V is also the datasheet default. |
| `setDC1Voltage(3300)` | DCDC1 | 3V3 system rail (ESP32, sensors) |
| `setALDO1/2/3Voltage(3300)` | `0x92..0x94` | Voltages only. ALDO1/2 are never explicitly enabled; they rely on power-on defaults. |
| `setPowerKeyPressOffTime(4S)` | `0x27[3:2]=00` | OFFLEVEL: hold PWRON 4 s for a forced power-off (datasheet default `01` = 6 s) |
| `setPowerKeyPressOnTime(1S)` | `0x27[1:0]=10` | ONLEVEL: PWRON low ≥1 s powers on. `0x27[5:4]` IRQLEVEL is left at its default (1.5 s). |
| `enable*Measure`, `enableBattDetection` | `0x30` bits 4,2,0,3; `0x68[0]` | ADC channels for die temperature, VBUS, VBAT, VSYS. Battery-presence detection on. |
| `setChargingLedMode(OFF)` | `0x69` | Manual mode (`[2:1]=10`, `[0]=1`) with output `[5:4]=00` = Hi-Z. **CHGLED (GPIO2, `PMIC_CHG`) carries no charge status under this config.** |
| `setPrechargeCurr` / `setChargerConstantCurr` / `setChargerTerminationCurr` / `setChargeTargetVoltage` | `0x61` / `0x62` / `0x63` / `0x64` | Precharge 50 mA, CC 200 mA, termination 25 mA, CV 4.2 V |
| `setButtonBatteryChargeVoltage(3000)` + `enableButtonBatteryCharge()` | `0x6A`, `0x18[2]` | Charges the backup cell on VBACKUP (100 µA linear, per datasheet) to 3.0 V. Datasheet default is 2.9 V, charging off. |
| `setLowBatWarnThreshold(10)`, `setLowBatShutdownThreshold(5)` | `0x1A[7:4]`, `0x1A[3:0]` | The datasheet calls these "low battery warning level 2 / level 1". They only raise IRQs, and the IRQ is unused, so they have no effect here **(inferred)**. The real cut-off is VOFF. |
| `fuelGaugeControl(true, true)` | `0xA2[4]=0`, `0xA2[0]=1` | Selects ROM for gauge data and enables the "brom writer" |

**Rail helpers.** `gatpwrstate()`, `enapwrstate()`, `disapwrstate()`, `getpwrVoltage()` and
`setpwrVoltage()` switch on `axp2101_pwr_tab_t { DC1 = 1, ALDO1, ALDO2, ALDO3, ALDO4 }` and forward to
the matching XPowersLib calls. DC1 disable/set is refused with a warning. Only ALDO3 is toggled:
`EPD_Init()` calls `enapwrstate(ALDO3)`; `EPD_Sleep()` sends deep-sleep (`0x10`/`0x01`), drives
RST/CS/DC low, then `disapwrstate(ALDO3)`. ALDO1/ALDO2 are never switched; the PMIC is shut down instead.

**Status, battery and USB.**

```cpp
int get_battery_power(void)  { return axp2101.getBatteryPercent(); }
bool get_usb_connected()     { return axp2101.isVbusIn(); }
void axp_pwr_off()
{
    // ...
    es8311_audio_shutdown_cleanup();  // Clean up the audio first
    // ...
    axp2101.shutdown();
}
```

- `getBatteryPercent()`: `-1` if `0x00[3]` (battery present) is 0, else the gauge byte `0xA4` (0–100).
  `isVbusIn()` = `0x01[3] == 0 && 0x00[5]` ("not in VINDPM and VBUS good"). `isCharging()`,
  `isDischarge()` and `isStandby()` decode `0x01[7:5]`; `getChargerStatus()` = `0x01[2:0]`.
- `shutdown()` sets `0x10[0]`. Per the datasheet, every output except RTCLDO turns off, DC1 included, so
  the ESP32 loses power. The audio cleanup before it (mute, volume 0, disable I2S, close codec, about
  300 ms of delays) presumably avoids a speaker pop **(inferred)**.
- `axp2101_getVoltage_Task()` (logs the values above once a second) is never created. `getstat()` is only
  referenced from a commented-out line.
- **IRQ: unused.** GPIO38 (`PMIC_IRQ`) is never configured and no IRQ calls exist. The PWR key isn't
  read at runtime; only the PMIC's 1 s / 4 s thresholds act on it.

**Status-bar battery** ([main.cc#L150-L157](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/main.cc#L150-L157)).
Every page repeats the same pattern. The percentage is read on each redraw. A missing battery shows
"-1%" with a full bar:

```cpp
    BAT_Power = get_battery_power();
    // ...
    snprintf(BAT_Power_str, sizeof(BAT_Power_str), "%d%%", BAT_Power);
    if(BAT_Power == -1) BAT_Power = 20;
    else BAT_Power = BAT_Power * 20 / 100;
    Paint_DrawString_EN(411, 11, BAT_Power_str, &Font16, WHITE, BLACK);
    Paint_DrawRectangle(375, 22, 395, 30, WHITE, DOT_PIXEL_1X1, DRAW_FILL_FULL);
    Paint_DrawRectangle(375, 22, 375+BAT_Power, 30, BLACK, DOT_PIXEL_1X1, DRAW_FILL_FULL);
```

## 3. RTC (PCF85063A)

The RTC is at I2C `0x51` on the shared bus. Its VDD is the PMIC's **VRTC** (RTCLDO, which stays alive
while the PMIC is off). The datasheet says RTCLDO1 can be sourced from the backup cell on VBACKUP. The
driver does one I2C transaction per register: `PCF85063_Read_Byte()` returns 0 on error and
`PCF85063_Write_Byte()` only logs errors.

| Addr | Register | Bits used by the driver (datasheet names) |
| --- | --- | --- |
| `0x00` | Control_1 | `PCF85063_init()` clears bit 7 `EXT_TEST`. STOP, `12_24` (0 = 24 h) and `CAP_SEL` are left at reset (0). |
| `0x01` | Control_2 | bit 7 `AIE` (set in alarm enable, cleared in disable), bit 6 `AF` (read and cleared). MI/HMI/TF/COF are unused. |
| `0x02`/`0x03` | Offset / RAM_byte | unused |
| `0x04` | Seconds | bit 7 `OS` (oscillator-stop flag, reset value 1), BCD seconds, masked `0x7F` |
| `0x05`–`0x0A` | Minutes, Hours, Days, Weekdays, Months, Years | BCD. Masks `0x7F`, `0x3F`, `0x3F`, `0x07`, `0x1F`, none. Years are 0–99 and the app adds 2000. |
| `0x0B`–`0x0E` | Second / Minute / Hour / Day alarm | bit 7 `AEN_x` (**0 = this field is compared**) + BCD value |
| `0x0F` | Weekday_alarm | never written (reset `0x80` = ignored) |
| `0x10`/`0x11` | Timer_value / Timer_mode | `#define`d only. **The countdown timer is not used.** |

```c
int DecToBcd(int val) { return ((val/10)*16 + (val%10)); }   // body verbatim, braces joined
int BcdToDec(int val) { return ((val/16)*10 + (val%16)); }
void PCF85063_init()
{
    esp_gpio_Init();                       // GPIO45 (RTC_INT) + GPIO1 (PWR_OUT): input, internal pull-up
    uint8_t ctrl1 = PCF85063_Read_Byte(CONTROL_1_REG);
    ctrl1 &= ~(1 << 7); 
    PCF85063_Write_Byte(CONTROL_1_REG, ctrl1);
    vTaskDelay(pdMS_TO_TICKS(10));
    // ...
    while (1) {
        seconds_reg = PCF85063_Read_Byte(SECONDS_REG);
        if ((seconds_reg & 0x80) == 0) {
            break;
        }
        PCF85063_Write_Byte(SECONDS_REG, seconds_reg & 0x7F);
        vTaskDelay(pdMS_TO_TICKS(100));
        inspect++;
        if (inspect > 50) { /* "Clock stability unknown or clock stopped" */ break; }
    }
}

Time_data PCF85063_GetTime()   // 7 single-byte reads, years first ... seconds last
	time.years = BcdToDec(PCF85063_Read_Byte(YEARS_REG));
	// ... months &0x1F, days &0x3F, hours &0x3F, minutes &0x7F, seconds &0x7F, week &0x07
void PCF85063_SetTime(Time_data time)  // HMS, YMD, weekday: 7 single-byte writes (values clamped)
```

`PCF85063_init()` clears the OS flag silently (it can block up to about 5 s waiting for the crystal).
Nothing records that the time was invalid. After backup power loss the RTC restarts at its reset value,
2000-01-01 00:00:00, and only NTP in the clock/calendar/weather pages fixes it.

**Alarm.** `PCF85063_alarm_Time_Enabled(t)` normalises overflow (sec→min→hour→day, wrapping the day
using the length of `t.months`, with `years%4` for leap years), then:

```c
	PCF85063_Write_Byte(CONTROL_2_REG, PCF85063_Read_Byte(CONTROL_2_REG)|0x80);	// Alarm on
	PCF85063_Write_Byte(DAY_ALARM_REG, DecToBcd(time.days) & 0x7F);
    PCF85063_Write_Byte(HOUR_ARARM_REG, DecToBcd(time.hours) & 0x7F);
	PCF85063_Write_Byte(MINUTES_ALARM_REG, DecToBcd(time.minutes) & 0x7F);
	PCF85063_Write_Byte(SECOND_ALARM_REG, DecToBcd(time.seconds) & 0x7F);
}
void PCF85063_alarm_Time_Disable()   // sets AEN bit 7 in 0x0D,0x0C,0x0B,0x0E, then clears AIE
void PCF85063_clear_alarm_flag()     // Control_2 &= 0xBF (clear AF)
	PCF85063_Write_Byte(CONTROL_2_REG   ,PCF85063_Read_Byte(CONTROL_2_REG)&0xBF);
```

All four fields (day, hour, minute, second) are compared, so the alarm is a one-shot absolute
date-time within the month. `PCF85063_get_alarm_flag()` (Control_2 & 0x40) and `rtcRunAlarm()` are
unused.

**INT pin.** INT is open-drain. With AIE=1 it "follows the condition of bit AF" and stays low until AF
is cleared by command (datasheet §8.5). On this board INT pulls the PMIC's PWRON low through a diode,
which powers the board on, and also reads LOW on GPIO45 (`RTC_INT`). Topology, read from the rendered
schematic (page 1, Power block): D8 (B5819WS) sits between `RTC_INT_ESP` (GPIO45) and `RTC_INT`, and D4
(B5819WS) between `RTC_INT` and the `PWRON` / Key1 node (Key1 → GND; R26 510 Ω to the PMIC PWRON pin).
The same node drives the gate of T1 (BSS138, via R25 1 kΩ). Its drain is `PWR_OUT` (GPIO1, 10 kΩ pull-up
to 3V3). So GPIO1 reads HIGH whenever PWRON is pulled low, by the key or the RTC. The diode
orientation isn't legible at that resolution; the stated behaviour is what the firmware relies on.

## 4. The power lifecycle

**Power sources.** The PMIC wakes from off on (a) PWRON held low ≥1 s, from the PWR key or the RTC INT
diode; (b) VBUS insertion. Per the datasheet, VBUS power-on is an eFuse "customization", but the
firmware clearly expects it (see the USB branch below) **(inferred)**.

**Boot path** ([main.cc#L438-L523](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/main.cc#L438-L523)).
The init order is `nvs_flash_init`, `_sdcard_init`, `i2c_master_init` + `i2c_devices_init`,
`spiffs_init`, `PCF85063_init`, `axp_init`, then:

```cpp
    char mode = load_mode_enable_from_nvs();      // NVS "mode_state"/"mode", u8, default 0
    if(RTC_INT && mode!=0)                         // GPIO45 HIGH => NOT an RTC wake
    {
        if(get_usb_connected()){
            vTaskDelay(pdMS_TO_TICKS(100));
            if((!PWR_OUT))                         // GPIO1 LOW => PWR key not held
            {
                ESP_LOGE(TAG,"USB connection, but the device is shut down when the mode is not set to 0");
                axp_pwr_off();
            } else { /* "Others, continue the process" */ }
        }
        save_mode_enable_to_nvs(0);                // user woke it: leave low-power mode
        mode = 0;
    }
    // ... button_Init, SHTC3, audio, epaper_port_init, EPD_Init, PSRAM framebuffer
    PCF85063_clear_alarm_flag();                   // releases INT (and PWRON)
    if(mode == 1)      xTaskCreate(clock_mode_task,    /* ... */);
    else if(mode == 2) xTaskCreate(calendar_mode_task, /* ... */);
    else if(mode == 3) xTaskCreate(weather_mode_task,  /* ... */);
    else {
        PCF85063_alarm_Time_Disable();
        PCF85063_clear_alarm_flag();
        // ... QMI8658_init, Wi-Fi per NVS, mutexes (rtc_mutex etc. only exist on this path)
        xTaskCreate(user_Task, "user_Task", 64 * 1024, NULL, USER_TASK_PRIO, NULL);
        xTaskCreate(alarm_task, "alarm_task", 4 * 1024, NULL, ALARM_TASK_PRIO, NULL);
    }
```

```
                 PMIC off (only RTCLDO/VRTC alive)
   PWR key >=1s        RTC alarm (INT low)        USB plugged in
        |                     |                         |
        +----------> app_main: init ... axp_init, read NVS mode <---+
                              |
     GPIO45 LOW (RTC wake) or mode==0 ----------------> keep mode
     GPIO45 HIGH and mode!=0:
        no USB ...................................> mode := 0 (user pressed PWR)
        USB, GPIO1 LOW after 100 ms ...............> axp_pwr_off()  (USB plug only; mode kept)
        USB, GPIO1 HIGH ...........................> mode := 0 (PWR key held during boot)
                              |
     clear AF -> mode 1/2/3: one-shot task: draw, maybe ring alarm, arm RTC, axp_pwr_off()
              -> mode 0: disable RTC alarm, interactive UI (user_Task + alarm_task)
```

**Interactive (mode 0) inactivity** ([main.cc#L307-L349](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/main.cc#L307-L349)).
`EPD_Sleep_Time 60` (s) and `Unattended_Time 10` (min) are re-`#define`d in every page file:

```cpp
        button = wait_key_event_and_return_code(pdMS_TO_TICKS(1000));
        time_count++;
        if((time_count > EPD_Sleep_Time)) {
            EPD_Sleep();
            int sleep_js = 0;
            while(1)
            {
                // ... any key: EPD_Init(), redraw, time_count = 0, break
                // ... read RTC; on each new minute:
                    EPD_Init();
                    // ... redraw status-bar time + battery (partial refresh)
                    EPD_Sleep();
                    sleep_js++;
                    if(sleep_js > Unattended_Time){
                        axp_pwr_off();
                    } 
```

- After about 60 s without a handled key, the panel sleeps (ALDO3 off). The status-bar clock is still
  redrawn every minute. On the 11th minute rollover the PMIC shuts down. Total: about 11–12 min.
- **BOOT double-click (code 22) on the home page calls `axp_pwr_off()` immediately.** BOOT long-press
  (23) opens settings. Function long-press (12) does a full refresh.
- Neither home-menu shutdown path arms the RTC or changes `mode`. The device stays off until the PWR key,
  and **alarms set in the alarm page will not fire while it is off** (`alarm_task` only rings while
  mode 0 is running). Every other page (file browser, alarm, audio, fiction, network, settings) has the
  same "panel sleep, then `sleep_js > 10` → `axp_pwr_off()`" loop without arming the RTC.

**Entering a low-power mode.** Only three pages do this, on their own timeout: `sleep_js > Unattended_Time`
counts minute rollovers on the page and is never reset by key presses. The clock page runs
`save_mode_enable_to_nvs(1)`, arms `rtc_time` + 1 min with seconds 0 (same code as the end of
`page_clock_show_mode()` below), saves the framebuffer to `CLOCK_PARTIAL_PATH` (NVS fallback), then
`axp_pwr_off()`. Calendar runs `save_mode_enable_to_nvs(2); load_alarms_from_nvs(); Wake_up_time_setting_calendar(rtc_time);`.
Weather runs the same with mode 3 and `Wake_up_time_setting_weather()`, plus the SD save.

**Mode wake (clock, mode 1)** ([page_clock.cc#L454-L506](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/page_clock/page_clock.cc#L454-L506)):

```cpp
void page_clock_show_mode()
{
    load_alarms_from_nvs();
    Time_data rtc_time = PCF85063_GetTime();
    if(rtc_time.hours == 8 && rtc_time.minutes == 0)
    {   // page_network_init_mode() (Wi-Fi up to ~30 s + IP ~10 s) then NTP -> PCF85063_SetTime()
    }
    if(!check_alarm(rtc_time.hours, rtc_time.minutes)){
        // Do a full brush at every hour and half
        if(rtc_time.minutes == 0 || rtc_time.minutes == 30){
            display_clock_init();
            display_clock_img(rtc_time, Global_refresh);
        } else {
            display_clock_mode_img(rtc_time);   // reload last frame from SD (or NVS), partial refresh
        }
        EPD_Sleep();
    } else {
        // ... same drawing + EPD_Sleep(), then page_audio_play_memory() (rings once; any key stops it)
    }
    Time_data alarm_time = rtc_time;
    alarm_time.minutes += 1;
    alarm_time.seconds = 0;
    PCF85063_alarm_Time_Enabled(alarm_time);
    vTaskDelay(pdMS_TO_TICKS(50));
    axp_pwr_off();
}
```

Calendar mode and weather mode arm the next wake **first**, then optionally refresh (Wi-Fi/NTP/HTTP),
ring a matching alarm, `EPD_Sleep()` and `axp_pwr_off()`. The clock arms **last**. Mode tasks call the
PCF85063 functions without `rtc_mutex`, which is NULL on those paths.

**What persists across power-off**

| Store | Key / location | Content |
| --- | --- | --- |
| NVS | `mode_state` / `mode` (u8) | 0 interactive, 1 clock, 2 calendar, 3 weather |
| NVS | `alarm` / `alarms` (blob) | `Alarm[6]` = `{hour, minute, enabled}`, kept sorted (enabled first, by time) |
| NVS | `clock_th` / `th_data` (blob) | `Clock_TH` fallback used to redraw the clock face if the SD write failed |
| NVS | `clock_th` / `timezone_idx` (i32) | `NVS_NAMESPACE` is `#define`d twice (`clock_config`, then `clock_th`), so the second value wins |
| SD | `/sdcard/GUI/clock_partial.bin` | Last 48,000-byte mono framebuffer: the base image for the next partial refresh |
| RTC | `0x04`–`0x0A`, `0x0B`–`0x0E`, Control_2 AIE | Time, and the armed alarm |
| PMIC | registers | Re-applied by `axp_init()` on every boot anyway |

## 5. Wake-scheduling policy per page

| Mode | Next wake armed | Notes |
| --- | --- | --- |
| 1 clock | Always now + 1 min (`hh:mm+1:00`) | Each wake: partial refresh, plus a full refresh at :00 and :30. NTP at 08:00. Alarms are checked every minute. |
| 2 calendar | First enabled alarm in array order that is later today, else next day `00:00` | Seconds are **not** zeroed (inherits `rtc_time.seconds`). Redraws on midnight wakes or non-alarm wakes. NTP with hard-coded `CST-8`. |
| 3 weather | Earlier of the next weather slot (04:00, 09:00, 14:00, 20:00) and an enabled alarm | After 20:00: a later alarm today, else next day 04:00 (or that alarm if its hour ≤ 4). Fetch + NTP on non-alarm wakes. |
| 0 | Nothing (alarm disabled at boot) | Off until the PWR key |

The wiki (`docs/wiki/03-ESP-IDF.md`) says "by default the RTC wakes once every hour; if an alarm is set
it wakes once per minute". **The code does not do that.** It also says up to 5 alarms (the code has
`MAX_ALARMS 6`), "sleep after 5 seconds" (the code uses 60 s), and "press PWR 4 s to power on" (the
code sets ONLEVEL to 1 s; 4 s is the forced-off time).

## 6. Battery and charging figures

- Waveshare settings: see the §2 table (CC 200 mA to 4.2 V, 1.5 A input limit, VOFF 2.6 V).
- Vendor claim (wiki "Battery Life" and FAQ): **"In clock mode, a 1500mAh battery can operate for over
  15 days."** No measurement method is given. That implies an average of ≤ 1500 mAh / 360 h ≈ 4.2 mA,
  or about 250 mA·s per one-minute wake cycle if the off current is negligible **(inferred)**.
- TG28 datasheet: off state with only the battery present draws typically 40 µA (RTCLDO only).
- Wake cost drivers: `sdkconfig` has `CONFIG_SPIRAM_MEMTEST=y`, 160 MHz CPU and no `CONFIG_PM_ENABLE`;
  SD and SPIFFS are mounted before the mode check.

## 7. Porting notes and pitfalls

**Reproducing it in Arduino**

- Use the PlatformIO library `lewisxhe/XPowersLib` with `-DXPOWERS_CHIP_AXP2101` (or the
  `XPowersAXP2101` class). With `ARDUINO` defined, v0.2.7 compiles out the
  `begin(i2c_master_bus_handle_t, addr)` overload. That leaves `begin(Wire, addr, SDA, SCL)`, which calls
  `Wire.begin()` itself, or **Waveshare's callback `begin(addr, read, write)`**. Prefer the callbacks
  (0 = ok, -1 = error; read = register write + repeated-start read), built on `Wire` or on an
  `i2c_master` device on `i2cBusHandle(0)`, so the bus keeps one owner.
- Keep the vendor `axp_init()` values (§2) as the baseline. Use `XPOWERS_CHG_LED_CTRL_CHG` instead if you
  want GPIO2 to show charging **(inferred)**.
- Better boot-reason detection than GPIO sampling: read PMIC `0x20` (PWRON status). Bit 0 = PWRON-low
  (key or RTC INT), bit 1 = IRQ pin, bit 2 = VBUS insert. Combine it with GPIO45 / RTC `AF` to tell the
  RTC apart from the key. Don't use XPowersLib's `getPowerOnSource()`: it casts the raw byte to a
  sequential enum (0,1,2,…), not bit masks **(inferred from code vs datasheet)**.
- RTC: read and write 0x04–0x0A in **one burst** (the register address auto-increments). The datasheet
  requires this to avoid roll-over corruption. Waveshare's 7 single-byte transactions violate it. Check
  `OS` (0x04 bit 7) and treat the time as invalid until NTP. Disabling CLKOUT (`COF=111`) may save VRTC
  current **(inferred)**.

**Pitfalls in the vendor flow**

1. **INT holds PWRON low until AF is cleared.** Waveshare clears AF late (after SD, SPIFFS, PMIC, audio
   and `EPD_Init`). If boot plus the 1 s ONLEVEL ever exceeded OFFLEVEL (4 s), the PMIC would force-off
   mid-boot **(inferred)**. Clear AF (and arm or disable the alarm) early in `setup()`.
2. **Missed alarm = month-long sleep.** All four alarm fields are compared, so a target that has already
   passed next matches next month **(inferred from datasheet)**. Clock mode computes "+1 min" at wake,
   then may spend about 45 s on Wi-Fi/NTP at 08:00, plus the alarm sound, before arming. Arm from a fresh
   RTC read and check that the target is still in the future.
3. **USB-plugged shutdown race.** In a low-power mode with USB attached, a PWR-key boot only reaches the
   UI if the key is **still held** when GPIO1 is sampled. That happens after SD/SPIFFS/RTC/PMIC init plus
   100 ms. Otherwise the board powers straight back off **(inferred)**.
4. Inactivity shutdown also happens on USB power (`get_usb_connected()` is only consulted at boot), so the
   USB-CDC console disappears. Skip the PMIC shutdown while VBUS is present during development.
5. `isVbusIn()` returns false while the PMIC is in VINDPM (a sagging supply), even with USB attached
   **(inferred from code + datasheet)**.
6. Polarity: `RTC_INT` (GPIO45) **LOW** = alarm asserted. `PWR_OUT` (GPIO1) **HIGH** = PWRON is low (PWR
   held, or RTC INT). Both use internal pull-ups. GPIO45 is also a strapping pin. A low level during
   reset is its default value **(inferred)**.
7. XPowersLib's `begin()` disables the TS-pin ADC and Waveshare never re-enables it, though the
   schematic shows a 10 kΩ NTC on TG28 `TS`. Decide deliberately about temperature-based charge protection.
8. `alarm_task` and the page loops share one key event group (cleared on read), so a press during
   ringing can be stolen. `i2c_bsp.c` passes `pdMS_TO_TICKS(1000)` as a millisecond timeout, which is
   100 ms at `CONFIG_FREERTOS_HZ=100`.
9. Wake calculations: a calendar alarm keeps the current seconds. Calendar picks the first alarm in
   array order, which is chronological only while the list is sorted (add/remove/edit call `qsort`).
   Weather skips a slot's update when an alarm falls in that slot's hour (e.g. 09:00–09:59), because the
   alarm wake rings without fetching.

## 8. Licensing

`axp_prot.cpp/.h`, `pcf85063_bsp.c/.h`, `i2c_bsp.c/.h`, `main.cc` and `page_*.cc` carry **no license or
copyright header**, and the repo root has no LICENSE file. The vendored XPowersLib files carry an MIT
header, e.g. `XPowersAXP2101.tpp` / `XPowersCommon.tpp`:

`@license MIT License` / `Copyright (c) 2022 lewis he` / "Permission is hereby granted, free of
charge, …" (`XPowersLib.h` says 2024). Pull XPowersLib in as a dependency rather than copying it.
