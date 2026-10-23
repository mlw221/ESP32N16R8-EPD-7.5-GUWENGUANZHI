# ESP32S3 N16R8+EPD-7.5
ESP32-S3 N16R8 《古文观止》每日一篇 · 离线版 · U8g2同款Unifont字库	
屏幕：7.5 寸 480x800 黑白红三色墨水屏 GDEW075Z08 (GD7965)	
驱动：GxEPD2_750c_GDEW075Z08	

字库：U8g2 同款 Unifont 16px 点阵字库（guwen_song_font.h），覆盖 GB2312	
全部可打印字符 + ASCII 可打印（数字/斜杠等，供页码显示）。	
GB2312 之外的生僻字（扩展区等）字库未收录，绘制时输出空格占位。	
ASCII 空格(0x20)/全角空格(0x3000)由程序特殊处理；页码数字/斜杠正常。	
每日一篇、NVS 持久化、deep sleep 24h、段落左对齐+段首缩进、长文末尾“…”均保留。	

分区表建议：Tools > Partition Scheme 选择大 APP 分区	
（如 “Huge APP (3MB No OTA)” 或 16M Flash 带 ≥4MB APP 的选项）。	

引脚：CS=5, DC=17, RST=16, BUSY=4, SCK=12, MOSI=11	
