/* $Cambridge: exim/exim-src/src/lookups/dnsdb.h,v 1.2 2005/01/04 10:00:44 ph10 Exp $ */

/*************************************************
*     Exim - an Internet mail transport agent    *
*************************************************/

/* Copyright (c) University of Cambridge 1995 - 2005 */
/* See the file NOTICE for conditions of use and distribution. */

/* Header for the dnsdb lookup */

extern void *dnsdb_open(uschar *, uschar **);
extern int   dnsdb_find(void *, uschar *, uschar *, int, uschar **, uschar **,
               BOOL *);

/* End of lookups/dnsdb.h */
