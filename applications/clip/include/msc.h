/*
 * Copyright (c) 2025 Seeed Technology Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MSC_H
#define MSC_H

int msc_init(void);
int msc_enable(void);
int msc_disable(void);
bool msc_is_enabled(void);

#endif /* MSC_H */
