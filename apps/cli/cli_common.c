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
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <stdarg.h>
#include <time.h>
#include <ctype.h>

#include <unistd.h>
#ifdef HAVE_CRYPT_H
#include <crypt.h>
#endif 
#include <dirent.h>
#include <syslog.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <pwd.h>
#include <assert.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

#include "clixon_cli_api.h"

#include "cli_common.h"

static int xml2csv(FILE *f, cxobj *x, cvec *cvv);
//static int xml2csv_raw(FILE *f, cxobj *x);

/*! Initialize candidate database
 * We have implemented these:
 * shared - all users share a common candidate db
 */
int 
init_candidate_db(clicon_handle h)
{
    int          retval = -1;

    if (xmldb_exists(h, "running") != 1){
	clicon_err(OE_FATAL, 0, "Running db does not exist");
	goto err;
    }
    if (xmldb_exists(h, "candidate") != 1)
	if (xmldb_copy(h, "running", "candidate") < 0)
	    goto err;
    retval = 0;
  err:
    return retval;
}

/*
 * exit_candidate_db
 * private canddidates should be removed
 */
int 
exit_candidate_db(clicon_handle h)
{
    return 0;
}

/*
 * cli_debug
 * set debug level on stderr (not syslog).
 * The level is either what is specified in arg as int argument.
 * _or_ if a 'level' variable is present in vars use that value instead.
 */
int
cli_debug(clicon_handle h, cvec *vars, cg_var *arg)
{
    cg_var *cv;
    int     level;

    if ((cv = cvec_find_var(vars, "level")) == NULL)
	cv = arg;
    level = cv_int32_get(cv);
    /* cli */
    clicon_debug_init(level, NULL); /* 0: dont debug, 1:debug */
    /* config daemon */
    if (clicon_rpc_debug(h, level) < 0)
	goto done;
  done:
    return 0;
}


void
cli_signal_block(clicon_handle h)
{
	clicon_signal_block (SIGTSTP);
	clicon_signal_block (SIGQUIT);
	clicon_signal_block (SIGCHLD);
	if (!clicon_quiet_mode(h))
	    clicon_signal_block (SIGINT);
}

void
cli_signal_unblock(clicon_handle h)
{
	clicon_signal_unblock (SIGTSTP);
	clicon_signal_unblock (SIGQUIT);
	clicon_signal_unblock (SIGCHLD);
	clicon_signal_unblock (SIGINT);
}

/*
 * Flush pending signals for a given signal type
 */
void
cli_signal_flush(clicon_handle h)
{
    /* XXX A bit rough. Use sigpending() and more clever logic ?? */

    sigfn_t   h1, h2, h3, h4;

    set_signal (SIGTSTP, SIG_IGN, &h1);
    set_signal (SIGQUIT, SIG_IGN, &h2);
    set_signal (SIGCHLD, SIG_IGN, &h3);
    set_signal (SIGINT, SIG_IGN, &h4);

    cli_signal_unblock (h);

    set_signal (SIGTSTP, h1, NULL);
    set_signal (SIGQUIT, h2, NULL);
    set_signal (SIGCHLD, h3, NULL);
    set_signal (SIGINT, h4, NULL);

    cli_signal_block (h);
}


/* Code for recording which CLI commands have been issued */

static FILE *_recordf = NULL;
static int _isrecording = 0;

int
isrecording(void)
{
    return _isrecording;
}

int
cli_record(clicon_handle h, cvec *vars, cg_var *arg)
{
    _isrecording = cv_int32_get(arg);
    return 0;
}

static int
record_open(void)
{
    char file[] = "/tmp/cli.record.XXXXXX";
    int fd;

    if ((fd = mkstemp(file)) < 0 || (_recordf = fdopen(fd, "w")) < 0) {
	clicon_err(OE_UNIX, errno, "mkstemp/fdopen");
	return -1;
    }
    return 0;
}

/*
 * record commands in file
 */
int
record_command(char *str)
{
    if (_recordf==NULL)
	if (record_open() < 0)
	    return -1;
    fprintf(_recordf, "%s\n", str);
    fflush(_recordf);
    return 0;
}


/*
 * Callback to set syntax mode
 */
int
cli_set_mode(clicon_handle h, cvec *vars, cg_var *arg)
{
    int     retval = -1;
    char   *str = NULL;

    if (arg == NULL || (str = cv_string_get(arg)) == NULL){
	clicon_err(OE_PLUGIN, 0, "%s: requires string argument", __FUNCTION__);
	goto done;
    }
    cli_set_syntax_mode(h, str);
    retval = 0;
  done:
    return retval;
}

/*
 * XXX Application specific??
 * cli_start_shell
 * Start bash from cli callback
 */ 
int
cli_start_shell(clicon_handle h, cvec *vars, cg_var *arg)
{
    char *cmd;
    struct passwd *pw;
    int retval;
    char bcmd[128];
    cg_var *cv1 = cvec_i(vars, 1);

    cmd = (cvec_len(vars)>1 ? cv_string_get(cv1) : NULL);

    if ((pw = getpwuid(getuid())) == NULL){
	fprintf(stderr, "%s: getpwuid: %s\n", 
		__FUNCTION__, strerror(errno));
	return -1;
    }
    if (chdir(pw->pw_dir) < 0){ 
	fprintf(stderr, "%s: chdir(%s): %s\n",
		__FUNCTION__, pw->pw_dir, strerror(errno));
	endpwent();
	return -1;
    }
    endpwent();
    cli_signal_flush(h);
    cli_signal_unblock(h);
    if (cmd){
	snprintf(bcmd, 128, "bash -l -c \"%s\"", cmd);
	if ((retval = system(bcmd)) < 0){
	    cli_signal_block(h);
	    fprintf(stderr, "%s: system(bash -c): %s\n", 
		    __FUNCTION__, strerror(errno));
	    return -1;
	}
    }
    else
	if ((retval = system("bash -l")) < 0){
	    cli_signal_block(h);
	    fprintf(stderr, "%s: system(bash): %s\n", 
		    __FUNCTION__, strerror(errno));
	    return -1;
	}
    cli_signal_block(h);
#if 0 /* Allow errcodes from bash */
    if (retval != 0){
	fprintf(stderr, "%s: system(%s) code=%d\n", __FUNCTION__, cmd, retval);
      return -1;
    }
#endif

    return 0;
}

/*
 * Generic quit callback
 */
int 
cli_quit(clicon_handle h, cvec *vars, cg_var *arg)
{
    cli_set_exiting(h, 1);
    return 0;
}

/*! Generic commit callback
 * @param[in]  arg   If 1, then snapshot and copy to startup config
 */
int
cli_commit(clicon_handle h, 
	   cvec         *vars, 
	   cg_var       *arg)
{
    int            retval = -1;
    int            snapshot = arg?cv_int32_get(arg):0;

    if ((retval = clicon_rpc_commit(h, 
				    "running", 
				    "candidate", 
				    snapshot, /* snapshot */
				    snapshot)) < 0){ /* startup */
	cli_output(stderr, "Commit failed. Edit and try again or discard changes\n");
	goto done;
    }
    retval = 0;
  done:
    return retval;
}

/*
 * Generic validatecallback
 */
int
cli_validate(clicon_handle h, cvec *vars, cg_var *arg)
{
    int            retval = -1;

    if ((retval = clicon_rpc_validate(h, "candidate")) < 0)
	cli_output(stderr, "Validate failed. Edit and try again or discard changes\n");
    return retval;
}

/*! Completion callback primarily intended for automatically generated data model
 *
 * Returns an expand-type list of commands as used by cligen 'expand' 
 * functionality.
 * arg is a string: "<dbname> <keypattern> <variable>". 
 *   <dbname> is either running or candidate
 *   <pattern> matches a set of database keys following clicon_dbspec. 
 *             Eg a[].b[] $!x $!y
 *             the last being the variable to expand for.
 * Example:
 * dbspec is a[].b[] $!x $!y
 * clispec is a <x> b (<y>|<y expand_dbvar_auto()>;
 * db contains entries:
 * a.0 $x=5
 * a.1 $x=10
 * a.0.b.0 $x=5 $y=12
 * a.0.b.1 $x=5 $y=20
 * a.1.b.0 $x=10 $y=99
 *
 * The user types a 5 b <?> which produces the following output:
 *   <int>
 *   12
 *   20
 *
 * Assume callback given in a cligen spec: a <x:int expand_dbvar_auto("arg")
 * @param[in]   h        clicon handle 
 * @param[in]   name     Name of this function (eg "expand_dbvar-auto")
 * @param[in]   cvv      The command so far. Eg: cvec [0]:"a 5 b"; [1]: x=5;
 * @param[in]   arg      Argument given at the callback "<db> <xmlkeyfmt>"
 * @param[out]  len      len of return commands & helptxt 
 * @param[out]  commands vector of function pointers to callback functions
 * @param[out]  helptxt  vector of pointers to helptexts
 * @see cli_expand_var_generate  This is where arg is generated
 */
int
expand_dbvar_dbxml(void   *h, 
		   char   *name, 
		   cvec   *cvv, 
		   cg_var *arg, 
		   int    *nr, 
		   char ***commands, 
		   char ***helptexts)
{
    int              nvec;
    char           **vec = NULL;
    int              retval = -1;
    char            *xkfmt;
    char            *str;
    char            *dbstr;    
    cxobj           *xt = NULL;
    char            *xk = NULL;
    cxobj          **xvec = NULL;
    size_t           xlen = 0;
    cxobj           *x;
    char            *bodystr;
    int              i;
    int              i0;

    if (arg == NULL || (str = cv_string_get(arg)) == NULL){
	clicon_err(OE_PLUGIN, 0, "%s: requires string argument", __FUNCTION__);
	goto done;
    }
    /* In the example, str = "candidate a[].b[] $!x $!y" */
    if ((vec = clicon_strsplit(str, " ", &nvec, __FUNCTION__)) == NULL){
	clicon_err(OE_PLUGIN, errno, "clicon_strsplit");	
	goto done;
    }
    dbstr  = vec[0];
    if (strcmp(dbstr, "running") != 0 &&
	strcmp(dbstr, "candidate") != 0){
	clicon_err(OE_PLUGIN, 0, "No such db name: %s", dbstr);	
	goto done;
    }
    xkfmt = vec[1];
    /* xkfmt = /interface/%s/address/%s
       --> ^/interface/eth0/address/.*$
       --> /interface/[name=eth0]/address
    */
    if (xmlkeyfmt2xpath(xkfmt, cvv, &xk) < 0)
	goto done;   
    if (xmldb_get(h, dbstr, xk, 1, &xt, &xvec, &xlen) < 0)
	goto done;
    i0 = *nr;
    *nr += xlen;
    if ((*commands = realloc(*commands, sizeof(char *) * (*nr))) == NULL) {
	clicon_err(OE_UNIX, errno, "realloc: %s", strerror (errno));	
	goto done;
    }
    for (i = 0; i < xlen; i++) {
	x = xvec[i];
	if ((bodystr = xml_body(x)) == NULL){
	    clicon_err(OE_CFG, 0, "No xml body");
	    goto done;
	}
	(*commands)[i0+i] = strdup(bodystr);
    }
    retval = 0;
  done:
    unchunk_group(__FUNCTION__);
    if (xvec)
	free(xvec);
    if (xt)
	xml_free(xt);
    if (xk)
	free(xk);
    return retval;

}

/*
 * expand_dir
 * List files in a directory
 */
int
expand_dir(char *dir, int *nr, char ***commands, mode_t flags, int detail)
{
    DIR	*dirp;
    struct dirent *dp;
    struct stat st;
    char *str;
    char *cmd;
    int len;
    int retval = -1;
    struct passwd *pw;
    char filename[MAXPATHLEN];

    if ((dirp = opendir(dir)) == 0){
	fprintf(stderr, "expand_dir: opendir(%s) %s\n", 
		dir, strerror(errno));
	return -1;
    }
    *nr = 0;
    while ((dp = readdir(dirp)) != NULL) {
	if (
#if 0
	    strcmp(dp->d_name, ".") != 0 &&
	    strcmp(dp->d_name, "..") != 0
#else
	    dp->d_name[0] != '.'
#endif	    
	    ) {
	    snprintf(filename, MAXPATHLEN-1, "%s/%s", dir, dp->d_name);
	    if (lstat(filename, &st) == 0){
		if ((st.st_mode & flags) == 0)
		    continue;

#if EXPAND_RECURSIVE
		if (S_ISDIR(st.st_mode)) {
		    int nrsav = *nr;
		    if(expand_dir(filename, nr, commands, detail) < 0)
			goto quit;
		    while(nrsav < *nr) {
			len = strlen(dp->d_name) +  strlen((*commands)[nrsav]) + 2;
			if((str = malloc(len)) == NULL) {
			    fprintf(stderr, "expand_dir: malloc: %s\n",
				    strerror(errno));
			    goto quit;
			}
			snprintf(str, len-1, "%s/%s",
				 dp->d_name, (*commands)[nrsav]);
			free((*commands)[nrsav]);
			(*commands)[nrsav] = str;
			
			nrsav++;
		    }
		    continue;
		}
#endif
		if ((cmd = strdup(dp->d_name)) == NULL) {
		    fprintf(stderr, "expand_dir: strdup: %s\n",
			    strerror(errno));
		    goto quit;
		}
		if (0 &&detail){
		    if ((pw = getpwuid(st.st_uid)) == NULL){
			fprintf(stderr, "expand_dir: getpwuid(%d): %s\n",
				st.st_uid, strerror(errno));
			goto quit;
		    }
		    len = strlen(cmd) + 
			strlen(pw->pw_name) +
#ifdef __FreeBSD__
			strlen(ctime(&st.st_mtimespec.tv_sec)) +
#else
			strlen(ctime(&st.st_mtim.tv_sec)) +
#endif

			strlen("{ by }") + 1 /* \0 */;
		    if ((str=realloc(cmd, strlen(cmd)+len)) == NULL) {
			fprintf(stderr, "expand_dir: malloc: %s\n",
				strerror(errno));
			goto quit;
		    }
		    snprintf(str + strlen(dp->d_name), 
			     len - strlen(dp->d_name),
			     "{%s by %s}",
#ifdef __FreeBSD__
			     ctime(&st.st_mtimespec.tv_sec),
#else
			     ctime(&st.st_mtim.tv_sec),
#endif

			     pw->pw_name
			);
		    cmd = str;
		}
		if (((*commands) =
		     realloc(*commands, ((*nr)+1)*sizeof(char**))) == NULL){
		    perror("expand_dir: realloc");
		    goto quit;
		}
		(*commands)[(*nr)] = cmd;
		(*nr)++;
		if (*nr >= 128) /* Limit number of options */
		    break;
	    }
	}
    }
    retval = 0;
  quit:
    closedir(dirp);
    return retval;
}

/*! Compare two dbs using XML. Write to file and run diff
 */
static int
compare_xmls(cxobj *xc1, 
	     cxobj *xc2, 
	     int    astext)
{
    int    fd;
    FILE  *f;
    char   filename1[MAXPATHLEN];
    char   filename2[MAXPATHLEN];
    char   cmd[MAXPATHLEN];
    int    retval = -1;
    cxobj *xc;

    snprintf(filename1, sizeof(filename1), "/tmp/cliconXXXXXX");
    snprintf(filename2, sizeof(filename2), "/tmp/cliconXXXXXX");
    if ((fd = mkstemp(filename1)) < 0){
	clicon_err(OE_UNDEF, errno, "tmpfile: %s", strerror (errno));
	goto done;
    }
    if ((f = fdopen(fd, "w")) == NULL)
	goto done;
    xc = NULL;
    if (astext)
	while ((xc = xml_child_each(xc1, xc, -1)) != NULL)
	    xml2txt(f, xc, 0);
    else
	while ((xc = xml_child_each(xc1, xc, -1)) != NULL)
	    clicon_xml2file(f, xc, 0, 1);

    fclose(f);
    close(fd);

    if ((fd = mkstemp(filename2)) < 0){
	clicon_err(OE_UNDEF, errno, "mkstemp: %s", strerror (errno));
	goto done;
    }
    if ((f = fdopen(fd, "w")) == NULL)
	goto done;
    xc = NULL;
    if (astext)
	while ((xc = xml_child_each(xc2, xc, -1)) != NULL)
	    xml2txt(f, xc, 0);
    else
	while ((xc = xml_child_each(xc2, xc, -1)) != NULL)
	    clicon_xml2file(f, xc, 0, 1);
    fclose(f);
    close(fd);

    snprintf(cmd, sizeof(cmd), "/usr/bin/diff -dU 1 %s %s |  grep -v @@ | sed 1,2d", 		 filename1, filename2);
    if (system(cmd) < 0)
	goto done;

    retval = 0;
  done:
    unlink(filename1);
    unlink(filename2);
    return retval;
}

/*! Compare two dbs using XML. Write to file and run diff
 * @param[in]   h     Clicon handle
 * @param[in]   cvv  
 * @param[in]   arg   arg: 0 as xml, 1: as text
 */
int
compare_dbs(clicon_handle h, cvec *cvv, cg_var *arg)
{
    cxobj *xc1 = NULL; /* running xml */
    cxobj *xc2 = NULL; /* candidate xml */
    int    retval = -1;

    if (xmldb_get(h, "running", "/", 0, &xc1, NULL, NULL) < 0)
	goto done;
    if (xmldb_get(h, "candidate", "/", 0, &xc2, NULL, NULL) < 0)
	goto done;
    if (compare_xmls(xc1, xc2, arg?cv_int32_get(arg):0) < 0) /* astext? */
	goto done;
    retval = 0;
  done:
    if (xc1)
	xml_free(xc1);    
    if (xc2)
	xml_free(xc2);

    return retval;
}


/*! Modify xml database frm a callback using xml key format strings
 * @param[in]  h    Clicon handle
 * @param[in]  cvv  Vector of cli string and instantiated variables 
 * @param[in]  arg  An xml key format string, eg /aaa/%s 
 * @param[in]  op   Operation to perform on database
 * Cvv will contain forst the complete cli string, and then a set of optional
 * instantiated variables.
 * Example:
 * cvv[0] = "set interfaces interface eth0 type bgp"
 * cvv[1] = "eth0"
 * cvv[2] = "bgp"
 * arg = "/interfaces/interface/%s/type"
 * op: OP_MERGE
 * @see cli_callback_xmlkeyfmt_generate where arg is generated
 */
static int
cli_dbxml(clicon_handle       h, 
	  cvec               *cvv, 
	  cg_var             *arg, 
	  enum operation_type op)
{
    int        retval = -1;
    char      *str = NULL;
    char      *xkfmt;  /* xml key format */
    char      *xk = NULL; /* xml key */
    cg_var    *cval;
    char      *val = NULL;

    /* 
     * clicon_rpc_xmlput(h, db, MERGE,"<interfaces><interface><name>eth0</name><type>hej</type></interface><interfaces>");
     * Wanted database content:
     * /interfaces 
     * /interfaces/interface/eth0
     * /interfaces/interface/eth0/name eth0
     * /interfaces/interface/eth0/type hej
     * Algorithm alt1:
     * arg = "<interfaces><interface><name>$1</name><type>$2</type></interface><interfaces>"
     * Where is arg computed? In eg yang2cli_leaf, otherwise in yang_parse,..
     * Create string using cbuf and save that. 
     */
    xkfmt = cv_string_get(arg);
    if (xmlkeyfmt2key(xkfmt, cvv, &xk) < 0)
	goto done;
    cval = cvec_i(cvv, cvec_len(cvv)-1);
    if ((val = cv2str_dup(cval)) == NULL){
	clicon_err(OE_UNIX, errno, "cv2str_dup");
	goto done;
    }
    if (clicon_rpc_change(h, "candidate", op, xk, val) < 0)
	goto done;
    if (clicon_autocommit(h)) {
	if (clicon_rpc_commit(h, "running", "candidate", 0, 0) < 0) 
	    goto done;
    }
    retval = 0;
 done:
    if (str)
	free(str);
    if (xk)
	free(xk);
    return retval;
}

int 
cli_set(clicon_handle h, cvec *cvv, cg_var *arg)
{
    int retval = 1;

    if (cli_dbxml(h, cvv, arg, OP_REPLACE) < 0)
	goto done;
    retval = 0;
 done:
    return retval;
}

int 
cli_merge(clicon_handle h, cvec *cvv, cg_var *arg)
{
    int retval = -1;

    if (cli_dbxml(h, cvv, arg, OP_MERGE) < 0)
	goto done;
    retval = 0;
 done:
    return retval;
}

int 
cli_del(clicon_handle h, cvec *cvv, cg_var *arg)
{
    int   retval = -1;

    if (cli_dbxml(h, cvv, arg, OP_REMOVE) < 0)
	goto done;
    retval = 0;
 done:
    return retval;
}

/*! Load a configuration file to candidate database
 * Utility function used by cligen spec file
 * @param[in] h     CLICON handle
 * @param[in] cvv  Vector of variables (where <varname> is found)
 * @param[in] arg   A string: "<varname> (merge|replace)" 
 *   <varname> is name of a variable occuring in "cvv" containing filename
 * @note that "filename" is local on client filesystem not backend. 
 * @note file is assumed to have a dummy top-tag, eg <clicon></clicon>
 * @code
 *   # cligen spec
 *   load file <name2:string>, load_config_file("name2 merge");
 * @endcode
 * @see save_config_file
 */
int 
load_config_file(clicon_handle h, 
		 cvec         *cvv, 
		 cg_var       *arg)
{
    int         ret = -1;
    struct stat st;
    char      **vec;
    char      **vecp;
    char       *filename;
    int         replace;
    char       *str;
    cg_var     *cv;
    int         nvec;
    char       *opstr;
    char       *varstr;
    int         fd = -1;
    cxobj      *xt = NULL;
    cxobj      *xn;
    cxobj      *x;
    cbuf       *cbxml;

    if (arg == NULL || (str = cv_string_get(arg)) == NULL){
	clicon_err(OE_PLUGIN, 0, "%s: requires string argument", __FUNCTION__);
	goto done;
    }
    if ((vec = clicon_strsplit(str, " ", &nvec, __FUNCTION__)) == NULL){
	clicon_err(OE_PLUGIN, errno, "clicon_strsplit");	
	goto done;
    }
    if (nvec != 2){
	clicon_err(OE_PLUGIN, 0, "Arg syntax is <varname> <replace|merge>");	
	goto done;
    }
    varstr = vec[0];
    opstr  = vec[1];
    if (strcmp(opstr, "merge") == 0) 
	replace = 0;
    else
    if (strcmp(opstr, "replace") == 0) 
	replace = 1;
    else{
	clicon_err(OE_PLUGIN, 0, "No such op: %s, expected merge or replace", opstr);	
	goto done;
    }
    if ((cv = cvec_find_var(cvv, varstr)) == NULL){
	clicon_err(OE_PLUGIN, 0, "No such var name: %s", varstr);	
	goto done;
    }
    if ((vecp = clicon_realpath(NULL, cv_string_get(cv), __FUNCTION__)) == NULL){
	cli_output(stderr, "Failed to resolve filename\n");
	goto done;
    }
    filename = vecp[0];
    if (stat(filename, &st) < 0){
 	clicon_err(OE_UNIX, 0, "load_config: stat(%s): %s\n", 
 		filename, strerror(errno));
	goto done;
    }
    /* Open and parse local file into xml */
    if ((fd = open(filename, O_RDONLY)) < 0){
	clicon_err(OE_UNIX, errno, "%s: open(%s)", __FUNCTION__, filename);
	goto done;
    }
    if (clicon_xml_parse_file(fd, &xt, "</clicon>") < 0)
	goto done;
    if ((xn = xml_child_i(xt, 0)) != NULL){
	if ((cbxml = cbuf_new()) == NULL)
	    goto done;
	x = NULL;
	while ((x = xml_child_each(xn, x, -1)) != NULL) 
	    if (clicon_xml2cbuf(cbxml, x, 0, 0) < 0)
		goto done;
	if (clicon_rpc_xmlput(h, "candidate",
			      replace?OP_REPLACE:OP_MERGE, 
			      cbuf_get(cbxml)) < 0)
	    goto done;
	cbuf_free(cbxml);
    }
    ret = 0;
  done:
    if (xt)
	xml_free(xt);
    if (fd != -1)
	close(fd);
    return ret;
}

/*! Copy database to local file 
 * Utility function used by cligen spec file
 * @param[in] h     CLICON handle
 * @param[in] cvv  variable vector (containing <varname>)
 * @param[in] arg   a string: "<dbname> <varname>" 
 *   <dbname>  is running or candidate
 *   <varname> is name of cligen variable in the "cvv" vector containing file name
 * Note that "filename" is local on client filesystem not backend.
 * The function can run without a local database
 * @note The file is saved with dummy top-tag: clicon: <clicon></clicon>
 * @code
 *   save file <name:string>, save_config_file("running name");
 * @endcode
 * @see load_config_file
 */
int
save_config_file(clicon_handle h, 
		 cvec         *cvv, 
		 cg_var       *arg)
{
    int        retval = -1;
    char     **vec;
    char     **vecp;
    char      *filename;
    cg_var    *cv;
    int        nvec;
    char      *str;
    char      *dbstr;
    char      *varstr;
    cxobj     *xt = NULL;
    FILE      *f = NULL;

    if (arg == NULL || (str = cv_string_get(arg)) == NULL){
	clicon_err(OE_PLUGIN, 0, "%s: requires string argument", __FUNCTION__);
	goto done;
    }
    if ((vec = clicon_strsplit(str, " ", &nvec, __FUNCTION__)) == NULL){
	clicon_err(OE_PLUGIN, errno, "clicon_strsplit");	
	goto done;
    }
    if (nvec != 2){
	clicon_err(OE_PLUGIN, 0, "Arg syntax is <dbname> <varname>");	
	goto done;
    }
    dbstr  = vec[0];
    varstr = vec[1];
    if (strcmp(dbstr, "running") != 0 && strcmp(dbstr, "candidate") != 0) {
	clicon_err(OE_PLUGIN, 0, "No such db name: %s", dbstr);	
	goto done;
    }
    if ((cv = cvec_find_var(cvv, varstr)) == NULL){
	clicon_err(OE_PLUGIN, 0, "No such var name: %s", varstr);	
	goto done;
    }
    if ((vecp = clicon_realpath(NULL, cv_string_get(cv), __FUNCTION__)) == NULL){
	cli_output(stderr, "Failed to resolve filename\n");
	goto done;
    }
    filename = vecp[0];
    if (xmldb_get(h, dbstr, "/", 0, &xt, NULL, NULL) < 0)
	goto done;
    if ((f = fopen(filename, "wb")) == NULL){
	clicon_err(OE_CFG, errno, "Creating file %s", filename);
	goto done;
    } 
    if (clicon_xml2file(f, xt, 0, 1) < 0)
	goto done;
    retval = 0;
    /* Fall through */
  done:
    unchunk_group(__FUNCTION__);
    if (xt)
	xml_free(xt);
    if (f != NULL)
	fclose(f);
    return retval;
}

/*! Delete all elements in a database 
 * Utility function used by cligen spec file
 */
int
delete_all(clicon_handle h, cvec *cvv, cg_var *arg)
{
    char            *dbstr;
    int              retval = -1;

    if (arg == NULL || (dbstr = cv_string_get(arg)) == NULL){
	clicon_err(OE_PLUGIN, 0, "%s: requires string argument", __FUNCTION__);
	goto done;
    }
    if (strcmp(dbstr, "running") != 0 && strcmp(dbstr, "candidate") != 0){
	clicon_err(OE_PLUGIN, 0, "No such db name: %s", dbstr);	
	goto done;
    }
    if (xmldb_delete(h, dbstr) < 0)
	goto done;
    if (xmldb_init(h, dbstr) < 0) 
	goto done;
    retval = 0;
  done:
    return retval;
}

/*! Discard all changes in candidate and replace with running
 * Utility function used by cligen spec file
 */
int
discard_changes(clicon_handle h, cvec *cvv, cg_var *arg)
{
    return xmldb_copy(h, "running", "candidate");
}

/*! Generic function for showing configurations.
 * the callback differs.
 * @param[in] h     CLICON handle
 * @param[in] cvv  Vector of variables (not needed) 
 * @param[in] arg   A string: <dbname> <xpath>
 *   <dbname> is either running or candidate
 *   <xpath>  xpath expression as in nertconf get-config
 * @param fn
 * @param fnarg
 * @code
 *    # cligen spec
 *   show config id <n:string>, show_conf_as("running interfaces/interface[name=eth*]");
 * @endcode
 */
static int
show_conf_xmldb_as(clicon_handle h, 
		   cvec         *cvv, 
		   cg_var       *arg, 
		   cxobj       **xt) /* top xml */
{
    int              retval = -1;
    char            *db;
    char           **vec = NULL;
    int              nvec;
    char            *str;
    char            *xpath;

    if (arg == NULL || (str = cv_string_get(arg)) == NULL){
	clicon_err(OE_PLUGIN, 0, "%s: requires string argument", __FUNCTION__);
	goto done;
    }
    if ((vec = clicon_strsplit(str, " ", &nvec, __FUNCTION__)) == NULL){
	clicon_err(OE_PLUGIN, errno, "clicon_strsplit");	
	goto done;
    }
    if (nvec != 2){
	clicon_err(OE_PLUGIN, 0, "format error \"%s\" - expected <dbname> <xpath>", str);	
	goto done;
    }
    /* Dont get attr here, take it from arg instead */
    db = vec[0];
    if (strcmp(db, "running") != 0 && strcmp(db, "candidate") != 0) {
	clicon_err(OE_PLUGIN, 0, "No such db name: %s", db);	
	goto done;
    }
    xpath = vec[1];
    if (xmldb_get(h, db, xpath, 0, xt, NULL, NULL) < 0)
	goto done;
    retval = 0;
done:
    unchunk_group(__FUNCTION__);
    return retval;
}


/*! Show a configuration database on stdout using XML format
 * Utility function used by cligen spec file
 */
static int
show_conf_as_xml1(clicon_handle h, cvec *cvv, cg_var *arg, int netconf)
{
    cxobj *xt = NULL;
    cxobj *xc;
    int    retval = -1;

    if (show_conf_xmldb_as(h, cvv, arg, &xt) < 0)
	goto done;
    if (netconf) /* netconf prefix */
	fprintf(stdout, "<rpc><edit-config><target><candidate/></target><config>\n");
    xc = NULL; /* Dont print xt itself */
    while ((xc = xml_child_each(xt, xc, -1)) != NULL)
	clicon_xml2file(stdout, xc, netconf?2:0, 1);
    if (netconf) /* netconf postfix */
	fprintf(stdout, "</config></edit-config></rpc>]]>]]>\n");
    retval = 0;
  done:
    if (xt)
	xml_free(xt);
    return retval;

}

/*! Show configuration as prettyprinted xml 
 * Utility function used by cligen spec file
 */
int
show_conf_as_xml(clicon_handle h, cvec *cvv, cg_var *arg)
{
    return show_conf_as_xml1(h, cvv, arg, 0);
}

/*! Show configuration as prettyprinted xml with netconf hdr/tail
 * Utility function used by cligen spec file
 */
int
show_conf_as_netconf(clicon_handle h, cvec *cvv, cg_var *arg)
{
    return show_conf_as_xml1(h, cvv, arg, 1);
}

/*! Show configuration as JSON
 * Utility function used by cligen spec file
 */
int
show_conf_as_json(clicon_handle h, cvec *cvv, cg_var *arg)
{
    cxobj *xt = NULL;
    cxobj *xc;
    int    retval = -1;

    if (show_conf_xmldb_as(h, cvv, arg, &xt) < 0)
	goto done;
    xc = NULL; /* Dont print xt itself */
    while ((xc = xml_child_each(xt, xc, -1)) != NULL)
	xml2json(stdout, xc, 1);
    retval = 0;
  done:
    if (xt)
	xml_free(xt);
    return retval;
}

int
show_conf_xpath(clicon_handle h, cvec *cvv, cg_var *arg)
{
    int              retval = -1;
    char            *str;
    char            *xpath;
    cg_var          *cv;
    cxobj           *xt = NULL;
    cxobj          **xv = NULL;
    size_t           xlen;
    int              i;

    if (arg == NULL || (str = cv_string_get(arg)) == NULL){
	clicon_err(OE_PLUGIN, 0, "%s: requires string argument", __FUNCTION__);
	goto done;
    }
    /* Dont get attr here, take it from arg instead */
    if (strcmp(str, "running") != 0 && strcmp(str, "candidate") != 0){
	clicon_err(OE_PLUGIN, 0, "No such db name: %s", str);	
	goto done;
    }
    cv = cvec_find_var(cvv, "xpath");
    xpath = cv_string_get(cv);
    if (xmldb_get(h, str, xpath, 1, &xt, &xv, &xlen) < 0)
	goto done;
    for (i=0; i<xlen; i++)
	clicon_xml2file(stdout, xv[i], 0, 1);

    retval = 0;
done:
    if (xv)
	free(xv);
    if (xt)
	xml_free(xt);
    unchunk_group(__FUNCTION__);
    return retval;
}



/*! Show configuration as text
 * Utility function used by cligen spec file
 */
static int
show_conf_as_text1(clicon_handle h, cvec *cvv, cg_var *arg)
{
    cxobj       *xt = NULL;
    cxobj       *xc;
    int          retval = -1;

    if (show_conf_xmldb_as(h, cvv, arg, &xt) < 0)
	goto done;
    xc = NULL; /* Dont print xt itself */
    while ((xc = xml_child_each(xt, xc, -1)) != NULL)
	xml2txt(stdout, xc, 0); /* tree-formed text */
    retval = 0;
  done:
    if (xt)
	xml_free(xt);
    unchunk_group(__FUNCTION__);
    return retval;
}


/* Show configuration as commands, ie not tree format but as one-line commands
 */
static int
show_conf_as_command(clicon_handle h, cvec *cvv, cg_var *arg, char *prepend)
{
    cxobj             *xt = NULL;
    cxobj             *xc;
    enum genmodel_type gt;
    int                retval = -1;

    if ((xt = xml_new("tmp", NULL)) == NULL)
	goto done;
    if (show_conf_xmldb_as(h, cvv, arg, &xt) < 0)
	goto done;
    xc = NULL; /* Dont print xt itself */
    while ((xc = xml_child_each(xt, xc, -1)) != NULL){
	if ((gt = clicon_cli_genmodel_type(h)) == GT_ERR)
	    goto done;
	xml2cli(stdout, xc, prepend, gt, __FUNCTION__); /* cli syntax */
    }
    retval = 0;
  done:
    if (xt)
	xml_free(xt);
    unchunk_group(__FUNCTION__);
    return retval;
}

int
show_conf_as_text(clicon_handle h, cvec *cvv, cg_var *arg)
{
    return show_conf_as_text1(h, cvv, arg);
}

int
show_conf_as_cli(clicon_handle h, cvec *cvv, cg_var *arg)
{
    return show_conf_as_command(h, cvv, arg, NULL); /* XXX: how to set prepend? */
}

int
show_yang(clicon_handle h, cvec *cvv, cg_var *arg)
{
  yang_node *yn;
  char      *str = NULL;
  yang_spec *yspec;

  yspec = clicon_dbspec_yang(h);	
  if (arg != NULL){
    str = cv_string_get(arg);
    yn = (yang_node*)yang_find((yang_node*)yspec, 0, str);
  }
  else
    yn = (yang_node*)yspec;
  yang_print(stdout, yn, 0);
  return 0;
}

/* These are strings that can be used as 3rd argument to cli_setlog */
static const char *SHOWAS_TXT     = "txt";
static const char *SHOWAS_XML     = "xml";
static const char *SHOWAS_XML2TXT = "xml2txt";
static const char *SHOWAS_XML2JSON = "xml2json";

/*! This is the callback used by cli_setlog to print log message in CLI
 * param[in]  s    UNIX socket from backend  where message should be read
 * param[in]  arg  format: txt, xml, xml2txt, xml2json
 */
static int
cli_notification_cb(int s, void *arg)
{
    struct clicon_msg *reply;
    enum clicon_msg_type type;
    int                eof;
    int                retval = -1;
    char              *eventstr = NULL;
    int                level;
    cxobj             *xt = NULL;
    cxobj             *xn;
    char              *format = (char*)arg;

    /* get msg (this is the reason this function is called) */
    if (clicon_msg_rcv(s, &reply, &eof, __FUNCTION__) < 0)
	goto done;
    if (eof){
	clicon_err(OE_PROTO, ESHUTDOWN, "%s: Socket unexpected close", __FUNCTION__);
	close(s);
	errno = ESHUTDOWN;
	event_unreg_fd(s, cli_notification_cb);
	goto done;
    }
    if (format == NULL)
	goto done;
    type = ntohs(reply->op_type);
    switch (type){
    case CLICON_MSG_NOTIFY:
	if (clicon_msg_notify_decode(reply, &level, &eventstr, __FUNCTION__) < 0) 
	    goto done;
	if (strcmp(format, SHOWAS_TXT) == 0){
	    fprintf(stdout, "%s", eventstr);
	}
	else
	if (strcmp(format, SHOWAS_XML) == 0){
	    if (clicon_xml_parse_string(&eventstr, &xt) < 0)
		goto done;
	    if ((xn = xml_child_i(xt, 0)) != NULL)
		if (clicon_xml2file(stdout, xn, 0, 1) < 0)
		    goto done;
	}
	else
	if (strcmp(format, SHOWAS_XML2TXT) == 0){
	    if (clicon_xml_parse_string(&eventstr, &xt) < 0)
		goto done;
	    if ((xn = xml_child_i(xt, 0)) != NULL)
		if (xml2txt(stdout, xn, 0) < 0)
		    goto done;
	}
	else
        if (strcmp(format, SHOWAS_XML2JSON) == 0){
	    if (clicon_xml_parse_string(&eventstr, &xt) < 0)
		goto done;
	    if ((xn = xml_child_i(xt, 0)) != NULL){
		if (xml2json(stdout, xn, 0) < 0)
		    goto done;
	    }
	}
	break;
    default:
	clicon_err(OE_PROTO, 0, "%s: unexpected reply: %d", 
		__FUNCTION__, type);
	goto done;
	break;
    }
    retval = 0;
  done:
    if (xt)
	xml_free(xt);
    unchunk_group(__FUNCTION__); /* event allocated by chunk */
    return retval;

}


/*! Make a notify subscription to backend and un/register callback for return messages.
 * 
 * @param[in] h      Clicon handle
 * @param[in] cvv   Not used
 * @param[in] arg    A string with <log stream name> <stream status> [<format>]
 * where <status> is "0" or "1"
 * and   <format> is XXX
 * Example code: Start logging of mystream and show logs as xml
 * @code
 * cmd("comment"), cli_notify("mystream 1 xml"); 
 * @endcode
 * XXX: format is a memory leak
 */
int
cli_notify(clicon_handle h, cvec *cvv, cg_var *arg)
{
    char            *stream = NULL;
    int              retval = -1;
    char           **vec = NULL;
    int              nvec;
    char            *str;
    int              status;
    char            *formatstr = NULL;
    enum format_enum format = MSG_NOTIFY_TXT;

    if (arg==NULL || (str = cv_string_get(arg)) == NULL){
	clicon_err(OE_PLUGIN, 0, "%s: requires string argument", __FUNCTION__);
	goto done;
    }
    if ((vec = clicon_strsplit(str, " ", &nvec, __FUNCTION__)) == NULL){
	clicon_err(OE_PLUGIN, errno, "clicon_strsplit");	
	goto done;
    }
    if (nvec < 2){
	clicon_err(OE_PLUGIN, 0, "format error \"%s\" - expected <stream> <status>", str);	
	goto done;
    }
    stream = vec[0];
    status = atoi(vec[1]);
    if (nvec > 2){
	formatstr = strdup(vec[2]); /* memory leak */
	if (strcmp(formatstr, "SHOWAS_TXT") != 0)
	    format = MSG_NOTIFY_XML;
    }
    if (cli_notification_register(h, 
				  stream, 
				  format,
				  "", 
				  status, 
				  cli_notification_cb, 
				  (void*)formatstr) < 0)
	goto done;

    retval = 0;
  done:
    unchunk_group(__FUNCTION__);
    return retval;
}

/*! Register log notification stream
 * @param[in] h       Clicon handle
 * @param[in] stream  Event stream. CLICON is predefined, others are application-defined
 * @param[in] filter  Filter. For xml notification ie xpath: /[name=kalle]
 * @param[in] status  0 for stop, 1 to start
 * @param[in] fn      Callback function called when notification occurs
 * @param[in] arg     Argumnent to function
 */
int
cli_notification_register(clicon_handle h, 
			  char         *stream, 
			  enum format_enum format,
			  char         *filter, 
			  int           status, 
			  int         (*fn)(int, void*),
			  void         *arg)
{
    int              retval = -1;
    char            *logname;
    void            *p;
    int              s;
    clicon_hash_t   *cdat = clicon_data(h);
    size_t           len;
    int              s_exist = -1;

    if ((logname = chunk_sprintf(__FUNCTION__, "log_socket_%s", stream)) == NULL){
	clicon_err(OE_PLUGIN, errno, "%s: chunk_sprintf", __FUNCTION__);
	goto done;
    }
    if ((p = hash_value(cdat, logname, &len)) != NULL)
	s_exist = *(int*)p;

    if (status){
	if (s_exist!=-1){
	    clicon_err(OE_PLUGIN, 0, "%s: result log socket already exists", __FUNCTION__);
	    goto done;
	}
	if (clicon_rpc_subscription(h, status, stream, format, filter, &s) < 0)
	    goto done;
	if (cligen_regfd(s, fn, arg) < 0)
	    goto done;
	if (hash_add(cdat, logname, &s, sizeof(s)) == NULL)
	    goto done;
    }
    else{
	if (s_exist != -1){
	    cligen_unregfd(s_exist);
	}
	hash_del(cdat, logname);
	if (clicon_rpc_subscription(h, status, stream, format, filter, NULL) < 0)
	    goto done;

    }
    retval = 0;
  done:
    unchunk_group(__FUNCTION__);
    return retval;
}


#ifdef notused
/*! XML to CSV raw variant 
 * @see xml2csv
 */
static int 
xml2csv_raw(FILE *f, cxobj *x)
{
    cxobj           *xc;
    cxobj           *xb;
    int              retval = -1;
    int              i = 0;

    xc = NULL;
    while ((xc = xml_child_each(x, xc, CX_ELMNT)) != NULL) {
	if (xml_child_nr(xc)){
	    xb = xml_child_i(xc, 0);
	    if (xml_type(xb) == CX_BODY){
		if (i++)
		    fprintf(f, ";");
		fprintf(f, "%s", xml_value(xb));
	    }
	}
    }
    fprintf(f, "\n");
    retval = 0;
    return retval;
}
#endif

/*! Translate XML -> CSV commands
 * Can only be made in a 'flat tree', ie on the form:
 * <X><A>B</A></X> --> 
 * Type, A
 * X,  B
 * @param[in]  f     Output file
 * @param[in]  x     XML tree
 * @param[in]  cvv   A vector of field names present in XML
 * This means that only fields in x that are listed in cvv will be printed.
 */
static int 
xml2csv(FILE *f, cxobj *x, cvec *cvv)
{
    cxobj *xe, *xb;
    int              retval = -1;
    cg_var          *vs;

    fprintf(f, "%s", xml_name(x));
    xe = NULL;

    vs = NULL;
    while ((vs = cvec_each(cvv, vs))) {
	if ((xe = xml_find(x, cv_name_get(vs))) == NULL){
	    fprintf(f, ";");
	    continue;
	}
	if (xml_child_nr(xe)){
	    xb = xml_child_i(xe, 0);
	    fprintf(f, ";%s", xml_value(xb));
	}
    }
    fprintf(f, "\n");
    retval = 0;
    return retval;
}


static int
show_conf_as_csv1(clicon_handle h, cvec *cvv0, cg_var *arg)
{
    cxobj      *xt = NULL;
    cxobj      *xc;
    int         retval = -1;
    cvec       *cvv=NULL;
    char       *str;

    if (show_conf_xmldb_as(h, cvv0, arg, &xt) < 0)
	goto done;
    xc = NULL; /* Dont print xt itself */
    while ((xc = xml_child_each(xt, xc, -1)) != NULL){
	if ((str = chunk_sprintf(__FUNCTION__, "%s[]", xml_name(xc))) == NULL)
	    goto done;
#ifdef NOTYET /* yang-spec? */
	if (ds==NULL && (ds = key2spec_key(dbspec, str)) != NULL){
	    cg_var     *vs;
	    fprintf(stdout, "Type");
	    cvv = db_spec2cvec(ds);
	    vs = NULL;
	    while ((vs = cvec_each(cvv, vs))) 
		fprintf(stdout, ";%s",	cv_name_get(vs));
	    fprintf(stdout, "\n");
	} /* Now values just need to follow,... */
#endif /* yang-spec? */
	if (cvv== NULL)
	    goto done;
	xml2csv(stdout, xc, cvv); /* csv syntax */
    }
    retval = 0;
  done:
    if (xt)
	xml_free(xt);
    unchunk_group(__FUNCTION__);
    return retval;
}

int
show_conf_as_csv(clicon_handle h, cvec *cvv, cg_var *arg)
{
    return show_conf_as_csv1(h, cvv, arg);
}
