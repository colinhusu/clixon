/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2016 Olof Hagsand and Benny Holmgren
  Copyright (C) 2017-2019 Olof Hagsand
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

  * Utility for manipulating XML trees. In all operations, there is a primary base tree x0/xb) and a
  * secondary tree (x1/xs). There are several operations on how to modify the base tree using the 
  * secondary tree. Both x0 and x1 are root trees, whereas xb/xs are subytrees of x0/x1 respectively
  * after path has been applied.
  * This includes:
  * - Insert subtree (last) in list: -b <x0> -x <x1> -p <path>
  *   which gives xb and xs. The first element of xs is inserted under xb
  *   Example: xb := <b><c/></b>; xs := <b><d/></b>
  *        Result is : xb = <b><c/><d/></b>
  * - Merging trees: -o merge -b <base> -x <2nd> -p <path>
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
#include <signal.h>

/* cligen */
#include <cligen/cligen.h>

/* clixon */
#include "clixon/clixon.h"

/* Command line options passed to getopt(3) */
#define UTIL_XML_MOD_OPTS "hD:o:y:Y:b:x:p:s"

enum opx{
    OPX_ERROR = -1,
    OPX_INSERT,
    OPX_MERGE,
    OPX_PARENT
};

static const map_str2int opx_map[] = {
    {"insert",  OPX_INSERT},
    {"merge",   OPX_MERGE},
    {"parent",  OPX_PARENT},
    {NULL,             -1}
};

const enum opx
opx_str2int(char *opstr)
{
    return clicon_str2int(opx_map, opstr);
}

static int
usage(char *argv0)
{
    fprintf(stderr, "usage:%s [options]\n"
	    "where options are\n"
            "\t-h \t\tHelp\n"
    	    "\t-D <level>\tDebug\n"
	    "\t-o <op>   \tOperation: parent, insert or merge\n"
	    "\t-y <file> \tYANG spec file\n"
    	    "\t-Y <dir> \tYang dirs (can be several)\n"
	    "\t-b <base> \tXML base expression\n"
	    "\t-x <xml>  \tXML to insert\n"
	    "\t-p <xpath>\tXpath to where in base and XML\n"
	    "\t-s        \tSort output after operation\n",
	    argv0
	    );
    exit(0);
}

int
main(int argc, char **argv)
{
    int           retval = -1;
    char         *argv0 = argv[0];
    int           c;
    char         *yangfile = NULL;
    int           fd = 0; /* unless overriden by argv[1] */
    char         *x0str = NULL;
    char         *x1str = NULL;
    char         *xpath = NULL;
    yang_stmt    *yspec = NULL;
    cxobj        *x0 = NULL;
    cxobj        *x1 = NULL;
    cxobj        *xb = NULL;
    cxobj        *xi = NULL;
    cxobj        *xi1 = NULL;
    cxobj        *xerr = NULL;
    int           sort = 0;
    int           ret;
    clicon_handle h;
    enum opx      opx = OPX_ERROR;
    char         *reason = NULL;
    int           dbg = 0;
    cxobj        *xcfg = NULL;
    
    clicon_log_init("clixon_insert", LOG_DEBUG, CLICON_LOG_STDERR); 
    if ((h = clicon_handle_init()) == NULL)
	goto done;
    if ((xcfg = xml_new("clixon-config", NULL, CX_ELMNT)) == NULL)
	goto done;
    if (clicon_conf_xml_set(h, xcfg) < 0)
	goto done;
    optind = 1;
    opterr = 0;
    while ((c = getopt(argc, argv, UTIL_XML_MOD_OPTS)) != -1)
	switch (c) {
	case 'h':
	    usage(argv0);
	    break;
    	case 'D':
	    if (sscanf(optarg, "%d", &dbg) != 1)
		usage(argv0);
	    break;
	case 'o': /* Operation */
	    opx = opx_str2int(optarg);
	    break;
	case 'y': /* YANG spec file */
	    yangfile = optarg;
	    break;
	case 'Y':
	    if (clicon_option_add(h, "CLICON_YANG_DIR", optarg) < 0)
		goto done;
	    break;
	case 'b': /* Base XML expression */
	    x0str = optarg;
	    break;
	case 'x': /* XML to insert */
	    x1str = optarg;
	    break;
	case 'p': /* XPATH base */
	    xpath = optarg;
	    break;
	case 's': /* sort output after insert */
	    sort++;
	    break;
	default:
	    usage(argv[0]);
	    break;
	}
    /* Sanity check: check mandatory arguments */
    if (x1str == NULL || x0str == NULL || yangfile == NULL)
	usage(argv0);
    if (opx == OPX_ERROR) 
	usage(argv0);
    clicon_debug_init(dbg, NULL);
    if ((yspec = yspec_new()) == NULL)
	goto done;
    if (yang_spec_parse_file(h, yangfile, yspec) < 0)
	goto done;
    /* Parse base XML */
    if ((ret = clixon_xml_parse_string(x0str, YB_MODULE, yspec, &x0, &xerr)) < 0){
	clicon_err(OE_XML, 0, "Parsing base xml: %s", x0str);
	goto done;
    }
    if (ret == 0){
	clixon_netconf_error(xerr, "Parsing base xml", NULL);
	goto done;
    }
    /* Get base subtree by xpath */
    if (xpath == NULL)
	xb = x0;
    else if ((xb = xpath_first(x0, NULL, "%s", xpath)) == NULL){
	clicon_err(OE_XML, 0, "xpath: %s not found in x0", xpath);
	goto done;
    }
    if (clicon_debug_get()){
	clicon_debug(1, "xb:");
	xml_print(stderr, xb);
    }
    switch (opx){
    case OPX_PARENT:
	/* Parse insert XML */
	if ((ret = clixon_xml_parse_string(x1str, YB_PARENT, yspec, &xb, &xerr)) < 0){
	    clicon_err(OE_XML, 0, "Parsing insert xml: %s", x1str);
	    goto done;
	}
	if (ret == 0){
	    clixon_netconf_error(xerr, "Parsing secondary xml", NULL);
	    goto done;
	}
	break;
    case OPX_MERGE:
	/* Parse merge XML */
	if ((ret = clixon_xml_parse_string(x1str, YB_MODULE, yspec, &x1, &xerr)) < 0){
	    clicon_err(OE_XML, 0, "Parsing insert xml: %s", x1str);
	    goto done;
	}
	if (ret == 0){
	    clixon_netconf_error(xerr, "Parsing secondary xml", NULL);
	    goto done;
	}
	if (xpath == NULL)
	    xi = x1;
	else if ((xi = xpath_first(x1, NULL, "%s", xpath)) == NULL){
	    clicon_err(OE_XML, 0, "xpath: %s not found in xi", xpath);
	    goto done;
	}
	if ((ret = xml_merge(xb, xi, yspec, &reason)) < 0) 
	    goto done;
	if (ret == 0){
	    clicon_err(OE_XML, 0, "%s", reason);
	    goto done;
	}
	break;
    case OPX_INSERT:
	/* Parse insert XML */
	if ((ret = clixon_xml_parse_string(x1str, YB_MODULE, yspec, &x1, &xerr)) < 0){
	    clicon_err(OE_XML, 0, "Parsing insert xml: %s", x1str);
	    goto done;
	}
	if (ret == 0){
	    clixon_netconf_error(xerr, "Parsing secondary xml", NULL);
	    goto done;
	}
	/* Get secondary subtree by xpath */
	if (xpath == NULL)
	    xi = x1;
	else if ((xi = xpath_first(x1, NULL, "%s", xpath)) == NULL){
	    clicon_err(OE_XML, 0, "xpath: %s not found in xi", xpath);
	    goto done;
	}

	/* Find first element child of secondary */
	if ((xi1 = xml_child_i_type(xi, 0, CX_ELMNT)) == NULL){
	    clicon_err(OE_XML, 0, "xi has no element child");
	    goto done;
	}
	/* Remove it from parent */
	if (xml_rm(xi1) < 0)
	    goto done;
	if (xml_insert(xb, xi1, INS_LAST, NULL, NULL) < 0) 
	    goto done;
	break;
    default:
	usage(argv0);
    }
    if (clicon_debug_get()){
	clicon_debug(1, "x0:");
	xml_print(stderr, x0);
    }
    if (sort)
	xml_sort_recurse(xb);
    if (strcmp(xml_name(xb),"top")==0)
	clicon_xml2file(stdout, xml_child_i_type(xb, 0, CX_ELMNT), 0, 0);
    else
	clicon_xml2file(stdout, xb, 0, 0);
    fprintf(stdout, "\n");
    retval = 0;
 done:
    if (x0)
	xml_free(x0);
    if (x1)
	xml_free(x1);
    if (xcfg)
	xml_free(xcfg);
    if (xerr)
	xml_free(xerr);
    if (reason)
	free(reason);
    if (yspec)
	ys_free(yspec);
    if (fd > 0)
	close(fd);
    return retval;
}
