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
#include "android/utils/debug.h"
#include "qemu-common.h"
#include "sim_card.h"
#include <string.h>
#include <assert.h>
#include <stdio.h>

#define  DEBUG  0

#if DEBUG
#  define  D_ACTIVE  VERBOSE_CHECK(modem)
#  define  D(...)   do { if (D_ACTIVE) fprintf( stderr, __VA_ARGS__ ); } while (0)
#else
#  define  D(...)   ((void)0)
#endif

#define  A_SIM_PIN_SIZE  4
#define  A_SIM_PUK_SIZE  8

#define  SIM_FILE_RECORD_ABSOLUTE_MODE  4

// @see TS 102.221 section 10.2.1 Status conditions returned by the UICC.
/* Normal processing */
// Normal ending of the command - sw1='90', sw2='00'.
#define  SIM_RESPONSE_NORMAL_ENDING         "+CRSM: 144,0"

/* Execution errors */
// sw1='64', sw2='00', No information given, state of non-volatile memory unchanged.
#define  SIM_RESPONSE_EXECUTION_ERROR       "+CRSM: 100,0"

/* Checking errors */
// sw1='67', sw2='00', Wrong length.
#define  SIM_RESPONSE_WRONG_LENGTH          "+CRSM: 103,0"

/* Wrong parameters */
// sw1='6A', sw2='81', Function not supported.
#define  SIM_RESPONSE_FUNCTION_NOT_SUPPORT  "+CRSM: 106,129"
// sw1='6A', sw2='82', File not found.
#define  SIM_RESPONSE_FILE_NOT_FOUND        "+CRSM: 106,130"
// sw1='6A', sw2='83', Record not found
#define  SIM_RESPONSE_RECORD_NOT_FOUND      "+CRSM: 106,131"
// sw1='6A', sw2='86', Incorrect parameters P1 to P2.
#define  SIM_RESPONSE_INCORRECT_PARAMETERS  "+CRSM: 106,134"

/* Command not allowed */
// sw1='69', sw2='85', Conditions of use not satisfied.
#define  SIM_RESPONSE_CONDITION_NOT_SATISFIED "+CRSM: 105,133"

typedef union SimFileRec_ SimFileRec, *SimFile;

typedef struct ASimCardRec_ {
    ASimStatus  status;
    bool        pin_enabled;
    char        pin[ A_SIM_PIN_SIZE+1 ];
    char        puk[ A_SIM_PUK_SIZE+1 ];
    int         pin_retries;
    int         puk_retries;
    int         port;
    int         instance_id;

    char        out_buff[ 256 ];
    int         out_size;

    char        last_stk_response[ 1024 ];
    char        last_stk_envelope[ 1024 ];

    SimFile     efs;

} ASimCardRec;

static int asimcard_ef_init( ASimCard sim );
static void asimcard_ef_remove( ASimCard sim, SimFile ef );
static ASimCardRec  _s_card[MAX_GSM_DEVICES];

ASimCard
asimcard_create(int port, int instance_id)
{
    ASimCard  card    = &_s_card[instance_id];
    card->status      = A_SIM_STATUS_READY;
    card->pin_retries = 0;
    card->puk_retries = 0;
    strncpy( card->pin, "0000", sizeof(card->pin) );
    strncpy( card->puk, "12345678", sizeof(card->puk) );
    card->pin_enabled = false;
    card->port = port;
    card->instance_id = instance_id;
    asimcard_ef_init(card);
    return card;
}

void
asimcard_destroy( ASimCard  card )
{
    /* remove and free all EFs */
    SimFile ef = card->efs;
    for(; ef != NULL; ef = card->efs) {
        asimcard_ef_remove(card, ef);
    }

    /* nothing really */
    card=card;
}

static __inline__ int
asimcard_ready( ASimCard  card )
{
    return card->status == A_SIM_STATUS_READY;
}

ASimStatus
asimcard_get_status( ASimCard  sim )
{
    return sim->status;
}

void
asimcard_set_status( ASimCard  sim, ASimStatus  status )
{
    sim->status = status;
}

void
asimcard_set_sim_power( ASimCard  sim, bool  enabled )
{
    if (enabled) {
        if (sim->status != A_SIM_STATUS_ABSENT) {
            // Do nothing if sim is already powered on.
            return;
        }

        sim->status = sim->pin_enabled ? A_SIM_STATUS_PIN
                                       : A_SIM_STATUS_READY;
    } else {
        sim->status = A_SIM_STATUS_ABSENT;
    }
}

const char*
asimcard_get_pin( ASimCard  sim )
{
    return sim->pin;
}

const char*
asimcard_get_puk( ASimCard  sim )
{
    return sim->puk;
}

void
asimcard_set_pin( ASimCard  sim, const char*  pin )
{
    strncpy( sim->pin, pin, A_SIM_PIN_SIZE );
}

void
asimcard_set_puk( ASimCard  sim, const char*  puk )
{
    strncpy( sim->puk, puk, A_SIM_PUK_SIZE );
}


int
asimcard_check_pin( ASimCard  sim, const char*  pin )
{
    if (sim->status != A_SIM_STATUS_PIN   &&
        sim->status != A_SIM_STATUS_READY )
        return 0;

    if ( !strcmp( sim->pin, pin ) ) {
        sim->status      = A_SIM_STATUS_READY;
        sim->pin_retries = 0;
        return 1;
    }

    if (++sim->pin_retries == A_SIM_PIN_RETRIES) {
        if (sim->status != A_SIM_STATUS_READY) {
            sim->status = A_SIM_STATUS_PUK;
        }
    }
    return 0;
}


int
asimcard_check_puk( ASimCard  sim, const char* puk, const char*  pin )
{
    if (sim->status != A_SIM_STATUS_PUK)
        return 0;

    if ( !strcmp( sim->puk, puk ) ) {
        strncpy( sim->puk, puk, A_SIM_PUK_SIZE );
        strncpy( sim->pin, pin, A_SIM_PIN_SIZE );
        sim->status      = A_SIM_STATUS_READY;
        sim->puk_retries = 0;
        sim->pin_retries = 0;
        return 1;
    }

    if ( ++sim->puk_retries == A_SIM_PUK_RETRIES ) {
        sim->status = A_SIM_STATUS_ABSENT;
    }
    return 0;
}

int
asimcard_get_pin_retries( ASimCard sim )
{
    return A_SIM_PIN_RETRIES - sim->pin_retries;
}

int
asimcard_get_puk_retries( ASimCard sim )
{
    return A_SIM_PUK_RETRIES - sim->puk_retries;
}

bool
asimcard_get_pin_enabled( ASimCard sim )
{
    return sim->pin_enabled;
}

int
asimcard_set_pin_enabled( ASimCard sim, bool enabled, const char* pin )
{
    if (asimcard_check_pin(sim, pin)) {
        sim->pin_enabled = enabled;
        return 1;
    }
    return 0;
}

typedef enum {
    SIM_FILE_DM = 0,
    SIM_FILE_DF,
    SIM_FILE_EF_DEDICATED,
    SIM_FILE_EF_LINEAR,
    SIM_FILE_EF_CYCLIC
} SimFileType;

typedef enum {
    SIM_FILE_READ_ONLY       = (1 << 0),
    SIM_FILE_NEED_PIN = (1 << 1),
} SimFileFlags;

/* descriptor for a known SIM File */
#define  SIM_FILE_HEAD       \
    SimFileRec*     next;    \
    SimFileRec**    prev;    \
    SimFileType     type;    \
    unsigned short  id;      \
    unsigned short  flags;

typedef struct {
    SIM_FILE_HEAD
} SimFileAnyRec, *SimFileAny;

typedef struct {
    SIM_FILE_HEAD
    cbytes_t   data;
    int        length;
} SimFileEFDedicatedRec, *SimFileEFDedicated;

typedef struct {
    SIM_FILE_HEAD
    byte_t     rec_count;
    byte_t     rec_len;
    cbytes_t   records;
} SimFileEFLinearRec, *SimFileEFLinear;

typedef SimFileEFLinearRec   SimFileEFCyclicRec;
typedef SimFileEFCyclicRec*  SimFileEFCyclic;

union SimFileRec_ {
    SimFileAnyRec          any;
    SimFileEFDedicatedRec  dedicated;
    SimFileEFLinearRec     linear;
    SimFileEFCyclicRec     cyclic;
};

static int
asimcard_ef_add( ASimCard sim, SimFile ef )
{
    SimFile first = sim->efs;

    sim->efs     = ef;
    ef->any.next = first;
    ef->any.prev = &(sim->efs);

    if (first) {
        first->any.prev = &(ef->any.next);
    }

    return 0;
}

static SimFile
asimcard_ef_new_dedicated( unsigned short id, unsigned short flags )
{
    SimFileEFDedicatedRec* dedicated = (SimFileEFDedicatedRec*) calloc(1, sizeof(SimFileEFDedicatedRec));

    if (dedicated == NULL) {
        return NULL;
    }

    dedicated->type   = SIM_FILE_EF_DEDICATED;
    dedicated->id     = id;
    dedicated->flags  = flags;
    dedicated->length = 0;
    dedicated->data   = NULL;

    return (SimFile) dedicated;
}

static SimFile
asimcard_ef_new_linear( unsigned short id, unsigned short flags, byte_t rec_len )
{
    SimFileEFLinearRec* linear= (SimFileEFLinearRec*) calloc(1, sizeof(SimFileEFLinearRec));

    if (linear == NULL) {
        return NULL;
    }

    linear->type      = SIM_FILE_EF_LINEAR;
    linear->id        = id;
    linear->flags     = flags;
    linear->rec_len   = rec_len;
    linear->rec_count = 0;
    linear->records   = NULL;

    return (SimFile) linear;
}

static void
asimcard_ef_free( SimFile ef )
{
    if (ef == NULL) {
        return;
    }

    if (ef->any.type == SIM_FILE_EF_LINEAR) {
        if (ef->linear.records) {
            free(ef->linear.records);
        }
    } else if (ef->any.type == SIM_FILE_EF_DEDICATED) {
        if (ef->dedicated.data) {
            free(ef->dedicated.data);
        }
    }

    free(ef);
}

static void
asimcard_ef_remove( ASimCard sim, SimFile ef )
{
    if (ef == NULL) {
        return;
    }

    SimFile next = ef->any.next;
    if (next) {
        next->any.prev = ef->any.prev;
    }

    ef->any.prev[0] = ef->any.next;
    ef->any.next    = NULL;
    ef->any.prev    = &(ef->any.next);

    asimcard_ef_free(ef);
}

static int
asimcard_ef_update_dedicated( SimFile ef, char* data )
{
    if (data == NULL || (strlen(data) % 2) == 1) {
        D("ERROR: Length of data should be even.\n");
        return -1;
    }

    if (ef == NULL || ef->any.type != SIM_FILE_EF_DEDICATED) {
        D("ERROR: The type of EF is not SIM_FILE_EF_DEDICATED.\n");
        return -1;
    }

    SimFileEFDedicated dedicated = (SimFileEFDedicated) ef;

    if (dedicated->length != (strlen(data) / 2)) {
        if (dedicated->data) {
            free(dedicated->data);
        }

        dedicated->length = strlen(data) / 2;
        dedicated->data   = (cbytes_t) malloc(dedicated->length * sizeof(byte_t));
        memset(dedicated->data, 0xff, dedicated->length * sizeof(byte_t));
    }

    return gsm_hex_to_bytes((cbytes_t) data, strlen(data), dedicated->data);
}

static int
asimcard_ef_update_linear( SimFile ef, byte_t record_id, char* data )
{
    if (data == NULL || (strlen(data) % 2) == 1) {
        D("ERROR: Length of data should be even.\n");
        return -1;
    }

    if (record_id < 0x00 || record_id > 0xff) {
        D("ERROR: Invaild record id.\n");
        return -1;
    }

    if (ef == NULL || ef->any.type != SIM_FILE_EF_LINEAR) {
        D("ERROR: The type of EF is not SIM_FILE_EF_LINEAR.\n");
        return -1;
    }

    if (ef->linear.rec_len < (strlen(data) / 2)) {
        D("ERROR: Data is too large.\n");
        return -1;
    }

    SimFileEFLinear linear = (SimFileEFLinear) ef;

    if (linear->rec_count < record_id) {
        int      file_size     = linear->rec_len * record_id;
        int      rec_count_old = linear->rec_count;
        cbytes_t records_old   = linear->records;

        linear->rec_count = record_id;
        linear->records   = (cbytes_t) malloc(file_size * sizeof(byte_t));
        memset(linear->records, 0xff, file_size * sizeof(byte_t));

        if (records_old) {
            int size = linear->rec_len * rec_count_old;
            memcpy(linear->records, records_old, size * sizeof(byte_t));
            free(records_old);
        }
    }

    bytes_t record = linear->records + ((record_id - 1) * linear->rec_len);

    return gsm_hex_to_bytes((cbytes_t) data, strlen(data), record);
}

static int
asimcard_ef_read_dedicated( SimFile ef, char* dst )
{
    if (dst == NULL) {
        D("ERROR: Destination buffer is NULL.\n");
        return -1;
    }

    if (ef == NULL || ef->any.type != SIM_FILE_EF_DEDICATED) {
        D("ERROR: The type of EF is not SIM_FILE_EF_DEDICATED.\n");
        return -1;
    }

    SimFileEFDedicated dedicated = (SimFileEFDedicated) ef;

    gsm_hex_from_bytes(dst, dedicated->data, dedicated->length);
    dst[dedicated->length * 2] = '\0';

    return dedicated->length;
}

static int
asimcard_ef_read_linear( SimFile ef, byte_t record_id, char* dst )
{
    if (dst == NULL) {
        D("ERROR: Destination buffer is NULL.\n");
        return -1;
    }

    if (ef == NULL || ef->any.type != SIM_FILE_EF_LINEAR) {
        D("ERROR: The type of EF is not SIM_FILE_EF_LINEAR.\n");
        return -1;
    }

    if (ef->linear.rec_count < record_id) {
        D("ERROR: Invaild record id.\n");
        return -1;
    }

    SimFileEFLinear linear = (SimFileEFLinear) ef;
    bytes_t         record = linear->records + ((record_id - 1) * linear->rec_len);

    gsm_hex_from_bytes(dst, record, linear->rec_len);
    dst[linear->rec_len * 2] = '\0';

    return linear->rec_len;
}

static SimFile
asimcard_ef_find( ASimCard sim, int id )
{
    SimFile ef = sim->efs;

    for(; ef != NULL; ef = ef->any.next) {
        if (ef->any.id == id) {
            break;
        }
    }

    return ef;
}

static const char*
asimcard_io_read_binary( ASimCard sim, int id, int p1, int p2, int p3 )
{
    char*   out = sim->out_buff;
    SimFile ef  = asimcard_ef_find(sim, id);

    if (ef == NULL) {
        return SIM_RESPONSE_FILE_NOT_FOUND;
    }

    if (p1 != 0 || p2 != 0) {
        return SIM_RESPONSE_INCORRECT_PARAMETERS;
    }

    if (ef->any.type != SIM_FILE_EF_DEDICATED) {
        return SIM_RESPONSE_FUNCTION_NOT_SUPPORT;
    }

    if (ef->dedicated.length < p3) {
        return SIM_RESPONSE_WRONG_LENGTH;
    }

    sprintf(out, "%s,", SIM_RESPONSE_NORMAL_ENDING);
    out  += strlen(out);
    if (asimcard_ef_read_dedicated(ef, out) < 0) {
        return SIM_RESPONSE_EXECUTION_ERROR;
    }

    return sim->out_buff;
}

static const char*
asimcard_io_read_record( ASimCard sim, int id, int p1, int p2, int p3 )
{
    char*   out = sim->out_buff;
    SimFile ef  = asimcard_ef_find(sim, id);

    if (ef == NULL) {
        return SIM_RESPONSE_FILE_NOT_FOUND;
    }

    // We only support ABSOLUTE_MODE
    if (p2 != SIM_FILE_RECORD_ABSOLUTE_MODE || p1 <= 0) {
        return SIM_RESPONSE_INCORRECT_PARAMETERS;
    }

    if (ef->any.type != SIM_FILE_EF_LINEAR) {
        return SIM_RESPONSE_FUNCTION_NOT_SUPPORT;
    }

    if (ef->linear.rec_count < p1) {
        return SIM_RESPONSE_RECORD_NOT_FOUND;
    }

    if (ef->linear.rec_len < p3) {
        return SIM_RESPONSE_WRONG_LENGTH;
    }

    sprintf(out, "%s,", SIM_RESPONSE_NORMAL_ENDING);
    out += strlen(out);
    if (asimcard_ef_read_linear(ef, p1, out) < 0) {
        return SIM_RESPONSE_EXECUTION_ERROR;
    }

    return sim->out_buff;
}

static const char*
asimcard_io_update_record( ASimCard sim, int id, int p1, int p2, int p3, char* data )
{
    char* out = sim->out_buff;
    SimFile ef  = asimcard_ef_find(sim, id);

    if (ef == NULL) {
        return SIM_RESPONSE_FILE_NOT_FOUND;
    }

    // We only support ABSOLUTE_MODE
    if (p2 != SIM_FILE_RECORD_ABSOLUTE_MODE || p1 <= 0) {
        return SIM_RESPONSE_INCORRECT_PARAMETERS;
    }

    if (ef->any.flags & SIM_FILE_READ_ONLY) {
        return SIM_RESPONSE_CONDITION_NOT_SATISFIED;
    }

    if (ef->linear.rec_len < p3) {
        return SIM_RESPONSE_WRONG_LENGTH;
    }

    if (ef->linear.rec_count < p1) {
        return SIM_RESPONSE_RECORD_NOT_FOUND;
    }

    if (asimcard_ef_update_linear(ef, p1, data) < 0) {
        return SIM_RESPONSE_EXECUTION_ERROR;
    }

    return SIM_RESPONSE_NORMAL_ENDING;
}

static int
asimcard_ef_init( ASimCard card )
{
    char buff[128] = {'\0'};
    SimFile ef;

    // CPHS Network Operator Name(6F14):
    //   File size: 0x14
    //   PLMN Name: "Android"
    // @see Common PCN Handset Specification (Version 4.2) B.4.1.2 Network Operator Name
    ef = asimcard_ef_new_dedicated(0x6f14, SIM_FILE_READ_ONLY | SIM_FILE_NEED_PIN);
    asimcard_ef_update_dedicated(ef, "416e64726f6964ffffffffffffffffffffffffff");
    asimcard_ef_add(card, ef);

    // CPHS Voice message waiting flag(6F11):
    //   File size: 0x01
    //   Voice Message Waiting Indicator flags:
    //     Line 1: no messages waiting.
    //     Line 2: no messages waiting.
    // @see Common PCN Handset Specification (Version 4.2) B.4.2.3 Voice Message Waiting Flags in the SIM
    ef = asimcard_ef_new_dedicated(0x6f11, SIM_FILE_NEED_PIN);
    asimcard_ef_update_dedicated(ef, "55");
    asimcard_ef_add(card, ef);

    // ICC Identification(2FE2):
    //   File size: 0x0a
    //   Identification number: "8901410321111851072" + (sim->instance_id)
    //                          e.g. "89014103211118510720" for first sim.
    // @see 3GPP TS 11.011 section 10.1.1 EFiccid (ICC Identification)
    ef = asimcard_ef_new_dedicated(0x2fe2, SIM_FILE_READ_ONLY);
    snprintf(buff, sizeof(buff), "981014301211811570%d2", card->instance_id);
    asimcard_ef_update_dedicated(ef, buff);
    asimcard_ef_add(card, ef);

    // CPHS Call forwarding flags(6F13):
    //   File size: 0x01
    //   Voice Call forward unconditional flags:
    //     Line 1: no call forwarding message waiting.
    //     Line 2: no call forwarding message waiting.
    // @see Common PCN Handset Specification (Version 4.2) B.4.5 Diverted Call Status Indicator
    ef = asimcard_ef_new_dedicated(0x6f13, SIM_FILE_NEED_PIN);
    asimcard_ef_update_dedicated(ef, "55");
    asimcard_ef_add(card, ef);

    // SIM Service Table(6F38):
    //   File size: 0x0f
    //   Enabled: 1..4, 7, 9..19, 25..27, 29, 30, 38, 39, 51..56
    // @see 3GPP TS 51.011 section 10.3.7 EFsst (SIM Service Table)
    ef = asimcard_ef_new_dedicated(0x6f38, SIM_FILE_READ_ONLY | SIM_FILE_NEED_PIN);
    asimcard_ef_update_dedicated(ef, "ff30ffff3f003f0f003c0000f0ff00");
    asimcard_ef_add(card, ef);

    // Mailbox Identifier(6FC9):
    //   Record size: 0x04
    //   Record count: 0x02
    //   Mailbox Dialing Number Identifier - Voicemail:      1
    //   Mailbox Dialing Number Identifier - Fax:            no mailbox dialing number associated
    //   Mailbox Dialing Number Identifier - Eletronic Mail: no mailbox dialing number associated
    //   Mailbox Dialing Number Identifier - Other:          no mailbox dialing number associated
    //   Mailbox Dialing Number Identifier - Videomail:      no mailbox dialing number associated
    // @see 3GPP TS 31.102 section 4.2.62 EFmbi (Mailbox Identifier)
    ef = asimcard_ef_new_linear(0x6fc9, SIM_FILE_NEED_PIN, 0x04);
    asimcard_ef_update_linear(ef, 0x01, "01000000");
    asimcard_ef_update_linear(ef, 0x02, "ffffffff");
    asimcard_ef_add(card, ef);

    // Message Waiting Indication Status(6FCA):
    //   Record size: 0x05
    //   Record count: 0x02
    //   Message Waiting Indicator Status: all inactive
    //   Number of Voicemail Messages Waiting:       0
    //   Number of Fax Messages Waiting:             0
    //   Number of Electronic Mail Messages Waiting: 0
    //   Number of Other Messages Waiting:           0
    //   Number of Videomail Messages Waiting:       0
    // @see 3GPP TS 31.102 section 4.2.63 EFmwis (Message Waiting Indication Status)
    ef = asimcard_ef_new_linear(0x6fca, SIM_FILE_NEED_PIN, 0x05);
    asimcard_ef_update_linear(ef, 0x01, "0000000000");
    asimcard_ef_update_linear(ef, 0x02, "ffffffffff");
    asimcard_ef_add(card, ef);

    // Administrative Data(6FAD):
    //   File size: 0x04
    //   UE Operation mode: normal
    //   Additional information: none
    //   Length of MNC in the IMSI: 3
    // @see 3GPP TS 31.102 section 4.2.18 EFad (Administrative Data)
    ef = asimcard_ef_new_dedicated(0x6fad, SIM_FILE_READ_ONLY);
    asimcard_ef_update_dedicated(ef, "00000003");
    asimcard_ef_add(card, ef);

    // Image Instance Data Files(4FXX):
    // @see 3GPP TS 51.011 section 10.6.1.2, Image Instance Data Files
    // @see 3GPP TS 31.102 section 4.6.1.2, EFiidf (Image Instance Data Files)
    //   File size: 0x1f
    //   Image Instance Data (4f02):
    //     Image width:                 08
    //     Image length:                08
    //     Bits per raster image point: 02
    //     Number of CLUT entries:      03
    //     Location of CLUT:            0016
    //     Image body:                  see below
    ef = asimcard_ef_new_dedicated(0x4f02, SIM_FILE_READ_ONLY | SIM_FILE_NEED_PIN);
    asimcard_ef_update_dedicated(ef, "080802030016AAAA800285428142814281528002AAAAFF000000FF000000FF");
    asimcard_ef_add(card, ef);
    //   File size: 0x0a
    //   Image Instance Data (4f04):
    //     Image width:                 08
    //     Image length:                08
    //     Image body:                  see below
    ef = asimcard_ef_new_dedicated(0x4f04, SIM_FILE_READ_ONLY | SIM_FILE_NEED_PIN);
    asimcard_ef_update_dedicated(ef, "0808FF03A59999A5C3FF");
    asimcard_ef_add(card, ef);

    // EF-IMG (4F20) : Each record of this EF identifies instances of one particular graphical image,
    //                 which graphical image is identified by this EF's record number.
    //   Record size: 0x14
    // @see 3GPP TS 51.011 section 10.6.1.1, EF-img
    ef = asimcard_ef_new_linear(0x4f20, SIM_FILE_READ_ONLY | SIM_FILE_NEED_PIN, 0x14);
    //   Record number: 0x01
    //   Number of image instance specified by this record:                 01
    //   Image instance width (raster image points):                        08
    //   Image instance heigh (raster image points):                        08
    //   Color image coding scheme:                                         11
    //   Image identifier id of the EF where is store the image instance:   4F04
    //   Offset of the image instance in the 4F04 EF:                       0000
    //   Length of image instance data:                                     000A
    asimcard_ef_update_linear(ef, 0x01, "010808114f040000000Affffffffffffffffffff");
    //   Record number: 0x03
    //   Number of image instance specified by this record:                 01
    //   Image instance width (raster image points):                        08
    //   Image instance heigh (raster image points):                        08
    //   Color image coding scheme:                                         21
    //   Image identifier id of the EF where is store the image instance:   4F02
    //   Offset of the image instance in the 4F02 EF:                       0000
    //   Length of image instance data:                                     0016
    asimcard_ef_update_linear(ef, 0x03, "010808214f0200000016ffffffffffffffffffff");
    //   Record number: 0x05
    //   Number of image instance specified by this record:                 01
    //   Image instance width (raster image points):                        08
    //   Image instance heigh (raster image points):                        08
    //   Color image coding scheme:                                         22
    //   Image identifier id of the EF where is store the image instance:   4F02
    //   Offset of the image instance in the 4F02 EF:                       0000
    //   Length of image instance data:                                     0016
    asimcard_ef_update_linear(ef, 0x05, "010808224f0200000016ffffffffffffffffffff");
    //   Record number: 0x07
    //   Number of image instance specified by this record:                 02
    //   Descriptor of image instace 1:
    //     Image instance width (raster image points):                      08
    //     Image instance heigh (raster image points):                      08
    //     Color image coding scheme:                                       21
    //     Image identifier id of the EF where is store the image instance: 4F02
    //     Offset of the image instance in the 4F02 EF:                     0000
    //     Length of image instance data:                                   0016
    //   Descriptor of image instace 2:
    //     Image instance width (raster image points):                      08
    //     Image instance heigh (raster image points):                      08
    //     Color image coding scheme:                                       22
    //     Image identifier id of the EF where is store the image instance: 4F02
    //     Offset of the image instance in the 4F02 EF:                     0000
    //     Length of image instance data:                                   0016
    asimcard_ef_update_linear(ef, 0x07, "020808214f02000000160808224f0200000016ff");
    asimcard_ef_add(card, ef);

    // CPHS Information(6F16):
    //   File size: 0x02
    //   CPHS Phase: 2
    //   CPHS Service Table:
    //     CSP(Customer Service Profile): allocated and activated
    //     Information Numbers:           allocated and activated
    // @see Common PCN Handset Specification (Version 4.2) B.3.1.1 CPHS Information
    ef = asimcard_ef_new_dedicated(0x6f16, SIM_FILE_READ_ONLY | SIM_FILE_NEED_PIN);
    asimcard_ef_update_dedicated(ef, "0233");
    asimcard_ef_add(card, ef);

    // Service Provider Name(6F46):
    //   File size: 0x11
    //   Display Condition: 0x1, display network name in HPLMN; display SPN if not in HPLMN.
    //   Service Provider Name: "Android"
    // @see 3GPP TS 31.102 section 4.2.12 EFspn (Service Provider Name)
    // @see 3GPP TS 51.011 section 9.4.4 Referencing Management
    ef = asimcard_ef_new_dedicated(0x6f46, SIM_FILE_READ_ONLY);
    asimcard_ef_update_dedicated(ef, "01416e64726f6964ffffffffffffffffff");
    asimcard_ef_add(card, ef);

    // Service Provider Display Information(6FCD):
    //   File size: 0x0d
    //   SPDI TLV (tag = 'a3')
    //     SPDI TLV (tag = '80')
    //       PLMN: 234136
    //       PLMN: 46692
    // @see 3GPP TS 31.102 section 4.2.66 EFspdi (Service Provider Display Information)
    // @see 3GPP TS 51.011 section 9.4.4 Referencing Management
    ef = asimcard_ef_new_dedicated(0x6fcd, SIM_FILE_READ_ONLY);
    asimcard_ef_update_dedicated(ef, "a30b800932643164269fffffff");
    asimcard_ef_add(card, ef);

    // PLMN Network Name(6FC5):
    //   Record size: 0x18
    //   Record count: 0x0a
    //   Record:
    //     PNN 1: Full name: "Test1", Short name: "Test1"
    //     PNN 2: Full name: "Test2", Short name: (none)
    //     PNN 3: Full name: "Test3", Short name: (none)
    //     PNN 4: Full name: "Test4", Short name: (none)
    //     PNN 5: Full name: "Test5", Short name: (none)
    //     PNN 6: Full name: "Test6", Short name: (none)
    //     PNN 7: Full name: "Test7", Short name: (none)
    // @see 3GPP TS 31.102 section 4.2.58 EFpnn (PLMN Network Name)
    // @see 3GPP TS 24.008
    ef = asimcard_ef_new_linear(0x6fc5, SIM_FILE_READ_ONLY, 0x18);
    // Long name: 43mn8x..., where <mn> is tlvLen, x is trailing spare bits.
    // Short name: 45mn8x..., where <mn> is tlvLen, x is trailing spare bits.
    // 'T'=b1010100, 'e'=b1100101, 's'=b1110011, 't'=b1110100, '1'=b0110001,
    // "Test1"=11010100, 11110010, 10011100, 00011110, 00000011=0xd4f29c1e03 with 5 trailing bits.
    asimcard_ef_update_linear(ef, 0x01, "430685d4f29c1e03450685d4f29c1e03ffffffffffffffff");
    asimcard_ef_update_linear(ef, 0x02, "430685d4f29c2e03ffffffffffffffffffffffffffffffff");
    asimcard_ef_update_linear(ef, 0x03, "430685d4f29c3e03ffffffffffffffffffffffffffffffff");
    asimcard_ef_update_linear(ef, 0x04, "430685d4f29c4e03ffffffffffffffffffffffffffffffff");
    asimcard_ef_update_linear(ef, 0x05, "430685d4f29c5e03ffffffffffffffffffffffffffffffff");
    asimcard_ef_update_linear(ef, 0x06, "430685d4f29c6e03ffffffffffffffffffffffffffffffff");
    asimcard_ef_update_linear(ef, 0x07, "430685d4f29c7e03ffffffffffffffffffffffffffffffff");
    asimcard_ef_update_linear(ef, 0x0a, "ffffffffffffffffffffffffffffffffffffffffffffffff");
    asimcard_ef_add(card, ef);

    // Operator PLMN List (6FC6):
    //   Record size: 0x18
    //   Record count: 0x0a
    //   Record:
    //     MCC = 001, MNC =  01, START=0000, END=FFFE, PNN = 01,
    //     MCC = 001, MNC =  02, START=0001, END=0010, PNN = 02,
    //     MCC = 001, MNC =  03, START=0011, END=0011, PNN = 03,
    //     MCC = 001, MNC = 001, START=0012, END=0012, PNN = 04,
    //     MCC = 001, MNC =  1D, START=0000, END=FFFE, PNN = 05,
    //     MCC = 001, MNC = 2DD, START=0000, END=FFFE, PNN = 06,
    //     MCC = 001, MNC = DDD, START=0000, END=FFFE, PNN = 07,
    // @see 3GPP TS 31.102 section 4.2.59 EFopl (Operator PLMN List)
    // @see 3GPP TS 24.008
    // @see http://en.wikipedia.org/wiki/Mobile_country_code
    ef = asimcard_ef_new_linear(0x6fc6, SIM_FILE_READ_ONLY, 0x18);
    // If mnc=012, mcc=345, lac/tac start=abcd, end=wxyz, PLMN record id=mn, then:
    //                                  "105243abcdwxyzmnffffffffffffffffffffffffffffffff"
    asimcard_ef_update_linear(ef, 0x01, "00110f0000fffe01ffffffffffffffffffffffffffffffff");
    asimcard_ef_update_linear(ef, 0x02, "00210f0001001002ffffffffffffffffffffffffffffffff");
    asimcard_ef_update_linear(ef, 0x03, "00310f0011001103ffffffffffffffffffffffffffffffff");
    asimcard_ef_update_linear(ef, 0x04, "0011000012001204ffffffffffffffffffffffffffffffff");
    asimcard_ef_update_linear(ef, 0x05, "00d11f0000fffe05ffffffffffffffffffffffffffffffff");
    asimcard_ef_update_linear(ef, 0x06, "00d1d20000fffe06ffffffffffffffffffffffffffffffff");
    asimcard_ef_update_linear(ef, 0x07, "00d1dd0000fffe07ffffffffffffffffffffffffffffffff");
    asimcard_ef_update_linear(ef, 0x0a, "ffffffffffffffffffffffffffffffffffffffffffffffff");
    asimcard_ef_add(card, ef);

    // MSISDN(6F40):
    //   Record size: 0x20
    //   Record count: 0x04
    //   Alpha Identifier: (empty)
    //   Length of BCD number/SSC contents: 7
    //   TON and NPI: 0x81
    //   Dialing Number/SSC String: 15555218135, actual number is "155552"
    //                              + (sim->instance_id + 1) + emulator port,
    //                              e.g. "15555215554" for first sim of first emulator.
    //   Capacity/Configuration 2 Record Identifier: not used
    //   Extension 5 Record Identifier: not used
    // @see 3GPP TS 31.102 section 4.2.26 EFmsisdn (MSISDN)
    ef = asimcard_ef_new_linear(0x6f40, SIM_FILE_NEED_PIN, 0x20);
    snprintf(buff, sizeof(buff), "ffffffffffffffffffffffffffffffffffff0781515525%d%d%d%df%dffffffffffff",
        (card->port / 1000) % 10,
        (card->instance_id + 1),
        (card->port / 10) % 10,
        (card->port / 100) % 10,
        card->port % 10);
    asimcard_ef_update_linear(ef, 0x01, buff);
    asimcard_ef_update_linear(ef, 0x04, "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
    asimcard_ef_add(card, ef);

    // Mailbox Dialing Numbers(6FC7):
    //   Record size: 0x20
    //   Record count: 0x02
    //   Alpha Identifier: "Voicemail"
    //   Length of BCD number/SSC contents: 7
    //   TON and NPI: 0x91
    //   Dialing Number/SSC String: 15552175049
    //   Capacity/Configuration 2 Record Identifier: not used
    //   Extension 6 Record Identifier: not used
    // @see 3GPP TS 31.102 section 4.2.60 EFmbdn (Mailbox Dialing Numbers)
    ef = asimcard_ef_new_linear(0x6fc7, SIM_FILE_NEED_PIN, 0x20);
    asimcard_ef_update_linear(ef, 0x01, "566f6963656d61696cffffffffffffffffff07915155125740f9ffffffffffff");
    asimcard_ef_update_linear(ef, 0x02, "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
    asimcard_ef_add(card, ef);

    // Abbreviated Dialling Numbers(6F3A)
    //   Record size: 0x20
    //   Record count: 0xff
    //   Length of BCD number/SSC contents: 7
    //   TON and NPI: 0x81
    // @see 3GPP TS 51.011 section 10.5.1 EFadn
    ef = asimcard_ef_new_linear(0x6f3a, SIM_FILE_NEED_PIN, 0x20);
    // Alpha Id(Encoded with GSM 8 bit): "Mozilla", Dialling Number: 15555218201
    asimcard_ef_update_linear(ef, 0x01, "4d6f7a696c6c61ffffffffffffffffffffff07815155258102f1ffffffffffff");
    // Alpha Id(Encoded with UCS2 0x80: "Saßê\u9ec3", Dialling Number: 15555218202
    asimcard_ef_update_linear(ef, 0x02, "800053006100df00ea9ec3ffffffffffffff07815155258102f2ffffffffffff");
    // Alpha Id(Encoded with UCS2 0x81): "Fire \u706b", Dialling Number: 15555218203
    asimcard_ef_update_linear(ef, 0x03, "8106e04669726520ebffffffffffffffffff07815155258102f3ffffffffffff");
    // Alpha Id(Encoded with UCS2 0x82): "Huang \u9ec3", Dialling Number: 15555218204
    asimcard_ef_update_linear(ef, 0x04, "82079e804875616e6720c3ffffffffffffff07815155258102f4ffffffffffff");
    asimcard_ef_update_linear(ef, 0xff, "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
    asimcard_ef_add(card, ef);

    // Fixed Dialling Numbers(6F3B)
    //   Record size: 0x20
    //   Record count: 0xff
    //   Length of BCD number/SSC contents: 7
    //   TON and NPI: 0x81
    // @see 3GPP TS 51.011 section 10.5.2 EFfdn
    ef = asimcard_ef_new_linear(0x6f3b, SIM_FILE_NEED_PIN, 0x20);
    // Alpha Id(Encoded with GSM 8 bit): "Mozilla", Dialling Number: 15555218201
    asimcard_ef_update_linear(ef, 0x01, "4d6f7a696c6c61ffffffffffffffffffffff07815155258102f1ffffffffffff");
    // Alpha Id(Encoded with UCS2 0x80: "Saßê\u9ec3", Dialling Number: 15555218202
    asimcard_ef_update_linear(ef, 0x02, "800053006100df00ea9ec3ffffffffffffff07815155258102f2ffffffffffff");
    // Alpha Id(Encoded with UCS2 0x81): "Fire \u706b", Dialling Number: 15555218203
    asimcard_ef_update_linear(ef, 0x03, "8106e04669726520ebffffffffffffffffff07815155258102f3ffffffffffff");
    // Alpha Id(Encoded with UCS2 0x82): "Huang \u9ec3", Dialling Number: 15555218204
    asimcard_ef_update_linear(ef, 0x04, "82079e804875616e6720c3ffffffffffffff07815155258102f4ffffffffffff");
    asimcard_ef_update_linear(ef, 0xff, "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
    asimcard_ef_add(card, ef);

    // Group Identifier Level 1(6F3E):
    //   File size: 0x02
    //   GID1: 5a4d
    // @see 3GPP TS 51.011 section 10.3.9 EFgid1 (Group Identifier Level 1)
    // @see 3GPP TS 31.102 section 4.2.10 EFgid1 (Group Identifier Level 1)
    ef = asimcard_ef_new_dedicated(0x6f3e, SIM_FILE_READ_ONLY | SIM_FILE_NEED_PIN);
    asimcard_ef_update_dedicated(ef, "5a4d");
    asimcard_ef_add(card, ef);

    // Service Dialling Numbers(6F49)
    //   Record size: 0x20
    //   Record count: 0xff
    //   Length of BCD number/SSC contents: 7
    //   TON and NPI: 0x81
    // @see 3GPP TS 51.011 section 10.5.9 EFsdn
    ef = asimcard_ef_new_linear(0x6f49, SIM_FILE_NEED_PIN, 0x20);
    // Alpha Id(Encoded with GSM 8 bit): "Mozilla", Dialling Number: 15555218201
    asimcard_ef_update_linear(ef, 0x01, "4d6f7a696c6c61ffffffffffffffffffffff07815155258102f1ffffffffffff");
    // Alpha Id(Encoded with UCS2 0x80: "Saßê\u9ec3", Dialling Number: 15555218202
    asimcard_ef_update_linear(ef, 0x02, "800053006100df00ea9ec3ffffffffffffff07815155258102f2ffffffffffff");
    // Alpha Id(Encoded with UCS2 0x81): "Fire \u706b", Dialling Number: 15555218203
    asimcard_ef_update_linear(ef, 0x03, "8106e04669726520ebffffffffffffffffff07815155258102f3ffffffffffff");
    // Alpha Id(Encoded with UCS2 0x82): "Huang \u9ec3", Dialling Number: 15555218204
    asimcard_ef_update_linear(ef, 0x04, "82079e804875616e6720c3ffffffffffffff07815155258102f4ffffffffffff");
    asimcard_ef_update_linear(ef, 0xff, "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
    asimcard_ef_add(card, ef);

    // Cell Broadcast Message Identifier selection(6F45):
    //   File size: 0x06
    //   CB Message Identifier 1: 45056(B000)
    //   CB Message Identifier 2: 65535(FFFF, not used)
    //   CB Message Identifier 3: 61440(F000, not settable by MMI)
    // @see 3GPP TS 31.102 section 4.2.14 EFcbmi (Cell Broadcast Message Identifier selection)
    ef = asimcard_ef_new_dedicated(0x6f45, SIM_FILE_READ_ONLY);
    asimcard_ef_update_dedicated(ef, "b000fffff000");
    asimcard_ef_add(card, ef);

    // Cell Broadcast Message Identifier for Data Download(6F48):
    //   File size: 0x06
    //   CB Message Identifier 1: 45056(C001)
    //   CB Message Identifier 2: 65535(FFFF, not used)
    //   CB Message Identifier 3: 61440(F001, not settable by MMI)
    // @see 3GPP TS 31.102 v110.02.0 section 4.2.20 EFcbmid (Cell Broadcast Message Identifier for Data Download)
    ef = asimcard_ef_new_dedicated(0x6f48, SIM_FILE_READ_ONLY);
    asimcard_ef_update_dedicated(ef, "c001fffff001");
    asimcard_ef_add(card, ef);

    // Cell Broadcast Message Identifier Range selection(6F50):
    //   File size: 0x10
    //   CB Message Identifier Range 1: 45058..49152(B002..C000)
    //   CB Message Identifier Range 2: 65535..49153(FFFF..C001, should be ignored)
    //   CB Message Identifier Range 3: 49153..65535(C001..FFFF, should be ignored)
    //   CB Message Identifier Range 4: 61442..65280(F002..FF00, not settable by MMI)
    // @see 3GPP TS 31.102 section 4.2.14 EFcbmir (Cell Broadcast Message Identifier Range selection)
    ef = asimcard_ef_new_dedicated(0x6f50, SIM_FILE_READ_ONLY);
    asimcard_ef_update_dedicated(ef, "b002c000ffffc001c001fffff002ff00");
    asimcard_ef_add(card, ef);

    return 0;
}

static int sim_file_to_hex( SimFile  file, bytes_t  dst );

static const char*
asimcard_io_get_response( ASimCard sim, int id, int p1, int p2, int p3 )
{
    int    count;
    char*   out = sim->out_buff;
    SimFile ef  = asimcard_ef_find(sim, id);

    if (ef == NULL) {
        return SIM_RESPONSE_FILE_NOT_FOUND;
    }

    if (p1 != 0 || p2 != 0 || p3 != 15) {
        return SIM_RESPONSE_INCORRECT_PARAMETERS;
    }

    sprintf(out, "%s,", SIM_RESPONSE_NORMAL_ENDING);
    out  += strlen(out);
    count = sim_file_to_hex(ef, out);
    if (count < 0) {
        return SIM_RESPONSE_EXECUTION_ERROR;
    }
    out[count] = 0;

    return sim->out_buff;
}

/* convert a SIM File descriptor into an ASCII string,
   assumes 'dst' is NULL or properly sized.
   return the number of chars, or -1 on error */
static int
sim_file_to_hex( SimFile  file, bytes_t  dst )
{
    SimFileType  type   = file->any.type;
    int          result = 0;

    /* see 9.2.1 in TS 51.011 */
    switch (type) {
        case SIM_FILE_EF_DEDICATED:
        case SIM_FILE_EF_LINEAR:
        case SIM_FILE_EF_CYCLIC:
            {
                if (dst) {
                    int  file_size, perm;

                    memcpy(dst, "0000", 4);  /* bytes 1-2 are RFU */
                    dst += 4;

                    /* bytes 3-4 are the file size */
                    if (type == SIM_FILE_EF_DEDICATED)
                        file_size = file->dedicated.length;
                    else
                        file_size = file->linear.rec_count * file->linear.rec_len;

                    gsm_hex_from_short( dst, file_size );
                    dst += 4;

                    /* bytes 5-6 are the file id */
                    gsm_hex_from_short( dst, file->any.id );
                    dst += 4;

                    /* byte 7 is the file type - always EF, i.e. 0x04 */
                    dst[0] = '0';
                    dst[1] = '4';
                    dst   += 2;

                    /* byte 8 is RFU, except bit 7 for cyclic files, which indicates
                       that INCREASE is allowed. Since we don't support this yet... */
                    dst[0] = '0';
                    dst[1] = '0';
                    dst   += 2;

                    /* byte 9-11 are access conditions */
                    if (file->any.flags & SIM_FILE_READ_ONLY) {
                        if (file->any.flags & SIM_FILE_NEED_PIN)
                            perm = 0x1a;
                        else
                            perm = 0x0a;
                    } else {
                        if (file->any.flags & SIM_FILE_NEED_PIN)
                            perm = 0x11;
                        else
                            perm = 0x00;
                    }
                    gsm_hex_from_byte(dst, perm);
                    memcpy( dst+2, "a0aa", 4 );
                    dst += 6;

                    /* byte 12 is file status, we don't support invalidation */
                    dst[0] = '0';
                    dst[1] = '0';
                    dst   += 2;

                    /* byte 13 is length of the following data, always 2 */
                    dst[0] = '0';
                    dst[1] = '2';
                    dst   += 2;

                    /* byte 14 is struct of EF */
                    dst[0] = '0';
                    if (type == SIM_FILE_EF_DEDICATED) {
                        dst[1] = '0';
                    } else if (type == SIM_FILE_EF_LINEAR) {
                        dst[1] = '1';
                    } else {
                        dst[1] = '3';
                    }
                    dst   += 2;

                    /* byte 15 is lenght of record, or 0 */
                    if (type == SIM_FILE_EF_DEDICATED) {
                        dst[0] = '0';
                        dst[1] = '0';
                    } else {
                        gsm_hex_from_byte( dst, file->linear.rec_len );
                    }
                    dst   += 2;
                }
                result = 30;
            }
            break;

        default:
            result = -1;
    }
    return result;
}

const char*
asimcard_io( ASimCard  sim, const char*  cmd )
{
    int  command, id, p1, p2, p3;
    char data[128] = {'\0'};

    assert( memcmp( cmd, "+CRSM=", 6 ) == 0 );

    if ( sim->status == A_SIM_STATUS_ABSENT) {
        // SIM not inserted
        return "+CME ERROR: 10";
    }

    if ( sscanf(cmd, "+CRSM=%d,%d,%d,%d,%d,%s", &command, &id, &p1, &p2, &p3, data) >= 5 ) {
        switch (command) {
            case A_SIM_CMD_GET_RESPONSE:
                return asimcard_io_get_response(sim, id, p1, p2, p3);

            case A_SIM_CMD_READ_BINARY:
                return asimcard_io_read_binary(sim, id, p1, p2, p3);

            case A_SIM_CMD_READ_RECORD:
                return asimcard_io_read_record(sim, id, p1, p2, p3);

            case A_SIM_CMD_UPDATE_RECORD:
                return asimcard_io_update_record(sim, id, p1, p2, p3, data);

            default:
                return SIM_RESPONSE_FUNCTION_NOT_SUPPORT;
        }
    }

    return SIM_RESPONSE_INCORRECT_PARAMETERS;
}

const char*
asimcard_stk_terminal_response( ASimCard  sim, const char*  cmd )
{
    // Expected Format: +CUSATT=<terminal_response>
    assert( memcmp( cmd, "+CUSATT=", 8 ) == 0 );

    if ( sim->status == A_SIM_STATUS_ABSENT) {
        // SIM not inserted
        return "+CME ERROR: 10";
    }

    memset(sim->last_stk_response, 0, sizeof(sim->last_stk_response));

    if (sscanf(cmd, "+CUSATT=%s", sim->last_stk_response) != 1) {
        // Incorrect parameters
        return "+CME ERROR: 50";
    }

    return NULL;
}

const char*
asimcard_stk_envelope_command( ASimCard  sim, const char*  cmd )
{
    // Expected Format: +CUSATE=<envelope_command>
    assert( memcmp( cmd, "+CUSATE=", 8 ) == 0 );

    if ( sim->status == A_SIM_STATUS_ABSENT) {
        // SIM not inserted
        return "+CME ERROR: 10";
    }

    memset(sim->last_stk_envelope, 0, sizeof(sim->last_stk_envelope));

    if (sscanf(cmd, "+CUSATE=%s", sim->last_stk_envelope) != 1) {
        // Incorrect parameters
        return "+CME ERROR: 50";
    }

    // TODO fill in envelope response PDU accordingly.
    return NULL;
}

const char*
asimcard_get_last_stk_response( ASimCard  sim)
{
    return sim->last_stk_response;
}

const char*
asimcard_get_last_stk_envelope( ASimCard  sim)
{
    return sim->last_stk_envelope;
}
