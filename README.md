# ESP32-S3 64×64 HUB75E 像素时钟 v1.0

基于 Clockwise 开源项目移植并二次开发，适用于 ESP32-S3 与 64×64 HUB75E RGB 点阵屏。

## 主要功能

- 数字时钟、模拟时钟和彩点动态表盘
- Mario 像素风表盘
- Pokemon/Pokedex 图鉴表盘
- 64×64 图片轮播
- 浏览器像素画板
- 4 组自定义静态或动态时钟
- 火焰、星光、雨滴像素特效
- LittleFS 保存图片、画板和自定义表盘
- GPIO14 实体息屏按键
- 开机显示当前 IP 地址
- 配网热点强制门户

## 首次配网

1. 设备未保存 Wi-Fi 或连接失败时，会开启热点 `Clockwise-Setup`。
2. 热点密码为 `12345678`。
3. 手机或电脑连接热点后，系统通常会自动弹出配网页面。
4. 未自动弹出时，打开任意网页或访问 `http://192.168.4.1`。
5. 填写 Wi-Fi 名称和密码并保存，设备会在不重启主程序的情况下尝试连接。
6. 新网络连接失败时，设备会恢复配置热点。

## Arduino 环境

建议使用支持 ESP32-S3 的 Arduino ESP32 Core，并安装：

- `ESP32-HUB75-MatrixPanel-I2S-DMA`

其余库由 ESP32 Arduino Core 提供。

主程序文件：

`WuyongLab_ESP32S3_PixelClock_v1_0.ino`

## 文件说明

- `web_ui.h`：媒体工作室页面
- `project_identity.*`：项目身份数据
- `integrity_guard.*`：项目身份完整性校验和备用标识
- `gfx/`：表盘字体与图形资源
- `WIRING.md`：接线和供电说明
- `CHANGELOG.md`：版本变化
- `THIRD_PARTY_NOTICES.md`：第三方来源和素材说明

## 存储说明

正式版不会在 LittleFS 挂载失败时自动格式化。若文件系统异常，媒体功能会停用并在串口输出错误，避免因临时挂载问题误删图片和配置。


## 许可证

Clockwise 衍生代码应保留上游 MIT License 和版权声明。Mario、Pokemon 等主题素材的权利状态见 `THIRD_PARTY_NOTICES.md`。
