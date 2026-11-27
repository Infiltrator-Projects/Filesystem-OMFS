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
    CHECK(argc>=4&&!(argc%2));
    unsigned files=(unsigned)(argc-2)/2;
    omfs_super_t super;omfs_root_t root;
    omfs_info_t info={.super=&super,.root=&root,.dev_mutex=PTHREAD_MUTEX_INITIALIZER};
    info.dev=fopen(argv[1],"r+b");CHECK(info.dev);
    CHECK(!omfs_read_super(&info)&&!omfs_read_root_block(&info)&&!omfs_load_bitmap(&info));
    unsigned block=swap_be32(super.s_blocksize);CHECK(block==4096);
    CHECK(swap_be32(super.s_mirrors)==2);
    u64 root_id=swap_be64(root.r_root_dir);CHECK(root_id==3);
    u64 blocks=swap_be64(super.s_num_blocks),cursor=8+2*files;
    CHECK(cursor<blocks);
    omfs_inode_t *dir=omfs_get_inode(&info,root_id);CHECK(dir);
    u8 *buf=calloc(1,block);CHECK(buf);
    for(unsigned f=0;f<files;++f){
        const char *name=argv[2+2*f];FILE *payload=fopen(argv[3+2*f],"rb");
        CHECK(payload&&*name&&strlen(name)<256&&!strchr(name,'/'));
        CHECK(!fseeko(payload,0,SEEK_END));off_t end=ftello(payload);CHECK(end>0);rewind(payload);
        u64 size=(u64)end,need=(size+block-1)/block,id=6+2*f;
        CHECK(need<blocks&&cursor<blocks&&need<=blocks-cursor);
        CHECK(omfs_allocate_one_block(&info,id)==1&&omfs_allocate_one_block(&info,id+1)==1);
        omfs_inode_t *file=omfs_new_inode(&info,id,(char *)name,OMFS_FILE);CHECK(file);
        file->i_parent=swap_be64(root_id);file->i_size=swap_be64(size);
        struct omfs_extent *table=(void *)((u8 *)file+OMFS_EXTENT_START);
        u64 first=(need+1)/2,second=need-first,other=cursor+first+8;
        CHECK(!second||(other<blocks&&second<=blocks-other));
        struct omfs_extent_entry entries[3]={{swap_be64(cursor),swap_be64(first)}};
        unsigned count=1;
        if(second)entries[count++]=(struct omfs_extent_entry){swap_be64(other),swap_be64(second)};
        entries[count++]=(struct omfs_extent_entry){~0ULL,swap_be64(~need)};
        table->e_extent_count=swap_be32(count);table->e_next=~0ULL;
        memcpy((u8 *)table+16,entries,count*sizeof(entries[0]));
        u64 remain=size;
        for(u64 i=0;i<need;++i){
            u64 at=i<first?cursor+i:other+i-first;
            CHECK(omfs_allocate_one_block(&info,at)==1);
            memset(buf,0,block);size_t n=remain<block?(size_t)remain:block;
            CHECK(fread(buf,1,n,payload)==n&&!omfs_write_block(&info,at,buf));remain-=n;
        }
        CHECK(!remain&&fgetc(payload)==EOF);fclose(payload);
        cursor=second?other+second+2:cursor+first+2;
        int bucket=omfs_compute_hash(&info,(char *)name);CHECK(bucket>=0);
        u8 *slot=(u8 *)dir+OMFS_DIR_START+8*bucket;
        memcpy(&file->i_sibling,slot,8);
        u64 pointer=swap_be64(id);memcpy(slot,&pointer,8);
        CHECK(!omfs_write_inode(&info,file));omfs_release_inode(file);
    }
    CHECK(!omfs_write_inode(&info,dir));omfs_release_inode(dir);
    free(buf);omfs_sync(&info);CHECK(!fclose(info.dev));
    free(info.bitmap->bmap);free(info.bitmap->dirty);free(info.bitmap);
    return 0;
}
