// vim: noet:ts=4:sts=4:sw=4:et
#define FUSE_USE_VERSION 26
#define _GNU_SOURCE

#include <assert.h>
#include <endian.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <iconv.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <inttypes.h>

#include "vfat.h"
#include "util.h"
#include "debugfs.h"

#define DEBUG_PRINT(...) printf(__VA_ARGS)

iconv_t iconv_utf16;
char* DEBUGFS_PATH = "/.debug";

long bigEndian_4(unsigned char* number){
    return (number[0] + number[1] * 256 + number[2] * 256 * 256 + number[3] * 256 * 256 * 256); // no next cluster
}

static void vfat_init(const char *dev)
{
    //This implimentation is started by me (Orcun) as fat 32 
    //(Only 32 if you want to change it to support other you can do it :) )
    //From pdf: In this assignment, you will develop a read-only file system driver for the FAT32 file system
    
    struct fat_boot_header s; 

    iconv_utf16 = iconv_open("utf-8", "utf-16"); // from utf-16 to utf-8
    // These are useful so that we can setup correct permissions in the mounted directories
    vfat_info.mount_uid = getuid();
    vfat_info.mount_gid = getgid();

    // Use mount time as mtime and ctime for the filesystem root entry (e.g. "/")
    vfat_info.mount_time = time(NULL);

    vfat_info.fd = open(dev, O_RDONLY);
    if (vfat_info.fd < 0)
        err(1, "open(%s)", dev);
    if (pread(vfat_info.fd, &s, sizeof(s), 0) != sizeof(s))
        err(1, "read super block");
        
    /* XXX BEGIN */
    
    
    //First part of assigment  
    //Exact information from BootSector
    //Information which will exact from First Sector
    vfat_info.bytes_per_cluster = s.bytes_per_sector * s.sectors_per_cluster;
    vfat_info.bytes_per_sector = s.bytes_per_sector;
    vfat_info.sectors_per_cluster = s.sectors_per_cluster;
    vfat_info.reserved_sectors = s.reserved_sectors;
    vfat_info.sectors_per_fat = s.sectors_per_fat;
    vfat_info.fat_entries = s.sectors_per_fat / s.sectors_per_cluster ;
    vfat_info.fat_size = (s.sectors_per_fat_small != 0) ? s.sectors_per_fat_small : s.sectors_per_fat;
    vfat_info.total_sector = (s.total_sectors_small != 0) ? s.total_sectors_small : s.total_sectors;
    vfat_info.count_of_cluster = (vfat_info.total_sector - (vfat_info.reserved_sectors + ( vfat_info.fat_size  * s.fat_count))) / vfat_info.sectors_per_cluster ;
    vfat_info.cluster_begin_offset = (vfat_info.reserved_sectors + 2 * vfat_info.fat_size) * vfat_info.bytes_per_sector; // Dont forget to change 2 constant number
    
    //Determining Fat Type !IMPORTANT This implimentation is only for Fat 32
    if(vfat_info.count_of_cluster < 4085) {
        //This is fat 12
        //printf("\nThis is Fat 12");    fflush(stdout);
        return;
    } else if(vfat_info.count_of_cluster < 65525) {
        //This is fat 16
        //printf("\nThis is Fat 16");    fflush(stdout);
        return;
    } else if(!((s.fat_flags >> 7) & 1)){     
        //This is fat 32
        //printf("\nThis is Fat 32");
        vfat_info.active_fat = (s.fat_flags & 1);
    }
    
    vfat_info.fat_begin_offset =  (vfat_info.reserved_sectors + vfat_info.active_fat * vfat_info.sectors_per_fat) * vfat_info.bytes_per_sector ;   //Not sure about fat_begin_offset
    //Mapping
    vfat_info.fat = mmap_file(vfat_info.fd,vfat_info.fat_begin_offset, vfat_info.fat_size);
    
    /*
    printf("\nBytes per sector: %d",vfat_info.bytes_per_sector);
    printf("\nSector by cluster: %d",vfat_info.sectors_per_cluster);
    printf("\nReserved sectors: %d",vfat_info.reserved_sectors);
    printf("\nSectors_per_fat: %d",vfat_info.sectors_per_fat);
    fflush(stdout);
    */
  
    //Trying to predict it
    //End of predict
    
    //Test for cluster chains
    /*
    unsigned char clusters[512];
    int j = 0;
    lseek(vfat_info.fd,vfat_info.fat_begin_offset , SEEK_SET);
    read(vfat_info.fd, clusters, 512 );
    for( i= 0; i < 512 ; i = i + 4){
        
        j = bigEndian_4(clusters + i);
    
            printf("\n %d: %d ",i/4, j);                
            print_hex(clusters + i);
            
    }
    fflush(stdout);
    */
    
    /* XXX END */
    
    vfat_info.root_inode.st_ino = le32toh(s.root_cluster);
    vfat_info.root_inode.st_mode = 0555 | S_IFDIR;
    vfat_info.root_inode.st_nlink = 1;
    vfat_info.root_inode.st_uid = vfat_info.mount_uid;
    vfat_info.root_inode.st_gid = vfat_info.mount_gid;
    vfat_info.root_inode.st_size = 0;
    vfat_info.root_inode.st_atime = vfat_info.root_inode.st_mtime = vfat_info.root_inode.st_ctime = vfat_info.mount_time;

}

/* XXX add your code here */

int vfat_next_cluster(uint32_t c)
{ 
    //Can control codes for false places be added but in order to icrease accessing time i did not add them -orcungumus
    //Instead control can be done other places.
    return vfat_info.fat[c];
}    

int vfat_readdir(uint32_t first_cluster, fuse_fill_dir_t callback, void *callbackdata)
{
    struct stat st;
    
    st.st_uid = vfat_info.mount_uid;
    st.st_gid = vfat_info.mount_gid;
    st.st_nlink = 1;
    
    struct fat32_direntry direntry;
        
    int count = 0; char direcory_end = 1; long current_cluster = first_cluster;
    
    while(current_cluster < 268435448 && direcory_end != 0){
        count = 0; // Number of directories in current cluster
        do{
            long place = vfat_info.cluster_begin_offset + (current_cluster - 2) * vfat_info.sectors_per_cluster * vfat_info.bytes_per_sector + 32 * count;
            
            if (pread(vfat_info.fd, &direntry, sizeof(direntry), place) != sizeof(direntry))
                err(1, "read direntry 2");
           
            else
            {
                //printf("\n%x",direntry.nameext[0]);
                if(((direntry.nameext[0] & 0xFF) != 0xE5) && direntry.attr != 0x0F){
                    st.st_mode = S_IRWXU | S_IRWXG | S_IRWXO;//Dont forget to change constant values here
                    
                    st.st_mode |= ((direntry.attr >> 4) & 1) ? S_IFDIR : S_IFREG;
                    
                    st.st_size = direntry.size;
                    fflush(stdout);
                         
                    struct tm info;
                    
                    //Problematic?
                    info.tm_year = (((direntry.atime_date >> 9) & 0x3F) + 80);
                    info.tm_mon =  (( direntry.atime_date >> 5 ) & 0xF ) - 1;
                    info.tm_mday = ( direntry.atime_date  & 0x1F );    
                    
                    st.st_atime = (direntry.atime_date > 0) ? mktime(&info) : 0;
                
                    
                    info.tm_year = (((direntry.mtime_date >> 9) & 0x3F) + 80);
                    info.tm_mon =  (( direntry.mtime_date >> 5 ) & 0xF ) - 1;
                    info.tm_mday = ( direntry.mtime_date  & 0x1F );    
                    
                    info.tm_hour = ((direntry.mtime_time >> 11) & 0x1F) + 1;
                    info.tm_min =  (( direntry.mtime_time >> 5 ) & 0x3F );
                    info.tm_sec = 2 * ( direntry.mtime_time  & 0x1F ); 
                    
                    st.st_mtime = (direntry.mtime_date > 0) ? mktime(&info) : 0;
                
                    info.tm_year = (((direntry.ctime_date >> 9) & 0x3F) + 80);
                    info.tm_mon =  (( direntry.ctime_date >> 5 ) & 0xF ) - 1 ;
                    info.tm_mday = ( direntry.ctime_date  & 0x1F ); 
                    
                    info.tm_hour = ((direntry.ctime_time >> 11) & 0x1F) + 1;
                    info.tm_min =  (( direntry.ctime_time >> 5 ) & 0x3F ) ;
                    info.tm_sec = 2 * ( direntry.ctime_time  & 0x1F ) + 1;   
                     
                    st.st_ctime = (direntry.ctime_date > 0) ? mktime(&info) : 0;

                    st.st_dev = 0;
                    st.st_blocks = 2;
                    st.st_blksize = 4;
                    st.st_ino = direntry.cluster_hi * 256 * 256 + direntry.cluster_lo;
                    
                    //Remove Space Padding For end
                    char newname[13] = "ZZZZZZZZZZZ";
                    char ext_exist = 0; char padding_end = 0;
                   // printf("\n\n%s", direntry.nameext);fflush(stdout);
                    int index = 11; // Delete if exist in the end
                    while(index > 8){
                        if(direntry.nameext[(index-1)] != '\ '){
                            ext_exist = 1;                       
                            newname[index] = direntry.nameext[(index - 1)];
                           
                        }
                        else
                            newname[index] = 0;
                        index--;
                    }
                  
                    if(ext_exist == 1){
                        newname[index] = '.'; index--;
                    }
                    while(index > -1){
                         //printf("\n%s", newname);fflush(stdout);
                        if(direntry.nameext[index] == '\ ' && padding_end == 0){
                            newname[index] = newname[index+1];
                            newname[index+1] = newname[index+2];
                            newname[index+2] = newname[index+3];
                            newname[index+3] = newname[index+4];
                            newname[index+4] = 0;
                            
                        }
                        else{
                            padding_end = 1;
                            newname[index] = direntry.nameext[index];
                        }
                        index--;
                    }
                    
                    //You can summarize this code , i know this is kind of bad, but it is fine for now :)
                    
                    //printf("\n%s",direntry.nameext);
                    //printf("%s\n",name); fflush(stdout);
                    if(callback(callbackdata,newname, &st, 0)){
                    }
                }
            }count++;
                
            pread(vfat_info.fd, &direcory_end, sizeof(direcory_end), place);
            //printf("\n%dth directory", count);  fflush(stdout);
        } while(direcory_end != 0 && count <  (vfat_info.bytes_per_cluster / (32 * 4))  /* Check this -orcungumus */ );
        
      //  printf("%d",vfat_info.bytes_per_cluster);
       // printf("%d->", current_cluster); 
        current_cluster = vfat_next_cluster(current_cluster);
        //if(current_cluster >= 268435448)
        //    printf("%d\n\n", current_cluster); 
            
            
    }//End for cluster chain
        fflush(stdout);
    return 0;
}


// Used by vfat_search_entry()
struct vfat_search_data {
    char*  name;
    int          found;
    struct stat* st;
};


// You can use this in vfat_resolve as a callback function for vfat_readdir
// This way you can get the struct stat of the subdirectory/file.
int vfat_search_entry(void *data, const char *name, const struct stat *st, off_t offs)
{
    struct vfat_search_data *sd = data;
    //printf("\n%s-%s&",sd->name, name);
    if (strcmp(sd->name, name) != 0) return 0;
    //printf("\nSame");
    sd->found = 1;
    *sd->st = *st;
    return 1;
}

/**
 * Fills in stat info for a file/directory given the path
 * @path full path to a file, directories separated by slash
 * @st file stat structure
 * @returns 0 iff operation completed succesfully -errno on error
*/
int vfat_resolve(const char *path, struct stat *st)
{
    int res = -ENOENT; // Not Found 
    int debug = 0;
    long cluster = vfat_info.root_inode.st_ino; // dont forget to correct it
     
    if (strcmp("/", path) == 0) {
        *st = vfat_info.root_inode;
        res = 0;
    }
    else{
        struct vfat_search_data sd;
        struct stat st_test; sd.st = &st_test; 
       
        
        sd.name = strtok ((char *) path,"/");
        
        while(sd.name != NULL){
            //printf("\n0.Cluster:%d debug:%d Search For: %s",(int)cluster,debug,sd.name );   fflush(stdout); 
            sd.found = 0;
            vfat_readdir(cluster, vfat_search_entry , &sd);
            if(sd.found == 1){
                //printf("\n1.Cluster:%d",(int)cluster);   fflush(stdout); 
                cluster = sd.st->st_ino;
                //printf("\n2.Cluster:%d",(int)cluster);   fflush(stdout); 
                *st = *sd.st;
                sd.name = strtok (NULL, "/");
                res = 0; debug++;                           
            }
            else
                break;
        }   
    }return res;
}

// Get file attributes
int vfat_fuse_getattr(const char *path, struct stat *st)
{
    if (strncmp(path, DEBUGFS_PATH, strlen(DEBUGFS_PATH)) == 0) {
        // This is handled by debug virtual filesystem
        return debugfs_fuse_getattr(path + strlen(DEBUGFS_PATH), st);
    } else {
        // Normal file
        return vfat_resolve(path, st);
    }
}

// Extended attributes useful for debugging
int vfat_fuse_getxattr(const char *path, const char* name, char* buf, size_t size)
{
    struct stat st;
    int ret = vfat_resolve(path, &st);
    if (ret != 0) return ret;
    if (strcmp(name, "debug.cluster") != 0) return -ENODATA;

    if (buf == NULL) {
        ret = snprintf(NULL, 0, "%u", (unsigned int) st.st_ino);
        if (ret < 0) err(1, "WTF?");
        return ret + 1;
    } else {
        ret = snprintf(buf, size, "%u", (unsigned int) st.st_ino);
        if (ret >= size) return -ERANGE;
        return ret;
    }
}

int vfat_fuse_readdir(
        const char *path, void *callback_data,
        fuse_fill_dir_t callback, off_t unused_offs, struct fuse_file_info *unused_fi)
{
    if (strncmp(path, DEBUGFS_PATH, strlen(DEBUGFS_PATH)) == 0) {
        // This is handled by debug virtual filesystem
        return debugfs_fuse_readdir(path + strlen(DEBUGFS_PATH), callback_data, callback, unused_offs, unused_fi);
    }{
        struct stat st;
        vfat_resolve(path, &st);
        vfat_readdir(st.st_ino, callback, callback_data);
    }
    /* TODO: Add your code here. You should reuse vfat_readdir and vfat_resolve functions
    */
    return 0;
}

int vfat_fuse_read(const char *path, char *buf, size_t size, off_t offs,struct fuse_file_info *unused)
{
    if (strncmp(path, DEBUGFS_PATH, strlen(DEBUGFS_PATH)) == 0) {
        // This is handled by debug virtual filesystem
        return debugfs_fuse_read(path + strlen(DEBUGFS_PATH), buf, size, offs, unused);
    }
    
    //char tmpbuf[1024];
    /* TODO: Add your code here. Look at debugfs_fuse_read for example interaction.
    */
    
    struct stat st; int current = 0; int read_byte = vfat_info.bytes_per_cluster;
   
    vfat_resolve(path, &st);
 
    char tmpbuf[size];
    int len = (int) st.st_size - offs;

    if (len <= 0) return 0;
    
    if (len > size) {
      len = size;
    }
    
    //printf("\n\n\nWelcome to file reading. \nSize is: %d \n", (int)st.st_size );
    //printf("\nAllowed Size:%d Size:%d offs:%d len:%d&\n",(int) size ,(int)st.st_size, (int)offs,(int)len);
      
    int where_is = offs; int current_cluster = st.st_ino;

    while(where_is > vfat_info.bytes_per_cluster){
         //printf("%d->",current_cluster); fflush(stdout);
         current_cluster = vfat_next_cluster(current_cluster);
         where_is -= vfat_info.bytes_per_cluster;
    } current += where_is;
     
    
    long place =0;
    while(current < len){
  
        
        place = vfat_info.cluster_begin_offset + (current_cluster - 2) * vfat_info.sectors_per_cluster * vfat_info.bytes_per_sector;
        //printf("%d->",place);fflush(stdout);
        if (pread(vfat_info.fd, tmpbuf ,read_byte , place ) != read_byte)
            err(1, "read super block");
            

        memcpy(buf + current, tmpbuf, read_byte);
        
        if((len - current) > vfat_info.bytes_per_cluster){
            current += vfat_info.bytes_per_cluster;
            current_cluster = vfat_next_cluster(current_cluster);
            //printf("->%d", current_cluster); fflush(stdout);
        }
        else{
            current += (len - current);
            read_byte = (len - current);
            //printf("End");
        }
    }     
    return len;
}

////////////// No need to modify anything below this point
int vfat_opt_args(void *data, const char *arg, int key, struct fuse_args *oargs)
{
    if (key == FUSE_OPT_KEY_NONOPT && !vfat_info.dev) {
        vfat_info.dev = strdup(arg);
        return (0);
    }
    return (1);
}

struct fuse_operations vfat_available_ops = {
    .getattr = vfat_fuse_getattr,
    .getxattr = vfat_fuse_getxattr,
    .readdir = vfat_fuse_readdir,
    .read = vfat_fuse_read,
};

int main(int argc, char **argv)
{
    
    //printf("Finally Start On 09.05.2015");
    fflush(stdout);

    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

    fuse_opt_parse(&args, NULL, NULL, vfat_opt_args);

    if (!vfat_info.dev) 
        errx(1, "missing file system parameter"); // What is standart our error output

    vfat_init(vfat_info.dev); // If we commend this line; we see the helloworld.
    return (fuse_main(args.argc, args.argv, &vfat_available_ops, NULL));

}