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
  use your version of this file under the terms of Apache License version 2, indicate
  your decision by deleting the provisions above and replace them with the 
  notice and other provisions required by the GPL. If you do not delete
  the provisions above, a recipient may use your version of this file under
  the terms of any one of the Apache License version 2 or the GPL.

  ***** END LICENSE BLOCK *****

 * Clixon XML XPath 1.0 according to https://www.w3.org/TR/xpath-10
 * See XPATH_LIST_OPTIMIZE
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
#include <syslog.h>
#include <fcntl.h>
#include <math.h> /* NaN */

/* cligen */
#include <cligen/cligen.h>

/* clixon */
#include "clixon_string.h"
#include "clixon_queue.h"
#include "clixon_hash.h"
#include "clixon_handle.h"
#include "clixon_err.h"
#include "clixon_log.h"
#include "clixon_debug.h"
#include "clixon_yang.h"
#include "clixon_xml.h"
#include "clixon_xml_vec.h"
#include "clixon_xml_sort.h"
#include "clixon_xpath_ctx.h"
#include "clixon_xpath.h"
#include "clixon_xpath_optimize.h"

#ifdef XPATH_LIST_OPTIMIZE
static xpath_tree *_xmtop = NULL; /* pattern match tree top */
static xpath_tree *_xm = NULL;
static xpath_tree *_xe = NULL;
static int _optimize_enable = 1;
static int _optimize_hits = 0;
#endif /* XPATH_LIST_OPTIMIZE */

/* XXX development in clixon_xpath_eval */
int
xpath_list_optimize_stats(int *hits)
{
#ifdef XPATH_LIST_OPTIMIZE
    *hits = _optimize_hits;
    _optimize_hits = 0;
#endif
    return 0;
}

/*! Enable xpath optimize
 *
 * Cant replace this with option since there is no handle in xpath functions,...
 */
int
xpath_list_optimize_set(int enable)
{
#ifdef XPATH_LIST_OPTIMIZE
    _optimize_enable = enable;
#endif
    return 0;
}

void
xpath_optimize_exit(void)
{
#ifdef XPATH_LIST_OPTIMIZE
    if (_xmtop)
        xpath_tree_free(_xmtop);
#endif
}

#ifdef XPATH_LIST_OPTIMIZE
/*! Initialize xpath module
 *
 * XXX move to clixon_xpath.c 
 * @see loop_preds
 */
int
xpath_optimize_init(xpath_tree **xm,
                    xpath_tree **xe)
{
    int         retval = -1;
    xpath_tree *xs;

    if (_xm == NULL){
        /* Initialize xpath-tree */
        if (xpath_parse("_x[_y='_z']", &_xmtop) < 0)
            goto done;
        /* Go down two steps */
        if ((_xm = xpath_tree_traverse(_xmtop, 0, 0, -1)) == NULL)
            goto done;
        /* get nodetest tree (_x) */
        if ((xs = xpath_tree_traverse(_xm, 0, -1)) == NULL)
            goto done;
        xs->xs_match++;
        /* get predicates [_y=_z][z=2] */
        if ((xs = xpath_tree_traverse(_xm, 1, -1)) == NULL)
            goto done;
        xs->xs_match++;
        /* get expression [_y=_z] */
        if ((_xe = xpath_tree_traverse(xs, 1, -1)) == NULL)
            goto done;
        /* get keyname (_y) */
        if ((xs = xpath_tree_traverse(_xe, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1)) == NULL)
            goto done;
        xs->xs_match++; /* in loop_preds get name in xs_s1 XXX: leaf-list is different */
        /* get keyval (_z) */
        if ((xs = xpath_tree_traverse(_xe, 0, 0, 1, 0, 0, 0, 0, -1)) == NULL)
            goto done;
        xs->xs_match++; /* in loop_preds get value in xs_s0 or xs_strnr */
    }
    *xm = _xm;
    *xe = _xe;
    retval = 0;
 done:
    return retval;
}


/*! Recursive function to loop over all EXPR and pattern match them
 *
 * @param[in]  xt    XPath tree of type PRED
 * @param[in]  xepat Pattern matching XPath tree of type EXPR
 * @param[out] cvk   Vector of <keyname>:<keyval> pairs
 * @retval     1     Match
 * @retval     0     No match
 * @retval    -1     Error
 * @see xpath_optimize_init
 */
static int
loop_preds(xpath_tree *xt,
           xpath_tree *xepat,
           cvec       *cvk)
{
    int          retval = -1;
    int          ret;
    xpath_tree  *xe;
    xpath_tree **vec = NULL;
    size_t       veclen = 0;
    cg_var      *cvi;

    if (xt->xs_type == XP_PRED && xt->xs_c0){
        if ((ret = loop_preds(xt->xs_c0, xepat, cvk)) < 0)
            goto done;
        if (ret == 0)
            goto ok;
    }
    if ((xe = xt->xs_c1) && (xe->xs_type == XP_EXP)){
        if ((ret = xpath_tree_eq(xepat, xe, &vec, &veclen)) < 0)
            goto done;
        if (ret == 0)
            goto ok;
        if (veclen != 2)
            goto ok;
        if ((cvi = cvec_add(cvk, CGV_STRING)) == NULL){
            clixon_err(OE_XML, errno, "cvec_add");
            goto done;
        }
        cv_name_set(cvi, vec[0]->xs_s1);
        if (vec[1]->xs_type == XP_PRIME_NR)
            cv_string_set(cvi, vec[1]->xs_strnr);
        else
            cv_string_set(cvi, vec[1]->xs_s0);
    }
    retval = 1;
 done:
    if (vec)
        free(vec);
    return retval;
 ok: /* no match, not special case */
    retval = 0;
    goto done;
}

/*! Pattern matching to find fastpath
 *
 * @param[in]  xt     XPath tree
 * @param[in]  xv     XML base node
 * @param[out] xvec   Array of found nodes
 * @param[out] xlen   Len of xvec
 * @param[out] key
 * @param[out] keyval
 * @retval     1      Match
 * @retval     0      No match - use non-optimized lookup
 * @retval    -1      Error
 *  XPath:
 *  y[k=3] # corresponds to: <name>[<keyname>=<keyval>]
 */
static int
xpath_list_optimize_fn(xpath_tree  *xt,
                       cxobj       *xv,
                       clixon_xvec *xvec)
{
    int          retval = -1;
    xpath_tree  *xm = NULL;
    xpath_tree  *xem = NULL;
    char        *name;
    yang_stmt   *yp;
    yang_stmt   *yc;
    cvec        *cvv = NULL;
    xpath_tree **vec = NULL;
    size_t       veclen = 0;
    xpath_tree  *xtp;
    int          ret;
    cvec        *cvk = NULL; /* vector of index keys */
    cg_var      *cvi;
    int          i;
    yang_stmt   *ypp;

    /* revert to non-optimized if no yang */
    if ((yp = xml_spec(xv)) == NULL)
        goto ok;
    /* or if not config data (state data should not be ordered) */
    if (yang_config_ancestor(yp) == 0)
        goto ok;
    /* Check that there is no "outer" list. */
    ypp = yp;
    do {
        if (yang_keyword_get(ypp) == Y_LIST)
            goto ok;
    } while((ypp = yang_parent_get(ypp)) != NULL);
    /* Check yang and that only a list with key as index is a special case can do bin search 
     * That is, ONLY check optimize cases of this type:_x[_y='_z']
     * Should we extend this simple example and have more cases (all cases?)
     */
    xpath_optimize_init(&xm, &xem);
    /* Here is where pattern is checked for equality and where variable binding is made (if
     * equal) */
    if ((ret = xpath_tree_eq(xm, xt, &vec, &veclen)) < 0)
        goto done;
    if (ret == 0)
        goto ok; /* no match */
    if (veclen != 2)
        goto ok;
    name = vec[0]->xs_s1;
    /* Extract variables */
    if ((yc = yang_find(yp, Y_LIST, name)) == NULL)
#ifdef NOTYET /* leaf-list is not detected by xpath optimize detection */
        if ((yc = yang_find(yp, Y_LEAF_LIST, name)) == NULL) /* XXX */
#endif
            goto ok;
    /* Validate keys */
    if ((cvv = yang_cvec_get(yc)) == NULL)
        goto ok;
    xtp = vec[1];
    if ((cvk = cvec_new(0)) == NULL){
        clixon_err(OE_YANG, errno, "cvec_new");
        goto done;
    }
    if ((ret = loop_preds(xtp, xem, cvk)) < 0)
        goto done;
    if (ret == 0)
        goto ok;

    if (cvec_len(cvv) != cvec_len(cvk))
        goto ok;
    i = 0;
    cvi = NULL;
    while ((cvi = cvec_each(cvk, cvi)) != NULL) {
        if (strcmp(cv_name_get(cvi), cv_string_get(cvec_i(cvv,i))))
            goto ok;
        i++;
    }
    /* Use 2a form since yc allready given to compute cvk */
    if (clixon_xml_find_index(xv, yp, NULL, name, cvk, xvec) < 0)
        goto done;
    retval = 1; /* match */
 done:
    if (vec)
        free(vec);
    if (cvk)
        cvec_free(cvk);
    return retval;
 ok: /* no match, not special case */
    retval = 0;
    goto done;
}
#endif /* XPATH_LIST_OPTIMIZE */

/*! Identify XPath special cases and if match, use binary search.
 *
 * @retval  1  Optimization made, special case, use x (found if != NULL)
 * @retval  0  Dont optimize: not special case, do normal processing
 * @retval -1  Error
 * XXX Contains glue code between cxobj ** and clixon_xvec code 
 */
int
xpath_optimize_check(xpath_tree *xs,
                     cxobj      *xv,
                     cxobj    ***xvec0,
                     int        *xlen0)
{
#ifdef XPATH_LIST_OPTIMIZE
    int          ret;
    clixon_xvec *xvec = NULL;

    if (!_optimize_enable)
        return 0; /* use regular code */
    if ((xvec = clixon_xvec_new()) == NULL)
        return -1;
    /* Glue code since xpath code uses (old) cxobj ** and search code uses (new) clixon_xvec */
    if ((ret = xpath_list_optimize_fn(xs, xv, xvec)) < 0)
        return -1;
    if (ret == 1){
        if (clixon_xvec_extract(xvec, xvec0, xlen0, NULL) < 0)
            return -1;
        clixon_xvec_free(xvec);
        _optimize_hits++;
        return 1; /* Optimized */
    }
    else{
        clixon_xvec_free(xvec);
        return 0; /* use regular code */
    }
#else
    return 0; /* use regular code */
#endif
}

