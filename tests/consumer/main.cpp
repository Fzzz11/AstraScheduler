#include <astra/export.hpp>

// R-110 独立 consumer smoke：仅验证 public header 可编译、
// package target 可链接且可执行文件可运行；不调用任何库符号，
// 不依赖库的 public API（Phase 0 无 public 语义）。

int main() { return 0; }
