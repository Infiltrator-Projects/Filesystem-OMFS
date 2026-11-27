/* SPDX-License-Identifier: GPL-3.0-or-later
 * Test-only writer using the pinned original omfsprogs library. Never
 * linked into the provider or invoked by any runtime consumer.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "omfs.h"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"oracle:%d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(int argc,char **argv)
{
    CHECK(argc==3);
    omfs_super_t super;omfs_root_t root;
    omfs_info_t info={.super=&super,.root=&root,.dev_mutex=PTHREAD_MUTEX_INITIALIZER};
    info.dev=fopen(argv[1],"r+b");CHECK(info.dev);
    FILE *payload=fopen(argv[2],"rb");CHECK(payload);
    CHECK(!omfs_read_super(&info)&&!omfs_read_root_block(&info)&&!omfs_load_bitmap(&info));
    unsigned block=swap_be32(super.s_blocksize);CHECK(block==4096);
    CHECK(swap_be32(super.s_mirrors)==2);
    u64 root_id=swap_be64(root.r_root_dir);CHECK(root_id==3);
    unsigned taken[]={6,7,10,11,20};
    for(unsigned i=0;i<sizeof(taken)/sizeof(taken[0]);++i)CHECK(omfs_allocate_one_block(&info,taken[i])==1);
    omfs_inode_t *file=omfs_new_inode(&info,6,"proof.bin",OMFS_FILE);
    omfs_inode_t *dir=omfs_get_inode(&info,root_id);CHECK(file&&dir);
    CHECK(!fseeko(payload,0,SEEK_END));u64 size=ftello(payload);rewind(payload);CHECK(size==2*block+37);
    file->i_parent=swap_be64(root_id);file->i_size=swap_be64(size);
    struct omfs_extent *table=(void *)((u8 *)file+OMFS_EXTENT_START);
    table->e_extent_count=swap_be32(3);table->e_next=~0ULL;
    /* Use actual on-disk entry records, not an array past the historical
     * upstream struct's single-element declaration. */
    struct omfs_extent_entry entries[3]={
        {swap_be64(10),swap_be64(2)},{swap_be64(20),swap_be64(1)},{~0ULL,swap_be64(~(u64)3)}
    };
    memcpy((u8 *)table+16,entries,sizeof entries);
    u8 *buf=calloc(1,block);CHECK(buf);
    for(unsigned i=0;i<3;++i){
        memset(buf,0,block);size_t n=fread(buf,1,block,payload);CHECK(n==(i<2?block:37));
        CHECK(!omfs_write_block(&info,i<2?10+i:20,buf));
    }
    free(buf);fclose(payload);
    int bucket=omfs_compute_hash(&info,"proof.bin");CHECK(bucket>=0);
    u64 pointer=swap_be64(6);memcpy((u8 *)dir+OMFS_DIR_START+8*bucket,&pointer,8);
    CHECK(!omfs_write_inode(&info,file)&&!omfs_write_inode(&info,dir));
    omfs_release_inode(file);omfs_release_inode(dir);omfs_sync(&info);
    CHECK(!fclose(info.dev));free(info.bitmap->bmap);free(info.bitmap->dirty);free(info.bitmap);
    return 0;
}
