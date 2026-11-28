/* SPDX-License-Identifier: GPL-3.0-or-later
 * Byte-authored format fixture; not a claim of third-party media qualification.
 * Separate fixture encoder and host callbacks, including corrupt metadata,
 * noncontiguous extents, nested directories and stale opaque tokens.
 */
#include <infiltratr/filesystem_module_v2.h>
#include "../Libraries/tests/filesystem_allocation_pages.h"
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
enum {B=4096,N=64,S=512};
static unsigned char disk[B*N],original[B*N];
static unsigned sector=512,allocations,fail_alloc,fail_read;
static void w16(unsigned char *p,unsigned n){p[0]=n>>8;p[1]=n;}
static void w32(unsigned char *p,uint32_t n){w16(p,n>>16);w16(p+2,n);}
static void w64(unsigned char *p,uint64_t n){w32(p,n>>32);w32(p+4,n);}
static unsigned char *blk(unsigned n){return disk+n*B;}
static void mark(unsigned n){blk(2)[n/8]|=1u<<(n%8);}
static void checksum(unsigned b,int type)
{
    unsigned char *p=blk(b);w64(p,b);w32(p+8,S-24);
    uint16_t c=0;
    for(unsigned i=24;i<S;++i){c^=(uint16_t)p[i]<<8;for(unsigned j=0;j<8;++j)c=(uint16_t)((c<<1)^((c&0x8000)?0x1021:0));}
    w16(p+12,c);p[16]=1;p[17]=type;p[18]=0xd2;p[19]=0;
    for(unsigned i=0;i<19;++i)p[19]^=p[i];
    mark(b);
}
static void inode(unsigned b,unsigned parent,uint64_t sibling,int type,const char *name,uint64_t size)
{
    unsigned char *p=blk(b);w64(p+24,parent);w64(p+32,sibling);p[83]=type;
    memcpy(p+152,name,strlen(name));w64(p+408,size);
    if(type=='D')memset(p+440,255,S-440);
    checksum(b,'e');
}
static void fixture(void)
{
    memset(disk,0,sizeof disk);mark(0);mark(2);
    w64(disk+256,1);w64(disk+264,N);w32(disk+272,0xc2993d87);w32(disk+276,B);
    w32(disk+280,1);w32(disk+284,S);
    unsigned char *p=blk(1);w64(p+32,N);w64(p+40,3);w64(p+48,2);
    w32(p+56,B);w32(p+60,1);w64(p+64,1);memcpy(p+72,"OMFS proof",10);checksum(1,'s');
    inode(3,3,UINT64_MAX,'D',"",S);
    inode(4,3,UINT64_MAX,'D',"dir",S);w64(blk(3)+440,4);checksum(3,'e');
    inode(5,4,UINT64_MAX,'F',"proof.bin",B+37);w64(blk(4)+440,5);checksum(4,'e');
    p=blk(5)+464;w64(p,6);w32(p+8,2);w64(p+16,10);w64(p+24,1);
    w64(p+32,UINT64_MAX);w64(p+40,~UINT64_C(1));checksum(5,'e');
    p=blk(6)+64;w64(p,UINT64_MAX);w32(p+8,2);w64(p+16,20);w64(p+24,1);
    w64(p+32,UINT64_MAX);w64(p+40,~UINT64_C(1));checksum(6,'c');
    mark(10);mark(20);
    for(unsigned i=0;i<B;++i)blk(10)[i]=(unsigned char)(i*37+19);
    for(unsigned i=0;i<B;++i)blk(20)[i]=(unsigned char)(i*29+7);
}
static int rd(void *c,uint64_t off,void *out,uint32_t n)
{(void)c;if(fail_read||off>sizeof disk||n>sizeof disk-off||off%sector||n%sector)return -1;memcpy(out,disk+off,n);return 0;}
static void *alloc(void *c,size_t n){(void)c;if(fail_alloc)return NULL;void *p=malloc(n);if(p)++allocations;return p;}
static void drop(void *c,void *p){(void)c;if(p){assert(allocations);--allocations;free(p);}}
static int fixture_allocated(uint64_t block)
{
    return !!(blk(2)[block/8] & (1u<<(block%8)));
}
int main(int argc,char **argv)
{
    assert(argc==2);void *lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);if(!lib){puts(dlerror());return 1;}
    IfsFsModuleApiEntryV2 entry;*(void **)(&entry)=dlsym(lib,IFS_FS_COMMON_EXPORT_V2);
    assert(entry);const IfsFsModuleApiV2 *api=entry();assert(ifs_fs_api_v2_valid(api,"omfs"));
    const IfsFsCoreOpsV2 *core=(const void *)ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    const IfsFsVolumeOpsV2 *vol=(const void *)ifs_fs_group_v2(api,IFS_FS_GROUP_VOLUME);
    assert(core&&vol&&!core->write&&!vol->format);
    fixture();memcpy(original,disk,sizeof disk);
    IfsFsHostRuntimeV2 h={.abi_version=2,.structure_bytes=sizeof(h),.capabilities=IFS_FS_HOST_READ_ONLY,
      .volume_bytes=sizeof disk,.maximum_transfer_bytes=4096,.read_at=rd,.allocate=alloc,.release=drop};
    for(sector=512;sector<=4096;sector*=8){
        h.logical_sector_bytes=h.physical_sector_bytes=sector;
        uint32_t variant=0;assert(core->probe(&h,&variant)==0&&variant==1&&!allocations);
        IfsFsVolumeInfoV2 i={.structure_bytes=sizeof i};void *v=NULL,*other=NULL;
        assert(core->mount(&h,&v,&i)==0&&v&&i.total_bytes==sizeof disk&&i.free_bytes==(N-9)*B);
        const IfsFsAllocationOpsV2 *allocation=(const void *)ifs_fs_group_v2(api,IFS_FS_GROUP_ALLOCATION);
        check_allocation_pages(allocation,v,B,h.volume_bytes,i.free_bytes,fixture_allocated);
        assert(core->mount(&h,&other,&i)==0&&other!=v);
        char name[257];IfsFsEntryV2 e={.structure_bytes=sizeof e,.name=name,.name_capacity=sizeof name};
        assert(core->stat(v,"/",&e)==0&&e.type==IFS_FS_ENTRY_DIRECTORY);
        assert(core->directory_entry(v,"/",0,&e)==0&&!strcmp(name,"dir"));
        assert(core->directory_entry(v,"/",1,&e)==IFS_FS_END);
        assert(core->stat(v,"/dir/proof.bin",&e)==0&&e.size_bytes==B+37);
        assert(core->stat(v,"/dir/proof",&e)==IFS_FS_NOT_FOUND);
        assert(core->directory_entry(v,"/dir",0,&e)==0&&!strcmp(name,"proof.bin"));
        unsigned char data[B+37];assert(core->read(v,"/dir/proof.bin",0,data,sizeof data)==0);
        assert(!memcmp(data,blk(10),B)&&!memcmp(data+B,blk(20),37));
        assert(core->read(v,"/dir/proof.bin",B-13,data,50)==0&&!memcmp(data,blk(10)+B-13,13)&&!memcmp(data+13,blk(20),37));
        assert(core->read(v,"/dir/proof.bin",B+37,data,1)==IFS_FS_INVALID);
        uint32_t n=0;assert(vol->label(v,name,sizeof name,&n)==0&&n==10&&!strcmp(name,"OMFS proof"));
        fail_alloc=1;assert(core->read(v,"/dir/proof.bin",0,data,1)!=0);fail_alloc=0;
        fail_read=1;assert(core->stat(v,"/dir",&e)!=0);fail_read=0;
        blk(5)[152]^=1;assert(core->stat(v,"/dir/proof.bin",&e)==IFS_FS_CORRUPT);blk(5)[152]^=1;
        w64(blk(6)+64,6);checksum(6,'c');assert(core->read(v,"/dir/proof.bin",0,data,1)==IFS_FS_CORRUPT);
        memcpy(disk,original,sizeof disk);
        w64(blk(6)+64+16,10);checksum(6,'c');assert(core->read(v,"/dir/proof.bin",0,data,1)==IFS_FS_CORRUPT);
        memcpy(disk,original,sizeof disk);
        assert(core->unmount(v)==0&&core->unmount(v)==IFS_FS_INVALID&&core->stat(v,"/",&e)==IFS_FS_INVALID);
        assert(core->unmount(other)==0&&!allocations);
        assert(!memcmp(disk,original,sizeof disk));
        /* Bitmap must agree with known metadata ownership. */
        blk(2)[0]&=~(1u<<3);assert(core->mount(&h,&v,&i)==IFS_FS_CORRUPT&&!v&&!allocations);
        memcpy(disk,original,sizeof disk);
    }
    puts("OMFS independent byte fixture: 512/4096 sectors, nested reads, corruption and lifetime PASS");
    dlclose(lib);return 0;
}
