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
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <unistd.h>
#include <pwd.h>
#include <syslog.h>
#include <errno.h>
#include <sys/types.h>
#include <signal.h>

/* net-snmp */
#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>
#include <net-snmp/agent/net-snmp-agent-includes.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

/* Command line options to be passed to getopt(3) */
#define SNMP_OPTS "hD:f:l:o:"

#if 1 // XXX hardcoded MIB object from https://github.com/net-snmp/subagent-example/blob/master/example-demon.c
/* cp NET-SNMP-TUTORIAL-MIB.txt ~/.snmp/mibs/
 * sudo /usr/local/sbin/snmpd -Lo -C --rwcommunity=public --master=agentx -f
 * sudo example_demon
 * snmpget -v 2c -c public localhost NET-SNMP-TUTORIAL-MIB::nstAgentSubagentObject.0
 */
/*! 
 * our initialization routine, automatically called by the agent 
 * (to get called, the function name must match init_FILENAME()) 
 * the variable we want to tie an OID to.  The agent will handle all
 * * GET and SET requests to this variable changing it's value as needed.
 */
static long      nstAgentSubagentObject = 2;

static void
init_nstAgentSubagentObject(clicon_handle h)
{
    static oid      nstAgentSubagentObject_oid[] =
        { 1, 3, 6, 1, 4, 1, 8072, 2, 4, 1, 1, 2, 0 };

    clicon_debug(1, "%s", __FUNCTION__);
    /*
     * a debugging statement.  Run the agent with -DnstAgentSubagentObject to see
     * the output of this debugging statement. 
     */
    DEBUGMSGTL(("nstAgentSubagentObject",
                "Initializing the nstAgentSubagentObject module\n"));

    /*
     * the line below registers our variables defined above as
     * accessible and makes it writable.  A read only version of any
     * of these registration would merely call
     * register_read_only_long_instance() instead.  The functions
     * called below should be consistent with your MIB, however.
     * 
     * If we wanted a callback when the value was retrieved or set
     * (even though the details of doing this are handled for you),
     * you could change the NULL pointer below to a valid handler
     * function. 
     */
    DEBUGMSGTL(("nstAgentSubagentObject",
                "Initalizing nstAgentSubagentObject scalar integer.  Default value = %ld\n",
                nstAgentSubagentObject));

    netsnmp_register_long_instance("nstAgentSubagentObject",
                                  nstAgentSubagentObject_oid,
                                  OID_LENGTH(nstAgentSubagentObject_oid),
                                  &nstAgentSubagentObject, NULL);

    DEBUGMSGTL(("nstAgentSubagentObject",
                "Done initalizing nstAgentSubagentObject module\n"));
}
#endif // XXX Hardcoded

/*! Signal terminates process
 * Just set exit flag for proper exit in event loop
 */
static void
clixon_snmp_sig_term(int arg)
{
    clicon_log(LOG_NOTICE, "%s: %s: pid: %u Signal %d", 
	       __PROGRAM__, __FUNCTION__, getpid(), arg);
    /* This should ensure no more accepts or incoming packets are processed because next time eventloop
     * is entered, it will terminate.
     * However there may be a case of sockets closing rather abruptly for clients
     */
    clixon_exit_set(1); 
}

/*! Callback for single socket 
 * This is a workaround for netsnmps API usiing fdset:s, instead an fdset is created before calling
 * the snmp api
 * @param[in]  s   Read socket
 * @param[in]  arg Clixon handle
 */
static int
clixon_snmp_input_cb(int   s, 
		     void *arg)
{
    int    retval = -1;
    fd_set readfds;
    //    clicon_handle h = (clicon_handle)arg;

    clicon_debug(1, "%s", __FUNCTION__);
    FD_ZERO(&readfds);
    FD_SET(s, &readfds);
    snmp_read(&readfds);
    retval = 0;
    // done:
    return retval;
}

/*! Get which sockets are used from SNMP API, the register single sockets into clixon event system
 *
 * This is a workaround for netsnmps API usiing fdset:s, instead an fdset is created before calling
 * the snmp api
 * if you use select(), see snmp_select_info() in snmp_api(3) 
 * snmp_select_info(int *numfds, fd_set *fdset, struct timeval *timeout, int *block)
 * @see clixon_snmp_input_cb
 */
static int
clixon_snmp_fdset_register(clicon_handle h)
{
    int             retval = -1;
    int             numfds = 0;
    fd_set          readfds;
    struct timeval  timeout = { LONG_MAX, 0 };
    int             block = 0;
    int             nr;
    int             i;

    FD_ZERO(&readfds);
    if ((nr = snmp_sess_select_info(NULL, &numfds, &readfds, &timeout, &block)) < 0){
	clicon_err(OE_SNMP, errno, "snmp_select_error");
	goto done;
    }
    for (i=0; i<numfds; i++){
	if (FD_ISSET(i, &readfds)){
	    if (clixon_event_reg_fd(i, clixon_snmp_input_cb, h, "snmp socket") < 0)
		goto done;
	}
    }
    retval = 0;
 done:
    return retval;
}

/*! Init netsnmp agent connection
 * @param[in]  h      Clixon handle
 * @param[in]  logdst Log destination, see clixon_log.h
 * @see snmp_terminate
 */
static int
clixon_snmp_init(clicon_handle h,
		 int           logdst)
{
    int retval = -1;

    clicon_debug(1, "%s", __FUNCTION__);
    if (logdst == CLICON_LOG_SYSLOG)
	snmp_enable_calllog();
    else
	snmp_enable_stderrlog();
    /* make a agentx client. */
    netsnmp_ds_set_boolean(NETSNMP_DS_APPLICATION_ID, NETSNMP_DS_AGENT_ROLE, 1);    

    /* initialize the agent library */
    init_agent(__PROGRAM__);
  
    /* XXX Hardcoded, replace this with generic MIB */
    init_nstAgentSubagentObject(h);

    /* example-demon will be used to read example-demon.conf files. */
    init_snmp(__PROGRAM__);

    if (set_signal(SIGTERM, clixon_snmp_sig_term, NULL) < 0){
	clicon_err(OE_DAEMON, errno, "Setting signal");
	goto done;
    }
    if (set_signal(SIGINT, clixon_snmp_sig_term, NULL) < 0){
	clicon_err(OE_DAEMON, errno, "Setting signal");
	goto done;
    }
    if (set_signal(SIGPIPE, SIG_IGN, NULL) < 0){
	clicon_err(OE_UNIX, errno, "Setting DIGPIPE signal");
	goto done;
    }
    /* Workaround for netsnmps API use of fdset:s instead of sockets */
    if (clixon_snmp_fdset_register(h) < 0)
	goto done;
    retval = 0;
 done:
    return retval;
}


/*! Clean and close all state of netconf process (but dont exit). 
 * Cannot use h after this 
 * @param[in]  h  Clixon handle
 */
static int
snmp_terminate(clicon_handle h)
{
    yang_stmt  *yspec;
    cvec       *nsctx;
    cxobj      *x;
    
    shutdown_agent();
    clicon_rpc_close_session(h);
    if ((yspec = clicon_dbspec_yang(h)) != NULL)
	ys_free(yspec);
    if ((yspec = clicon_config_yang(h)) != NULL)
	ys_free(yspec);
    if ((nsctx = clicon_nsctx_global_get(h)) != NULL)
	cvec_free(nsctx);
    if ((x = clicon_conf_xml(h)) != NULL)
	xml_free(x);
    xpath_optimize_exit();
    clixon_event_exit();
    clicon_handle_exit(h);
    clixon_err_exit();
    clicon_log_exit();
    return 0;
}


/*! Usage help routine
 * @param[in]  h      Clixon handle
 * @param[in]  argv0  command line
 */
static void
usage(clicon_handle h,
      char         *argv0)
{
    fprintf(stderr, "usage:%s\n"
	    "where options are\n"
            "\t-h\t\tHelp\n"
	    "\t-D <level>\tDebug level\n"
    	    "\t-f <file>\tConfiguration file (mandatory)\n"
	    "\t-l (e|o|s|f<file>) Log on std(e)rr, std(o)ut, (s)yslog(default), (f)ile\n"
	    "\t-o \"<option>=<value>\"\tGive configuration option overriding config file (see clixon-config.yang)\n",
	    argv0
	    );
    exit(0);
}

int
main(int    argc,
     char **argv)
{
    int              retval = -1;
    int              c;
    char            *argv0 = argv[0];
    clicon_handle    h;
    int              logdst = CLICON_LOG_STDERR;
    struct passwd   *pw;
    yang_stmt       *yspec = NULL;
    char            *str;
    uint32_t         id;
    cvec            *nsctx_global = NULL; /* Global namespace context */
    size_t           cligen_buflen;
    size_t           cligen_bufthreshold;
    int              dbg = 0;
    size_t           sz;
    
    /* Create handle */
    if ((h = clicon_handle_init()) == NULL)
	return -1;
    /* In the startup, logs to stderr & debug flag set later */
    clicon_log_init(__PROGRAM__, LOG_INFO, logdst); 

    /* Set username to clixon handle. Use in all communication to backend */
    if ((pw = getpwuid(getuid())) == NULL){
	clicon_err(OE_UNIX, errno, "getpwuid");
	goto done;
    }
    if (clicon_username_set(h, pw->pw_name) < 0)
	goto done;
    while ((c = getopt(argc, argv, SNMP_OPTS)) != -1)
	switch (c) {
	case 'h' : /* help */
	    usage(h, argv[0]);
	    break;
	case 'D' : /* debug */
	    if (sscanf(optarg, "%d", &dbg) != 1)
		usage(h, argv[0]);
	    break;
	 case 'f': /* override config file */
	    if (!strlen(optarg))
		usage(h, argv[0]);
	    clicon_option_str_set(h, "CLICON_CONFIGFILE", optarg);
	    break;
	 case 'l': /* Log destination: s|e|o */
	    if ((logdst = clicon_log_opt(optarg[0])) < 0)
		usage(h, argv[0]);
	    if (logdst == CLICON_LOG_FILE &&
		strlen(optarg)>1 &&
		clicon_log_file(optarg+1) < 0)
		goto done;
	     break;
	}

    /* 
     * Logs, error and debug to stderr or syslog, set debug level
     */
    clicon_log_init(__PROGRAM__, dbg?LOG_DEBUG:LOG_INFO, logdst); 
    clicon_debug_init(dbg, NULL); 

    yang_init(h);
    
    /* Find, read and parse configfile */
    if (clicon_options_main(h) < 0)
	goto done;
    
    /* Now rest of options */
    optind = 1;
    opterr = 0;
    while ((c = getopt(argc, argv, SNMP_OPTS)) != -1)
	switch (c) {
	case 'h' : /* help */
	case 'D' : /* debug */
	case 'f':  /* config file */
	case 'l':  /* log  */
	    break; /* see above */
	case 'o':{ /* Configuration option */
	    char          *val;
	    if ((val = index(optarg, '=')) == NULL)
		usage(h, argv0);
	    *val++ = '\0';
	    if (clicon_option_add(h, optarg, val) < 0)
		goto done;
	    break;
	}
	default:
	    usage(h, argv[0]);
	    break;
	}
    argc -= optind;
    argv += optind;

    /* Access the remaining argv/argc options (after --) w clicon-argv_get() */
    clicon_argv_set(h, argv0, argc, argv);

    /* Init cligen buffers */
    cligen_buflen = clicon_option_int(h, "CLICON_CLI_BUF_START");
    cligen_bufthreshold = clicon_option_int(h, "CLICON_CLI_BUF_THRESHOLD");
    cbuf_alloc_set(cligen_buflen, cligen_bufthreshold);

    if ((sz = clicon_option_int(h, "CLICON_LOG_STRING_LIMIT")) != 0)
	clicon_log_string_limit_set(sz);

    /* Set default namespace according to CLICON_NAMESPACE_NETCONF_DEFAULT */
    xml_nsctx_namespace_netconf_default(h);

    /* Add (hardcoded) netconf features in case ietf-netconf loaded here
     * Otherwise it is loaded in netconf_module_load below
     */
    if (netconf_module_features(h) < 0)
	goto done;

    /* In case ietf-yang-metadata is loaded by application, handle annotation extension */
#if 0
    if (yang_metadata_init(h) < 0)
	goto done;    
#endif
    /* Create top-level yang spec and store as option */
    if ((yspec = yspec_new()) == NULL)
	goto done;
    clicon_dbspec_yang_set(h, yspec);	

    /* Load Yang modules
     * 1. Load a yang module as a specific absolute filename */
    if ((str = clicon_yang_main_file(h)) != NULL){
	if (yang_spec_parse_file(h, str, yspec) < 0)
	    goto done;
    }
    /* 2. Load a (single) main module */
    if ((str = clicon_yang_module_main(h)) != NULL){
	if (yang_spec_parse_module(h, str, clicon_yang_module_revision(h),
				   yspec) < 0)
	    goto done;
    }
    /* 3. Load all modules in a directory */
    if ((str = clicon_yang_main_dir(h)) != NULL){
	if (yang_spec_load_dir(h, str, yspec) < 0)
	    goto done;
    }
    /* Load clixon lib yang module */
    if (yang_spec_parse_module(h, "clixon-lib", NULL, yspec) < 0)
	goto done;
     /* Load yang module library, RFC7895 */
    if (yang_modules_init(h) < 0)
	goto done;
    /* Add netconf yang spec, used by netconf client and as internal protocol */
    if (netconf_module_load(h) < 0)
	goto done;
    /* Here all modules are loaded 
     * Compute and set canonical namespace context
     */
    if (xml_nsctx_yangspec(yspec, &nsctx_global) < 0)
	goto done;
    if (clicon_nsctx_global_set(h, nsctx_global) < 0)
	goto done;

#if 0
    /* Call start function is all plugins before we go interactive */
    if (clixon_plugin_start_all(h) < 0)
	goto done;
#endif
    /* Get session id from backend hello */
    clicon_session_id_set(h, getpid()); 

    /* Send hello request to backend to get session-id back
     * This is done once at the beginning of the session and then this is
     * used by the client, even though new TCP sessions are created for
     * each message sent to the backend.
     */
    if (clicon_hello_req(h, &id) < 0)
	goto done;
    clicon_session_id_set(h, id);
    
    /* Init snmp as subagent */
    if (clixon_snmp_init(h, logdst) < 0)
	goto done;
    
    if (dbg)
	clicon_option_dump(h, dbg);
    if (clixon_event_loop(h) < 0)
	goto done;
    retval = 0;
  done:
    snmp_terminate(h);
    clicon_log_init(__PROGRAM__, LOG_INFO, 0); /* Log on syslog no stderr */
    clicon_log(LOG_NOTICE, "%s: %u Terminated", __PROGRAM__, getpid());
    return retval;
}
