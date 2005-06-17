/* $Cambridge: exim/exim-src/src/sieve.c,v 1.12 2005/06/17 10:47:05 ph10 Exp $ */

/*************************************************
*     Exim - an Internet mail transport agent    *
*************************************************/

/* Copyright (c) Michael Haardt 2003-2005 */
/* See the file NOTICE for conditions of use and distribution. */

/* This code was contributed by Michael Haardt. */


/* Sieve mail filter. */

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "exim.h"

#if HAVE_ICONV
#include <iconv.h>
#endif

/* Define this for RFC compliant \r\n end-of-line terminators.      */
/* Undefine it for UNIX-style \n end-of-line terminators (default). */
#undef RFC_EOL

/* Define this for development of the subaddress Sieve extension.   */
#define SUBADDRESS

/* Define this for the vacation Sieve extension.                    */
#define VACATION

/* Must be >= 1                                                     */
#define VACATION_MIN_DAYS 1
/* Must be >= VACATION_MIN_DAYS, must be > 7, should be > 30        */
#define VACATION_MAX_DAYS 31

/* Keep this at 75 to accept only RFC compliant MIME words.         */
/* Increase it if you want to match headers from buggy MUAs.        */
#define MIMEWORD_LENGTH 75

struct Sieve
  {
  uschar *filter;
  const uschar *pc;
  int line;
  const uschar *errmsg;
  int keep;
  int require_envelope;
  int require_fileinto;
#ifdef SUBADDRESS
  int require_subaddress;
#endif
#ifdef VACATION
  int require_vacation;
  int vacation_ran;
#endif
  uschar *vacation_directory;
  const uschar *subaddress;
  const uschar *useraddress;
  int require_copy;
  int require_iascii_numeric;
  };

enum Comparator { COMP_OCTET, COMP_EN_ASCII_CASEMAP, COMP_ASCII_NUMERIC };
enum MatchType { MATCH_IS, MATCH_CONTAINS, MATCH_MATCHES };
#ifdef SUBADDRESS
enum AddressPart { ADDRPART_USER, ADDRPART_DETAIL, ADDRPART_LOCALPART, ADDRPART_DOMAIN, ADDRPART_ALL };
#else
enum AddressPart { ADDRPART_LOCALPART, ADDRPART_DOMAIN, ADDRPART_ALL };
#endif
enum RelOp { LT, LE, EQ, GE, GT, NE };

struct String
  {
  uschar *character;
  int length;
  };

static int parse_test(struct Sieve *filter, int *cond, int exec);
static int parse_commands(struct Sieve *filter, int exec, address_item **generated);

static uschar str_from_c[]="From";
static const struct String str_from={ str_from_c, 4 };
static uschar str_to_c[]="To";
static const struct String str_to={ str_to_c, 2 };
static uschar str_cc_c[]="Cc";
static const struct String str_cc={ str_cc_c, 2 };
static uschar str_bcc_c[]="Bcc";
static const struct String str_bcc={ str_bcc_c, 3 };
static uschar str_sender_c[]="Sender";
static const struct String str_sender={ str_sender_c, 6 };
static uschar str_resent_from_c[]="Resent-From";
static const struct String str_resent_from={ str_resent_from_c, 11 };
static uschar str_resent_to_c[]="Resent-To";
static const struct String str_resent_to={ str_resent_to_c, 9 };
static uschar str_fileinto_c[]="fileinto";
static const struct String str_fileinto={ str_fileinto_c, 8 };
static uschar str_envelope_c[]="envelope";
static const struct String str_envelope={ str_envelope_c, 8 };
#ifdef SUBADDRESS
static uschar str_subaddress_c[]="subaddress";
static const struct String str_subaddress={ str_subaddress_c, 10 };
#endif
#ifdef VACATION
static uschar str_vacation_c[]="vacation";
static const struct String str_vacation={ str_vacation_c, 8 };
static uschar str_subject_c[]="Subject";
static const struct String str_subject={ str_subject_c, 7 };
#endif
static uschar str_copy_c[]="copy";
static const struct String str_copy={ str_copy_c, 4 };
static uschar str_iascii_casemap_c[]="i;ascii-casemap";
static const struct String str_iascii_casemap={ str_iascii_casemap_c, 15 };
static uschar str_enascii_casemap_c[]="en;ascii-casemap";
static const struct String str_enascii_casemap={ str_enascii_casemap_c, 16 };
static uschar str_ioctet_c[]="i;octet";
static const struct String str_ioctet={ str_ioctet_c, 7 };
static uschar str_iascii_numeric_c[]="i;ascii-numeric";
static const struct String str_iascii_numeric={ str_iascii_numeric_c, 15 };
static uschar str_comparator_iascii_casemap_c[]="comparator-i;ascii-casemap";
static const struct String str_comparator_iascii_casemap={ str_comparator_iascii_casemap_c, 26 };
static uschar str_comparator_enascii_casemap_c[]="comparator-en;ascii-casemap";
static const struct String str_comparator_enascii_casemap={ str_comparator_enascii_casemap_c, 27 };
static uschar str_comparator_ioctet_c[]="comparator-i;octet";
static const struct String str_comparator_ioctet={ str_comparator_ioctet_c, 18 };
static uschar str_comparator_iascii_numeric_c[]="comparator-i;ascii-numeric";
static const struct String str_comparator_iascii_numeric={ str_comparator_iascii_numeric_c, 26 };


/*************************************************
*          Encode to quoted-printable            *
*************************************************/

/*
Arguments:
  src               UTF-8 string
*/

static struct String *quoted_printable_encode(const struct String *src, struct String *dst)
{
int pass;
const uschar *start,*end;
uschar *new = NULL;
uschar ch;
size_t line;

for (pass=0; pass<=1; ++pass)
  {
  line=0;
  if (pass==0)
    dst->length=0;
  else
    {
    dst->character=store_get(dst->length+1); /* plus one for \0 */
    new=dst->character;
    }
  for (start=src->character,end=start+src->length; start<end; ++start)
    {
    ch=*start;
    if (line>=73)
      {
      if (pass==0)
        dst->length+=2;
      else
        {
        *new++='=';
        *new++='\n';
        }
      line=0;
      }
    if
      (
      (ch>=33 && ch<=60)
      || (ch>=62 && ch<=126)
      ||
        (
        (ch==9 || ch==32)
#ifdef RFC_EOL
        && start+2<end
        && (*(start+1)!='\r' || *(start+2)!='\n')
#else
        && start+1<end
        && *(start+1)!='\n'
#endif
        )
      )
      {
      if (pass==0)
        ++dst->length;
      else
        *new++=*start;
      ++line;
      }
#ifdef RFC_EOL
    else if (ch=='\r' && start+1<end && *(start+1)=='\n')
      {
      if (pass==0)
        {
        ++dst->length;
        line=0;
        ++start;
        }
      else
        *new++='\n';
        line=0;
      }
#else
    else if (ch=='\n')
      {
      if (pass==0)
        ++dst->length;
      else
        *new++=*start;
      ++line;
      }
#endif
    else
      {
      if (pass==0)
        dst->length+=3;
      else
        {
        sprintf(CS new,"=%02X",ch);
        new+=3;
        }
      line+=3;
      }
    }
  }
  *new='\0'; /* not included in length, but nice */
  return dst;
}


/*************************************************
*          Octet-wise string comparison          *
*************************************************/

/*
Arguments:
  needle            UTF-8 string to search ...
  haystack          ... inside the haystack
  match_prefix      1 to compare if needle is a prefix of haystack

Returns:      0               needle not found in haystack
              1               needle found
*/

static int eq_octet(const struct String *needle,
  const struct String *haystack, int match_prefix)
{
size_t nl,hl;
const uschar *n,*h;

nl=needle->length;
n=needle->character;
hl=haystack->length;
h=haystack->character;
while (nl>0 && hl>0)
  {
#if !HAVE_ICONV
  if (*n&0x80) return 0;
  if (*h&0x80) return 0;
#endif
  if (*n!=*h) return 0;
  ++n;
  ++h;
  --nl;
  --hl;
  }
return (match_prefix ? nl==0 : nl==0 && hl==0);
}


/*************************************************
*    ASCII case-insensitive string comparison    *
*************************************************/

/*
Arguments:
  needle            UTF-8 string to search ...
  haystack          ... inside the haystack
  match_prefix      1 to compare if needle is a prefix of haystack

Returns:      0               needle not found in haystack
              1               needle found
*/

static int eq_asciicase(const struct String *needle,
  const struct String *haystack, int match_prefix)
{
size_t nl,hl;
const uschar *n,*h;
uschar nc,hc;

nl=needle->length;
n=needle->character;
hl=haystack->length;
h=haystack->character;
while (nl>0 && hl>0)
  {
  nc=*n;
  hc=*h;
#if !HAVE_ICONV
  if (nc&0x80) return 0;
  if (hc&0x80) return 0;
#endif
  /* tolower depends on the locale and only ASCII case must be insensitive */
  if ((nc&0x80) || (hc&0x80)) { if (nc!=hc) return 0; }
  else if ((nc>='A' && nc<='Z' ? nc|0x20 : nc) != (hc>='A' && hc<='Z' ? hc|0x20 : hc)) return 0;
  ++n;
  ++h;
  --nl;
  --hl;
  }
return (match_prefix ? nl==0 : nl==0 && hl==0);
}


/*************************************************
*        Octet-wise glob pattern search          *
*************************************************/

/*
Arguments:
  needle      pattern to search ...
  haystack    ... inside the haystack

Returns:      0               needle not found in haystack
              1               needle found
*/

static int eq_octetglob(const struct String *needle,
  const struct String *haystack)
{
struct String n,h;

n=*needle;
h=*haystack;
while (n.length)
  {
  switch (n.character[0])
    {
    case '*':
      {
      int currentLength;

      ++n.character;
      --n.length;
      /* The greedy match is not yet well tested.  Some day we may */
      /* need to refer to the matched parts, so the code is already */
      /* prepared for that. */
#if 1
      /* greedy match */
      currentLength=h.length;
      h.character+=h.length;
      h.length=0;
      while (h.length<=currentLength)
        {
        if (eq_octetglob(&n,&h)) return 1;
        else /* go back one octet */
          {
          --h.character;
          ++h.length;
          }
        }
      return 0;
#else
      /* minimal match */
      while (h.length)
        {
        if (eq_octetglob(&n,&h)) return 1;
        else /* advance one octet */
          {
          ++h.character;
          --h.length;
          }
        }
      break;
#endif
      }
    case '?':
      {
      if (h.length)
        {
        ++h.character;
        --h.length;
        ++n.character;
        --n.length;
        }
      else return 0;
      break;
      }
    case '\\':
      {
      ++n.character;
      --n.length;
      /* FALLTHROUGH */
      }
    default:
      {
      if
        (
        h.length==0 ||
#if !HAVE_ICONV
        (h.character[0]&0x80) || (n.character[0]&0x80) ||
#endif
        h.character[0]!=n.character[0]
        ) return 0;
      else
        {
        ++h.character;
        --h.length;
        ++n.character;
        --n.length;
        };
      }
    }
  }
return (h.length==0);
}


/*************************************************
*   ASCII case-insensitive glob pattern search   *
*************************************************/

/*
Arguments:
  needle      UTF-8 pattern to search ...
  haystack    ... inside the haystack

Returns:      0               needle not found in haystack
              1               needle found
*/

static int eq_asciicaseglob(const struct String *needle,
  const struct String *haystack)
{
struct String n,h;

n=*needle;
h=*haystack;
while (n.length)
  {
  switch (n.character[0])
    {
    case '*':
      {
      int currentLength;

      ++n.character;
      --n.length;
      /* The greedy match is not yet well tested.  Some day we may */
      /* need to refer to the matched parts, so the code is already */
      /* prepared for that. */
#if 1
      /* greedy match */
      currentLength=h.length;
      h.character+=h.length;
      h.length=0;
      while (h.length<=currentLength)
        {
        if (eq_asciicaseglob(&n,&h)) return 1;
        else /* go back one UTF-8 character */
          {
          if (h.length==currentLength) return 0;
          --h.character;
          ++h.length;
          if (h.character[0]&0x80)
            {
            while (h.length<currentLength && (*(h.character-1)&0x80))
              {
              --h.character;
              ++h.length;
              }
            }
          }
        }
      /* NOTREACHED */
#else
      while (h.length)
        {
        if (eq_asciicaseglob(&n,&h)) return 1;
        else /* advance one UTF-8 character */
          {
          if (h.character[0]&0x80)
            {
            while (h.length && (h.character[0]&0x80))
              {
              ++h.character;
              --h.length;
              }
            }
          else
            {
            ++h.character;
            --h.length;
            }
          }
        }
      break;
#endif
      }
    case '?':
      {
      if (h.length)
        {
        ++n.character;
        --n.length;
        /* advance one UTF-8 character */
        if (h.character[0]&0x80)
          {
          while (h.length && (h.character[0]&0x80))
            {
            ++h.character;
            --h.length;
            }
          }
        else
          {
          ++h.character;
          --h.length;
          }
        }
      else return 0;
      break;
      }
    case '\\':
      {
      ++n.character;
      --n.length;
      /* FALLTHROUGH */
      }
    default:
      {
      char nc,hc;

      if (h.length==0) return 0;
      nc=n.character[0];
      hc=h.character[0];
#if !HAVE_ICONV
      if ((hc&0x80) || (nc&0x80)) return 0;
#endif
      /* tolower depends on the locale and only ASCII case must be insensitive */
      if ((nc&0x80) || (hc&0x80)) { if (nc!=hc) return 0; }
      else if ((nc>='A' && nc<='Z' ? nc|0x20 : nc) != (hc>='A' && hc<='Z' ? hc|0x20 : hc)) return 0;
      ++h.character;
      --h.length;
      ++n.character;
      --n.length;
      }
    }
  }
return (h.length==0);
}


/*************************************************
*    ASCII numeric comparison                    *
*************************************************/

/*
Arguments:
  a                 first numeric string
  b                 second numeric string
  relop             relational operator

Returns:      0               not (a relop b)
              1               a relop b
*/

static int eq_asciinumeric(const struct String *a,
  const struct String *b, enum RelOp relop)
{
size_t al,bl;
const uschar *as,*aend,*bs,*bend;
int cmp;

as=a->character;
aend=a->character+a->length;
bs=b->character;
bend=b->character+b->length;

while (*as>='0' && *as<='9' && as<aend) ++as;
al=as-a->character;
while (*bs>='0' && *bs<='9' && bs<bend) ++bs;
bl=bs-b->character;

if (al && bl==0) cmp=-1;
else if (al==0 && bl==0) cmp=0;
else if (al==0 && bl) cmp=1;
else
  {
  cmp=al-bl;
  if (cmp==0) cmp=memcmp(a->character,b->character,al);
  }
switch (relop)
  {
  case LT: return cmp<0;
  case LE: return cmp<=0;
  case EQ: return cmp==0;
  case GE: return cmp>=0;
  case GT: return cmp>0;
  case NE: return cmp!=0;
  }
  /*NOTREACHED*/
  return -1;
}


/*************************************************
*             Compare strings                    *
*************************************************/

/*
Arguments:
  needle      UTF-8 pattern or string to search ...
  haystack    ... inside the haystack
  co          comparator to use
  mt          match type to use

Returns:      0               needle not found in haystack
              1               needle found
              -1              comparator does not offer matchtype
*/

static int compare(struct Sieve *filter, const struct String *needle, const struct String *haystack,
  enum Comparator co, enum MatchType mt)
{
int r=0;

if ((filter_test != FTEST_NONE && debug_selector != 0) ||
  (debug_selector & D_filter) != 0)
  {
  debug_printf("String comparison (match ");
  switch (mt)
    {
    case MATCH_IS: debug_printf(":is"); break;
    case MATCH_CONTAINS: debug_printf(":contains"); break;
    case MATCH_MATCHES: debug_printf(":matches"); break;
    }
  debug_printf(", comparison \"");
  switch (co)
    {
    case COMP_OCTET: debug_printf("i;octet"); break;
    case COMP_EN_ASCII_CASEMAP: debug_printf("en;ascii-casemap"); break;
    case COMP_ASCII_NUMERIC: debug_printf("i;ascii-numeric"); break;
    }
  debug_printf("\"):\n");
  debug_printf("  Search = %s (%d chars)\n", needle->character,needle->length);
  debug_printf("  Inside = %s (%d chars)\n", haystack->character,haystack->length);
  }
switch (mt)
  {
  case MATCH_IS:
    {
    switch (co)
      {
      case COMP_OCTET:
        {
        if (eq_octet(needle,haystack,0)) r=1;
        break;
        }
      case COMP_EN_ASCII_CASEMAP:
        {
        if (eq_asciicase(needle,haystack,0)) r=1;
        break;
        }
      case COMP_ASCII_NUMERIC:
        {
        if (!filter->require_iascii_numeric)
          {
          filter->errmsg=CUS "missing previous require \"comparator-i;ascii-numeric\";";
          return -1;
          }
        if (eq_asciinumeric(needle,haystack,EQ)) r=1;
        break;
        }
      }
    break;
    }
  case MATCH_CONTAINS:
    {
    struct String h;

    switch (co)
      {
      case COMP_OCTET:
        {
        for (h=*haystack; h.length; ++h.character,--h.length) if (eq_octet(needle,&h,1)) { r=1; break; }
        break;
        }
      case COMP_EN_ASCII_CASEMAP:
        {
        for (h=*haystack; h.length; ++h.character,--h.length) if (eq_asciicase(needle,&h,1)) { r=1; break; }
        break;
        }
      default:
        {
        filter->errmsg=CUS "comparator does not offer specified matchtype";
        return -1;
        }
      }
    break;
    }
  case MATCH_MATCHES:
    {
    switch (co)
      {
      case COMP_OCTET:
        {
        if (eq_octetglob(needle,haystack)) r=1;
        break;
        }
      case COMP_EN_ASCII_CASEMAP:
        {
        if (eq_asciicaseglob(needle,haystack)) r=1;
        break;
        }
      default:
        {
        filter->errmsg=CUS "comparator does not offer specified matchtype";
        return -1;
        }
      }
    break;
    }
  }
if ((filter_test != FTEST_NONE && debug_selector != 0) ||
  (debug_selector & D_filter) != 0)
  debug_printf("  Result %s\n",r?"true":"false");
return r;
}


/*************************************************
*         Check header field syntax              *
*************************************************/

/*
RFC 2822, section 3.6.8 says:

  field-name      =       1*ftext

  ftext           =       %d33-57 /               ; Any character except
                          %d59-126                ;  controls, SP, and
                                                  ;  ":".

That forbids 8-bit header fields.  This implementation accepts them, since
all of Exim is 8-bit clean, so it adds %d128-%d255.

Arguments:
  header      header field to quote for suitable use in Exim expansions

Returns:      0               string is not a valid header field
              1               string is a value header field
*/

static int is_header(const struct String *header)
{
size_t l;
const uschar *h;

l=header->length;
h=header->character;
if (l==0) return 0;
while (l)
  {
  if (((unsigned char)*h)<33 || ((unsigned char)*h)==':' || ((unsigned char)*h)==127) return 0;
  else
    {
    ++h;
    --l;
    }
  }
return 1;
}


/*************************************************
*       Quote special characters string          *
*************************************************/

/*
Arguments:
  header      header field to quote for suitable use in Exim expansions
              or as debug output

Returns:      quoted string
*/

static const uschar *quote(const struct String *header)
{
uschar *quoted=NULL;
int size=0,ptr=0;
size_t l;
const uschar *h;

l=header->length;
h=header->character;
while (l)
  {
  switch (*h)
    {
    case '\0':
      {
      quoted=string_cat(quoted,&size,&ptr,CUS "\\0",2);
      break;
      }
    case '$':
    case '{':
    case '}':
      {
      quoted=string_cat(quoted,&size,&ptr,CUS "\\",1);
      }
    default:
      {
      quoted=string_cat(quoted,&size,&ptr,h,1);
      }
    }
  ++h;
  --l;
  }
quoted=string_cat(quoted,&size,&ptr,CUS "",1);
return quoted;
}


/*************************************************
*   Add address to list of generated addresses   *
*************************************************/

/*
According to RFC 3028, duplicate delivery to the same address must
not happen, so the list is first searched for the address.

Arguments:
  generated   list of generated addresses
  addr        new address to add
  file        address denotes a file

Returns:      nothing
*/

static void add_addr(address_item **generated, uschar *addr, int file, int maxage, int maxmessages, int maxstorage)
{
address_item *new_addr;

for (new_addr=*generated; new_addr; new_addr=new_addr->next)
  {
  if (Ustrcmp(new_addr->address,addr)==0 && (file ? testflag(new_addr, af_pfr|af_file) : 1))
    {
    if ((filter_test != FTEST_NONE && debug_selector != 0) || (debug_selector & D_filter) != 0)
      {
      debug_printf("Repeated %s `%s' ignored.\n",file ? "fileinto" : "redirect", addr);
      }
    return;
    }
  }

if ((filter_test != FTEST_NONE && debug_selector != 0) || (debug_selector & D_filter) != 0)
  {
  debug_printf("%s `%s'\n",file ? "fileinto" : "redirect", addr);
  }
new_addr=deliver_make_addr(addr,TRUE);
if (file)
  {
  setflag(new_addr, af_pfr|af_file);
  new_addr->mode = 0;
  }
new_addr->p.errors_address = NULL;
new_addr->next = *generated;
*generated = new_addr;
}


/*************************************************
*         Return decoded header field            *
*************************************************/

/*
Arguments:
  value       returned value of the field
  header      name of the header field

Returns:      nothing          The expanded string is empty
                               in case there is no such header
*/

static void expand_header(struct String *value, const struct String *header)
{
uschar *s,*r,*t;
uschar *errmsg;

value->length=0;
value->character=(uschar*)0;

t=r=s=expand_string(string_sprintf("$rheader_%s",quote(header)));
while (*r==' ') ++r;
while (*r)
  {
  if (*r=='\n')
    {
    ++r;
    while (*r==' ' || *r=='\t') ++r;
    if (*r) *t++=' ';
    }
  else
    *t++=*r++;
  }
*t++='\0';
value->character=rfc2047_decode(s,TRUE,US"utf-8",'\0',&value->length,&errmsg);
}


/*************************************************
*        Parse remaining hash comment            *
*************************************************/

/*
Token definition:
  Comment up to terminating CRLF

Arguments:
  filter      points to the Sieve filter including its state

Returns:      1                success
              -1               syntax error
*/

static int parse_hashcomment(struct Sieve *filter)
{
++filter->pc;
while (*filter->pc)
  {
#ifdef RFC_EOL
  if (*filter->pc=='\r' && *(filter->pc+1)=='\n')
#else
  if (*filter->pc=='\n')
#endif
    {
#ifdef RFC_EOL
    filter->pc+=2;
#else
    ++filter->pc;
#endif
    ++filter->line;
    return 1;
    }
  else ++filter->pc;
  }
filter->errmsg=CUS "missing end of comment";
return -1;
}


/*************************************************
*       Parse remaining C-style comment          *
*************************************************/

/*
Token definition:
  Everything up to star slash

Arguments:
  filter      points to the Sieve filter including its state

Returns:      1                success
              -1               syntax error
*/

static int parse_comment(struct Sieve *filter)
{
  filter->pc+=2;
  while (*filter->pc)
  {
    if (*filter->pc=='*' && *(filter->pc+1)=='/')
    {
      filter->pc+=2;
      return 1;
    }
    else ++filter->pc;
  }
  filter->errmsg=CUS "missing end of comment";
  return -1;
}


/*************************************************
*         Parse optional white space             *
*************************************************/

/*
Token definition:
  Spaces, tabs, CRLFs, hash comments or C-style comments

Arguments:
  filter      points to the Sieve filter including its state

Returns:      1                success
              -1               syntax error
*/

static int parse_white(struct Sieve *filter)
{
while (*filter->pc)
  {
  if (*filter->pc==' ' || *filter->pc=='\t') ++filter->pc;
#ifdef RFC_EOL
  else if (*filter->pc=='\r' && *(filter->pc+1)=='\n')
#else
  else if (*filter->pc=='\n')
#endif
    {
#ifdef RFC_EOL
    filter->pc+=2;
#else
    ++filter->pc;
#endif
    ++filter->line;
    }
  else if (*filter->pc=='#')
    {
    if (parse_hashcomment(filter)==-1) return -1;
    }
  else if (*filter->pc=='/' && *(filter->pc+1)=='*')
    {
    if (parse_comment(filter)==-1) return -1;
    }
  else break;
  }
return 1;
}


/*************************************************
*          Parse a optional string               *
*************************************************/

/*
Token definition:
   quoted-string = DQUOTE *CHAR DQUOTE
           ;; in general, \ CHAR inside a string maps to CHAR
           ;; so \" maps to " and \\ maps to \
           ;; note that newlines and other characters are all allowed
           ;; in strings

   multi-line          = "text:" *(SP / HTAB) (hash-comment / CRLF)
                         *(multi-line-literal / multi-line-dotstuff)
                         "." CRLF
   multi-line-literal  = [CHAR-NOT-DOT *CHAR-NOT-CRLF] CRLF
   multi-line-dotstuff = "." 1*CHAR-NOT-CRLF CRLF
           ;; A line containing only "." ends the multi-line.
           ;; Remove a leading '.' if followed by another '.'.
  string           = quoted-string / multi-line

Arguments:
  filter      points to the Sieve filter including its state
  id          specifies identifier to match

Returns:      1                success
              -1               syntax error
              0                identifier not matched
*/

static int parse_string(struct Sieve *filter, struct String *data)
{
int dataCapacity=0;

data->length=0;
data->character=(uschar*)0;
if (*filter->pc=='"') /* quoted string */
  {
  ++filter->pc;
  while (*filter->pc)
    {
    if (*filter->pc=='"') /* end of string */
      {
      int foo=data->length;

      ++filter->pc;
      data->character=string_cat(data->character,&dataCapacity,&foo,CUS "",1);
      return 1;
      }
    else if (*filter->pc=='\\' && *(filter->pc+1)) /* quoted character */
      {
      if (*(filter->pc+1)=='0') data->character=string_cat(data->character,&dataCapacity,&data->length,CUS "",1);
      else data->character=string_cat(data->character,&dataCapacity,&data->length,filter->pc+1,1);
      filter->pc+=2;
      }
    else /* regular character */
      {
      data->character=string_cat(data->character,&dataCapacity,&data->length,filter->pc,1);
      filter->pc++;
      }
    }
  filter->errmsg=CUS "missing end of string";
  return -1;
  }
else if (Ustrncmp(filter->pc,CUS "text:",5)==0) /* multiline string */
  {
  filter->pc+=5;
  /* skip optional white space followed by hashed comment or CRLF */
  while (*filter->pc==' ' || *filter->pc=='\t') ++filter->pc;
  if (*filter->pc=='#')
    {
    if (parse_hashcomment(filter)==-1) return -1;
    }
#ifdef RFC_EOL
  else if (*filter->pc=='\r' && *(filter->pc+1)=='\n')
#else
  else if (*filter->pc=='\n')
#endif
    {
#ifdef RFC_EOL
    filter->pc+=2;
#else
    ++filter->pc;
#endif
    ++filter->line;
    }
  else
    {
    filter->errmsg=CUS "syntax error";
    return -1;
    }
  while (*filter->pc)
    {
#ifdef RFC_EOL
    if (*filter->pc=='\r' && *(filter->pc+1)=='\n') /* end of line */
#else
    if (*filter->pc=='\n') /* end of line */
#endif
      {
      data->character=string_cat(data->character,&dataCapacity,&data->length,CUS "\r\n",2);
#ifdef RFC_EOL
      filter->pc+=2;
#else
      ++filter->pc;
#endif
      ++filter->line;
#ifdef RFC_EOL
      if (*filter->pc=='.' && *(filter->pc+1)=='\r' && *(filter->pc+2)=='\n') /* end of string */
#else
      if (*filter->pc=='.' && *(filter->pc+1)=='\n') /* end of string */
#endif
        {
        data->character=string_cat(data->character,&dataCapacity,&data->length,CUS "",1);
#ifdef RFC_EOL
        filter->pc+=3;
#else
        filter->pc+=2;
#endif
        ++filter->line;
        return 1;
        }
      else if (*filter->pc=='.' && *(filter->pc+1)=='.') /* remove dot stuffing */
        {
        data->character=string_cat(data->character,&dataCapacity,&data->length,CUS ".",1);
        filter->pc+=2;
        }
      }
    else /* regular character */
      {
      data->character=string_cat(data->character,&dataCapacity,&data->length,filter->pc,1);
      filter->pc++;
      }
    }
  filter->errmsg=CUS "missing end of multi line string";
  return -1;
  }
else return 0;
}


/*************************************************
*          Parse a specific identifier           *
*************************************************/

/*
Token definition:
  identifier       = (ALPHA / "_") *(ALPHA DIGIT "_")

Arguments:
  filter      points to the Sieve filter including its state
  id          specifies identifier to match

Returns:      1                success
              0                identifier not matched
*/

static int parse_identifier(struct Sieve *filter, const uschar *id)
{
  size_t idlen=Ustrlen(id);

  if (Ustrncmp(filter->pc,id,idlen)==0)
  {
    uschar next=filter->pc[idlen];

    if ((next>='A' && next<='Z') || (next>='a' && next<='z') || next=='_' || (next>='0' && next<='9')) return 0;
    filter->pc+=idlen;
    return 1;
  }
  else return 0;
}


/*************************************************
*                 Parse a number                 *
*************************************************/

/*
Token definition:
  number           = 1*DIGIT [QUANTIFIER]
  QUANTIFIER       = "K" / "M" / "G"

Arguments:
  filter      points to the Sieve filter including its state
  data        returns value

Returns:      1                success
              -1               no string list found
*/

static int parse_number(struct Sieve *filter, unsigned long *data)
{
unsigned long d,u;

if (*filter->pc>='0' && *filter->pc<='9')
  {
  uschar *e;

  errno=0;
  d=Ustrtoul(filter->pc,&e,10);
  if (errno==ERANGE)
    {
    filter->errmsg=CUstrerror(ERANGE);
    return -1;
    }
  filter->pc=e;
  u=1;
  if (*filter->pc=='K') { u=1024; ++filter->pc; }
  else if (*filter->pc=='M') { u=1024*1024; ++filter->pc; }
  else if (*filter->pc=='G') { u=1024*1024*1024; ++filter->pc; }
  if (d>(ULONG_MAX/u))
    {
    filter->errmsg=CUstrerror(ERANGE);
    return -1;
    }
  d*=u;
  *data=d;
  return 1;
  }
else
  {
  filter->errmsg=CUS "missing number";
  return -1;
  }
}


/*************************************************
*              Parse a string list               *
*************************************************/

/*
Grammar:
  string-list      = "[" string *("," string) "]" / string

Arguments:
  filter      points to the Sieve filter including its state
  data        returns string list

Returns:      1                success
              -1               no string list found
*/

static int parse_stringlist(struct Sieve *filter, struct String **data)
{
const uschar *orig=filter->pc;
int dataCapacity=0;
int dataLength=0;
struct String *d=(struct String*)0;
int m;

if (*filter->pc=='[') /* string list */
  {
  ++filter->pc;
  for (;;)
    {
    if (parse_white(filter)==-1) goto error;
    if ((dataLength+1)>=dataCapacity) /* increase buffer */
      {
      struct String *new;
      int newCapacity;          /* Don't amalgamate with next line; some compilers grumble */
      newCapacity=dataCapacity?(dataCapacity*=2):(dataCapacity=4);
      if ((new=(struct String*)store_get(sizeof(struct String)*newCapacity))==(struct String*)0)
        {
        filter->errmsg=CUstrerror(errno);
        goto error;
        }
      if (d) memcpy(new,d,sizeof(struct String)*dataLength);
      d=new;
      dataCapacity=newCapacity;
      }
    m=parse_string(filter,&d[dataLength]);
    if (m==0)
      {
      if (dataLength==0) break;
      else
        {
        filter->errmsg=CUS "missing string";
        goto error;
        }
      }
    else if (m==-1) goto error;
    else ++dataLength;
    if (parse_white(filter)==-1) goto error;
    if (*filter->pc==',') ++filter->pc;
    else break;
    }
  if (*filter->pc==']')
    {
    d[dataLength].character=(uschar*)0;
    d[dataLength].length=-1;
    ++filter->pc;
    *data=d;
    return 1;
    }
  else
    {
    filter->errmsg=CUS "missing closing bracket";
    goto error;
    }
  }
else /* single string */
  {
  if ((d=store_get(sizeof(struct String)*2))==(struct String*)0)
    {
    return -1;
    }
  m=parse_string(filter,&d[0]);
  if (m==-1)
    {
    return -1;
    }
  else if (m==0)
    {
    filter->pc=orig;
    return 0;
    }
  else
    {
    d[1].character=(uschar*)0;
    d[1].length=-1;
    *data=d;
    return 1;
    }
  }
error:
filter->errmsg=CUS "missing string list";
return -1;
}


/*************************************************
*    Parse an optional address part specifier    *
*************************************************/

/*
Grammar:
  address-part     =  ":localpart" / ":domain" / ":all"
  address-part     =/ ":user" / ":detail"

Arguments:
  filter      points to the Sieve filter including its state
  a           returns address part specified

Returns:      1                success
              0                no comparator found
              -1               syntax error
*/

static int parse_addresspart(struct Sieve *filter, enum AddressPart *a)
{
#ifdef SUBADDRESS
if (parse_identifier(filter,CUS ":user")==1)
  {
  if (!filter->require_subaddress)
    {
    filter->errmsg=CUS "missing previous require \"subaddress\";";
    return -1;
    }
  *a=ADDRPART_USER;
  return 1;
  }
else if (parse_identifier(filter,CUS ":detail")==1)
  {
  if (!filter->require_subaddress)
    {
    filter->errmsg=CUS "missing previous require \"subaddress\";";
    return -1;
    }
  *a=ADDRPART_DETAIL;
  return 1;
  }
else
#endif
if (parse_identifier(filter,CUS ":localpart")==1)
  {
  *a=ADDRPART_LOCALPART;
  return 1;
  }
else if (parse_identifier(filter,CUS ":domain")==1)
  {
  *a=ADDRPART_DOMAIN;
  return 1;
  }
else if (parse_identifier(filter,CUS ":all")==1)
  {
  *a=ADDRPART_ALL;
  return 1;
  }
else return 0;
}


/*************************************************
*         Parse an optional comparator           *
*************************************************/

/*
Grammar:
  comparator = ":comparator" <comparator-name: string>

Arguments:
  filter      points to the Sieve filter including its state
  c           returns comparator

Returns:      1                success
              0                no comparator found
              -1               incomplete comparator found
*/

static int parse_comparator(struct Sieve *filter, enum Comparator *c)
{
struct String comparator_name;

if (parse_identifier(filter,CUS ":comparator")==0) return 0;
if (parse_white(filter)==-1) return -1;
switch (parse_string(filter,&comparator_name))
  {
  case -1: return -1;
  case 0:
    {
    filter->errmsg=CUS "missing comparator";
    return -1;
    }
  default:
    {
    int match;

    if (eq_asciicase(&comparator_name,&str_ioctet,0))
      {
      *c=COMP_OCTET;
      match=1;
      }
    else if (eq_asciicase(&comparator_name,&str_iascii_casemap,0))
      {
      *c=COMP_EN_ASCII_CASEMAP;
      match=1;
      }
    else if (eq_asciicase(&comparator_name,&str_enascii_casemap,0))
      {
      *c=COMP_EN_ASCII_CASEMAP;
      match=1;
      }
    else if (eq_asciicase(&comparator_name,&str_iascii_numeric,0))
      {
      *c=COMP_ASCII_NUMERIC;
      match=1;
      }
    else
      {
      filter->errmsg=CUS "invalid comparator";
      match=-1;
      }
    return match;
    }
  }
}


/*************************************************
*          Parse an optional match type          *
*************************************************/

/*
Grammar:
  match-type = ":is" / ":contains" / ":matches"

Arguments:
  filter      points to the Sieve filter including its state
  m           returns match type

Returns:      1                success
              0                no match type found
*/

static int parse_matchtype(struct Sieve *filter, enum MatchType *m)
{
  if (parse_identifier(filter,CUS ":is")==1)
  {
    *m=MATCH_IS;
    return 1;
  }
  else if (parse_identifier(filter,CUS ":contains")==1)
  {
    *m=MATCH_CONTAINS;
    return 1;
  }
  else if (parse_identifier(filter,CUS ":matches")==1)
  {
    *m=MATCH_MATCHES;
    return 1;
  }
  else return 0;
}


/*************************************************
*   Parse and interpret an optional test list    *
*************************************************/

/*
Grammar:
  test-list = "(" test *("," test) ")"

Arguments:
  filter      points to the Sieve filter including its state
  n           total number of tests
  true        number of passed tests
  exec        Execute parsed statements

Returns:      1                success
              0                no test list found
              -1               syntax or execution error
*/

static int parse_testlist(struct Sieve *filter, int *n, int *true, int exec)
{
if (parse_white(filter)==-1) return -1;
if (*filter->pc=='(')
  {
  ++filter->pc;
  *n=0;
   *true=0;
  for (;;)
    {
    int cond;

    switch (parse_test(filter,&cond,exec))
      {
      case -1: return -1;
      case 0: filter->errmsg=CUS "missing test"; return -1;
      default: ++*n; if (cond) ++*true; break;
      }
    if (parse_white(filter)==-1) return -1;
    if (*filter->pc==',') ++filter->pc;
    else break;
    }
  if (*filter->pc==')')
    {
    ++filter->pc;
    return 1;
    }
  else
    {
    filter->errmsg=CUS "missing closing paren";
    return -1;
    }
  }
else return 0;
}


/*************************************************
*     Parse and interpret an optional test       *
*************************************************/

/*
Arguments:
  filter      points to the Sieve filter including its state
  cond        returned condition status
  exec        Execute parsed statements

Returns:      1                success
              0                no test found
              -1               syntax or execution error
*/

static int parse_test(struct Sieve *filter, int *cond, int exec)
{
if (parse_white(filter)==-1) return -1;
if (parse_identifier(filter,CUS "address"))
  {
  /*
  address-test = "address" { [address-part] [comparator] [match-type] }
                 <header-list: string-list> <key-list: string-list>

  header-list From, To, Cc, Bcc, Sender, Resent-From, Resent-To
  */

  enum AddressPart addressPart=ADDRPART_ALL;
  enum Comparator comparator=COMP_EN_ASCII_CASEMAP;
  enum MatchType matchType=MATCH_IS;
  struct String *hdr,*h,*key,*k;
  int m;
  int ap=0,co=0,mt=0;

  for (;;)
    {
    if (parse_white(filter)==-1) return -1;
    if ((m=parse_addresspart(filter,&addressPart))!=0)
      {
      if (m==-1) return -1;
      if (ap)
        {
        filter->errmsg=CUS "address part already specified";
        return -1;
        }
      else ap=1;
      }
    else if ((m=parse_comparator(filter,&comparator))!=0)
      {
      if (m==-1) return -1;
      if (co)
        {
        filter->errmsg=CUS "comparator already specified";
        return -1;
        }
      else co=1;
      }
    else if ((m=parse_matchtype(filter,&matchType))!=0)
      {
      if (m==-1) return -1;
      if (mt)
        {
        filter->errmsg=CUS "match type already specified";
        return -1;
        }
      else mt=1;
      }
    else break;
    }
  if (parse_white(filter)==-1) return -1;
  if ((m=parse_stringlist(filter,&hdr))!=1)
    {
    if (m==0) filter->errmsg=CUS "header string list expected";
    return -1;
    }
  if (parse_white(filter)==-1) return -1;
  if ((m=parse_stringlist(filter,&key))!=1)
    {
    if (m==0) filter->errmsg=CUS "key string list expected";
    return -1;
    }
  *cond=0;
  for (h=hdr; h->length!=-1 && !*cond; ++h)
    {
    uschar *header_value=(uschar*)0,*extracted_addr,*end_addr;

    if
      (
      !eq_asciicase(h,&str_from,0)
      && !eq_asciicase(h,&str_to,0)
      && !eq_asciicase(h,&str_cc,0)
      && !eq_asciicase(h,&str_bcc,0)
      && !eq_asciicase(h,&str_sender,0)
      && !eq_asciicase(h,&str_resent_from,0)
      && !eq_asciicase(h,&str_resent_to,0)
      )
      {
      filter->errmsg=CUS "invalid header field";
      return -1;
      }
    if (exec)
      {
      /* We are only interested in addresses below, so no MIME decoding */
      header_value=expand_string(string_sprintf("$rheader_%s",quote(h)));
      if (header_value == NULL)
        {
        filter->errmsg=CUS "header string expansion failed";
        return -1;
        }
      parse_allow_group = TRUE;
      while (*header_value && !*cond)
        {
        uschar *error;
        int start, end, domain;
        int saveend;
        uschar *part=NULL;

        end_addr = parse_find_address_end(header_value, FALSE);
        saveend = *end_addr;
        *end_addr = 0;
        extracted_addr = parse_extract_address(header_value, &error, &start, &end, &domain, FALSE);

        if (extracted_addr) switch (addressPart)
          {
          case ADDRPART_ALL: part=extracted_addr; break;
#ifdef SUBADDRESS
          case ADDRPART_USER:
#endif
          case ADDRPART_LOCALPART: part=extracted_addr; part[domain-1]='\0'; break;
          case ADDRPART_DOMAIN: part=extracted_addr+domain; break;
#ifdef SUBADDRESS
          case ADDRPART_DETAIL: part=NULL; break;
#endif
          }

        *end_addr = saveend;
        if (part)
          {
          for (k=key; k->length!=-1; ++k)
            {
            struct String partStr;

            partStr.character=part;
            partStr.length=Ustrlen(part);
            if (extracted_addr)
              {
              *cond=compare(filter,k,&partStr,comparator,matchType);
              if (*cond==-1) return -1;
              if (*cond) break;
              }
            }
          }
        if (saveend == 0) break;
        header_value = end_addr + 1;
        }
      }
    }
  return 1;
  }
else if (parse_identifier(filter,CUS "allof"))
  {
  /*
  allof-test   = "allof" <tests: test-list>
  */

  int n,true;

  switch (parse_testlist(filter,&n,&true,exec))
    {
    case -1: return -1;
    case 0: filter->errmsg=CUS "missing test list"; return -1;
    default: *cond=(n==true); return 1;
    }
  }
else if (parse_identifier(filter,CUS "anyof"))
  {
  /*
  anyof-test   = "anyof" <tests: test-list>
  */

  int n,true;

  switch (parse_testlist(filter,&n,&true,exec))
    {
    case -1: return -1;
    case 0: filter->errmsg=CUS "missing test list"; return -1;
    default: *cond=(true>0); return 1;
    }
  }
else if (parse_identifier(filter,CUS "exists"))
  {
  /*
  exists-test = "exists" <header-names: string-list>
  */

  struct String *hdr,*h;
  int m;

  if (parse_white(filter)==-1) return -1;
  if ((m=parse_stringlist(filter,&hdr))!=1)
    {
    if (m==0) filter->errmsg=CUS "header string list expected";
    return -1;
    }
  if (exec)
    {
    *cond=1;
    for (h=hdr; h->length!=-1 && *cond; ++h)
      {
      uschar *header_def;

      header_def=expand_string(string_sprintf("${if def:header_%s {true}{false}}",quote(h)));
      if (header_def == NULL)
        {
        filter->errmsg=CUS "header string expansion failed";
        return -1;
        }
      if (Ustrcmp(header_def,"false")==0) *cond=0;
      }
    }
  return 1;
  }
else if (parse_identifier(filter,CUS "false"))
  {
  /*
  false-test = "false"
  */

  *cond=0;
  return 1;
  }
else if (parse_identifier(filter,CUS "header"))
  {
  /*
  header-test = "header" { [comparator] [match-type] }
                <header-names: string-list> <key-list: string-list>
  */

  enum Comparator comparator=COMP_EN_ASCII_CASEMAP;
  enum MatchType matchType=MATCH_IS;
  struct String *hdr,*h,*key,*k;
  int m;
  int co=0,mt=0;

  for (;;)
    {
    if (parse_white(filter)==-1) return -1;
    if ((m=parse_comparator(filter,&comparator))!=0)
      {
      if (m==-1) return -1;
      if (co)
        {
        filter->errmsg=CUS "comparator already specified";
        return -1;
        }
      else co=1;
      }
    else if ((m=parse_matchtype(filter,&matchType))!=0)
      {
      if (m==-1) return -1;
      if (mt)
        {
        filter->errmsg=CUS "match type already specified";
        return -1;
        }
      else mt=1;
      }
    else break;
    }
  if (parse_white(filter)==-1) return -1;
  if ((m=parse_stringlist(filter,&hdr))!=1)
    {
    if (m==0) filter->errmsg=CUS "header string list expected";
    return -1;
    }
  if (parse_white(filter)==-1) return -1;
  if ((m=parse_stringlist(filter,&key))!=1)
    {
    if (m==0) filter->errmsg=CUS "key string list expected";
    return -1;
    }
  *cond=0;
  for (h=hdr; h->length!=-1 && !*cond; ++h)
    {
    if (!is_header(h))
      {
      filter->errmsg=CUS "invalid header field";
      return -1;
      }
    if (exec)
      {
      struct String header_value;
      uschar *header_def;

      expand_header(&header_value,h);
      header_def=expand_string(string_sprintf("${if def:header_%s {true}{false}}",quote(h)));
      if (header_value.character == NULL || header_def == NULL)
        {
        filter->errmsg=CUS "header string expansion failed";
        return -1;
        }
      for (k=key; k->length!=-1; ++k)
        {
        if (Ustrcmp(header_def,"true")==0)
          {
          *cond=compare(filter,k,&header_value,comparator,matchType);
          if (*cond==-1) return -1;
          if (*cond) break;
          }
        }
      }
    }
  return 1;
  }
else if (parse_identifier(filter,CUS "not"))
  {
  if (parse_white(filter)==-1) return -1;
  switch (parse_test(filter,cond,exec))
    {
    case -1: return -1;
    case 0: filter->errmsg=CUS "missing test"; return -1;
    default: *cond=!*cond; return 1;
    }
  }
else if (parse_identifier(filter,CUS "size"))
  {
  /*
  relop = ":over" / ":under"
  size-test = "size" relop <limit: number>
  */

  unsigned long limit;
  int overNotUnder;

  if (parse_white(filter)==-1) return -1;
  if (parse_identifier(filter,CUS ":over")) overNotUnder=1;
  else if (parse_identifier(filter,CUS ":under")) overNotUnder=0;
  else
    {
    filter->errmsg=CUS "missing :over or :under";
    return -1;
    }
  if (parse_white(filter)==-1) return -1;
  if (parse_number(filter,&limit)==-1) return -1;
  *cond=(overNotUnder ? (message_size>limit) : (message_size<limit));
  return 1;
  }
else if (parse_identifier(filter,CUS "true"))
  {
  *cond=1;
  return 1;
  }
else if (parse_identifier(filter,CUS "envelope"))
  {
  /*
  envelope-test = "envelope" { [comparator] [address-part] [match-type] }
                  <envelope-part: string-list> <key-list: string-list>

  envelope-part is case insensitive "from" or "to"
  */

  enum Comparator comparator=COMP_EN_ASCII_CASEMAP;
  enum AddressPart addressPart=ADDRPART_ALL;
  enum MatchType matchType=MATCH_IS;
  struct String *env,*e,*key,*k;
  int m;
  int co=0,ap=0,mt=0;

  if (!filter->require_envelope)
    {
    filter->errmsg=CUS "missing previous require \"envelope\";";
    return -1;
    }
  for (;;)
    {
    if (parse_white(filter)==-1) return -1;
    if ((m=parse_comparator(filter,&comparator))!=0)
      {
      if (m==-1) return -1;
      if (co)
        {
        filter->errmsg=CUS "comparator already specified";
        return -1;
        }
      else co=1;
      }
    else if ((m=parse_addresspart(filter,&addressPart))!=0)
      {
      if (m==-1) return -1;
      if (ap)
        {
        filter->errmsg=CUS "address part already specified";
        return -1;
        }
      else ap=1;
      }
    else if ((m=parse_matchtype(filter,&matchType))!=0)
      {
      if (m==-1) return -1;
      if (mt)
        {
        filter->errmsg=CUS "match type already specified";
        return -1;
        }
      else mt=1;
      }
    else break;
    }
  if (parse_white(filter)==-1) return -1;
  if ((m=parse_stringlist(filter,&env))!=1)
    {
    if (m==0) filter->errmsg=CUS "envelope string list expected";
    return -1;
    }
  if (parse_white(filter)==-1) return -1;
  if ((m=parse_stringlist(filter,&key))!=1)
    {
    if (m==0) filter->errmsg=CUS "key string list expected";
    return -1;
    }
  *cond=0;
  for (e=env; e->character; ++e)
    {
    const uschar *envelopeExpr=CUS 0;
    uschar *envelope=US 0;

    if (eq_asciicase(e,&str_from,0))
      {
      switch (addressPart)
        {
        case ADDRPART_ALL: envelopeExpr=CUS "$sender_address"; break;
#ifdef SUBADDRESS
        case ADDRPART_USER:
#endif
        case ADDRPART_LOCALPART: envelopeExpr=CUS "${local_part:$sender_address}"; break;
        case ADDRPART_DOMAIN: envelopeExpr=CUS "${domain:$sender_address}"; break;
#ifdef SUBADDRESS
        case ADDRPART_DETAIL: envelopeExpr=CUS 0; break;
#endif
        }
      }
    else if (eq_asciicase(e,&str_to,0))
      {
      switch (addressPart)
        {
        case ADDRPART_ALL: envelopeExpr=CUS "$local_part_prefix$local_part$local_part_suffix@$domain"; break;
#ifdef SUBADDRESS
        case ADDRPART_USER: envelopeExpr=filter->useraddress; break;
        case ADDRPART_DETAIL: envelopeExpr=filter->subaddress; break;
#endif
        case ADDRPART_LOCALPART: envelopeExpr=CUS "$local_part_prefix$local_part$local_part_suffix"; break;
        case ADDRPART_DOMAIN: envelopeExpr=CUS "$domain"; break;
        }
      }
    else
      {
      filter->errmsg=CUS "invalid envelope string";
      return -1;
      }
    if (exec && envelopeExpr)
      {
      if ((envelope=expand_string(US envelopeExpr)) == NULL)
        {
        filter->errmsg=CUS "header string expansion failed";
        return -1;
        }
      for (k=key; k->length!=-1; ++k)
        {
        struct String envelopeStr;

        envelopeStr.character=envelope;
        envelopeStr.length=Ustrlen(envelope);
        *cond=compare(filter,k,&envelopeStr,comparator,matchType);
        if (*cond==-1) return -1;
        if (*cond) break;
        }
      }
    }
  return 1;
  }
else return 0;
}


/*************************************************
*     Parse and interpret an optional block      *
*************************************************/

/*
Arguments:
  filter      points to the Sieve filter including its state
  exec        Execute parsed statements
  generated   where to hang newly-generated addresses

Returns:      2                success by stop
              1                other success
              0                no block command found
              -1               syntax or execution error
*/

static int parse_block(struct Sieve *filter, int exec,
  address_item **generated)
{
int r;

if (parse_white(filter)==-1) return -1;
if (*filter->pc=='{')
  {
  ++filter->pc;
  if ((r=parse_commands(filter,exec,generated))==-1 || r==2) return r;
  if (*filter->pc=='}')
    {
    ++filter->pc;
    return 1;
    }
  else
    {
    filter->errmsg=CUS "expecting command or closing brace";
    return -1;
    }
  }
else return 0;
}


/*************************************************
*           Match a semicolon                    *
*************************************************/

/*
Arguments:
  filter      points to the Sieve filter including its state

Returns:      1                success
              -1               syntax error
*/

static int parse_semicolon(struct Sieve *filter)
{
  if (parse_white(filter)==-1) return -1;
  if (*filter->pc==';')
  {
    ++filter->pc;
    return 1;
  }
  else
  {
    filter->errmsg=CUS "missing semicolon";
    return -1;
  }
}


/*************************************************
*     Parse and interpret a Sieve command        *
*************************************************/

/*
Arguments:
  filter      points to the Sieve filter including its state
  exec        Execute parsed statements
  generated   where to hang newly-generated addresses

Returns:      2                success by stop
              1                other success
              -1               syntax or execution error
*/
static int parse_commands(struct Sieve *filter, int exec,
  address_item **generated)
{
while (*filter->pc)
  {
  if (parse_white(filter)==-1) return -1;
  if (parse_identifier(filter,CUS "if"))
    {
    /*
    if-command = "if" test block *( "elsif" test block ) [ else block ]
    */

    int cond,m,unsuccessful;

    /* test block */
    if (parse_white(filter)==-1) return -1;
    if ((m=parse_test(filter,&cond,exec))==-1) return -1;
    if (m==0)
      {
      filter->errmsg=CUS "missing test";
      return -1;
      }
    m=parse_block(filter,exec ? cond : 0, generated);
    if (m==-1 || m==2) return m;
    if (m==0)
      {
      filter->errmsg=CUS "missing block";
      return -1;
      }
    unsuccessful = !cond;
    for (;;) /* elsif test block */
      {
      if (parse_white(filter)==-1) return -1;
      if (parse_identifier(filter,CUS "elsif"))
        {
        if (parse_white(filter)==-1) return -1;
        m=parse_test(filter,&cond,exec && unsuccessful);
        if (m==-1 || m==2) return m;
        if (m==0)
          {
          filter->errmsg=CUS "missing test";
          return -1;
          }
        m=parse_block(filter,exec && unsuccessful ? cond : 0, generated);
        if (m==-1 || m==2) return m;
        if (m==0)
          {
          filter->errmsg=CUS "missing block";
          return -1;
          }
        if (exec && unsuccessful && cond) unsuccessful = 0;
        }
      else break;
      }
    /* else block */
    if (parse_white(filter)==-1) return -1;
    if (parse_identifier(filter,CUS "else"))
      {
      m=parse_block(filter,exec && unsuccessful, generated);
      if (m==-1 || m==2) return m;
      if (m==0)
        {
        filter->errmsg=CUS "missing block";
        return -1;
        }
      }
    }
  else if (parse_identifier(filter,CUS "stop"))
    {
    /*
    stop-command     =  "stop" { stop-options } ";"
    stop-options     =
    */

    if (parse_semicolon(filter)==-1) return -1;
    if (exec)
      {
      filter->pc+=Ustrlen(filter->pc);
      return 2;
      }
    }
  else if (parse_identifier(filter,CUS "keep"))
    {
    /*
    keep-command     =  "keep" { keep-options } ";"
    keep-options     =
    */

    if (parse_semicolon(filter)==-1) return -1;
    if (exec)
      {
      add_addr(generated,US"inbox",1,0,0,0);
      filter->keep = 0;
      }
    }
  else if (parse_identifier(filter,CUS "discard"))
    {
    /*
    discard-command  =  "discard" { discard-options } ";"
    discard-options  =
    */

    if (parse_semicolon(filter)==-1) return -1;
    if (exec) filter->keep=0;
    }
  else if (parse_identifier(filter,CUS "redirect"))
    {
    /*
    redirect-command =  "redirect" redirect-options "string" ";"
    redirect-options =
    redirect-options =) ":copy"
    */

    struct String recipient;
    int m;
    int copy=0;

    for (;;)
      {
      if (parse_white(filter)==-1) return -1;
      if (parse_identifier(filter,CUS ":copy")==1)
        {
        if (!filter->require_copy)
          {
          filter->errmsg=CUS "missing previous require \"copy\";";
          return -1;
          }
          copy=1;
        }
      else break;
      }
    if (parse_white(filter)==-1) return -1;
    if ((m=parse_string(filter,&recipient))!=1)
      {
      if (m==0) filter->errmsg=CUS "missing redirect recipient string";
      return -1;
      }
    if (strchr(CCS recipient.character,'@')==(char*)0)
      {
      filter->errmsg=CUS "unqualified recipient address";
      return -1;
      }
    if (exec)
      {
      add_addr(generated,recipient.character,0,0,0,0);
      if (!copy) filter->keep = 0;
      }
    if (parse_semicolon(filter)==-1) return -1;
    }
  else if (parse_identifier(filter,CUS "fileinto"))
    {
    /*
    fileinto-command =  "fileinto" { fileinto-options } string ";"
    fileinto-options =
    fileinto-options =) [ ":copy" ]
   */

    struct String folder;
    uschar *s;
    int m;
    unsigned long maxage, maxmessages, maxstorage;
    int copy=0;

    maxage = maxmessages = maxstorage = 0;
    if (!filter->require_fileinto)
      {
      filter->errmsg=CUS "missing previous require \"fileinto\";";
      return -1;
      }
    for (;;)
      {
      if (parse_white(filter)==-1) return -1;
      if (parse_identifier(filter,CUS ":copy")==1)
        {
        if (!filter->require_copy)
          {
          filter->errmsg=CUS "missing previous require \"copy\";";
          return -1;
          }
          copy=1;
        }
      else break;
      }
    if (parse_white(filter)==-1) return -1;
    if ((m=parse_string(filter,&folder))!=1)
      {
      if (m==0) filter->errmsg=CUS "missing fileinto folder string";
      return -1;
      }
    m=0; s=folder.character;
    if (folder.length==0) m=1;
    if (Ustrcmp(s,"..")==0 || Ustrncmp(s,"../",3)==0) m=1;
    else while (*s)
      {
      if (Ustrcmp(s,"/..")==0 || Ustrncmp(s,"/../",4)==0) { m=1; break; }
      ++s;
      }
    if (m)
      {
      filter->errmsg=CUS "invalid folder";
      return -1;
      }
    if (exec)
      {
      add_addr(generated, folder.character, 1, maxage, maxmessages, maxstorage);
      if (!copy) filter->keep = 0;
      }
    if (parse_semicolon(filter)==-1) return -1;
    }
#ifdef VACATION
  else if (parse_identifier(filter,CUS "vacation"))
    {
    /*
    vacation-command =  "vacation" { vacation-options } <reason: string> ";"
    vacation-options =  [":days" number]
                        [":subject" string]
                        [":from" string]
                        [":addresses" string-list]
                        [":mime"]
                        [":handle" string]
    */

    int m;
    unsigned long days;
    struct String subject;
    struct String from;
    struct String *addresses;
    int reason_is_mime;
    string_item *aliases;
    struct String handle;
    struct String reason;

    if (!filter->require_vacation)
      {
      filter->errmsg=CUS "missing previous require \"vacation\";";
      return -1;
      }
    if (exec)
      {
      if (filter->vacation_ran)
        {
        filter->errmsg=CUS "trying to execute vacation more than once";
        return -1;
        }
      filter->vacation_ran=1;
      }
    days=VACATION_MIN_DAYS>7 ? VACATION_MIN_DAYS : 7;
    subject.character=(uschar*)0;
    subject.length=-1;
    from.character=(uschar*)0;
    from.length=-1;
    addresses=(struct String*)0;
    aliases=NULL;
    reason_is_mime=0;
    handle.character=(uschar*)0;
    handle.length=-1;
    for (;;)
      {
      if (parse_white(filter)==-1) return -1;
      if (parse_identifier(filter,CUS ":days")==1)
        {
        if (parse_white(filter)==-1) return -1;
        if (parse_number(filter,&days)==-1) return -1;
        if (days<VACATION_MIN_DAYS) days=VACATION_MIN_DAYS;
        else if (days>VACATION_MAX_DAYS) days=VACATION_MAX_DAYS;
        }
      else if (parse_identifier(filter,CUS ":subject")==1)
        {
        if (parse_white(filter)==-1) return -1;
        if ((m=parse_string(filter,&subject))!=1)
          {
          if (m==0) filter->errmsg=CUS "subject string expected";
          return -1;
          }
        }
      else if (parse_identifier(filter,CUS ":from")==1)
        {
        int start, end, domain;
        uschar *error,*ss;

        if (parse_white(filter)==-1) return -1;
        if ((m=parse_string(filter,&from))!=1)
          {
          if (m==0) filter->errmsg=CUS "from string expected";
          return -1;
          }
        if (from.length>0)
          {
          ss = parse_extract_address(from.character, &error, &start, &end, &domain,
            FALSE);
          if (ss == NULL)
            {
            filter->errmsg=string_sprintf("malformed address \"%s\" in "
              "Sieve filter: %s", from.character, error);
            return -1;
            }
          }
        else
          {
          filter->errmsg=CUS "empty :from address in Sieve filter";
          return -1;
          }
        }
      else if (parse_identifier(filter,CUS ":addresses")==1)
        {
        struct String *a;

        if (parse_white(filter)==-1) return -1;
        if ((m=parse_stringlist(filter,&addresses))!=1)
          {
          if (m==0) filter->errmsg=CUS "addresses string list expected";
          return -1;
          }
        for (a=addresses; a->length!=-1; ++a)
          {
          string_item *new;

          new=store_get(sizeof(string_item));
          new->text=store_get(a->length+1);
          if (a->length) memcpy(new->text,a->character,a->length);
          new->text[a->length]='\0';
          new->next=aliases;
          aliases=new;
          }
        }
      else if (parse_identifier(filter,CUS ":mime")==1)
        reason_is_mime=1;
      else if (parse_identifier(filter,CUS ":handle")==1)
        {
        if (parse_white(filter)==-1) return -1;
        if ((m=parse_string(filter,&from))!=1)
          {
          if (m==0) filter->errmsg=CUS "handle string expected";
          return -1;
          }
        }
      else break;
      }
    if (parse_white(filter)==-1) return -1;
    if ((m=parse_string(filter,&reason))!=1)
      {
      if (m==0) filter->errmsg=CUS "missing reason string";
      return -1;
      }
    if (reason_is_mime)
      {
      uschar *s,*end;

      for (s=reason.character,end=reason.character+reason.length; s<end && (*s&0x80)==0; ++s);
      if (s<end)
        {
        filter->errmsg=CUS "MIME reason string contains 8bit text";
        return -1;
        }
      }
    if (parse_semicolon(filter)==-1) return -1;

    if (exec)
      {
      address_item *addr;
      int capacity,start;
      uschar *buffer;
      int buffer_capacity;
      struct String key;
      md5 base;
      uschar digest[16];
      uschar hexdigest[33];
      int i;
      uschar *once;

      if (filter_personal(aliases,TRUE))
        {
        if (filter_test == FTEST_NONE)
          {
          /* ensure oncelog directory exists; failure will be detected later */

          (void)directory_make(NULL, filter->vacation_directory, 0700, FALSE);
          }
        /* build oncelog filename */

        key.character=(uschar*)0;
        key.length=0;
        capacity=0;
        if (handle.length==-1)
          {
          if (subject.length!=-1) key.character=string_cat(key.character,&capacity,&key.length,subject.character,subject.length);
          if (from.length!=-1) key.character=string_cat(key.character,&capacity,&key.length,from.character,from.length);
          key.character=string_cat(key.character,&capacity,&key.length,reason_is_mime?US"1":US"0",1);
          key.character=string_cat(key.character,&capacity,&key.length,reason.character,reason.length);
          }
        else
          key=handle;
        md5_start(&base);
        md5_end(&base, key.character, key.length, digest);
        for (i = 0; i < 16; i++) sprintf(CS (hexdigest+2*i), "%02X", digest[i]);
        if (filter_test != FTEST_NONE)
          {
          debug_printf("Sieve: mail was personal, vacation file basename: %s\n", hexdigest);
          }
        else
          {
          capacity=Ustrlen(filter->vacation_directory);
          start=capacity;
          once=string_cat(filter->vacation_directory,&capacity,&start,US"/",1);
          once=string_cat(once,&capacity,&start,hexdigest,33);
          once[start] = '\0';

          /* process subject */

          if (subject.length==-1)
            {
            expand_header(&subject,&str_subject);
            capacity=6;
            start=6;
            subject.character=string_cat(US"Auto: ",&capacity,&start,subject.character,subject.length);
            subject.length=start;
            }

          /* add address to list of generated addresses */

          addr = deliver_make_addr(string_sprintf(">%.256s", sender_address), FALSE);
          setflag(addr, af_pfr);
          setflag(addr, af_ignore_error);
          addr->next = *generated;
          *generated = addr;
          addr->reply = store_get(sizeof(reply_item));
          memset(addr->reply,0,sizeof(reply_item)); /* XXX */
          addr->reply->to = string_copy(sender_address);
          if (from.length==-1)
            addr->reply->from = expand_string(US"$local_part@$domain");
          else
            addr->reply->from = from.character;
          /* Allocation is larger than neccessary, but enough even for split MIME words */
          buffer_capacity=16+4*subject.length;
          buffer=store_get(buffer_capacity);
          addr->reply->subject=parse_quote_2047(subject.character, subject.length, US"utf-8", buffer, buffer_capacity);
          addr->reply->oncelog=once;
          addr->reply->once_repeat=days*86400;

          /* build body and MIME headers */

          if (reason_is_mime)
            {
            uschar *mime_body,*reason_end;
#ifdef RFC_EOL
            static const uschar nlnl[]="\r\n\r\n";
#else
            static const uschar nlnl[]="\n\n";
#endif

            for
              (
              mime_body=reason.character,reason_end=reason.character+reason.length;
              mime_body<(reason_end-sizeof(nlnl)-1) && memcmp(mime_body,nlnl,sizeof(nlnl)-1);
              ++mime_body
              );
            capacity = 0;
            start = 0;
            addr->reply->headers = string_cat(NULL,&capacity,&start,reason.character,mime_body-reason.character);
            addr->reply->headers[start] = '\0';
            capacity = 0;
            start = 0;
            if (mime_body+(sizeof(nlnl)-1)<reason_end) mime_body+=sizeof(nlnl)-1;
            else mime_body=reason_end-1;
            addr->reply->text = string_cat(NULL,&capacity,&start,mime_body,reason_end-mime_body);
            addr->reply->text[start] = '\0';
            }
          else
            {
            struct String qp;

            capacity = 0;
            start = reason.length;
            addr->reply->headers = US"MIME-Version: 1.0\n"
                                   "Content-Type: text/plain;\n"
                                   "\tcharset=\"utf-8\"\n"
                                   "Content-Transfer-Encoding: quoted-printable";
            addr->reply->text = quoted_printable_encode(&reason,&qp)->character;
            }
          }
        }
        else if (filter_test != FTEST_NONE)
          {
          debug_printf("Sieve: mail was not personal, vacation would ignore it\n");
          }
      }
    }
    else break;
#endif
  }
return 1;
}


/*************************************************
*       Parse and interpret a sieve filter       *
*************************************************/

/*
Arguments:
  filter      points to the Sieve filter including its state
  exec        Execute parsed statements
  generated   where to hang newly-generated addresses

Returns:      1                success
              -1               syntax or execution error
*/

static int parse_start(struct Sieve *filter, int exec,
  address_item **generated)
{
filter->pc=filter->filter;
filter->line=1;
filter->keep=1;
filter->require_envelope=0;
filter->require_fileinto=0;
#ifdef SUBADDRESS
filter->require_subaddress=0;
#endif
#ifdef VACATION
filter->require_vacation=0;
filter->vacation_ran=0;
#endif
filter->require_copy=0;
filter->require_iascii_numeric=0;

if (parse_white(filter)==-1) return -1;

if (exec && filter->vacation_directory != NULL && filter_test == FTEST_NONE)
  {
  DIR *oncelogdir;
  struct dirent *oncelog;
  struct stat properties;
  time_t now;

  /* clean up old vacation log databases */

  oncelogdir=opendir(CS filter->vacation_directory);

  if (oncelogdir ==(DIR*)0 && errno != ENOENT)
    {
    filter->errmsg=CUS "unable to open vacation directory";
    return -1;
    }

  if (oncelogdir != NULL)
    {
    time(&now);

    while ((oncelog=readdir(oncelogdir))!=(struct dirent*)0)
      {
      if (strlen(oncelog->d_name)==32)
        {
        uschar *s=string_sprintf("%s/%s",filter->vacation_directory,oncelog->d_name);
        if (Ustat(s,&properties)==0 && (properties.st_mtime+VACATION_MAX_DAYS*86400)<now)
          Uunlink(s);
        }
      }
    closedir(oncelogdir);
    }
  }

while (parse_identifier(filter,CUS "require"))
  {
  /*
  require-command = "require" <capabilities: string-list>
  */

  struct String *cap,*check;
  int m;

  if (parse_white(filter)==-1) return -1;
  if ((m=parse_stringlist(filter,&cap))!=1)
    {
    if (m==0) filter->errmsg=CUS "capability string list expected";
    return -1;
    }
  for (check=cap; check->character; ++check)
    {
    if (eq_octet(check,&str_envelope,0)) filter->require_envelope=1;
    else if (eq_octet(check,&str_fileinto,0)) filter->require_fileinto=1;
#ifdef SUBADDRESS
    else if (eq_octet(check,&str_subaddress,0)) filter->require_subaddress=1;
#endif
#ifdef VACATION
    else if (eq_octet(check,&str_vacation,0))
      {
      if (filter_test == FTEST_NONE && filter->vacation_directory == NULL)
        {
        filter->errmsg=CUS "vacation disabled";
        return -1;
        }
      filter->require_vacation=1;
      }
#endif
    else if (eq_octet(check,&str_copy,0)) filter->require_copy=1;
    else if (eq_octet(check,&str_comparator_ioctet,0)) ;
    else if (eq_octet(check,&str_comparator_iascii_casemap,0)) ;
    else if (eq_octet(check,&str_comparator_enascii_casemap,0)) ;
    else if (eq_octet(check,&str_comparator_iascii_numeric,0)) filter->require_iascii_numeric=1;
    else
      {
      filter->errmsg=CUS "unknown capability";
      return -1;
      }
    }
    if (parse_semicolon(filter)==-1) return -1;
  }
  if (parse_commands(filter,exec,generated)==-1) return -1;
  if (*filter->pc)
    {
    filter->errmsg=CUS "syntax error";
    return -1;
    }
  return 1;
}


/*************************************************
*            Interpret a sieve filter file       *
*************************************************/

/*
Arguments:
  filter      points to the entire file, read into store as a single string
  options     controls whether various special things are allowed, and requests
              special actions (not currently used)
  sieve_vacation_directory  where to store vacation "once" files
  useraddress string expression for :user part of address
  subaddress  string expression for :subaddress part of address
  generated   where to hang newly-generated addresses
  error       where to pass back an error text

Returns:      FF_DELIVERED     success, a significant action was taken
              FF_NOTDELIVERED  success, no significant action
              FF_DEFER         defer requested
              FF_FAIL          fail requested
              FF_FREEZE        freeze requested
              FF_ERROR         there was a problem
*/

int
sieve_interpret(uschar *filter, int options, uschar *vacation_directory,
  uschar *useraddress, uschar *subaddress, address_item **generated, uschar **error)
{
struct Sieve sieve;
int r;
uschar *msg;

options = options; /* Keep picky compilers happy */
error = error;

DEBUG(D_route) debug_printf("Sieve: start of processing\n");
sieve.filter=filter;

if (vacation_directory == NULL)
  sieve.vacation_directory = NULL;
else
  {
  sieve.vacation_directory=expand_string(vacation_directory);
  if (sieve.vacation_directory == NULL)
    {
    *error = string_sprintf("failed to expand \"%s\" "
      "(sieve_vacation_directory): %s", vacation_directory,
      expand_string_message);
    return FF_ERROR;
    }
  }

sieve.useraddress = useraddress == NULL ? CUS "$local_part_prefix$local_part$local_part_suffix" : useraddress;
sieve.subaddress = subaddress;

#ifdef COMPILE_SYNTAX_CHECKER
if (parse_start(&sieve,0,generated)==1)
#else
if (parse_start(&sieve,1,generated)==1)
#endif
  {
  if (sieve.keep)
    {
    add_addr(generated,US"inbox",1,0,0,0);
    msg = string_sprintf("Keep");
    r = FF_DELIVERED;
    }
    else
    {
    msg = string_sprintf("No keep");
    r = FF_DELIVERED;
    }
  }
else
  {
  msg = string_sprintf("Sieve error: %s in line %d",sieve.errmsg,sieve.line);
#ifdef COMPILE_SYNTAX_CHECKER
  r = FF_ERROR;
  *error = msg;
#else
  add_addr(generated,US"inbox",1,0,0,0);
  r = FF_DELIVERED;
#endif
  }

#ifndef COMPILE_SYNTAX_CHECKER
if (filter_test != FTEST_NONE) printf("%s\n", (const char*) msg);
  else debug_printf("%s\n", msg);
#endif

DEBUG(D_route) debug_printf("Sieve: end of processing\n");
return r;
}
