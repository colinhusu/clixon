/*
 *
  Copyright (C) 2009-2016 Olof Hagsand and Benny Holmgren

  This file is part of CLIXON.

  CLIXON is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 3 of the License, or
  (at your option) any later version.

  CLIXON is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with CLIXON; see the file LICENSE.  If not, see
  <http://www.gnu.org/licenses/>.

 *
 * Errors may be syslogged using LOG_ERR, and printed to stderr, as controlled
 * by clicon_log_init
 * global error variables are set:
 *  clicon_errno, clicon_suberrno, clicon_err_reason.
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <syslog.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <sys/types.h>

#include "clixon_log.h"
#include "clixon_queue.h"
#include "clixon_chunk.h"
#include "clixon_err.h"

/*
 * Types
 */
struct errvec{
    char *ev_str;
    int   ev_err;
};

struct err_state{
    int  es_errno;
    int  es_suberrno;
    char es_reason[ERR_STRLEN];
};

/*
 * Variables
 */
int clicon_errno  = 0;    /* See enum clicon_err */
int clicon_suberrno  = 0; /* Corresponds to errno.h */
char clicon_err_reason[ERR_STRLEN] = {0, };

/*
 * Error descriptions. Must stop with NULL element.
 */
static struct errvec EV[] = {
    {"Database error",         OE_DB},
    {"Demon error",            OE_DEMON},
    {"Event error",            OE_EVENTS},
    {"Config error",           OE_CFG},
    {"Protocol error",         OE_PROTO},
    {"Regexp error",           OE_REGEX},
    {"UNIX error",             OE_UNIX},
    {"Syslog error",           OE_SYSLOG},
    {"Routing demon error",    OE_ROUTING},
    {"Plugins",                OE_PLUGIN},
    {"Yang error",             OE_YANG},
    {"FATAL",                  OE_FATAL},
    {"Undefined",              OE_UNDEF},
    {NULL,                     -1}
};

static char *
clicon_strerror1(int err, struct errvec vec[])
{
    struct errvec *ev;

    for (ev=vec; ev->ev_err != -1; ev++)
	if (ev->ev_err == err)
	    break;
    return ev?(ev->ev_str?ev->ev_str:"unknown"):"CLICON unknown error";
}

/*! Clear error state and continue.
 *
 * Clear error state and get on with it, typically non-fatal error and you wish to continue.
 */
int
clicon_err_reset(void)
{
    clicon_errno = 0;
    clicon_suberrno = 0;
    memset(clicon_err_reason, 0, ERR_STRLEN);
    return 0;
}

/*! Report an error.
 *
 * Library routines should call this function when an error occurs.
 * The function does he following:
 * - Logs to syslog with LOG_ERR
 * - Set global error variable name clicon_errno
 * - Set global reason string clicon_err_reason
 * @note: err direction (syslog and/or stderr) controlled by clicon_log_init()
 *
 * @param    fn       Inline function name (when called from clicon_err() macro)
 * @param    line     Inline file line number (when called from clicon_err() macro)
 * @param    err      Error number, typically errno
 * @param    suberr   Sub-error number   
 * @param    reason   Error string, format with argv
 */
int
clicon_err_fn(const char *fn, 
	      const int line, 
	      int category, 
	      int suberr, 
	      char *reason, ...)
{
    va_list args;
    int     len;
    char   *msg    = NULL;
    int     retval = -1;

    /* Set the global variables */
    clicon_errno    = category;
    clicon_suberrno = suberr;

    /* first round: compute length of error message */
    va_start(args, reason);
    len = vsnprintf(NULL, 0, reason, args);
    va_end(args);

    /* allocate a message string exactly fitting the message length */
    if ((msg = malloc(len+1)) == NULL){
	fprintf(stderr, "malloc: %s\n", strerror(errno)); /* dont use clicon_err here due to recursion */
	goto done;
    }

    /* second round: compute write message from reason and args */
    va_start(args, reason);
    if (vsnprintf(msg, len+1, reason, args) < 0){
	va_end(args);
	fprintf(stderr, "vsnprintf: %s\n", strerror(errno)); /* dont use clicon_err here due to recursion */
	goto done;
    }
    va_end(args);
    strncpy(clicon_err_reason, msg, ERR_STRLEN-1);

    /* Actually log it */
    if (suberr){
	/* Here we could take care of specific suberr, like application-defined errors */
	clicon_log(LOG_ERR, "%s: %d: %s: %s: %s", 
		   fn,
		   line,
		   clicon_strerror(category),
		   msg,
		   strerror(suberr));
    }
    else
	clicon_log(LOG_ERR, "%s: %d: %s: %s", 
		   fn,
		   line,
		   clicon_strerror(category),
		   msg);

    retval = 0;
  done:
    if (msg)
	free(msg);
    return retval;
}

/*! Translate from numeric error to string representation
 */
char *
clicon_strerror(int err)
{
    return clicon_strerror1(err, EV);
}

/*! Push an error state, if recursive error handling
 */
void*
clicon_err_save(void)
{
    struct err_state *es;

    if ((es = chunk(sizeof(*es), NULL)) == NULL)
	return NULL;
    es->es_errno = clicon_errno;
    es->es_suberrno = clicon_suberrno;
    strncpy(es->es_reason, clicon_err_reason, ERR_STRLEN-1);
    return (void*)es;
}

/*! Pop an error state, if recursive error handling
 */
int
clicon_err_restore(void* handle)
{
    struct err_state *es;

    es = (struct err_state *)handle;
    clicon_errno = es->es_errno;
    clicon_suberrno = es->es_suberrno;
    strncpy(clicon_err_reason, es->es_reason, ERR_STRLEN-1);
    unchunk(es);
    return 0;
}
