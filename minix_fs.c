/* 
 * minix_fs.c – shared code for minls and minget.
 * Provides MINIX filesystem parsing: partitions, superblock, inodes,
 * directory traversal, and file reading (including indirect blocks).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "minix_fs.h"

/* For getopt */
extern int getopt(int argc, char * const argv[], const char *optstring);
extern int opterr;
extern int optind;
extern char *optarg;

static void
usage_minls(void)
{
   fprintf(stderr,
       "usage: minls [ -v ] [ -p num [ -s num ] ] imagefile [ path ]\n\n"
       "Options:\n"
       "-p part --- select partition for filesystem (default: none)\n"
       "-s sub --- select subpartition for filesystem (default: none)\n"
       "-h help --- print usage information and exit\n"
       "-v verbose --- increase verbosity level\n");
}


static void
usage_minget(void)
{
   fprintf(stderr,
       "usage: minget [ -v ] [ -p num [ -s num ] ] imagefile "
       "srcpath [ dstpath ]\n\n"


       "Options:\n"
       "-p part --- select partition for filesystem (default: none)\n"
       "-s sub --- select subpartition for filesystem (default: none)\n"
       "-h help --- print usage information and exit\n"
       "-v verbose --- increase verbosity level\n");
}


/*
 * parse_common_options:
 *   Parse shared command-line options for minls and minget.
 *   Fills in 'opt' with verbose/partition/subpartition flags and
 *   leaves *rest pointing at the remaining argv (imagefile + paths).
 *   'need_path_args' is not enforced here; each main() does its own
 *   argument validation after this call.
 */
int
parse_common_options(int argc, char **argv,
                    struct options *opt,
                    int need_path_args,
                    char ***rest)
{
   int c;
   int is_minls = (strstr(argv[0], "minls") != NULL);


   opt->verbose = 0;
   opt->have_partition = 0;
   opt->have_subpartition = 0;
   opt->part = 0;
   opt->subpart = 0;


   opterr = 0; 


   while ((c = getopt(argc, argv, "vp:s:h")) != -1) {
       switch (c) {
       case 'v':
           opt->verbose = 1;
           break;
       case 'p':
           opt->have_partition = 1;
           opt->part = atoi(optarg);
           break;
       case 's':
           opt->have_subpartition = 1;
           opt->subpart = atoi(optarg);
           break;
       case 'h':
       default:
           if (is_minls) {
               usage_minls();
           } else {
               usage_minget();
           }
           exit(EXIT_FAILURE);
       }
   }


   *rest = &argv[optind];

   return 0;
}


/* ----- Low-level helpers for partition reading ----- */

/* 
 * read_boot_signature:
 *   Verify the 0x55AA boot sector signature at 'base' in the image.
 *   Returns 0 on success, -1 on error or invalid signature.
 */
static int
read_boot_signature(FILE *fp, long base)
{
   unsigned char sig[2];


   if (fseek(fp, base + BOOT_SIG_OFFSET_1, SEEK_SET) != 0) {
       perror("fseek boot signature");
       return -1;
   }
   if (fread(sig, 1, 2, fp) != 2) {
       perror("fread boot signature");
       return -1;
   }
   if (sig[0] != BOOT_SIG_BYTE_1 || sig[1] != BOOT_SIG_BYTE_2) {
       fprintf(stderr, "Bad boot sector signature.\n");
       return -1;
   }
   return 0;
}

/*
 * read_partition_entry:
 *   Read partition table entry 'index' (0–3) from table at 'base'.
 *   Returns 0 on success, -1 on error or invalid index.
 */
static int
read_partition_entry(FILE *fp, long base, int index,
                    struct partition_entry *p)
{
       long off = base + PART_TABLE_OFFSET
           + index * sizeof(struct partition_entry);




   if (index < 0 || index > 3) {
       fprintf(stderr, "Invalid partition index %d\n", index);
       return -1;
   }
   if (fseek(fp, off, SEEK_SET) != 0) {
       perror("fseek partition table");
       return -1;
   }
   if (fread(p, sizeof(*p), 1, fp) != 1) {
       perror("fread partition entry");
       return -1;
   }
   return 0;
}


/* ----- Superblock + inode helpers ----- */

/*
 * fs_read_super:
 *   Read and validate the MINIX superblock for the current filesystem.
 *   The superblock is always at byte offset 1024 from fs->fs_offset.
 *   On success, initializes fs->sb, fs->blocksize, and fs->zonesize.
 */
int
fs_read_super(struct fs *fs, int verbose)
{
   /* Superblock is at block 1 (offset 1024 bytes from filesystem start) */
   if (fseek(fs->fp, fs->fs_offset + 1024, SEEK_SET) != 0) {
       perror("fseek superblock");
       return -1;
   }
   if (fread(&fs->sb, sizeof(fs->sb), 1, fs->fp) != 1) {
       perror("fread superblock");
       return -1;
   }
   if (fs->sb.magic != MINIX_MAGIC) {
       fprintf(stderr,
               "Bad magic number. (0x%04x)\n"
               "This does not look like a MINIX filesystem.\n",


               (unsigned)fs->sb.magic);
       return -1;
   }


   fs->blocksize = fs->sb.blocksize;
   fs->zonesize  = fs->blocksize << fs->sb.log_zone_size;


   if (verbose) {
       fprintf(stderr, "Superblock information:\n");
       fprintf(stderr, "  ninodes       = %u\n",   fs->sb.ninodes);
       fprintf(stderr, "  i_blocks      = %d\n",   fs->sb.i_blocks);
       fprintf(stderr, "  z_blocks      = %d\n",   fs->sb.z_blocks);
       fprintf(stderr, "  firstdata     = %u\n",   fs->sb.firstdata);
       fprintf(stderr, "  log_zone_size = %d\n",   fs->sb.log_zone_size);
       fprintf(stderr, "  max_file      = %u\n",   fs->sb.max_file);
       fprintf(stderr, "  zones         = %u\n",   fs->sb.zones);
       fprintf(stderr, "  magic         = 0x%04x\n", (unsigned)fs->sb.magic);
       fprintf(stderr, "  blocksize     = %u\n",   fs->sb.blocksize);
       fprintf(stderr, "  subversion    = %u\n",   fs->sb.subversion);
   }


   return 0;
}

/*
 * fs_init:
 *   Initialize an fs context for a given image file and options.
 *   Handles unpartitioned images, primary partitions (-p), and
 *   subpartitions (-s) before reading the MINIX superblock.
 */
int
fs_init(struct fs *fs, FILE *fp, const struct options *opt, int verbose)
{
    struct partition_entry p;
    long base = 0;  /* start of "current" partition in bytes */

    memset(fs, 0, sizeof(*fs));
    fs->fp = fp;
    fs->fs_offset = 0;

    /* Unpartitioned image: just read superblock directly. */
    if (!opt->have_partition && !opt->have_subpartition) {
        if (fs_read_super(fs, verbose) < 0)
            return -1;
        return 0;
    }

    /* Step 1: read primary partition table from MBR */
    if (read_boot_signature(fp, 0) < 0)
        return -1;

    if (opt->have_partition) {
        if (read_partition_entry(fp, 0, opt->part, &p) < 0)
            return -1;

        if (p.type != MINIX_PARTTYPE) {
            fprintf(stderr,
                "Partition %d is not a MINIX partition "
                "(type 0x%02x)\n",
                opt->part, p.type);
            return -1;
        }

        base = (long)p.lFirst * SECTOR_SIZE;
        if (verbose) {
            fprintf(stderr,
                "Partition %d: lFirst=%u size=%u  -> base=%ld\n",
                opt->part, p.lFirst, p.size, base);
        }
    }

    /* Step 2: if subpartition requested, read its table inside primary */
    if (opt->have_subpartition) {
        struct partition_entry sub;

        if (read_boot_signature(fp, base) < 0)
            return -1;

        if (read_partition_entry(fp, base, opt->subpart, &sub) < 0)
            return -1;

        if (sub.type != MINIX_PARTTYPE) {
            fprintf(stderr,
                "Subpartition %d is not a MINIX partition "
                "(type 0x%02x)\n",
                opt->subpart, sub.type);
            return -1;
        }

        if (verbose) {
            fprintf(stderr, "  Subpartition %d: lFirst=%u size=%u\n",
                    opt->subpart, sub.lFirst, sub.size);
        }

        /* IMPORTANT: lFirst is absolute, from start of disk. */
        base = (long)sub.lFirst * SECTOR_SIZE;
    }

    fs->fs_offset = base;

    if (fs_read_super(fs, verbose) < 0)
        return -1;

    return 0;
}



/* ----- Inode access ----- */

/*
 * fs_get_inode:
 *   Load inode 'inum' from the inode table into *ino.
 *   Computes the inode table location from the superblock fields.
 */
int
fs_get_inode(const struct fs *fs, uint32_t inum, struct inode *ino)
{
   uint32_t inode_table_block;
   uint32_t idx;
   long     off;


   if (inum == 0 || inum > fs->sb.ninodes) {
       fprintf(stderr, "Invalid inode number %u\n", inum);
       return -1;
   }


   /* Inode table starts after:
    *   block 0: boot
    *   block 1: superblock
    *   then i_blocks of inode bitmap,
    *   then z_blocks of zone bitmap.
    */
   inode_table_block = 2 + fs->sb.i_blocks + fs->sb.z_blocks;
   idx = inum - 1;


   off = fs->fs_offset
       + (long)inode_table_block * fs->blocksize
       + (long)idx * sizeof(struct inode);


   if (fseek(fs->fp, off, SEEK_SET) != 0) {
       perror("fseek inode");
       return -1;
   }
   if (fread(ino, sizeof(*ino), 1, fs->fp) != 1) {
       perror("fread inode");
       return -1;
   }
   return 0;
}


/* ----- Type + permission helpers ----- */


int
fs_is_dir(const struct inode *ino)
{
   return (ino->mode & I_TYPE_MASK) == I_DIRECTORY;
}


int
fs_is_regular(const struct inode *ino)
{
   return (ino->mode & I_TYPE_MASK) == I_REGULAR;
}


void
fs_perm_string(const struct inode *ino, char *out)
{
   uint16_t mode = ino->mode;


   out[0] = fs_is_dir(ino) ? 'd' : '-';


   out[1] = (mode & 0400) ? 'r' : '-';
   out[2] = (mode & 0200) ? 'w' : '-';
   out[3] = (mode & 0100) ? 'x' : '-';


   out[4] = (mode & 0040) ? 'r' : '-';
   out[5] = (mode & 0020) ? 'w' : '-';
   out[6] = (mode & 0010) ? 'x' : '-';


   out[7] = (mode & 0004) ? 'r' : '-';
   out[8] = (mode & 0002) ? 'w' : '-';
   out[9] = (mode & 0001) ? 'x' : '-';


   out[10] = '\0';
}

/*
 * scan_dir_zone:
 *   Scan up to 'to_read' bytes of directory entries starting at 'base'.
 *   If list_mode == 0, look for 'name' and return:
 *      1 on found (out_inum set),
 *      0 on not found,
 *     -1 on error.
 *   If list_mode == 1, ignore 'name' and print each non-empty entry.
 *   'remaining' is decremented by DIR_ENTRY_SIZE for each entry visited.
 */
void
fs_print_inode_verbose(const struct inode *ino)
{
   fprintf(stderr,
           "mode=0%o size=%u links=%u uid=%u gid=%u\n",
           ino->mode, ino->size, ino->links, ino->uid, ino->gid);
}


static int
scan_dir_zone(const struct fs *fs,
             long base,
             uint32_t to_read,
             const char *name,    /* may be NULL for fs_list_dir */
             uint32_t *remaining,
             uint32_t *out_inum,  /* ignored if name == NULL */
             int list_mode)       /* 0 = lookup, 1 = list */
{
   uint32_t offset = 0;


   while (offset + DIR_ENTRY_SIZE <= to_read) {
       struct dirent de;
       long off = base + offset;


       if (fseek(fs->fp, off, SEEK_SET) != 0) {
           perror("fseek dirent");
           return -1;
       }
       if (fread(&de, sizeof(de), 1, fs->fp) != 1) {
           perror("fread dirent");
           return -1;
       }


       if (de.inode != 0) {
           char dname[61];
           memcpy(dname, de.name, 60);
           dname[60] = '\0';


           if (!list_mode) {
               /* lookup mode */
               if (strcmp(dname, name) == 0) {
                   if (out_inum) {
                       *out_inum = de.inode;
                   }
                   return 1;  /* found */
               }
           } else {
               /* list mode: print entry */
               struct inode child;
               char perm[11];


               if (fs_get_inode(fs, de.inode, &child) < 0) {
                   return -1;
               }
               fs_perm_string(&child, perm);
               printf("%s %9u %s\n",
                      perm,
                      (unsigned)child.size,
                      dname);
           }
       }


       offset += DIR_ENTRY_SIZE;
       if (*remaining <= DIR_ENTRY_SIZE) {
           *remaining = 0;
           break;
       }
       *remaining -= DIR_ENTRY_SIZE;
   }


   return 0;  /* not found / done with this zone */
}

/*
 * dir_lookup:
 *   Search the directory inode 'dir_ino' for entry 'name'.
 *   Returns:
 *     1 if found (*out_inum set),
 *     0 if not found,
 *    -1 on error.
 *   Scans all direct zones, then any single-indirect zones.
 */
static int
dir_lookup(const struct fs *fs,
          const struct inode *dir_ino,
          const char *name,
          uint32_t *out_inum)
{
   uint32_t remaining = dir_ino->size;
   int i;


   if (!fs_is_dir(dir_ino)) {
       fprintf(stderr, "dir_lookup on non-directory\n");
       return -1;
   }


   /* 1) Direct zones */
   for (i = 0; i < DIRECT_ZONES && remaining > 0; i++) {
       uint32_t z = dir_ino->zone[i];
       long base;
       uint32_t to_read;


       if (z == 0) {
           uint32_t skip = fs->zonesize;
           if (remaining <= skip) break;
           remaining -= skip;
           continue;
       }


       base = fs->fs_offset + (long)z * fs->zonesize;
       to_read = (remaining < fs->zonesize) ? remaining : fs->zonesize;


       {
           int rc = scan_dir_zone(fs, base, to_read,
                                  name, &remaining, out_inum, 0);
           if (rc != 0) {
               return rc;   /* 1 = found, -1 = error */
           }
       }
   }


   /* 2) Single-indirect zones (directory grows beyond DIRECT_ZONES) */
   if (remaining > 0 && dir_ino->indirect != 0) {
       uint32_t nentries = fs->zonesize / sizeof(uint32_t);
       uint32_t *ind = malloc(fs->zonesize);


       if (!ind) {
           fprintf(stderr, "malloc indirect\n");
           return -1;
       }


       {
           long ind_off = fs->fs_offset
               + (long)dir_ino->indirect * fs->zonesize;


           if (fseek(fs->fp, ind_off, SEEK_SET) != 0) {
               perror("fseek dir indirect");
               free(ind);
               return -1;
           }
           if (fread(ind, 1, fs->zonesize, fs->fp) != fs->zonesize) {
               perror("fread dir indirect");
               free(ind);
               return -1;
           }
       }


       for (i = 0; i < (int)nentries && remaining > 0; i++) {
           uint32_t z = ind[i];
           long base;
           uint32_t to_read;


           if (z == 0) {
               uint32_t skip = fs->zonesize;
               if (remaining <= skip) break;
               remaining -= skip;
               continue;
           }


           base = fs->fs_offset + (long)z * fs->zonesize;
           to_read = (remaining < fs->zonesize)
                   ? remaining : fs->zonesize;


           {
               int rc = scan_dir_zone(fs, base, to_read,
                                      name, &remaining, out_inum, 0);
               if (rc != 0) {
                   free(ind);
                   return rc;
               }
           }
       }


       free(ind);
   }


   *out_inum = 0;
   return 0;  /* not found */
}




/* canonicalize_path: treat NULL/"" as "/", strip extra slashes, etc. */
void
canonicalize_path(const char *in, char *out, size_t outsz)
{
   char tmp[1024];
   size_t j = 0;
   int last_was_slash = 0;
   size_t i;


   if (!in || !*in) {
       strncpy(out, "/", outsz);
       out[outsz - 1] = '\0';
       return;
   }


   /* Ensure leading '/', collapse duplicate '/' */
   if (in[0] != '/') {
       tmp[j++] = '/';
       last_was_slash = 1;
   }


   for (i = 0; in[i] != '\0' && j + 1 < sizeof(tmp); i++) {
       if (in[i] == '/') {
           if (!last_was_slash) {
               tmp[j++] = '/';
               last_was_slash = 1;
           }
       } else {
           tmp[j++] = in[i];
           last_was_slash = 0;
       }
   }


   /* Remove trailing slash unless root */
   if (j > 1 && tmp[j - 1] == '/') {
       j--;
   }
   tmp[j] = '\0';


   strncpy(out, tmp, outsz);
   out[outsz - 1] = '\0';
}


/*
 * fs_find_path:
 *   Resolve 'path' starting from the root inode (1).
 *   Uses canonicalize_path + dir_lookup to walk each component.
 *   On success, fills *ino and *inum with the final inode.
 */
int
fs_find_path(const struct fs *fs, const char *path,
            struct inode *ino, uint32_t *inum)
{
   char canon[1024];
   char *token;
   struct inode cur;
   uint32_t cur_inum = 1;  /* root */


   canonicalize_path(path, canon, sizeof(canon));


   if (fs_get_inode(fs, cur_inum, &cur) < 0)
       return -1;


   /* root itself */
   if (strcmp(canon, "/") == 0) {
       *inum = cur_inum;
       *ino  = cur;
       return 0;
   }


   /* Walk each path component */
   token = strtok(canon + 1, "/"); /* skip leading '/' */
   while (token != NULL) {
       uint32_t child_inum = 0;
       int rc;


       if (!fs_is_dir(&cur)) {
           fprintf(stderr, "Not a directory while traversing path.\n");
           return -1;
       }


       rc = dir_lookup(fs, &cur, token, &child_inum);
       if (rc < 0)
           return -1;
       if (rc == 0 || child_inum == 0) {
           fprintf(stderr, "File not found.\n");
           return -1;
       }


       if (fs_get_inode(fs, child_inum, &cur) < 0)
           return -1;


       cur_inum = child_inum;
       token = strtok(NULL, "/");
   }


   *inum = cur_inum;
   *ino  = cur;
   return 0;
}

/*
 * fs_list_dir:
 *   List the contents of directory inode 'dir_ino' in long format.
 *   Prints one line per entry: "perm size name".
 */
int
fs_list_dir(const struct fs *fs, const char *path,
           const struct inode *dir_ino)
{
   uint32_t remaining = dir_ino->size;
   int i;


   (void)path;


   if (!fs_is_dir(dir_ino)) {
       fprintf(stderr, "fs_list_dir called on non-directory\n");
       return -1;
   }


   /* 1) Direct zones */
   for (i = 0; i < DIRECT_ZONES && remaining > 0; i++) {
       uint32_t z = dir_ino->zone[i];
       long base;
       uint32_t to_read;


       if (z == 0) {
           uint32_t skip = fs->zonesize;
           if (remaining <= skip) break;
           remaining -= skip;
           continue;
       }


       base = fs->fs_offset + (long)z * fs->zonesize;
       to_read = (remaining < fs->zonesize) ? remaining : fs->zonesize;


       if (scan_dir_zone(fs, base, to_read,
                         NULL, &remaining, NULL, 1) < 0) {
           return -1;
       }
   }


   /* 2) Single-indirect zones */
   if (remaining > 0 && dir_ino->indirect != 0) {
       uint32_t nentries = fs->zonesize / sizeof(uint32_t);
       uint32_t *ind = malloc(fs->zonesize);


       if (!ind) {
           fprintf(stderr, "malloc indirect\n");
           return -1;
       }


       {
           long ind_off = fs->fs_offset
               + (long)dir_ino->indirect * fs->zonesize;


           if (fseek(fs->fp, ind_off, SEEK_SET) != 0) {
               perror("fseek dir indirect");
               free(ind);
               return -1;
           }
           if (fread(ind, 1, fs->zonesize, fs->fp) != fs->zonesize) {
               perror("fread dir indirect");
               free(ind);
               return -1;
           }
       }


       for (i = 0; i < (int)nentries && remaining > 0; i++) {
           uint32_t z = ind[i];
           long base;
           uint32_t to_read;


           if (z == 0) {
               uint32_t skip = fs->zonesize;
               if (remaining <= skip) break;
               remaining -= skip;
               continue;
           }


           base = fs->fs_offset + (long)z * fs->zonesize;
           to_read = (remaining < fs->zonesize)
                   ? remaining : fs->zonesize;


           if (scan_dir_zone(fs, base, to_read,
                             NULL, &remaining, NULL, 1) < 0) {
               free(ind);
               return -1;
           }
       }


       free(ind);
   }


   return 0;
}
