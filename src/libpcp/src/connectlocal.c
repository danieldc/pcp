/*
 * Copyright (c) 1995-2002,2004 Silicon Graphics, Inc.  All Rights Reserved.
 * Copyright (c) 2010 Ken McDonell.  All Rights Reserved.
 * 
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 * 
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 */

#include "pmapi.h"
#include "impl.h"
#include "pmda.h"
#include <ctype.h>
#if defined(HAVE_SYS_MMAN_H)
#include <sys/mman.h> 
#endif

static __pmDSO *dsotab;
static int	numdso = -1;

static int
build_dsotab(void)
{
    /*
     * parse pmcd's config file extracting details from dso lines
     *
     * very little syntactic checking here ... pmcd(1) does that job
     * nicely and even if we get confused, the worst thing that happens
     * is we don't include one or more of the DSO PMDAs in dsotab[]
     *
     * lines for DSO PMDAs generally look like this ...
     * Name	Domain	Type	Init Routine	Path
     * mmv	70	dso	mmv_init	/var/lib/pcp/pmdas/mmv/pmda_mmv.so 
     *
     */
    char	configFileName[MAXPATHLEN];
    FILE	*configFile;
    char	*config;
    char	*p;
    char	*q;
    struct stat	sbuf;
    int		lineno = 1;
    int		domain;
    char	*init;
    char	*name;
    char	peekc;

    numdso = 0;
    dsotab = NULL;

    strcpy(configFileName, pmGetConfig("PCP_PMCDCONF_PATH"));
#ifdef PCP_DEBUG
    if (pmDebug & DBG_TRACE_CONTEXT) {
	fprintf(stderr, "build_dsotab: parsing %s\n", configFileName);
    }
#endif
    if (stat(configFileName, &sbuf) < 0) {
	return -errno;
    }
    configFile = fopen(configFileName, "r");
    if (configFile == NULL) {
	return -errno;
    }
    if ((config = mmap(NULL, sbuf.st_size, PROT_READ|PROT_WRITE, MAP_PRIVATE, fileno(configFile), 0)) == MAP_FAILED) {
	fclose(configFile);
	return -errno;
    }

    p = config;
    while (*p != '\0') {
	/* each time through here we're at the start of a new line */
	if (*p == '#')
	    goto eatline;
	if (strncmp(p, "pmcd", 4) == 0) {
	    /*
	     * the pmcd PMDA is an exception ... it makes reference to
	     * symbols in pmcd, and only makes sense when attached to the
	     * pmcd process, so we skip this one
	     */
	    goto eatline;
	}
	/* skip the PMDA's name */
	while (*p != '\0' && *p != '\n' && !isspace(*p))
	    p++;
	while (*p != '\0' && *p != '\n' && isspace(*p))
	    p++;
	/* extract domain number */
	domain = (int)strtol(p, &q, 10);
	p = q;
	while (*p != '\0' && *p != '\n' && isspace(*p))
	    p++;
	/* only interested if the type is "dso" */
	if (strncmp(p, "dso", 3) != 0)
	    goto eatline;
	p += 3;
	while (*p != '\0' && *p != '\n' && isspace(*p))
	    p++;
	/* up to the init routine name */
	init = p;
	while (*p != '\0' && *p != '\n' && !isspace(*p))
	    p++;
	*p = '\0';
	p++;
	while (*p != '\0' && *p != '\n' && isspace(*p))
	    p++;
	/* up to the dso pathname */
	name = p;
	while (*p != '\0' && *p != '\n' && !isspace(*p))
	    p++;
	peekc = *p;
	*p = '\0';
#ifdef PCP_DEBUG
	if (pmDebug & DBG_TRACE_CONTEXT) {
	    fprintf(stderr, "[%d] domain=%d, name=%s, init=%s\n", lineno, domain, name, init);
	}
#endif
	/*
	 * a little be recursive if we got here via __pmLocalPMDA(),
	 * but numdso has been set correctly, so this is OK
	 */
	__pmLocalPMDA(PM_LOCAL_ADD, domain, name, init);
	*p = peekc;

eatline:
	while (*p != '\0' && *p != '\n')
	    p++;
	if (*p == '\n') {
	    lineno++;
	    p++;
	}
    }

    fclose(configFile);
    munmap(config, sbuf.st_size);
    return 0;
}

#if defined(HAVE_DLFCN_H)
#include <dlfcn.h>
#endif

/*
 * As of PCP version 2.1, we're no longer searching for DSO's;
 * pmcd's config file should have full paths to each of 'em.
 */
const char *
__pmFindPMDA(const char *name)
{
    return (access(name, F_OK) == 0) ? name : NULL;
}

__pmDSO *
__pmLookupDSO(int domain)
{
    int		i;
    for (i = 0; i < numdso; i++) {
	if (dsotab[i].domain == domain && dsotab[i].handle != NULL)
	    return &dsotab[i];
    }
    return NULL;
}

int
__pmConnectLocal(void)
{
    int			i;
    __pmDSO		*dp;
    char		pathbuf[MAXPATHLEN];
    const char		*path;
#if defined(HAVE_DLOPEN)
    unsigned int	challenge;
    void		(*initp)(pmdaInterface *);
#endif

    if (numdso == -1) {
	int	sts;
	sts = build_dsotab();
	if (sts < 0) return sts;
    }

    for (i = 0; i < numdso; i++) {
	dp = &dsotab[i];
	if (dp->domain == -1 || dp->handle != NULL)
	    continue;
	/*
	 * __pmLocalPMDA() means the path to the DSO may be something
	 * other than relative to $PCP_PMDAS_DIR ... need to try both
	 * options and also with and without DSO_SUFFIX (so, dll, etc)
	 */
	snprintf(pathbuf, sizeof(pathbuf), "%s%c%s",
		 pmGetConfig("PCP_PMDAS_DIR"), __pmPathSeparator(), dp->name);
	if ((path = __pmFindPMDA(pathbuf)) == NULL) {
	    snprintf(pathbuf, sizeof(pathbuf), "%s%c%s.%s",
		 pmGetConfig("PCP_PMDAS_DIR"), __pmPathSeparator(), dp->name, DSO_SUFFIX);
	    if ((path = __pmFindPMDA(pathbuf)) == NULL) {
		if ((path = __pmFindPMDA(dp->name)) == NULL) {
		    snprintf(pathbuf, sizeof(pathbuf), "%s.%s", dp->name, DSO_SUFFIX);
		    if ((path = __pmFindPMDA(pathbuf)) == NULL) {
			pmprintf("__pmConnectLocal: Warning: cannot find DSO at \"%s\" or \"%s\"\n", 
			     pathbuf, dp->name);
			pmflush();
			dp->domain = -1;
			dp->handle = NULL;
			continue;
		    }
		}
	    }
	}
#if defined(HAVE_DLOPEN)
	dp->handle = dlopen(path, RTLD_NOW);
	if (dp->handle == NULL) {
	    pmprintf("__pmConnectLocal: Warning: error attaching DSO "
		     "\"%s\"\n%s\n\n", path, dlerror());
	    pmflush();
	    dp->domain = -1;
	}
#else	/* ! HAVE_DLOPEN */
	dp->handle = NULL;
	pmprintf("__pmConnectLocal: Warning: error attaching DSO \"%s\"\n",
		 path);
	pmprintf("No dynamic DSO/DLL support on this platform\n\n");
	pmflush();
	dp->domain = -1;
#endif

	if (dp->handle == NULL)
	    continue;

#if defined(HAVE_DLOPEN)
	/*
	 * rest of this only makes sense if the dlopen() worked
	 */
	if (dp->init == NULL)
	    initp = NULL;
	else
	    initp = (void (*)(pmdaInterface *))dlsym(dp->handle, dp->init);
	if (initp == NULL) {
	    pmprintf("__pmConnectLocal: Warning: couldn't find init function "
		     "\"%s\" in DSO \"%s\"\n", dp->init, path);
	    pmflush();
	    dlclose(dp->handle);
	    dp->domain = -1;
	    continue;
	}

	/*
	 * Pass in the expected domain id.
	 * The PMDA initialization routine can (a) ignore it, (b) check it
	 * is the expected value, or (c) self-adapt.
	 */
	dp->dispatch.domain = dp->domain;

	/*
	 * the PMDA interface / PMAPI version discovery as a "challenge" ...
	 * for pmda_interface it is all the bits being set,
	 * for pmapi_version it is the complement of the one you are using now
	 */
	challenge = 0xff;
	dp->dispatch.comm.pmda_interface = challenge;
	dp->dispatch.comm.pmapi_version = ~PMAPI_VERSION;

	dp->dispatch.comm.flags = 0;
	dp->dispatch.status = 0;

	(*initp)(&dp->dispatch);

	if (dp->dispatch.status != 0) {
	    /* initialization failed for some reason */
	    pmprintf("__pmConnectLocal: Warning: initialization "
		     "routine \"%s\" failed in DSO \"%s\": %s\n", 
		     dp->init, path, pmErrStr(dp->dispatch.status));
	    pmflush();
	    dlclose(dp->handle);
	    dp->domain = -1;
	}
	else {
	    if (dp->dispatch.comm.pmda_interface == challenge) {
		/*
		 * DSO did not change pmda_interface, assume PMAPI version 1
		 * from PCP 1.x and PMDA_INTERFACE_1
		 */
		dp->dispatch.comm.pmda_interface = PMDA_INTERFACE_1;
		dp->dispatch.comm.pmapi_version = PMAPI_VERSION_1;
	    }
	    else {
		/*
		 * gets a bit tricky ...
		 * interface_version (8-bits) used to be version (4-bits),
		 * so it is possible that only the bottom 4 bits were
		 * changed and in this case the PMAPI version is 1 for
		 * PCP 1.x
		 */
		if ((dp->dispatch.comm.pmda_interface & 0xf0) == (challenge & 0xf0)) {
		    dp->dispatch.comm.pmda_interface &= 0x0f;
		    dp->dispatch.comm.pmapi_version = PMAPI_VERSION_1;
		}
	    }

	    if (dp->dispatch.comm.pmda_interface < PMDA_INTERFACE_1 ||
		dp->dispatch.comm.pmda_interface > PMDA_INTERFACE_LATEST) {
		pmprintf("__pmConnectLocal: Error: Unknown PMDA interface "
			 "version %d in \"%s\" DSO\n", 
			 dp->dispatch.comm.pmda_interface, path);
		pmflush();
		dlclose(dp->handle);
		dp->domain = -1;
	    }

	    if (dp->dispatch.comm.pmapi_version != PMAPI_VERSION_1 &&
		dp->dispatch.comm.pmapi_version != PMAPI_VERSION_2) {
		pmprintf("__pmConnectLocal: Error: Unknown PMAPI version %d "
			 "in \"%s\" DSO\n",
			 dp->dispatch.comm.pmapi_version, path);
		pmflush();
		dlclose(dp->handle);
		dp->domain = -1;
	    }
	}
#endif	/* HAVE_DLOPEN */
    }

    return 0;
}

int
__pmLocalPMDA(int op, int domain, const char *name, const char *init)
{
    int		sts = 0;
    int		i;

#ifdef PCP_DEBUG
    if (pmDebug & DBG_TRACE_CONTEXT) {
	fprintf(stderr, "__pmLocalPMDA(op=");
	if (op == PM_LOCAL_ADD) fprintf(stderr, "ADD");
	else if (op == PM_LOCAL_DEL) fprintf(stderr, "DEL");
	else if (op == PM_LOCAL_CLEAR) fprintf(stderr, "CLEAR");
	else fprintf(stderr, "%d ???", op);
	fprintf(stderr, ", domain=%d, name=%s, init=%s)\n", domain, name, init);
    }
#endif

    if (numdso == -1) {
	sts = build_dsotab();
	if (sts < 0) return sts;
    }

    switch (op) {
	case PM_LOCAL_ADD:
	    if ((dsotab = (__pmDSO *)realloc(dsotab, (numdso+1)*sizeof(__pmDSO))) == NULL) {
		__pmNoMem("__pmLocalPMDA realloc", (numdso+1)*sizeof(__pmDSO), PM_FATAL_ERR);
		/*NOTREACHED*/
	    }
	    dsotab[numdso].domain = domain;
	    if (name == NULL) {
		/* odd, will fail later at dlopen */
		dsotab[numdso].name = NULL;
	    }
	    else {
		if ((dsotab[numdso].name = strdup(name)) == NULL) {
		    sts = -errno;
		    __pmNoMem("__pmLocalPMDA name", strlen(name)+1, PM_RECOV_ERR);
		    return sts;
		}
	    }
	    if (init == NULL) {
		/* odd, will fail later at initialization call */
		dsotab[numdso].init = NULL;
	    }
	    else {
		if ((dsotab[numdso].init = strdup(init)) == NULL) {
		    sts = -errno;
		    __pmNoMem("__pmLocalPMDA init", strlen(init)+1, PM_RECOV_ERR);
		    return sts;
		}
	    }
	    dsotab[numdso].handle = NULL;
	    numdso++;
	    break;

	case PM_LOCAL_DEL:
	    sts = PM_ERR_INDOM;
	    for (i = 0; i < numdso; i++) {
		if ((domain != -1 && dsotab[i].domain == domain) ||
		    (name != NULL && strcmp(dsotab[i].name, name) == 0)) {
		    if (dsotab[i].handle) {
			dlclose(dsotab[i].handle);
			dsotab[i].handle = NULL;
		    }
		    dsotab[i].domain = -1;
		    sts = 0;
		}
	    }
	    break;

	case PM_LOCAL_CLEAR:
	    for (i = 0; i < numdso; i++) {
		free(dsotab[i].name);
	    	free(dsotab[i].init);
		if (dsotab[i].handle)
		    dlclose(dsotab[i].handle);
	    }
	    free(dsotab);
	    dsotab = NULL;
	    numdso = 0;
	    break;

	default:
	    sts = PM_ERR_CONV;
	    break;
    }

#ifdef PCP_DEBUG
    if (pmDebug & DBG_TRACE_CONTEXT) {
	if (sts != 0)
	    fprintf(stderr, "__pmLocalPMDA -> %s\n", pmErrStr(sts));
	fprintf(stderr, "Local Context PMDA Table");
	if (numdso == 0)
	    fprintf(stderr, " ... empty");
	fputc('\n', stderr);
	for (i = 0; i < numdso; i++) {
	    fprintf(stderr, "%p [%d] domain=%d name=%s init=%s handle=%p\n",
		&dsotab[i], i, dsotab[i].domain, dsotab[i].name, dsotab[i].init, dsotab[i].handle);
	}
    }
#endif

    return sts;
}

/*
 * Parse a command line string that encodes arguments to __pmLocalPMDA(),
 * then call __pmLocalPMDA().
 *
 * The syntax for the string is 1 to 4 fields separated by colons:
 * 	- op ("add" for add, "del" for delete, "clear" for clear)
 *	- domain (PMDA's PMD)
 *	- path (path to DSO PMDA)
 *	- init (name of DSO's initialization routine)
 */
char *
__pmSpecLocalPMDA(const char *spec)
{
    int		op;
    int		domain = -1;
    char	*name = NULL;
    char	*init = NULL;
    int		sts;
    char	*arg;
    char	*sbuf;
    char	*ap;

    if ((arg = sbuf = strdup(spec)) == NULL) {
	sts = -errno;
	__pmNoMem("__pmSpecLocalPMDA dup spec", strlen(spec)+1, PM_RECOV_ERR);
	return "strdup failed";
    }
    if (strncmp(arg, "add", 3) == 0) {
	op = PM_LOCAL_ADD;
	ap = &arg[3];
    }
    else if (strncmp(arg, "del", 3) == 0) {
	op = PM_LOCAL_DEL;
	ap = &arg[3];
    }
    else if (strncmp(arg, "clear", 5) == 0) {
	op = PM_LOCAL_CLEAR;
	ap = &arg[5];
    }
    else {
	free(sbuf);
	return "bad op in spec";
    }
    if (op == PM_LOCAL_CLEAR && *ap == '\0')
	goto doit;

    if (*ap != ',') {
	free(sbuf);
	return "bad spec";
    }
    arg = ++ap;
    if (*ap != ',' && *ap != '\0') {
	domain = (int)strtol(arg, &ap, 10);
	if ((*ap != ',' && *ap != '\0') || domain < 0 || domain > 510) {
	    free(sbuf);
	    return "bad domain in spec";
	}
    }
    if (*ap == ',') {
	ap++;
	if (*ap == ',') {
	    /* no name, could have init (not useful but possible!) */
	    ap++;
	    if (*ap != '\0')
		init = ap;
	}
	else if (*ap != '\0') {
	    /* have name and possibly init */
	    name = ap;
	    while (*ap != ',' && *ap != '\0')
		ap++;
	    if (*ap == ',') {
		*ap++ = '\0';
		if (*ap != '\0')
		    init = ap;
	    }
	}
    }

doit:
    sts = __pmLocalPMDA(op, domain, name, init);
    if (sts < 0) {
	static char buffer[256];
	snprintf(buffer, sizeof(buffer), "__pmLocalPMDA: %s", pmErrStr(sts));
	free(sbuf);
	return buffer;
    }

    free(sbuf);
    return NULL;
}
