/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2017 Olof Hagsand and Benny Holmgren

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

 * Yang type related functions
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <ctype.h>
#define __USE_GNU /* strverscmp */
#include <string.h>
#include <arpa/inet.h>
#include <regex.h>
#include <syslog.h>
#include <assert.h>
#include <netinet/in.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include "clixon_log.h"
#include "clixon_err.h"
#include "clixon_string.h"
#include "clixon_queue.h"
#include "clixon_hash.h"
#include "clixon_handle.h"
#include "clixon_yang.h"
#include "clixon_hash.h"
#include "clixon_plugin.h"
#include "clixon_options.h"
#include "clixon_yang.h"
#include "clixon_yang_type.h"

/* 
 * Local types and variables
 */

/* Mapping between yang types <--> cligen types
   Note, first match used wne translating from cv to yang --> order is significant */
static const map_str2int ytmap[] = {
    {"int32",       CGV_INT32},  /* NOTE, first match on right is significant, dont move */
    {"string",      CGV_STRING}, /* NOTE, first match on right is significant, dont move */
    {"string",      CGV_REST},   /* For cv -> yang translation of rest */
    {"binary",      CGV_STRING},    
    {"bits",        CGV_STRING},    
    {"boolean",     CGV_BOOL},
    {"decimal64",   CGV_DEC64},  
    {"empty",       CGV_VOID},  /* May not include any content */
    {"enumeration", CGV_STRING}, 
    {"identityref", CGV_STRING},  /* XXX */
    {"instance-identifier", CGV_STRING}, /* XXX */
    {"int8",        CGV_INT8},  
    {"int16",       CGV_INT16},  
    {"int64",       CGV_INT64},
    {"leafref",     CGV_STRING},  /* XXX */
    {"uint8",       CGV_UINT8}, 
    {"uint16",      CGV_UINT16},
    {"uint32",      CGV_UINT32},
    {"uint64",      CGV_UINT64},
    {"union",       CGV_REST},  /* Is replaced by actual type */
    {NULL,         -1}
};

/* return 1 if built-in, 0 if not */
static int
yang_builtin(char *type)
{
    if (clicon_str2int(ytmap, type) != -1)
	return 1;
    return 0;
}

/*! Set type cache for yang type
 */
int
yang_type_cache_set(yang_type_cache **ycache0,
		    yang_stmt        *resolved,
		    int               options, 
		    cg_var           *mincv, 
		    cg_var           *maxcv, 
		    char             *pattern,
		    uint8_t           fraction)
{
    int              retval = -1;
    yang_type_cache *ycache = *ycache0;

    assert (ycache == NULL);
    if ((ycache = (yang_type_cache *)malloc(sizeof(*ycache))) == NULL){
	clicon_err(OE_UNIX, errno, "malloc");
	goto done;
    }
    memset(ycache, 0, sizeof(*ycache));
    *ycache0 = ycache;
    ycache->yc_resolved  = resolved;
    ycache->yc_options  = options;
    if (mincv && (ycache->yc_mincv  = cv_dup(mincv)) == NULL){
	clicon_err(OE_UNIX, errno, "cv_dup");
	goto done;
    }
    if (maxcv && (ycache->yc_maxcv  = cv_dup(maxcv)) == NULL){
	clicon_err(OE_UNIX, errno, "cv_dup");
	goto done;
    }
    if (pattern && (ycache->yc_pattern  = strdup(pattern)) == NULL){
	clicon_err(OE_UNIX, errno, "strdup");
	goto done;
    }
    ycache->yc_fraction  = fraction;
    retval = 0;
 done:
    return retval;
}

/*! Get individual fields (direct/destrucively) from yang type cache. */
int
yang_type_cache_get(yang_type_cache *ycache,
		    yang_stmt      **resolved,
		    int             *options, 
		    cg_var         **mincv, 
		    cg_var         **maxcv, 
		    char           **pattern,
		    uint8_t         *fraction)
{
    if (resolved)
	*resolved = ycache->yc_resolved;
    if (options)
	*options  = ycache->yc_options;
    if (mincv)
	*mincv    = ycache->yc_mincv;
    if (maxcv)
	*maxcv    = ycache->yc_maxcv;
    if (pattern)
	*pattern  = ycache->yc_pattern;
    if (fraction)
	*fraction = ycache->yc_fraction;
    return 0;
}

int
yang_type_cache_cp(yang_type_cache **ycnew, 
		   yang_type_cache  *ycold)
{
    int        retval = -1;
    int        options;
    cg_var    *mincv;
    cg_var    *maxcv;
    char      *pattern;
    uint8_t    fraction;
    yang_stmt *resolved;

    yang_type_cache_get(ycold, &resolved, &options, &mincv, &maxcv, &pattern, &fraction);
    if (yang_type_cache_set(ycnew, resolved, options, mincv, maxcv, pattern, fraction) < 0)
	goto done;
    retval = 0;
 done:
    return retval;
}

int
yang_type_cache_free(yang_type_cache *ycache)
{
    if (ycache->yc_mincv)
	cv_free(ycache->yc_mincv);
    if (ycache->yc_maxcv)
	cv_free(ycache->yc_maxcv);
    if (ycache->yc_pattern)
	free(ycache->yc_pattern);
    free(ycache);
    return 0;
}

/*! Resolve types: populate type caches 
 * @param[in]  ys  This is a type statement
 * @param[in]  arg Not used
 * Typically only called once when loading te yang type system.
 * @note unions not cached
 */
int
ys_resolve_type(yang_stmt *ys, 
		void      *arg)
{
    int               retval = -1;
    int               options = 0x0;
    cg_var           *mincv = NULL;
    cg_var           *maxcv = NULL;
    char             *pattern = NULL;
    uint8_t           fraction = 0;
    yang_stmt        *resolved = NULL;
 
    assert(ys->ys_keyword == Y_TYPE);
    if (yang_type_resolve((yang_stmt*)ys->ys_parent, ys, &resolved,
			  &options, &mincv, &maxcv, &pattern, &fraction) < 0)
	goto done;

    if (resolved && strcmp(resolved->ys_argument, "union")==0)
	; 
    /* skip unions since they may have different sets of options, mincv, etc 
     * You would have to resolve all sub-types also recursively
     */
    else
	if (yang_type_cache_set(&ys->ys_typecache, 
				resolved, options, mincv, maxcv, pattern, fraction) < 0)
	    goto done;
    retval = 0;
 done:
    return retval;
}

/*! Translate from a yang type to a cligen variable type
 *
 * Currently many built-in types from RFC6020 and some RFC6991 types.
 * But not all, neither built-in nor 6991.
 * Also, there is no support for derived types, eg yang typedefs.
 * See 4.2.4 in RFC6020
 * Return 0 if no match but set cv_type to CGV_ERR
 */
int
yang2cv_type(char         *ytype, 
	     enum cv_type *cv_type)
{
    int                ret;

    *cv_type = CGV_ERR;
    /* built-in types */
    if ((ret = clicon_str2int(ytmap, ytype)) != -1){
	*cv_type = ret;
	return 0;
    }
    /* special derived types */
    if (strcmp("ipv4-address", ytype) == 0){ /* RFC6991 */
	*cv_type = CGV_IPV4ADDR;
	return 0;
    }
    if (strcmp("ipv6-address", ytype) == 0){ /* RFC6991 */
	*cv_type = CGV_IPV6ADDR;
	return 0;
    }
    if (strcmp("ipv4-prefix", ytype) == 0){ /* RFC6991 */
	*cv_type = CGV_IPV4PFX;
	return 0;
    }
    if (strcmp("ipv6-prefix", ytype) == 0){ /* RFC6991 */
	*cv_type = CGV_IPV6PFX;
	return 0;
    }
    if (strcmp("date-and-time", ytype) == 0){ /* RFC6991 */
	*cv_type = CGV_TIME;
	return 0;
    }
    if (strcmp("mac-address", ytype) == 0){ /* RFC6991 */
	*cv_type = CGV_MACADDR;
	return 0;
    }
    if (strcmp("uuid", ytype) == 0){ /* RFC6991 */
	*cv_type = CGV_UUID;
	return 0;
    }
    return 0;
}

/*! Translate from a cligen variable type to a yang type
 */
char *
cv2yang_type(enum cv_type cv_type)
{
    char                *ytype;
    const char          *str;

    ytype = "empty";
    /* built-in types */
    if ((str = clicon_int2str(ytmap, cv_type)) != NULL)
	return (char*)str;

    /* special derived types */
    if (cv_type == CGV_IPV4ADDR) /* RFC6991 */
	return "ipv4-address";

    if (cv_type == CGV_IPV6ADDR) /* RFC6991 */
	return "ipv6-address";

    if (cv_type == CGV_IPV4PFX) /* RFC6991 */
	return "ipv4-prefix";

    if (cv_type == CGV_IPV6PFX) /* RFC6991 */
	return "ipv6-prefix";

    if (cv_type == CGV_TIME) /* RFC6991 */
	return "date-and-time";

    if (cv_type == CGV_MACADDR) /* RFC6991 */
	return "mac-address";

    if (cv_type == CGV_UUID) /* RFC6991 */
	return "uuid";

    return ytype;
}

/*! Translate from yang type -> cligen type, after yang resolve has been made.
 * handle case where yang resolve did not succedd (rtype=NULL) and then try
 * to find special cligen types such as ipv4addr.
 * not true yang types
 * @param[in]  origtype Name of original type
 * @param[in]  restype  Resolved type. may be null, in that case origtype is used
 * @param[out] cvtype   Translation from resolved type 
 * @note Thereis a kludge for handling direct translations of native cligen types
 */
int
clicon_type2cv(char         *origtype, 
	       char         *restype, 
	       enum cv_type *cvtype)
{
    int retval = -1;

    *cvtype = CGV_ERR;
    if (restype != NULL){ 
	yang2cv_type(restype, cvtype);
	if (*cvtype == CGV_ERR){
	    clicon_err(OE_DB, 0, "%s: \"%s\" type not translated", __FUNCTION__, restype);
	    goto done;
	}
    }
    else{
	/* 
	 * Not resolved, but we can use special cligen types, eg ipv4addr 
	 * Note this is a kludge or at least if we intend of using rfc types
	 */
	yang2cv_type(origtype, cvtype);
	if (*cvtype == CGV_ERR){
	    clicon_err(OE_DB, 0, "%s: \"%s\": type not resolved", __FUNCTION__, origtype);
	    goto done;
	}
    }
    retval = 0;
  done:
    return retval;
}

/* cf cligen/cligen_var.c */
#define range_check(i, rmin, rmax, type)       \
    ((rmin && (i) < cv_##type##_get(rmin)) ||  \
     (rmax && (i) > cv_##type##_get(rmax)))


/*!
 * @retval -1  Error (fatal), with errno set to indicate error
 * @retval 0   Validation not OK, malloced reason is returned. Free reason with free()
 * @retval 1   Validation OK
 */
static int
cv_validate1(cg_var      *cv,
	     enum cv_type cvtype, 
	     int          options, 
	     cg_var      *range_min,
	     cg_var      *range_max, 
	     char        *pattern,
	     yang_stmt   *yrestype,
	     char        *restype,
	     char       **reason)
{
    int             retval = 1; /* OK */
    int             retval2;
    yang_stmt      *yi = NULL;
    uint64_t        u = 0;
    int64_t         i = 0;
    char           *str;

    if (reason && *reason){
	free(*reason);
	*reason = NULL;
    }
    switch (cvtype){
    case CGV_INT8:
	if ((options & YANG_OPTIONS_RANGE) != 0){
	    i = cv_int8_get(cv);
	    if (range_check(i, range_min, range_max, int8)){
		if (reason)
		    *reason = cligen_reason("Number out of range: %ld", i);
		retval = 0;
		break;
	    }
	}
	break;
    case CGV_INT16:
	if ((options & YANG_OPTIONS_RANGE) != 0){
	    i = cv_int16_get(cv);
	    if (range_check(i, range_min, range_max, int16)){
		if (reason)
		    *reason = cligen_reason("Number out of range: %ld", i);
		retval = 0;
		break;
	    }
	}
	break;
    case CGV_INT32:
	if ((options & YANG_OPTIONS_RANGE) != 0){
	    i = cv_int32_get(cv);
	    if (range_check(i, range_min, range_max, int32)){
		if (reason)
		    *reason = cligen_reason("Number out of range: %ld", i);
		retval = 0;
		break;
	    }
	}
	break;
    case CGV_INT64:
	if ((options & YANG_OPTIONS_RANGE) != 0){
	    i = cv_int64_get(cv);
	    if (range_check(i, range_min, range_max, int64)){
		if (reason)
		    *reason = cligen_reason("Number out of range: %ld", i);
		retval = 0;
		break;
	    }
	}
	break;
    case CGV_UINT8:
	if ((options & YANG_OPTIONS_RANGE) != 0){
	    u = cv_uint8_get(cv);
	    if (range_check(u, range_min, range_max, uint8)){
		if (reason)
		    *reason = cligen_reason("Number out of range: %lu", u);
		retval = 0;
		break;
	    }
	}
	break;
    case CGV_UINT16:
	if ((options & YANG_OPTIONS_RANGE) != 0){
	    u = cv_uint16_get(cv);
	    if (range_check(u, range_min, range_max, uint16)){
		if (reason)
		    *reason = cligen_reason("Number out of range: %lu", u);
		retval = 0;
		break;
	    }
	}
	break;
    case CGV_UINT32:
	if ((options & YANG_OPTIONS_RANGE) != 0){
	    u = cv_uint32_get(cv);
	    if (range_check(u, range_min, range_max, uint32)){
		if (reason)
		    *reason = cligen_reason("Number out of range: %lu", u);
		retval = 0;
		break;
	    }
	}
	break;
    case CGV_UINT64:
	if ((options & YANG_OPTIONS_RANGE) != 0){
	    u = cv_uint64_get(cv);
	    if (range_check(u, range_min, range_max, uint64)){
		if (reason)
		    *reason = cligen_reason("Number out of range: %lu", u);
		retval = 0;
		break;
	    }
	}
	break;
    case CGV_DEC64:
	if ((options & YANG_OPTIONS_RANGE) != 0){
	    i = cv_int64_get(cv);
	    if (range_check(i, range_min, range_max, int64)){
		if (reason)
		    *reason = cligen_reason("Number out of range: %ld", i);
		retval = 0;
		break;
	    }
	}
	break;
    case CGV_STRING:
    case CGV_REST:
	str = cv_string_get(cv);
	if (restype && 
	    (strcmp(restype, "enumeration") == 0 || strcmp(restype, "bits") == 0)){
	    int found = 0;
	    while ((yi = yn_each((yang_node*)yrestype, yi)) != NULL){
		if (yi->ys_keyword != Y_ENUM && yi->ys_keyword != Y_BIT)
		    continue;
		if (strcmp(yi->ys_argument, str) == 0){
		    found++;
		    break;
		}
	    }
	    if (!found){
		if (reason)
		    *reason = cligen_reason("'%s' does not match enumeration", str);
		retval = 0;
		break;
	    }
	}
	if ((options & YANG_OPTIONS_LENGTH) != 0){
	    u = strlen(str);
	    if (range_check(u, range_min, range_max, uint64)){
		if (reason)
		    *reason = cligen_reason("string length out of range: %lu", u);
		retval = 0;
		break;
	    }
	}
	if ((options & YANG_OPTIONS_PATTERN) != 0){
	    if ((retval2 = match_regexp(str, pattern)) < 0){
		clicon_err(OE_DB, 0, "match_regexp: %s", pattern);
		return -1;
	    }
	    if (retval2 == 0){
		if (reason)
		    *reason = cligen_reason("regexp match fail: \"%s\" does not match %s",
					    str, pattern);
		retval = 0;
		break;
	    }
	}
	break;
    case CGV_ERR:
    case CGV_VOID:
	retval = 0;
	if (reason)
	    *reason = cligen_reason("Invalid cv");
	retval = 0;
	break;
    case CGV_BOOL:
    case CGV_INTERFACE:
    case CGV_IPV4ADDR: 
    case CGV_IPV6ADDR: 
    case CGV_IPV4PFX: 
    case CGV_IPV6PFX: 
    case CGV_MACADDR:
    case CGV_URL: 
    case CGV_UUID: 
    case CGV_TIME: 
    case CGV_EMPTY:  /* XXX */
	break;
    }

    if (reason && *reason)
	assert(retval == 0); /* validation failed with error reason */
    return retval;
}

/* Forward */
static int ys_cv_validate_union(yang_stmt *ys, char **reason, yang_stmt *yrestype,
				char *type, char *val);

/*!
 * @retval -1  Error (fatal), with errno set to indicate error
 * @retval 0   Validation not OK, malloced reason is returned. Free reason with free()
 * @retval 1   Validation OK
 */
static int
ys_cv_validate_union_one(yang_stmt *ys,
			 char     **reason,
			 yang_stmt *yt,
			 char      *type,  /* orig type */
			 char      *val)
{
    int          retval = -1;
    yang_stmt   *yrt;      /* union subtype */
    int          options = 0;
    cg_var      *range_min = NULL; 
    cg_var      *range_max = NULL; 
    char        *pattern = NULL;
    uint8_t      fraction = 0; 
    char        *restype;
    enum cv_type cvtype;
    cg_var      *cvt=NULL;

    if (yang_type_resolve(ys, yt, &yrt, 
			  &options, &range_min, &range_max, &pattern, 
			  &fraction) < 0)
	goto done;
    restype = yrt?yrt->ys_argument:NULL;
    if (restype && strcmp(restype, "union") == 0){      /* recursive union */
	if ((retval = ys_cv_validate_union(ys, reason, yrt, type, val)) < 0)
	    goto done;
    }
    else {
	if (clicon_type2cv(type, restype, &cvtype) < 0)
	    goto done;
	/* reparse value with the new type */
	if ((cvt = cv_new(cvtype)) == NULL){
	    clicon_err(OE_UNIX, errno, "cv_new");
	    goto done;
	}
	if (cv_parse(val, cvt) <0){
	    clicon_err(OE_UNIX, errno, "cv_parse");
	    goto done;
	}
	if ((retval = cv_validate1(cvt, cvtype, options, range_min, range_max, 
				   pattern, yrt, restype, reason)) < 0)
	    goto done;
    }
 done:
    if (cvt)
	cv_free(cvt);
    return retval;
}

/*!
 * @retval -1  Error (fatal), with errno set to indicate error
 * @retval 0   Validation not OK, malloced reason is returned. Free reason with free()
 * @retval 1   Validation OK
 */
static int
ys_cv_validate_union(yang_stmt *ys,
		     char     **reason,
		     yang_stmt *yrestype,
		     char      *type,  /* orig type */
		     char      *val)
{
    int        retval = 1; /* valid */
    yang_stmt *yt = NULL;

    while ((yt = yn_each((yang_node*)yrestype, yt)) != NULL){
	if (yt->ys_keyword != Y_TYPE)
	    continue;
	if ((retval = ys_cv_validate_union_one(ys, reason, yt, type, val)) < 0)
	    goto done;
	if (retval == 1) /* Enough that one type validates value */
	    break;
    }
 done:
    return retval;
}

/*! Validate cligen variable cv using yang statement as spec
 *
 * @param[in]  cv      A cligen variable to validate. This is a correctly parsed cv.
 * @param[in]  ys      A yang statement, must be leaf of leaf-list.
 * @param[out] reason  If given, and if return value is 0, contains a malloced string
 *                      describing the reason why the validation failed. Must be freed.
 * @retval -1  Error (fatal), with errno set to indicate error
 * @retval 0   Validation not OK, malloced reason is returned. Free reason with free()
 * @retval 1   Validation OK
 * See also cv_validate - the code is similar.
 */
int
ys_cv_validate(cg_var    *cv, 
	       yang_stmt *ys, 
	       char     **reason)
{
    int             retval = -1; 
    cg_var         *ycv;        /* cv of yang-statement */  
    int             options = 0;
    cg_var         *range_min = NULL; 
    cg_var         *range_max = NULL; 
    char           *pattern = NULL;
    enum cv_type    cvtype;
    char           *type;  /* orig type */
    yang_stmt      *yrestype; /* resolved type */
    char           *restype;
    uint8_t         fraction = 0; 
    int             retval2;
    char           *val;
    cg_var         *cvt=NULL;

    if (reason)
	*reason=NULL;
    if (ys->ys_keyword != Y_LEAF && ys->ys_keyword != Y_LEAF_LIST){
	retval = 1;
	goto done;
    }
    ycv = ys->ys_cv;
    if (yang_type_get(ys, &type, &yrestype, 
		      &options, &range_min, &range_max, &pattern,
		      &fraction) < 0)
	goto done;
    restype = yrestype?yrestype->ys_argument:NULL;
    if (clicon_type2cv(type, restype, &cvtype) < 0)
	goto done;

    if (cv_type_get(ycv) != cvtype){
	/* special case: dbkey has rest syntax-> cv but yang cant have that */
	if (cvtype == CGV_STRING && cv_type_get(ycv) == CGV_REST)
	    ;
	else {
	    clicon_err(OE_DB, 0, "%s: Type mismatch data:%s != yang:%s", 
		       __FUNCTION__, cv_type2str(cvtype), cv_type2str(cv_type_get(ycv)));
	    goto done;
	}
    }
    /* Note restype can be NULL here for example with unresolved hardcoded uuid */
    if (restype && strcmp(restype, "union") == 0){ 
	assert(cvtype == CGV_REST);
	val = cv_string_get(cv);
	if ((retval2 = ys_cv_validate_union(ys, reason, yrestype, type, val)) < 0)
	    goto done;
	retval = retval2; /* invalid (0) with latest reason or valid 1 */
    }
    else
	if ((retval = cv_validate1(cv, cvtype, options, range_min, range_max, pattern,
				yrestype, restype, reason)) < 0)
	    goto done;
  done:
    if (cvt)
	cv_free(cvt);
    return retval;
}

/*
 * a typedef can be under module, submodule, container, list, grouping, rpc, 
 * input, output, notification
 */
static inline int
ys_typedef(yang_stmt *ys)
{
    return ys->ys_keyword == Y_MODULE || ys->ys_keyword == Y_SUBMODULE ||
	ys->ys_keyword == Y_CONTAINER || ys->ys_keyword == Y_LIST;
}

/* find next ys up which can contain a typedef */
static yang_stmt *
ys_typedef_up(yang_stmt *ys)
{
    yang_node *yn;

    while (ys != NULL && !ys_typedef(ys)){
	yn = ys->ys_parent;
	/* Some extra stuff to ensure ys is a stmt */
	if (yn && yn->yn_keyword == Y_SPEC)
	    yn = NULL;
	ys = (yang_stmt*)yn;
    }
    /* Here it is either NULL or is a typedef-kind yang-stmt */
    return (yang_stmt*)ys;
}

/*! Return yang-stmt of identity
  This is a sanity check of base identity of identity-ref and for identity 
  statements.

  Return true if node is identityref and is derived from identity_name
  The derived-from() function returns true if the (first) node (in
   document order in the argument "nodes") is a node of type identityref,
   and its value is an identity that is derived from the identity
   "identity-name" defined in the YANG module "module-name"; otherwise
   it returns false.

 Valid values for an identityref are any identities derived from the
   identityref's base identity. 
   1. (base) identity must exist (be found). This is a sanity check
     of the specification and also necessary for identity statements.
   2. Check if a given node has value derived from base identity. This is
      a run-time check necessary when validating eg netconf.
   3. Find all valid derived identities from a identityref base identity.
     This is for cli generation.
   S� vad �r det denna function ska g�ra? Svar: 1
*/
yang_stmt *
yang_find_identity(yang_stmt *ys, 
		   char      *identity)
{
    char        *id;
    char        *prefix = NULL;
    yang_stmt   *ymodule;
    yang_stmt   *yid = NULL;
    yang_node   *yn;

    if ((id = strchr(identity, ':')) == NULL)
	id = identity;
    else{
	prefix = strdup(identity);
	prefix[id-identity] = '\0';
	id++;
    }
    /* No, now check if identityref is derived from base */
    if (prefix){ /* Go to top and find import that matches */
	if ((ymodule = yang_find_module_by_prefix(ys, prefix)) == NULL)
	    goto done;
	yid = yang_find((yang_node*)ymodule, Y_IDENTITY, id);
    }
    else{
	while (1){
	    /* Check upwards in hierarchy for matching typedefs */
	    if ((ys = ys_typedef_up(ys)) == NULL) /* If reach top */
		break;
	    /* Here find identity */
	    if ((yid = yang_find((yang_node*)ys, Y_IDENTITY, id)) != NULL)
		break;
	    /* Did not find a matching typedef there, proceed to next level */
	    yn = ys->ys_parent;
	    if (yn && yn->yn_keyword == Y_SPEC)
		yn = NULL;
	    ys = (yang_stmt*)yn;
	}
    }
  done:
    if (prefix)
	free(prefix);
    return yid;
}

/*
 */
static int
resolve_restrictions(yang_stmt   *yrange,
		     yang_stmt   *ylength,
		     yang_stmt   *ypattern,
		     yang_stmt   *yfraction,
		     int         *options, 
		     cg_var     **mincv, 
		     cg_var     **maxcv, 
		     char       **pattern,
		     uint8_t     *fraction)
{
    if (options && mincv && maxcv && yrange != NULL){
	*mincv = cvec_find(yrange->ys_cvec, "range_min");
	*maxcv = cvec_find(yrange->ys_cvec, "range_max");
	*options  |= YANG_OPTIONS_RANGE;
    }
    if (options && mincv && maxcv && ylength != NULL){
	*mincv = cvec_find(ylength->ys_cvec, "range_min"); /* XXX fel typ */
	*maxcv = cvec_find(ylength->ys_cvec, "range_max");
	*options  |= YANG_OPTIONS_LENGTH;
    }
    if (options && pattern && ypattern != NULL){
	*pattern   = ypattern->ys_argument;
	*options  |= YANG_OPTIONS_PATTERN;
    }
    if (options && fraction && yfraction != NULL){
	*fraction  = cv_uint8_get(yfraction->ys_cv);
	*options  |= YANG_OPTIONS_FRACTION_DIGITS;
    }
    return 0;
}

/*! Recursively resolve a yang type to built-in type with optional restrictions
 * @param [in]  ys       yang-stmt from where the current search is based
 * @param [in]  ytype    yang-stmt object containing currently resolving type
 * @param [out] yrestype resolved type. return built-in type or NULL. mandatory
 * @param [out] options  pointer to flags field of optional values. optional
 * @param [out] mincv    pointer to cv with min range or length. If options&YANG_OPTIONS_RANGE
 * @param [out] maxcv    pointer to cv with max range or length. If options&YANG_OPTIONS_RANGE
 * @param [out] pattern  pointer to static string of yang string pattern. optional
 * @param [out] fraction for decimal64, how many digits after period
 * @retval      0        OK. Note yrestype may still be NULL.
 * @retval     -1        Error, clicon_err handles errors
 * The setting of the options argument has the following semantics:
 *   options&YANG_OPTIONS_RANGE or YANG_OPTIONS_LENGTH --> mincv and max _can_ be set
 *   options&YANG_OPTIONS_PATTERN --> pattern is set
 *   options&YANG_OPTIONS_FRACTION_DIGITS --> fraction is set
 * Note that the static output strings (type, pattern) should be copied if used asap.
 * Note also that for all pointer arguments, if NULL is given, no value is assigned.
 */
int 
yang_type_resolve(yang_stmt   *ys, 
		  yang_stmt   *ytype, 
		  yang_stmt  **yrestype, 
		  int         *options, 
		  cg_var     **mincv, 
		  cg_var     **maxcv, 
		  char       **pattern,
		  uint8_t     *fraction)
{
    yang_stmt   *rytypedef = NULL; /* Resolved typedef of ytype */
    yang_stmt   *rytype;           /* Resolved type of ytype */
    yang_stmt   *yrange;
    yang_stmt   *ylength;
    yang_stmt   *ypattern;
    yang_stmt   *yfraction;
    char        *type;
    char        *prefix = NULL;
    int          retval = -1;
    yang_node   *yn;
    yang_stmt   *ymod;

    if (options)
	*options = 0x0;
    *yrestype    = NULL; /* Initialization of resolved type that may not be necessary */
    type      = ytype_id(ytype);     /* This is the type to resolve */
    prefix    = ytype_prefix(ytype); /* And this its prefix */
    /* Cache does not work for eg string length 32 */
    if (!yang_builtin(type) && ytype->ys_typecache != NULL){
	if (yang_type_cache_get(ytype->ys_typecache, 
				yrestype, options, mincv, maxcv, pattern, fraction) < 0)
	    goto done;
	goto ok;
    }
    yrange    = yang_find((yang_node*)ytype, Y_RANGE, NULL);
    ylength   = yang_find((yang_node*)ytype, Y_LENGTH, NULL);
    ypattern  = yang_find((yang_node*)ytype, Y_PATTERN, NULL);
    yfraction = yang_find((yang_node*)ytype, Y_FRACTION_DIGITS, NULL);

    /* Check if type is basic type. If so, return that */
    if (prefix == NULL && yang_builtin(type)){
	*yrestype = ytype; 
	resolve_restrictions(yrange, ylength, ypattern, yfraction, options, 
			     mincv, maxcv, pattern, fraction);
	goto ok;
    }

    /* Not basic type. Now check if prefix which means we look in other module */
    if (prefix){ /* Go to top and find import that matches */
	if ((ymod = yang_find_module_by_prefix(ys, prefix)) == NULL){
	    clicon_err(OE_DB, 0, "Type not resolved: %s:%s", prefix, type);
	    goto done;
	}
	if ((rytypedef = yang_find((yang_node*)ymod, Y_TYPEDEF, type)) == NULL)
	    goto ok; /* unresolved */
    }
    else
	while (1){
	    /* Check upwards in hierarchy for matching typedefs */
	    if ((ys = ys_typedef_up(ys)) == NULL){ /* If reach top */
		*yrestype = NULL;
		break;
	    }
	    /* Here find typedef */
	    if ((rytypedef = yang_find((yang_node*)ys, Y_TYPEDEF, type)) != NULL)
		break;
	    /* Did not find a matching typedef there, proceed to next level */
	    yn = ys->ys_parent;
	    if (yn && yn->yn_keyword == Y_SPEC)
		yn = NULL;
	    ys = (yang_stmt*)yn;
	}
    if (rytypedef != NULL){     /* We have found a typedef */
	/* Find associated type statement */
	if ((rytype = yang_find((yang_node*)rytypedef, Y_TYPE, NULL)) == NULL){
	    clicon_err(OE_DB, 0, "%s: mandatory type object is not found", __FUNCTION__);
	    goto done;
	}
	/* recursively resolve this new type */
	if (yang_type_resolve(ys, rytype, yrestype, 
			      options, mincv, maxcv, pattern, fraction) < 0)
	    goto done;
	/* overwrites the resolved if any */
	resolve_restrictions(yrange, ylength, ypattern, yfraction, 
			     options, mincv, maxcv, pattern, fraction);
    }

  ok:
    retval = 0;
  done:
    if (prefix)
	free(prefix);
    return retval;
}

/*! Get type information about a leaf/leaf-list yang-statement
 *
 * @code
 *   yang_stmt    *yrestype;
 *   int           options;
 *   int64_t       min, max;
 *   char         *pattern;
 *   uint8_t       fraction;
 *
 *   if (yang_type_get(ys, &type, &yrestype, &options, &min, &max, &pattern, &fraction) < 0)
 *      goto err;
 *   if (yrestype == NULL) # unresolved
 *      goto err;
 *   if (options & YANG_OPTIONS_LENGTH != 0)
 *      printf("%d..%d\n", min , max);
 *   if (options & YANG_OPTIONS_PATTERN != 0)
 *      printf("regexp: %s\n", pattern);
 * @endcode
 * @param [in]  ys       yang-stmt, leaf or leaf-list
 * @param [out] origtype original type may be derived or built-in
 * @param [out] yrestype pointer to resolved type stmt. should be built-in or NULL
 * @param [out] options  pointer to flags field of optional values
 * @param [out] mincv    pointer to cv of min range or length. optional
 * @param [out] maxcv    pointer to cv of max range or length. optional
 * @param [out] pattern  pointer to static string of yang string pattern. optional
 * @param [out] fraction for decimal64, how many digits after period
 * @retval      0        OK, but note that restype==NULL means not resolved.
 * @retval     -1        Error, clicon_err handles errors
 * The setting of the options argument has the following semantics:
 *   options&YANG_OPTIONS_RANGE or YANG_OPTIONS_LENGTH --> mincv and max _can_ be set
 *   options&YANG_OPTIONS_PATTERN --> pattern is set
 *   options&YANG_OPTIONS_FRACTION_DIGITS --> fraction is set
 * Note that the static output strings (type, pattern) should be copied if used asap.
 * Note also that for all pointer arguments, if NULL is given, no value is assigned.
 * @See yang_type_resolve(). This function is really just a frontend to that.
 */
int 
yang_type_get(yang_stmt    *ys, 
	      char        **origtype, 
	      yang_stmt   **yrestype, 
	      int          *options, 
	      cg_var      **mincv, 
	      cg_var      **maxcv, 
	      char        **pattern,
	      uint8_t      *fraction
    )
{
    int retval = -1;
    yang_stmt    *ytype;        /* type */
    char         *type;

    if (options)
	*options = 0x0;
    /* Find mandatory type */
    if ((ytype = yang_find((yang_node*)ys, Y_TYPE, NULL)) == NULL){
	clicon_err(OE_DB, 0, "%s: mandatory type object is not found", __FUNCTION__);
	goto done;
    }
    /* XXX: here we seem to have some problems if type is union */
    type = ytype_id(ytype);
    if (origtype)
	*origtype = type;
    if (yang_type_resolve(ys, ytype, yrestype, 
			  options, mincv, maxcv, pattern, fraction) < 0)
	goto done;
    clicon_debug(3, "%s: %s %s->%s", __FUNCTION__, ys->ys_argument, type, 
		 *yrestype?(*yrestype)->ys_argument:"null");
    retval = 0;
  done:
    return retval;
}

