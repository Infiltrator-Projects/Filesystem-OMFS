/* SPDX-License-Identifier: GPL-3.0-or-later
 * Independent OMFS (Optimized MPEG File System) read-only engine.
 * On-disk reference: Filesystems/native/filesystems/omfs/linux/omfs_fs.h,
 * inode.c, dir.c, file.c and bitmap.c (Linux v6.12.107; original OMFS
 * implementation by Bob Copeland). This implementation uses only Common
 * V2 byte I/O and never calls Linux VFS or a system filesystem driver.
 */
#include <infiltratr/filesystem_module_v2.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>

enum { HEADER=24, DIR_START=440, EXT_START=464, CONT_START=64,
       MAX_SYS=4096, MAX_NAME=256, MAX_CHAIN=65536, MAX_RUNS=4096,
       MAX_TABLES=1024, MAX_BITMAP=16*1024*1024, SLOTS=8 };
#define NONE UINT64_MAX
typedef struct {
    IfsFsHostRuntimeV2 host;
    uint64_t blocks,system,root,bitmap,bitmap_blocks,free_blocks;
    uint32_t block,sys,mirrors;
    char label[MAX_NAME+1];
    uint8_t *bits;
    uintptr_t token;
} Volume;
typedef struct {uint64_t block,parent,sibling,size;uint32_t type,n;char name[MAX_NAME+1];} Node;
typedef struct {uint64_t block,count;} Run;
typedef struct {Run data[MAX_RUNS];uint64_t tables[MAX_TABLES];uint32_t runs,ntables;} Map;
static Volume slots[SLOTS];
static uintptr_t generation;
static atomic_flag gate=ATOMIC_FLAG_INIT;
static int enter(void){return !atomic_flag_test_and_set_explicit(&gate,memory_order_acquire);}
static int leave(int rc){atomic_flag_clear_explicit(&gate,memory_order_release);return rc;}
static void copy(void *out,const void *in,size_t n)
{uint8_t *d=out;const uint8_t *s=in;while(n--)*d++=*s++;}
static void zero(void *out,size_t n){uint8_t *p=out;while(n--)*p++=0;}
static uint32_t be16(const uint8_t *p){return (uint32_t)p[0]<<8|p[1];}
static uint32_t be32(const uint8_t *p){return be16(p)<<16|be16(p+2);}
static uint64_t be64(const uint8_t *p){return (uint64_t)be32(p)<<32|be32(p+4);}
static uint32_t power2(uint32_t n){return n&&!(n&(n-1));}
static uint32_t bounded_name(const uint8_t *p){uint32_t n=0;while(n<MAX_NAME&&p[n])++n;return n;}
static int same(const char *a,const char *b,uint32_t n)
{while(n--)if(*a++!=*b++)return 0;return 1;}
static int cancel(const Volume *v){return v->host.cancelled&&v->host.cancelled(v->host.context);}
static int read_bytes(const Volume *v,uint64_t off,void *p,uint32_t n)
{return ifs_fs_host_read_bytes_v2(&v->host,off,p,n);}
static int span(const Volume *v,uint64_t b,uint64_t n)
{return b<v->blocks&&n&&n<=v->blocks-b;}
static int overlap(uint64_t a,uint64_t an,uint64_t b,uint64_t bn)
{return a<b+bn&&b<a+an;}
static int allocated(const Volume *v,uint64_t b,uint64_t n)
{
    if(!span(v,b,n))return 0;
    if(!v->bits)return 1; /* probe has not loaded the bitmap */
    for(uint64_t i=0;i<n;++i)
        if(!(v->bits[(b+i)/8]&(1u<<((b+i)%8))))return 0;
    return 1;
}
static uint16_t crc16(const uint8_t *p,uint32_t n)
{
    uint16_t crc=0;
    while(n--){crc^=(uint16_t)*p++<<8;for(unsigned j=0;j<8;++j)
        crc=(uint16_t)((crc<<1)^((crc&0x8000u)?0x1021u:0u));}
    return crc;
}
/* Verify the exact record used for parsing. This initial profile reads
 * the primary copy only; a damaged primary is never silently repaired. */
static int record(const Volume *v,uint64_t b,uint8_t kind,uint8_t *raw)
{
    if(!b||!allocated(v,b,v->mirrors))return IFS_FS_CORRUPT;
    int rc=read_bytes(v,b*v->block,raw,v->sys);if(rc)return rc;
    uint32_t body=be32(raw+8);uint8_t x=0;
    for(unsigned i=0;i<19;++i)x^=raw[i];
    if(be64(raw)!=b||body>v->sys-HEADER||raw[16]!=1||raw[17]!=kind||
       raw[18]!=0xd2)return IFS_FS_CORRUPT;
    if(kind=='s'){
        if(body!=304&&body!=312&&body!=v->sys-HEADER)return IFS_FS_CORRUPT;
        /* Original omfsprogs 0.1.0 writes a 336-byte mirrored root
         * structure without computing its CRC/XOR. Admit exactly that
         * historical representation only with agreeing mirror bytes and
         * separately validated super/root geometry. Not a general
         * exemption from metadata integrity checking. */
        if(body==312&&!be16(raw+12)&&!raw[19]&&v->mirrors>=2){
            uint8_t mirror[MAX_SYS];
            for(uint32_t i=1;i<v->mirrors;++i){
                rc=read_bytes(v,(b+i)*v->block,mirror,HEADER+body);
                if(rc)return rc;
                if(!same((const char *)raw,(const char *)mirror,HEADER+body))
                    return IFS_FS_CORRUPT;
            }
            return IFS_FS_OK;
        }
    }else if(body!=v->sys-HEADER)return IFS_FS_CORRUPT;
    if(x!=raw[19]||crc16(raw+HEADER,body)!=be16(raw+12))
        return IFS_FS_CORRUPT;
    return IFS_FS_OK;
}
static int reserved(const Volume *v,uint64_t b,uint64_t n)
{return !b||overlap(b,n,v->system,v->mirrors)||
       overlap(b,n,v->bitmap,v->bitmap_blocks);}
static int inode(const Volume *v,uint64_t b,Node *out,uint8_t *raw)
{
    if(!span(v,b,v->mirrors)||reserved(v,b,v->mirrors))return IFS_FS_CORRUPT;
    int rc=record(v,b,'e',raw);if(rc)return rc;
    uint32_t type=raw[83]=='D'?IFS_FS_ENTRY_DIRECTORY:
                  raw[83]=='F'?IFS_FS_ENTRY_FILE:0;
    if(!type)return IFS_FS_CORRUPT;
    Node n={.block=b,.parent=be64(raw+24),.sibling=be64(raw+32),
            .size=type==IFS_FS_ENTRY_DIRECTORY?v->sys:be64(raw+408),.type=type,
            .n=bounded_name(raw+152)};
    if(n.size>(uint64_t)v->blocks*v->block)return IFS_FS_CORRUPT;
    copy(n.name,raw+152,n.n);n.name[n.n]=0;
    if(b!=v->root){
        if(!n.n||!n.parent||n.parent>=v->blocks||
           (n.n==1&&n.name[0]=='.')||(n.n==2&&same(n.name,"..",2)))return IFS_FS_CORRUPT;
        for(uint32_t i=0;i<n.n;++i)if(n.name[i]=='/'||n.name[i]=='\\')return IFS_FS_CORRUPT;
    }
    if(n.sibling!=NONE&&(!n.sibling||n.sibling>=v->blocks||n.sibling==b))
        return IFS_FS_CORRUPT;
    *out=n;return IFS_FS_OK;
}
static int open_volume(const IfsFsHostRuntimeV2 *h,Volume *v)
{
    if(!ifs_fs_host_v2_valid(h)||!v)return IFS_FS_INVALID;
    zero(v,sizeof(*v));size_t bytes=h->structure_bytes;
    if(bytes>sizeof(v->host))bytes=sizeof(v->host);
    copy(&v->host,h,bytes);v->host.structure_bytes=(uint32_t)bytes;
    if(h->volume_bytes<512)return IFS_FS_NOT_FOUND;
    uint8_t raw[MAX_SYS];int rc=read_bytes(v,0,raw,512);if(rc)return rc;
    if(be32(raw+272)!=UINT32_C(0xc2993d87))return IFS_FS_NOT_FOUND;
    v->system=be64(raw+256);v->blocks=be64(raw+264);
    v->block=be32(raw+276);v->mirrors=be32(raw+280);v->sys=be32(raw+284);
    if(!power2(v->block)||!power2(v->sys)||v->sys<512||v->sys>MAX_SYS||
       v->block<v->sys||v->block>8192||!v->blocks||v->blocks>(UINT64_C(1)<<31)||
       v->blocks>h->volume_bytes/v->block||!v->mirrors||v->mirrors>8||
       !v->system||!span(v,v->system,v->mirrors))return IFS_FS_CORRUPT;
    rc=record(v,v->system,'s',raw);if(rc)return rc;
    v->root=be64(raw+40);v->bitmap=be64(raw+48);
    uint64_t mirrors=be64(raw+64);
    /* mkomfs historically converts an already big-endian 32-bit count
     * as a 64-bit field. Accept only the exact known encoding on its
     * mirrored checksumless system record. */
    if(mirrors!=v->mirrors&&!(be32(raw+8)==312&&!be16(raw+12)&&!raw[19]&&
       v->mirrors>=2&&mirrors==((uint64_t)v->mirrors<<24)))return IFS_FS_CORRUPT;
    if(be64(raw+32)!=v->blocks||be32(raw+56)!=v->block||
       !be32(raw+60)||be32(raw+60)>8)return IFS_FS_CORRUPT;
    /* ReplayTV's bitmapless format needs a separate ownership walk. */
    if(v->bitmap==NONE)return IFS_FS_UNSUPPORTED;
    uint64_t bitmap_bytes=(v->blocks+7)/8;
    if(bitmap_bytes>MAX_BITMAP)return IFS_FS_UNSUPPORTED;
    v->bitmap_blocks=(bitmap_bytes+v->block-1)/v->block;
    if(!v->bitmap||!span(v,v->bitmap,v->bitmap_blocks)||
       overlap(v->bitmap,v->bitmap_blocks,v->system,v->mirrors))return IFS_FS_CORRUPT;
    copy(v->label,raw+72,bounded_name(raw+72));
    Node root;rc=inode(v,v->root,&root,raw);
    if(rc)return rc;
    return root.type==IFS_FS_ENTRY_DIRECTORY?IFS_FS_OK:IFS_FS_CORRUPT;
}
static int load_bitmap(Volume *v)
{
    uint32_t n=(uint32_t)((v->blocks+7)/8);
    v->bits=v->host.allocate(v->host.context,n);
    if(!v->bits)return IFS_FS_NO_MEMORY;
    for(uint32_t at=0;at<n;){
        if(cancel(v))return IFS_FS_BUSY;
        uint32_t count=n-at;if(count>65536)count=65536;
        int rc=read_bytes(v,v->bitmap*v->block+at,v->bits+at,count);if(rc)return rc;
        at+=count;
    }
    if(!allocated(v,0,1)||!allocated(v,v->system,v->mirrors)||
       !allocated(v,v->root,v->mirrors)||!allocated(v,v->bitmap,v->bitmap_blocks))
        return IFS_FS_CORRUPT;
    uint64_t free=0;
    for(uint64_t i=0;i<v->blocks;++i)free+=!(v->bits[i/8]&(1u<<(i%8)));
    v->free_blocks=free;return IFS_FS_OK;
}
static void dispose(Volume *v)
{if(v->bits)v->host.release(v->host.context,v->bits);zero(v,sizeof(*v));}
static Volume *mounted(void *p)
{if(p)for(unsigned i=0;i<SLOTS;++i)if(slots[i].token==(uintptr_t)p)return &slots[i];return NULL;}
static void info(const Volume *v,IfsFsVolumeInfoV2 *o)
{*o=(IfsFsVolumeInfoV2){.structure_bytes=sizeof(*o),.variant=1,.max_name_bytes=MAX_NAME,
  .block_bytes=v->block,.total_bytes=v->blocks*v->block,.free_bytes=v->free_blocks*v->block};}
static int probe_impl(const IfsFsHostRuntimeV2 *h,uint32_t *variant)
{if(!variant)return IFS_FS_INVALID;Volume v;int rc=open_volume(h,&v);if(!rc)*variant=1;return rc;}
static int mount_impl(const IfsFsHostRuntimeV2 *h,void **out,IfsFsVolumeInfoV2 *o)
{
    if(!out||!o||o->structure_bytes<sizeof(*o))return IFS_FS_INVALID;
    *out=NULL;
    if(!ifs_fs_host_v2_valid(h))return IFS_FS_INVALID;
    if(!(h->capabilities&IFS_FS_HOST_READ_ONLY))return IFS_FS_ACCESS;
    unsigned i=0;while(i<SLOTS&&slots[i].token)++i;
    if(i==SLOTS||generation==UINTPTR_MAX)return IFS_FS_BUSY;
    Volume *v=&slots[i];int rc=open_volume(h,v);
    if(!rc)rc=load_bitmap(v);
    if(rc){dispose(v);return rc;}
    v->token=++generation;*out=(void *)v->token;info(v,o);return IFS_FS_OK;
}
static int unmount_impl(void *p)
{Volume *v=mounted(p);if(!v)return IFS_FS_INVALID;dispose(v);return IFS_FS_OK;}
/* Directory bucket traversal. The parent pointer binds every member to
 * this directory; bounded Brent cycle detection prevents infinite chains.
 * A name lookup scans all buckets, avoiding the historical prefix match
 * and signed-char hash ambiguities. Reject duplicate matching names. */
static int child(const Volume *v,const Node *dir,const char *name,uint32_t n,
                 uint32_t index,Node *out)
{
    if(dir->type!=IFS_FS_ENTRY_DIRECTORY)return IFS_FS_NOT_FOUND;
    uint8_t table[MAX_SYS],raw[MAX_SYS];Node checked,found;
    int rc=inode(v,dir->block,&checked,table);if(rc)return rc;
    uint32_t visited=0;int have=0;
    for(uint32_t off=DIR_START;off+8<=v->sys;off+=8){
        uint64_t b=be64(table+off),anchor=b,power=1,steps=0;
        while(b!=NONE){
            if(cancel(v))return IFS_FS_BUSY;
            if(++visited>MAX_CHAIN)return IFS_FS_UNSUPPORTED;
            if(b==dir->block||b==v->root)return IFS_FS_CORRUPT;
            Node item;rc=inode(v,b,&item,raw);if(rc)return rc;
            if(item.parent!=dir->block)return IFS_FS_CORRUPT;
            if(name?(item.n==n&&same(item.name,name,n)):index==0){
                if(have&&name)return IFS_FS_CORRUPT;
                if(!have){found=item;have=1;}
            }
            if(!name&&index) --index;
            b=item.sibling;++steps;
            if(b!=NONE&&b==anchor)return IFS_FS_CORRUPT;
            if(steps==power){anchor=b;power*=2;steps=0;}
        }
    }
    if(!have)return name?IFS_FS_NOT_FOUND:IFS_FS_END;
    *out=found;return IFS_FS_OK;
}
static int resolve(const Volume *v,const char *p,Node *out)
{
    if(!p||*p!='/')return IFS_FS_INVALID;
    uint8_t raw[MAX_SYS];Node node;int rc=inode(v,v->root,&node,raw);if(rc)return rc;
    ++p;
    if(!*p){node.n=1;node.name[0]='/';node.name[1]=0;}
    for(unsigned depth=0;*p;++depth){
        if(depth>=256)return IFS_FS_INVALID;
        uint32_t n=0;while(n<=MAX_NAME&&p[n]&&p[n]!='/')++n;
        if(!n||n>MAX_NAME||(n==1&&p[0]=='.')||(n==2&&same(p,"..",2)))return IFS_FS_INVALID;
        Node next;rc=child(v,&node,p,n,0,&next);if(rc)return rc;
        node=next;p+=n;if(*p=='/'){++p;if(!*p)return IFS_FS_INVALID;}
    }
    *out=node;return IFS_FS_OK;
}
static int emit(const Node *n,IfsFsEntryV2 *o)
{
    if(!o||o->structure_bytes<sizeof(*o)||!o->name)return IFS_FS_INVALID;
    if(o->name_capacity<=n->n)return IFS_FS_NO_SPACE;
    char *name=o->name;uint32_t cap=o->name_capacity;
    *o=(IfsFsEntryV2){.structure_bytes=sizeof(*o),.type=n->type,.size_bytes=n->size,
                      .name_bytes=n->n,.name=name,.name_capacity=cap};
    copy(name,n->name,n->n+1);return IFS_FS_OK;
}
static int stat_impl(void *p,const char *path,IfsFsEntryV2 *o)
{Volume *v=mounted(p);if(!v)return IFS_FS_INVALID;Node n;int rc=resolve(v,path,&n);return rc?rc:emit(&n,o);}
static int list_impl(void *p,const char *path,uint32_t i,IfsFsEntryV2 *o)
{
    Volume *v=mounted(p);if(!v)return IFS_FS_INVALID;
    Node dir,n;int rc=resolve(v,path,&dir);if(!rc)rc=child(v,&dir,NULL,0,i,&n);
    return rc?rc:emit(&n,o);
}
/* Decode the complete selected file's extent chain before exposing data.
 * Local ownership proof rejects repeated metadata, overlapping runs and
 * extents referencing known system/bitmap blocks. No write authority. */
static int build_map(const Volume *v,const Node *node,Map *m)
{
    m->runs=m->ntables=0;
    uint8_t raw[MAX_SYS];uint64_t b=node->block,total=0;
    uint32_t off=EXT_START;
    while(b!=NONE){
        if(cancel(v))return IFS_FS_BUSY;
        if(m->ntables==MAX_TABLES)return IFS_FS_UNSUPPORTED;
        if(reserved(v,b,v->mirrors)||!allocated(v,b,v->mirrors))return IFS_FS_CORRUPT;
        for(uint32_t j=0;j<m->ntables;++j)
            if(overlap(b,v->mirrors,m->tables[j],v->mirrors))return IFS_FS_CORRUPT;
        for(uint32_t j=0;j<m->runs;++j)
            if(overlap(b,v->mirrors,m->data[j].block,m->data[j].count))return IFS_FS_CORRUPT;
        int rc=record(v,b,off==EXT_START?'e':'c',raw);if(rc)return rc;
        m->tables[m->ntables++]=b;
        uint64_t next=be64(raw+off);
        uint32_t count=be32(raw+off+8);
        if(!count||count>(v->sys-off-16)/16)return IFS_FS_CORRUPT;
        uint64_t local=0;
        for(uint32_t i=0;i+1<count;++i){
            const uint8_t *p=raw+off+16+i*16;
            uint64_t at=be64(p),n=be64(p+8);
            if(!span(v,at,n)||reserved(v,at,n)||overlap(at,n,v->root,v->mirrors)||!allocated(v,at,n)||
               n>v->blocks-total)return IFS_FS_CORRUPT;
            if(m->runs==MAX_RUNS)return IFS_FS_UNSUPPORTED;
            for(uint32_t j=0;j<m->runs;++j)
                if(overlap(at,n,m->data[j].block,m->data[j].count))return IFS_FS_CORRUPT;
            for(uint32_t j=0;j<m->ntables;++j)
                if(overlap(at,n,m->tables[j],v->mirrors))return IFS_FS_CORRUPT;
            m->data[m->runs++]=(Run){at,n};total+=n;local+=n;
        }
        const uint8_t *end=raw+off+16+(count-1)*16;
        if(be64(end)!=NONE||~be64(end+8)!=local)return IFS_FS_CORRUPT;
        b=next;off=CONT_START;
    }
    return total*v->block>=node->size?IFS_FS_OK:IFS_FS_CORRUPT;
}
static int read_impl(void *p,const char *path,uint64_t at,void *out,uint32_t n)
{
    Volume *v=mounted(p);if(!v||(!out&&n))return IFS_FS_INVALID;
    Node node;int rc=resolve(v,path,&node);if(rc)return rc;
    if(node.type!=IFS_FS_ENTRY_FILE)return IFS_FS_UNSUPPORTED;
    if(at>node.size||n>node.size-at)return IFS_FS_INVALID;
    if(!n)return IFS_FS_OK;
    Map *m=v->host.allocate(v->host.context,sizeof(*m));if(!m)return IFS_FS_NO_MEMORY;
    rc=build_map(v,&node,m);
    if(!rc){
        uint8_t *dest=out;
        for(uint32_t i=0;i<m->runs&&n;++i){
            uint64_t bytes=m->data[i].count*v->block;
            if(at>=bytes){at-=bytes;continue;}
            uint32_t chunk=n;if((uint64_t)chunk>bytes-at)chunk=(uint32_t)(bytes-at);
            rc=read_bytes(v,m->data[i].block*v->block+at,dest,chunk);
            if(rc)break;
            dest+=chunk;n-=chunk;at=0;
        }
        if(!rc&&n)rc=IFS_FS_CORRUPT;
    }
    v->host.release(v->host.context,m);return rc;
}
static int query_impl(void *p,IfsFsVolumeInfoV2 *o)
{Volume *v=mounted(p);if(!v||!o||o->structure_bytes<sizeof(*o))return IFS_FS_INVALID;info(v,o);return IFS_FS_OK;}
static int label_impl(void *p,char *out,uint32_t cap,uint32_t *n)
{
    Volume *v=mounted(p);if(!n)return IFS_FS_INVALID;*n=0;
    if(!v||!out)return IFS_FS_INVALID;
    uint32_t len=bounded_name((const uint8_t *)v->label);
    if(cap<=len)return IFS_FS_NO_SPACE;
    copy(out,v->label,len+1);*n=len;return IFS_FS_OK;
}
#define WRAP(name,args,call) static int name args {if(!enter())return IFS_FS_BUSY;return leave(call);}
WRAP(probe,(const IfsFsHostRuntimeV2 *h,uint32_t *n),probe_impl(h,n))
WRAP(mount,(const IfsFsHostRuntimeV2 *h,void **v,IfsFsVolumeInfoV2 *o),mount_impl(h,v,o))
WRAP(unmount,(void *p),unmount_impl(p))
WRAP(stat_path,(void *p,const char *s,IfsFsEntryV2 *o),stat_impl(p,s,o))
WRAP(list_path,(void *p,const char *s,uint32_t i,IfsFsEntryV2 *o),list_impl(p,s,i,o))
WRAP(read_path,(void *p,const char *s,uint64_t a,void *b,uint32_t n),read_impl(p,s,a,b,n))
WRAP(query,(void *p,IfsFsVolumeInfoV2 *o),query_impl(p,o))
WRAP(label,(void *p,char *s,uint32_t c,uint32_t *n),label_impl(p,s,c,n))
static const IfsFsCoreOpsV2 core={
 .header={2,sizeof(core),IFS_FS_GROUP_CORE,0,IFS_FS_CORE_PROBE|IFS_FS_CORE_MOUNT|IFS_FS_CORE_STAT|IFS_FS_CORE_LIST|IFS_FS_CORE_READ},
 .probe=probe,.mount=mount,.unmount=unmount,.stat=stat_path,.directory_entry=list_path,.read=read_path};
static const IfsFsVolumeOpsV2 volume={
 .header={2,sizeof(volume),IFS_FS_GROUP_VOLUME,0,IFS_FS_VOLUME_QUERY|IFS_FS_VOLUME_LABEL_GET},
 .query=query,.label=label};
static const IfsFsGroupHeaderV2 *group(uint32_t id,uint32_t lo,uint32_t hi)
{if(lo>2||hi<2)return NULL;if(id==IFS_FS_GROUP_CORE)return &core.header;if(id==IFS_FS_GROUP_VOLUME)return &volume.header;return NULL;}
static const IfsFsModuleApiV2 api={.abi_version=2,.structure_bytes=sizeof(api),.format_id="omfs",
#ifdef IFS_BOOT_FIRMWARE
 .runtime_profiles=IFS_FS_PROFILE_FREESTANDING|IFS_FS_PROFILE_FIRMWARE,
#else
 .runtime_profiles=IFS_FS_PROFILE_HOSTED,
#endif
 .group_mask=IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE)|IFS_FS_GROUP_BIT(IFS_FS_GROUP_VOLUME),.query_group=group};
__attribute__((visibility("default"))) const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void){return &api;}
