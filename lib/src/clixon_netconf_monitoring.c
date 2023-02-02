/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2019 Olof Hagsand
  Copyright (C) 2020-2022 Olof Hagsand and Rubicon Communications, LLC(Netgate)

  This file is part of CLIXON.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  Alternatively, the contents of this file may be used under the terms of
  the GNU General Public License Version 3 or later (the "GPL"),
  in which case the provisions of the GPL are applicable instead
  of those above. If you wish to allow use of your version of this file only
  under the terms of the GPL, and not to allow others to
  use your version of this file under the terms of Apache License version 2, 
  indicate your decision by deleting the provisions above and replace them with
  the  notice and other provisions required by the GPL. If you do not delete
  the provisions above, a recipient may use your version of this file under
  the terms of any one of the Apache License version 2 or the GPL.

  ***** END LICENSE BLOCK *****

 * RFC 6022 YANG Module for NETCONF Monitoring
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/time.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include "clixon_log.h"
#include "clixon_queue.h"
#include "clixon_hash.h"
#include "clixon_handle.h"
#include "clixon_yang.h"
#include "clixon_xml.h"
#include "clixon_yang_module.h"
#include "clixon_xml_io.h"
#include "clixon_netconf_lib.h"
#include "clixon_options.h"
#include "clixon_err.h"
#include "clixon_data.h"
#include "clixon_datastore.h"
#include "clixon_netconf_monitoring.h"

static int
per_datastore(clicon_handle h,
              cbuf         *cb,
              const char   *db)
{
    int            retval = -1;
    uint32_t       sid;
    struct timeval tv = {0,};
    char           timestr[28];

    cprintf(cb, "<datastore><name>%s</name>", db);
    if ((sid = xmldb_islocked(h, db)) > 0){
        cprintf(cb, "<locks>");
        cprintf(cb, "<global-lock>");
        cprintf(cb, "<locked-by-session>%u</locked-by-session>", sid);
        xmldb_lock_timestamp(h, db, &tv);
        if (time2str(tv, timestr, sizeof(timestr)) < 0){
            clicon_err(OE_UNIX, errno, "time2str");
            goto done;
        }
        cprintf(cb, "<locked-time>%s</locked-time>", timestr);
        cprintf(cb, "</global-lock>");
        cprintf(cb, "</locks>");
    }
    cprintf(cb, "</datastore>");
    retval = 0;
 done:
    return retval;
}

/*! Get netconf monitoring datastore state
 *
 * @param[in]     h       Clicon handle
 * @param[in]     yspec   Yang spec
 * @param[in,out] cb      CLIgen buffer
 * @retval        0       OK
 * @retval       -1       Error (fatal)
 * @see RFC 6022 Section 2.1.2
 */
static int
netconf_monitoring_datastores(clicon_handle h,
                              yang_stmt  *yspec,
                              cbuf       *cb)
{
    int      retval = -1;

    cprintf(cb, "<datastores>");
    if (per_datastore(h, cb, "running") < 0)
        goto done;
    if (per_datastore(h, cb, "candidate") < 0)
        goto done;
    if (if_feature(yspec, "ietf-netconf", "startup")){
        if (per_datastore(h, cb, "startup") < 0)
            goto done;
    }
    cprintf(cb, "</datastores>");
    retval = 0;
 done:
    return retval;
}

/*! Get netconf monitoring schema state
 *
 * @param[in]     h       Clicon handle
 * @param[in]     yspec   Yang spec
 * @param[in,out] cb      CLIgen buffer
 * @retval        0       OK
 * @retval       -1       Error (fatal)
 * @see RFC 6022 Section 2.1.3
 */
static int
netconf_monitoring_schemas(clicon_handle h,
                           yang_stmt    *yspec,
                           cbuf         *cb)
{
    int        retval = -1;
    yang_stmt *ym = NULL;
    yang_stmt *y1;
    char      *identifier;
    char      *revision;
    char      *dir;

    cprintf(cb, "<schemas>");
    while ((ym = yn_each(yspec, ym)) != NULL) {
        cprintf(cb, "<schema>");
        identifier = yang_argument_get(ym);
        cprintf(cb, "<identifier>%s</identifier>", identifier);
        cprintf(cb, "<version>");
        revision = NULL;
        if ((y1 = yang_find(ym, Y_REVISION, NULL)) != NULL){
            revision = yang_argument_get(y1);
            cprintf(cb, "%s", revision);
        }
        cprintf(cb, "</version>");
        cprintf(cb, "<format>yang</format>");
        cprintf(cb, "<namespace>%s</namespace>", yang_find_mynamespace(ym));
        /* A local implementation may have other locations, how to configure? */
        cprintf(cb, "<location>NETCONF</location>");
        if ((dir = clicon_option_str(h,"CLICON_NETCONF_MONITORING_LOCATION")) != NULL){
            if (revision)
                cprintf(cb, "<location>%s/%s@%s.yang</location>", dir, identifier, revision);
            else
                cprintf(cb, "<location>%s/%s.yang</location>", dir, identifier);
        }
        cprintf(cb, "</schema>");
    }
    cprintf(cb, "</schemas>");
    retval = 0;
    //done:
    return retval;
}

/*! Get netconf monitoring statistics state
 *
 * @param[in]     h       Clicon handle
 * @param[in]     yspec   Yang spec
 * @param[in,out] cb      CLIgen buffer
 * @retval        0       OK
 * @retval       -1       Error (fatal)
 * @see RFC 6022 Section 2.1.5
 */
static int
netconf_monitoring_statistics(clicon_handle h,
                              yang_stmt    *yspec,
                              cbuf         *cb)
{
    int   retval = -1;
    char *str;
    cvec *cvv = NULL;
    cg_var *cv;
        
    cprintf(cb, "<statistics>");
    if (clicon_data_get(h, "netconf-start-time", &str) == 0 &&
        str != NULL){
        cprintf(cb, "<netconf-start-time>%s</netconf-start-time>", str);
    }
    if ((cvv = clicon_data_cvec_get(h, "netconf-statistics")) == NULL)
        goto ok;
    if ((cv = cvec_find(cvv, "in-bad-hellos")) != NULL)
        cprintf(cb, "<in-bad-hellos>%u</in-bad-hellos>", cv_uint32_get(cv));
    if ((cv = cvec_find(cvv, "in-sessions")) != NULL)
        cprintf(cb, "<in-sessions>%u</in-sessions>", cv_uint32_get(cv));
    if ((cv = cvec_find(cvv, "dropped-sessions")) != NULL)
        cprintf(cb, "<dropped-sessions>%u</dropped-sessions>", cv_uint32_get(cv));
    if ((cv = cvec_find(cvv, "in-rpcs")) != NULL)
        cprintf(cb, "<in-rpcs>%u</in-rpcs>", cv_uint32_get(cv));
    if ((cv = cvec_find(cvv, "in-bad-rpcs")) != NULL)
        cprintf(cb, "<in-bad-rpcs>%u</in-bad-rpcs>", cv_uint32_get(cv));
    if ((cv = cvec_find(cvv, "out-rpc-errors")) != NULL)
        cprintf(cb, "<out-rpc-errors>%u</out-rpc-errors>", cv_uint32_get(cv));
    if ((cv = cvec_find(cvv, "out-notifications")) != NULL)
        cprintf(cb, "<out-notifications>%u</out-notifications>", cv_uint32_get(cv));
    cprintf(cb, "</statistics>");
 ok:
    retval = 0;
    // done:
    return retval;
}

/*! Get netconf monitoring state
 *
 * Netconf monitoring state is:
 *   capabilities, datastores, schemas, sessions, statistics
 * @param[in]     h       Clicon handle
 * @param[in]     yspec   Yang spec
 * @param[in]     xpath   XML Xpath
 * @param[in]     nsc     XML Namespace context for xpath
 * @param[in,out] xret    Existing XML tree, merge x into this
 * @param[out]    xerr    XML error tree, if retval = 0
 * @retval        1       OK
 * @retval        0       Statedata callback failed, error in xret
 * @retval       -1       Error (fatal)
 * @see backend_monitoring_state_get
 * @see RFC 6022
 */
int
netconf_monitoring_state_get(clicon_handle h,
                             yang_stmt    *yspec,
                             char         *xpath,
                             cvec         *nsc,
                             cxobj       **xret,
                             cxobj       **xerr)
{
    int    retval = -1;
    cbuf  *cb = NULL;
    int    ret;
    
    if ((cb = cbuf_new()) ==NULL){
        clicon_err(OE_XML, errno, "cbuf_new");
        goto done;
    }
    cprintf(cb, "<netconf-state xmlns=\"%s\">", NETCONF_MONITORING_NAMESPACE);
    if (netconf_capabilites(h, cb) < 0)
        goto done;
    if (netconf_monitoring_datastores(h, yspec, cb) < 0)
        goto done;
    if (netconf_monitoring_schemas(h, yspec, cb) < 0)
        goto done;
    /* sessions is backend-specific */
    if (netconf_monitoring_statistics(h, yspec, cb) < 0)
        goto done;
    cprintf(cb, "</netconf-state>");
    if ((ret = clixon_xml_parse_string(cbuf_get(cb), YB_MODULE, yspec, xret, xerr)) < 0)
        goto done;
    if (ret == 0){
        goto fail;
    }
    retval = 1;
 done:
    clicon_debug(1, "%s %d", __FUNCTION__, retval);
    if (cb)
        cbuf_free(cb);
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Add RFC6022 empty counter32 with zero
 *
 * @param[in]  cvv   Cligen vector
 * @param[in]  name  Name of new counter
 */
static int
stat_counter_add(cvec *cvv,
                 char *name)
{
    int     retval = -1;
    cg_var *cv;

    if ((cv = cvec_add(cvv, CGV_UINT32)) == NULL){
        clicon_err(OE_UNIX, errno, "cvec_add");
        goto done;
    }
    cv_name_set(cv, name);
    cv_uint32_set(cv, 0);
    retval = 0;
 done:
    return retval;
}

/*! Init RFC6022 stats
 *
 * @param[in]  h   Clicon handle
 */
int
netconf_monitoring_statistics_init(clicon_handle h)
{
    int            retval = -1;
    struct timeval tv;
    char           timestr[28];
    cvec          *cvv = NULL;

    gettimeofday(&tv, NULL);
    if (time2str(tv, timestr, sizeof(timestr)) < 0)
        goto done;
    clicon_data_set(h, "netconf-start-time", timestr); /* RFC 6022 */
    if ((cvv = cvec_new(0)) == NULL){
        clicon_err(OE_UNIX, errno, "cvec_new");
        goto done;
    }
    if (clicon_data_cvec_set(h, "netconf-statistics", cvv) < 0)
        goto done;
    if (stat_counter_add(cvv, "in-bad-hellos") < 0)
        goto done;
    if (stat_counter_add(cvv, "in-sessions") < 0)
        goto done;
    if (stat_counter_add(cvv, "dropped-sessions") < 0)
        goto done;
    if (stat_counter_add(cvv, "in-rpcs") < 0)
        goto done;
    if (stat_counter_add(cvv, "in-bad-rpcs") < 0)
        goto done;
    if (stat_counter_add(cvv, "out-rpc-errors") < 0)
        goto done;
    if (stat_counter_add(cvv, "out-notifications") < 0)
        goto done;
    retval = 0;
 done:
    return retval;
}

/*! Increment RFC6022 statistics counter
 *
 * @param[in]  h     Clixon handle
 * @param[in]  name  Name of counter
 */
int
netconf_monitoring_counter_inc(clicon_handle h,
                               char         *name)
{
    int      retval = -1;
    cvec    *cvv = NULL;
    cg_var  *cv;
    uint32_t u32;

    if ((cvv = clicon_data_cvec_get(h, "netconf-statistics")) != NULL){
        if ((cv = cvec_find(cvv, name)) != NULL){
            u32 = cv_uint32_get(cv);
            u32++;
            cv_uint32_set(cv, u32);
        }
    }
    retval = 0;
    return retval;
}
