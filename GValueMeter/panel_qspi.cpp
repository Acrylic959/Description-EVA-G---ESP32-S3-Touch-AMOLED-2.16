#include "panel_qspi.h"

#include <driver/spi_master.h>
#include <esp_heap_caps.h>
#include <string.h>

#if __has_include(<esp_cache.h>)
#include <esp_cache.h>
#define PQ_HAVE_MSYNC 1
#endif

#include "pin_config.h"

/* ------------------------------ 常量 ------------------------------ */
#define PQ_HOST        SPI2_HOST          /* 与 Arduino_ESP32QSPI 同一个主机 */
#define PQ_HZ          80000000           /* QIO 80MHz × 4 线 = 40MB/s（CO5300 额定上限） */
#define PQ_CHUNK_PX    8192               /* 每块 16KB */
#define PQ_POOL        8                  /* 8 块 16KB DMA 缓冲 = 128KB 内部 RAM */
#define PQ_CHUNK_BYTES (PQ_CHUNK_PX * 2)
#define PQ_QUEUE       16                 /* 队列深度 ≥ 池深度 */

/* CO5300 QSPI 协议：0x02 = 写命令（24bit 地址 = 寄存器<<8），0x32 = 四线写显存 */
#define PQ_CMD_WRITE   0x02
#define PQ_CMD_QUADWR  0x32
#define PQ_ADDR_QUADWR 0x003C00
#define PQ_REG_CASET   0x2A
#define PQ_REG_PASET   0x2B
#define PQ_REG_RAMWR   0x2C

static spi_device_handle_t s_dev = nullptr;

/* 实测：ESP32-S3 GPSPI 的 DMA 不接受 PSRAM 地址（spi_device_queue_trans 返回
 * ESP_ERR_INVALID_ARG=257），所以只能把像素拷进内部 RAM 再走 DMA。
 * 优化点改成：提高时钟 + 加深 DMA 池，让"排队阻塞"尽量小。 */
static bool s_direct = false;

static uint8_t              *s_buf[PQ_POOL];
static spi_transaction_ext_t s_tr[PQ_POOL];
static int                   s_poolN = 0;
static int                   s_qpos = 0;
static int                   s_queued = 0;
static bool                  s_first = true;

static uint32_t s_frameStartUs = 0, s_transferUs = 0;
static uint32_t s_pushUs = 0, s_flushUs = 0, s_fillUs = 0, s_queueUs = 0;
static uint32_t s_statDirect = 0, s_statCopy = 0;

uint32_t panelLastTransferUs() { return s_transferUs; }
uint32_t panelPushUs()  { return s_pushUs; }
uint32_t panelFlushUs() { return s_flushUs; }
uint32_t panelFillUs()  { return s_fillUs; }
uint32_t panelQueueUs() { return s_queueUs; }
uint32_t panelDirectFrames()  { return s_statDirect; }
uint32_t panelCopyFrames()    { return s_statCopy; }

/* ------------------------- 初始化：挂一个 DMA 设备 ------------------------- */
bool panelAsyncBegin() {
  if (s_dev) return true;

  spi_device_interface_config_t cfg = {};
  cfg.command_bits   = 8;
  cfg.address_bits   = 24;
  cfg.dummy_bits     = 0;
  cfg.mode           = 0;
  cfg.clock_source   = SPI_CLK_SRC_DEFAULT;
  cfg.clock_speed_hz = PQ_HZ;
  cfg.input_delay_ns = 0;
  cfg.spics_io_num   = -1;                 /* CS 自己控制，整帧保持低电平 */
  cfg.flags          = SPI_DEVICE_HALFDUPLEX;
  cfg.queue_size     = PQ_QUEUE;

  esp_err_t err = spi_bus_add_device(PQ_HOST, &cfg, &s_dev);
  if (err != ESP_OK) {
    Serial.printf("[LCD] panel device add failed: %d\r\n", (int)err);
    s_dev = nullptr;
    return false;
  }

  int got = 0;
  for (int i = 0; i < PQ_POOL; i++) {
    s_buf[i] = (uint8_t *)heap_caps_malloc(PQ_CHUNK_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_buf[i]) break;
    got++;
  }
  s_poolN = got;
  for (int i = 0; i < PQ_POOL; i++) memset(&s_tr[i], 0, sizeof(s_tr[i]));
  if (s_poolN < 2) {
    Serial.printf("[LCD] DMA pool too small (%d), free=%u\r\n",
                  s_poolN, (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    return false;
  }

  pinMode(LCD_CS, OUTPUT);
  digitalWrite(LCD_CS, HIGH);
  Serial.printf("[LCD] async QSPI push ready: %dMHz x4, pool=%dx%dKB, queue=%d\r\n",
                (int)(PQ_HZ / 1000000), s_poolN, PQ_CHUNK_BYTES / 1024, PQ_QUEUE);
  return true;
}

/* ------------------------- 短命令：轮询式一次事务 ------------------------- */
static void xferCmd(uint32_t addr, const uint8_t *data, int n) {
  spi_transaction_ext_t t = {};
  t.base.flags  = SPI_TRANS_MULTILINE_CMD | SPI_TRANS_MULTILINE_ADDR;
  t.base.cmd    = PQ_CMD_WRITE;
  t.base.addr   = addr;
  t.base.length = 0;
  if (n > 0) {                       /* 无数据事务不能再带 USE_TXDATA，否则驱动报错 */
    t.base.flags |= SPI_TRANS_USE_TXDATA;
    t.base.length = (size_t)(n * 8);
    for (int i = 0; i < n && i < 4; i++) t.base.tx_data[i] = data[i];
  }
  spi_device_polling_transmit(s_dev, (spi_transaction_t *)&t);
}

void panelSetWindow(int16_t x, int16_t y, int16_t w, int16_t h) {
  uint8_t d[4];
  d[0] = (uint8_t)(x >> 8); d[1] = (uint8_t)x;
  d[2] = (uint8_t)((x + w - 1) >> 8); d[3] = (uint8_t)(x + w - 1);
  digitalWrite(LCD_CS, LOW);
  xferCmd((uint32_t)PQ_REG_CASET << 8, d, 4);
  d[0] = (uint8_t)(y >> 8); d[1] = (uint8_t)y;
  d[2] = (uint8_t)((y + h - 1) >> 8); d[3] = (uint8_t)(y + h - 1);
  xferCmd((uint32_t)PQ_REG_PASET << 8, d, 4);
  xferCmd((uint32_t)PQ_REG_RAMWR << 8, nullptr, 0);
  s_first = true;
  s_frameStartUs = micros();
}

/* 取回最早入队的一个事务（FIFO） */
static void reapOne() {
  spi_transaction_t *t = nullptr;
  spi_device_get_trans_result(s_dev, &t, portMAX_DELAY);
  if (s_queued > 0) s_queued--;
}

/* 填充事务的公共字段；first=true 时带上四线写显存命令 */
static void fillTrans(spi_transaction_ext_t *t, const void *buf, size_t bytesBytes) {
  memset(t, 0, sizeof(*t));
  if (s_first) {
    t->base.flags = SPI_TRANS_MODE_QIO;
    t->base.cmd   = PQ_CMD_QUADWR;
    t->base.addr  = PQ_ADDR_QUADWR;
    s_first = false;
  } else {
    t->base.flags   = SPI_TRANS_MODE_QIO | SPI_TRANS_VARIABLE_CMD |
                      SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_DUMMY;
    t->command_bits = 0;
    t->address_bits = 0;
    t->dummy_bits   = 0;
  }
  t->base.tx_buffer = buf;
  t->base.length    = bytesBytes * 8;
}

/* 把缓存里脏的像素行写回 PSRAM（DMA 绕过 CPU 缓存，必须显式回写） */
static void syncOut(const void *p, size_t bytes) {
#ifdef PQ_HAVE_MSYNC
  esp_cache_msync((void *)p, bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
#endif
}

void panelPush(const uint16_t *px, size_t n) {
  if (!s_dev || s_poolN < 2) return;
  uint32_t tPush0 = micros();
  s_fillUs = s_queueUs = 0;

  while (n) {
    size_t chunk = (n > PQ_CHUNK_PX) ? PQ_CHUNK_PX : n;

    /* 池满：等最早那块发完再复用。池越深，这里阻塞越少 */
    while (s_queued >= s_poolN) reapOne();

    int slot = s_qpos;
    s_qpos = (s_qpos + 1) % s_poolN;

    /* 帧缓冲在 CanvasGFX 里就存成大端（SPI 高位先出），这里只需整块拷进
     * 内部 RAM；逐像素 bswap 会让 PSRAM 随机读变慢 5 倍（实测 25ms→5ms）。 */
    uint16_t *dst = (uint16_t *)s_buf[slot];
    uint32_t tf = micros();
    memcpy(dst, px, chunk * 2);
    s_fillUs += micros() - tf;

    spi_transaction_ext_t *t = &s_tr[slot];
    fillTrans(t, dst, chunk * 2);
    uint32_t tq = micros();
    if (spi_device_queue_trans(s_dev, (spi_transaction_t *)t, 0) != ESP_OK) return;
    s_queueUs += micros() - tq;

    s_queued++;
    px += chunk;
    n  -= chunk;
  }
  s_statCopy++;
  s_pushUs = micros() - tPush0;
}

void panelFlush() {
  if (!s_dev) return;
  uint32_t tFlush0 = micros();
  while (s_queued > 0) reapOne();
  s_flushUs = micros() - tFlush0;
  if (s_frameStartUs) {
    s_transferUs = micros() - s_frameStartUs;
    s_frameStartUs = 0;
  }
  digitalWrite(LCD_CS, HIGH);
}
