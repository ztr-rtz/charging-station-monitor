# 充电站环境检测网关 · i.MX6ULL(arm) 交叉编译
#
# 部署到板子（Cortex-A7 / Linux），只能使用 arm 交叉编译器：
#   make                                          (默认 arm-linux-gnueabihf-gcc)
#   make CC=arm-linux-gnueabihf-gcc               (显式指定)
# 提示：make CC=gcc 仅用于开发机排查语法错误，产物不可在 imx6ull 上运行。

CC       ?= arm-linux-gnueabihf-gcc
CFLAGS   := -std=c99 -Wall -Wextra -O2
LDFLAGS  := -lpthread -lsqlite3
SOURCES  := main.c uart.c modbus_env.c node_snapshot.c mbox.c data_process.c filter.c alert.c storage.c lvgl_ui.c
OBJ      := gateway_modbus_master

$(OBJ): $(SOURCES)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(OBJ)

.PHONY: all clean