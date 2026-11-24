// gen_cjk.m — 把中文字形烘焙成 1-bit 点阵表，供 ESP32 上直接用 fillRect 画
// 用法: ./gen_cjk <字符清单.txt> <输出头文件.h>
// 只处理非 ASCII（汉字 + 中文标点 + …·±），ASCII 仍由 5x7 字库负责
#import <Cocoa/Cocoa.h>

static const int SIZES[3] = {16, 24, 32};
static const int NSIZE = 3;

// 小字号用中黑（否则 16px 下笔画会断），大字号用常规（否则 32px 下笔画会糊成块）
static NSFont *pickFont(CGFloat size, int S) {
  NSString *name = (S <= 20) ? @"PingFangSC-Semibold" : @"PingFangSC-Regular";
  NSFont *f = [NSFont fontWithName:name size:size];
  if (!f) f = [NSFont fontWithName:@"PingFang SC" size:size];
  if (!f) f = [NSFont systemFontOfSize:size];
  return f;
}

// 返回 S*S 的 0/1 位图（行优先，从上到下）
// 4 倍超采样 + 覆盖率阈值：比直接在小字号上二值化更接近真实字形（笔画不会糊成块）
static void renderGlyph(unichar ch, int S, unsigned char *out, int previewAscii) {
  const int SS = 6;   /* 6 倍超采样：小字号的断笔明显更少 */
  int B = S * SS;
  memset(out, 0, S * S);
  NSFont *font = pickFont((CGFloat)S * 0.95f * SS, S);
  NSString *s = [NSString stringWithCharacters:&ch length:1];

  NSBitmapImageRep *rep = [[NSBitmapImageRep alloc]
      initWithBitmapDataPlanes:NULL pixelsWide:B pixelsHigh:B
                  bitsPerSample:8 samplesPerPixel:1 hasAlpha:NO isPlanar:NO
                 colorSpaceName:NSDeviceWhiteColorSpace
                    bytesPerRow:B bitsPerPixel:8];
  unsigned char *buf = [rep bitmapData];
  memset(buf, 0, (size_t)B * B);

  [NSGraphicsContext saveGraphicsState];
  [NSGraphicsContext setCurrentContext:
      [NSGraphicsContext graphicsContextWithBitmapImageRep:rep]];

  NSDictionary *at = @{NSFontAttributeName : font,
                       NSForegroundColorAttributeName : [NSColor whiteColor]};
  NSSize sz = [s sizeWithAttributes:at];
  CGFloat x = (B - sz.width) / 2.f;
  CGFloat baselineFromBottom = (B - (font.ascender - font.descender)) / 2.f + font.descender;
  CGFloat y = baselineFromBottom - font.descender;
  [s drawAtPoint:NSMakePoint(x, y) withAttributes:at];

  [NSGraphicsContext restoreGraphicsState];

  for (int r = 0; r < S; r++) {
    for (int c = 0; c < S; c++) {
      int sum = 0;
      for (int yy = 0; yy < SS; yy++) {
        int rowFlip = r * SS + yy;                // 位图内存是自上而下
        for (int xx = 0; xx < SS; xx++) sum += buf[rowFlip * B + c * SS + xx];
      }
      int cov = sum / (SS * SS);
      int th = (S <= 16) ? 118 : (S <= 24 ? 126 : 132);
      out[r * S + c] = (cov >= th) ? 1 : 0;
    }
  }
  if (previewAscii) {
    printf("---- U+%04X ----\n", ch);
    for (int r = 0; r < S; r++) {
      for (int c = 0; c < S; c++) putchar(out[r * S + c] ? '#' : '.');
      putchar('\n');
    }
  }
}

int main(int argc, const char **argv) {
  @autoreleasepool {
    if (argc < 3) { printf("usage: gen_cjk <chars.txt> <out.h> [previewChar]\n"); return 1; }
    NSString *txt = [NSString stringWithContentsOfFile:[NSString stringWithUTF8String:argv[1]]
                                               encoding:NSUTF8StringEncoding error:nil];
    if (!txt) { printf("cannot read %s\n", argv[1]); return 1; }

    NSMutableArray *chars = [NSMutableArray array];
    for (NSUInteger i = 0; i < txt.length; i++) {
      unichar c = [txt characterAtIndex:i];
      if (c < 0x80) continue;                   // 只收中文
      NSNumber *boxed = @(c);
      if (![chars containsObject:boxed]) [chars addObject:boxed];
    }
    printf("chars: %lu\n", (unsigned long)chars.count);

    int previewChar = (argc > 3) ? (int)strtol(argv[3], NULL, 16) : -1;

    NSMutableString *h = [NSMutableString string];
    [h appendString:@"#pragma once\n"];
    [h appendString:@"/* 自动生成，勿手改：中文字形点阵表（16px / 24px / 32px，1-bit，行优先）\n"];
    [h appendString:@"   数据在 cjk_font.cpp 里，这里只声明，避免每个翻译单元都存一份 */\n"];
    [h appendString:@"#include <stdint.h>\n\n"];
    [h appendString:@"typedef struct {\n  uint32_t cp;\n  const uint8_t *p16;\n  const uint8_t *p24;\n  const uint8_t *p32;\n} CjkGlyph;\n\n"];
    [h appendString:@"extern const CjkGlyph CJK_TABLE[];\n"];
    [h appendString:@"extern const int CJK_COUNT;\n"];

    NSString *outH = [NSString stringWithUTF8String:argv[2]];
    NSString *outC = [[outH stringByDeletingPathExtension] stringByAppendingPathExtension:@"cpp"];

    NSMutableString *c = [NSMutableString string];
    [c appendString:@"/* 自动生成，勿手改：由 tools/gen_cjk 从苹方渲染得到 */\n"];
    [c appendString:@"#include \"cjk_font.h\"\n\n"];

    int n = (int)chars.count;
    unsigned char *bits = (unsigned char *)malloc(32 * 32);
    for (int side = 0; side < NSIZE; side++) {
      int S = SIZES[side];
      int bpr = (S + 7) / 8;
      for (int i = 0; i < n; i++) {
        unichar ch0 = (unichar)[chars[i] unsignedShortValue];
        renderGlyph(ch0, S, bits, previewChar == (int)ch0);
        [c appendFormat:@"static const uint8_t CJK%d_%d[%d] = {", S, i, S * bpr];
        for (int y = 0; y < S; y++) {
          for (int b = 0; b < bpr; b++) {
            uint8_t v = 0;
            for (int k = 0; k < 8; k++) {
              int x = b * 8 + k;
              if (x < S && bits[y * S + x]) v |= (uint8_t)(0x80 >> k);
            }
            [c appendFormat:@"%s0x%02X", (y == 0 && b == 0) ? "" : ",", v];
          }
        }
        [c appendString:@"};\n"];
      }
    }
    free(bits);

    [c appendString:@"\nconst CjkGlyph CJK_TABLE[] = {\n"];
    for (int i = 0; i < n; i++) {
      [c appendFormat:@"  {0x%04X, CJK16_%d, CJK24_%d, CJK32_%d},\n", [chars[i] unsignedShortValue], i, i, i];
    }
    [c appendString:@"};\n"];
    [c appendFormat:@"extern const int CJK_COUNT = %d;\n", n];

    [h writeToFile:outH atomically:YES encoding:NSUTF8StringEncoding error:nil];
    [c writeToFile:outC atomically:YES encoding:NSUTF8StringEncoding error:nil];
    printf("wrote %s + cjk_font.cpp\n", argv[2]);
  }
  return 0;
}
