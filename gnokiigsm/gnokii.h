/*

  $Id: gnokii.h,v 1.108 2006/11/26 16:24:24 pkot Exp $

  G N O K I I

  A Linux/Unix toolset and driver for Nokia mobile phones.

  This file is part of gnokii.

  Gnokii is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  Gnokii is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with gnokii; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

  Copyright (C) 1999-2000 Hugh Blemings & Pavel Jan�k ml.
  Copyright (C) 2001-2005 Pawel Kot
  Copyright (C) 2001      Chris Kemp
  copyright (C) 2002      Markus Plail
  Copyright (C) 2002-2003 BORBELY Zoltan, Ladis Michl
  Copyright (C) 2004      Martin Goldhahn
  Copyright (C) 2006      Helge Deller
  
  Main header file for gnokii. Include just this file in your app.
*/

#ifndef _gnokii_h
#define _gnokii_h

#ifdef __cplusplus
extern "C" {
#endif

/* Some portability definitions first */
#if defined(__linux__) || defined(__GLIBC__)
#  include <stdint.h>
#  include <sys/time.h>
#elif defined(__svr4__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__bsdi__) || defined(__MACH__)
#  include <inttypes.h>
#  include <sys/time.h>
#elif defined(_MSC_VER) && defined(WIN32)
#  include <Winsock.h>	/* for struct timeval */
typedef unsigned char uint8_t;
#endif

#include <stdarg.h>
	
#ifndef GNOKII_API
# define GNOKII_API
#endif

struct gn_statemachine;

#include <error.h>
#include <common.h>
#include <data.h>
#include <encoding.h>
#include <sms.h>
#include <call.h>
#include <networks.h>
#include <bitmaps.h>
#include <ringtones.h>
#include <virtmodem.h>
#include <rlp-common.h>
#include <wappush.h>

#include <statemachine.h>

/* Gnokii library functions - they all start with gn_lib_ */
/* Get the current runtime version (LIBGNOKII_VERSION) of libgnokii - see LIBGNOKII_MAKE_VERSION() */
GNOKII_API unsigned int gn_lib_version();

/* free any resources which were used by libgnokii */
GNOKII_API void gn_lib_library_free( void );

/* return last error code from functions below */
GNOKII_API gn_error gn_lib_lasterror( struct gn_statemachine *state );

/* initialize gnokii library and structures for given phone.
   configname is the name of the profile, if empty or NULL, [global] will be taken.
   gn_lib_phoneprofile_load assumes default config file. */
GNOKII_API gn_error gn_lib_phoneprofile_load_from_file( const char *configfile, const char *configname, struct gn_statemachine **state );
GNOKII_API gn_error gn_lib_phoneprofile_load( const char *configname, struct gn_statemachine **state );

/* Free any structures and resources which were loaded in this state machine. */
GNOKII_API gn_error gn_lib_phoneprofile_free( struct gn_statemachine **state );

/* opens connection to the phone. Necessary locking will be made */
GNOKII_API gn_error gn_lib_phone_open( struct gn_statemachine *state );

/* closes the connection to the phone */
GNOKII_API gn_error gn_lib_phone_close( struct gn_statemachine *state );

/* ask phone for static information (model, manufacturer, revision and imei) */
GNOKII_API const char *gn_lib_get_phone_model       ( struct gn_statemachine *state ); /* e.g. 6310 */
GNOKII_API const char *gn_lib_get_phone_product_name( struct gn_statemachine *state ); /* e.g. NPE-4 */
GNOKII_API const char *gn_lib_get_phone_manufacturer( struct gn_statemachine *state ); /* e.g. Nokia */
GNOKII_API const char *gn_lib_get_phone_revision    ( struct gn_statemachine *state ); /* e.g. V 04.20 */
GNOKII_API const char *gn_lib_get_phone_imei        ( struct gn_statemachine *state );

/* Get the key value from the given section and the key name */
GNOKII_API const char *gn_lib_cfg_get(const char *section, const char *key);

/* Phone Addressbook functions */
/* get number of used/free phone addressbook memory slots */
GNOKII_API gn_error gn_lib_addressbook_memstat( struct gn_statemachine *state,
		const gn_memory_type memory_type,
		int *num_used, int *num_free );

/* remove an addressbook entry physically from phone. */
GNOKII_API gn_error gn_lib_phonebook_entry_delete( struct gn_statemachine *state,
		const gn_memory_type memory_type, const int index );

/* check if given addressbook entry is empty. Returns "true" if empty, 0 if not. */
GNOKII_API int gn_lib_phonebook_entry_isempty( struct gn_statemachine *state,
		const gn_memory_type memory_type, const int index );

/* read given addressbook entry into internal structure. Afterwards ask for each value with functions below */
GNOKII_API gn_error gn_lib_phonebook_read_entry( struct gn_statemachine *state,
		const gn_memory_type memory_type, const int index );

/* after reading an entry with gn_lib_phonebook_read_entry() ask for the values of the phonebook entry */
GNOKII_API const char *            gn_lib_get_pb_name           ( struct gn_statemachine *state ); /* Mr. Miller */
GNOKII_API const char *            gn_lib_get_pb_number         ( struct gn_statemachine *state ); /* +18001189383 */
GNOKII_API gn_phonebook_group_type gn_lib_get_pb_caller_group   ( struct gn_statemachine *state ); /* */
GNOKII_API gn_memory_type          gn_lib_get_pb_memtype        ( struct gn_statemachine *state ); /* */
GNOKII_API int                     gn_lib_get_pb_location       ( struct gn_statemachine *state ); /* 1*/
GNOKII_API gn_timestamp            gn_lib_get_pb_date           ( struct gn_statemachine *state ); /* */
GNOKII_API int                     gn_lib_get_pb_num_subentries ( struct gn_statemachine *state ); /* */
GNOKII_API gn_error	gn_lib_get_pb_subentry( struct gn_statemachine *state, const int index, 
			gn_phonebook_entry_type *entry_type, gn_phonebook_number_type *number_type, const char **number );

/* How to write to a phonebook entry with 3 steps:
   a) call gn_lib_phonebook_prepare_write_entry() to initialize structures
   b) call gn_lib_set_pb_*() to set values (do this for each value you want to set)
   c) call gn_lib_phonebook_write_entry() to write it physically to phone
 */

/* initialize gnokii internal phonebook structure */
GNOKII_API gn_error gn_lib_phonebook_prepare_write_entry( struct gn_statemachine *state );

/* call each of the functions to set a specific value in current internal memory */
GNOKII_API gn_error gn_lib_set_pb_name        ( struct gn_statemachine *state, const char *name ); /* Mr. Miller */
GNOKII_API gn_error gn_lib_set_pb_number      ( struct gn_statemachine *state, const char *number ); /* +18001189383 */
GNOKII_API gn_error gn_lib_set_pb_caller_group( struct gn_statemachine *state, gn_phonebook_group_type grouptype ); /* */
GNOKII_API gn_error gn_lib_set_pb_memtype     ( struct gn_statemachine *state, gn_memory_type memtype ); /* */
GNOKII_API gn_error gn_lib_set_pb_location    ( struct gn_statemachine *state, int location ); /* 1*/
GNOKII_API gn_error gn_lib_set_pb_date        ( struct gn_statemachine *state, gn_timestamp timestamp ); /* */
GNOKII_API gn_error gn_lib_set_pb_subentry    ( struct gn_statemachine *state, const int index, /* index=-1 appends it */
	gn_phonebook_entry_type entry_type, gn_phonebook_number_type number_type, const char *number );

/* and now write the addressbook entry physically to phone. */
GNOKII_API gn_error gn_lib_phonebook_write_entry( struct gn_statemachine *state,
        const gn_memory_type memory_type, const int index );

GNOKII_API char *gn_subentrytype2string(gn_phonebook_entry_type entry_type, gn_phonebook_number_type number_type);

/* standard helper functions */

/* use this function to get list of all supported phone models.
   Just start with num=0 and increase until you get back NULL */
GNOKII_API const char *gn_lib_get_supported_phone_model( const int num );

/* use this function to get list of all supported connection type strings.
   Just start with num=0 and increase until you get back NULL */
GNOKII_API const char *gn_lib_get_supported_connection( const int num );

/* use this function to search for a phone which is connected to this
   computer. Currently libgnokii will search only on IRDA and bluetooth.
   The state variable will be initialized if a phone was found. */
GNOKII_API gn_error gn_lib_search_one_connected_phone( struct gn_statemachine **state );

/* Misc */
GNOKII_API gn_memory_type gn_str2memory_type(const char *s);
GNOKII_API const char *gn_memory_type2str(gn_memory_type mt);
GNOKII_API int gn_timestamp_isvalid(const gn_timestamp dt);
GNOKII_API void gn_timestamp_set(gn_timestamp *dt, int year, int month, int day,
			int hour, int minute, int second, int timezone);
GNOKII_API void gn_timestamp_get(gn_timestamp *dt, int *year, int *month, int *day,
			int *hour, int *minute, int *second, int *timezone);



/****************************************************************************/
/* ALL FOLLOWING FUNCTIONS SHOULD BE USED BY GNOKII INTERNAL PROGRAMS ONLY  */
/****************************************************************************/

#ifndef GNOKII_DEPRECATED
#if __GNUC__ - 0 > 3 || (__GNUC__ - 0 == 3 && __GNUC_MINOR__ - 0 >= 2)
  /* gcc >= 3.2 */
# define GNOKII_DEPRECATED __attribute__ ((deprecated))
#elif defined(_MSC_VER) && (_MSC_VER >= 1300) && (_MSC_VER < 1400)
  /* msvc >= 7 */
# define GNOKII_DEPRECATED __declspec(deprecated)
#elif defined(_MSV_VER) && (_MSC_VER >= 1400)
  /* MS Visual Studio 2005 */
# define GNOKII_DEPRECATED
#else
# define GNOKII_DEPRECATED
#endif
#endif

/* The global variable that keeps the current configuration. This should be
 * filled in before the phone initialization */
extern GNOKII_API struct gn_cfg_header *gn_cfg_info GNOKII_DEPRECATED;

/* Files */
GNOKII_API int gn_file_text_save(char *filename, char *text, int mode) GNOKII_DEPRECATED;

/* Zero the gn_data structure */
GNOKII_API void gn_data_clear(gn_data *data);
extern GNOKII_API gn_phone *gn_gsm_info;
extern GNOKII_API gn_error (*gn_gsm_f)(gn_operation op, gn_data *data,
			 struct gn_statemachine *state);
/* Initialise the connection and setup the driver according to the current
 * configuration */
GNOKII_API gn_error gn_gsm_initialise(struct gn_statemachine *sm) GNOKII_DEPRECATED;

/* Config handling */
/* Get the key value from the given config, given section and the key name */
GNOKII_API char *gn_cfg_get(struct gn_cfg_header *cfg, const char *section, const char *key) GNOKII_DEPRECATED;
GNOKII_API gn_error gn_cfg_read(char **bindir) GNOKII_DEPRECATED;
/* Read the config from the file filename */
GNOKII_API gn_error gn_cfg_file_read(const char *filename) GNOKII_DEPRECATED;
/* Read the config from the file already put into the memory */
GNOKII_API gn_error gn_cfg_memory_read(const char **lines) GNOKII_DEPRECATED;
/* Read the config from the standard $HOME/.gnokiirc or /etc/gnokiirc locations */
GNOKII_API gn_error gn_cfg_read_default() GNOKII_DEPRECATED;
/* free any resources which were used by libgnokii */
GNOKII_API void gn_cfg_free_default() GNOKII_DEPRECATED;
/* Use phone_iname section for the communication. Default is the global section */
GNOKII_API gn_error gn_cfg_phone_load(const char *iname, struct gn_statemachine *state) GNOKII_DEPRECATED;

/* In/Out routines, file formats */
GNOKII_API int gn_phonebook2vcard(FILE *f, gn_phonebook_entry *entry, char *location);
GNOKII_API int gn_vcard2phonebook(FILE *f, gn_phonebook_entry *entry);

GNOKII_API int gn_phonebook2ldif(FILE *f, gn_phonebook_entry *entry);
GNOKII_API int gn_ldif2phonebook(FILE *f, gn_phonebook_entry *entry);

/* reads internal gnokii raw phonebook format */
GNOKII_API gn_error gn_file_phonebook_raw_parse(gn_phonebook_entry *entry, char *buffer);
GNOKII_API gn_error gn_file_phonebook_raw_write(FILE *f, gn_phonebook_entry *entry, char *memory_type_string);

/* DEPRECATED */
GNOKII_API int gn_vcal_file_event_read(char *filename, gn_calnote *cnote, int number) GNOKII_DEPRECATED;
GNOKII_API int gn_vcal_file_todo_read(char *filename, gn_todo *ctodo, int number) GNOKII_DEPRECATED;

GNOKII_API int gn_calnote2ical(FILE *f, gn_calnote *calnote);
GNOKII_API int gn_ical2calnote(FILE *f, gn_calnote *calnote, int id);

GNOKII_API int gn_todo2ical(FILE *f, gn_todo *ctodo);
GNOKII_API int gn_ical2todo(FILE *f, gn_todo *ctodo, int id);

GNOKII_API void gn_number_sanitize(char *number, int maxlen);
GNOKII_API void gn_phonebook_entry_sanitize(gn_phonebook_entry *entry);

/* Debugging */
extern GNOKII_API gn_log_target gn_log_debug_mask;
extern GNOKII_API gn_log_target gn_log_rlpdebug_mask;
extern GNOKII_API gn_log_target gn_log_xdebug_mask;
extern GNOKII_API void (*gn_elog_handler)(const char *fmt, va_list ap);
GNOKII_API void gn_log_debug(const char *fmt, ...);
GNOKII_API void gn_log_rlpdebug(const char *fmt, ...);
GNOKII_API void gn_log_xdebug(const char *fmt, ...);
GNOKII_API void gn_elog_write(const char *fmt, ...);
typedef void (*gn_log_func_t)(const char *fmt, ...);

GNOKII_API int gn_line_get(FILE *file, char *line, int count) GNOKII_DEPRECATED;

/* Place a lock for the given device in /var/lock or /var/run */
GNOKII_API char *gn_device_lock(const char *) GNOKII_DEPRECATED;
/* Remove a lock for the given device from /var/lock or /var/run */
GNOKII_API int gn_device_unlock(char *) GNOKII_DEPRECATED;

GNOKII_API const char *gn_model_get(const char *product_name);
GNOKII_API gn_phone_model *gn_phone_model_get(const char *product_name);

/* SMS */
GNOKII_API gn_error gn_sms_send(gn_data *data, struct gn_statemachine *state);
GNOKII_API gn_error gn_sms_save(gn_data *data, struct gn_statemachine *state);
GNOKII_API gn_error gn_sms_get(gn_data *data, struct gn_statemachine *state);
GNOKII_API gn_error gn_sms_get_no_validate(gn_data *data, struct gn_statemachine *state);
GNOKII_API gn_error gn_sms_get_folder_changes(gn_data *data, struct gn_statemachine *state,
				       int has_folders);
GNOKII_API gn_error gn_sms_delete(gn_data *data, struct gn_statemachine *state);
GNOKII_API gn_error gn_sms_delete_no_validate(gn_data *data, struct gn_statemachine *state);

/* Call service */
GNOKII_API gn_error gn_call_dial(int *call_id, gn_data *data, struct gn_statemachine *state);
GNOKII_API gn_error gn_call_check_active(struct gn_statemachine *state);

#ifdef __cplusplus
}
#endif

#endif	/* _gnokii_h */
