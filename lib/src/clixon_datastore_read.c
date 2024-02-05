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
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <limits.h>
#include <stdint.h>
#include <sys/types.h>
#include <unistd.h>
#include <dirent.h>
#include <assert.h>
#include <syslog.h>
#include <fcntl.h>

/* cligen */
#include <cligen/cligen.h>

/* clixon */
#include "clixon_string.h"
#include "clixon_queue.h"
#include "clixon_hash.h"
#include "clixon_handle.h"
#include "clixon_yang.h"
#include "clixon_xml.h"
#include "clixon_err.h"
#include "clixon_log.h"
#include "clixon_debug.h"
#include "clixon_file.h"
#include "clixon_xml_sort.h"
#include "clixon_xml_bind.h"
#include "clixon_options.h"
#include "clixon_data.h"
#include "clixon_xpath_ctx.h"
#include "clixon_xpath.h"
#include "clixon_json.h"
#include "clixon_nacm.h"
#include "clixon_path.h"
#include "clixon_netconf_lib.h"
#include "clixon_yang_module.h"
#include "clixon_yang_parse_lib.h"
#include "clixon_xml_map.h"
#include "clixon_xml_default.h"
#include "clixon_xml_io.h"
#include "clixon_xml_nsctx.h"
#include "clixon_datastore.h"
#include "clixon_datastore_read.h"

#define handle(xh) (assert(text_handle_check(xh)==0),(struct text_handle *)(xh))

/*! Ensure that xt only has a single sub-element and that is "config" 
 *
 * @retval     0     There exists a single "config" sub-element
 * @retval    -1     Error: Top element not "config" or "config" element not unique or other
 */
static int
singleconfigroot(cxobj  *xt,
                 cxobj **xp)
{
    int    retval = -1;
    cxobj *x = NULL;
    int    i = 0;

    /* There should only be one element and called config */
    x = NULL;
    while ((x = xml_child_each(xt, x,  CX_ELMNT)) != NULL){
        i++;
        if (strcmp(xml_name(x), DATASTORE_TOP_SYMBOL)){
            clixon_err(OE_DB, ENOENT, "Wrong top-element %s expected %s",
                       xml_name(x), DATASTORE_TOP_SYMBOL);
            goto done;
        }
    }
    if (i != 1){
        clixon_err(OE_DB, ENOENT, "Top-element is not unique, expecting single config");
        goto done;
    }
    x = NULL;
    while ((x = xml_child_each(xt, x,  CX_ELMNT)) != NULL){
        if (xml_rm(x) < 0)
            goto done;
        if (xml_free(xt) < 0)
            goto done;
        *xp = x;
        break;
    }
    retval = 0;
 done:
    return retval;
}

/*! Recurse up from x0 up to x0t then create objects from x1t down to new object x1
 *
 * @retval     0    OK
 * @retval    -1    OK
 */
static int
xml_copy_bottom_recurse(cxobj  *x0t,
                        cxobj  *x0,
                        cxobj  *x1t,
                        cxobj **x1pp)
{
    int        retval = -1;
    cxobj     *x0p = NULL;
    cxobj     *x1p = NULL;
    cxobj     *x1 = NULL;
    cxobj     *x1a = NULL;
    cxobj     *x0a = NULL;
    cxobj     *x0k;
    cxobj     *x1k;
    yang_stmt *y = NULL;
    cvec      *cvk = NULL; /* vector of index keys */
    cg_var    *cvi;
    char      *keyname;

    if (x0 == x0t){
        *x1pp = x1t;
        goto ok;
    }
    if ((x0p = xml_parent(x0)) == NULL){
        clixon_err(OE_XML, EFAULT, "Reached top of tree");
        goto done;
    }
    if (xml_copy_bottom_recurse(x0t, x0p, x1t, &x1p) < 0)
        goto done;
    y = xml_spec(x0);
    /* Look if it exists */
    if (match_base_child(x1p, x0, y, &x1) < 0)
        goto done;
    if (x1 == NULL){ /* If not, create it and copy it one level only */
        if ((x1 = xml_new(xml_name(x0), x1p, CX_ELMNT)) == NULL)
            goto done;
        if (xml_copy_one(x0, x1) < 0)
            goto done;
        /* Copy all attributes */
        x0a = NULL;
        while ((x0a = xml_child_each(x0, x0a, -1)) != NULL) {
            /* Assume ordered, skip after attributes */
            if (xml_type(x0a) != CX_ATTR)
                break;
            if ((x1a = xml_new(xml_name(x0a), x1, CX_ATTR)) == NULL)
                goto done;
            if (xml_copy_one(x0a, x1a) < 0)
                goto done;
        }
        /* Key nodes in lists are copied */
        if (y && yang_keyword_get(y) == Y_LIST){
            /* Loop over all key variables */
            cvk = yang_cvec_get(y); /* Use Y_LIST cache, see ys_populate_list() */
            cvi = NULL;
            /* Iterate over individual keys  */
            while ((cvi = cvec_each(cvk, cvi)) != NULL) {
                keyname = cv_string_get(cvi);
                if ((x0k = xml_find_type(x0, NULL, keyname, CX_ELMNT)) != NULL){
                    if ((x1k = xml_new(keyname, x1, CX_ELMNT)) == NULL)
                        goto done;
                    if (xml_copy(x0k, x1k) < 0)
                        goto done;
                }

            }
        }
    }
    *x1pp = x1;
 ok:
    retval = 0;
 done:
    return retval;
}

/*! Copy an XML tree bottom-up
 *
 * @retval     0    OK
 * @retval    -1    OK
 */
static int
xml_copy_from_bottom(cxobj  *x0t,
                     cxobj  *x0,
                     cxobj  *x1t)
{
    int        retval = -1;
    cxobj     *x1p    = NULL;
    cxobj     *x0p    = NULL;
    cxobj     *x1     = NULL;
    yang_stmt *y      = NULL;

    if (x0 == x0t)
        goto ok;
    x0p = xml_parent(x0);
    if (xml_copy_bottom_recurse(x0t, x0p, x1t, &x1p) < 0)
        goto done;
    if ((y = xml_spec(x0)) != NULL){
        /* Look if it exists */
        if (match_base_child(x1p, x0, y, &x1) < 0)
            goto done;
    }
    if (x1 == NULL){ /* If not, create it and copy complete tree */
        if ((x1 = xml_new(xml_name(x0), x1p, CX_ELMNT)) == NULL)
            goto done;
        if (xml_copy(x0, x1) < 0)
            goto done;
    }
 ok:
    retval = 0;
 done:
    return retval;
}

/*! Read module-state in an XML tree
 *
 * @param[in]  th     Datastore text handle
 * @param[in]  yspec  Top-level yang spec 
 * @param[in]  xt     XML tree
 * @param[out] msdiff Modules-state differences
 * @retval     0      OK
 * @retval    -1      Error
 *
 * The modstate difference contains:
 * - if there is a modstate
 * - the modstate identifier
 * - An entry for each modstate that differs marked with flag: ADD|DEL|CHANGE
 *
 * Algorithm:
 * Read mst (module-state-tree) from xml tree (if any) and compare it with 
 * the system state mst.
 * This can happen:
 * 1) There is no modules-state info in the file
 * 2) There is module state info in the file
 * 3) For each module state m in the file:
 *    3a) There is no such module in the system -> add to list mark as DEL
 *    3b) File module-state matches system
 *    3c) File module-state does not match system -> add to list mark as CHANGE
 * 4) For each module state s in the system
 *    4a) If there is no such module in the file -> add to list mark as ADD
 */
static int
text_read_modstate(clixon_handle       h,
                   yang_stmt          *yspec,
                   cxobj              *xt,
                   modstate_diff_t    *msdiff)
{
    int    retval = -1;
    cxobj *xmodfile = NULL;   /* modstate of system (loaded yang modules in runtime) */
    cxobj *xyanglib = NULL;
    cxobj *xmodcache;
    cxobj *xmodsystem = NULL; /* modstate of file, eg startup */
    cxobj *xf = NULL;         /* xml modstate in file */
    cxobj *xf2;               /* copy */
    cxobj *xs;                /* xml modstate in system */
    cxobj *xs2;               /* copy */
    char  *name;              /* module name */
    char  *frev;              /* file revision */
    char  *srev;              /* system revision */
    int    rfc7895=0;         /* backward-compatible: old version */

    /* Read module-state as computed at startup, see startup_module_state() */
    if ((xmodcache = clicon_modst_cache_get(h, 1)) != NULL)
        xmodsystem = xml_find_type(xmodcache, NULL, "module-set", CX_ELMNT);

    xyanglib = xml_find_type(xt, NULL, "yang-library", CX_ELMNT);
    if ((xmodfile = xpath_first(xt, NULL, "yang-library/module-set")) != NULL)
        ;
    else if ((xmodfile = xml_find_type(xt, NULL, "modules-state", CX_ELMNT)) != NULL)
        rfc7895++;
    if (xmodfile && xmodsystem && msdiff){
        msdiff->md_status = 1;  /* There is module state in the file */
        /* Create modstate tree for this file 
         * Note, module-set is not a top-level symbol, so cannot bind using module-set
         */
        if (clixon_xml_parse_string("<module-set xmlns=\"urn:ietf:params:xml:ns:yang:ietf-yang-library\"/>",
                                    YB_NONE, yspec, &msdiff->md_diff, NULL) < 0)
            goto done;
        if (xml_rootchild(msdiff->md_diff, 0, &msdiff->md_diff) < 0)
            goto done;

        if (!rfc7895){
            if ((xf = xpath_first(xt, NULL, "yang-library/content-id")) != NULL){
                if (xml_body(xf) && (msdiff->md_content_id = strdup(xml_body(xf))) == NULL){
                    clixon_err(OE_UNIX, errno, "strdup");
                    goto done;
                }
            }
        }
        /* 3) For each module state m in the file */
        xf = NULL;
        while ((xf = xml_child_each(xmodfile, xf, CX_ELMNT)) != NULL) {
            if (rfc7895){
                if (strcmp(xml_name(xf), "module-set-id") == 0){
                    if (xml_body(xf) && (msdiff->md_content_id = strdup(xml_body(xf))) == NULL){
                        clixon_err(OE_UNIX, errno, "strdup");
                        goto done;
                    }
                    continue;
                }
            }
            if (strcmp(xml_name(xf), "module"))
                continue; /* ignore other tags, such as module-set-id */
            if ((name = xml_find_body(xf, "name")) == NULL)
                continue;
            /* 3a) There is no such module in the system */
            if ((xs = xpath_first(xmodsystem, NULL, "module[name=\"%s\"]", name)) == NULL){
                if ((xf2 = xml_dup(xf)) == NULL)          /* Make a copy of this modstate */
                    goto done;
                if (xml_addsub(msdiff->md_diff, xf2) < 0)   /* Add it to modstatediff */
                    goto done;
                xml_flag_set(xf2, XML_FLAG_DEL);
                continue;
            }
            /* These two shouldnt happen since revision is key, just ignore */
            if ((frev = xml_find_body(xf, "revision")) == NULL)
                continue;
            if ((srev = xml_find_body(xs, "revision")) == NULL)
                continue;
            if (strcmp(frev, srev) != 0){
                /* 3c) File module-state does not match system */
                if ((xf2 = xml_dup(xf)) == NULL)
                    goto done;
                if (xml_addsub(msdiff->md_diff, xf2) < 0)
                    goto done;
                xml_flag_set(xf2, XML_FLAG_CHANGE);
            }
        }
        /* 4) For each module state s in the system (xmodsystem) */
        xs = NULL;
        while ((xs = xml_child_each(xmodsystem, xs, CX_ELMNT)) != NULL) {
            if (strcmp(xml_name(xs), "module"))
                continue; /* ignore other tags, such as module-set-id */
            if ((name = xml_find_body(xs, "name")) == NULL)
                continue;
            /* 4a) If there is no such module in the file -> add to list mark as ADD */
            if ((xf = xpath_first(xmodfile, NULL, "module[name=\"%s\"]", name)) == NULL){
                if ((xs2 = xml_dup(xs)) == NULL)          /* Make a copy of this modstate */
                    goto done;
                if (xml_addsub(msdiff->md_diff, xs2) < 0)   /* Add it to modstatediff */
                    goto done;
                xml_flag_set(xs2, XML_FLAG_ADD);
                continue;
            }
        }
    }
    /* The module-state is removed from the input XML tree. This is done
     * in all cases, whether CLICON_XMLDB_MODSTATE is on or not.
     * Clixon systems with CLICON_XMLDB_MODSTATE disabled ignores it
     */
    if (rfc7895){
        if (xmodfile){
            if (xml_purge(xmodfile) < 0)
                goto done;
        }
    }
    else if (xyanglib)
        if (xml_purge(xyanglib) < 0)
                goto done;
    retval = 0;
 done:
    return retval;
}

/*! Check if nacm only contains default values, if so disable NACM
 *
 * @param[in]  xt    Top-level XML
 * @param[in]  yspec YANG spec  
 * @retval     0     OK
 * @retval    -1     Error
 */
static int
disable_nacm_on_empty(cxobj     *xt,
                      yang_stmt *yspec)
{
    int        retval = -1;
    yang_stmt *ymod;
    cxobj    **vec = NULL;
    int        len = 0;
    cxobj     *xnacm = NULL;
    cxobj     *x;
    cxobj     *xb;

    if ((ymod = yang_find(yspec, Y_MODULE, "ietf-netconf-acm")) == NULL)
        goto ok;
    if ((xnacm = xpath_first(xt, NULL, "nacm")) == NULL)
        goto ok;
    /* Go through all children and check all are defaults, otherwise quit */
    x = NULL;
    while ((x = xml_child_each(xnacm, x, CX_ELMNT)) != NULL) {
        if (!xml_flag(x, XML_FLAG_DEFAULT))
            break;
    }
    if (x != NULL)
        goto ok; /* not empty, at least one non-default child of nacm */
    if (clixon_xml_find_instance_id(xt, yspec, &vec, &len, "/nacm:nacm/nacm:enable-nacm") < 1)
        goto done;
    if (len){
        if ((xb = xml_body_get(vec[0])) == NULL)
            goto done;
        if (xml_value_set(xb, "false") < 0)
            goto done;
    }
    if (vec)
        free(vec);
 ok:
    retval = 0;
 done:
    return retval;
}

/*! Common read function that reads an XML tree from file
 *
 * @param[in]  th     Datastore text handle
 * @param[in]  db     Symbolic database name, eg "candidate", "running"
 * @param[in]  yb     How to bind yang to XML top-level when parsing
 * @param[in]  yspec  Top-level yang spec
 * @param[out] xp     XML tree read from file
 * @param[out] de     If set, return db-element status (eg empty flag)
 * @param[out] msdiff If set, return modules-state differences
 * @param[out] xerr   XML error if retval is 0
 * @retval     1      OK
 * @retval     0      Parse OK but yang assigment not made (or only partial) and xerr set
 * @retval    -1      Error
 * @note Use of 1 for OK
 * @note retval 0 is NYI because calling functions cannot handle it yet
 * XXX if this code pass tests this code can be rewritten, esp the modstate stuff
 */
int
xmldb_readfile(clixon_handle    h,
               const char      *db,
               yang_bind        yb,
               yang_stmt       *yspec,
               cxobj          **xp,
               db_elmnt        *de,
               modstate_diff_t *msdiff0,
               cxobj          **xerr)
{
    int              retval = -1;
    cxobj           *x0 = NULL;
    char            *dbfile = NULL;
    FILE            *fp = NULL;
    char            *format;
    int              ret;
    modstate_diff_t *msdiff = NULL;
    cxobj           *xmsd;           /* XML module state diff */
    yang_stmt       *ymod;
    char            *name;
    char            *ns;             /* namespace */
    char            *rev;            /* revision */
    int              needclone;
    cxobj           *xmodfile = NULL;
    cxobj           *x;
    yang_stmt       *yspec1 = NULL;

    if (yb != YB_MODULE && yb != YB_NONE){
        clixon_err(OE_XML, EINVAL, "yb is %d but should be module or none", yb);
        goto done;
    }
    if (xmldb_db2file(h, db, &dbfile) < 0)
        goto done;
    if (dbfile==NULL){
        clixon_err(OE_XML, 0, "dbfile NULL");
        goto done;
    }
    if ((format = clicon_option_str(h, "CLICON_XMLDB_FORMAT")) == NULL){
        clixon_err(OE_CFG, ENOENT, "No CLICON_XMLDB_FORMAT");
        goto done;
    }
    clixon_debug(CLIXON_DBG_DATASTORE, "Reading datastore %s using %s", dbfile, format);
    /* Parse file into internal XML tree from different formats */
    if ((fp = fopen(dbfile, "r")) == NULL) {
        clixon_err(OE_UNIX, errno, "open(%s)", dbfile);
        goto done;
    }
    /* Read whole datastore file on the form:
     * <config>
     *   modstate*  # this is analyzed, stripped and returned as msdiff in text_read_modstate
     *   config*
     * </config>
     * ret == 0 should not happen with YB_NONE. Binding is done later */
    if (strcmp(format, "json")==0){
        if (clixon_json_parse_file(fp, 1, YB_NONE, yspec, &x0, xerr) < 0)
            goto done;
    }
    else {
        if (clixon_xml_parse_file(fp, YB_NONE, yspec, &x0, xerr) < 0){
            goto done;
        }
    }
    /* Always assert a top-level called "config". 
     * To ensure that, deal with two cases:
     * 1. File is empty <top/> -> rename top-level to "config" 
     */
    if (xml_child_nr(x0) == 0){
        if (xml_name_set(x0, DATASTORE_TOP_SYMBOL) < 0)
            goto done;
    }
    /* 2. File is not empty <top><config>...</config></top> -> replace root */
    else{
        /* There should only be one element and called config */
        if (singleconfigroot(x0, &x0) < 0)
            goto done;
    }
    /* Purge all top-level body objects */
    x = NULL;
    while ((x = xml_find_type(x0, NULL, "body", CX_BODY)) != NULL)
        xml_purge(x);

    xml_flag_set(x0, XML_FLAG_TOP);
    if (xml_child_nr(x0) == 0 && de)
        de->de_empty = 1;
    /* Check if we support modstate */
    if (clicon_option_bool(h, "CLICON_XMLDB_MODSTATE"))
        if ((msdiff = modstate_diff_new()) == NULL)
            goto done;
    /* First try RFC8525, but also backward compatible RFC7895 */
    if ((x = xpath_first(x0, NULL, "yang-library/module-set")) != NULL ||
        (x = xml_find_type(x0, NULL, "modules-state", CX_ELMNT)) != NULL){
        if ((xmodfile = xml_dup(x)) == NULL)
            goto done;
    }
    /* Datastore files may contain module-state defining
     * which modules are used in the file. 
     * Strip module-state, analyze it with CHANGE/ADD/RM and return msdiff
     */
    if (text_read_modstate(h, yspec, x0, msdiff) < 0)
        goto done;
    if (yb == YB_MODULE){
        if (msdiff){
            /* Check if old/deleted yangs not present in the loaded/running yangspec.
             * If so, append them to the global yspec
             */
            needclone = 0;
            xmsd = NULL;
            while ((xmsd = xml_child_each(msdiff->md_diff, xmsd, CX_ELMNT)) != NULL) {
                if (xml_flag(xmsd, XML_FLAG_CHANGE|XML_FLAG_DEL) == 0)
                    continue;
                needclone++;
                /* Extract name, namespace, and revision */
                if ((name = xml_find_body(xmsd, "name")) == NULL)
                    continue;
                if ((ns = xml_find_body(xmsd, "namespace")) == NULL)
                    continue;
                /* Extract revision */
                if ((rev = xml_find_body(xmsd, "revision")) == NULL)
                    continue;
                /* Add old/deleted yangs not present in the loaded/running yangspec. */
                if ((ymod = yang_find_module_by_namespace_revision(yspec, ns, rev)) == NULL){
                    /* YANG Module not found, look for it and append if found */
                    if (yang_spec_parse_module(h, name, rev, yspec) < 0){
                        /* Special case: file-not-found errors */
                        if (clixon_err_subnr() == ENOENT){
                            cbuf *cberr = NULL;
                            if ((cberr = cbuf_new()) == NULL){
                                clixon_err(OE_XML, errno, "cbuf_new");
                                goto done;
                            }
                            cprintf(cberr, "Internal error: %s", clixon_err_reason());
                            clixon_err_reset();
                            if (xerr && netconf_operation_failed_xml(xerr, "application", cbuf_get(cberr))< 0)
                                goto done;
                            cbuf_free(cberr);
                            goto fail;
                        }
                        goto done;
                    }
                }
            }
            /* If we found an obsolete yang module, we need to make a clone yspec with the
             * exactly the yang modules found 
             * Same ymodules are inserted into yspec1, ie pointers only
             */
            if (needclone && xmodfile){
                if ((yspec1 = yspec_new()) == NULL)
                    goto done;
                xmsd = NULL;
                while ((xmsd = xml_child_each(xmodfile, xmsd, CX_ELMNT)) != NULL) {
                    if (strcmp(xml_name(xmsd), "module"))
                        continue;
                    if ((ns = xml_find_body(xmsd, "namespace")) == NULL)
                        continue;
                    if ((rev = xml_find_body(xmsd, "revision")) == NULL)
                        continue;
                    if ((ymod = yang_find_module_by_namespace_revision(yspec, ns, rev)) == NULL)
                        continue; // XXX error?
                    if (yn_insert1(yspec1, ymod) < 0)
                        goto done;
                }
            }
        } /* if msdiff */
        /* xml looks like: <top><config><x>... actually YB_MODULE_NEXT 
         */
        if ((ret = xml_bind_yang(h, x0, YB_MODULE, yspec1?yspec1:yspec, xerr)) < 0)
            goto done;
        if (ret == 0)
            goto fail;
        if (xml_sort_recurse(x0) < 0)
            goto done;
    }
    if (xp){
        *xp = x0;
        x0 = NULL;
    }
    if (msdiff0){
        *msdiff0 = *msdiff;
        free(msdiff); /* Just body */
        msdiff = NULL;
    }
    retval = 1;
 done:
    if (yspec1)
        ys_free1(yspec1, 1);
    if (xmodfile)
        xml_free(xmodfile);
    if (msdiff)
        modstate_diff_free(msdiff);
    if (fp)
        fclose(fp);
    if (dbfile)
        free(dbfile);
    if (x0)
        xml_free(x0);
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Get content of database using xpath. return a set of matching sub-trees
 *
 * The function returns a minimal tree that includes all sub-trees that match
 * xpath.
 * This is a clixon datastore plugin of the the xmldb api
 * @param[in]  h      Clixon handle
 * @param[in]  db     Name of database to search in (filename including dir path
 * @param[in]  yb     How to bind yang to XML top-level when parsing
 * @param[in]  nsc    External XML namespace context, or NULL
 * @param[in]  xpath  String with XPath syntax. or NULL for all
 * @param[in]  wdef   With-defaults parameter, see RFC 6243
 * @param[out] xret   Single return XML tree. Free with xml_free()
 * @param[out] msdiff If set, return modules-state differences
 * @param[out] xerr   XML error if retval is 0
 * @retval     1      OK
 * @retval     0      Parse OK but yang assigment not made (or only partial) and xerr set
 * @retval    -1      Error
 */
static int
xmldb_get_cache(clixon_handle     h,
                const char       *db,
                yang_bind         yb,
                cvec             *nsc,
                const char       *xpath,
                withdefaults_type wdef,
                cxobj           **xret,
                modstate_diff_t  *msdiff,
                cxobj           **xerr)
{
    int        retval = -1;
    yang_stmt *yspec;
    cxobj     *x0t = NULL; /* (cached) top of tree */
    cxobj     *x0;
    cxobj    **xvec = NULL;
    size_t     xlen;
    int        i;
    db_elmnt  *de = NULL;
    cxobj     *x1t = NULL;
    db_elmnt   de0 = {0,};
    int        ret;

    clixon_debug(CLIXON_DBG_DATASTORE, "db %s", db);
    if (xret == NULL){
        clixon_err(OE_DB, EINVAL, "xret is NULL");
        return -1;
    }
    if ((yspec = clicon_dbspec_yang(h)) == NULL){
        clixon_err(OE_YANG, ENOENT, "No yang spec");
        goto done;
    }
    de = clicon_db_elmnt_get(h, db);
    if (de == NULL || de->de_xml == NULL){ /* Cache miss, read XML from file */
        /* If there is no xml x0 tree (in cache), then read it from file */
        /* xml looks like: <top><config><x>... where "x" is a top-level symbol in a module */
        if ((ret = xmldb_readfile(h, db, yb, yspec, &x0t, &de0, msdiff, xerr)) < 0)
            goto done;
        if (ret == 0)
            goto fail;
        /* Should we validate file if read from disk?
         * No, argument against: we may want to have a semantically wrong file and wish to edit?
         */
        de0.de_xml = x0t;
        if (de)
            de0.de_id = de->de_id;
        clicon_db_elmnt_set(h, db, &de0); /* Content is copied */
        /* Add default global values (to make xpath below include defaults) */
        // Alt:  xmldb_populate(h, db)
        if (xml_global_defaults(h, x0t, nsc, xpath, yspec, 0) < 0)
            goto done;
        /* Add default recursive values */
        if (xml_default_recurse(x0t, 0) < 0)
            goto done;
    } /* x0t == NULL */
    else
        x0t = de->de_xml;
    /* Here x0t looks like: <config>...</config> */
    /* Given the xpath, return a vector of matches in xvec 
     * Can we do everything in one go?
     * 0) Make a new tree
     * 1) make the xpath check 
     * 2) iterate thru matches (maybe this can be folded into the xpath_vec?)
     *   a) for every node that is found, copy to new tree
     *   b) if config dont dont state data
     */
    if (xpath_vec(x0t, nsc, "%s", &xvec, &xlen, xpath?xpath:"/") < 0)
        goto done;
    // XXX: Remove copying and return x0 eventually
    /* Make new tree by copying top-of-tree from x0t to x1t */
    if ((x1t = xml_new(xml_name(x0t), NULL, CX_ELMNT)) == NULL)
        goto done;
    xml_flag_set(x1t, XML_FLAG_TOP);
    xml_spec_set(x1t, xml_spec(x0t));
    if (xlen < 1000){
        /* This is optimized for the case when the tree is large and xlen is small
         * If the tree is large and xlen too, then the other is better.
         * This only works if yang bind
         */
        for (i=0; i<xlen; i++){
            x0 = xvec[i];
            if (xml_copy_from_bottom(x0t, x0, x1t) < 0) /* config */
                goto done;
        }
    }
    else {
        /* Iterate through the match vector
         * For every node found in x0, mark the tree up to t1
         * XXX can we do this directly from xvec?
         */
        for (i=0; i<xlen; i++){
            x0 = xvec[i];
            xml_flag_set(x0, XML_FLAG_MARK);
            xml_apply_ancestor(x0, (xml_applyfn_t*)xml_flag_set, (void*)XML_FLAG_CHANGE);
        }
        if (xml_copy_marked(x0t, x1t) < 0) /* config */
            goto done;
        if (xml_apply(x0t, CX_ELMNT, (xml_applyfn_t*)xml_flag_reset, (void*)(XML_FLAG_MARK|XML_FLAG_CHANGE)) < 0)
            goto done;
        if (xml_apply(x1t, CX_ELMNT, (xml_applyfn_t*)xml_flag_reset, (void*)(XML_FLAG_MARK|XML_FLAG_CHANGE)) < 0)
            goto done;
    }
    /* If empty NACM config, then disable NACM if loaded
     */
    if (clicon_option_bool(h, "CLICON_NACM_DISABLED_ON_EMPTY")){
        if (disable_nacm_on_empty(x1t, yspec) < 0)
            goto done;
    }
    clixon_debug_xml(CLIXON_DBG_DATASTORE | CLIXON_DBG_DETAIL, x1t, "");
    *xret = x1t;
    retval = 1;
 done:
    clixon_debug(CLIXON_DBG_DATASTORE | CLIXON_DBG_DETAIL, "retval:%d", retval);
    if (xvec)
        free(xvec);
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Get content of datastore and return a copy of the XML tree
 *
 * @param[in]  h      Clixon handle
 * @param[in]  db     Name of database to search in, eg "running"
 * @param[in]  nsc    XML namespace context for XPath
 * @param[in]  xpath  String with XPath syntax. or NULL for all
 * @param[out] xret   Single return XML tree. Free with xml_free()
 * @retval     1      OK
 * @retval     0      Parse OK but yang assigment not made (or only partial) and xerr set
 * @retval    -1      Error
 * @note Use of 1 for OK
 * @code
 *   if (xmldb_get(xh, "running", NULL, "/interfaces/interface[name="eth"]", &xt) < 0)
 *      err;
 *   xml_free(xt);
 * @endcode
 * @see xmldb_get0  Underlying more capable API for enabling zero-copy
 */
int
xmldb_get(clixon_handle    h,
          const char      *db,
          cvec            *nsc,
          char            *xpath,
          cxobj          **xret)
{
    return xmldb_get0(h, db, YB_MODULE, nsc, xpath, 1, 0, xret, NULL, NULL);
}

/*! Get content of datastore, use cache if present
 *
 * The function returns a minimal tree that includes all sub-trees that match
 * xpath. 
 * @param[in]  h      Clixon handle
 * @param[in]  db     Name of datastore, eg "running"
 * @param[in]  yb     How to bind yang to XML top-level when parsing (if YB_NONE, no defaults)
 * @param[in]  nsc    External XML namespace context, or NULL
 * @param[in]  xpath  String with XPath syntax. or NULL for all
 * @param[in]  copy   Force copy. Overrides cache_zerocopy -> cache 
 * @param[in]  wdef   With-defaults parameter, see RFC 6243
 * @param[out] xret   Single return XML tree. Free with xml_free()
 * @param[out] msdiff If set, return modules-state differences (upgrade code)
 * @param[out] xerr   XML error if retval is 0
 * @retval     1      OK
 * @retval     0      Parse OK but yang assigment not made (or only partial) and xerr set
 * @retval    -1      Error
 * @note Use of 1 for OK
 * @code
 *   cxobj   *xt;
 *   cxobj   *xerr = NULL;
 *   if (xmldb_get0(h, "running", YB_MODULE, nsc, "/interface[name="eth"]", 0, 0, &xt, NULL, &xerr) < 0)
 *      err;
 *   if (ret == 0){ # Not if YB_NONE
 *      # Error handling
 *   }
 *   ...
 *   xml_free(xerr);
 * @endcode
 * @see xml_nsctx_node  to get a XML namespace context from XML tree
 * @see xmldb_get for a copy version (old-style)
 * @note An annoying issue is one with default values and xpath miss:
 *   Assume a yang spec: 
 *      module m { 
 *         container c { 
 *            leaf x { 
 *               default 0;
 *   And a db content:
 *      <c><x>1</x></c>
 *   With the following call:
 *      xmldb_get0(h, "running", NULL, NULL, "/c[x=0]", 1, 0, &xt, NULL, NULL)
 *   which result in a miss (there is no c with x=0), but when the returned xt is printed 
 *   (the existing tree is discarded), the default (empty) xml tree is:
 *      <c><x>0</x></c>
 */
int
xmldb_get0(clixon_handle    h,
           const char      *db,
           yang_bind        yb,
           cvec            *nsc,
           const char      *xpath,
           int              copy,
           withdefaults_type wdef,
           cxobj          **xret,
           modstate_diff_t *msdiff,
           cxobj          **xerr)
{
    int    retval = -1;
    int    ret;
    cxobj *x = NULL;

    if (wdef != WITHDEFAULTS_EXPLICIT)
        return xmldb_get_cache(h, db, yb, nsc, xpath, 0, xret, msdiff, xerr);
    if ((ret = xmldb_get_cache(h, db, yb, nsc, xpath, 0, &x, msdiff, xerr)) < 0)
        goto done;
    if (ret == 0)
        goto fail;
    if (xml_defaults_nopresence(x, 2) < 0)
        goto done;
    *xret = x;
    x = NULL;
    retval = 1;
 done:
    if (x)
        xml_free(x);
    return retval;
 fail:
    retval = 0;
    goto done;
}
