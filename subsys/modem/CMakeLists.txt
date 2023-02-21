# Copyright (c) 2023 Trackunit Corporation
# SPDX-License-Identifier: Apache-2.0

if(CONFIG_MODEM_MODULES)

zephyr_library()

zephyr_library_sources_ifdef(CONFIG_MODEM_BACKEND_TTY modem_backend_tty.c)
zephyr_library_sources_ifdef(CONFIG_MODEM_BACKEND_UART modem_backend_uart.c)
zephyr_library_sources_ifdef(CONFIG_MODEM_CHAT modem_chat.c)
zephyr_library_sources_ifdef(CONFIG_MODEM_CMUX modem_cmux.c)
zephyr_library_sources_ifdef(CONFIG_MODEM_PIPE modem_pipe.c)
zephyr_library_sources_ifdef(CONFIG_MODEM_PPP modem_ppp.c)

endif()
