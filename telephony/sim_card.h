/* Copyright (C) 2007-2008 The Android Open Source Project
**
** This software is licensed under the terms of the GNU General Public
** License version 2, as published by the Free Software Foundation, and
** may be copied, distributed, and modified under those terms.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
*/
#ifndef _android_sim_card_h
#define _android_sim_card_h

#include "gsm.h"
#include <stdbool.h>

#define  A_SIM_PIN_RETRIES 3
#define  A_SIM_PUK_RETRIES 6

typedef struct ASimCardRec_*    ASimCard;

extern ASimCard  asimcard_create( int base_port , int instance_id);
extern void      asimcard_destroy( ASimCard  sim );

typedef enum {
    A_SIM_STATUS_ABSENT = 0,
    A_SIM_STATUS_NOT_READY,
    A_SIM_STATUS_READY,
    A_SIM_STATUS_PIN,
    A_SIM_STATUS_PUK,
    A_SIM_STATUS_NETWORK_PERSONALIZATION
} ASimStatus;

extern ASimStatus  asimcard_get_status( ASimCard  sim );
extern void        asimcard_set_status( ASimCard  sim, ASimStatus  status );
extern void        asimcard_set_sim_power( ASimCard  sim, bool  enabled );

extern const char*  asimcard_get_pin( ASimCard  sim );
extern const char*  asimcard_get_puk( ASimCard  sim );
extern void         asimcard_set_pin( ASimCard  sim, const char*  pin );
extern void         asimcard_set_puk( ASimCard  sim, const char*  puk );

extern int         asimcard_check_pin( ASimCard  sim, const char*  pin );
extern int         asimcard_check_puk( ASimCard  sim, const char*  puk, const char*  pin );

extern int         asimcard_get_pin_retries( ASimCard  sim );
extern int         asimcard_get_puk_retries( ASimCard  sim );

extern bool        asimcard_get_pin_enabled( ASimCard  sim );
extern int         asimcard_set_pin_enabled( ASimCard  sim, bool  enabled, const char*  pin );

/* Restricted SIM Access command, as defined by 8.18 of 3GPP 27.007 */
typedef enum {
    A_SIM_CMD_READ_BINARY = 176,
    A_SIM_CMD_READ_RECORD = 178,
    A_SIM_CMD_GET_RESPONSE = 192,
    A_SIM_CMD_UPDATE_BINARY = 214,
    A_SIM_CMD_UPDATE_RECORD = 220,
    A_SIM_CMD_STATUS = 242
} ASimCommand;

extern const char*  asimcard_io( ASimCard  sim, const char*  cmd );

extern const char*  asimcard_stk_terminal_response( ASimCard  sim, const char*  cmd );

extern const char*  asimcard_stk_envelope_command( ASimCard  sim, const char*  cmd );

extern const char*  asimcard_get_last_stk_response( ASimCard  sim );

extern const char*  asimcard_get_last_stk_envelope( ASimCard  sim );

#endif /* _android_sim_card_h */
