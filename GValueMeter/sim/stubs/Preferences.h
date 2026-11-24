#pragma once
/* sim 用 Preferences 替身：固定槽位存键值（本机没有 C++ 标准库头，避免 STL） */
#include <stdint.h>
#include <string.h>
#include <stdio.h>

class Preferences {
 public:
  bool begin(const char *ns, bool ro = false) { (void)ro; snprintf(_ns, sizeof(_ns), "%s", ns); return true; }
  void end() {}

  uint8_t getUChar(const char *k, uint8_t def = 0) {
    int i = find(k);
    return i < 0 ? def : (uint8_t)_v[i];
  }
  void putUChar(const char *k, uint8_t v) { store(k, (float)v); }

  float getFloat(const char *k, float def = 0.f) {
    int i = find(k);
    return i < 0 ? def : _v[i];
  }
  void putFloat(const char *k, float v) { store(k, v); }

 private:
  int find(const char *k) {
    char full[40];
    snprintf(full, sizeof(full), "%s/%s", _ns, k);
    for (int i = 0; i < _n; i++) if (!strcmp(_k[i], full)) return i;
    return -1;
  }
  void store(const char *k, float v) {
    char full[40];
    snprintf(full, sizeof(full), "%s/%s", _ns, k);
    for (int i = 0; i < _n; i++) if (!strcmp(_k[i], full)) { _v[i] = v; return; }
    if (_n < 8) { snprintf(_k[_n], sizeof(_k[_n]), "%s", full); _v[_n] = v; _n++; }
  }

  char _ns[16] = "sim";
  int _n = 0;
  char _k[8][40];
  float _v[8] = {0};
};
