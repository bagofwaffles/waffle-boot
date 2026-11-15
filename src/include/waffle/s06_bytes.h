/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The S-06 names of the helpers in waffle/bytes.h, for the files that still
 * use them. New code uses waffle/bytes.h.
 */
#ifndef WAFFLE_S06_BYTES_H
#define WAFFLE_S06_BYTES_H

#include "waffle/bytes.h"

#define s06_copy    wb_copy
#define s06_fill    wb_fill
#define s06_equal   wb_equal
#define s06_rd_le16 wb_get_le16
#define s06_rd_be16 wb_get_be16
#define s06_rd_le32 wb_get_le32
#define s06_wr_le16 wb_put_le16
#define s06_wr_be16 wb_put_be16
#define s06_wr_le32 wb_put_le32
#define s06_wr_be32 wb_put_be32

#endif
