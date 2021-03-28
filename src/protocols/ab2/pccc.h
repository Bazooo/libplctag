/***************************************************************************
 *   Copyright (C) 2021 by Kyle Hayes                                      *
 *   Author Kyle Hayes  kyle.hayes@gmail.com                               *
 *                                                                         *
 * This software is available under either the Mozilla Public License      *
 * version 2.0 or the GNU LGPL version 2 (or later) license, whichever     *
 * you choose.                                                             *
 *                                                                         *
 * MPL 2.0:                                                                *
 *                                                                         *
 *   This Source Code Form is subject to the terms of the Mozilla Public   *
 *   License, v. 2.0. If a copy of the MPL was not distributed with this   *
 *   file, You can obtain one at http://mozilla.org/MPL/2.0/.              *
 *                                                                         *
 *                                                                         *
 * LGPL 2:                                                                 *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU Library General Public License as       *
 *   published by the Free Software Foundation; either version 2 of the    *
 *   License, or (at your option) any later version.                       *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this program; if not, write to the                 *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

 #pragma once

#include <stdint.h>
#include <lib/tag.h>
#include <ab2/ab.h>
#include <util/atomic_int.h>
#include <util/attr.h>
#include <util/plc.h>

typedef struct {
    struct plc_tag_t base_tag;

    uint16_t elem_size;
    uint16_t elem_count;

    /* data type info */
    df1_file_t data_file_type;
    int data_file_num;
    int data_file_elem;
    int data_file_sub_elem;

    /* plc and request info */
    plc_p plc;
    struct plc_request_s request;

    uint16_t tsn; /* transfer sequence number of the most recent request. */

    /* count of bytes sent or received. */
    uint16_t trans_offset;
} pccc_tag_t;
typedef pccc_tag_t *pccc_tag_p;

extern plc_tag_p pccc_tag_create(ab2_plc_type_t plc_type, attr attribs);
extern int pccc_get_int_attrib(plc_tag_p raw_tag, const char *attrib_name, int default_value);
extern int pccc_set_int_attrib(plc_tag_p raw_tag, const char *attrib_name, int new_value);
