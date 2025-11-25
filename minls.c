/* 
 * minls.c
 *   Implementation of the minls utility: list directories or file info
 *   from a MINIX filesystem image using the shared minix_fs.c helpers.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "minix_fs.h"

int
main(int argc, char **argv)
{
   struct options opt;
   struct fs fs;
   char **rest;
   char *imagefile;
   char *path = NULL;
   FILE *fp;
   struct inode ino;
   uint32_t inum = 0;
   char perm[11];
   char canon[1024];
   char *printpath;


   /* Parse options; rest will point to imagefile + optional path */
   parse_common_options(argc, argv, &opt, 0, &rest);


   if (rest[0] == NULL) {
       fprintf(stderr,
           "usage: minls [ -v ] [ -p num [ -s num ] ] imagefile [ path ]\n");
       exit(EXIT_FAILURE);
   }


   imagefile = rest[0];
   if (rest[1] != NULL) {
       path = rest[1];
   } 
   else {
       path = "/";  /* default */
   }


   fp = fopen(imagefile, "rb");
   if (!fp) {
       perror("fopen imagefile");
       exit(EXIT_FAILURE);
   }


   if (fs_init(&fs, fp, &opt, opt.verbose) < 0) {
       fclose(fp);
       exit(EXIT_FAILURE);
   }


   if (fs_find_path(&fs, path, &ino, &inum) < 0) {
       fclose(fp);
       exit(EXIT_FAILURE);
   }


    /* Canonicalize for printing */
    canonicalize_path(path, canon, sizeof(canon));

    /* Drop leading '/' for printing file names, except for root itself. */
    printpath = canon;
    if (strcmp(canon, "/") != 0 && canon[0] == '/') {
        printpath = canon + 1;
    }

    /* If verbose, print inode info to stderr */
    if (opt.verbose) {
        fs_print_inode_verbose(&ino);
    }

    if (fs_is_dir(&ino)) {
        /*
         * Directories: header should include the leading '/'.
         * e.g., "/Files:", "/DeepPaths/...:", "/Deleted:".
         */
        printf("%s:\n", canon);
        fs_list_dir(&fs, canon, &ino); 
    } 
    else {
        /* Single file listing: keep your original printpath behavior. */
        fs_perm_string(&ino, perm);
        printf("%s %9u %s\n",
               perm,
               (unsigned int)ino.size,
               printpath);
    }

    fclose(fp);
    return 0;
}





