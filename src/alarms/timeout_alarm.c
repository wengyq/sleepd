/* @@@LICENSE
*
*      Copyright (c) 2011-2012 Hewlett-Packard Development Company, L.P.
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*
* LICENSE@@@ */


/**
* @file timeout_alarm.c
*
* @brief New interface add/clear alarms using RTC.
*
*/


#include <glib.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <cjson/json.h>
#include <sys/stat.h>
#include <luna-service2/lunaservice.h>

#include "main.h"
#include "logging.h"
#include "smartsql.h"

#include "lunaservice_utils.h"

#include "timersource.h"

#include "timeout_alarm.h"
#include "config.h"
#include "init.h"

#define LOG_DOMAIN "ALARMS-TIMEOUT: "

#define ACTIVITY_DURATION_MS_MINIMUM 5000
#define ACTIVITY_DURATION_MS_MINIMUM_AS_TEXT "5000 ms"

/* Always define these so we can warn (but allow) a
   timeout below TIMEOUT_MINIMUM_HANDSET_SEC on the
   desktop or emulator builds
*/
#define TIMEOUT_MINIMUM_HANDSET_SEC (5*60)
#define TIMEOUT_MINIMUM_HANDSET_AS_TEXT "5 minutes"

#define TIMEOUT_MINIMUM_SEC (5)
#define TIMEOUT_MINIMUM_AS_TEXT "5 seconds"

// keep the device on for at least 5s.
#define TIMEOUT_KEEP_ALIVE_MS 5000

#define DEFAULT_ACTIVITY_ID "com.palm.sleepd.timeout_fired"

#define STD_ASCTIME_BUF_SIZE	26

// FIXME: database path should be based on gPowerConfig.preference_dir
#define TIMEOUT_DATABASE_NAME "/var/preferences/com.palm.sleep/SysTimeouts.db"

// This allows testing the SQL commands to add the new columns. Set to false for production code
#define CREATE_DB_WITHOUT_ACTIVITY_COLUMNS 0

typedef enum {
    AlarmTimeoutRelative,
    AlarmTimeoutCalendar,
} AlarmTimeoutType;

static LSPalmService *psh = NULL;
static sqlite3 *timeout_db = NULL;
static GTimerSource *sTimerCheck = NULL;

static const char* kSysTimeoutDatabaseName = TIMEOUT_DATABASE_NAME;
/*
   Database Schema.

   We use an sqlite3 database to store all pending events. The schema
   is designed so that the event list can be quickly sorted and the
   next item to fire can be quickly read.

   expiry is the absolute system time of when the next event is to be 
   fired. It is GMT, so all math performed with it needs to be in GMT
   as well.
   */
#if CREATE_DB_WITHOUT_ACTIVITY_COLUMNS
static const char* kSysTimeoutDatabaseCreateSchema = "\
CREATE TABLE IF NOT EXISTS AlarmTimeout (t1key INTEGER PRIMARY KEY,\
                                         app_id TEXT,\
                                         key TEXT,\
                                         uri TEXT,\
                                         params TEXT,\
                                         public_bus INTEGER,\
                                         wakeup   INTEGER,\
                                         calendar INTEGER,\
                                         expiry DATE);";
#else
static const char* kSysTimeoutDatabaseCreateSchema = "\
CREATE TABLE IF NOT EXISTS AlarmTimeout (t1key INTEGER PRIMARY KEY,\
                                         app_id TEXT,\
                                         key TEXT,\
                                         uri TEXT,\
                                         params TEXT,\
                                         public_bus INTEGER,\
                                         wakeup   INTEGER,\
                                         calendar INTEGER,\
                                         expiry DATE,\
                                         activity_id TEXT,\
                                         activity_duration_ms INTEGER);";
#endif

static const char* kSysTimeoutDatabaseCreateIndex = "\
CREATE INDEX IF NOT EXISTS expiry_index on AlarmTimeout (expiry);";

/**
 * @defgroup NewInterface	New interface
 * @ingroup RTCAlarms
 * @brief New interface for RTC alarms using SQLite.
 */

/**
 * @addtogroup NewInterface
 * @{
 */


static time_t rtc_to_wall = 0;
/** 
* @brief Convert to rtc time.
* 
* @param  t 
* 
* @retval
*/
static time_t to_rtc(time_t t)
{
    return t - rtc_to_wall;
}

/** 
* @brief Last wall time.
* 
* @retval
*/
time_t rtc_wall_time(void)
{
	time_t rtctime = 0;
	nyx_system_query_rtc_time(GetNyxSystemDevice(),&rtctime);
    return rtctime + rtc_to_wall;
}

/**
* @brief Calculate the time difference between RTC time and wall time
*/

bool
wall_rtc_diff(time_t *ret_delta)
{
    time_t rtc_time_now = 0;
    time_t wall_time_now = 0;

	nyx_system_query_rtc_time(GetNyxSystemDevice(),&rtc_time_now);

    time(&wall_time_now);

    /* Calculate the time difference */
    time_t delta = wall_time_now - rtc_time_now;
    if (ret_delta) {
        *ret_delta = delta;
    }
    return true;
}

/** 
* @brief Update the rtc and return the difference rtc changed by.
* 
* @retval
*/
static time_t update_rtc(time_t *ret_delta)
{
    bool retVal;
    time_t new_delta = 0;
    time_t delta = 0;

    retVal = wall_rtc_diff(&new_delta);
    if (!retVal)
        return false;

    if (new_delta != rtc_to_wall) {
        delta = new_delta - rtc_to_wall;
        rtc_to_wall = new_delta;
    }
    if (ret_delta)
        *ret_delta = delta;
    return true;
}

static void
_print_timeout(const char *message, const char *app_id, const char *key, bool public_bus, time_t expiry)
{
    struct tm expiry_tm;
    char buf[STD_ASCTIME_BUF_SIZE];

    gmtime_r(&expiry, &expiry_tm);
    asctime_r(&expiry_tm, buf);
    SLEEPDLOG(LOG_DEBUG, "%s, timeout for (\"%s\", \"%s\", %s) at %ld, %s",
              message, app_id, key, public_bus ? "public" : "private", expiry, buf); 
}

void _update_timeouts(void);

/**
* @brief Called when a new alarm from the RTC is fired.
*/
static void _rtc_alarm_fired(nyx_device_handle_t handle, nyx_callback_status_t status, void* data)
{
    g_debug(LOG_DOMAIN "%s ", __FUNCTION__);

    _update_timeouts();
}


/**
* @brief Response to timeout message.
* 
* @param  sh 
* @param  msg 
* @param  ctx 
* 
* @retval
*/
static bool
_timeout_response(LSHandle *sh, LSMessage *message, void *ctx)
{
    struct json_object *object;

    object = json_tokener_parse(LSMessageGetPayload(message));
    if ( is_error(object) ) {
        goto cleanup;
    }

    struct json_object *retValObject;
    retValObject = json_object_object_get(object, "returnValue");

    if (retValObject) {
        bool retVal = json_object_get_boolean(retValObject);
        if (!retVal) {
            SLEEPDLOG(LOG_ERR,
                "%s: Error not send timeout message because of %s",
                __FUNCTION__, LSMessageGetPayload(message));
        }
    }

cleanup:
    if (!is_error(object)) json_object_put(object);
    return true;
}

/** 
* @brief Send a message to the (uri, params) associated with the timeout.
* 
* @param  timeout 
*/
static void
_timeout_fire(_AlarmTimeout *timeout)
{
    g_return_if_fail( timeout_db != NULL );
    g_return_if_fail( timeout != NULL );
    GString *payload = g_string_new("");

    SLEEPDLOG(LOG_INFO, "%s (%s,%s => %s)", __FUNCTION__,
        timeout->app_id, timeout->key, timeout->uri);

    LSHandle *sh = NULL;
    bool retVal;
    LSError lserror;
    LSErrorInit(&lserror);

    sh = GetLunaServiceHandle();

    /* Give system some time to process this timeout before
     * going to sleep again. The client can provide a specific
     * activity ID and duration. Otherwise we use a common default. 
     */ 
    if (
        timeout->activity_id && strlen(timeout->activity_id) &&
        0 != timeout->activity_duration_ms
    ) {
    	g_string_append_printf(payload,
    	            "{\"id\":\"%s\","
    	            "\"duration_ms\":%d}", timeout->activity_id, timeout->activity_duration_ms);
    }
    else
    {
    	g_string_append_printf(payload,
    	    	     "{\"id\":\"%s\","
    	    	     "\"duration_ms\":%d}", DEFAULT_ACTIVITY_ID, TIMEOUT_KEEP_ALIVE_MS);
    }
    LSCallOneReply(sh, "palm://com.palm.power/com/palm/power/activityStart",
               payload->str, NULL, NULL, NULL, NULL);


    if (timeout->public_bus) {
        sh = LSPalmServiceGetPublicConnection(psh);
    }
    else {
        sh = LSPalmServiceGetPrivateConnection(psh);
    }

    // Call Luna-service bus with the uri/params.
    retVal = LSCallFromApplicationOneReply( sh,
            timeout->uri, timeout->params, timeout->app_id,
            _timeout_response, NULL, NULL, &lserror );
    if (!retVal)
    {
        SLEEPDLOG(LOG_WARNING, "%s: Could not send (%s %s): %s",
                __FUNCTION__,
                timeout->uri, timeout->params, lserror.message);
        LSErrorFree (&lserror);
    }
    g_string_free(payload, TRUE);
}

bool _sql_step_finalize(const char *func, sqlite3_stmt *st)
{
    int rc;

    rc = sqlite3_step( st );
    if( rc != SQLITE_DONE ) {
        SLEEPDLOG(LOG_ERR,"%s(), sqlite3_step failed with %d\n", func, rc );
        return false;
    }
    rc = sqlite3_finalize( st );
    if( rc != SQLITE_OK ) {
        SLEEPDLOG(LOG_ERR,"%s(), sqlite3_finalize failed with %d\n", func, rc );
        return false;
    }
    return true;
}

/** 
* @brief Adjusts all relative (non-calendar alarms) by the delta amount.
* 
* @param  delta 
*/
static void
_recalculate_timeouts(time_t delta)
{
    g_debug(LOG_DOMAIN "%s(%ld)", __FUNCTION__, delta);

    if (delta) {

        int rc;
        char** table;
        int noRows, noCols;
        char* zErrMsg;
        int i;

        /* Find all relative (non-calendar alarms) */
        const char *sqlquery =
            "SELECT t1key,expiry FROM AlarmTimeout WHERE calendar=0";

        rc = sqlite3_get_table( timeout_db, sqlquery,
                &table, &noRows, &noCols, &zErrMsg );

        if (SQLITE_OK != rc) {
            SLEEPDLOG( LOG_ERR,
                    "%s() select... failed %s, rc=%d\n", __FUNCTION__, zErrMsg, rc );
            sqlite3_free(zErrMsg);
            return;
        }

        int base = 0;
        for (i = 0; i < noRows; i++) {

            base += noCols;
            const char *table_id = table[base];
            const char *expiry = table[base+1];

            time_t new_expiry = atoi(expiry) + delta;

            /* Delete the timeout.*/
            sqlite3_stmt* st = NULL;
            const char* tail;
            rc=sqlite3_prepare_v2( timeout_db,
                    "UPDATE AlarmTimeout SET expiry=$1 WHERE t1key=$2",
                    -1, &st, &tail);
            if( rc != SQLITE_OK )
            {
                SLEEPDLOG(LOG_ERR, "%s() cannot update expiry", __FUNCTION__);
            }
            else
            {
                rc=sqlite3_bind_int( st, 1, new_expiry );
                rc=sqlite3_bind_int( st, 2, atoi(table_id) );
                _sql_step_finalize(__func__, st);
            }
        }

        sqlite3_free_table( table );
    }
}

/** 
* @brief Trigger all expired timeouts.
*/
static void
_expire_timeouts(void)
{
    g_debug(LOG_DOMAIN "%s ", __FUNCTION__);

    int rc;
    char** table;
    int noRows, noCols;
    char* zErrMsg;
    int i;
    time_t now;
    int base;
    _AlarmTimeout timeout;

    now = rtc_wall_time();

    /* Find all expired calendar timeouts */
    char *sqlquery = g_strdup_printf(
            "SELECT t1key,app_id,key,uri,params,public_bus,activity_id,activity_duration_ms FROM AlarmTimeout "
            "WHERE expiry<=%ld ORDER BY expiry", now);

    rc = sqlite3_get_table( timeout_db, sqlquery,
                            &table, &noRows, &noCols, &zErrMsg );

    g_free(sqlquery);

    if (SQLITE_OK != rc) {
        SLEEPDLOG( LOG_ERR,
            "%s() select... failed %s, rc=%d\n", __FUNCTION__, zErrMsg, rc );
        sqlite3_free(zErrMsg);
        return;
    }

    for (i = 0, base = 0; i < noRows; i++) {

        base += noCols;

        timeout.table_id = table[base];
        timeout.app_id = table[base+1];
        timeout.key = table[base+2];
        timeout.uri = table[base+3];
        timeout.params = table[base+4];
        timeout.public_bus = atoi(table[base+5]);

        /* 
          If we have an upgraded db where the activity_id and activity_duration_ms columns were
          added and there were existing rows then these two fields will return NULL.
        */
        if (!table[base+6] || !table[base+7]) {
            g_debug(LOG_DOMAIN "%s: null activity_id or activity_duration_ms fields for \"%s\":\"%s\"",
                    __func__, timeout.app_id, timeout.key);
        }
        timeout.activity_id = table[base+6]; // _timeout_fire can handle a null activity_id
        timeout.activity_duration_ms = table[base+7] ? atoi(table[base+7]) : 0; // _timeout_fire will fill-in the default duration

        /* Fire timeout */
        _timeout_fire(&timeout);

        /* Delete the timeout.*/
        sqlite3_stmt* st = NULL;
        const char* tail;
        rc=sqlite3_prepare_v2( timeout_db,
                "DELETE FROM AlarmTimeout WHERE t1key=$1",-1,&st,&tail);
        if( rc == SQLITE_OK ) {
            rc=sqlite3_bind_int( st, 1, atoi(timeout.table_id) );
            _sql_step_finalize(__func__, st);
        }
        else {
            SLEEPDLOG(LOG_ERR, "%s prepare failed because of %d", __FUNCTION__, rc);
        }
    }

    sqlite3_free_table( table );
}


bool
timeout_get_next_wakeup(time_t *expiry, gchar ** app_id, gchar ** key)
{
    g_return_val_if_fail(expiry != NULL, false);
    g_return_val_if_fail(app_id != NULL, false);
    g_return_val_if_fail(key != NULL, false);
    bool ret = false;

    char** table;
    int noRows, noCols;
    char* zErrMsg;
    int rc;

    rc = sqlite3_get_table( timeout_db, "SELECT expiry, app_id, key FROM AlarmTimeout "
        "WHERE wakeup=1 ORDER BY expiry LIMIT 1", &table, &noRows, &noCols, &zErrMsg );
    if( rc != SQLITE_OK ) {
        g_critical("%s() failed select expiry %s, rc=%d", __func__, zErrMsg, rc );
        sqlite3_free(zErrMsg );
        return false;
    }

    if( noRows ) {
        *expiry = atol( table[ noCols ] );
        *app_id = g_strdup(table[ noCols + 1 ]);
        *key = g_strdup(table[ noCols + 2 ]);
        ret = true;
    }
    sqlite3_free_table( table );
    return ret;
}


/** 
* @brief Queues both a RTC alarm for wakeup timeouts
*        and a timer for non-wakeup timeouts. 
*
* @param set_callback_fn 
*  If set_callback_fn is set to true, the callback function _rtc_alarm_fired
*  will be triggered as soon as the alarm is fired.  
*  It will be set to true as long as device is awake, and will be set to false when 
*  the device suspends.
* 
* The non-wakeup timeout timer is necessary so that
* these timeouts do not wake the device when they fire.
* Case 1: non-wakeup timeout expires when device is awake (trivial).
* Case 2: non-wakeup timeout expires when device is asleep.
*     On resume, we will check to see if any alarms are expired and fire them.
*/
void
_queue_next_timeout(bool set_callback_fn)
{
    g_debug(LOG_DOMAIN "%s ", __FUNCTION__);

    int rc;
    char** table;
    int noRows, noCols;
    char* zErrMsg;

    time_t rtc_expiry = 0;
    time_t timer_expiry = 0;
    time_t now = rtc_wall_time(); // TODO wall clock? or RTC?

    g_return_if_fail(timeout_db != NULL);

    rc = sqlite3_get_table( timeout_db, "SELECT expiry FROM AlarmTimeout "
        "WHERE wakeup=1 ORDER BY expiry LIMIT 1", &table, &noRows, &noCols, &zErrMsg );
    if( rc != SQLITE_OK ) {
        SLEEPDLOG( LOG_ERR,
            "%s() failed select expiry %s, rc=%d\n",
            __FUNCTION__, zErrMsg, rc );
        sqlite3_free(zErrMsg );
        return;
    }

    if( !noRows ) {
        nyx_system_set_alarm(GetNyxSystemDevice(),0,NULL,NULL);
    }
    else {
        rtc_expiry = atol( table[ noCols ]);
	if(set_callback_fn)
	        nyx_system_set_alarm(GetNyxSystemDevice(),to_rtc(rtc_expiry),_rtc_alarm_fired,NULL);
	else
	        nyx_system_set_alarm(GetNyxSystemDevice(),to_rtc(rtc_expiry),NULL,NULL);

    }
    sqlite3_free_table( table );

    rc = sqlite3_get_table( timeout_db, "SELECT expiry FROM AlarmTimeout "
        "ORDER BY expiry LIMIT 1", &table, &noRows, &noCols, &zErrMsg );
    if( rc != SQLITE_OK ) {
        SLEEPDLOG( LOG_ERR,
            "%s() failed select expiry %s, rc=%d\n",
            __FUNCTION__, zErrMsg, rc );
        sqlite3_free(zErrMsg );
        return;
    }

    if ( !noRows ) {
        g_timer_source_set_interval_seconds(sTimerCheck, 60*60, true);
    }
    else {
        timer_expiry = atol( table[ noCols ] );
        
        long wakeInSeconds = timer_expiry - now;
        if( wakeInSeconds < 0 ) {
            wakeInSeconds = 0;
        }

        g_timer_source_set_interval_seconds(sTimerCheck, wakeInSeconds, true);
    }
    sqlite3_free_table( table );
}

/** 
* @brief Trigger expired timeouts, and queue up the next one.
*/
void
_update_timeouts(void)
{
    g_debug(LOG_DOMAIN "%s ", __FUNCTION__);

    time_t delta = 0;  // prevent compile error "may be used uninitialized" when optimization is cranked up

    update_rtc(&delta);
    if (delta) {
        _recalculate_timeouts(delta);
        void update_alarms_delta(time_t delta);
        update_alarms_delta(delta);
    }
    _expire_timeouts();
    _queue_next_timeout(true);
}

void _timeout_create(_AlarmTimeout *timeout,
        const char *app_id, const char *key,
        const char *uri, const char *params,
        bool public_bus, bool wakeup,
        const char *activity_id,
        int activity_duration_ms,
        bool calendar, time_t expiry)
{
    g_debug(LOG_DOMAIN "%s ", __FUNCTION__);

    g_return_if_fail(timeout != NULL);

    timeout->app_id = app_id;
    timeout->key = key;
    timeout->uri = uri;
    timeout->params = params;
    timeout->public_bus = public_bus;
    timeout->wakeup = wakeup;
    timeout->activity_id = activity_id;
    timeout->activity_duration_ms = activity_duration_ms;
    timeout->calendar = calendar;
    timeout->expiry = expiry;
}

bool
_timeout_set(_AlarmTimeout *timeout)
{
    g_debug(LOG_DOMAIN "%s ", __FUNCTION__);

    int rc;
    sqlite3_stmt* st = NULL;
    const char* tail;

    g_return_val_if_fail(timeout != NULL, false);

    /* Delete (app_id,key,public_bus) if it already exists */
    _timeout_delete(timeout->app_id, timeout->key, timeout->public_bus);

    rc = sqlite3_prepare_v2( timeout_db,
            "INSERT INTO AlarmTimeout (app_id,key,uri,params,public_bus,wakeup,calendar,expiry,activity_id,activity_duration_ms) "
            "VALUES ( $1, $2, $3, $4, $5, $6, $7, $8, $9, $10 )", -1, &st, &tail );
    if( rc != SQLITE_OK ) {
        SLEEPDLOG(LOG_ERR,"Insert into AlarmTimeout failed with %d\n", rc );
        return false;
    }

    sqlite3_bind_text( st,  1, timeout->app_id, timeout->app_id ? strlen(timeout->app_id) : -1, SQLITE_STATIC );
    sqlite3_bind_text( st,  2, timeout->key, strlen(timeout->key), SQLITE_STATIC );
    sqlite3_bind_text( st,  3, timeout->uri, strlen(timeout->uri), SQLITE_STATIC );
    sqlite3_bind_text( st,  4, timeout->params, strlen(timeout->params), SQLITE_STATIC );
    sqlite3_bind_int(  st,  5, timeout->public_bus );
    sqlite3_bind_int(  st,  6, timeout->wakeup );
    sqlite3_bind_int(  st,  7, timeout->calendar );
    sqlite3_bind_int(  st,  8, timeout->expiry );
    sqlite3_bind_text( st,  9, timeout->activity_id, strlen(timeout->activity_id), SQLITE_STATIC );
    sqlite3_bind_int(  st, 10, timeout->activity_duration_ms );

    if (!_sql_step_finalize(__func__, st))
    {
        return false;
    }

    _update_timeouts();

    return true;
}

void
_free_timeout_fields(_AlarmTimeoutNonConst *timeout)
{
    g_free(timeout->table_id);
    g_free(timeout->app_id);
    g_free(timeout->key);
    g_free(timeout->uri);
    g_free(timeout->params);
    g_free(timeout->activity_id);
    memset(timeout, 0, sizeof(*timeout));
}

/** 
* @brief Read an existing timeout from the database.
*  
* @param  timeout 
* @param  app_id 
* @param  key 
* @param  public_bus 
* 
* @retval true if at least one row matched 
*  
* Call _free_timeout_fields() with the timeout when done. 
*  
*/
bool
_timeout_read(_AlarmTimeoutNonConst *timeout, const char *app_id, const char *key, bool public_bus)
{
    bool ret = false;
    int rc;
    char** table;
    int noRows, noCols;
    char* zErrMsg;

    if (!app_id) {
        app_id = "";
    }
    if (!key) {
        key = "";
    }

    g_debug(LOG_DOMAIN "%s: SELECT (\"%s\", \"%s\", %s)", __FUNCTION__,
            app_id, key, public_bus ? "public" : "private");

    char *sqlquery = g_strdup_printf(
            "SELECT t1key,app_id,key,uri,params,public_bus,wakeup,calendar,expiry,activity_id,activity_duration_ms FROM AlarmTimeout "
            "WHERE app_id=\"%s\" AND key=\"%s\" AND public_bus=%d", app_id, key, public_bus);

    rc = sqlite3_get_table( timeout_db, sqlquery, &table, &noRows, &noCols, &zErrMsg );

    g_free(sqlquery);

    if( rc != SQLITE_OK ) {
        SLEEPDLOG( LOG_ERR,
            "%s() failed select on %s, rc=%d\n",
            __FUNCTION__, zErrMsg, rc );
        sqlite3_free(zErrMsg );
        goto exit;
    }

    if( noRows ) {
        if (noRows > 1) {
            g_warning(LOG_DOMAIN "%s: ERROR, %d rows for (\"%s\", \"%s\", %s)", __func__, noRows, 
                      app_id, key, public_bus ? "public" : "private");
        }

        timeout->table_id               = g_strdup( table[ noCols      ]);
        timeout->app_id                 = g_strdup( table[ noCols +  1 ]);
        timeout->key                    = g_strdup( table[ noCols +  2 ]);
        timeout->uri                    = g_strdup( table[ noCols +  3 ]);
        timeout->params                 = g_strdup( table[ noCols +  4 ]);
        timeout->public_bus             = atoi(     table[ noCols +  5 ]);
        timeout->wakeup                 = atoi(     table[ noCols +  6 ]);
        timeout->calendar               = atoi(     table[ noCols +  7 ]);
        timeout->expiry                 = atol(     table[ noCols +  8 ]);

        // The two "activity" fields could be null if this is an
        // old record where the new columns were inserted.
        timeout->activity_id            = table[noCols + 9] ? g_strdup(table[noCols +  9]) : g_strdup(DEFAULT_ACTIVITY_ID);
        timeout->activity_duration_ms   = table[noCols + 10] ? atoi(table[noCols + 10]) : TIMEOUT_KEEP_ALIVE_MS;

        ret = true;
    }

    sqlite3_free_table( table );

exit:
    return ret;

} // _timeout_read

/** 
* @brief Delete an existing timeout.
* 
* @param  app_id 
* @param  key 
* @param  public_bus 
* 
* @retval
*/
bool
_timeout_delete(const char *app_id, const char *key, bool public_bus)
{
    sqlite3_stmt* st = NULL;
    const char* tail;
    int rc;

    if (!app_id) {
        app_id = "";
    }
    if (!key) {
        key = "";
    }

    g_debug(LOG_DOMAIN "%s (\"%s\", \"%s\", %s)", __FUNCTION__, app_id, key,
            public_bus ? "public" : "private");

    /* Delete the matching timeout.*/
    rc=sqlite3_prepare_v2( timeout_db,
            "DELETE FROM AlarmTimeout WHERE "
            "app_id=$1 AND key=$2 AND public_bus=$3",-1,&st,&tail);
    if( rc != SQLITE_OK ) {
        SLEEPDLOG(LOG_DEBUG,
            "Could not remove AlarmTimeout, failed with %d\n", rc );
        return false;
    }

    sqlite3_bind_text( st, 1, app_id, app_id ? strlen(app_id) : -1, SQLITE_STATIC );
    sqlite3_bind_text( st, 2, key, key ? strlen(key) : -1, SQLITE_STATIC );
    sqlite3_bind_int(  st, 3, public_bus );

    return _sql_step_finalize(__func__, st);

} // _timeout_delete

/** 
 * @brief Clear an existing timeout & reschedule the next.
 * 
 * @param  app_id 
 * @param  key 
 */
bool
_timeout_clear(const char *app_id, const char *key, bool public_bus)
{
    bool retVal;
    retVal = _timeout_delete(app_id, key, public_bus);
    if (retVal) {
        _update_timeouts();
    }
    return retVal;
}


/** 
* @brief To help us track down NOV-80968, where rtc dies...
* 
* @param  data 
* 
* @retval
*/
static gboolean
_rtc_check(gpointer data)
{
    static long int sLastRTCTime = 0;
    static long int sNumTimes = 0;

    long int this_time = 0;
	nyx_system_query_rtc_time(GetNyxSystemDevice(),(time_t *)&this_time);

    if (this_time == sLastRTCTime) {
        sNumTimes++;
        g_critical("%s: RTC appears not to be ticking! nyx_query_time(NULL) returned %ld, number of times showing same RTC time : %ld", __func__, this_time, sNumTimes);
    } else {
        sNumTimes = 0;
    }
    sLastRTCTime=this_time;

    return TRUE;
}

/** 
* @brief Triggered for non-wakeup source.
* 
* @param  data 
* 
* @retval
*/
static gboolean
_timer_check(gpointer data)
{
    g_debug(LOG_DOMAIN "%s ", __FUNCTION__);

    _update_timeouts();
    return TRUE;
}

static char *
_get_appid_dup(const char *app_instance_id)
{
    if (!app_instance_id) {
        return NULL;
    }

    char *s = strstr(app_instance_id, " ");
    if (s) {
        int len = s - app_instance_id;
        return g_strndup(app_instance_id, len);
    }
    else {
        return g_strdup(app_instance_id);
    }
}

static bool
_timeout_exists(const char *app_id, const char *key, bool public_bus)
{
    _AlarmTimeoutNonConst timeout;

    bool ret = _timeout_read(&timeout, app_id, key, public_bus);

    if (ret) {
        _print_timeout(LOG_DOMAIN "timeout exists", app_id, key, public_bus, timeout.expiry); 
        _free_timeout_fields(&timeout);
    }

    return ret;
}

/** 
* @brief Handle a timeout/set message and add a new power timeout.
* Relative timeouts can be set by passing the "in" parameter.
* Absolute timeouts can be set by passing the "at" parameter.
* 
* @param  sh 
* @param  message 
* @param  ctx 
* 
* @retval
*/
static bool
_alarm_timeout_set(LSHandle *sh, LSMessage *message, void *ctx)
{
    g_debug(LOG_DOMAIN "%s ", __FUNCTION__);

    bool retVal;
    const char *app_instance_id;
    const char *key;
    const char *at;
    const char *in;
    const char *uri;
    const char *params;
    bool wakeup;
    const char *activity_id;
    int activity_duration_ms;
    bool calendar;
    time_t expiry;
    _AlarmTimeout timeout;

    char *app_id = NULL;

    bool public_bus;
    AlarmTimeoutType timeout_type;
    struct json_object *object;
    struct json_object *duration_object;
    bool duration_provided;

    // for optional "keep_existing" boolean argument
    bool keep_existing_provided;
    bool keep_existing = false;
    struct json_object *keep_existing_object;

    object = json_tokener_parse(LSMessageGetPayload(message));
    if ( is_error(object) ) {
        goto malformed_json;
    }

    app_instance_id = LSMessageGetApplicationID(message);
    key = json_object_get_string(json_object_object_get(object, "key"));
    at = json_object_get_string(json_object_object_get(object, "at"));
    in = json_object_get_string(json_object_object_get(object, "in"));
    uri = json_object_get_string(json_object_object_get(object, "uri"));
    params = json_object_get_string(json_object_object_get(object, "params"));
    wakeup = json_object_get_boolean(json_object_object_get(object, "wakeup"));

    if (!key || !strlen(key) || !uri || !strlen(uri) || !params || !strlen(params)) goto invalid_json;
    if (!app_instance_id) app_instance_id = "";

    // optional arguments to allow caller to specify activity name and duration
    activity_id = json_object_get_string(json_object_object_get(object, "activity_id"));
    duration_provided = json_object_object_get_ex(object, "activity_duration_ms", &duration_object);

    if (activity_id) {
        if (!duration_provided) {
            g_debug(LOG_DOMAIN "activity_id w/o activity_duration_ms");
            goto invalid_json;
        }

        activity_duration_ms = json_object_get_int(duration_object);
        if (activity_duration_ms < ACTIVITY_DURATION_MS_MINIMUM) {
            goto activity_duration_too_short;
        }
    } else {
        if (duration_provided) {
            g_debug(LOG_DOMAIN "activity_duration_ms w/o activity_id");
            goto invalid_json;
        }

        activity_id = DEFAULT_ACTIVITY_ID;
        activity_duration_ms = TIMEOUT_KEEP_ALIVE_MS;
    }

    // optional argument which tells us to keep a pre-existing alarm with the same key
    keep_existing_provided = json_object_object_get_ex(object, "keep_existing", &keep_existing_object);
    if (keep_existing_provided) {
        keep_existing = json_object_get_boolean(keep_existing_object);
    }

    app_id = _get_appid_dup(app_instance_id);

    if (at) {

        SLEEPDLOG(LOG_INFO, "%s (%s,%s,%s) at %s",
            __FUNCTION__, app_id, key, wakeup ? "wakeup" : "_",
            at);

        timeout_type = AlarmTimeoutCalendar;

        int mm, dd, yyyy;
        int HH, MM, SS;

        int ret = sscanf(at, "%02d/%02d/%04d %02d:%02d:%02d",
                &mm, &dd, &yyyy, &HH, &MM, &SS);
        if (ret != 6)
            goto invalid_json;

        int valid_days[12]= {31,28,31,30,31,30,31,31,30,31,30,31};
        int valid_leapyr_days[12]= {31,29,31,30,31,30,31,31,30,31,30,31};

        if(yyyy % 4 != 0) {
        	if ((mm < 0 || mm > 12) || (dd < 0 || dd > valid_days[mm-1])) // Not a leap year
        		goto invalid_json;
        }
        else if ((mm < 0 || mm > 12) || (dd < 0 || dd > valid_leapyr_days[mm-1])) // Leap year
			goto invalid_json;



        struct tm gm_time;
        memset(&gm_time, 0, sizeof(struct tm));

        gm_time.tm_hour = HH;
        gm_time.tm_min = MM;
        gm_time.tm_sec = SS;
        gm_time.tm_mon = mm - 1; // month-of-year [0-11]
        gm_time.tm_mday = dd;      // day-of-month [1-31]
        gm_time.tm_year = yyyy - 1900;

        /* timegm converts time(GMT) -> seconds since epoch */
        expiry = timegm(&gm_time);
        if (expiry < 0)
            expiry = 0;
    }
    else if (in) {

        SLEEPDLOG(LOG_INFO, "%s (%s,%s,%s) in %s",
            __FUNCTION__, app_id, key, wakeup ? "wakeup" : "_",
            in);

        timeout_type = AlarmTimeoutRelative;

        int HH, MM, SS;
        if (sscanf(in, "%02d:%02d:%02d", &HH, &MM, &SS) != 3)
            goto invalid_json;
        else if(HH < 0 || HH > 24 || MM < 0 || MM > 59 || SS < 0 || SS > 59)
        	goto invalid_json;

        int delta = SS + MM*60 + HH*60*60;

#if 0
        if (delta < TIMEOUT_MINIMUM_SEC) {
            goto timeout_relative_too_short;
        }
#endif
        if (delta < TIMEOUT_MINIMUM_HANDSET_SEC) {
            SLEEPDLOG(
                LOG_WARNING,
                "%s: alarm timeout interval of %d seconds is below limit of "
                "%d seconds enforced on actual handsets",
                __FUNCTION__,
                 delta,
                TIMEOUT_MINIMUM_HANDSET_SEC
            );
        }

        expiry = rtc_wall_time() + delta;
    }
    else {
        goto invalid_json;
    }

    public_bus = LSMessageIsPublic(psh, message);
    calendar = (timeout_type == AlarmTimeoutCalendar);

    bool kept_existing = false;
    char *payload;
    if (keep_existing && _timeout_exists(app_id, key, public_bus)) {

        kept_existing = true;

        SLEEPDLOG(LOG_DEBUG, "%s: keeping existing timeout for (\"%s\", \"%s\", %s)",
                  __FUNCTION__, app_id, key, public_bus ? "public" : "private"); 
    } else {
        _timeout_create(&timeout, app_id, key, uri, params,
                        public_bus, wakeup, activity_id, activity_duration_ms, calendar, expiry);
    
        retVal = _timeout_set(&timeout);
        if (!retVal) goto unknown_error;
    }

    char *escaped_key = g_strescape(key, NULL);

    if (keep_existing_provided) {
        payload = g_strdup_printf(
            "{\"returnValue\":true,\"key\":\"%s\",\"kept_existing\":%s}", escaped_key, kept_existing ? "true" : "false");
    } else {
        payload = g_strdup_printf(
            "{\"returnValue\":true,\"key\":\"%s\"}", escaped_key);
    }

    retVal = LSMessageReply(sh, message, payload, NULL);
    if (!retVal)
    {
        SLEEPDLOG(LOG_WARNING, "%s could not send reply.", __FUNCTION__);
    }
    
    g_free(payload);
    g_free(escaped_key);
    goto cleanup;
#if 0
timeout_relative_too_short:
    retVal = LSMessageReply(sh, message, "{\"returnValue\":false,"
            "\"errorText\":\"Timeout value for 'in' less than " TIMEOUT_MINIMUM_AS_TEXT ".\"}", NULL);
    if (!retVal)
    {
        SLEEPDLOG(LOG_WARNING, "%s could not send reply <timeout_too_short>.", __FUNCTION__);
    }

    goto cleanup;
#endif
activity_duration_too_short:
    retVal = LSMessageReply(sh, message, "{\"returnValue\":false,"
            "\"errorText\":\"activity_duration_ms less than " ACTIVITY_DURATION_MS_MINIMUM_AS_TEXT ".\"}", NULL);
    if (!retVal)
    {
        SLEEPDLOG(LOG_WARNING, "%s could not send reply <activity duration too short>.", __FUNCTION__);
    }

    goto cleanup;

unknown_error:
    retVal = LSMessageReply(sh, message, "{\"returnValue\":false,"
            "\"errorText\":\"Could not set timeout.\"}", NULL);
    if (!retVal)
    {
        SLEEPDLOG(LOG_WARNING,
            "%s could not send reply <unknown error>.", __FUNCTION__);
    }
    goto cleanup;

invalid_json:
    retVal = LSMessageReply(sh, message, "{\"returnValue\":false,"
            "\"errorText\":\"Invalid format for 'timeout/set'.\"}", NULL);
    if (!retVal)
    {
        SLEEPDLOG(LOG_WARNING,
            "%s could not send reply <invalid format>.", __FUNCTION__);
    }
    goto cleanup;
malformed_json:
    LSMessageReplyErrorBadJSON(sh, message);
    goto cleanup;
cleanup:
    if (!is_error(object)) json_object_put(object);

    g_free(app_id);
    return true;
}

/**
* @brief Handle a timeout/clear message and delete a timeout by its key.
*
* @param  sh
* @param  message
* @param  ctx
*
* @retval
*/

static bool
_alarm_timeout_clear(LSHandle *sh, LSMessage *message, void *ctx)
{
    bool retVal;

    struct json_object *object;
    const char *app_instance_id;
    const char *key;
    bool public_bus;
    LSError lserror;
    LSErrorInit(&lserror);

    char *app_id = NULL;

    object = json_tokener_parse(LSMessageGetPayload(message));
    if ( is_error(object) ) {
        goto malformed_json;
    }

    app_instance_id = LSMessageGetApplicationID(message);
    key = json_object_get_string(json_object_object_get(object, "key"));
    public_bus = LSMessageIsPublic(psh, message);
    if (!key) {
        goto invalid_json;
    }

    if (!app_instance_id) app_instance_id = "";

    app_id = _get_appid_dup(app_instance_id);

    SLEEPDLOG(LOG_INFO, "%s (%s,%s,%s)", __FUNCTION__, app_id, key,
        public_bus ? "public" : "private");

    retVal = _timeout_clear(app_id, key, public_bus);

    if (!retVal) {
        retVal = LSMessageReply(sh, message,
            "{\"returnValue\":false,\"errorText\":\"Could not find key.\"}", &lserror);
        if (!retVal)
        {
            LSErrorPrint(&lserror, stderr);
        }
    }
    else {
        char *escaped_key = g_strescape(key, NULL);
        char *payload = g_strdup_printf(
                "{\"returnValue\":true,\"key\":\"%s\"}", escaped_key);

        retVal = LSMessageReply(sh, message, payload, &lserror);
        if (!retVal)
        {
            LSErrorPrint(&lserror, stderr);
        }

        g_free(payload);
        g_free(escaped_key);
    }
    goto cleanup;
invalid_json:
    LSMessageReplyErrorInvalidParams(sh, message);
    goto cleanup;
malformed_json:
    LSMessageReplyErrorBadJSON(sh, message);
    goto cleanup;
cleanup:
    if (!is_error(object)) json_object_put(object);
    g_free(app_id);
    LSErrorFree(&lserror);
    return true;

}

static LSMethod timeout_methods[] = {
    { "set", _alarm_timeout_set },
    { "clear", _alarm_timeout_clear },
    { },
};

/** 
* @brief When we wake, we should check to see if any non-wakeup timeouts
*        have expired.
* 
* @param  sh 
* @param  message 
* @param  ctx 
* 
* @retval
*/
static bool
_resume_callback(LSHandle *sh, LSMessage *message, void *ctx)
{
    _update_timeouts();
    return true;
}

static int
_alarms_timeout_init(void)
{
    bool retVal;

    if (gSleepConfig.disable_rtc_alarms)
	{
		SLEEPDLOG(LOG_INFO, "%s: RTC alarms disabled", __FUNCTION__);
		return 0;
	}

    /* Set up database */
    const char* ptr = strrchr( kSysTimeoutDatabaseName, '/' );
    gchar* filename = g_filename_from_utf8( kSysTimeoutDatabaseName, ptr-kSysTimeoutDatabaseName, 0, 0, 0);
    if (!filename) {
        goto error;
    }

    g_mkdir_with_parents(filename, S_IRWXU);
    g_free(filename);

    retVal = smart_sql_open( kSysTimeoutDatabaseName, &timeout_db );
    if( !retVal ) {
        SLEEPDLOG(LOG_ERR,"Failed to open database %s\n",
                kSysTimeoutDatabaseName );
        goto error;
    }

    retVal = smart_sql_exec( timeout_db, kSysTimeoutDatabaseCreateSchema );
    if( !retVal ) {
        SLEEPDLOG(LOG_ERR, "%s could not create database\n", __FUNCTION__);
        goto error;
    }

    retVal = smart_sql_exec( timeout_db, kSysTimeoutDatabaseCreateIndex);
    if( !retVal ) {
        SLEEPDLOG(LOG_ERR, "%s could not create index\n", __FUNCTION__);
        goto error;
    }

    /* Set up luna service */

    psh = GetPalmService();

    LSError lserror;
    LSErrorInit(&lserror);
    if (!LSPalmServiceRegisterCategory(psh,
                "/timeout", timeout_methods /*public*/, NULL /*private*/, NULL, NULL, &lserror)) {
        SLEEPDLOG(LOG_ERR, "%s could not register category: %s",
                __FUNCTION__, lserror.message);
        LSErrorFree(&lserror);
        goto error;
    }

    retVal = LSCall(LSPalmServiceGetPrivateConnection(psh),
            "palm://com.palm.bus/signal/addmatch",
            "{\"category\":\"/com/palm/power\",\"method\":\"resume\"}",
            _resume_callback, NULL, NULL, &lserror);
    if (!retVal) {
    	SLEEPDLOG(LOG_ERR, "%s could not register for suspend resume signal: %s",
                __FUNCTION__, lserror.message);
        LSErrorFree(&lserror);
        goto error;
    }

    retVal = update_rtc(NULL);
    if (!retVal) {
        SLEEPDLOG(LOG_ERR, "%s could not get wall-rtc offset.", __FUNCTION__);
    }
    else {
        SLEEPDLOG(LOG_INFO, "Initial WALL-RTC = %ld", rtc_to_wall);
    }

    GTimerSource *timer_rtc_check = g_timer_source_new_seconds(5*60);
    g_source_set_callback((GSource*)timer_rtc_check,
                    (GSourceFunc)_rtc_check, NULL, NULL);
    g_source_attach((GSource*)timer_rtc_check, GetMainLoopContext());

    sTimerCheck = g_timer_source_new_seconds(60*60);
    g_source_set_callback((GSource*)sTimerCheck,
                    (GSourceFunc)_timer_check, NULL, NULL);
    g_source_attach((GSource*)sTimerCheck, GetMainLoopContext());

    /** To support the deprecated interface */
    int alarm_init(void);
    alarm_init();
    /*************/

    _update_timeouts();

    return 0;

error:
    return -1;
}

INIT_FUNC(INIT_FUNC_END, _alarms_timeout_init);

/* @} END OF NewInterface */

