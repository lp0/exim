/* $Cambridge: exim/exim-src/src/spool_mbox.c,v 1.9 2005/08/01 13:51:05 ph10 Exp $ */

/*************************************************
*     Exim - an Internet mail transport agent    *
*************************************************/

/* Copyright (c) Tom Kistner <tom@duncanthrax.net> 2003-???? */
/* License: GPL */

/* Code for setting up a MBOX style spool file inside a /scan/<msgid>
sub directory of exim's spool directory. */

#include "exim.h"
#ifdef WITH_CONTENT_SCAN

/* externals, we must reset them on unspooling */
#ifdef WITH_OLD_DEMIME
extern int demime_ok;
extern struct file_extension *file_extensions;
#endif

extern int malware_ok;
extern int spam_ok;

int spool_mbox_ok = 0;
uschar spooled_message_id[17];

/* returns a pointer to the FILE, and puts the size in bytes into mbox_file_size */

FILE *spool_mbox(unsigned long *mbox_file_size) {
  uschar mbox_path[1024];
  uschar message_subdir[2];
  uschar data_buffer[65535];
  FILE *mbox_file;
  FILE *data_file = NULL;
  header_line *my_headerlist;
  struct stat statbuf;
  int i,j;
  uschar *mbox_delimiter;
  uschar *envelope_from;
  uschar *envelope_to;

  if (!spool_mbox_ok) {
    /* create scan directory, if not present */
    if (!directory_make(spool_directory, US "scan", 0750, FALSE)) {
      debug_printf("unable to create directory: %s/scan\n", spool_directory);
      return NULL;
    };

    /* create temp directory inside scan dir */
    snprintf(CS mbox_path, 1024, "%s/scan/%s", spool_directory, message_id);
    if (!directory_make(NULL, mbox_path, 0750, FALSE)) {
      debug_printf("unable to create directory: %s/scan/%s\n", spool_directory, message_id);
      return NULL;
    };

    /* open [message_id].eml file for writing */
    snprintf(CS mbox_path, 1024, "%s/scan/%s/%s.eml", spool_directory, message_id, message_id);
    mbox_file = Ufopen(mbox_path,"wb");

    if (mbox_file == NULL) {
      debug_printf("unable to open file for writing: %s\n", mbox_path);
      return NULL;
    };

    /* Generate mailbox delimiter */
    mbox_delimiter = expand_string(US"From ${sender_address} ${tod_bsdinbox}\n");
    if (mbox_delimiter != NULL) {
      if (mbox_delimiter[0] != 0) {
        i = fwrite(mbox_delimiter, 1, Ustrlen(mbox_delimiter), mbox_file);
        if (i != Ustrlen(mbox_delimiter)) {
          debug_printf("error/short write on writing in: %s", mbox_path);
          (void)fclose(mbox_file);
          return NULL;
        };
      };
    };
    /* Generate X-Envelope-From header */
    envelope_from = expand_string(US"${sender_address}");
    if (envelope_from != NULL) {
      if (envelope_from[0] != 0) {
        uschar *my_envelope_from;
        my_envelope_from = string_sprintf("X-Envelope-From: <%s>\n", envelope_from);
        i = fwrite(my_envelope_from, 1, Ustrlen(my_envelope_from), mbox_file);
        if (i != Ustrlen(my_envelope_from)) {
          debug_printf("error/short write on writing in: %s", mbox_path);
          (void)fclose(mbox_file);
          return NULL;
        };
      };
    };
    /* Generate X-Envelope-To header */
    envelope_to = expand_string(US"${if def:received_for{$received_for}}");
    if (envelope_to != NULL) {
      if (envelope_to[0] != 0) {
        uschar *my_envelope_to;
        my_envelope_to = string_sprintf("X-Envelope-To: <%s>\n", envelope_to);
        i = fwrite(my_envelope_to, 1, Ustrlen(my_envelope_to), mbox_file);
        if (i != Ustrlen(my_envelope_to)) {
          debug_printf("error/short write on writing in: %s", mbox_path);
          (void)fclose(mbox_file);
          return NULL;
        };
      };
    };

    /* write all header lines to mbox file */
    my_headerlist = header_list;
    while (my_headerlist != NULL) {

      /* skip deleted headers */
      if (my_headerlist->type == '*') {
        my_headerlist = my_headerlist->next;
        continue;
      };

      i = fwrite(my_headerlist->text, 1, my_headerlist->slen, mbox_file);
      if (i != my_headerlist->slen) {
        debug_printf("error/short write on writing in: %s", mbox_path);
        (void)fclose(mbox_file);
        return NULL;
      };

      my_headerlist = my_headerlist->next;
    };

    /* copy body file */
    message_subdir[1] = '\0';
    for (i = 0; i < 2; i++) {
      message_subdir[0] = (split_spool_directory == (i == 0))? message_id[5] : 0;
      sprintf(CS mbox_path, "%s/input/%s/%s-D", spool_directory, message_subdir, message_id);
      data_file = Ufopen(mbox_path,"rb");
      if (data_file != NULL)
        break;
    };

    /* The code used to use this line, but it doesn't work in Cygwin.
     *
     *  (void)fread(data_buffer, 1, 18, data_file);
     *
     * What's happening is that spool_mbox used to use an fread to jump over the
     * file header. That fails under Cygwin because the header is locked, but
     * doing an fseek succeeds. We have to output the leading newline
     * explicitly, because the one in the file is parted of the locked area.
     */

    (void)fwrite("\n", 1, 1, mbox_file);
    (void)fseek(data_file, SPOOL_DATA_START_OFFSET, SEEK_SET);

    do {
      j = fread(data_buffer, 1, sizeof(data_buffer), data_file);

      if (j > 0) {
        i = fwrite(data_buffer, 1, j, mbox_file);
        if (i != j) {
          debug_printf("error/short write on writing in: %s", mbox_path);
          (void)fclose(mbox_file);
          (void)fclose(data_file);
          return NULL;
        };
      };
    } while (j > 0);

    (void)fclose(data_file);
    (void)fclose(mbox_file);
    Ustrcpy(spooled_message_id, message_id);
    spool_mbox_ok = 1;
  };

  snprintf(CS mbox_path, 1024, "%s/scan/%s/%s.eml", spool_directory, message_id, message_id);

  /* get the size of the mbox message */
  stat(CS mbox_path, &statbuf);
  *mbox_file_size = statbuf.st_size;

  /* open [message_id].eml file for reading */
  mbox_file = Ufopen(mbox_path,"rb");

  return mbox_file;
}

/* remove mbox spool file, demimed files and temp directory */
void unspool_mbox(void) {

  /* reset all exiscan state variables */
  #ifdef WITH_OLD_DEMIME
  demime_ok = 0;
  demime_errorlevel = 0;
  demime_reason = NULL;
  file_extensions = NULL;
  #endif

  spam_ok = 0;
  malware_ok = 0;

  if (spool_mbox_ok) {

    spool_mbox_ok = 0;

    if (!no_mbox_unspool) {
      uschar mbox_path[1024];
      uschar file_path[1024];
      int n;
      struct dirent *entry;
      DIR *tempdir;

      snprintf(CS mbox_path, 1024, "%s/scan/%s", spool_directory, spooled_message_id);

  tempdir = opendir(CS mbox_path);
  /* loop thru dir & delete entries */
  n = 0;
  do {
    entry = readdir(tempdir);
    if (entry == NULL) break;
    snprintf(CS file_path, 1024,"%s/scan/%s/%s", spool_directory, spooled_message_id, entry->d_name);
    if ( (Ustrcmp(entry->d_name,"..") != 0) && (Ustrcmp(entry->d_name,".") != 0) ) {
      debug_printf("unspool_mbox(): unlinking '%s'\n", file_path);
              n = unlink(CS file_path);
            };
  } while (n > -1);

  closedir(tempdir);

  /* remove directory */
  n = rmdir(CS mbox_path);
    };
  };
}

#endif
