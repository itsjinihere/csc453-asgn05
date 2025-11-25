/* minix_fs.h
*
* Shared definitions for minls and minget.
*/


#ifndef MINIX_FS_H
#define MINIX_FS_H


#include <stdint.h>
#include <stdio.h>
#include <stddef.h>


#define MINIX_PARTTYPE 0x81
#define MINIX_MAGIC    0x4D5A


#define DIRECT_ZONES   7
#define DIR_ENTRY_SIZE 64


#define PART_TABLE_OFFSET  0x1BE
#define SECTOR_SIZE        512


#define BOOT_SIG_OFFSET_1  510
#define BOOT_SIG_OFFSET_2  511
#define BOOT_SIG_BYTE_1    0x55
#define BOOT_SIG_BYTE_2    0xAA


/* Mode bits (same as Unix / assignment) */
#define I_TYPE_MASK  0170000
#define I_DIRECTORY  0040000
#define I_REGULAR    0100000




/* Partition table entry: from assignment handout */
struct __attribute__((packed)) partition_entry {
   uint8_t bootind;
   uint8_t start_head;
   uint8_t start_sec;
   uint8_t start_cyl;
   uint8_t type;
   uint8_t end_head;
   uint8_t end_sec;
   uint8_t end_cyl;
   uint32_t lFirst;   /* first sector (LBA) */
   uint32_t size;     /* number of sectors */
};


/* Minix superblock */
struct __attribute__((packed)) superblock {
   uint32_t ninodes;
   uint16_t pad1;
   int16_t  i_blocks;
   int16_t  z_blocks;
   uint16_t firstdata;
   int16_t  log_zone_size;
   int16_t  pad2;
   uint32_t max_file;
   uint32_t zones;
   int16_t  magic;
   int16_t  pad3;
   uint16_t blocksize;
   uint8_t  subversion;
};


/* Minix inode */
struct __attribute__((packed)) inode {
   uint16_t mode;
   uint16_t links;
   uint16_t uid;
   uint16_t gid;
   uint32_t size;
   int32_t  atime;
   int32_t  mtime;
   int32_t  ctime;
   uint32_t zone[DIRECT_ZONES];
   uint32_t indirect;
   uint32_t two_indirect;
   uint32_t unused;
};


/* Minix directory entry */
struct __attribute__((packed)) dirent {
   uint32_t inode;
   unsigned char name[60];   /* not always NUL-terminated if full */
};


/* Options parsed from command line */
struct options {
   int verbose;
   int have_partition;
   int have_subpartition;
   int part;
   int subpart;
};


/* Represents the filesystem context (computed once) */
struct fs {
   FILE *fp;
   long  fs_offset;      /* byte offset of start of filesystem within image */
   struct superblock sb;
   uint32_t blocksize;
   uint32_t zonesize;
};


/* Option parsing helpers */
int parse_common_options(int argc, char **argv,
                        struct options *opt,
                        int need_path_args,
                        char ***rest);


/* Filesystem helpers */
int   fs_init(struct fs *fs, FILE *fp, const struct options *opt, int verbose);
int   fs_read_super(struct fs *fs, int verbose);
int   fs_get_inode(const struct fs *fs, uint32_t inum, struct inode *ino);
int   fs_find_path(const struct fs *fs, const char *path, struct inode *ino,
                  uint32_t *inum);
int   fs_list_dir(const struct fs *fs, const char *path,
                 const struct inode *dir_ino);
int   fs_is_dir(const struct inode *ino);
int   fs_is_regular(const struct inode *ino);
void  fs_print_inode_verbose(const struct inode *ino);
void  fs_perm_string(const struct inode *ino, char *out);




void  canonicalize_path(const char *in, char *out, size_t outsz);




/* For minget */
int fs_copy_file_to_stream(const struct fs *fs,
                          const struct inode *ino,
                          FILE *out);


#endif /* MINIX_FS_H */
