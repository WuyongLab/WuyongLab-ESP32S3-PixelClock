# 接线与供电

## HUB75E J2 接口

正面对着接口，1 脚位于左上角。

| 引脚 | 信号 | ESP32-S3 |
|---:|---|---:|
| 1 | R1 | GPIO4 |
| 2 | G1 | GPIO5 |
| 3 | B1 | GPIO6 |
| 4 | GND | GND |
| 5 | R2 | GPIO7 |
| 6 | G2 | GPIO15 |
| 7 | B2 | GPIO16 |
| 8 | E | GPIO10 |
| 9 | A | GPIO17 |
| 10 | B | GPIO18 |
| 11 | C | GPIO8 |
| 12 | D | GPIO9 |
| 13 | CLK | GPIO11 |
| 14 | LAT/STB | GPIO12 |
| 15 | OE | GPIO13 |
| 16 | GND | GND |

## 息屏按键

- 按键一端接 GPIO14
- 另一端接 GND
- 程序使用内部上拉，无需外接上拉电阻

## 供电

- LED 点阵屏使用独立 5V 大电流电源
- LED 屏 GND、外部电源 GND 和 ESP32-S3 GND 必须共地
- 不要通过 ESP32-S3 开发板给整块点阵屏供电
- 首次测试建议从低亮度开始
