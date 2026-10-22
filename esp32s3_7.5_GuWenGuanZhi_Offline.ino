// ============================================================
//  ESP32-S3 N16R8 《古文观止》每日一篇 · 离线版 · U8g2同款Unifont字库
//  屏幕：7.5 寸 480x800 黑白红三色墨水屏 GDEW075Z08 (GD7965)
//  驱动：GxEPD2_750c_GDEW075Z08
//
//  字库：U8g2 同款 Unifont 16px 点阵字库（guwen_song_font.h），覆盖 GB2312
//   全部可打印字符 + ASCII 可打印（数字/斜杠等，供页码显示）。
//   GB2312 之外的生僻字（扩展区等）字库未收录，绘制时输出空格占位。
//   ASCII 空格(0x20)/全角空格(0x3000)由程序特殊处理；页码数字/斜杠正常。
//  每日一篇、NVS 持久化、deep sleep 24h、段落左对齐+段首缩进、长文末尾“…”均保留。
//
//  分区表建议：Tools > Partition Scheme 选择大 APP 分区
//    （如 “Huge APP (3MB No OTA)” 或 16M Flash 带 ≥4MB APP 的选项）。
//
//  引脚：CS=5, DC=17, RST=16, BUSY=4, SCK=12, MOSI=11
// ============================================================

#include <GxEPD2_3C.h>
#include <SPI.h>
#include <esp_sleep.h>
#include <Preferences.h>
#include "guwenguanzhi_data.h"
#include "guwen_song_font.h"

// ============ 硬件配置（ESP32-S3 引脚） ============
#define CS_PIN   5
#define DC_PIN   17
#define RST_PIN  16
#define BUSY_PIN 4
#define SCK_PIN  12
#define MOSI_PIN 11

#define SCREEN_WIDTH  480
#define SCREEN_HEIGHT 800

// 分页缓冲：page_height=160 → 缓冲 100*160*2=32KB，避免 DRAM 溢出
GxEPD2_3C<GxEPD2_750c_GDEW075Z08, 160> display(
  GxEPD2_750c_GDEW075Z08(CS_PIN, DC_PIN, RST_PIN, BUSY_PIN)
);

// ============ 布局配置（480x800 竖屏） ============
#define MARGIN_X 5      // 左右安全边距（用户要求 2~5px，取 5）
#define MARGIN_TOP 45
#define MARGIN_BOTTOM 45

#define FONT_PX SONG_FONT_PX   // Unifont 16px（U8g2同款，笔画方正无断划）

#define GAP_TITLE_AUTHOR  18
#define GAP_AUTHOR_LINE   24
#define GAP_LINE_CONTENT  30

// ============ 休眠间隔（默认 24 小时 = 每天一篇） ============
#define DEEP_SLEEP_HOURS 24

// ============ NVS 持久化 ============
Preferences prefs;
const char* PREFS_NS  = "guwen";
const char* PREFS_KEY = "idx";
int currentIndex = 0;

// ============ UTF-8 工具 ============
// 由 UTF-8 首字节判定该字符占用的字节数
static int charLenOf(unsigned char c) {
  if ((c & 0xE0) == 0xC0) return 2;
  if ((c & 0xF0) == 0xE0) return 3;
  if ((c & 0xF8) == 0xF0) return 4;
  return 1;
}

// 由 UTF-8 字节序列解码出 Unicode 码点
static uint32_t utf8ToCode(const char* s) {
  unsigned char c0 = (unsigned char)s[0];
  if (c0 < 0x80) return c0;
  if ((c0 & 0xE0) == 0xC0)
    return ((uint32_t)(c0 & 0x1F) << 6) | ((unsigned char)s[1] & 0x3F);
  if ((c0 & 0xF0) == 0xE0)
    return ((uint32_t)(c0 & 0x0F) << 12) | (((unsigned char)s[1] & 0x3F) << 6) | ((unsigned char)s[2] & 0x3F);
  // 4 字节（本项目正文不含 4 字节字符，缺字由 □ 处理，此处兼容）
  return ((uint32_t)(c0 & 0x07) << 18) | (((unsigned char)s[1] & 0x3F) << 12)
       | (((unsigned char)s[2] & 0x3F) << 6) | ((unsigned char)s[3] & 0x3F);
}

// ============ 全宋体字库查询 ============
// 二分查找码点，返回在字库中的索引；未收录返回 -1
static int songFind(uint32_t code) {
  int lo = 0, hi = SONG_COUNT - 1;
  while (lo <= hi) {
    int mid = (lo + hi) >> 1;
    uint32_t v = pgm_read_dword(&song_code[mid]);
    if (v == code) return mid;
    else if (v < code) lo = mid + 1;
    else hi = mid - 1;
  }
  return -1;
}

// 单个字符的显示宽度（像素）；缺字(未收录)按 FONT_PX 宽
static int songCharWidth(uint32_t code) {
  if (code == 0x20) return FONT_PX / 2;     // ASCII 空格：半格
  if (code == 0x3000) return FONT_PX;       // 全角空格 U+3000：全角
  int idx = songFind(code);
  if (idx < 0) return FONT_PX;              // 生僻字：空格占位宽(全角)
  return pgm_read_byte(&song_width[idx]);
}

// 在 (x, topY) 绘制单个字符，返回实际占用的宽度（像素）。字符顶部对齐。
static int songDrawChar(uint32_t code, int x, int topY, uint16_t color) {
  if (code == 0x20 || code == 0x3000) return songCharWidth(code);  // 空格：不绘制
  int idx = songFind(code);
  if (idx < 0) return FONT_PX;              // 生僻字(非GB2312)：空格占位, 不绘制
  const uint8_t* g = song_glyph[idx];
  int w = pgm_read_byte(&song_width[idx]);

  // 每行字节数随字库字号自适应（16px=2 字节/行，24px=3）
  int bytesPerRow = SONG_GLYPH_BYTES / SONG_FONT_PX;
  for (int yy = 0; yy < SONG_FONT_PX; yy++) {
    for (int bb = 0; bb < bytesPerRow; bb++) {
      uint8_t byte = pgm_read_byte(&g[yy * bytesPerRow + bb]);
      if (byte == 0) continue;
      for (int bit = 0; bit < 8; bit++) {
        int xpos = bb * 8 + bit;
        if (xpos < w && (byte & (0x80 >> bit)))
          display.drawPixel(x + xpos, topY + yy, color);
      }
    }
  }
  return w;
}

// 整段文本的测量宽度（逐字符累加）
int textWidthAll(const char* text) {
  int w = 0;
  const char* p = text;
  while (*p) {
    unsigned char c = (unsigned char)*p;
    int len = charLenOf(c);
    w += songCharWidth(utf8ToCode(p));
    p += len;
  }
  return w;
}

// 整段文本逐字符绘制（顶部对齐）
void drawTextAll(const char* text, int x, int topY, uint16_t color) {
  const char* p = text;
  int cx = x;
  while (*p) {
    unsigned char c = (unsigned char)*p;
    int len = charLenOf(c);
    cx += songDrawChar(utf8ToCode(p), cx, topY, color);
    p += len;
  }
}

// ============ 绘制函数 ============
// 顶部对齐、水平居中；超宽时贴左边界
void drawUTF8CenteredTop(const char* text, int topY, uint16_t color) {
  int width = textWidthAll(text);
  int maxWidth = SCREEN_WIDTH - MARGIN_X * 2;
  int x = (width > maxWidth) ? MARGIN_X : (SCREEN_WIDTH - width) / 2;
  drawTextAll(text, x, topY, color);
}

// 按中文习惯折行绘制正文：段落左对齐、段首缩进两个汉字；
// 每行预留右缘安全余量，杜绝最后一个字符超出右边界被裁；
// 最后一屏放不下时在末尾追加“…”省略号。全宋体逐字符等宽处理。
int drawUTF8WrappedTop(const char* text, int startY, int maxWidth,
                       uint16_t color, int lineGap, int maxLines) {
  String remaining = String(text);
  remaining.replace("\\n", "\n");

  // 段首缩进：两个汉字宽度（16px 汉字 → 32px）
  int indent = 2 * songCharWidth(utf8ToCode("\xE5\xAD\x97"));  // “字”
  // 右缘安全余量：6px
  const int rightPad = 6;
  int safeMaxWidth = maxWidth - rightPad;
  int ellipsisCode = utf8ToCode("\xE2\x80\xA6");               // “…”
  int ellipsisWidth = songCharWidth(ellipsisCode);

  int currentTopY = startY;
  int lineCount = 0;

  while (remaining.length() > 0 && lineCount < maxLines) {
    int newlinePos = remaining.indexOf('\n');
    String para;
    if (newlinePos >= 0) {
      para = remaining.substring(0, newlinePos);
      remaining = remaining.substring(newlinePos + 1);
    } else {
      para = remaining;
      remaining = "";
    }
    if (para.length() == 0) continue;   // 跳过空段

    bool firstLineOfPara = true;
    String processLine = para;
    while (processLine.length() > 0) {
      int availWidth = safeMaxWidth - (firstLineOfPara ? indent : 0);
      if (availWidth < FONT_PX) availWidth = FONT_PX;

      int fitLen = 0;
      int lineWidth = 0;
      int i = 0;
      while (i < processLine.length()) {
        unsigned char c = (unsigned char)processLine[i];
        int charLen = charLenOf(c);
        String testChar = processLine.substring(i, i + charLen);
        int cw = songCharWidth(utf8ToCode(testChar.c_str()));

        int reservedWidth = 0;
        if (lineCount >= maxLines - 1 &&
            (remaining.length() > 0 || i + charLen < processLine.length())) {
          reservedWidth = ellipsisWidth;
        }
        if (lineWidth + cw + reservedWidth > availWidth && i > 0) break;
        lineWidth += cw;
        i += charLen;
        fitLen = i;
      }

      if (fitLen == 0) {
        unsigned char c = (unsigned char)processLine[0];
        fitLen = charLenOf(c);
      }

      String drawLine = processLine.substring(0, fitLen);
      processLine = processLine.substring(fitLen);

      bool needEllipsis = false;
      if (lineCount >= maxLines - 1 &&
          (processLine.length() > 0 || remaining.length() > 0)) {
        needEllipsis = true;
      }

      // 左对齐：x 固定在左边距 + 段首缩进
      int x = MARGIN_X + (firstLineOfPara ? indent : 0);
      if (x < MARGIN_X) x = MARGIN_X;
      int lineTopY = currentTopY;

      // 逐字符绘制本行
      int cx = x;
      int j = 0;
      while (j < drawLine.length()) {
        unsigned char c = (unsigned char)drawLine[j];
        int charLen = charLenOf(c);
        String cc = drawLine.substring(j, j + charLen);
        cx += songDrawChar(utf8ToCode(cc.c_str()), cx, lineTopY, color);
        j += charLen;
      }
      if (needEllipsis) {
        cx += songDrawChar(ellipsisCode, cx, lineTopY, color);
      }

      currentTopY += FONT_PX + lineGap;
      lineCount++;
      if (lineCount >= maxLines) return lineCount;
      firstLineOfPara = false;
    }
  }
  return lineCount;
}

// ============ 显示一篇《古文观止》 ============
void displayArticle(const GuwenArticle& art) {
  display.setFullWindow();
  display.firstPage();

  do {
    display.fillScreen(GxEPD_WHITE);

    int maxTextWidth = SCREEN_WIDTH - MARGIN_X * 2;

    // 标题（红色，全宋体16px）
    int titleTopY = MARGIN_TOP;
    drawUTF8CenteredTop(art.title, titleTopY, GxEPD_RED);

    // 朝代 · 作者 · 出处（黑色）
    String authorInfo = String(art.dynasty);
    if (strlen(art.author) > 0) {
      authorInfo += " · ";
      authorInfo += String(art.author);
    }
    if (strlen(art.source) > 0) {
      authorInfo += " · ";
      authorInfo += String(art.source);
    }
    int authorTopY = titleTopY + FONT_PX + GAP_TITLE_AUTHOR;
    drawUTF8CenteredTop(authorInfo.c_str(), authorTopY, GxEPD_BLACK);

    // 分割线（红色，贯穿正文区宽度）
    int lineY = authorTopY + FONT_PX + GAP_AUTHOR_LINE;
    display.drawLine(MARGIN_X, lineY, SCREEN_WIDTH - MARGIN_X, lineY, GxEPD_RED);

    // 正文（黑色），一屏放不下则末尾省略号
    int contentTopY = lineY + GAP_LINE_CONTENT;
    int contentMaxHeight = SCREEN_HEIGHT - MARGIN_BOTTOM - contentTopY;
    int contentLineGap = max(6, FONT_PX / 2 + 2);
    int maxLines = max(1, contentMaxHeight / (FONT_PX + contentLineGap));

    drawUTF8WrappedTop(art.content, contentTopY, maxTextWidth,
                       GxEPD_BLACK, contentLineGap, maxLines);

    // 页脚：显示“第 N / 222 篇”（黑色，居中）
    String pageInfo = String(currentIndex + 1) + " / " + String(guwen_article_count);
    drawUTF8CenteredTop(pageInfo.c_str(), SCREEN_HEIGHT - MARGIN_BOTTOM + 8, GxEPD_BLACK);

  } while (display.nextPage());
}

// ============ 错误显示 ============
void displayError(const char* msg) {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    int w = textWidthAll(msg);
    int x = (SCREEN_WIDTH - w) / 2;
    int topY = (SCREEN_HEIGHT - FONT_PX) / 2;
    drawTextAll(msg, x, topY, GxEPD_RED);
  } while (display.nextPage());
}

// ============ Setup ============
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== ESP32-S3 GuWen GuanZhi Daily (Offline, Song Font) ===");

  // ESP32-S3 SPI：SCK=12, MISO=-1(未用), MOSI=11, CS=5
  SPI.begin(SCK_PIN, -1, MOSI_PIN, CS_PIN);

  // 4MHz SPI 加速大屏数据传输
  display.init(4000000, true, 2, false);
  display.setRotation(1);

  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
  } while (display.nextPage());

  Serial.println("Display initialized");

  // ---- 读取并轮换索引（NVS 持久化，掉电不丢） ----
  prefs.begin(PREFS_NS, false);
  currentIndex = prefs.getInt(PREFS_KEY, 0);
  if (currentIndex < 0 || currentIndex >= guwen_article_count) {
    currentIndex = 0;
  }

  const GuwenArticle& art = guwen_articles[currentIndex];
  Serial.printf("Today #%d: %s《%s》 %s %s\n",
                currentIndex + 1, art.dynasty, art.title,
                art.author, art.source);

  displayArticle(art);

  // 保存下一篇索引
  int nextIndex = (currentIndex + 1) % guwen_article_count;
  prefs.putInt(PREFS_KEY, nextIndex);
  prefs.end();

  Serial.printf("Next index saved: %d\n", nextIndex);

  // ---- 进入休眠 ----
  display.hibernate();   // 关升压，压到 µA 级
  Serial.flush();

  unsigned long long sleepUs =
    (unsigned long long)DEEP_SLEEP_HOURS * 3600ULL * 1000000ULL;
  Serial.printf("Deep sleep for %d hours...\n", DEEP_SLEEP_HOURS);
  esp_sleep_enable_timer_wakeup(sleepUs);
  esp_deep_sleep_start();
}

void loop() {
  // 不使用
}
