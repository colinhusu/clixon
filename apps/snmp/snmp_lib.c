/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2022 Olof Hagsand and Kristofer Hallin

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
  * See RFC 6643
  * Extensions are grouped in some categories, the one I have seen are, example:
  * 1. leaf
  *      smiv2:max-access "read-write";
  *      smiv2:oid "1.3.6.1.4.1.8072.2.1.1";
  *      smiv2:defval "42"; (not always)
  * 2. container, list
  *      smiv2:oid "1.3.6.1.4.1.8072.2.1";	
  * 3. module level
  *      smiv2:alias "netSnmpExamples" {
  *        smiv2:oid "1.3.6.1.4.1.8072.2";
  *

 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <unistd.h>
#include <pwd.h>
#include <syslog.h>
#include <errno.h>
#include <signal.h>
#include <assert.h>
#include <sys/types.h>

/* net-snmp */
#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>
#include <net-snmp/agent/net-snmp-agent-includes.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

#include "snmp_lib.h"

/*
 * Local variables
 */
/* Mapping between yang keyword string <--> clixon constants 
 * Here is also the place where doc on some types store variables (cv)
 */
/* Mapping between smiv2 yang extension access string string <--> netsnmp handler codes (agent_handler.h) 
 * Here is also the place where doc on some types store variables (cv)
 * see netsnmp_handler_registration_create()
 */
static const map_str2int snmp_access_map[] = {
    {"read-only",             HANDLER_CAN_RONLY}, /* HANDLER_CAN_GETANDGETNEXT */
    {"read-write",            HANDLER_CAN_RWRITE}, /* HANDLER_CAN_GETANDGETNEXT | HANDLER_CAN_SET */
    {"not-accessible",        0}, // XXX
    {"accessible-for-notify", 0}, // XXX
    {NULL,                   -1}
};

/* Map between clixon and ASN.1 types. 
 * @see net-snmp/library/asn1.h
 * @see union netsnmp_vardata in net-snmp/types.h
 * XXX not complete
 * XXX TimeTicks
 */
static const map_str2int snmp_type_map[] = {

    {"int32",        ASN_INTEGER},
    {"string",       ASN_OCTET_STR},
    //  {"bool",         ASN_BOOLEAN},
    //  {"empty",        ASN_NULL},
    //  {"bits",         ASN_BIT_STR},
    //  {"", ASN_OBJECT_ID},
    //  {"", ASN_SEQUENCE},
    //  {"", ASN_SET},
    {NULL,           -1}
};

/* Map between SNMP message / mode str and int form
 */
static const map_str2int snmp_msg_map[] = {
    {"MODE_SET_RESERVE1",    MODE_SET_RESERVE1},
    {"MODE_SET_RESERVE2",    MODE_SET_RESERVE2},
    {"MODE_SET_ACTION",      MODE_SET_ACTION},
    {"MODE_SET_COMMIT",      MODE_SET_COMMIT},
    {"MODE_GET",             MODE_GET}, 
    {"MODE_GETNEXT",         MODE_GETNEXT}, 
    {NULL,                   -1}
};

/*! Translate from snmp string to int representation
 * @note Internal snmpd, maybe find something in netsnmpd?
 */
int
snmp_access_str2int(char *modes_str)
{
    return clicon_str2int(snmp_access_map, modes_str);
}

const char *
snmp_msg_int2str(int msg)
{
    return clicon_int2str(snmp_msg_map, msg);
}
/*! Translate from YANG to SNMP asn1.1 type ids (not value)
 *
 * @param[in]    ys         YANG leaf node
 * @param[out]   asn1_type  ASN.1 type id
 * @param[out]   cvtype     Clixon cv type
 * @retval   0   OK
 * @retval   -1  Error
 */
int
yang2snmp_types(yang_stmt    *ys,
		int          *asn1_type,
		enum cv_type *cvtype)
{
    int        retval = -1;
    yang_stmt *yrestype;  /* resolved type */
    char      *restype;  /* resolved type */
    char      *origtype=NULL;   /* original type */
    int        at;

    /* Get yang type of leaf and trasnslate to ASN.1 */
    if (yang_type_get(ys, &origtype, &yrestype, NULL, NULL, NULL, NULL, NULL) < 0)
	goto done;
    restype = yrestype?yang_argument_get(yrestype):NULL;
    /* translate to asn.1 */
    if ((at = clicon_str2int(snmp_type_map, restype)) < 0){
	clicon_err(OE_YANG, 0, "No snmp translation for YANG %s type:%s",
		   yang_argument_get(ys), restype);
	//	goto done;
    }
    if (asn1_type)
	*asn1_type = at;
    if (cvtype && clicon_type2cv(origtype, restype, ys, cvtype) < 0)
	goto done;
    clicon_debug(1, "%s type:%s", __FUNCTION__, restype);
    retval = 0;
 done:
    return retval;
}

/*! Translate from yang/xml/clixon to SNMP/ASN.1
 *
 * @param[in]   valstr   Clixon/yang/xml string value
 * @param[in]   cvtype   Type of clixon type
 * @param[in]   reqinfo  snmpd API struct for error
 * @param[in]   requests snmpd API struct for error
 * @param[out]  snmpval  Malloc:ed snmp type
 * @param[out]  snmplen  Length of snmp type
 * @retval      1        OK
 * @retval      0        Invalid value or type
 * @retval      -1       Error
 */
int
type_yang2snmp(char                       *valstr,
	       enum cv_type                cvtype,
	       netsnmp_agent_request_info *reqinfo,
	       netsnmp_request_info       *requests,
	       u_char                    **snmpval,
	       size_t                     *snmplen)
{
    int     retval = -1;
    int     ret;
    char   *reason = NULL;
    size_t  cvlen;
    cg_var *cv;

    clicon_debug(1, "%s", __FUNCTION__);
    if (snmpval == NULL || snmplen == NULL){
	clicon_err(OE_UNIX, EINVAL, "snmpval or snmplen is NULL");
	goto done;
    }
    if ((cv = cv_new(cvtype)) == NULL){
	clicon_err(OE_UNIX, errno, "cv_new");
	goto done; 
    }
    if ((ret = cv_parse1(valstr, cv, &reason)) < 0)
	goto done;
    if (ret == 0){
	clicon_debug(1, "%s %s", __FUNCTION__, reason);
	netsnmp_set_request_error(reqinfo, requests, SNMP_ERR_WRONGTYPE);
	goto fail;
    }
    cvlen = cv_len(cv);
    if ((*snmpval = malloc(cvlen)) == NULL){
	clicon_err(OE_UNIX, errno, "malloc");
	goto done;
    }
    switch (cvtype){
    case CGV_INT32:{
	int i = cv_int32_get(cv);
	memcpy(*snmpval, &i, cvlen);
	*snmplen = cvlen;
	break;
    }
    case CGV_STRING:{
	strcpy((char*)*snmpval, cv_string_get(cv));
	*snmplen = cvlen;
	break;
    }
    default:
	assert(0); // XXX
	clicon_debug(1, "%s %s not supported", __FUNCTION__, cv_type2str(cvtype));
	netsnmp_set_request_error(reqinfo, requests, SNMP_ERR_WRONGTYPE);
	goto fail;
	break;
    }
    retval = 1;
 done:
    clicon_debug(1, "%s %d", __FUNCTION__, retval);
    if (reason)
	free(reason);
    return retval;
 fail:
    retval = 0;
    goto done;
}
/*! Translate from yang/xml/clixon to SNMP/ASN.1
 *
 * @param[in]   snmpval  Malloc:ed snmp type
 * @param[in]   snmplen  Length of snmp type
 * @param[in]   reqinfo  snmpd API struct for error
 * @param[in]   requests snmpd API struct for error
 * @param[out]  valstr   Clixon/yang/xml string value, free after use)
 * @retval      1        OK, and valstr set
 * @retval      0        Invalid value or type
 * @retval      -1       Error
 */
int
type_snmp2yang(netsnmp_variable_list      *requestvb,
	       netsnmp_agent_request_info *reqinfo,
	       netsnmp_request_info       *requests,
	       char                      **valstr)
{
    int          retval = -1;
    char        *cvtypestr;
    enum cv_type cvtype;
    cg_var      *cv;

    clicon_debug(1, "%s", __FUNCTION__);
    if (valstr == NULL){
	clicon_err(OE_UNIX, EINVAL, "valstr is NULL");
	goto done;
    }
    cvtypestr = (char*)clicon_int2str(snmp_type_map, requestvb->type);
    cvtype = cv_str2type(cvtypestr);
    if ((cv = cv_new(cvtype)) == NULL){
	clicon_err(OE_UNIX, errno, "cv_new");
	goto done; 
    }
    switch (requestvb->type){
    case ASN_BOOLEAN:
    case ASN_INTEGER:
	cv_int32_set(cv, *requestvb->val.integer);
	break;
    case ASN_OCTET_STR:
	cv_string_set(cv, (char*)requestvb->val.string);
	break;
    default:
	assert(0); // XXX
	clicon_debug(1, "%s %s not supported", __FUNCTION__, cv_type2str(cvtype));
	netsnmp_set_request_error(reqinfo, requests, SNMP_ERR_WRONGTYPE);
	goto fail;
	break;
    }
    if ((*valstr = cv2str_dup(cv)) == NULL){
	clicon_err(OE_UNIX, errno, "cv2str_dup");
	goto done;
    }
    retval = 1;
 done:
    clicon_debug(1, "%s %d", __FUNCTION__, retval);
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Construct an xpath from yang statement, internal fn using cb
 * Recursively construct it to the top.
 * @param[in]  ys    Yang statement
 * @param[out] cb    xpath as cbuf
 * @retval     0     OK
 * @retval    -1     Error
 * @see yang2xpath
 */ 
static int
yang2xpath_cb(yang_stmt *ys, 
	      cbuf      *cb)
{
    yang_stmt *yp; /* parent */
    int        i;
    cvec      *cvk = NULL; /* vector of index keys */
    int        retval = -1;
    
    if ((yp = yang_parent_get(ys)) == NULL){
	clicon_err(OE_YANG, EINVAL, "yang expected parent %s", yang_argument_get(ys));
	goto done;
    }
    if (yp != NULL && /* XXX rm */
	yang_keyword_get(yp) != Y_MODULE && 
	yang_keyword_get(yp) != Y_SUBMODULE){

	if (yang2xpath_cb(yp, cb) < 0) /* recursive call */
	    goto done;
	if (yang_keyword_get(yp) != Y_CHOICE && yang_keyword_get(yp) != Y_CASE){
	    cprintf(cb, "/");
	}
    }
    if (yang_keyword_get(ys) != Y_CHOICE && yang_keyword_get(ys) != Y_CASE){
	cprintf(cb, "%s:", yang_find_myprefix(ys));
	cprintf(cb, "%s", yang_argument_get(ys));
    }
    switch (yang_keyword_get(ys)){
    case Y_LIST: // XXX not xpaths
	cvk = yang_cvec_get(ys); /* Use Y_LIST cache, see ys_populate_list() */
	if (cvec_len(cvk))
	    cprintf(cb, "=");
	/* Iterate over individual keys  */
	for (i=0; i<cvec_len(cvk); i++){
	    if (i)
		cprintf(cb, ",");
	    cprintf(cb, "%%s");
	}
	break;
    case Y_LEAF_LIST:
	cprintf(cb, "=%%s");
	break;
    default:
	break;
    } /* switch */
    retval = 0;
 done:
    return retval;
}

/*! Construct an xpath from yang statement
 * Recursively construct it to the top.
 * @param[in]  ys    Yang statement
 * @param[out] xpath Malloced xpath string, use free() after use
 * @retval     0     OK
 * @retval     -1    Error
 * @note
 * 1. This should really be in a core .c file, like clixon_yang, BUT
 * 2. It is far from complete so maybe keep it here as a special case
 */ 
int
yang2xpath(yang_stmt *ys,
	   char     **xpath)
{
    int   retval = -1;
    cbuf *cb = NULL;

    if ((cb = cbuf_new()) == NULL){
	clicon_err(OE_UNIX, errno, "cbuf_new");
	goto done;
    }
    if (yang2xpath_cb(ys, cb) < 0)
	goto done;
    if (xpath && (*xpath = strdup(cbuf_get(cb))) == NULL){
	clicon_err(OE_UNIX, errno, "strdup");
	goto done;
    }
    retval = 0;
 done:
    if (cb)
	cbuf_free(cb);
    return retval;
}

int
clixon_table_create(netsnmp_table_data_set *table, yang_stmt *ys, clicon_handle h)
{
    cvec                   *nsc = NULL;
    cxobj                  *xt = NULL;
    cxobj                  *xerr;
    char                   *xpath;
    cxobj                  *xtable;
    cxobj                  *xe;
    cxobj                  *xleaf;
    int                     i;
    char                   *valstr;
    netsnmp_table_row      *row, *tmprow;
    int                    retval = -1;

    if (xml_nsctx_yang(ys, &nsc) < 0)
        goto done;

    if (yang2xpath(ys, &xpath) < 0)
        goto done;

    if (clicon_rpc_get(h, xpath, nsc, CONTENT_ALL, -1, &xt) < 0)
        goto done;

    if ((xerr = xpath_first(xt, NULL, "/rpc-error")) != NULL){
        clixon_netconf_error(xerr, "clicon_rpc_get", NULL);
        goto done;
    }

    netsnmp_table_dataset_add_index(table, ASN_OCTET_STR);
    netsnmp_table_set_multi_add_default_row(table, 2, ASN_OCTET_STR, 1, NULL, 0, 3, ASN_OCTET_STR, 1, NULL, 0, 0);

    if ((xtable = xpath_first(xt, nsc, "%s", xpath)) != NULL) {
        for (tmprow = table->table->first_row; tmprow; tmprow = tmprow->next)
            netsnmp_table_dataset_remove_and_delete_row(table, tmprow);

        xe = NULL; /* Loop thru entries in table */
        while ((xe = xml_child_each(xtable, xe, CX_ELMNT)) != NULL) {
            row = netsnmp_create_table_data_row();
            xleaf = NULL; /* Loop thru leafs in entry */
            i = 1; /* tableindex start at 1 */
            while ((xleaf = xml_child_each(xe, xleaf, CX_ELMNT)) != NULL) {
                valstr = xml_body(xleaf);
                if (i == 1) // Assume first netry is key XXX should check YANG
                    netsnmp_table_row_add_index(row, ASN_OCTET_STR, valstr, strlen(valstr));
                else{
                    netsnmp_set_row_column(row, i, ASN_OCTET_STR, valstr, strlen(valstr));
                    netsnmp_mark_row_column_writable(row, i, 1);
                }
                i++;
            }

            netsnmp_table_dataset_add_row(table, row);
        }
    }

    retval = 1;

done:
    if (xt)
        xml_free(xt);
    if (nsc)
        xml_nsctx_free(nsc);

    return retval;
}
