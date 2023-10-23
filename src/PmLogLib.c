// Copyright (c) 2007-2021 LG Electronics, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// SPDX-License-Identifier: Apache-2.0


// get GNU extensions from dlfcn.h (dladdr)
#define _GNU_SOURCE

#include "PmLogLib.h"
#include "PmLogLibPrv.h"

#include <assert.h>
#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/syslog.h>
#include <sys/shm.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <glib/gprintf.h>
#include <pbnjson.h>

// take advantage of glibc
extern const char*    __progname;
static PmLogContext libProcessContext = kPmLogDefaultContext;

// shared memory segment
static int              lock_fd          = -1;
static uint8_t          *gShmData        = NULL;

// typed pointers to shared memory segment

/*
 * Temporarily suppress the deprecation warnings for this block, as we
 * are the deprecators
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
static PmLogGlobals defaultSet =
{
    .signature = PMLOG_SIGNATURE,
    .reserved = 0,
    .maxUserContexts = PMLOG_MAX_NUM_CONTEXTS,
    .numUserContexts = 0,
    .contextLogging = 0,
    .consoleConf.stdErrMinLevel = kPmLogLevel_Emergency,
    .consoleConf.stdErrMaxLevel = kPmLogLevel_Error,
    .consoleConf.stdOutMinLevel = kPmLogLevel_Warning,
    .consoleConf.stdOutMaxLevel = kPmLogLevel_Debug,
    .devMode = true,
    .globalContext =
    {
       .info =
       {
           .enabledLevel = kPmLogLevel_Info,
           .flags = 0
       },
       .component = kPmLogGlobalContextName
    }
};
#pragma GCC diagnostic pop

static PmLogGlobals    *gGlobalsP = &defaultSet;
static PmLogContext_   *gGlobalContextP = &defaultSet.globalContext;

#define DEBUG_MSG_ID "DBGMSG"
#define TRUNCATED_MSG_SIZE 128

/***********************************************************************
 * gettid
 ***********************************************************************/
pid_t gettid(void)
{
    return (pid_t) syscall(__NR_gettid);
}


//#######################################################################


/***********************************************************************
 * COMPONENT_PREFIX
 ***********************************************************************/
#define COMPONENT_PREFIX    "PmLogLib"

#define LOG_PROCESS_IDS_TAG "logProcessIds"
#define LOG_THREAD_IDS_TAG  "logThreadIds"
#define LOG_TO_CONSOLE_TAG  "logToConsole"
#define LOG_LEVEL_TAG       "level"

#define BUFFER_LEN 1024
#define CONFIG_DIR WEBOS_INSTALL_SYSCONFDIR "/pmlog.d"
#define OVERRIDES_CONF WEBOS_INSTALL_PREFERENCESDIR "/pmloglib/overrides.conf"
#define MSGID_LEN 32
#define PIDSTR_LEN 32

#define VALIDATE_INCOMING_LIBPROCESSCONTEXT

#define MAX_PROGRAM_NAME 256
static bool syslogConnected = false;
static char progName[MAX_PROGRAM_NAME];

void block_signals(sigset_t *old_set)
{
    sigset_t new_set;
    sigfillset(&new_set);
    pthread_sigmask(SIG_SETMASK, &new_set, old_set);
}

void unblock_signals(sigset_t *old_set)
{
    pthread_sigmask(SIG_SETMASK, old_set, NULL);
}

void CallSysLog(const char *context, const int level, const char* pidtid, const char* fmt, ...)
{
    char buffer[1024] = {0,};
    va_list args;
    int index = snprintf(buffer, sizeof(buffer), "%s %s %s ", pidtid, PMLOG_IDENTIFIER, context);
    if(index < 0)
    {
        return;
    }
    va_start (args, fmt);
    g_vsnprintf (buffer + index, sizeof(buffer) - index, fmt, args);
    va_end (args);

    sigset_t old_set;
    block_signals(&old_set);
    syslog(level, "%s", buffer);
    unblock_signals(&old_set);

}

#ifdef DEBUG_ENABLED
    /***********************************************************************
     * DbgPrint
     ***********************************************************************/
    #define DbgPrint(...) \
        {                                                                           \
            fprintf(stdout, COMPONENT_PREFIX __VA_ARGS__);                          \
            CallSysLog(COMPONENT_PREFIX, "[]", __VA_ARGS__)                         \
        }

    #define ErrPrint(context, pidtid, ...) \
        {                                                                           \
            fprintf(stderr, context __VA_ARGS__);                                   \
            fprintf(stderr, "\n");                                                  \
            CallSysLog(context, pidtid, __VA_ARGS__);                               \
        }

    #define WarnPrint(context, pidtid, ...) \
        {                                                                           \
            fprintf(stderr, context __VA_ARGS__);                                   \
            fprintf(stderr, "\n");                                                  \
            CallSysLog(context, pidtid, __VA_ARGS__);                               \
        }
#else
    /***********************************************************************
     * DbgPrint
     ***********************************************************************/
    #define DbgPrint(pidtid, ...)

    /***********************************************************************
    * ErrPrint
    ***********************************************************************/
    #define ErrPrint(context, pidtid, ...) CallSysLog(context, LOG_ERR, pidtid, __VA_ARGS__)

    /***********************************************************************
    * WarnPrint
    ***********************************************************************/
    #define WarnPrint(context, pidtid, ...) CallSysLog(context, LOG_WARNING, pidtid, __VA_ARGS__)
#endif // DEBUG_ENABLED

/***********************************************************************
 * DEBUG_LOGGING
 *
 * Because the PmLog global initialization will occur on first use that
 * means it may occur before PmLogDaemon is running.  In which case we
 * can't do logging or print debugging information as it will get
 * dropped.  To work around that we can direct our local logs to a
 * separate file.
 * ?? Later, consider having this integrated into the main bottleneck,
 * e.g. PrvLogWrite.  We can have a field in the shared memory that
 * is a flag set by PmLogDaemon when it is running.  If logging is done
 * before that we can manually append to either a separate or the main
 * log file.  That way we'll be sure not to drop any messages during
 * boot or if PmLogDaemon doesn't run for some reason.
 * To be revisited after the OE build switch is complete.
 ***********************************************************************/

// don't check in with this on!
//#define DEBUG_LOGGING


#ifdef DEBUG_LOGGING


/*********************************************************************/
/* PrintAppendToFile */
/**
@brief  Logs the specified formatted text to the specified context.
**********************************************************************/
static void PrintAppendToFile(const char* filePath, const char* fmt, ...)
{
    va_list args;
    FILE*    f;

    va_start(args, fmt);

    f = fopen(filePath, "a");
    if (f != NULL)
    {
        fprintf(f, "%s: ", __progname);

        vfprintf(f, fmt, args);

        (void) fclose(f);
    }

    va_end(args);
}


/***********************************************************************
 * DbgPrint
 ***********************************************************************/
#undef DbgPrint
#define DbgPrint(...) \
    {                                                            \
        const char* path = WEBOS_INSTALL_LOGDIR "/pmlog.log";                \
        PrintAppendToFile(path, COMPONENT_PREFIX __VA_ARGS__);    \
    }

/***********************************************************************
 * ErrPrint
 ***********************************************************************/
#undef ErrPrint
#define ErrPrint    DbgPrint


#endif // DEBUG_LOGGING


/***********************************************************************
 * GetPidStr
 *
 * Return a formatted string containing the process and thread ids
 * according to the context flags.
 ***********************************************************************/
void GetPidStr(PmLogContext_ *context,char *ptidStr,long int ptidStrLen)
{
    pid_t    pid;
    pid_t    tid;

    memset(ptidStr, 0, ptidStrLen);

    if ((context->info.flags & kPmLogFlag_LogProcessIds) ||
        (context->info.flags & kPmLogFlag_LogThreadIds)) {
        pid = getpid();
        tid = gettid();
        if (context->info.flags & kPmLogFlag_LogThreadIds &&
            (tid != pid)) {
            snprintf(ptidStr, ptidStrLen, "[%d:%d]", (int) pid,
                (int) tid);
        } else {
            snprintf(ptidStr, ptidStrLen, "[%d]", (int) pid);
        }
    } else {
        (void) g_strlcpy(ptidStr, "[]", ptidStrLen);
    }
}


//#######################################################################

/*********************************************************************
 * validate_msgid
 *
 * Ensure that a provided MSGID meets the following constraints:
 *   The pointer is non-NULL
 *   It is not an empty string
 *   It does not contain a space, '{' or '}'
 *   It consists of less than 32 charaters.
 *
 * Returns kPmLogErr_none on success, else kPmLogErr_InvalidMsgID
 *
 *********************************************************************/
static PmLogErr validate_msgid(const char *msgid, PmLogContext_ *context_ptr)
{
//! This macro can be defined to restrict logging
//! to whitelist logs
#ifndef ENABLE_WHITELIST
    int         msgid_len = 0;
    const char  *ptr_index = NULL;
    char        ptidStr[ PIDSTR_LEN ];

    GetPidStr(context_ptr, ptidStr, sizeof(ptidStr));

    if (!msgid) {
        ErrPrint(context_ptr->component,
                 ptidStr,
                 "NULL_MSGID {} NULL MSGID provided for non-debug log");
        return kPmLogErr_InvalidMsgID;
    }

    // Check each character in msgid against our list of invalid ones.
    for (ptr_index = msgid, msgid_len = 0; *ptr_index != '\0'; ++ptr_index, ++msgid_len) {
        if (msgid_len >= MSGID_LEN) {
            ErrPrint(context_ptr->component,
                     ptidStr,
                     "LONG_MSGID {\"MSGID\":\"%s\"} MSGID's length is restricted within 32 characters",
                     msgid);
            return kPmLogErr_InvalidMsgID;
        }

        if (strchr(" {}", *ptr_index) != NULL) {
            ErrPrint(context_ptr->component,
                     ptidStr,
                     "INVALID_MSGID {\"MSGID\":\"%s\"} MSGID contains space, { or }.",
                     msgid);
            return kPmLogErr_InvalidMsgID;
        }
    }

    if (msgid_len == 0) {
        return kPmLogErr_EmptyMsgID;
    }
#endif
    return kPmLogErr_None;
}

/***********************************************************************
 * mystrcpy
 *
 * Easy to use wrapper for strcpy to make is safe against buffer
 * overflows and to report any truncations.
 ***********************************************************************/
static void mystrcpy(char* dst, size_t dstSize, const char* src)
{
    size_t    srcLen;

    if (dst == NULL)
    {
        DbgPrint("mystrcpy null dst\n");
        return;
    }

    if (dstSize < 1)
    {
        DbgPrint("mystrcpy invalid dst size\n");
        return;
    }

    dst[ 0 ] = 0;

    if (src == NULL)
    {
        DbgPrint("mystrcpy null src\n");
        return;
    }

    srcLen = strlen(src);
    if (srcLen >= dstSize)
    {
        DbgPrint("mystrcpy buffer overflow\n");
        srcLen = dstSize - 1;
    }

    memcpy(dst, src, srcLen);
    dst[ srcLen ] = 0;
}

/***********************************************************************
 * strtruncate_and_escape
 *
 * Truncate source string to TRUNCATED_MSG_SIZE and return newly
 * allcoated escaped string
 ***********************************************************************/
static inline gchar *strtruncate_and_escape(const char* source)
{
    char buffer[TRUNCATED_MSG_SIZE] = {0};
    strncpy(buffer, source, TRUNCATED_MSG_SIZE - 1);
    return g_strescape(buffer, NULL);
}

/***********************************************************************
 * IntLabel
 *
 * Define a integer value => string label mapping.
 ***********************************************************************/
typedef struct
{
    const char* s;
    int n;
}
IntLabel;


/***********************************************************************
 * PrvGetIntLabel
 *
 * Look up the string label for a given integer value from the given
 * mapping table.  Return NULL if not found.
 ***********************************************************************/
const char* PrvGetIntLabel(const IntLabel* labels, int n);


/***********************************************************************
 * PrvLabelToInt
 *
 * Look up the integer value matching a given string label from the
 * given mapping table.  Return NULL if not found.
 ***********************************************************************/
const int* PrvLabelToInt(const IntLabel* labels, const char* s);


//#######################################################################


/***********************************************************************
 * PrvGetIntLabel
 *
 * Look up the string label for a given integer value from the given
 * mapping table.  Return NULL if not found.
 ***********************************************************************/
const char* PrvGetIntLabel(const IntLabel* labels, int n)
{
    const IntLabel* p;

    for (p = labels; p->s != NULL; p++)
    {
        if (p->n == n)
        {
            return p->s;
        }
    }

    return NULL;
}


/***********************************************************************
 * PrvLabelToInt
 *
 * Look up the integer value matching a given string label from the
 * given mapping table.  Return NULL if not found.
 ***********************************************************************/
const int* PrvLabelToInt(const IntLabel* labels, const char* s)
{
    const IntLabel* p;

    for (p = labels; p->s != NULL; p++)
    {
        if (strcmp(p->s, s) == 0)
        {
            return &p->n;
        }
    }

    return NULL;
}


//#######################################################################


/***********************************************************************
 * kLogLevelLabels
 *
 * Define string labels for the supported logging levels.
 ***********************************************************************/
/*
 * Disable 'deprecatd declaration' warning around this table. We are the
 * ones who deprecated the Emergency, Alert, and Notice levels, but they
 * still need to be in this table.
 */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

static const IntLabel kLogLevelLabels[] =
{
    // use the same labels as the syslog standards
    { "none",        /* -1 */ kPmLogLevel_None },
    //-----------------------
    { "emerg",        /*  0 */ kPmLogLevel_Emergency },
    { "alert",        /*  1 */ kPmLogLevel_Alert     },
    { "crit",        /*  2 */ kPmLogLevel_Critical  },
    { "err",        /*  3 */ kPmLogLevel_Error     },
    { "warning",    /*  4 */ kPmLogLevel_Warning   },
    { "notice",        /*  5 */ kPmLogLevel_Notice    },
    { "info",        /*  6 */ kPmLogLevel_Info      },
    { "debug",        /*  7 */ kPmLogLevel_Debug     },
    //-----------------------
    { NULL,            0 }
};

#pragma GCC diagnostic pop

/***********************************************************************
 * PmLogLevelToString
 *
 * Given a numeric level value, returning the matching symbolic string.
 * -1 (kPmLogLevel_None)      => "none"
 *  0 (kPmLogLevel_Emergency) => "emerg"
 * etc.
 * Returns NULL if the level is not valid.
 ***********************************************************************/
const char* PmLogLevelToString(PmLogLevel level)
{
    const char*    s;

    s = PrvGetIntLabel(kLogLevelLabels, level);
    return s;
}


/***********************************************************************
 * PmLogStringToLevel
 *
 * Given a symbolic level string, returning the matching numeric value.
 * "none"  => -1 (kPmLogLevel_None)
 * "emerg" =>  0 (kPmLogLevel_Emergency)
 * etc.
 * Returns NULL if the level string is not mateched.
 ***********************************************************************/
const int* PmLogStringToLevel(const char* levelStr)
{
    const int*    nP;

    nP = PrvLabelToInt(kLogLevelLabels, levelStr);
    return nP;
}


//#######################################################################


/***********************************************************************
 * kLogFacilityLabels
 *
 * Define string labels for the supported logging facilities.
 ***********************************************************************/
static const IntLabel kLogFacilityLabels[] =
{
    //-----------------------
    { "kern",        /*  (0<<3) */ LOG_KERN     },
    { "user",        /*  (1<<3) */ LOG_USER     },
    { "mail",        /*  (2<<3) */ LOG_MAIL     },
    { "daemon",        /*  (3<<3) */ LOG_DAEMON   },
    { "auth",        /*  (4<<3) */ LOG_AUTH     },
    { "syslog",        /*  (5<<3) */ LOG_SYSLOG   },
    { "lpr",        /*  (6<<3) */ LOG_LPR      },
    { "news",        /*  (7<<3) */ LOG_NEWS     },
    { "uucp",        /*  (8<<3) */ LOG_UUCP     },
    { "cron",        /*  (9<<3) */ LOG_CRON     },
    { "authpriv",    /* (10<<3) */ LOG_AUTHPRIV },
    { "ftp",        /* (11<<3) */ LOG_FTP      },
    //-----------------------
    { "local0",        /* (16<<3) */ LOG_LOCAL0   },
    { "local1",        /* (17<<3) */ LOG_LOCAL1   },
    { "local2",        /* (18<<3) */ LOG_LOCAL2   },
    { "local3",        /* (19<<3) */ LOG_LOCAL3   },
    { "local4",        /* (20<<3) */ LOG_LOCAL4   },
    { "local5",        /* (21<<3) */ LOG_LOCAL5   },
    { "local6",        /* (22<<3) */ LOG_LOCAL6   },
    { "local7",        /* (23<<3) */ LOG_LOCAL7   },
    //-----------------------
    { NULL,            0 }
};


/***********************************************************************
 * PmLogFacilityToString
 *
 * Given a numeric facility value, returning the matching symbolic string.
 * LOG_KERN => "kern"
 * LOG_USER => "user"
 * etc.
 * Returns NULL if the facility is not valid.
 ***********************************************************************/
const char* PmLogFacilityToString(int facility)
{
    const char*    s;

    s = PrvGetIntLabel(kLogFacilityLabels, facility);
    return s;
}


/***********************************************************************
 * PmLogStringToFacility
 *
 * Given a symbolic facility string, returning the matching numeric value.
 * "kern" => LOG_KERN
 * "user" =>  LOG_USER
 * etc.
 * Returns NULL if the facility string is not matched.
 ***********************************************************************/
const int* PmLogStringToFacility(const char* facilityStr)
{
    const int* nP;

    nP = PrvLabelToInt(kLogFacilityLabels, facilityStr);
    return nP;
}


//#######################################################################


/***********************************************************************
 * PrvGetLevelStr
 *
 *
 * Given a numeric level value, returning the matching symbolic string.
 * -1 (kPmLogLevel_None)      => "none"
 *  0 (kPmLogLevel_Emergency) => "emerg"
 * etc.
 * Return "?" if not recognized (should not occur).
 ***********************************************************************/
static const char* PrvGetLevelStr(int level)
{
    const char* s;

    s = PmLogLevelToString(level);
    if (s != NULL)
    {
        return s;
    }

    return "?";
}


/***********************************************************************
 * PrvParseConfigLevel
 *
 * "none" => -1 (kPmLogLevel_None)
 * "err"  => LOG_ERR (kPmLogLevel_Error),
 * etc.
 * Return true if parsed OK, else false.
 ***********************************************************************/
static bool PrvParseConfigLevel(const char* s, int* levelP)
{
    const int* nP;

    nP = PmLogStringToLevel(s);
    if (nP != NULL)
    {
        *levelP = *nP;
        return true;
    }

    *levelP = -1;
    return false;
}

/*********************************************************************/
/* PrvResolveContext */
/**
@brief  Resolve a public PmLogContext pointer to the corresponding
        PmLogContext_ pointer.
        Also, resolve a NULL pointer to the real pointer of the
        global context.
**********************************************************************/
static inline PmLogContext_* PrvResolveContext(PmLogContext context)
{
    return (context == NULL) ? gGlobalContextP : (PmLogContext_*) context;
}

/*********************************************************************/
/* PrvIsGlobalContext */
/**
@brief  Returns true if the specified context pointer is for the
        global context.
**********************************************************************/
static inline bool PrvIsGlobalContext(const PmLogContext_* contextP)
{
    // context should already have been resolved
    assert(contextP != NULL);
    return (contextP == gGlobalContextP);
}

/*********************************************************************/
/* PrvInitContext */
/**
@brief  Read the configuration file for pre-defined contexts and
        context levels.
**********************************************************************/
static bool PrvInitContext(const char* contextName, const char* levelStr,
    char* errMsg, size_t errMsgBuffSize)
{
    PmLogErr        logErr;
    PmLogContext    context;
    PmLogContext_   *contextP;
    int                level;

    errMsg[ 0 ] = 0;

    DbgPrint("defining %s => %s\n", contextName, levelStr);

    level = kPmLogLevel_Debug;
    if (!PrvParseConfigLevel(levelStr, &level))
    {
        mystrcpy(errMsg, errMsgBuffSize, "Failed to parse level");
        return false;
    }

    context = NULL;
    logErr = PmLogGetContext(contextName, &context);
    if (logErr != kPmLogErr_None)
    {
        snprintf(errMsg, errMsgBuffSize, "Error getting context: %s",
            PmLogGetErrDbgString(logErr));
        return false;
    }

    logErr = PmLogSetContextLevel(context, level);
    if (logErr != kPmLogErr_None)
    {
        snprintf(errMsg, errMsgBuffSize, "Error setting context level: %s",
            PmLogGetErrDbgString(logErr));
        return false;
    }

    /* initializing context flags with global values by default */
    contextP = PrvResolveContext(context);
    if (contextP == NULL)
    {
        snprintf(errMsg, errMsgBuffSize, "Error setting context flags: %s",
            PmLogGetErrDbgString(kPmLogErr_ContextNotFound));
        return false;
    }

    if (gGlobalContextP)
    {
        contextP->info.flags = gGlobalContextP->info.flags;
    }
    return true;
}

/*********************************************************************/
/* PrvSetFlag */
/**
@brief  Set or clear the flag bit as indicated.
**********************************************************************/
static void PrvSetFlag(int* flagsP, int flagValue, bool set)
{
    if (set)
    {
        *flagsP = *flagsP | flagValue;
    }
    else
    {
        *flagsP = *flagsP & (~flagValue);
    }
}

/***********************************************************************
 * PrvSetContextFlag
 *
 * @brief                 Sets / resets logging context flag.
 * @param[in] contextP    Pointer to target context.
 * @param[in] flag        Flag to operate on.
 * @param[in] set         Flag value to set or reset.
 * @return................Error code
 ***********************************************************************/
PmLogErr PrvSetContextFlag(PmLogContext_ * contextP, int flag, bool set)
{
    if (contextP == NULL)
    {
        return kPmLogErr_InvalidContext;
    }

    PrvSetFlag(&(contextP->info.flags), flag, set);
    /* we should also mark flags as overridden */
    PrvSetFlag(&(contextP->info.flags), kPmLogFlag_Overridden, true);

    return kPmLogErr_None;
}

static void parse_config_flags(jvalue_ref j_context, const gchar *file_name, const char *context_name)
{
    jvalue_ref    value;
    bool          ret;
    bool          flag_value;
    int           flags = 0;
    PmLogContext  context;
    PmLogContext_ *context_ptr;
    int           err;
    char          ptidStr[ PIDSTR_LEN ] = "";

    err = PmLogGetContext(context_name, &context);
    if (kPmLogErr_None != err) {
        DbgPrint("FLG_CONTEXT_ERR {\"file\":\"%s\",\"context\":\"%s\"}",
                 file_name, context_name);
        return;
    }

    context_ptr = PrvResolveContext(context);

    if (!context_ptr) {
        ErrPrint(COMPONENT_PREFIX, ptidStr, "FLG_RSVL_ERR {\"file\":\"%s\",\"context\":\"%s\"}",
                 file_name, context_name);
        return;
    }

    GetPidStr(context_ptr, ptidStr, sizeof(ptidStr));

    ret = jobject_get_exists(j_context, j_cstr_to_buffer(LOG_PROCESS_IDS_TAG), &value);
    if (ret) { //found logProcessIds
        if (CONV_OK == jboolean_get(value, &flag_value)) {
            if (true == flag_value)
                flags |= kPmLogFlag_LogProcessIds;
            else
                flags &= ~kPmLogFlag_LogProcessIds;
        } else {
            ErrPrint(COMPONENT_PREFIX, ptidStr, "INV_PSID {\"file\":\"%s\",\"context\":\"%s\"}",
                     file_name, context_name);
        }
    }

    ret = jobject_get_exists(j_context, j_cstr_to_buffer(LOG_THREAD_IDS_TAG), &value);
    if (ret) { //found logThreadIds
        if (CONV_OK == jboolean_get(value, &flag_value)) {
            if (true == flag_value)
                flags |= kPmLogFlag_LogThreadIds;
            else
                flags &= ~kPmLogFlag_LogThreadIds;
        } else {
            ErrPrint(COMPONENT_PREFIX, ptidStr, "INV_THID {\"file\":\"%s\",\"context\":\"%s\"}",
                     file_name, context_name);
        }
    }

    ret = jobject_get_exists(j_context, j_cstr_to_buffer(LOG_TO_CONSOLE_TAG), &value);
    if (ret) { //found logToConsole
        if (CONV_OK == jboolean_get(value, &flag_value)) {
            if (true == flag_value)
                flags |= kPmLogFlag_LogToConsole;
            else
                flags &= ~kPmLogFlag_LogToConsole;
        } else {
            ErrPrint(COMPONENT_PREFIX, ptidStr, "INV_LOG_TO_CON {\"file\":\"%s\",\"context\":\"%s\"}",
                     file_name, context_name);
        }
    }

    if (!flags)
        return;

    //Set all the flags at once
    err = PrvSetContextFlag(context_ptr, flags, true);
    if (err != kPmLogErr_None) {
        ErrPrint(COMPONENT_PREFIX, ptidStr, "SET_CTX_FLG_ERR {\"file\":\"%s\",\"context\":\"%s\"}",
                 file_name, context_name);
    }
}

static bool parse_config_overrides(jvalue_ref j_overrides, const gchar *file_name)
{
    for (ssize_t i = 0; i < jarray_size(j_overrides); i++) {
        jvalue_ref j_override = jarray_get(j_overrides, i);
        if (!jis_object(j_override)) {
            ErrPrint(COMPONENT_PREFIX, "[]", "PARSE_ERROR {\"file\":\"%s\",\"index\":%zd} Invalid override (ignoring)",
                     file_name, i);
            continue;
        }

        jvalue_ref j_value;
        raw_buffer name;
        raw_buffer level_str;
        int           level;
        bool       valid_level = false;
        if (jobject_get_exists(j_override, J_CSTR_TO_BUF("name"), &j_value)) {
            name = jstring_get(j_value);
        } else { // global overrides
            name = j_str_to_buffer(NULL, 0);
        }

        if (jobject_get_exists(j_override, j_cstr_to_buffer(LOG_LEVEL_TAG), &j_value)) {
            level_str = jstring_get(j_value);
            valid_level = PrvParseConfigLevel(level_str.m_str, &level);
            if (!valid_level) {
                ErrPrint(COMPONENT_PREFIX, "[]", "PARSE_ERROR {\"file\":\"%s\",\"index\":%zu} Invalid log level \"%s\" (ignoring)",
                         file_name, i, level_str.m_str);
            }
        } else { // global overrides
            level_str = j_str_to_buffer(NULL, 0);
        }

        if (valid_level) {
            PmLogErr log_err;
            const char *context_name;
            if (name.m_str == NULL) {
                int contexts_count;
                context_name = "<all>";
                log_err = PmLogGetNumContexts(&contexts_count);
                if (log_err == kPmLogErr_None) {
                    PmLogContext context;
                    for (int n = 0; n < contexts_count; ++n) {
                        log_err = PmLogGetIndContext(n, &context);
                        if (log_err != kPmLogErr_None) break;

                        log_err = PmLogSetContextLevel(context, level);

                        if (log_err != kPmLogErr_None) {
                            // remember at which context we've met an error
                            context_name = PrvResolveContext(context)->component;
                            break;
                        }
                    }
                }
            } else {
                PmLogContext context;
                context_name = name.m_str;
                log_err = PmLogGetContext(context_name, &context);
                if (log_err == kPmLogErr_None) {
                    log_err = PmLogSetContextLevel(context, level);
                }
            }
            if (log_err != kPmLogErr_None) {
                ErrPrint(COMPONENT_PREFIX, "[]", "SET_CTX_LEVEL_FAIL {\"file\":\"%s\", \"index\":%zd} Failed to set log level for %s: %s",
                         file_name, i, context_name, PmLogGetErrDbgString(log_err));
            }
        }

        jstring_free_buffer(name);
        jstring_free_buffer(level_str);
    }
    return true;
}

static bool parse_json_file(const char *file_name)
{
    int         index;
    bool        found_context = false;
    bool        have_valid_overrides = false;
    JSchemaInfo schemainfo;
    jvalue_ref  parsed;
    jvalue_ref  contexts_array;
    bool        ret;
    jvalue_ref  value;

    jschema_info_init(&schemainfo, jschema_all(), NULL, NULL);
    parsed = jdom_parse_file(file_name, &schemainfo, DOMOPT_INPUT_NOCHANGE);
    if (jis_null(parsed)) {
        j_release(&parsed);
        ErrPrint(COMPONENT_PREFIX, "[]", "JSON_PARSE_ERR {\"file\":\"%s\"}", file_name);
        return false;
    }

        if(g_str_has_suffix(file_name, "default.conf")) {
                if (jobject_get_exists(parsed, j_cstr_to_buffer("contextLogging"), &value)) {
                        bool flag;
                        if (CONV_OK == jboolean_get(value, &flag)) {
                                gGlobalsP->contextLogging = flag;
                        }
                        else {
                                ErrPrint(COMPONENT_PREFIX, "[]", "INV_CTXFLAG {\"file\":\"%s\"}", file_name);
                        }
                }
        }

    ret = jobject_get_exists(parsed, j_cstr_to_buffer("contexts"), &contexts_array);
    if (ret) { // found contexts array
        for (index = 0; index < jarray_size(contexts_array); index++) {

            jvalue_ref j_context;

            j_context = jarray_get(contexts_array, index);
            if (!jis_null(j_context)) {

                raw_buffer    name;
                raw_buffer    level;
                char          err_msg[80];

                name.m_str = NULL;
                level.m_str = NULL;

                ret = jobject_get_exists(j_context, j_cstr_to_buffer("name"), &value);
                if (ret) { //found name
                    found_context = true;
                    name = jstring_get(value);
                    if (!name.m_str) {
                        ErrPrint(COMPONENT_PREFIX, "[]", "CTX_NAME_ERR {\"index\":%d,\"file\":\"%s\"}",
                                 index, file_name);
                        goto context_end;
                    }
                } else {
                    ErrPrint(COMPONENT_PREFIX, "[]", "NO_CTX_NAME {\"index\":%d,\"file\":\"%s\"}",
                             index, file_name);
                    goto context_end;
                }

                ret = jobject_get_exists(j_context, j_cstr_to_buffer(LOG_LEVEL_TAG), &value);
                if (ret) { //found level
                    level = jstring_get(value);
                    if (!level.m_str) {
                        ErrPrint(COMPONENT_PREFIX, "[]", "NO_CTX_LVL {\"context\":\"%s\",\"file\":\"%s\"}",
                                 name.m_str, file_name);
                        goto context_end;
                    }
                } else {
                    ErrPrint(COMPONENT_PREFIX, "[]", "CTX_LVL_MISSING {\"context\":\"%s\",\"file\":\"%s\"}",
                             name.m_str, file_name);
                    goto context_end;
                }

                if (!PrvInitContext(name.m_str, level.m_str, err_msg, sizeof(err_msg))) {
                    DbgPrint("PrvInitContext failed for %s:%s: %s\n",
                             file_name, name.m_str, err_msg);
                    ErrPrint(COMPONENT_PREFIX, "[]", "INIT_CTX_ERR {\"file\":\"%s\",\"context\":\"%s\",\"err\":\"%s\"}",
                             file_name, name.m_str, err_msg);
                        goto context_end;
                }

                // parse optional flags (Ex. LOG_PROCESS_IDS_TAG) for the given context
                parse_config_flags(j_context, file_name, name.m_str);

            context_end:
                jstring_free_buffer(name);
                jstring_free_buffer(level);
            } // if current entry in CONTEXTS array is valid
        } // for loop for traversing CONTEXTS array
    } // if we found a CONTEXTS array

    jvalue_ref overrides;
    ret = jobject_get_exists(parsed, j_cstr_to_buffer("overrides"), &overrides);
    if (ret && parse_config_overrides(overrides, file_name)) {
        have_valid_overrides = true;
    }

    j_release(&parsed);

    if (!found_context && !have_valid_overrides) {
        ErrPrint(COMPONENT_PREFIX, "[]", "CTX_MISSING {\"file\":\"%s\"}", file_name);
        return false;
    }

    return true;
}

#define DEFAULT_CONFIG "default.conf"
bool PmLogPrvReadConfigs(bool (*fn_ptr) (const char *file_name))
{
    GDir *dir;
    GError *error = NULL;
    const char *file_name;
    gchar *full_path = NULL;
    bool found_default_conf = false;

    dir = g_dir_open(CONFIG_DIR, 0, &error);
    if (!dir) {
        ErrPrint(COMPONENT_PREFIX, "[]", "DIR_OPEN_ERR {\"Error\":\"%s\"}", error->message);
        g_error_free(error);
        return false;
    }

    // default.conf should be the first one
    full_path = g_build_filename(CONFIG_DIR, DEFAULT_CONFIG, NULL);
    if (g_file_test(full_path, G_FILE_TEST_IS_REGULAR)) {
        found_default_conf = true;
        // execute caller supplied function
        fn_ptr(full_path);
    }
    g_free(full_path);
    full_path = NULL;

//! This macro can be defined to restrict logging
//! to whitelist logs
#ifndef ENABLE_WHITELIST // Parse only default.conf in production
    if (gGlobalsP->contextLogging)  // Do not parse individual context conf files if context logging is disabled in development builds
    {
        while ((file_name = g_dir_read_name(dir))) {
            // ignore hidden files or files without ".conf" suffix
            if ('.' == file_name[0] ||
                !g_str_has_suffix (file_name, ".conf")) {
                continue;
            }

            if (!g_strcmp0(file_name, DEFAULT_CONFIG)) {
                // we parsed this file already
                continue;
            }

            // Construct full path for parsing selected conf file
            // Ex: /etc/pmlog.d/default.conf = /etc/pmlog.d + default.conf
            full_path = g_build_filename(CONFIG_DIR, file_name, NULL);

            // execute caller supplied function
            fn_ptr(full_path);

            g_free(full_path);
        }
    }
#endif

    g_dir_close(dir);

    // Last phase - overrides.
    // It should be always last as long as override for all components walks
    // through already-defined-contexts.
    if (g_file_test(OVERRIDES_CONF, G_FILE_TEST_IS_REGULAR)) {
        // execute caller supplied function
        fn_ptr(OVERRIDES_CONF);
    }

    return found_default_conf;
}

/*********************************************************************/
/* kHexChars */
/**
@brief  Lookup for hex nybble output.
**********************************************************************/
static const char kHexChars[16] =
{
    '0', '1', '2', '3', '4', '5', '6', '7',
    '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'
};

/*********************************************************************/
/* init_function */
/**
@brief  Library constructor executes automatically in the loading
    process before any other library API is called.
**********************************************************************/
static void __attribute ((constructor)) init_function(void)
{
    const char* kPmLogLibSoFilePath = WEBOS_INSTALL_LIBDIR "/libPmLogLib.so";

    key_t       key;
    int         shmid;
    char*       data;
    size_t      shmSize;
    bool        needInit;
    Dl_info     dlInfo;
    int         result;
    const char* libFilePath;
    mode_t      mode;
    PmLogContext_* theContextP = NULL;

    // get/create the PmLogLib lock

    DbgPrint("Opening lock\n");

    mode = umask(0);
        lock_fd = open("/dev/shm/pmloglib.lock", O_CREAT|O_RDWR , 0666);
    umask(mode);
    if (lock_fd == -1)
    {
        DbgPrint("open error: %s\n", strerror(errno));
        return;
    }

    // determine this library's file path dynamically
    libFilePath = NULL;

    memset(&dlInfo, 0, sizeof(dlInfo));
    result = dladdr(init_function, &dlInfo);
    if (result)
    {
        libFilePath = dlInfo.dli_fname;
        DbgPrint("libFilePath: %s\n", libFilePath);
    }
    else
    {
        DbgPrint("dladdr err: %s\n", strerror(errno));
    }

    // if lookup failed for some reason, fall back to expected
    if (libFilePath == NULL)
    {
        libFilePath = kPmLogLibSoFilePath;
    }

    DbgPrint("getting shm key\n");

    key = ftok(libFilePath, 'A');
    if (key == -1)
    {
        DbgPrint("ftok error: %s\n", strerror(errno));
        return;
    }

    // lock the globals
    PmLogPrvLock();

    shmSize = sizeof(PmLogGlobals);
    DbgPrint("Getting shm size=%u\n", shmSize);

    // if shm is guaranteed initialized to 0, we can use
    // that to tell whether it needs initializing or not.
    // Otherwise, we can do a shmget without the IPC_CREAT,
    // and if returns errno == ENOENT we know it needs creation
    // and initialization.

    shmid = shmget(key, shmSize, 0666 | IPC_CREAT);
    if (shmid == -1)
    {
        DbgPrint("shmget error: %s\n", strerror(errno));
        return;
    }

    data = (char*) shmat(shmid, NULL, 0 /* SHM_RDONLY */);
    if (data == (char*) -1)
    {
        DbgPrint("shmat error: %s\n", strerror(errno));
        return;
    }

    gShmData = (uint8_t*) data;

    // same as gShmData, but typecast for use
    gGlobalsP = (PmLogGlobals*) gShmData;
    gGlobalContextP = &gGlobalsP->globalContext;

    needInit = false;

    // use header as initialization flag
    if (gGlobalsP->signature == 0)
    {
            DbgPrint("initializing shared mem\n");
            memcpy(gGlobalsP, &defaultSet, shmSize);
            //set default library context
            theContextP = &gGlobalsP->userContexts[0];
            gGlobalsP->numUserContexts++;
            mystrcpy(theContextP->component, sizeof(theContextP->component), kPmLogDefaultLibContextName);
            theContextP->info.enabledLevel = kPmLogLevel_Info;
            theContextP->info.flags = 0;
            needInit = true;
        }
    else if (gGlobalsP->signature == PMLOG_SIGNATURE)
    {
        DbgPrint("accessing shared mem\n");
    }
    else
    {
        DbgPrint("unrecognized shared mem\n");

        gGlobalsP = NULL;
        gGlobalContextP = NULL;
    }

    // release the globals lock
    PmLogPrvUnlock();

    // initialize contexts if this is the first time
    if (needInit)
    {
        PmLogPrvReadConfigs(parse_json_file);
    }
}


/*********************************************************************/
/* PmLogPrvGlobals */
/**
@brief  Returns the pointer to the globals.
**********************************************************************/
PmLogGlobals* PmLogPrvGlobals(void)
{
    return gGlobalsP;
}


/*********************************************************************/
/* PmLogPrvLock */
/**
@brief  Acquires the lock for write access to the PmLog shared
        memory context.  This should be held as briefly as possible,
        then released by calling PmLogPrvUnlock.
**********************************************************************/
void PmLogPrvLock(void)
{
    if (lockf(lock_fd, F_LOCK, 0) == -1)
    {
        DbgPrint("lock error: %s\n", strerror(errno));
    }
}


/*********************************************************************/
/* PmLogPrvUnlock */
/**
@brief  Releases the lock for write access to the PmLog shared
        memory context, as previously acquired via PmLogPrvLock.
**********************************************************************/
void PmLogPrvUnlock(void)
{
    if (lockf(lock_fd, F_ULOCK, 0) == -1)
    {
        DbgPrint("unlock error: %s\n", strerror(errno));
    }
}


/*********************************************************************/
/* PrvExportContext */
/**
@brief  Convert a private PmLogContext_ pointer to the corresponding
        public PmLogContext pointer.
**********************************************************************/
static inline PmLogContext PrvExportContext(const PmLogContext_* contextP)
{
    return (contextP == NULL) ? NULL : &contextP->info;
}


/*********************************************************************/
/* PrvIsValidLevel */
/**
@brief  Checks if the level is recognized.
**********************************************************************/
static inline bool PrvIsValidLevel(PmLogLevel level)
{
/*
 * Temporarily suppress the deprecation warnings for this block, as we
 * are the deprecators
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    return
        (level >= kPmLogLevel_Emergency) &&
        (level <= kPmLogLevel_Debug);
#pragma GCC diagnostic pop
}


/*********************************************************************/
/* PrvValidateContextName */
/**
@brief  Returns true if and only if the given string is a valid
        context name, i.e. meets the requirements for length
        and valid characters.
**********************************************************************/
static PmLogErr PrvValidateContextName(const char* contextName)
{
    size_t    n;
    size_t    i;
    char    c;

    // the global context name is a special case
    if (strcmp(contextName, kPmLogGlobalContextName) == 0)
    {
        return kPmLogErr_None;
    }

    if (strcmp(contextName, kPmLogDefaultLibContextName) == 0)
    {
        return kPmLogErr_None;
    }

    n = strlen(contextName);
    if ((n < 1) || (n > PMLOG_MAX_CONTEXT_NAME_LEN))
    {
        return kPmLogErr_InvalidContextName;
    }

    for (i = 0; i < n; i++)
    {
        c = contextName[ i ];

        if ((c >= 'A') && (c <= 'Z'))
            continue;

        if ((c >= 'a') && (c <= 'z'))
            continue;

        if ((c >= '0') && (c <= '9'))
            continue;

        if ((c == '.') ||
            (c == '-') ||
            (c == '_'))
            continue;

        return kPmLogErr_InvalidContextName;
    }

    return kPmLogErr_None;
}


/*********************************************************************/
/* PmLogGetNumContexts */
/**
@brief  Returns the number of defined contexts, including the global
        context.

@return Error code:
            kPmLogErr_None
            kPmLogErr_InvalidParameter
**********************************************************************/
PmLogErr PmLogGetNumContexts(int* pNumContexts)
{
    if (pNumContexts == NULL)
    {
        return kPmLogErr_InvalidParameter;
    }

    *pNumContexts = 0;

    if (gGlobalsP == NULL)
    {
        return kPmLogErr_Unknown;
    }

    *pNumContexts = 1 + gGlobalsP->numUserContexts;
    return kPmLogErr_None;
}


/*********************************************************************/
/* PmLogGetIndContext */
/**
@brief  Returns the context by index where index = 0..numContexts - 1.

@return Error code:
            kPmLogErr_None
            kPmLogErr_InvalidParameter
**********************************************************************/
PmLogErr PmLogGetIndContext(int contextIndex, PmLogContext* pContext)
{
    PmLogContext_*    theContextP;

    if (pContext != NULL)
    {
        *pContext = NULL;
    }

    if ((contextIndex < 0) || (contextIndex > gGlobalsP->numUserContexts))
    {
        return kPmLogErr_InvalidContextIndex;
    }

    if (pContext == NULL)
    {
        return kPmLogErr_InvalidParameter;
    }

    if (contextIndex == 0)
    {
        theContextP = &gGlobalsP->globalContext;
    }
    else
    {
        theContextP = &gGlobalsP->userContexts[ contextIndex - 1 ];
    }

    *pContext = PrvExportContext(theContextP);
    return kPmLogErr_None;
}


/*********************************************************************/
/* PmLogFindContext */
/**
@brief  Returns the logging context for the named context, or
        NULL if the context does not exist.

@return Error code:
            kPmLogErr_None
            kPmLogErr_InvalidParameter
            kPmLogErr_InvalidContext
            kPmLogErr_InvalidContextName
**********************************************************************/
PmLogErr PmLogFindContext(const char* contextName, PmLogContext* pContext)
{
    PmLogErr        logErr;
    int                i;
    PmLogContext_*    theContextP;
    PmLogContext_*    contextP;

    if (pContext == NULL)
    {
        return kPmLogErr_InvalidParameter;
    }

    *pContext = NULL;

    if (gGlobalsP == NULL)
    {
        return kPmLogErr_Unknown;
    }

    if (contextName == NULL)
    {
        return kPmLogErr_InvalidParameter;
    }

    logErr = PrvValidateContextName(contextName);
    if (logErr != kPmLogErr_None)
    {
        return logErr;
    }

    // lock the globals
    PmLogPrvLock();

    theContextP = NULL;

    // look for a match on the context name
    for (i = -1; i < gGlobalsP->numUserContexts; i++)
    {
        if (i == -1)
        {
            contextP = &gGlobalsP->globalContext;
        }
        else
        {
            contextP = &gGlobalsP->userContexts[ i ];
        }

        if (strcmp(contextName, contextP->component) == 0)
        {
            theContextP = contextP;
            break;
        }
    }

    // release the globals lock
    PmLogPrvUnlock();

    if (theContextP != NULL)
    {
        *pContext = PrvExportContext(theContextP);
        return kPmLogErr_None;
    }

    return kPmLogErr_ContextNotFound;
}


/*********************************************************************/
/* PrvGetContextDefaults */
/**
@brief  Look up the default settings for the context.  If the
        component is part of a hierarchy, search up the ancestor
        chain and use the settings of the first ancestor found.
        Otherwise, use the settings from the global context.
**********************************************************************/
static const PmLogContextInfo* PrvGetContextDefaults(const char* contextName)
{
    // Note: this function is called only by PmLogGetContext when
    // the context globals are locked.

    int                        i;
    const PmLogContext_*    contextP;
    char                    parent[ PMLOG_MAX_CONTEXT_NAME_LEN + 1 ];
    char*                    s;

    // copy the context name to a scratch buffer
    mystrcpy(parent, sizeof(parent), contextName);

    for (;;)
    {
        // if there is no 'parent' path, we're done
        s = strrchr(parent, '.');
        if (s == NULL)
        {
            break;
        }

        // else trim off the child name to get the parent
        *s = 0;

        // if a registered context matches the parent path,
        // use its level as the default for the child
        for (i = 0; i < gGlobalsP->numUserContexts; i++)
        {
            contextP = &gGlobalsP->userContexts[ i ];
            if (strcmp(parent, contextP->component) == 0)
            {
                return &contextP->info;
            }
        }
    }

    // otherwise use the global level as the default
    return &gGlobalContextP->info;
}


/*********************************************************************/
/* PmLogGetContext */
/**
@brief  Returns/creates the logging context for the named context.

        If contextName is NULL, returns the global context.

        Context names must be 1..31 characters long, and each
        character must be one of A-Z, a-z, 0-9, '_', '-', '.'.

        Component hierarchies can be indicated using '.' as the path
        separator.
        E.g. "FOO.BAR" would indicate the "BAR" subcomponent
        of the "FOO" component.

@return Error code:
            kPmLogErr_None
            kPmLogErr_InvalidParameter
            kPmLogErr_InvalidContext
            kPmLogErr_InvalidContextName
**********************************************************************/
PmLogErr PmLogGetContext(const char* contextName, PmLogContext* pContext)
{
    PmLogErr                logErr;
    int                        i;
    PmLogContext_*            theContextP;
    PmLogContext_*            contextP;
    const PmLogContextInfo*    defaultsP;

    if (pContext == NULL)
    {
        return kPmLogErr_InvalidParameter;
    }

    *pContext = NULL;

    if (gGlobalsP == NULL)
    {
        return kPmLogErr_Unknown;
    }

    if (contextName == NULL)
    {
        *pContext = PrvExportContext(gGlobalContextP);
        return kPmLogErr_None;
    }

    logErr = PrvValidateContextName(contextName);
    if (logErr != kPmLogErr_None)
    {
        return logErr;
    }

    // lock the globals
    PmLogPrvLock();

    // To meet RFC 3164, we need to use limited '__progname' to protect actual message
    if (!syslogConnected) {
        strncpy(progName, __progname, MAX_PROGRAM_NAME - 1);
        progName[MAX_PROGRAM_NAME - 1] = 0;
        openlog(progName, 0, 0);
        syslogConnected = true;
    }

    theContextP = NULL;
    logErr = kPmLogErr_None;

    // look for a match on the context name
    for (i = -1; i < gGlobalsP->numUserContexts; i++)
    {
        if (i == -1)
        {
            contextP = &gGlobalsP->globalContext;
        }
        else
        {
            contextP = &gGlobalsP->userContexts[ i ];
        }

        if (strcmp(contextName, contextP->component) == 0)
        {
            //DbgPrint("found context %s\n", contextName);
            theContextP = contextP;
            break;
        }
    }

    // if context not found, add it
    if (theContextP == NULL)
    {
        if (gGlobalsP->numUserContexts >= gGlobalsP->maxUserContexts)
        {
            DbgPrint("no more contexts available, fallback to global context\n");
        }
        else
        {
            DbgPrint("adding context %s\n", contextName);
            theContextP = &gGlobalsP->userContexts[ gGlobalsP->numUserContexts ];
            gGlobalsP->numUserContexts++;

            mystrcpy(theContextP->component, sizeof(theContextP->component),
                contextName);

            defaultsP = PrvGetContextDefaults(contextName);

            theContextP->info.enabledLevel = defaultsP->enabledLevel;
            theContextP->info.flags = defaultsP->flags;
        }
    }

    // release the globals lock
    PmLogPrvUnlock();

    if (theContextP != NULL)
    {
        *pContext = PrvExportContext(theContextP);

        return kPmLogErr_None;
    }

    // in case of error, return the global context pointer
    *pContext = PrvExportContext(gGlobalContextP);
    return logErr;
}

static int GetCurrentProcessName(char *dst, int size)
{
    FILE* f = fopen("/proc/self/cmdline", "rt");
    if (f) {
        int read = fread(dst, 1, size-1, f);
        dst[read] = 0;
        for(int i = 0 ; i < read ; i++) {
            if (dst[i] == 0)
                dst[i] = ' ';
        }
        fclose(f);
        return read;
    }
    return 0;
}

#ifdef VALIDATE_INCOMING_LIBPROCESSCONTEXT
void PmLogSetLibContext(PmLogContext libContext)
{
    bool invalid_value = (libContext == NULL);

    if (!invalid_value)
    {
        PmLogPrvLock();

        if (PrvExportContext(&gGlobalsP->globalContext) != libContext)
        {
            bool found = false;
            for (int i = 0; i < gGlobalsP->numUserContexts; i++)
            {
                if (PrvExportContext(&gGlobalsP->userContexts[i]) == libContext) {
                    found = true;
                    break;
                }
            }
            invalid_value = invalid_value || !found;
        }

        PmLogPrvUnlock();
    }

    if (invalid_value) {
        char procName[1024] = {0};
        int length = GetCurrentProcessName(procName, sizeof(procName));
        WarnPrint("UNKNOWN", "[]", "Invalid context was passed to PmLogSetLibContext. Value: %p. Process: %.*s", libContext, length, procName);
    } else {
        libProcessContext = libContext;
    }
}
#else
void PmLogSetLibContext(PmLogContext libContext)
{
    libProcessContext = libContext;
}
#endif

PmLogContext PmLogGetLibContext(void)
{
    if (libProcessContext == kPmLogDefaultContext)
    {
        return PrvExportContext(&gGlobalsP->userContexts[0]);
    }

    return libProcessContext;
}

void PmLogSetDevMode(bool isDevMode)
{
    PmLogPrvLock();
    gGlobalsP->devMode = isDevMode;
    PmLogPrvUnlock();
}


/*********************************************************************/
/* PmLogGetContextInline */
/**
@brief  Returns the logging context for the named context.
**********************************************************************/
PmLogContext PmLogGetContextInline(const char* contextName)
{
    PmLogErr        logErr;
    PmLogContext    context;

    context = NULL;
    logErr = PmLogGetContext(contextName, &context);
    if (logErr == kPmLogErr_None)
    {
        return context;
    }

    return kPmLogGlobalContext;
}


/*********************************************************************/
/* PmLogGetContextName */
/**
@brief  Returns the name of the specified context into the specified
        string buffer.

@return Error code:
            kPmLogErr_None
            kPmLogErr_InvalidParameter
**********************************************************************/
PmLogErr PmLogGetContextName(PmLogContext context, char* contextName,
    size_t contextNameBuffSize)
{
    PmLogContext_*    contextP;

    // clear out result in case of error
    if ((contextName != NULL) && (contextNameBuffSize > 0))
    {
        contextName[ 0 ] = 0;
    }

    contextP = PrvResolveContext(context);
    if (contextP == NULL)
    {
        return kPmLogErr_InvalidContext;
    }

    if ((contextName == NULL) || (contextNameBuffSize <= 1))
    {
        return kPmLogErr_InvalidParameter;
    }

    mystrcpy(contextName, contextNameBuffSize, contextP->component);

    if (contextNameBuffSize < strlen(contextP->component) + 1)
    {
        return kPmLogErr_BufferTooSmall;
    }

    return kPmLogErr_None;
}


/*********************************************************************/
/* PmLogGetContextLevel */
/**
@brief  Gets the logging level for the specified context.
        May be used for the global context.  This should generally not
        be used by the components doing logging themselves.
        Instead just use the PmLogPrint etc. APIs which will do
        inline enabled checks.

@return Error code:
            kPmLogErr_None
            kPmLogErr_InvalidParameter
**********************************************************************/
PmLogErr PmLogGetContextLevel(PmLogContext context, PmLogLevel* levelP)
{
    PmLogContext_*    contextP;

    // clear out result in case of error
    if (levelP != NULL)
    {
        *levelP = kPmLogLevel_Debug;
    }

    contextP = PrvResolveContext(context);
    if (contextP == NULL)
    {
        return kPmLogErr_InvalidContext;
    }

    if (levelP == NULL)
    {
        return kPmLogErr_InvalidParameter;
    }

    *levelP = contextP->info.enabledLevel;

    return kPmLogErr_None;
}


/*********************************************************************/
/* PmLogSetContextLevel */
/**
@brief  Sets the logging level for the specified context.
        May be used for the global context.
**********************************************************************/
PmLogErr PmLogSetContextLevel(PmLogContext context, PmLogLevel level)
{
    PmLogContext_*    contextP;
    int             fd;
        struct flock    fl;

    contextP = PrvResolveContext(context);
    if (contextP == NULL)
    {
        return kPmLogErr_InvalidContext;
    }

    if ((level != kPmLogLevel_None) && !PrvIsValidLevel(level))
    {
        return kPmLogErr_InvalidLevel;
    }

    // dummy reference to avoid unused function warning
    // when DbgPrint is compiled out
    (void) &PrvGetLevelStr;
    DbgPrint("SetContextLevel %s => %s\n", contextP->component,
        PrvGetLevelStr(level));

    // write which process calls this function for debugging
    if(gGlobalsP->devMode)
    {
        fd = open("/tmp/PmLogSetContextLevel.log", O_WRONLY | O_CREAT | O_NOCTTY |O_APPEND | O_NONBLOCK, 0644);
        if (fd >= 0)
        {
            /* get advisory file lock (write => exclusive lock) */
            memset(&fl, 0, sizeof(fl));
            fl.l_type = F_WRLCK;
            if (fcntl(fd, F_SETLKW, &fl) == 0)
            {
                char procName[512] ={0, };
                char debuglog[1024] ={0, };
                GetCurrentProcessName(procName, sizeof(procName));
                g_snprintf(debuglog, sizeof(debuglog), "PROCINFO:%s COMPONENT:%s ORIGINLEVEL:%d INPUTLEVEL:%d\n", procName, contextP->component, contextP->info.enabledLevel, level);
                write(fd, debuglog, strlen(debuglog));
            }

            /* release advisory file lock */
            memset(&fl, 0, sizeof(fl));
            fl.l_type = F_UNLCK;
            if(fcntl(fd, F_SETLKW, &fl)  == -1)
            {
                DbgPrint("fcntl return error. code : %s\n", strerror(errno));
            }

            close(fd);
        }
    }
    contextP->info.enabledLevel = level;
    return kPmLogErr_None;
}


/*********************************************************************/
/* PrvCheckContext */
/**
@brief  Validate the context and check whether logging is enabled.
**********************************************************************/
static PmLogErr PrvCheckContext(const PmLogContext_* contextP,
    PmLogLevel level)
{
    // context should already have been resolved
    assert(contextP != NULL);

    if (!PrvIsValidLevel(level))
    {
        return kPmLogErr_InvalidLevel;
    }

    if (level > contextP->info.enabledLevel)
    {
        return kPmLogErr_LevelDisabled;
    }

    return kPmLogErr_None;
}


/*********************************************************************/
/* PrvLogToConsole */
/**
@brief  Echos the logged info + message to the output.
**********************************************************************/
static void PrvLogToConsole(FILE* out, const char* identStr,
    const char* ptidStr, const char* componentStr, const char* s)
{
    size_t        sLen;
    const char* endStr;

    sLen = strlen(s);

    if ((sLen > 0) && (s[sLen - 1] == '\n'))
    {
        endStr = "";
    }
    else
    {
        endStr = "\n";
    }

    fprintf(out, "%s%s%s%s%s", identStr, ptidStr, componentStr, s, endStr);
}


/***********************************************************************
 * HandleLogLibCommand
 ***********************************************************************/
static bool HandleLogLibCommand(const char* msg)
{
    const char*  kLogLibCmdPrefix    = "!loglib ";
    const size_t kLogLibCmdPrefixLen = 8;
    int contextsNumber, contextIndex;
    PmLogContext  context;
    PmLogContext_ *contextP;
    PmLogErr      logErr;

    if (strncmp(msg, kLogLibCmdPrefix, kLogLibCmdPrefixLen) != 0)
    {
        return false;
    }

    msg += kLogLibCmdPrefixLen;

    if (strcmp(msg, "loadconf") == 0)
    {
        DbgPrint("HandleLogLibCommand: re-loading global config\n");
        PmLogPrvReadConfigs(parse_json_file);

        /* updating all context flags that preserved defaults */
        logErr = PmLogGetNumContexts(&contextsNumber);
        if (logErr != kPmLogErr_None)
        {
            DbgPrint("No contexts found. Error no: %d", logErr);
            return false;
        }
        for (contextIndex = 0; contextIndex < contextsNumber; ++contextIndex)
        {
            logErr = PmLogGetIndContext(contextIndex, &context);
            if(logErr != kPmLogErr_None)
            {
                DbgPrint("Context no %d not found. Error no: %d", contextIndex, logErr);
                continue;
            }
            contextP = PrvResolveContext(context);
            if ((contextP) && (gGlobalContextP) &&
                !(contextP->info.flags & kPmLogFlag_Overridden))
            {
                contextP->info.flags = gGlobalContextP->info.flags;
            }
        }
        return true;
    }

    return false;
}

/*********************************************************************/
/* PrvLogWrite */
/**
@brief  Logs the specified formatted text to the specified context.
**********************************************************************/
static PmLogErr PrvLogWrite(PmLogContext_ *contextP, PmLogLevel level,
        const char *msgid, const char *s)
{
    const char  *identStr;
    char        ptidStr[ PIDSTR_LEN ];
    int         savedErrNo;

    // one character before, 3 after, \0 terminator
    char        componentStr[ 1 + PMLOG_MAX_CONTEXT_NAME_LEN + 3 +1 ];

    // save and restore errno, so logging doesn't have side effects
    savedErrNo = errno;

    if (HandleLogLibCommand(s))
    {
        goto Exit;
    }

    identStr = __progname;

    GetPidStr(contextP, ptidStr, sizeof(ptidStr));

    snprintf(componentStr, sizeof(componentStr), "%s",
        contextP->component);

    sigset_t old_set;
    block_signals(&old_set);
    syslog(level, "%s %s %s %s %s", ptidStr, PMLOG_IDENTIFIER, componentStr, msgid ? msgid : "", s);
    unblock_signals(&old_set);

    if (contextP->info.flags & kPmLogFlag_LogToConsole)
    {
        const PmLogConsole* consoleConfP = &gGlobalsP->consoleConf;

        if (ptidStr[0] == 0)
        {
            mystrcpy(ptidStr, sizeof(ptidStr), ": ");
        }

        if ((level >= consoleConfP->stdErrMinLevel) &&
            (level <= consoleConfP->stdErrMaxLevel))
        {
            PrvLogToConsole(stderr, identStr, ptidStr, componentStr, s);
        }

        if ((level >= consoleConfP->stdOutMinLevel) &&
            (level <= consoleConfP->stdOutMaxLevel))
        {
            PrvLogToConsole(stdout, identStr, ptidStr, componentStr, s);
        }
    }

Exit:
    // save and restore errno, so logging doesn't have side effects
    errno = savedErrNo;

    return kPmLogErr_None;
}

static bool validate_json_string(const char* kvpairs, PmLogErr *logErr, const bool with_tailing)
{

//! This macro can be defined to restrict logging
//! to whitelist logs
#ifndef ENABLE_WHITELIST
    JSchemaInfo  schemaInfo;

    const char *ptr_kvpairs = kvpairs;
    char json_str[BUFFER_LEN] = {0, };
    int next_brace_pos = 0;
    int previous_pos = 0;
    char *search_str = NULL;
    int move_index = 0;

    if (with_tailing) {
        search_str = "} ";
        move_index = 2;
    }
    else {
        search_str = "}";
        move_index = 1;
    }

    // split json string and freetext until parsing is successful
    while (ptr_kvpairs != NULL)
    {
        ptr_kvpairs = strstr(ptr_kvpairs, search_str);
        if (!ptr_kvpairs)
        {
            return false;
        }

        next_brace_pos = ptr_kvpairs - kvpairs + move_index; // index after closing curly braces

        // To prevent buffer overflowing
        if (next_brace_pos > sizeof(json_str) - 1)
        {
            *logErr = kPmLogErr_TooMuchData;
            return false;
        }

        // copy memory for spaces
        memcpy(json_str + previous_pos, kvpairs + previous_pos, next_brace_pos - previous_pos);

        jschema_info_init(&schemaInfo, jschema_all(), NULL, NULL);
        if(jsax_parse(NULL, j_cstr_to_buffer(json_str), &schemaInfo))
        {
            return true;
        }

        // move 2 pointers to find next "} "
        ptr_kvpairs += move_index;
    }
    return false;
#else
    return true;
#endif
}

/*********************************************************************/
/* PmLogString_ */
/**
@brief  Logs the specified string of kv pairs and free text to the
        specified context.
**********************************************************************/
PmLogErr PmLogString_(PmLogContext context, PmLogLevel level,
        const char* msgid, const char* kvpairs, const char* message)
{
    PmLogContext_*  contextP;
    PmLogErr        logErr;
    char            lineStr[BUFFER_LEN] = {0,};
    int             ret;
    char            ptidStr[PIDSTR_LEN];
    const char      *ptr_msgid = msgid;

    contextP = PrvResolveContext(context);
    if (!contextP) {
        return kPmLogErr_InvalidContext;
    }

    logErr = PrvCheckContext(contextP, level);
    if (logErr != kPmLogErr_None) {
        return logErr;
    }

    GetPidStr(contextP, ptidStr, sizeof(ptidStr));

    if (level != kPmLogLevel_Debug) {
        logErr = validate_msgid(msgid, contextP);
        if (kPmLogErr_InvalidMsgID == logErr) {
            return logErr;
        }

        if (kvpairs) {
            if (!validate_json_string(kvpairs, &logErr, false)) {
                gchar *err_str = NULL;
                gchar *escaped_str = strtruncate_and_escape(kvpairs);

                if (logErr == kPmLogErr_TooMuchData)
                {
                    err_str = "The json string exceeded 1024 bytes.";
                }
                else
                {
                    err_str = "The json string is wrong.";
                }

                ErrPrint(contextP->component, ptidStr,
                         "INVALID_JSON {\"MSGID\":\"%s\", \"CAUSE\":\"%s\",\"JSON\":\"%s ...\"}",
                         msgid, err_str, escaped_str);

                g_free(escaped_str);
                return kPmLogErr_InvalidFormat;
            }
        }
    } else { //debug level
        if (msgid) {
            ErrPrint(contextP->component, ptidStr, "DBGLVL_MSGID {\"MSGID\":\"%s\"} %s",
                     msgid,
                     "MSGID should be NULL for debug level");
            return kPmLogErr_InvalidFormat;
        }

        if (kvpairs) {
            ErrPrint(contextP->component, ptidStr, "DBGLVL_KVPAIRS {} %s",
                     "kvpairs should be NULL for DEBUG level");
            return kPmLogErr_InvalidFormat;
        }

        if(!message) {
            ErrPrint(contextP->component, ptidStr, "INVALID_FREESTRING {} ");
            return kPmLogErr_InvalidFormat;
        }

        ptr_msgid = DEBUG_MSG_ID;
    }

    ret = snprintf(lineStr, sizeof(lineStr), "%s %s",
                   kvpairs ? kvpairs : "{}",
                   message ? message : "");
    if (ret < 0) {
        ErrPrint(contextP->component, ptidStr, "SNPRINTF_ERR {\"MSGID\":\"%s\",\"ERROR\":\"%s\"}",
                 msgid, strerror(errno));
        return kPmLogErr_FormatStringFailed;
    } else {
        if (ret >= sizeof(lineStr)) {
            DbgPrint("snprintf truncation\n");
        }
    }

    if (kPmLogErr_EmptyMsgID == logErr) {
        gchar *escaped_str = strtruncate_and_escape(lineStr);
        ErrPrint(contextP->component, ptidStr,
                 "EMPTY_MSGID {\"MESSAGE\":\"%s ...\"} MSGID must not be empty",
                 escaped_str);
        g_free(escaped_str);
        return kPmLogErr_InvalidFormat;
    }

    return PrvLogWrite(contextP, level, ptr_msgid, lineStr);
}

/*********************************************************************/
/* PrvLogVPrint */
/**
@brief  Logs the specified formatted text to the specified context.
**********************************************************************/
static PmLogErr PrvLogVPrint(PmLogContext_* contextP, PmLogLevel level,
    const char* fmt, va_list args)
{
    PmLogErr  logErr;
    char      lineStr[BUFFER_LEN];
    int       n;
    char      ptidStr[ PIDSTR_LEN ];

    GetPidStr(contextP, ptidStr, sizeof(ptidStr));

    if ((fmt == NULL) || (fmt[0] == 0))
    {
        return kPmLogErr_InvalidFormat;
    }

    n = vsnprintf(lineStr, sizeof(lineStr), fmt, args);
    if (n < 0)
    {
        // Deprecated function .....
        //ErrPrint(ptidStr, "vsnprintf error %s\n", strerror(errno));
        logErr = kPmLogErr_FormatStringFailed;
    }
    else
    {
        if (n >= sizeof(lineStr))
        {
            DbgPrint("vsnprintf truncation\n");
        }

        logErr = PrvLogWrite(contextP, level, NULL, lineStr);
    }

    return logErr;
}


/*********************************************************************/
/* PmLogPrint_ */
/**
@brief  Logs the specified formatted text to the specified context.
**********************************************************************/
PmLogErr PmLogPrint_(PmLogContext context, PmLogLevel level,
    const char* fmt, ...)
{
    PmLogContext_*    contextP;
    PmLogErr    logErr;
    va_list     args;

    PmLogContext forced_context = context;
    logErr = PmLogGetContext(LEGACY_LOG, &forced_context);
    if (logErr != kPmLogErr_None) {
        forced_context = context;
        DbgPrint("%s: PmLogGetContext err %d", __func__, logErr);
    }

    contextP = PrvResolveContext(forced_context);
    if (contextP == NULL)
    {
        return kPmLogErr_InvalidContext;
    }

    logErr = PrvCheckContext(contextP, level);
    if (logErr != kPmLogErr_None)
    {
        return logErr;
    }

    va_start(args, fmt);

    logErr = PrvLogVPrint(contextP, level, fmt, args);

    va_end(args);

    return logErr;
}

// ASCII start of heading seperator is used by macros in PmLogMsg.h
#define SOH '\001'
static bool validate_keys(size_t kv_count, const char *check_keywords,
                          const char *context_name, const char *msgid)
{

//! This macro can be defined to restrict logging
//! to whitelist logs
#ifndef ENABLE_WHITELIST
    int current_key;

    if (!check_keywords)
        return false;

    if (!kv_count) {
        ErrPrint(context_name, "[]", "MISSING_KV {\"MSGID\":\"%s\"}",
                 msgid);
        return false;
    }

    for (current_key = 1; *check_keywords != '\0'; check_keywords++) {
        char c = *check_keywords;

        if (SOH == c) {
            // keys are seperated by SOH
            // (start of heading) ASCII char
            current_key++;
        } else if (c < ' ' || c >= 0x7f) {
            ErrPrint(context_name, "[]", "INVALID_KEY {\"MSGID\":\"%s\",\"KEY\":%d,\"INVALID_CHAR\":\"%c\"}",
                     msgid, current_key, c);
            return false;
        } else if ('\\' == c) {
            if ('"' == check_keywords[1] || '\\' == check_keywords[1]) {
                check_keywords++;
            } else {
                ErrPrint(context_name, "[]", "INVALID_KEY {\"MSGID\":\"%s\",\"KEY\":%d,\"INVALID_CHAR\":\"%c\"}",
                         msgid, current_key,
                         check_keywords[1]);
                return false;
            }
        }
    }
#endif
    return true;
}

static bool validate_format(const unsigned int flags, const size_t kv_count, const char *format)
{

//! This macro can be defined to restrict logging
//! to whitelist logs
#ifndef ENABLE_WHITELIST
    size_t count = 0;

    if (!format || !kv_count) {
        return false;
    }

    while (*format) {
        if ('%' == *format) {
            if ('%' == *(format+1)) {
                format++;
            } else {
                count++;
            }
        }
        format++;
    }

    // The count value is "kv_count + 1", if PmLogInfoWithClock API was used.
    if (flags & kPmLogValidateFormatFlag_LogWithClock)
        return (kv_count + 1 == count);
    else
        return (kv_count == count);
#else
    return true;
#endif
}

PmLogErr _PmLogMsgKV(PmLogContext context, PmLogLevel level, unsigned int flags,
                     const char *msgid, size_t kv_count, const char *check_keywords,
                     const char *check_formats, const char *fmt, ...)
{
    PmLogContext_  *context_ptr;
    PmLogErr       err;
    va_list        args;
    int            ret;
    char           final_str[BUFFER_LEN] = {0, };
    char           *ptr_final_str = final_str;
    const char*    empty_kv_pair_str = "{} ";
    const char     *ptr_msgid = msgid;
    char           ptidStr[ PIDSTR_LEN ];
    int            empty_kv_pair_size = 0;

    context_ptr = PrvResolveContext(context);
    if (!context_ptr) {
        return kPmLogErr_InvalidContext;
    }

    err = PrvCheckContext(context_ptr, level);
    if (kPmLogErr_None != err) {
        return err;
    }

    GetPidStr(context_ptr, ptidStr, sizeof(ptidStr));

    if (kPmLogLevel_Debug != level) {

        err = validate_msgid(msgid, context_ptr);
        if (kPmLogErr_InvalidMsgID == err) {
            return err;
        }

        if (kv_count) {
            // make sure number of keys received matches with
            // kv_count
            if (!validate_keys(kv_count, check_keywords, context_ptr->component, msgid)) {
                ErrPrint(context_ptr->component, ptidStr, "MISMATCHED_FMT {\"MSGID\":\"%s\"}",
                         msgid);
                return kPmLogErr_InvalidFormat;
            }

            // check_formats should contain kv_count number of '%' chars
            if (!validate_format(flags, kv_count, check_formats)) {
                ErrPrint(context_ptr->component, ptidStr, "MISMATCHED_FMT {\"MSGID\":\"%s\"}",
                         msgid);
                return kPmLogErr_InvalidFormat;
            }
        }
    } else {
        // for debug level, make sure msgid is NULL and no kv pairs
        if (msgid) {
            ErrPrint(context_ptr->component, ptidStr, "DBGLVL_MSGID {\"MSGID\":\"%s\"} %s",
                     msgid,
                     "MSGID should be NULL for debug level");
            return kPmLogErr_InvalidFormat;
        }

        if (kv_count) {
            ErrPrint(context_ptr->component, ptidStr, "DBGLVL_KVCOUNT {} %s",
                     "kv_count should be 0 for DEBUG level");
            return kPmLogErr_InvalidFormat;
        }

        ptr_msgid = DEBUG_MSG_ID;
    }

    if (kv_count == 0) {
        // Add "{} " when kv_count comes 0 or level is debug
        snprintf(ptr_final_str, sizeof(final_str), "%s", empty_kv_pair_str);
        empty_kv_pair_size = strlen(empty_kv_pair_str);
        ptr_final_str += empty_kv_pair_size;
    }

    va_start(args, fmt);
    ret = vsnprintf(ptr_final_str, sizeof(final_str) - empty_kv_pair_size, fmt, args);
    va_end(args);
    if (ret < 0) {
        ErrPrint(context_ptr->component, ptidStr, "VSNPRN_ERR {\"MSGID\":\"%s\",\"ERR_STR\":\"%s\"}",
                 msgid ? msgid : "NULL",
                 strerror(errno));
        return kPmLogErr_FormatStringFailed;
    } else if (ret >= sizeof(final_str)) {
        gchar *escaped_str = strtruncate_and_escape(ptr_final_str);
        WarnPrint(context_ptr->component, ptidStr,
                  "MSG_TRUNCATED {\"MSGID\":\"%s\",\"CAUSE\":\"Log message exceeded 1024 bytes\",\"TRUNCATED_MSG\":\"%s ...\"}",
                  msgid ? msgid : "NULL", escaped_str);
        g_free(escaped_str);
    }


    if (kPmLogErr_EmptyMsgID == err) {
        gchar *escaped_str = strtruncate_and_escape(ptr_final_str);
        ErrPrint(context_ptr->component, ptidStr,
                 "EMPTY_MSGID {\"MESSAGE\":\"%s ...\"} MSGID must not be empty",
                 escaped_str);
        g_free(escaped_str);
        return kPmLogErr_InvalidFormat;
    }

    if (kPmLogLevel_Debug != level) {
        // validate key-value pairs string is valid json data
        if (kv_count != 0) {
            if (!validate_json_string(final_str, &err, true)) {
                char *err_str = NULL;
                gchar *escaped_str = strtruncate_and_escape(final_str);

                if (err == kPmLogErr_TooMuchData)
                {
                    err_str = "The json string exceeded 1024 bytes.";
                }
                else
                {
                    err_str = "The json string is wrong.";
                }

                ErrPrint(context_ptr->component, ptidStr,
                         "INVALID_JSON {\"MSGID\":\"%s\", \"CAUSE\":\"%s\",\"JSON\":\"%s ...\"}",
                         msgid, err_str, escaped_str);

                g_free(escaped_str);
                return kPmLogErr_InvalidFormat;
            }
        }
    }

    return PrvLogWrite(context_ptr, level, ptr_msgid, final_str);
}

/*********************************************************************/
/* PmLogVPrint_ */
/**
@brief  Logs the specified formatted text to the specified context.

        For efficiency, this API should not be used directly, but
        instead use the wrappers (PmLogVPrint, PmLogVPrintError, ...)
        that bypass the library call if the logging is not enabled.

@return Error code:
            kPmLogErr_None
            kPmLogErr_InvalidContext
            kPmLogErr_InvalidLevel
            kPmLogErr_InvalidFormat
**********************************************************************/
PmLogErr PmLogVPrint_(PmLogContext context, PmLogLevel level,
    const char* fmt, va_list args)
{
    PmLogContext_*    contextP;
    PmLogErr    logErr;

        PmLogContext forced_context = context;
        logErr = PmLogGetContext(LEGACY_LOG, &forced_context);
        if (logErr != kPmLogErr_None) {
                forced_context = context;
                DbgPrint("%s: PmLogGetContext err %d", __func__, logErr);
        }

        contextP = PrvResolveContext(forced_context);
    if (contextP == NULL)
    {
        return kPmLogErr_InvalidContext;
    }

    logErr = PrvCheckContext(contextP, level);
    if (logErr != kPmLogErr_None)
    {
        return logErr;
    }

    logErr = PrvLogVPrint(contextP, level, fmt, args);

    return logErr;
}


/*********************************************************************/
/* DumpData_OffsetHexAscii */
/**
@brief  Dump the specified data as hex dump with ASCII view too.
        This is similar to the "hexdump -C" output format, but that
        is not a requirement.  One difference is we don't output a
        trailing empty line with the final offset.

    000030c0  02 02 00 00 06 00 00 00  02 06 00 00 06 00 00 41  \
        |...............A|
**********************************************************************/
static PmLogErr DumpData_OffsetHexAscii(PmLogContext_* contextP,
    PmLogLevel level, const void* dataP, size_t dataSize)
{
    const size_t kMaxBytesPerLine = 16;

    const size_t kMaxLineLen = 8 + 2 + kMaxBytesPerLine * 3 + 2 +
        1 + kMaxBytesPerLine + 1;

    const uint8_t*    srcP;
    uint8_t            b;
    size_t            srcOffset;
    char            lineBuff[ kMaxLineLen + 1 ];
    size_t            lineBytes;
    size_t            i;
    char*            lineP;
    PmLogErr        logErr;

    logErr = kPmLogErr_NoData;

    srcP = (const uint8_t*) dataP;
    srcOffset = 0;

    while (srcOffset < dataSize)
    {
        lineBytes = dataSize - srcOffset;
        if (lineBytes > kMaxBytesPerLine)
        {
            lineBytes = kMaxBytesPerLine;
        }

        snprintf(lineBuff, sizeof(lineBuff), "%08zX", srcOffset);

        lineP = lineBuff + 8;
        *lineP++ = ' ';
        *lineP++ = ' ';

        for (i = 0; i < kMaxBytesPerLine; i++)
        {
            if (i == 8)
            {
                *lineP++ = ' ';
            }

            if (i < lineBytes)
            {
                b = srcP[ i ];
                *lineP++ = kHexChars[(unsigned)(b >> 4) & 0x0F ];
                *lineP++ = kHexChars[(unsigned)b & 0x0F ];
            }
            else
            {
                *lineP++ = ' ';
                *lineP++ = ' ';
            }

            *lineP++ = ' ';
        }

        *lineP++ = ' ';

        *lineP++ = '|';

        for (i = 0; i < lineBytes; i++)
        {
            b = srcP[ i ];
            if (!((b >= 0x20) && (b <= 0x7E)))
            {
                b = '.';
            }
            *lineP++ = (char) b;
        }

        *lineP++ = '|';

        // sanity check that the buffer was sized correctly
        assert((lineBytes < kMaxBytesPerLine) || ((lineP - lineBuff) == (ptrdiff_t) kMaxLineLen));
        *lineP = 0;

        logErr = PrvLogWrite(contextP, level, NULL, lineBuff);
        if (logErr != kPmLogErr_None)
        {
            break;
        }

        srcP += lineBytes;
        srcOffset += lineBytes;
    }

    return logErr;
}


/*********************************************************************/
/* PmLogDumpData_ */
/**
@brief  Logs the specified binary data as text dump to the specified context.
        Specify kPmLogDumpFormatDefault for the formatting parameter.
        For efficiency, this API should not be used directly, but
        instead use the wrappers (PmLogDumpData, ...) that
        bypass the library call if the logging is not enabled.
**********************************************************************/
PmLogErr PmLogDumpData_(PmLogContext context, PmLogLevel level,
    const void* data, size_t numBytes, const PmLogDumpFormat* format)
{

    PmLogContext_*    contextP;
    PmLogErr        logErr;
    const uint8_t*    pData;

    contextP = PrvResolveContext(context);
    if (contextP == NULL)
    {
        return kPmLogErr_InvalidContext;
    }

    logErr = PrvCheckContext(contextP, level);
    if (logErr != kPmLogErr_None)
    {
        return logErr;
    }

    if (numBytes == 0)
    {
        return kPmLogErr_NoData;
    }

    pData = (const uint8_t*) data;
    if (pData == NULL)
    {
        return kPmLogErr_InvalidData;
    }

    //?? TO DO: allow specifying format
    if (format != kPmLogDumpFormatDefault)
    {
        return kPmLogErr_InvalidFormat;
    }

    logErr = DumpData_OffsetHexAscii(contextP, level, data, numBytes);

    return logErr;
}


/***********************************************************************
 * PmLogGetErrDbgString
 *
 * Given the numeric error code value, returning a matching symbolic
 * string.  For debugging only, never to appear in user interface!
 ***********************************************************************/
const char* PmLogGetErrDbgString(PmLogErr logErr)
{
    #define DEFINE_ERR_STR(e)    \
        case kPmLogErr_##e: return #e

    switch (logErr)
    {
        /*   0 */ DEFINE_ERR_STR( None );
        //---------------------------------------------
        /*   1 */ DEFINE_ERR_STR( InvalidParameter );
        /*   2 */ DEFINE_ERR_STR( InvalidContextIndex );
        /*   3 */ DEFINE_ERR_STR( InvalidContext );
        /*   4 */ DEFINE_ERR_STR( InvalidLevel );
        /*   5 */ DEFINE_ERR_STR( InvalidFormat );
        /*   6 */ DEFINE_ERR_STR( InvalidData );
        /*   7 */ DEFINE_ERR_STR( NoData );
        /*   8 */ DEFINE_ERR_STR( TooMuchData );
        /*   9 */ DEFINE_ERR_STR( LevelDisabled );
        /*  10 */ DEFINE_ERR_STR( FormatStringFailed );
        /*  11 */ DEFINE_ERR_STR( TooManyContexts );
        /*  12 */ DEFINE_ERR_STR( InvalidContextName );
        /*  13 */ DEFINE_ERR_STR( ContextNotFound );
        /*  14 */ DEFINE_ERR_STR( BufferTooSmall );
        /*  15 */ DEFINE_ERR_STR( InvalidMsgID );
        /*  16 */ DEFINE_ERR_STR( EmptyMsgID );
        /*  17 */ DEFINE_ERR_STR( LoggingDisabled );
        //---------------------------------------------
        /* 999 */ DEFINE_ERR_STR( Unknown );
    }

    #undef DEFINE_ERR_STR

    // default
    return "?";
}


/*********************************************************************/
/* PmLogPrvTestReadMem */
/**
@brief  This is a private function to be used only by PmLog components
        for test and development purposes.
**********************************************************************/
static PmLogErr PmLogPrvTestReadMem(void* data)
{
    const unsigned long* p;
    unsigned long n;

    p = (const unsigned long*) data;
    printf("PmLogPrvTestReadMem 0x%08lX...\n", (unsigned long) p);
    n = *p;
    printf("PmLogPrvTestReadMem result = 0x%08lX...\n", n);

    return kPmLogErr_None;
}


/*********************************************************************/
/* PmLogPrvTest */
/**
@brief  This is a private function to be used only by PmLog components
        for test and development purposes.
**********************************************************************/
PmLogErr PmLogPrvTest(const char* cmd, void* data)
{
    if (strcmp(cmd, "ReadMem") == 0)
    {
        return PmLogPrvTestReadMem(data);
    }

    return kPmLogErr_InvalidParameter;
}
