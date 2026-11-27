/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include <infiltratr/filesystem_module_v2.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <assert.h>
static int fd;static uint32_t sector;
static int rd(void *ctx,uint64_t off,void *p,uint32_t n)
{(void)ctx;return off%sector||n%sector||pread(fd,p,n,(off_t)off)!=(ssize_t)n?-1:0;}
static void *alloc(void *c,size_t n){(void)c;return malloc(n);}
static void drop(void *c,void *p){(void)c;free(p);}
int main(int argc,char **argv)
{
    assert(argc==4);fd=open(argv[2],O_RDONLY);assert(fd>=0);
    struct stat st;assert(!fstat(fd,&st));
    FILE *f=fopen(argv[3],"rb");assert(f);unsigned char expected[8229],actual[8229];
    assert(fread(expected,1,sizeof expected,f)==sizeof expected&&fgetc(f)==EOF);fclose(f);
    void *lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);if(!lib){puts(dlerror());return 1;}
    IfsFsModuleApiEntryV2 entry;*(void **)(&entry)=dlsym(lib,IFS_FS_COMMON_EXPORT_V2);assert(entry);
    const IfsFsModuleApiV2 *api=entry();assert(ifs_fs_api_v2_valid(api,"omfs"));
    const IfsFsCoreOpsV2 *core=(const void *)ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    const IfsFsVolumeOpsV2 *vol=(const void *)ifs_fs_group_v2(api,IFS_FS_GROUP_VOLUME);
    for(sector=512;sector<=4096;sector*=8){
        IfsFsHostRuntimeV2 h={.abi_version=2,.structure_bytes=sizeof(h),.capabilities=IFS_FS_HOST_READ_ONLY,
          .volume_bytes=(uint64_t)st.st_size,.logical_sector_bytes=sector,.physical_sector_bytes=sector,
          .maximum_transfer_bytes=4096,.read_at=rd,.allocate=alloc,.release=drop};
        uint32_t variant=0;assert(core->probe(&h,&variant)==0&&variant==1);
        void *v=NULL;IfsFsVolumeInfoV2 i={.structure_bytes=sizeof i};assert(core->mount(&h,&v,&i)==0&&v);
        assert(i.total_bytes==(uint64_t)st.st_size&&i.free_bytes==i.total_bytes-11*4096);
        char name[257];IfsFsEntryV2 e={.structure_bytes=sizeof e,.name=name,.name_capacity=sizeof name};
        assert(core->stat(v,"/",&e)==0&&e.type==IFS_FS_ENTRY_DIRECTORY);
        assert(core->directory_entry(v,"/",0,&e)==0&&!strcmp(name,"proof.bin"));
        assert(core->directory_entry(v,"/",1,&e)==IFS_FS_END);
        assert(core->stat(v,"/proof.bin",&e)==0&&e.size_bytes==sizeof expected);
        assert(core->read(v,"/proof.bin",0,actual,sizeof actual)==0&&!memcmp(actual,expected,sizeof actual));
        uint32_t n;assert(vol->label(v,name,sizeof name,&n)==0&&n==4&&!strcmp(name,"omfs"));
        assert(core->unmount(v)==0);
    }
    close(fd);dlclose(lib);puts("OMFS independently formatted and populated media, both sector sizes PASS");return 0;
}
