/* minget.c */


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
   char *srcpath;
   char *dstpath = NULL;
   FILE *fp;
   FILE *out = NULL;
   struct inode ino;
   uint32_t inum = 0;


   parse_common_options(argc, argv, &opt, 1, &rest);


   if (!rest[0] || !rest[1]) {
       fprintf(stderr,
           "usage: minget [ -v ] [ -p num [ -s num ] ] imagefile "
           "srcpath [ dstpath ]\n");


       exit(EXIT_FAILURE);
   }


   imagefile = rest[0];
   srcpath   = rest[1];
   if (rest[2] != NULL) {
       dstpath = rest[2];
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


   if (fs_find_path(&fs, srcpath, &ino, &inum) < 0) {
       fclose(fp);
       exit(EXIT_FAILURE);
   }


   /* If verbose, show the inode for the source file */
   if (opt.verbose) {
       fs_print_inode_verbose(&ino);
   }


   if (!fs_is_regular(&ino)) {
       fprintf(stderr, "%s is not a regular file.\n", srcpath);
       fclose(fp);
       exit(EXIT_FAILURE);
   }


   if (dstpath) {
       out = fopen(dstpath, "wb");
       if (!out) {
           perror("fopen dstpath");
           fclose(fp);
           exit(EXIT_FAILURE);
       }
   } else {
       out = stdout;
   }


   if (fs_copy_file_to_stream(&fs, &ino, out) < 0) {
       if (dstpath && out != NULL && out != stdout) {
           fclose(out);
       }
       fclose(fp);
       exit(EXIT_FAILURE);
   }


   if (dstpath && out != NULL && out != stdout) {
       fclose(out);
   }
   fclose(fp);
   return 0;
}




static int
copy_from_zone(const struct fs *fs,
              uint32_t zone,
              uint32_t *remaining,
              FILE *out)
{
   char buffer[4096]; /* handle up to 4K blocks; we’ll chunk if needed */
   uint32_t zone_bytes = fs->zonesize;
   uint32_t to_do;


   if (*remaining == 0)
       return 0;


   to_do = (*remaining < zone_bytes) ? *remaining : zone_bytes;


   if (zone == 0) {
       /* Hole: write zeros */
       memset(buffer, 0, sizeof(buffer));
       while (to_do > 0) {
           uint32_t chunk = (to_do < sizeof(buffer)) ? to_do : sizeof(buffer);
           if (fwrite(buffer, 1, chunk, out) != chunk) {
               perror("fwrite hole");
               return -1;
           }
           to_do      -= chunk;
           *remaining -= chunk;
       }
       return 0;
   }


   {
       long base = fs->fs_offset + (long)zone * fs->zonesize;
       uint32_t left = to_do;


       if (fseek(fs->fp, base, SEEK_SET) != 0) {
           perror("fseek data zone");
           return -1;
       }


       while (left > 0) {
           uint32_t chunk = (left < sizeof(buffer)) ? left : sizeof(buffer);
           if (fread(buffer, 1, chunk, fs->fp) != chunk) {
               perror("fread data zone");
               return -1;
           }
           if (fwrite(buffer, 1, chunk, out) != chunk) {
               perror("fwrite out");
               return -1;
           }
           left       -= chunk;
           *remaining -= chunk;
       }
   }


   return 0;
}


int
fs_copy_file_to_stream(const struct fs *fs,
                      const struct inode *ino,
                      FILE *out)
{
   uint32_t remaining = ino->size;
   uint32_t zones_per_ind = fs->zonesize / sizeof(uint32_t);
   uint32_t block_index = 0;
   uint32_t *ind1 = NULL;      /* single-indirect table (if present) */
   uint32_t *dbl1 = NULL;      /* first-level double-indirect table */
   uint32_t *dbl2 = NULL;      /* cached second-level table */
   uint32_t dbl2_index = (uint32_t)-1;
   int rc = -1;


   /* Load single-indirect table if present */
   if (ino->indirect != 0) {
       long off = fs->fs_offset + (long)ino->indirect * fs->zonesize;
       ind1 = malloc(fs->zonesize);
       if (!ind1) {
           fprintf(stderr, "malloc indirect\n");
           goto done;
       }
       if (fseek(fs->fp, off, SEEK_SET) != 0) {
           perror("fseek indirect");
           goto done;
       }
       if (fread(ind1, 1, fs->zonesize, fs->fp) != fs->zonesize) {
           perror("fread indirect");
           goto done;
       }
   }


   /* Load first-level double-indirect table if present */
   if (ino->two_indirect != 0) {
       long off = fs->fs_offset + (long)ino->two_indirect * fs->zonesize;
       dbl1 = malloc(fs->zonesize);
       if (!dbl1) {
           fprintf(stderr, "malloc two_indirect\n");
           goto done;
       }
       if (fseek(fs->fp, off, SEEK_SET) != 0) {
           perror("fseek two_indirect");
           goto done;
       }
       if (fread(dbl1, 1, fs->zonesize, fs->fp) != fs->zonesize) {
           perror("fread two_indirect");
           goto done;
       }
   }


   /* Walk the file block-by-block until we've produced all bytes. */
   while (remaining > 0) {
       uint32_t zone = 0;


       if (block_index < DIRECT_ZONES) {
           /* Direct zones */
           zone = ino->zone[block_index];
       } else if (block_index < DIRECT_ZONES + zones_per_ind) {
           /* Single-indirect region */
           uint32_t idx = block_index - DIRECT_ZONES;


           if (ind1) {
               zone = ind1[idx];
           } else {
               /* No indirect block allocated: this whole region is a hole. */
               zone = 0;
           }
       } else {
           /* Double-indirect region */
           uint32_t idx2 = block_index - DIRECT_ZONES - zones_per_ind;
           uint32_t l1 = idx2 / zones_per_ind;  /* index into dbl1 */
           uint32_t l2 = idx2 % zones_per_ind; 


           /* Beyond what a double-indirect can address: stop. */
           if (l1 >= zones_per_ind) {
               break;
           }


           if (!dbl1) {
              
               zone = 0;
           } else {
               uint32_t l2_zone = dbl1[l1];


               if (l2_zone == 0) {
                   /* This entire second-level block is a hole. */
                   zone = 0;
               } else {
                  
                   if (!dbl2 || dbl2_index != l1) {
                       long off = fs->fs_offset + (long)l2_zone * fs->zonesize;


                       if (!dbl2) {
                           dbl2 = malloc(fs->zonesize);
                           if (!dbl2) {
                               fprintf(stderr, "malloc dbl2\n");
                               goto done;
                           }
                       }
                       if (fseek(fs->fp, off, SEEK_SET) != 0) {
                           perror("fseek dbl2");
                           goto done;
                       }
                       if (fread(dbl2, 1, fs->zonesize, fs->fp)
                           != fs->zonesize) {
                           perror("fread dbl2");
                           goto done;
                       }
                       dbl2_index = l1;
                   }


                   zone = dbl2[l2];
               }
           }
       }


       /* zone == 0 means "hole": copy_from_zone will write zeros. */
       if (copy_from_zone(fs, zone, &remaining, out) < 0) {
           goto done;
       }


       block_index++;
   }


   if (remaining != 0) {
       /* File bigger than we can address even with double-indirect. */
       fprintf(stderr,
               "Warning: file has %u more bytes than handled "
               "(indirects not fully implemented)\n",
               remaining);
       goto done;
   }


   rc = 0;


done:
   if (ind1) free(ind1);
   if (dbl1) free(dbl1);
   if (dbl2) free(dbl2);
   return rc;
}
