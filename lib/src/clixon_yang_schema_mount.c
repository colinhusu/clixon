/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2023 Olof Hagsand

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

 * RFC 8525 Yang schema mount support
 *
 * Structure of mount-points in XML:
 * YANG mount extentsion -->* YANG unknown mount stmt -->* XML mount-points
 *                            |
 *                            cvec mapping xpath->yspec mountpoint
 *
 * The calls into this code are:
 * 1. yang_schema_mount_point() Check that a yang nod eis mount-point
 * 2. xml_yang_mount_get(): from xml_bind_yang and xmldb_put 
 * 3. xml_yang_mount_freeall(): from ys_free1 when deallocating YANG trees
 * 4. yang_schema_mount_statedata(): from get_common/get_statedata to retrieve system state
 * 5. yang_schema_yanglib_parse_mount(): from xml_bind_yang to parse and mount
 * 6. yang_schema_get_child(): from xmldb_put/text_modify when adding new XML nodes
 *
 * Note: the xpath used as key in yang unknown cvec is "canonical" in the sense:
 * - it uses prefixes of the yang spec of relevance
 * - it uses '' not "" in prefixes (eg a[x='foo']. The reason is '' is easier printed in clispecs
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <string.h>
#include <sys/param.h>

/* cligen */
#include <cligen/cligen.h>

/* clixon */
#include "clixon_queue.h"
#include "clixon_hash.h"
#include "clixon_string.h"
#include "clixon_handle.h"
#include "clixon_err.h"
#include "clixon_log.h"
#include "clixon_debug.h"
#include "clixon_yang.h"
#include "clixon_xml.h"
#include "clixon_xml_io.h"
#include "clixon_xml_map.h"
#include "clixon_data.h"
#include "clixon_xpath_ctx.h"
#include "clixon_xpath.h"
#include "clixon_yang_module.h"
#include "clixon_yang_parse_lib.h"
#include "clixon_plugin.h"
#include "clixon_xml_bind.h"
#include "clixon_xml_nsctx.h"
#include "clixon_netconf_lib.h"
#include "clixon_yang_schema_mount.h"

/*! Check if YANG node is a RFC 8525 YANG schema mount
 *
 * Check if:
 * - y is CONTAINER or LIST, AND
 * - y has YANG schema mount "mount-point" as child element, AND
 * - the extension label matches y (see note below)
 * If so, then return 1
 * @param[in] y   Yang statement
 * @retval    1   Yes, y is a RFC 8525 YANG mount-point
 * @retval    0   No, y is not
 * @retval   -1   Error
 * @note That this may be a restriction on the usage of "label". The RFC is somewhat unclear.
 */
int
yang_schema_mount_point0(yang_stmt *y)
{
    int           retval = -1;
    enum rfc_6020 keyw;
    int           exist = 0;
    char         *value = NULL;

    if (y == NULL){
        clixon_err(OE_YANG, EINVAL, "y is NULL");
        goto done;
    }
    keyw = yang_keyword_get(y);
    if (keyw != Y_CONTAINER
#ifndef YANG_SCHEMA_MOUNT_ONLY_PRESENCE_CONTAINERS
        && keyw != Y_LIST
#endif
#if 0 /* See this in some standard YANGs but RFC 8528 does not allow it */
        && keyw != Y_ANYDATA
#endif
        )
        goto fail;
    if (yang_extension_value(y, "mount-point", YANG_SCHEMA_MOUNT_NAMESPACE, &exist, &value) < 0)
        goto done;
    if (exist == 0)
        goto fail;
    if (value == NULL)
        goto fail;
    retval = 1;
 done:
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Cached variant of yang_schema_mount_point
 */
int
yang_schema_mount_point(yang_stmt *y)
{
    return yang_flag_get(y, YANG_FLAG_MTPOINT_POTENTIAL) ? 1 : 0;
}

/*! Get yangspec mount-point
 *
 * @param[in]  y     Yang container/list containing unknown node
 * @param[in]  xpath Key for yspec on y
 * @param[out] yspec YANG stmt spec
 * @retval     0     OK
 */
int
yang_mount_get(yang_stmt  *y,
               char       *xpath,
               yang_stmt **yspec)
{
    cvec   *cvv;
    cg_var *cv;

    clixon_debug(CLIXON_DBG_DEFAULT, "%s %s %p", __FUNCTION__, xpath, y);
    /* Special value in yang unknown node for mount-points: mapping from xpath->mounted yspec */
    if ((cvv = yang_cvec_get(y)) != NULL &&
        (cv = cvec_find(cvv, xpath)) != NULL &&
        yspec)
        *yspec = cv_void_get(cv);
    else
        *yspec = NULL;
    return 0;
}

/*! Set yangspec mount-point on yang node containing extension
 *
 * Mount-points are stored in yang cvec in container/list node taht is a mount-point
 * as defined in yang_schema_mount_point()
 * @param[in]  y      Yang container/list containing unknown node
 * @param[in]  xpath  Key for yspec on y, in canonical form
 * @param[in]  yspec  Yangspec for this mount-point (consumed)
 * @retval     0      OK
 * @retval    -1      Error
 */
int
yang_mount_set(yang_stmt *y,
               char      *xpath,
               yang_stmt *yspec)
{
    int        retval = -1;
    yang_stmt *yspec0;
    cvec      *cvv;
    cg_var    *cv;
    cg_var    *cv2;

    clixon_debug(CLIXON_DBG_DEFAULT, "%s %s %p", __FUNCTION__, xpath, y);
    if ((cvv = yang_cvec_get(y)) != NULL &&
        (cv = cvec_find(cvv, xpath)) != NULL &&
        (yspec0 = cv_void_get(cv)) != NULL){
#if 0 /* Problematic to free yang specs here, upper layers should handle it? */
        ys_free(yspec0);
#endif
        cv_void_set(cv, NULL);
    }
    else if ((cv = yang_cvec_add(y, CGV_VOID, xpath)) == NULL)
        goto done;
    if ((cv2 = cv_new(CGV_STRING)) == NULL){
        clixon_err(OE_YANG, errno, "cv_new");
        goto done;
    }
    if (cv_string_set(cv2, xpath) == NULL){
        clixon_err(OE_UNIX, errno, "cv_string_set");
        goto done;
    }
    /* tag yspec with key/xpath */
    yang_cv_set(yspec, cv2);
    cv_void_set(cv, yspec);
    yang_flag_set(y, YANG_FLAG_MOUNTPOINT); /* Cache value */
    retval = 0;
 done:
    return retval;
}

/*! Get yangspec mount-point
 *
 * @param[in]  h     Clixon handle
 * @param[in]  x     XML mount-point node
 * @param[out] vallevel Do or dont do full RFC 7950 validation if given
 * @param[out] yspec YANG stmt spec of mount-point (if ret is 1)
 * @retval     1     x is a mount-point: yspec may be set
 * @retval     0     x is not a mount point
 * @retval    -1     Error
 */
int
xml_yang_mount_get(clixon_handle   h,
                   cxobj          *xt,
                   validate_level *vl,
                   yang_stmt     **yspec)
{
    int        retval = -1;
    yang_stmt *y;
    char      *xpath0 = NULL;
    int        ret;
    cvec      *nsc0 = NULL;
    yang_stmt *yspec0;
    char      *xpath1 = NULL;
    cvec      *nsc1 = NULL;
    cbuf      *reason = NULL;

    if ((y = xml_spec(xt)) == NULL)
        goto fail;
    if ((ret = yang_schema_mount_point(y)) < 0)
        goto done;
    if (ret == 0)
        goto fail;
    /* Check validate level */
    if (vl && clixon_plugin_yang_mount_all(h, xt, NULL, vl, NULL) < 0)
        goto done;
    if (xml2xpath(xt, NULL, 1, 0, &xpath0) < 0)
        goto done;
    if (xml_nsctx_node(xt, &nsc0) < 0)
        goto done;
    yspec0 = clicon_dbspec_yang(h);
    if ((ret = xpath2canonical(xpath0, nsc0, yspec0, &xpath1, &nsc1, &reason)) < 0)
        goto done;
    if (ret == 0)
        goto fail;
    if (yspec && yang_mount_get(y, xpath1, yspec) < 0)
        goto done;
    retval = 1;
 done:
    if (xpath0)
        free(xpath0);
    if (xpath1)
        free(xpath1);
    if (nsc0)
        cvec_free(nsc0);
    if (nsc1)
        cvec_free(nsc1);
    if (reason)
        cbuf_free(reason);
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Set yangspec mount-point via XML mount-point node
 *
 * Stored in a separate structure (not in XML config tree)
 * @param[in]  x      XML mount-point node
 * @param[in]  yspec  Yangspec for this mount-point (consumed)
 * @retval     0      OK
 * @retval    -1      Error
 */
int
xml_yang_mount_set(clixon_handle h,
                   cxobj        *x,
                   yang_stmt    *yspec)
{
    int        retval = -1;
    yang_stmt *y;
    char      *xpath0 = NULL;
    char      *xpath1 = NULL;
    cvec      *nsc0 = NULL;
    cvec      *nsc1 = NULL;
    yang_stmt *yspec0;
    cbuf      *reason = NULL;
    int        ret;

    if ((y = xml_spec(x)) == NULL){
        clixon_err(OE_YANG, 0, "No yang-spec");
        goto done;
    }
    if (xml2xpath(x, NULL, 1, 0, &xpath0) < 0)
        goto done;
    if (xml_nsctx_node(x, &nsc0) < 0)
        goto done;
    yspec0 = clicon_dbspec_yang(h);
    if ((ret = xpath2canonical(xpath0, nsc0, yspec0, &xpath1, &nsc1, &reason)) < 0)
        goto done;
    if (ret == 0){
        clixon_err(OE_YANG, 0, "%s", cbuf_get(reason));
        goto done;
    }
    if (yang_mount_set(y, xpath1, yspec) < 0)
        goto done;
    retval = 0;
 done:
    if (xpath0)
        free(xpath0);
    if (xpath1)
        free(xpath1);
    if (nsc0)
        cvec_free(nsc0);
    if (nsc1)
        cvec_free(nsc1);
    if (reason)
        cbuf_free(reason);
    return retval;
}

/*! Get any yspec of a mount-point, special function
 *
 * Get (the first) mounted yspec. 
 * A more generic way would be to call plugin_mount to get the yanglib and from that get the
 * yspec. But there is clixon code that cant call the plugin since h is not available
 * @param[in]  y     Yang container/list containing unknown node
 * @param[out] yspec YANG stmt spec
 * @retval     1     yspec found and set
 * @retval     0     Not found
 */
int
yang_mount_get_yspec_any(yang_stmt  *y,
                         yang_stmt **yspec)
{
    cvec   *cvv;
    cg_var *cv;
    void   *p;

    /* Special value in yang unknown node for mount-points: mapping from xpath->mounted yspec */
    if ((cvv = yang_cvec_get(y)) != NULL &&
        (cv = cvec_i(cvv, 0)) != NULL &&
        (p = cv_void_get(cv)) != NULL){
        *yspec = p;
        return 1;
    }
    return 0;
}

/*! Free all yspec yang-mounts
 *
 * @param[in] cvv  Cligen-variable vector containing xpath -> yspec mapping
 * @retval    0    OK
 */
int
yang_mount_freeall(cvec *cvv)
{
    cg_var    *cv = NULL;
    yang_stmt *ys;

    cv = NULL;
    while ((cv = cvec_each(cvv, cv)) != NULL){
        if ((ys = cv_void_get(cv)) != NULL)
            ys_free(ys);
    }
    return 0;
}

/*! Find schema mounts - callback function for xml_apply
 *
 * @param[in]  x    XML node  
 * @param[in]  arg  cvec, if match add node
 * @retval     2    Locally abort this subtree, continue with others
 * @retval     1    Abort, dont continue with others, return 1 to end user
 * @retval     0    OK, continue
 * @retval    -1    Error, aborted at first error encounter, return -1 to end user
 */
static int
find_schema_mounts(cxobj *x,
                   void  *arg)
{
    int        ret;
    yang_stmt *y;
    cvec      *cvv = (cvec *)arg;
    cg_var    *cv;

    if ((y = xml_spec(x)) == NULL)
        return 2;
    if (yang_config(y) == 0)
        return 2;
    if ((ret = yang_schema_mount_point(y)) < 0)
        return -1;
    if (ret == 0)
        return 0;
    if ((cv = cvec_add(cvv, CGV_VOID)) == NULL){
        clixon_err(OE_UNIX, errno, "cvec_add");
        return -1;
    }
    cv_void_set(cv, x);
    return 0;
}

/*! Find mount-points and return yang-library state
 *
 * Brute force: traverse whole XML, match all x that have ymount as yspec
 * Add yang-library state for all x
 * @param[in]     h       Clixon handle
 * @param[in]     xpath   XML Xpath
 * @param[in]     nsc     XML Namespace context for xpath
 * @param[in,out] xret    Existing XML tree, merge x into this
 * @param[out]    xerr    XML error tree, if retval = 0
 * @retval        1       OK
 * @retval        0       Validation failed, error in xret
 * @retval       -1       Error (fatal)
 *
 * RFC 8528 Section 3.4:
 *   A schema for a mount point contained in a mounted module can be
 *   specified by implementing the "ietf-yang-library" and
 *   "ietf-yang-schema-mount" modules in the mounted schema and specifying
 *   the schemas in exactly the same way as the top-level schema.
 * Alt: see snmp_yang2xml to get instances instead of brute force traverse of whole tree
 * @note: Mountpoints must exist in xret on entry
 */
static int
yang_schema_mount_statedata_yanglib(clixon_handle h,
                                    char         *xpath,
                                    cvec         *nsc,
                                    cxobj       **xret,
                                    cxobj       **xerr)
{
    int            retval = -1;
    cvec          *cvv = NULL;
    cg_var        *cv;
    cxobj         *xmp;          /* xml mount-point */
    cxobj         *yanglib = NULL; /* xml yang-lib */
    cbuf          *cb = NULL;
    yang_stmt     *yspec;
    int            ret;
    int            config = 1;
    validate_level vl = VL_FULL;

    if ((cb = cbuf_new()) == NULL){
        clixon_err(OE_UNIX, 0, "clicon buffer");
        goto done;
    }
    if ((cvv = cvec_new(0)) == NULL){
        clixon_err(OE_UNIX, errno, "cvec_new");
        goto done;
    }
    if (xml_apply(*xret, CX_ELMNT, find_schema_mounts, cvv) < 0)
        goto done;
    yspec = clicon_dbspec_yang(h);
    cv = NULL;
    while ((cv = cvec_each(cvv, cv)) != NULL) {
        xmp = cv_void_get(cv);
        yanglib = NULL;
        /* User callback */
        if (clixon_plugin_yang_mount_all(h, xmp, &config, &vl, &yanglib) < 0)
            goto done;
        if (yanglib == NULL)
            continue;
        if ((ret = xml_bind_yang0(h, yanglib, YB_MODULE, yspec, xerr)) < 0)
            goto done;
        if (ret == 0)
            goto fail;
        if (xml_addsub(xmp, yanglib) < 0)
            goto done;
        yanglib = NULL;
    }
    retval = 1;
 done:
    if (cvv)
        cvec_free(cvv);
    if (cb)
        cbuf_free(cb);
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Get schema mount-point state according to RFC 8528
 *
 * @param[in]     h       Clixon handle
 * @param[in]     yspec   Yang spec
 * @param[in]     xpath   XML XPath
 * @param[in]     nsc     XML Namespace context for xpath
 * @param[in,out] xret    Existing XML tree, merge x into this
 * @param[out]    xerr    XML error tree, if retval = 0
 * @retval        1       OK
 * @retval        0       Validation failed, error in xret
 * @retval       -1       Error (fatal)
 * @note  Only "inline" specification of mounted schema supported, not "shared schema"
 */
int
yang_schema_mount_statedata(clixon_handle h,
                            yang_stmt    *yspec,
                            char         *xpath,
                            cvec         *nsc,
                            cxobj       **xret,
                            cxobj       **xerr)
{
    int        retval = -1;
    cbuf      *cb = NULL;
    int        ret;
    yang_stmt *yext;
    yang_stmt *ymount;
    yang_stmt *ymodext;
    yang_stmt *ymod;
    cg_var    *cv;
    cg_var    *cv1;
    char      *label;
    cvec      *cvv;
    cxobj     *x1 = NULL;

    if ((ymodext = yang_find(yspec, Y_MODULE, "ietf-yang-schema-mount")) == NULL ||
        (yext = yang_find(ymodext, Y_EXTENSION, "mount-point")) == NULL){
        goto ok;
        //        clixon_err(OE_YANG, 0, "yang schema mount-point extension not found");
        //        goto done;
    }
    if ((cvv = yang_cvec_get(yext)) != NULL){
        if ((cb = cbuf_new()) ==NULL){
            clixon_err(OE_XML, errno, "cbuf_new");
            goto done;
        }
        cprintf(cb, "<schema-mounts xmlns=\"%s\">", YANG_SCHEMA_MOUNT_NAMESPACE); // XXX only if hit
        cv = NULL;
        while ((cv = cvec_each(cvv, cv)) != NULL){
            ymount = (yang_stmt*)cv_void_get(cv);
            ymod = ys_module(ymount);
            if ((cv1 = yang_cv_get(ymount)) == NULL){
                clixon_err(OE_YANG, 0, "mount-point extension must have label");
                goto done;
            }
            label = cv_string_get(cv1);
            cprintf(cb, "<mount-point>");
            cprintf(cb, "<module>%s</module>", yang_argument_get(ymod));
            cprintf(cb, "<label>%s</label>", label);
            cprintf(cb, "<inline/>");
            cprintf(cb, "</mount-point>");
        }
        cprintf(cb, "</schema-mounts>");
        if ((ret = clixon_xml_parse_string(cbuf_get(cb), YB_MODULE, yspec, &x1, xerr)) < 0)
            goto done;
        if (ret == 0)
            goto fail;
        if (xpath_first(x1, nsc, "%s", xpath) != NULL){
            if ((ret = netconf_trymerge(x1, yspec, xret)) < 0)
                goto done;
            if (ret == 0)
                goto fail;
        }
    }
    /* Find mount-points and return yang-library state */
    if (yang_schema_mount_statedata_yanglib(h, xpath, nsc, xret, xerr) < 0)
        goto done;
 ok:
    retval = 1;
 done:
    if (x1)
        xml_free(x1);
    if (cb)
        cbuf_free(cb);
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Statistics about mountpoints
 *
 * @param[in]  h     Clixon handle
 * @retval     0     OK
 * @retval    -1     Error
 * @see yang_schema_mount_statedata
 */
int
yang_schema_mount_statistics(clixon_handle h,
                             cxobj        *xt,
                             int           modules,
                             cbuf         *cb)
{
    int        retval = -1;
    cvec      *cvv = NULL;
    cg_var    *cv;
    cxobj     *xmp;          /* xml mount-point */
    yang_stmt *yspec;
    yang_stmt *ym;
    int        ret;
    char      *xpath = NULL;
    uint64_t   nr;
    size_t     sz;

    if ((cvv = cvec_new(0)) == NULL){
        clixon_err(OE_UNIX, errno, "cvec_new");
        goto done;
    }
    if (xml_apply(xt, CX_ELMNT, find_schema_mounts, cvv) < 0)
        goto done;
    cv = NULL;
    while ((cv = cvec_each(cvv, cv)) != NULL) {
        if ((xmp = cv_void_get(cv)) == NULL)
            continue;
        if ((ret = xml_yang_mount_get(h, xmp, NULL, &yspec)) < 0)
            goto done;
        if (ret == 0)
            continue;
        if (xml2xpath(xmp, NULL, 1, 0, &xpath) < 0)
            goto done;
        cprintf(cb, "<module-set><name>mountpoint: ");
        xml_chardata_cbuf_append(cb, xpath);
        cprintf(cb, "</name>");
        nr = 0; sz = 0;
        if (yang_stats(yspec, &nr, &sz) < 0)
            goto done;
        cprintf(cb, "<nr>%" PRIu64 "</nr><size>%zu</size>", nr, sz);
        if (modules){
            ym = NULL;
            while ((ym = yn_each(yspec, ym)) != NULL) {
                cprintf(cb, "<module><name>%s</name>", yang_argument_get(ym));
                nr = 0; sz = 0;
                if (yang_stats(ym, &nr, &sz) < 0)
                    goto done;
                cprintf(cb, "<nr>%" PRIu64 "</nr><size>%zu</size>", nr, sz);
                cprintf(cb, "</module>");
            }
        }
        cprintf(cb, "</module-set>");
        if (xpath){
            free(xpath);
            xpath = NULL;
        }
    }
    retval = 0;
 done:
    if (xpath)
        free(xpath);
    if (cvv)
        cvec_free(cvv);
    return retval;
}

/*! Get yanglib from user plugin callback, parse it and mount it
 * 
 * @param[in]     h     Clixon handle
 * @param[in]     xt       
 * @retval        1     OK
 * @retval        0     No yanglib or problem when parsing yanglib
 * @retval       -1     Error
 */
int
yang_schema_yanglib_parse_mount(clixon_handle h,
                                cxobj        *xt)
{
    int            retval = -1;
    cxobj         *yanglib = NULL;
    yang_stmt     *yspec = NULL;
    int            ret;
    int            config = 1;
    validate_level vl = VL_FULL;

    if (clixon_plugin_yang_mount_all(h, xt, &config, &vl, &yanglib) < 0)
        goto done;
    if (yanglib == NULL)
        goto anydata;
    /* Parse it and set mount-point */
    if ((yspec = yspec_new()) == NULL)
        goto done;
    if ((ret = yang_lib2yspec(h, yanglib, yspec)) < 0)
        goto done;
    if (ret == 0)
        goto anydata;
    if (xml_yang_mount_set(h, xt, yspec) < 0)
        goto done;
    yspec = NULL;
    retval = 1;
 done:
    if (yspec)
        ys_free(yspec);
    if (yanglib)
        xml_free(yanglib);
    return retval;
 anydata:   // Treat as anydata
    retval = 0;
    goto done;
}

/*! Check if XML node is mount-point and return matching YANG child
 *
 * @param[in]     h       Clixon handle
 * @param[in]     x1      XML node
 * @param[in]     x1c     A child of x1
 * @param[out]    yc      YANG child
 * @retval        1       OK, yc contains child
 * @retval        0       No such child
 * @retval       -1       Error
 * XXX maybe not needed
 */
int
yang_schema_get_child(clixon_handle h,
                      cxobj        *x1,
                      cxobj        *x1c,
                      yang_stmt   **yc)
{
    int        retval = -1;
    yang_stmt *yspec1;
    yang_stmt *ymod1 = NULL;
    char      *x1cname;
    int        ret;

    x1cname = xml_name(x1c);
    if ((ret = xml_yang_mount_get(h, x1, NULL, &yspec1)) < 0)
        goto done;
    if (ret == 1 && yspec1 != NULL){
        if (ys_module_by_xml(yspec1, x1c, &ymod1) <0)
            goto done;
        if (ymod1 != NULL)
            *yc = yang_find_datanode(ymod1, x1cname);
        else{ /* It is in fact a mountpoint, there is a yang mount, but it is not found */
            goto fail;
        }
    }
    retval = 1;
 done:
    return retval;
 fail:
    retval = 0;
    goto done;
}
