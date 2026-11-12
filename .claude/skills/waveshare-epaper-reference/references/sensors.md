# Sensors: SHTC3 and QMI8658A

Vendor code at commit `9b12d40`. Paths are relative to `ESP-IDF/08_ESP32-S3_e-Paper-3.97/` unless
they start with `Arduino/`. Both sensors sit on the shared I2C bus (GPIO41 SDA / GPIO42 SCL, port 0,
400 kHz), which is covered in `io.md`. Register meanings are checked against
`docs/datasheets/SHTC3-temp-humidity.pdf` and `docs/datasheets/QMI8658A-IMU.pdf` (Rev A).

## Contents

1. [Upstream file map](#1-upstream-file-map)
2. [SHTC3 temperature / humidity](#2-shtc3-temperature--humidity)
3. [QMI8658A IMU](#3-qmi8658a-imu)
4. [How the app uses the sensors](#4-how-the-app-uses-the-sensors)
5. [Porting notes and upstream bugs](#5-porting-notes-and-upstream-bugs)
6. [Licensing](#6-licensing)

## 1. Upstream file map

Base URL: `https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/`

| What | Link |
| --- | --- |
| SHTC3 command enum + CRC | [shtc3_bsp.c#L14-L53](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/shtc3_bsp/shtc3_bsp.c#L14-L53) |
| SHTC3 GetId / Wakeup / SoftReset | [shtc3_bsp.c#L55-L90](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/shtc3_bsp/shtc3_bsp.c#L55-L90) |
| SHTC3 conversion + polling measurement + Sleep | [shtc3_bsp.c#L92-L148](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/shtc3_bsp/shtc3_bsp.c#L92-L148) |
| SHTC3 high-level read + init | [shtc3_bsp.c#L174-L218](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/shtc3_bsp/shtc3_bsp.c#L174-L218) |
| SHTC3 header (CRC poly, prototypes) | [shtc3_bsp.h#L1-L97](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/shtc3_bsp/shtc3_bsp.h#L1-L97) |
| Arduino SHTC3 variant (IDF i2c_master, -4 °C offset) | [Arduino/examples/04_I2C_SHTC3/i2c_equipment.cpp#L78-L240](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/Arduino/examples/04_I2C_SHTC3/i2c_equipment.cpp#L78-L240) |
| QMI8658 register enum, INT pins, range/ODR enums | [qmi8658_bsp.h#L16-L320](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/qmi8658_bsp/qmi8658_bsp.h#L16-L320) |
| QMI8658 reg R/W wrappers, INT GPIO init | [qmi8658_bsp.c#L38-L83](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/qmi8658_bsp/qmi8658_bsp.c#L38-L83) |
| QMI8658 accel/gyro config (CTRL2/3/5) | [qmi8658_bsp.c#L176-L281](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/qmi8658_bsp/qmi8658_bsp.c#L176-L281) |
| QMI8658 temp + data reads + scaling | [qmi8658_bsp.c#L320-L427](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/qmi8658_bsp/qmi8658_bsp.c#L320-L427) |
| QMI8658 CTRL9 + wake-on-motion (unused) | [qmi8658_bsp.c#L483-L531](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/qmi8658_bsp/qmi8658_bsp.c#L483-L531) |
| QMI8658 enable / apply / init | [qmi8658_bsp.c#L533-L638](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/qmi8658_bsp/qmi8658_bsp.c#L533-L638) |
| Settings page IMU + memory screens | [page_settings.cc#L56-L225](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/page_settings/page_settings.cc#L56-L225) |
| Init calls in `app_main` | [main.cc#L482-L525](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/ESP-IDF/08_ESP32-S3_e-Paper-3.97/main/main.cc#L482-L525) |
| Arduino IMU example (SensorLib) | [Arduino/examples/06_QMI8658A/06_QMI8658A.ino#L9-L229](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/Arduino/examples/06_QMI8658A/06_QMI8658A.ino#L9-L229) |
| SensorLib v0.3.1 reset / init | [SensorQMI8658.hpp#L295-L321](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/Arduino/libraries/SensorLib/src/SensorQMI8658.hpp#L295-L321), [#L1987-L2030](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/Arduino/libraries/SensorLib/src/SensorQMI8658.hpp#L1987-L2030) |

## 2. SHTC3 temperature / humidity

**Address 0x70** (`SHTC3Addr`, `i2c_bsp.h`). Every command is a 2-byte big-endian word written
with no register byte, through `i2c_write_bytes()` / `i2c_read_bytes()` (see `io.md`).

| Command | Code | Datasheet timing / notes |
| --- | --- | --- |
| Wakeup | `0x3517` | Sleep → idle. Wake time refers to Table 5: t<sub>PU</sub> 180 µs typ / 240 µs max (inferred to apply) |
| Sleep | `0xB098` | ~0.3 µA in sleep |
| Soft reset | `0x805D` | t<sub>SR</sub> 180 µs typ / 240 µs max |
| Read ID | `0xEFC8` | Returns ID MSB, LSB, CRC. Product code: `(id & 0x083F) == 0x0807` (Table 15) |
| Measure, normal mode, T first, **no clock stretching** | `0x7866` | **The only measurement command upstream uses.** t<sub>MEAS</sub> 10.8 typ / 12.1 ms max |
| Other normal-mode variants (in enum, unused) | `0x7CA2` (T first, stretch), `0x58E0` (RH first), `0x5C24` (RH first, stretch) | |
| Low-power mode (not in upstream enum) | `0x609C` / `0x401A` (no stretch), `0x6458` / `0x44DE` (stretch) | 0.7 typ / 0.8 ms max |

**Init** (`i2c_shtc3_init`, called once from `app_main` after `button_Init()`):

```c
void i2c_shtc3_init(void)
{
    uint16_t id;
    // wake up the sensor from sleep mode
    SHTC3_Wakeup();
    // demonstartion of SoftReset command
    SHTC3_SoftReset();
    // wait for sensor to reset
    esp_rom_delay_us(100); //100us
    // demonstartion of GetId command
    SHTC3_GetId(&id);
    // ESP_LOGI(TAG,"0x%04x",id);
}
```

`SHTC3_Wakeup()` sends `0x35 0x17` and then `esp_rom_delay_us(100)`. `SHTC3_GetId()` does a write
of `0xEF 0xC8` followed by a repeated-start read of 3 bytes (`i2c_master_transmit_receive`) and checks
the CRC. The ID is thrown away, and no error is acted on.

**Measurement** (`SHTC3_GetTempAndHumiPolling`): write the command, sleep, read 6 bytes
`[T_msb, T_lsb, T_crc, RH_msb, RH_lsb, RH_crc]`:

```c
  uint8_t senBuf[2] = {(MEAS_T_RH_POLLING>>8),(MEAS_T_RH_POLLING&0xff)};
  err = i2c_write_bytes(SHTC3Addr, senBuf, 2);
  // ...
  vTaskDelay(pdMS_TO_TICKS(20));

  // if no error, read temperature and humidity raw values
  err = i2c_read_bytes(SHTC3Addr, bytes, 6);
  // ...
  error = SHTC3_CheckCrc(bytes, 2, bytes[2]);
  // ...
  error = SHTC3_CheckCrc(&bytes[3], 2, bytes[5]);
  // ...
  rawValueTemp = (bytes[0] << 8) | bytes[1];
  rawValueHumi = (bytes[3] << 8) | bytes[4];
  *temp = SHTC3_CalcTemperature(rawValueTemp);
  *humi = SHTC3_CalcHumidity(rawValueHumi);
```

**CRC and conversion.** CRC-8 with polynomial 0x31, init 0xFF, no reflection, no final XOR. This
matches datasheet Table 16 (test vectors: CRC(0x00)=0xAC, CRC(0xBEEF)=0x92). `0x131` works because
the `uint8_t` shift drops bit 8.

```c
#define CRC_POLYNOMIAL  0x131 // P(x) = x^8 + x^5 + x^4 + 1 = 100110001
  // ...
  uint8_t crc = 0xFF; // calculated checksum
  for(byteCtr = 0; byteCtr < nbrOfBytes; byteCtr++)
  {
    crc ^= (data[byteCtr]);
    for(bit = 8; bit > 0; --bit) {
      if(crc & 0x80) {
        crc = (crc << 1) ^ CRC_POLYNOMIAL;
      } else {
        crc = (crc << 1);
      }
    }
  }
// ...
static float SHTC3_CalcTemperature(uint16_t rawValue)
{
  // ...
  return 175 * (float)rawValue / 65536.0f - 45.0f;
}
// ...
static float SHTC3_CalcHumidity(uint16_t rawValue)
{
  // ...
  return 100 * (float)rawValue / 65536.0f;
}
```

The formulas match datasheet §5.11: T = −45 + 175·S/2¹⁶, RH = 100·S/2¹⁶.

**High-level API used by the app** (`SHTC3_GetEnvTemperatureHumidity`): wake, then up to **4**
attempts (`do … while(SHTC3_size--)` starting at 3), each preceded by a 100 ms delay, then sleep,
then a fixed **−1.5 °C** self-heating offset:

```c
    SHTC3_Wakeup();
    do{
        vTaskDelay(pdMS_TO_TICKS(100));
        error = SHTC3_GetTempAndHumiPolling(temperature, humidity);
        if(error == NO_ERROR){
            break;
        }
    } while(SHTC3_size--);

    SHTC3_Sleep();

    // The temperature detected by SHTC3 is generally 1 to 3 degrees Celsius higher than the normal temperature. Here, a relatively middle value is taken for deduction
    *temperature = *temperature - 1.5;
    // ...
    return (error == NO_ERROR) ? 0 : error;
```

A read costs ≥ 120 ms of blocking (100 ms + 20 ms) even on the first try. The Arduino example
(`04_I2C_SHTC3`) does the same sequence with `vTaskDelay(50 ms)` after wakeup, 20 ms after soft reset,
and subtracts **4 °C** inside the conversion (`SHTC3_PETP_NUM = 4`) instead of 1.5 °C.

## 3. QMI8658A IMU

**Address:** SA0 is tied to GND, so 0x6A (`QMI8658_SLAVE_ADDR_L`). `QMI8658_init()` probes `WHO_AM_I`
(reg 0x00, expect **0x05**) up to 5 times at 0x6A, then up to 5 times at 0x6B. After that it reads
`REVISION_ID` (0x01; datasheet default 0x68), then configures:

```c
        QMI8658_write_reg(QMI8658Register_Ctrl1, 0x60);
        QMI8658_config.inputSelection = QMI8658_CONFIG_ACCGYR_ENABLE; // QMI8658_CONFIG_ACCGYR_ENABLE;
        QMI8658_config.accRange = QMI8658AccRange_8g;
        QMI8658_config.accOdr = QMI8658AccOdr_1000Hz;
        QMI8658_config.gyrRange = QMI8658GyrRange_512dps; // QMI8658GyrRange_2048dps   QMI8658GyrRange_1024dps
        QMI8658_config.gyrOdr = QMI8658GyrOdr_1000Hz;
        // ... magOdr / magDev / aeOdr set, but MAG and AE are not in inputSelection
        QMI8658_Config_apply(&QMI8658_config);
        // ... then reads CTRL1..CTRL7 into a throwaway variable (debug leftover)
```

There is no soft reset, no wait for the 0x4D reset result, and no delay. `QMI8658_init()` runs late in
`app_main` (after the e-paper init), so the datasheet's 15 ms turn-on time has long passed.

**Resulting register writes**, in order (decoded with datasheet Table 22):

| Reg (addr) | Value | Datasheet meaning |
| --- | --- | --- |
| CTRL1 (0x02) | `0x60` | bit6 ADDR_AI=1 (auto-increment); bit5 BE=1 (datasheet: big-endian; reset default is already `0x20`); INT1_EN=INT2_EN=0, so both INT pins are high-Z; SensorDisable=0 |
| CTRL2 (0x03) | `0x23` | aST=0, aFS=010 → **±8 g** (4096 LSB/g), aODR=0011 → 1000 Hz accel-only, **896.8 Hz in 6-DOF mode** |
| CTRL5 (0x06) | `0x00` | Read-modify-write that ends in `ctl_dada = 0x00`, so **both LPFs off** (see bugs) |
| CTRL3 (0x04) | `0x43` | gST=0, gFS=100 → **±256 dps** per datasheet (upstream enum calls it 512 dps), gODR=0011 → 896.8 Hz |
| CTRL5 (0x06) | `0x00` | Written again by the gyro config, still 0 |
| CTRL7 (0x08) | `0x03` | aEN=1, gEN=1; DRDY_DIS=0 (DRDY routed to INT2, but INT2 is high-Z) |

The ODR, range and mode values are ORed straight into the register:

```c
    if (stEnable == QMI8658St_Enable)
        ctl_dada = (unsigned char)range | (unsigned char)odr | 0x80;
    else
        ctl_dada = (unsigned char)range | (unsigned char)odr;
    QMI8658_write_reg(QMI8658Register_Ctrl2, ctl_dada);
    // set LPF & HPF
    QMI8658_read_reg(QMI8658Register_Ctrl5, &ctl_dada, 1);
    ctl_dada &= 0xf0;
    if (lpfEnable == QMI8658Lpf_Enable)
    {
        ctl_dada |= A_LSP_MODE_3;
        ctl_dada |= 0x01;
    }
    // ...
    ctl_dada = 0x00;
    QMI8658_write_reg(QMI8658Register_Ctrl5, ctl_dada);
```

**Register addresses** (upstream enum, matching datasheet Table 19): STATUSINT 0x2D (bit7 CTRL9
CmdDone), STATUS0 0x2E (bit0 aDA, bit1 gDA), STATUS1 0x2F (motion flags, bit2 WoM),
TIMESTAMP 0x30–0x32 (24-bit), TEMP_L/H 0x33–0x34, **AX_L 0x35 … GZ_H 0x40** (12 bytes, little-endian
pairs), RESET 0x60 (not in the upstream enum).

**Data read and scaling** (`QMI8658_read_xyz`, compiled with `QMI8658_UINT_MG_DPS` defined): one
12-byte burst from AX_L, output in **mg** and **dps**:

```c
    QMI8658_read_reg(QMI8658Register_Ax_L, buf_reg, 12); // 0x19, 25
    raw_acc_xyz[0] = (short)((unsigned short)(buf_reg[1] << 8) | (buf_reg[0]));
    // ... raw_acc_xyz[1..2] from bytes 2..5, raw_gyro_xyz[0..2] from bytes 6..11, same L/H order
    // mg
    acc[AXIS_X] = (float)(raw_acc_xyz[AXIS_X] * 1000.0f) / acc_lsb_div;
    acc[AXIS_Y] = (float)(raw_acc_xyz[AXIS_Y] * 1000.0f) / acc_lsb_div;
    acc[AXIS_Z] = (float)(raw_acc_xyz[AXIS_Z] * 1000.0f) / acc_lsb_div;
    // ...
    // dps
    gyro[1] = (float)(raw_gyro_xyz[0] * 1.0f) / gyro_lsb_div;
    gyro[0] = (float)(raw_gyro_xyz[1] * 1.0f) / gyro_lsb_div;
    gyro[2] = (float)(raw_gyro_xyz[2] * 1.0f) / gyro_lsb_div;
```

The `// 0x19, 25` comment is a stale leftover; the enum value is 53 (0x35). `AXIS_Y = 0, AXIS_X = 1`,
so `acc[]` comes out as `[Ax, Ay, Az]` but `gyro[]` comes out as `[Gy, Gx, Gz]`. Divisors:
`acc_lsb_div` = 16384/8192/4096/2048 for 2/4/8/16 g (correct per datasheet Table 7). `gyro_lsb_div`
= 1024, 512, 256, 128, 64, 32, 16, 8 for enum codes 0–7, which is one step off the datasheet (Table 8:
code 0 = ±16 dps = 2048 LSB/dps … code 7 = ±2048 dps = 16 LSB/dps).

Other read functions: `QMI8658_read_acc_xyz()` returns **m/s²** (`raw * ONE_G / acc_lsb_div`,
`ONE_G = 9.807f`). `QMI8658_read_gyro_xyz()` returns dps without the X/Y swap. `QMI8658_readTemp()`
returns `int16(TEMP_H:TEMP_L) / 256.0f` °C, matching datasheet §3.9 (`T = TEMP_H + TEMP_L/256`).
`QMI8658_read_xyz_raw()` returns raw counts. Nothing in the app calls the last three.

**Interrupt pins** (header): `#define DOF_INT1 GPIO_NUM_39` and `#define DOF_INT2 GPIO_NUM_40`.
`qmi8658_gpio_int_init()` configures both as inputs with pull-ups (no ISR installed). It's only
called from `QMI8658_doCtrl9Command()`, which only `QMI8658_enableWakeOnMotion()` calls, and nothing
calls that. So the shipped app never touches GPIO39/40 for the IMU, and the IMU is purely polled.

## 4. How the app uses the sensors

- **SHTC3:** `page_clock.cc` calls `SHTC3_GetEnvTemperatureHumidity()` when it draws the clock face
  (lines 1112 and 1175). It ignores the return code, and prints `"%2.1f℃"` / `"%2.1f％"`.
- **QMI8658:** initialised only in interactive mode (`mode == 0` branch of `app_main`). The timed
  clock/calendar/weather modes skip it. `qmi8658_mutex` is created right after.
- **Settings page** (`page_settings_show`, reached by a long press of BOOT from the home menu):
  option 0 runs `SRAM_task()` once (free internal / PSRAM / 8-bit heap via `heap_caps_get_free_size`,
  SD capacity via `card_host->csd.capacity * 512` and `f_getfree("0:")`). Option 1 sets `mode = 1`,
  and it stays 1 while option 2 ("back") is highlighted. After that, every loop pass (driven by `wait_key_event_and_return_code(pdMS_TO_TICKS(1000))`, so about
  1 Hz) calls `qmi8658_task()`. That reads `QMI8658_read_xyz(acc, gyro, NULL)`, draws six
  `"%4.3fmg"` / `"%4.3fdps"` lines, partial-refreshes the panel, then classifies orientation under the
  mutex:

```c
int check_imu_status(const float acc[3], const float gyro[3]) {
    if(acc[0] > 800){
        qmi8658_status = 1;
    } else if (acc[1] > 900) {
        qmi8658_status = 2;
    } else if(acc[0] < -800){
        qmi8658_status = 3;
    } else if (acc[1] < -900) {
        qmi8658_status = 4;
    }
    return qmi8658_status;
}
```

  The status is sticky (it's left unchanged when no threshold is crossed). `qmi8658_status_t()` and
  `auto_rotate` are exported but nothing outside `page_settings.cc` uses them.

## 5. Porting notes and upstream bugs

**Arduino equivalents.**
- *QMI8658:* SensorLib (bundled v0.3.1, MIT). `qmi.begin(Wire, 0x6A, board::I2C_SDA, board::I2C_SCL)`.
  The Arduino example passes `QMI8658_H_SLAVE_ADDRESS`, which SensorLib defines as **0x6A**, with
  `QMI8658_L_SLAVE_ADDRESS` = **0x6B**. That's backwards from upstream's `_L`/`_H` naming, and
  SensorLib's *default* argument is 0x6B, so always pass the address. SensorLib does a proper soft
  reset in `begin()`: it writes 0xB0 to 0x60, polls 0x4D for 0x80 (timeout 500 ms), sets CTRL1 bit6,
  writes CTRL8 = 0x80 (STATUSINT.bit7 handshake), and runs `CTRL_CMD_COPY_USID`. The example then
  configures ±4 g at 1000 Hz with LPF mode 0, and ±64 dps at 896.8 Hz with LPF mode 3. Its gyro enum
  starts at `GYR_RANGE_16DPS = 0`, which matches the datasheet. Under `ARDUINO`, SensorLib only offers
  the `TwoWire&` / `SPIClass&` (and custom-callback) `begin()`; the `i2c_master_bus_handle_t` overload
  is compiled for pure ESP-IDF only.
- *SHTC3:* the upstream Arduino example has no library. It runs IDF `i2c_master` at 300 kHz on its own
  bus. With `Wire` it's about 30 lines: `beginTransmission(0x70); write(0x78); write(0x66);
  endTransmission(); delay(13); requestFrom(0x70, 6)`, check both CRCs, then convert. Third-party
  libraries (Adafruit/SparkFun SHTC3) also exist (inferred, not evaluated here).

**IMU interrupt: use GPIO40 (INT2), never GPIO39.**
- `es8311_bsp.h:30` defines `I2S_PA_PIN GPIO_NUM_39`, and `qmi8658_bsp.h:36` defines
  `DOF_INT1 GPIO_NUM_39`. Upstream assigns the same GPIO twice. If anything ever called
  `qmi8658_gpio_int_init()`, the speaker-amp enable would become an input with a pull-up (inferred:
  the amp could switch on).
- The Arduino example defines `SENSOR_IRQ 39`, but its interrupt code is guarded by `#if IMU_INT > 0`
  and `IMU_INT` is never defined, so it compiles out. Don't "fix" the example by wiring in
  `SENSOR_IRQ`.
- AGENTS.md: INT1 reaches GPIO39 only if R9 is fitted (it isn't by default). For our firmware, set
  CTRL1 bit4 (INT2_EN), read `board::IMU_INT2` (GPIO40), and route features to INT2. DRDY goes to
  INT2 when CTRL7.DRDY_DIS = 0; for WoM, set CAL1_H[7:6] to `01` or `11`. In SensorLib that's
  `setPins(board::IMU_INT2)` + `enableINT(INTERRUPT_PIN_2)`; `configWakeOnMotion()` already defaults
  to `INTERRUPT_PIN_2`.

**QMI8658 bugs to avoid copying.**
1. **Gyro scale is 2× too high.** The upstream `QMI8658_GyrRange` enum starts at "32 dps" for code 0,
   but on the QMI8658A code 0 is ±16 dps. Upstream writes code 4 (±256 dps, 128 LSB/dps) and divides
   by 64. Use the datasheet table (or SensorLib).
2. **LPF never enabled:** `config_acc`/`config_gyro` overwrite the computed CTRL5 value with `0x00`.
3. **Gyro X/Y swapped** in `QMI8658_read_xyz` only, so `acc[]` and `gyro[]` use different axis orders.
4. **Mixed units:** `read_xyz` gives mg; `read_acc_xyz` gives m/s².
5. **Timestamp unwrap is wrong:** on wrap it stores `timestamp + 0x1000000 - imu_timestamp` (a delta)
   as the new absolute value. It's unused in the app (`tim_count == NULL`).
6. **CTRL9 handshake is wrong:** `doCtrl9Command` polls STATUS1 bit2 (WoM) instead of STATUSINT bit7
   (CmdDone), never writes `CTRL_CMD_ACK` (0x00), `printf`s every 10 ms, and re-runs the GPIO init.
   `QMI8658_STATUS1_CMD_DONE (0x01)` names a reserved bit. `disableWakeOnMotion` skips the CTRL9 command
   that datasheet §12.6 requires.
7. **Dead QMI8658C/FIS-era code:** on the QMI8658A, CTRL4 (mag) and CTRL6 (AttitudeEngine) are
   reserved, and CTRL7 bits 3:2 are reserved. Don't port `config_mag`, `config_ae`, `read_ae` or the
   MAG/AE enable flags.
8. **Endianness:** the datasheet says CTRL1.BE=1 means big-endian reads, and the reset default has it
   set. Yet both upstream (`0x60`) and SensorLib (reset + set bit6, also leaving BE=1) parse the L
   register as the low byte, and upstream's settings page shows plausible values. So treat the
   `_L`/`_H` registers as little-endian (inferred from two drivers; confirm on hardware: Z ≈ +1000 mg
   lying flat).
9. **Reset value typo in the datasheet:** §5.9 says write **0xB0** to RESET (0x60); §7.4 says 0x0B.
   SensorLib uses 0xB0.
10. Each failed probe at 0x6B creates and deletes a temporary I2C device handle (see `io.md`). Expect
    NACK error logs if the chip doesn't answer at 0x6A (inferred).

**SHTC3 pitfalls.**
1. **Delays shorter than the datasheet:** 100 µs after wakeup and after soft reset, against a 240 µs
   maximum. Use ≥ 240 µs (or 1 ms).
2. **Measurement wait vs tick rate:** `vTaskDelay(pdMS_TO_TICKS(20))` with upstream's
   `CONFIG_FREERTOS_HZ=100` is 2 ticks, which can be as short as ~10 ms, under the 12.1 ms maximum
   (inferred). The retry loop hides it. Our Arduino build runs at 1000 Hz; still wait ≥ 13 ms, or
   retry the read while it NACKs.
3. **Error path:** the −1.5 °C is applied even when every attempt failed, and the caller ignores the
   return, so the clock shows "-1.5℃ 0％" (callers pass 0-initialised floats).
4. **Offset disagreement:** IDF subtracts 1.5 °C, the Arduino example subtracts 4 °C. Self-heating from
   the board depends on what's powered; calibrate on our hardware rather than copying either value.
5. ID never validated; check `(id & 0x083F) == 0x0807`.
6. `SHTC3_Init()` and `SHTC3_GetTempAndHumi()` (the clock-stretching variant) are declared in
   `shtc3_bsp.h` but never defined, so calling them fails at link time. `i2c_shtc3_task()` is an unused
   demo loop.

## 6. Licensing

`shtc3_bsp.*`, `qmi8658_bsp.*`, `page_settings.*` and the Arduino `04_I2C_SHTC3` / `06_QMI8658A` sources
carry **no license or copyright header**, and the upstream repo has no top-level LICENSE.
The SHTC3 code's structure and comments (`etError`, `SHTC3_CheckCrc`, "demonstartion of …") follow
Sensirion's sample driver (inferred). The QMI8658 code descends from QST's FIS210x/QMI8658 reference
driver (inferred from commented-out `fis210x` log strings and `struct FisImuRawSample`). SensorLib is **MIT**
(`Copyright (c) 2022 lewis he`, `Arduino/libraries/SensorLib/LICENSE`). Use SensorLib as a
PlatformIO dependency, and re-implement the SHTC3 code from the datasheet facts above.
