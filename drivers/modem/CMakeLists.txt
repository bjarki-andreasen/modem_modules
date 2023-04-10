# Copyright (c) 2023 Bjarki Arge Andreasen
# SPDX-License-Identifier: Apache-2.0

zephyr_library()

zephyr_library_sources_ifdef(CONFIG_MODEM_CELLULAR modem_cellular.c)
